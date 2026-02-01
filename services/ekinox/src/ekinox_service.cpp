#include "ironsoft/ekinox/ekinox_service.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

// Manual test (see docs/TEST_EKINOX_SERVICE.md):
// 1) ./build-ekinox/ekinox_service --config config/ekinox.json
// 2) mosquitto_sub -h 127.0.0.1 -v -t "ironsoft/uav/drone001/cmd" -t "ironsoft/uav/drone001/ack" \
//    -t "ironsoft/uav/drone001/ekinox/status"
// 3) Publish:
//    mosquitto_pub -h 127.0.0.1 -t "ironsoft/uav/drone001/cmd" -m '{"type":"logger.start","id":"t1","ts":0}'
//    mosquitto_pub -h 127.0.0.1 -t "ironsoft/uav/drone001/cmd" -m '{"type":"logger.status","id":"t2","ts":0}'
//    mosquitto_pub -h 127.0.0.1 -t "ironsoft/uav/drone001/cmd" -m '{"type":"logger.stop","id":"t3","ts":0}'

namespace ironsoft::ekinox {

namespace {
constexpr std::chrono::milliseconds kLoopSleep{50};
constexpr std::chrono::seconds kStatusInterval{1};
constexpr std::chrono::seconds kHeartbeatInterval{1};

std::string make_client_id() {
	const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
	return "ekinox_service_" + std::to_string(static_cast<long long>(now));
}

}  // namespace

EkinoxService::EkinoxService(EkinoxConfig config)
    : config_(std::move(config)),
      topics_(make_topics(config_.mqtt.drone_id)),
      server_uri_("tcp://" + config_.mqtt.host + ":" + std::to_string(config_.mqtt.port)),
      client_id_(make_client_id()),
      client_(server_uri_, client_id_) {
	std::cout << "[ekinox] TOPIC cmd=" << topics_.cmd << '\n';
	if (!topics_.cmd_legacy.empty()) {
		std::cout << "[ekinox] TOPIC cmd_legacy=" << topics_.cmd_legacy << '\n';
	}
	std::cout << "[ekinox] TOPIC ack=" << topics_.ack << '\n';
	if (!topics_.ack_legacy.empty()) {
		std::cout << "[ekinox] TOPIC ack_legacy=" << topics_.ack_legacy << '\n';
	}
	std::cout << "[ekinox] TOPIC ekinox/status=" << topics_.status << '\n';
	std::cout << "[ekinox] TOPIC ekinox/presence=" << topics_.presence << '\n';
	std::cout << "[ekinox] TOPIC ekinox/heartbeat=" << topics_.heartbeat << '\n';
	client_.set_callback(*this);

	conn_opts_.set_clean_session(true);
	conn_opts_.set_automatic_reconnect(false);
	conn_opts_.set_keep_alive_interval(20);
	if (!config_.mqtt.username.empty()) {
		conn_opts_.set_user_name(config_.mqtt.username);
	}
	if (!config_.mqtt.password.empty()) {
		conn_opts_.set_password(config_.mqtt.password);
	}
	if (config_.mqtt.tls_enabled) {
		mqtt::ssl_options ssl_opts;
		if (!config_.mqtt.ca_file.empty()) {
			ssl_opts.set_trust_store(config_.mqtt.ca_file);
		}
		conn_opts_.set_ssl(ssl_opts);
	}

	status_.state = ServiceState::kDisconnected;
	status_.api_ok = false;
	status_.link_alive = false;
	presence_.online = false;
	presence_.reason.clear();
	presence_.timestamp = unix_ts();

	start_tp_ = steady_clock::now();
	next_status_pub_ = start_tp_;
	next_heartbeat_pub_ = start_tp_;
	next_reconnect_attempt_ = start_tp_;
	next_sensor_attempt_ = start_tp_;
	current_backoff_ms_ = config_.timeouts.reconnect_backoff_ms;
	udp_config_.ip = config_.ekinox.ip;
	udp_config_.remote_port = static_cast<std::uint16_t>(std::max(config_.ekinox.udp_port, 0));
	udp_config_.local_port = static_cast<std::uint16_t>(std::max(config_.ekinox.udp_local_port, 0));
	udp_config_.rx_dead_ms = static_cast<std::uint32_t>(std::max(config_.timeouts.rx_dead_ms, 0));
	udp_session_ = std::make_unique<EkinoxUdpSession>();
}

EkinoxService::~EkinoxService() {
	if (udp_session_) {
		udp_session_->close();
	}
	try {
		if (connected_) {
			client_.disconnect()->wait();
		}
	} catch (const mqtt::exception& ex) {
		std::cerr << "[ekinox] disconnect error: " << ex.what() << '\n';
	}
}

bool EkinoxService::run(std::atomic_bool& stop_flag) {
	std::cout << "[ekinox] broker " << server_uri_ << '\n';

	while (!stop_flag.load()) {
		auto now = steady_clock::now();
		ensure_connection(now);

		if (connected_) {
			drain_commands();
			ensure_sensor_session(now);
			poll_sensor(now);

			if (now >= next_status_pub_) {
				publish_status();
				next_status_pub_ = now + kStatusInterval;
			}
			if (now >= next_heartbeat_pub_) {
				auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_tp_).count();
				publish_heartbeat(uptime);
				next_heartbeat_pub_ = now + kHeartbeatInterval;
			}
		}

		std::this_thread::sleep_for(kLoopSleep);
	}

	if (sensor_connected_) {
		handle_sensor_disconnect("shutdown");
	}
	if (connected_) {
		publish_presence(false, "shutdown");
		publish_status();
		disconnect();
	}
	return true;
}

void EkinoxService::connection_lost(const std::string& cause) {
	std::cerr << "[ekinox] MQTT connection lost: " << cause << '\n';
	connected_ = false;
	presence_online_ = false;
	status_.link_alive = false;
	if (udp_session_) {
		udp_session_->close();
	}
	sensor_connected_ = false;
	set_state(ServiceState::kDisconnected);
	next_reconnect_attempt_ = steady_clock::now();
}

void EkinoxService::message_arrived(mqtt::const_message_ptr msg) {
	const auto topic = msg->get_topic();
	const auto payload = msg->get_payload_str();
	std::cout << "[ekinox] RX mqtt topic=" << topic << " payload=" << payload << '\n';
	std::lock_guard<std::mutex> lock(commands_mutex_);
	pending_commands_.push(CommandMessage{topic, payload});
}

bool EkinoxService::connect_once() {
	try {
		set_state(ServiceState::kConnecting);
		std::cout << "[ekinox] Connecting to " << server_uri_ << '\n';
		client_.connect(conn_opts_)->wait();
		auto subscribe_and_log = [this](const std::string& topic) {
			if (topic.empty()) {
				return;
			}
			std::cout << "[ekinox] SUBSCRIBING topic=" << topic << '\n';
			auto token = client_.subscribe(topic, 1);
			const int rc = wait_for_token_rc(token);
			std::cout << "[ekinox] SUBSCRIBED topic=" << topic << " qos=1 rc=" << rc << '\n';
		};
		subscribe_and_log(topics_.cmd);
		subscribe_and_log(topics_.cmd_legacy);
		connected_ = true;
		presence_online_ = false;
		status_.link_alive = false;
		status_.api_ok = false;
		status_.recording_active = false;
		set_state(ServiceState::kConnecting);
		publish_presence(false, "connecting");
		publish_status();
		std::cout << "[ekinox] Connected" << '\n';
		return true;
	} catch (const mqtt::exception& ex) {
		set_state(ServiceState::kDisconnected);
		std::cerr << "[ekinox] connect failed: " << ex.what() << '\n';
		return false;
	}
}

void EkinoxService::disconnect() {
	if (!connected_) {
		return;
	}
	try {
		client_.disconnect()->wait();
		connected_ = false;
		status_.link_alive = false;
		set_state(ServiceState::kDisconnected);
	} catch (const mqtt::exception& ex) {
		std::cerr << "[ekinox] disconnect failed: " << ex.what() << '\n';
	}
}

void EkinoxService::ensure_connection(time_point now) {
	if (connected_) {
		return;
	}
	if (!next_reconnect_attempt_.has_value()) {
		next_reconnect_attempt_ = now;
	}
	if (now < *next_reconnect_attempt_) {
		return;
	}
	if (connect_once()) {
		current_backoff_ms_ = config_.timeouts.reconnect_backoff_ms;
		next_reconnect_attempt_ = now + std::chrono::milliseconds(config_.timeouts.reconnect_backoff_ms);
		return;
	}
	current_backoff_ms_ = std::min(current_backoff_ms_ * 2, config_.timeouts.reconnect_backoff_max_ms);
	next_reconnect_attempt_ = now + std::chrono::milliseconds(current_backoff_ms_);
}

void EkinoxService::publish_presence(bool online, const std::string& reason) {
	if (!connected_) {
		presence_.online = online;
		presence_.reason = reason;
		presence_.timestamp = unix_ts();
		last_presence_ts_ = presence_.timestamp;
		presence_online_ = online;
		return;
	}
	if (presence_online_ == online && presence_.reason == reason) {
		return;
	}
	presence_.online = online;
	presence_.reason = reason;
	presence_.timestamp = unix_ts();
	last_presence_ts_ = presence_.timestamp;
	Json::Value payload = build_presence_json(presence_);
	auto msg = mqtt::make_message(topics_.presence, serialize_json(payload));
	msg->set_qos(1);
	msg->set_retained(true);
	client_.publish(msg);
	presence_online_ = online;
}

void EkinoxService::publish_status() {
	if (!connected_) {
		return;
	}
	Json::Value payload = build_status_json(status_);
	auto msg = mqtt::make_message(topics_.status, serialize_json(payload));
	msg->set_qos(1);
	msg->set_retained(true);
	client_.publish(msg);
}

void EkinoxService::publish_heartbeat(std::int64_t uptime_s) {
	if (!connected_) {
		return;
	}
	Json::Value payload = build_heartbeat_json(++heartbeat_seq_, uptime_s);
	auto msg = mqtt::make_message(topics_.heartbeat, serialize_json(payload));
	msg->set_qos(0);
	msg->set_retained(false);
	client_.publish(msg);
}

void EkinoxService::publish_ack(const std::string& type,
	const std::string& id,
	bool ok,
	int http_code,
	const std::string& message,
	const std::string& err) {
	Json::Value ack;
	ack["type"] = type;
	ack["id"] = id;
	ack["ok"] = ok;
	ack["message"] = message;
	ack["err"] = err;
	ack["http_code"] = http_code;
	ack["ts"] = static_cast<Json::Int64>(unix_ts());
	const auto payload = serialize_json(ack);
	auto publish_topic = [this, &payload](const std::string& topic) {
		if (topic.empty()) {
			return;
		}
		int rc = -1;
		if (!connected_) {
			std::cerr << "[ekinox] TX ack error topic=" << topic << " reason=offline" << '\n';
		} else {
			auto msg = mqtt::make_message(topic, payload);
			msg->set_qos(1);
			msg->set_retained(false);
			try {
				auto token = client_.publish(msg);
				rc = wait_for_token_rc(token);
			} catch (const mqtt::exception& ex) {
				std::cerr << "[ekinox] TX ack error topic=" << topic << " what=" << ex.what() << '\n';
			}
		}
		std::cout << "[ekinox] TX ack topic=" << topic
			<< " payload=" << payload
			<< " qos=1 retained=0 rc=" << rc << '\n';
	};
	publish_topic(topics_.ack);
	if (!topics_.ack_legacy.empty() && topics_.ack_legacy != topics_.ack) {
		publish_topic(topics_.ack_legacy);
	}
	std::cout << "[ekinox] TX ack type=" << (type.empty() ? "-" : type)
		<< " id=" << (id.empty() ? "-" : id)
		<< " ok=" << (ok ? "true" : "false")
		<< " http=" << http_code
		<< " err=" << (err.empty() ? "-" : err)
		<< " message=" << (message.empty() ? "-" : message)
		<< '\n';
}

void EkinoxService::drain_commands() {
	std::queue<CommandMessage> local;
	{
		std::lock_guard<std::mutex> lock(commands_mutex_);
		std::swap(local, pending_commands_);
	}
	while (!local.empty()) {
		handle_command_message(local.front().topic, local.front().payload);
		local.pop();
	}
}

void EkinoxService::handle_command_message(const std::string& topic, const std::string& payload) {
	Json::CharReaderBuilder rb;
	rb["collectComments"] = false;
	Json::Value root;
	std::string errs;
	std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
	const auto log_rx_type = [&topic](const std::string& type, const std::string& id) {
		std::cout << "[ekinox] RX cmd topic=" << topic
			<< " type=" << (type.empty() ? "-" : type)
			<< " id=" << (id.empty() ? "-" : id) << '\n';
	};
	if (!reader->parse(payload.data(), payload.data() + payload.size(), &root, &errs)) {
		std::cerr << "[ekinox] bad cmd json: " << errs << '\n';
		log_rx_type("", "");
		publish_ack("", "", false, 400, "", "bad_request");
		return;
	}
	if (!root.isObject()) {
		log_rx_type("", "");
		publish_ack("", "", false, 400, "", "bad_request");
		return;
	}
	const Json::Value& type_value = root["type"];
	const Json::Value& id_value = root["id"];
	std::string type = type_value.isString() ? type_value.asString() : "";
	std::string id = id_value.isString() ? id_value.asString() : "";
	log_rx_type(type, id);
	if (!type_value.isString() || type.empty()) {
		publish_ack("", id, false, 400, "", "bad_request");
		return;
	}
	if (!id_value.isString() || id.empty()) {
		publish_ack(type, "", false, 400, "", "bad_request");
		return;
	}
	if (root.isMember("ts")) {
		const Json::Value& ts_value = root["ts"];
		if (!ts_value.isInt64() && !ts_value.isUInt64()) {
			publish_ack(type, id, false, 400, "", "bad_request");
			return;
		}
	}
	CommandType cmd_type = CommandType::kUnknown;
	try_parse_command_type(type, cmd_type);
	const bool supported = (cmd_type == CommandType::kLoggerStart) ||
		(cmd_type == CommandType::kLoggerStop) ||
		(cmd_type == CommandType::kLoggerStatus);
	if (!supported) {
		publish_ack(type, id, false, 400, "", "unknown_type");
		return;
	}
	std::cout << "[ekinox] dispatch type=" << type << " id=" << id << '\n';
	switch (cmd_type) {
	case CommandType::kLoggerStart:
		handle_logger_start(id, type);
		break;
	case CommandType::kLoggerStop:
		handle_logger_stop(id, type);
		break;
	case CommandType::kLoggerStatus:
		handle_logger_status(id, type);
		break;
	default:
		publish_ack(type, id, false, 400, "", "unknown_type");
		break;
	}
}

void EkinoxService::handle_logger_start(const std::string& id, const std::string& type) {
	std::string reason;
	if (!can_execute_logger_cmd(reason)) {
		status_.api_ok = false;
		set_error(reason);
		publish_status();
		send_ack(id, type, false, "", reason, 503);
		return;
	}
	if (status_.state != ServiceState::kIdle) {
		set_error("logger.start invalid state");
		publish_status();
		send_ack(id, type, false, "", "INVALID_STATE", 409);
		return;
	}
	set_state(ServiceState::kStarting);
	publish_status();
	const auto http_code = [](const LoggerResult& res, int fallback) {
		return static_cast<int>(res.http_code != 0 ? res.http_code : fallback);
	};
	LoggerResult start_result = EkinoxLoggerApi::start_http(config_, config_.rest_api);
	if (!start_result.ok) {
		status_.api_ok = false;
		status_.link_alive = false;
		const std::string err = !start_result.error_string.empty() ? start_result.error_string : "logger.start failed";
		set_error(err);
		set_state(ServiceState::kError);
		publish_status();
		send_ack(id, type, false, "", err, http_code(start_result, 500));
		return;
	}
	std::uint32_t raw_status = 0;
	bool recording = false;
	LoggerResult status_result = EkinoxLoggerApi::status_http(config_, config_.rest_api, raw_status, recording);
	const bool recording_flag_missing = status_result.ok && !recording;
	if (!status_result.ok || recording_flag_missing) {
		status_.api_ok = false;
		status_.link_alive = status_result.ok;
		const std::string err = recording_flag_missing ? "recording flag false"
			: (!status_result.error_string.empty() ? status_result.error_string : "logger.status failed");
		set_error(err);
		set_state(ServiceState::kError);
		publish_status();
		const int code = status_result.ok ? http_code(status_result, 409) : http_code(status_result, 500);
		send_ack(id, type, false, "", err, code);
		return;
	}
	status_.recording_active = true;
	status_.api_ok = true;
	status_.link_alive = true;
	status_.last_error.clear();
	set_state(ServiceState::kRecording);
	publish_status();
	const std::string message = !start_result.message.empty() ? start_result.message : "recording started";
	send_ack(id, type, true, message, "", http_code(start_result, 200));
}

void EkinoxService::handle_logger_stop(const std::string& id, const std::string& type) {
	std::string reason;
	if (!can_execute_logger_cmd(reason)) {
		status_.api_ok = false;
		set_error(reason);
		publish_status();
		send_ack(id, type, false, "", reason, 503);
		return;
	}
	if (status_.state != ServiceState::kRecording) {
		set_error("logger.stop invalid state");
		publish_status();
		send_ack(id, type, false, "", "INVALID_STATE", 409);
		return;
	}
	set_state(ServiceState::kStopping);
	publish_status();
	const auto http_code = [](const LoggerResult& res, int fallback) {
		return static_cast<int>(res.http_code != 0 ? res.http_code : fallback);
	};
	LoggerResult stop_result = EkinoxLoggerApi::stop_http(config_, config_.rest_api);
	if (!stop_result.ok) {
		status_.api_ok = false;
		status_.link_alive = false;
		const std::string err = !stop_result.error_string.empty() ? stop_result.error_string : "logger.stop failed";
		set_error(err);
		set_state(ServiceState::kError);
		publish_status();
		send_ack(id, type, false, "", err, http_code(stop_result, 500));
		return;
	}
	std::uint32_t raw_status = 0;
	bool recording = false;
	LoggerResult status_result = EkinoxLoggerApi::status_http(config_, config_.rest_api, raw_status, recording);
	const bool still_recording = status_result.ok && recording;
	if (!status_result.ok || still_recording) {
		status_.api_ok = false;
		status_.link_alive = status_result.ok;
		const std::string err = still_recording ? "recording flag true"
			: (!status_result.error_string.empty() ? status_result.error_string : "logger.status failed");
		set_error(err);
		set_state(ServiceState::kError);
		publish_status();
		const int code = status_result.ok ? http_code(status_result, 409) : http_code(status_result, 500);
		send_ack(id, type, false, "", err, code);
		return;
	}
	status_.recording_active = false;
	status_.api_ok = true;
	status_.link_alive = true;
	status_.last_error.clear();
	set_state(ServiceState::kIdle);
	publish_status();
	const std::string message = !stop_result.message.empty() ? stop_result.message : "recording stopped";
	send_ack(id, type, true, message, "", http_code(stop_result, 200));
}

void EkinoxService::handle_logger_status(const std::string& id, const std::string& type) {
	std::string reason;
	if (!can_execute_logger_cmd(reason)) {
		status_.api_ok = false;
		set_error(reason);
		publish_status();
		send_ack(id, type, false, "", reason, 503);
		return;
	}
	std::uint32_t raw_status = 0;
	bool recording = false;
	LoggerResult result = EkinoxLoggerApi::status_http(config_, config_.rest_api, raw_status, recording);
	const auto http_code = [](const LoggerResult& res, int fallback) {
		return static_cast<int>(res.http_code != 0 ? res.http_code : fallback);
	};
	if (result.ok) {
		status_.api_ok = true;
		status_.link_alive = true;
		status_.last_error.clear();
		status_.recording_active = recording;
		set_state(recording ? ServiceState::kRecording : ServiceState::kIdle);
		publish_status();
		const std::string message = !result.message.empty() ? result.message : (recording ? "recording active" : "recording inactive");
		send_ack(id, type, true, message, "", http_code(result, 200));
	} else {
		status_.api_ok = false;
		status_.link_alive = false;
		const std::string err = !result.error_string.empty() ? result.error_string : "logger.status failed";
		set_error(err);
		set_state(ServiceState::kError);
		publish_status();
		send_ack(id, type, false, "", err, http_code(result, 500));
	}
}

void EkinoxService::handle_ping(const std::string& id, const std::string& type) {
	send_ack(id, type, true, "pong", "", 200);
}

void EkinoxService::send_ack(const std::string& id,
	const std::string& type,
	bool ok,
	const std::string& message,
	const std::string& err,
	int http_code) {
	std::string ack_message = message;
	std::string ack_err = err;
	if (ok && ack_message.empty()) {
		ack_message = "ok";
	}
	if (!ok && ack_err.empty()) {
		ack_err = "error";
	}
	publish_ack(type, id, ok, http_code, ack_message, ack_err);
}

bool EkinoxService::can_execute_logger_cmd(std::string& err) const {
	if (!connected_) {
		err = "MQTT offline";
		return false;
	}
	if (!sensor_connected_ || !udp_session_) {
		err = "sensor offline";
		return false;
	}
	return true;
}

void EkinoxService::ensure_sensor_session(time_point now) {
#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)
	if (sensor_connected_) {
		return;
	}
	if (!udp_session_) {
		udp_session_ = std::make_unique<EkinoxUdpSession>();
	}
	if (!next_sensor_attempt_) {
		next_sensor_attempt_ = now;
	}
	if (now < *next_sensor_attempt_) {
		return;
	}
	std::string err;
	if (!udp_session_->open(udp_config_, err)) {
		status_.api_ok = false;
		status_.link_alive = false;
		set_error(err);
		publish_presence(false, err);
		current_backoff_ms_ = std::min(current_backoff_ms_ * 2, config_.timeouts.reconnect_backoff_max_ms);
		next_sensor_attempt_ = now + std::chrono::milliseconds(current_backoff_ms_);
		set_state(ServiceState::kConnecting);
		return;
	}
	sensor_connected_ = true;
	status_.api_ok = true;
	status_.link_alive = true;
	current_backoff_ms_ = config_.timeouts.reconnect_backoff_ms;
	set_state(ServiceState::kIdle);
	publish_presence(true, "");
	next_sensor_attempt_.reset();
#else
	(void)now;
	status_.api_ok = false;
	status_.link_alive = false;
	if (!reported_no_sbg_) {
		reported_no_sbg_ = true;
		set_error("sbgECom unavailable");
		publish_presence(false, "sbg disabled");
	}
#endif
}

void EkinoxService::poll_sensor(time_point now) {
	if (!sensor_connected_ || !udp_session_) {
		return;
	}
	std::string err;
	if (!udp_session_->poll(err)) {
		rx_timeout_strikes_ = 0;
		handle_sensor_error(err);
		return;
	}
	const auto age = udp_session_->last_rx_age_ms(now);
	const auto rx_dead_ms = static_cast<std::int64_t>(config_.timeouts.rx_dead_ms);
	if (age > rx_dead_ms) {
		++rx_timeout_strikes_;
		if (rx_timeout_strikes_ == 1) {
			std::cout << "[ekinox] sensor rx timeout detected (age=" << age << "ms)" << '\n';
			set_error("rx timeout");
		}
		status_.link_alive = false;
		status_.api_ok = false;
		const bool recording = (status_.state == ServiceState::kRecording) || status_.recording_active;
		if (recording && rx_timeout_strikes_ >= kRxTimeoutStrikesToReconnect) {
			std::cout << "[ekinox] sensor rx timeout strikes=" << rx_timeout_strikes_ << " reconnecting (recording)" << '\n';
			handle_sensor_disconnect("rx timeout");
		}
		return;
	}
	if (rx_timeout_strikes_ > 0) {
		std::cout << "[ekinox] sensor rx timeout cleared after strikes=" << rx_timeout_strikes_ << '\n';
	}
	rx_timeout_strikes_ = 0;
	status_.link_alive = true;
	status_.api_ok = true;
	if (!presence_online_) {
		publish_presence(true, "");
	}
}

void EkinoxService::handle_sensor_error(const std::string& reason) {
	set_error(reason);
	handle_sensor_disconnect(reason);
}

void EkinoxService::handle_sensor_disconnect(const std::string& reason) {
	rx_timeout_strikes_ = 0;
	if (udp_session_) {
		udp_session_->close();
	}
	sensor_connected_ = false;
	status_.link_alive = false;
	status_.api_ok = false;
	publish_presence(false, reason);
	set_state(ServiceState::kConnecting);
	next_sensor_attempt_ = steady_clock::now() + std::chrono::milliseconds(current_backoff_ms_);
}

void EkinoxService::set_state(ServiceState state) {
	if (status_.state == state) {
		return;
	}
	status_.state = state;
	std::cout << "[ekinox] state -> " << to_string(state) << '\n';
}

void EkinoxService::set_error(const std::string& message) {
	status_.last_error = message;
	status_.last_error_ts = unix_ts();
}

std::string EkinoxService::serialize_json(const Json::Value& value) const {
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	return Json::writeString(builder, value);
}

std::int64_t EkinoxService::unix_ts() const {
	auto now = std::chrono::system_clock::now();
	return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

int EkinoxService::wait_for_token_rc(const mqtt::token_ptr& tok) const {
	if (!tok) {
		return -1;
	}
	tok->wait();
	return tok->get_return_code();
}

}  // namespace ironsoft::ekinox
