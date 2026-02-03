#include "ironsoft/ekinox/ekinox_service.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
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
constexpr std::chrono::seconds kVerificationDelay{1};
constexpr std::chrono::milliseconds kPresenceDefault{10000};
constexpr std::chrono::milliseconds kPresenceMinPublishInterval{2000};
constexpr std::size_t kMaxErrorText{256};

std::string sanitize_and_clip(std::string_view text, std::size_t max_len = 300) {
	std::string out;
	out.reserve(std::min<std::size_t>(text.size(), max_len));
	for (unsigned char byte : text) {
		char ch = static_cast<char>(byte);
		if (byte == '\\' || byte == '"') {
			if (out.size() + 2 > max_len) {
				break;
			}
			out.push_back('\\');
			out.push_back(ch);
			continue;
		}
		if (byte < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') {
			ch = ' ';
		}
		if (byte >= 0x80) {
			ch = '?';
		}
		if (out.size() + 1 > max_len) {
			break;
		}
		out.push_back(ch);
	}
	return out;
}

std::string make_client_id() {
	const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
	return "ekinox_service_" + std::to_string(static_cast<long long>(now));
}

int http_code_or_default(const LoggerResult& result, int fallback) {
	return static_cast<int>(result.http_code != 0 ? result.http_code : fallback);
}

std::string rest_error_or_http(const LoggerResult& result) {
	if (!result.body.empty()) {
		return sanitize_and_clip(result.body);
	}
	if (!result.error_string.empty()) {
		return sanitize_and_clip(result.error_string);
	}
	if (result.http_code > 0) {
		return "HTTP_" + std::to_string(result.http_code);
	}
	if (result.error_code != 0) {
		return "HTTP_" + std::to_string(result.error_code);
	}
	return {};
}

bool logger_state_matches(const LoggerResult& result, bool expected) {
	return result.ok && result.has_recording_flag && (result.recording_active == expected);
}

bool result_reports_recording(const LoggerResult& result) {
	return result.ok && result.has_recording_flag && result.recording_active;
}

bool result_reports_idle(const LoggerResult& result) {
	return result.ok && result.has_recording_flag && !result.recording_active;
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
	const int presence_ms = (config_.timeouts.presence_timeout_ms > 0)
		? config_.timeouts.presence_timeout_ms
		: static_cast<int>(kPresenceDefault.count());
	presence_timeout_ = std::chrono::milliseconds(presence_ms);
	rest_alive_ttl_ = presence_timeout_;
	const auto half = presence_timeout_ / 2;
	const auto min_poll = std::chrono::milliseconds(1000);
	const auto max_poll = std::chrono::milliseconds(2000);
	if (half.count() <= 0) {
		rest_poll_interval_ = min_poll;
	} else {
		rest_poll_interval_ = std::clamp(half, min_poll, max_poll);
	}
	std::cout << "[ekinox] timers presence_timeout_ms=" << presence_timeout_.count()
		<< " rest_poll_interval_ms=" << rest_poll_interval_.count() << '\n';
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
		const auto now_ms = mono_ms(now);
		ensure_connection(now);

		if (connected_) {
			drain_commands();
			ensure_sensor_session(now);
			poll_sensor(now);
			refresh_rest_health(now, now_ms);
			if (next_rest_poll_ms_ <= 0) {
				next_rest_poll_ms_ = now_ms + static_cast<std::int64_t>(rest_poll_interval_.count());
			}
			if (now_ms >= next_rest_poll_ms_) {
				request_logger_state("poll.timer");
				next_rest_poll_ms_ = now_ms + static_cast<std::int64_t>(rest_poll_interval_.count());
			}

			if (now >= next_status_pub_) {
				publish_status();
				next_status_pub_ = now + kStatusInterval;
			}
			if (now >= next_heartbeat_pub_) {
				auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_tp_).count();
				publish_heartbeat(uptime);
				next_heartbeat_pub_ = now + kHeartbeatInterval;
			}
		} else {
			next_rest_poll_ms_ = 0;
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
	rest_success_streak_ = 0;
	next_rest_poll_ms_ = 0;
	if (udp_session_) {
		udp_session_->close();
	}
	sensor_connected_ = false;
	sensor_reconnect_needed_ = false;
	const auto now = steady_clock::now();
	const auto now_ms = mono_ms(now);
	update_link_health(now_ms);
	set_state(ServiceState::kDisconnected);
	next_reconnect_attempt_ = now;
	update_presence_state(now_ms, "mqtt offline");
}

void EkinoxService::message_arrived(mqtt::const_message_ptr msg) {
	const auto topic = msg->get_topic();
	const auto payload = msg->get_payload_str();
	std::cout << "[mqtt] RX topic=" << topic << " payload=" << payload << '\n';
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
		std::cout << "[mqtt] subscribed cmd=" << topics_.cmd << " ack=" << topics_.ack << '\n';
		subscribe_and_log(topics_.cmd_legacy);
		if (!topics_.cmd_legacy.empty()) {
			std::cout << "[mqtt] subscribed cmd_legacy=" << topics_.cmd_legacy << '\n';
		}
		std::cout << "[ekinox] MQTT topics:\n"
			<< "  server_uri=" << server_uri_ << '\n'
			<< "  drone_id=" << config_.mqtt.drone_id << '\n'
			<< "  cmd=" << topics_.cmd << '\n'
			<< "  cmd_legacy=" << (topics_.cmd_legacy.empty() ? "-" : topics_.cmd_legacy) << '\n'
			<< "  ack=" << topics_.ack << '\n'
			<< "  ack_legacy=" << (topics_.ack_legacy.empty() ? "-" : topics_.ack_legacy) << '\n'
			<< "  ekinox/status=" << topics_.status << '\n'
			<< "  ekinox/presence=" << topics_.presence << '\n'
			<< "  ekinox/heartbeat=" << topics_.heartbeat << '\n';
		connected_ = true;
		presence_online_ = false;
		status_.link_alive = false;
		status_.api_ok = false;
		status_.recording_active = false;
		udp_link_alive_ = false;
		rest_alive_ = false;
		last_rest_success_ = time_point{};
		last_rest_ok_ = time_point{};
		last_rest_ok_ts_ms_ = 0;
		rest_fail_streak_ = 0;
		rest_success_streak_ = 0;
		last_presence_emit_ts_ms_ = 0;
		sensor_reconnect_needed_ = false;
		last_rest_error_.clear();
		next_rest_poll_ms_ = mono_ms() + static_cast<std::int64_t>(rest_poll_interval_.count());
		set_state(ServiceState::kConnecting);
		const auto now = steady_clock::now();
		update_presence_state(mono_ms(now), "connecting");
		publish_status();
		std::cout << "[ekinox] Connected" << '\n';
		request_logger_state("connect.selftest");
		publish_status();
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
		rest_success_streak_ = 0;
		next_rest_poll_ms_ = 0;
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
	const std::string sanitized_reason = sanitize_and_clip(reason);
	const auto now_ms = mono_ms();
	const bool same_state = (presence_online_ == online);
	const auto min_interval_ms = static_cast<std::int64_t>(kPresenceMinPublishInterval.count());
	const bool rate_limited = same_state && (last_presence_emit_ts_ms_ > 0) &&
		((now_ms - last_presence_emit_ts_ms_) < min_interval_ms);
	if (!connected_) {
		presence_.online = online;
		presence_.reason = sanitized_reason;
		presence_.timestamp = unix_ts();
		last_presence_ts_ = presence_.timestamp;
		presence_online_ = online;
		std::cout << "[presence] -> " << (online ? "ONLINE" : "OFFLINE")
			<< " reason='" << (presence_.reason.empty() ? "-" : presence_.reason) << "' (deferred)" << '\n';
		return;
	}
	if (rate_limited) {
		return;
	}
	presence_.online = online;
	presence_.reason = sanitized_reason;
	presence_.timestamp = unix_ts();
	last_presence_ts_ = presence_.timestamp;
	Json::Value payload = build_presence_json(presence_);
	auto msg = mqtt::make_message(topics_.presence, serialize_json(payload));
	msg->set_qos(1);
	msg->set_retained(true);
	client_.publish(msg);
	presence_online_ = online;
	last_presence_emit_ts_ms_ = now_ms;
	std::cout << "[presence] -> " << (online ? "ONLINE" : "OFFLINE")
		<< " reason='" << (sanitized_reason.empty() ? "-" : sanitized_reason) << "'" << '\n';
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
	ack["ts"] = static_cast<Json::Int64>(unix_ts());
	if (!message.empty()) {
		ack["message"] = message;
	}
	if (!err.empty()) {
		ack["error"] = err;
	}
	if (http_code > 0) {
		ack["http_code"] = http_code;
	}
	const auto payload = serialize_json(ack);
	auto publish_topic = [this, &payload, &type, &id, ok](const std::string& topic) {
		if (topic.empty()) {
			return;
		}
		int rc = -1;
		if (!connected_) {
			std::cerr << "[mqtt] TX ack error topic=" << topic << " reason=offline" << '\n';
		} else {
			auto msg = mqtt::make_message(topic, payload);
			msg->set_qos(1);
			msg->set_retained(false);
			try {
				auto token = client_.publish(msg);
				rc = wait_for_token_rc(token);
			} catch (const mqtt::exception& ex) {
				std::cerr << "[mqtt] TX ack error topic=" << topic << " what=" << ex.what() << '\n';
			}
		}
		std::cout << "[mqtt] TX ack topic=" << topic
			<< " payload=" << payload
			<< " type=" << (type.empty() ? "-" : type)
			<< " id=" << (id.empty() ? "-" : id)
			<< " ok=" << (ok ? "1" : "0")
			<< " rc=" << rc
			<< '\n';
	};
	publish_topic(topics_.ack);
	if (!topics_.ack_legacy.empty() && topics_.ack_legacy != topics_.ack) {
		publish_topic(topics_.ack_legacy);
	}
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
	const bool supported = (cmd_type == CommandType::kPing) ||
		(cmd_type == CommandType::kLoggerStart) ||
		(cmd_type == CommandType::kLoggerStop) ||
		(cmd_type == CommandType::kLoggerStatus);
	if (!supported) {
		publish_ack(type, id, false, 400, "", "unknown_type");
		return;
	}
	std::cout << "[ekinox] dispatch type=" << type << " id=" << id << '\n';
	switch (cmd_type) {
	case CommandType::kPing:
		handle_ping(id, type);
		break;
	case CommandType::kLoggerStart:
		handle_logger_start(id, type, root);
		break;
	case CommandType::kLoggerStop:
		handle_logger_stop(id, type, root);
		break;
	case CommandType::kLoggerStatus:
		handle_logger_status(id, type, root);
		break;
	default:
		publish_ack(type, id, false, 400, "", "unknown_type");
		break;
	}
}

void EkinoxService::handle_logger_start(const std::string& id, const std::string& type, const Json::Value& cmd) {
	std::string reason;
	if (!can_execute_logger_cmd(reason)) {
		set_error(reason);
		publish_status();
		send_ack(id, type, false, "", reason, 503);
		return;
	}
	std::string rest_reason;
	if (!validate_rest_endpoint(rest_reason)) {
		set_error(rest_reason);
		const auto now = steady_clock::now();
		const auto now_ms = mono_ms(now);
		update_link_health(now_ms);
		update_presence_state(now_ms, rest_reason);
		publish_status();
		send_ack(id, type, false, rest_reason, "bad_config", 400);
		return;
	}
	LoggerResult pre_status = request_logger_state("logger.start.precheck");
	publish_status();
	if (result_reports_recording(pre_status)) {
		send_ack(id, type, true, "already recording", "", http_code_or_default(pre_status, 200));
		return;
	}
	std::string session_name = extract_session_name(cmd);
	if (session_name.empty()) {
		session_name = generate_session_name();
	}
	set_state(ServiceState::kStarting);
	publish_status();
	LoggerResult start_result = EkinoxLoggerApi::dataLoggerStart(config_, config_.rest_api, session_name);
	mark_rest_result(start_result, "logger.start");
	const int start_http_code = http_code_or_default(start_result, 500);
	if (!start_result.ok) {
		if (start_result.http_code == 409) {
			LoggerResult conflict_state = request_logger_state("logger.start.conflict");
			publish_status();
			send_ack(id, type, true, "already recording", "", start_http_code);
			return;
		}
		const bool not_found = (start_result.http_code == 404);
		set_state(not_found ? ServiceState::kIdle : ServiceState::kError);
		std::string err = rest_error_or_http(start_result);
		if (err.empty()) {
			err = "HTTP_" + std::to_string(start_http_code);
		}
		set_error(err);
		publish_status();
		send_ack(id, type, false, "start failed", err, start_http_code);
		return;
	}
	LoggerResult verify_now = request_logger_state("logger.start.verify.now");
	std::this_thread::sleep_for(kVerificationDelay);
	LoggerResult verify_later = request_logger_state("logger.start.verify.later");
	const bool confirmed = logger_state_matches(verify_now, true) || logger_state_matches(verify_later, true);
	publish_status();
	if (confirmed) {
		send_ack(id, type, true, "recording started", "", start_http_code);
		return;
	}
	const LoggerResult& failing = verify_later.ok ? verify_later : verify_now;
	std::string verify_err = rest_error_or_http(failing);
	if (verify_err.empty()) {
		verify_err = "state_not_confirmed";
	}
	const int verify_code = http_code_or_default(failing, start_http_code);
	send_ack(id, type, false, "recording not confirmed", verify_err, verify_code);
}

void EkinoxService::handle_logger_stop(const std::string& id, const std::string& type, const Json::Value& cmd) {
	(void)cmd;
	std::string reason;
	if (!can_execute_logger_cmd(reason)) {
		set_error(reason);
		publish_status();
		send_ack(id, type, false, "", reason, 503);
		return;
	}
	std::string rest_reason;
	if (!validate_rest_endpoint(rest_reason)) {
		set_error(rest_reason);
		const auto now = steady_clock::now();
		const auto now_ms = mono_ms(now);
		update_link_health(now_ms);
		update_presence_state(now_ms, rest_reason);
		publish_status();
		send_ack(id, type, false, rest_reason, "bad_config", 400);
		return;
	}
	LoggerResult pre_status = request_logger_state("logger.stop.precheck");
	publish_status();
	if (result_reports_idle(pre_status)) {
		status_.session_name.clear();
		set_state(ServiceState::kIdle);
		publish_status();
		send_ack(id, type, true, "already stopped", "", http_code_or_default(pre_status, 200));
		return;
	}
	set_state(ServiceState::kStopping);
	publish_status();
	LoggerResult stop_result = EkinoxLoggerApi::dataLoggerStop(config_, config_.rest_api);
	mark_rest_result(stop_result, "logger.stop");
	const int stop_http_code = http_code_or_default(stop_result, 500);
	if (!stop_result.ok) {
		if (stop_result.http_code == 500 || stop_result.http_code == 409) {
			LoggerResult retry_state = request_logger_state("logger.stop.post_error");
			publish_status();
			if (result_reports_idle(retry_state)) {
				status_.session_name.clear();
				set_state(ServiceState::kIdle);
				publish_status();
				send_ack(id, type, true, "already stopped", "", stop_http_code);
				return;
			}
		}
		const bool not_found = (stop_result.http_code == 404);
		set_state(not_found ? ServiceState::kIdle : ServiceState::kError);
		std::string err = rest_error_or_http(stop_result);
		if (err.empty()) {
			err = "HTTP_" + std::to_string(stop_http_code);
		}
		set_error(err);
		publish_status();
		send_ack(id, type, false, "stop failed", err, stop_http_code);
		return;
	}
	LoggerResult verify_now = request_logger_state("logger.stop.verify.now");
	std::this_thread::sleep_for(kVerificationDelay);
	LoggerResult verify_later = request_logger_state("logger.stop.verify.later");
	const bool confirmed = logger_state_matches(verify_now, false) || logger_state_matches(verify_later, false);
	if (confirmed) {
		status_.session_name.clear();
	}
	publish_status();
	if (confirmed) {
		send_ack(id, type, true, "recording stopped", "", stop_http_code);
		return;
	}
	const LoggerResult& failing = verify_later.ok ? verify_later : verify_now;
	std::string verify_err = rest_error_or_http(failing);
	if (verify_err.empty()) {
		verify_err = "state_not_confirmed";
	}
	const int verify_code = http_code_or_default(failing, stop_http_code);
	send_ack(id, type, false, "recording still active", verify_err, verify_code);
}

void EkinoxService::handle_logger_status(const std::string& id, const std::string& type, const Json::Value& cmd) {
	(void)cmd;
	std::string reason;
	if (!can_execute_logger_cmd(reason)) {
		set_error(reason);
		publish_status();
		send_ack(id, type, false, "", reason, 503);
		return;
	}
	std::string rest_reason;
	if (!validate_rest_endpoint(rest_reason)) {
		set_error(rest_reason);
		const auto now = steady_clock::now();
		const auto now_ms = mono_ms(now);
		update_link_health(now_ms);
		update_presence_state(now_ms, rest_reason);
		publish_status();
		send_ack(id, type, false, rest_reason, "bad_config", 400);
		return;
	}
	LoggerResult result = request_logger_state("logger.status.cmd");
	publish_status();
	if (result.ok) {
		const std::string message = !result.message.empty() ? result.message : (status_.recording_active ? "recording active" : "recording inactive");
		send_ack(id, type, true, message, "", http_code_or_default(result, 200));
		return;
	}
	std::string err = rest_error_or_http(result);
	if (err.empty()) {
		err = "HTTP_" + std::to_string(http_code_or_default(result, 500));
	}
	send_ack(id, type, false, "", err, http_code_or_default(result, 500));
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
	ack_message = sanitize_and_clip(ack_message);
	ack_err = sanitize_and_clip(ack_err);
	publish_ack(type, id, ok, http_code, ack_message, ack_err);
}

bool EkinoxService::can_execute_logger_cmd(std::string& err) const {
	if (!connected_) {
		err = "MQTT offline";
		return false;
	}
	return true;
}

bool EkinoxService::validate_rest_endpoint(std::string& err) const {
	if (config_.ekinox.ip.empty()) {
		err = "ekinox.ip missing";
		return false;
	}
	if (config_.ekinox.rest_port <= 0) {
		err = "rest_port is 0";
		return false;
	}
	return true;
}

void EkinoxService::mark_rest_result(const LoggerResult& result, const std::string& context_reason) {
	const auto now = steady_clock::now();
	const auto now_ms = mono_ms(now);
	const bool rest_success = result.ok && (result.http_code >= 200) && (result.http_code < 300);
	if (rest_success) {
		rest_alive_ = true;
		last_rest_success_ = now;
		last_rest_ok_ = now;
		last_rest_ok_ts_ms_ = now_ms;
		rest_fail_streak_ = 0;
		if (rest_success_streak_ < std::numeric_limits<int>::max()) {
			++rest_success_streak_;
		}
		last_rest_error_.clear();
		update_link_health(now_ms);
		update_presence_state(now_ms);
		return;
	}
	rest_success_streak_ = 0;
	last_rest_error_ = !result.error_string.empty() ? result.error_string : context_reason;
	const bool severe_failure = (result.http_code >= 500) || (result.http_code == 0);
	if (severe_failure) {
		++rest_fail_streak_;
	}
	const bool offline = rest_offline_condition(now_ms);
	const std::int64_t age_since_ok = rest_age_ms(now_ms);
	std::cout << "[rest] fail context=" << context_reason
		<< " streak=" << rest_fail_streak_
		<< " now=" << now_ms
		<< " last_ok=" << last_rest_ok_ts_ms_
		<< " age=" << age_since_ok
		<< " offline_decision=" << (offline ? 1 : 0)
		<< " reason='" << (last_rest_error_.empty() ? context_reason : last_rest_error_) << "'" << '\n';
	rest_alive_ = !offline;
	update_link_health(now_ms);
	if (offline) {
		update_presence_state(now_ms);
	}
}

void EkinoxService::update_link_health(std::int64_t now_ms) {
	status_.api_ok = !rest_offline_condition(now_ms);
	status_.link_alive = sensor_connected_ && udp_link_alive_;
}

void EkinoxService::refresh_rest_health(time_point now, std::int64_t now_ms) {
	const bool offline = rest_offline_condition(now_ms);
	if (!offline) {
		if (!rest_alive_) {
			rest_alive_ = true;
		}
		return;
	}
	if (!rest_alive_) {
		return;
	}
	const std::int64_t age_since_ok = rest_age_ms(now_ms);
	std::cout << "[rest] timeout now=" << now_ms
		<< " last_ok=" << last_rest_ok_ts_ms_
		<< " age=" << age_since_ok
		<< " streak=" << rest_fail_streak_
		<< " offline_decision=1" << '\n';
	rest_alive_ = false;
	if (last_rest_error_.empty()) {
		last_rest_error_ = "rest timeout";
	}
	update_link_health(now_ms);
	update_presence_state(now_ms);
}

void EkinoxService::set_udp_link_alive(bool alive) {
	udp_link_alive_ = alive;
	update_link_health(mono_ms());
}

void EkinoxService::update_presence_state(std::int64_t now_ms, const std::string& reason_override) {
	const auto timeout_ms = static_cast<std::int64_t>(presence_timeout_.count());
	const std::int64_t age = rest_age_ms(now_ms);
	const bool rest_ok = !rest_offline_condition(now_ms);
	const bool rest_ready = rest_ok && (rest_success_streak_ >= 2);
	const bool should_online = connected_ && rest_ready;
	std::string reason = reason_override;
	if (!should_online && reason.empty()) {
		if (!connected_) {
			reason = "mqtt offline";
		} else if (!rest_ok) {
			reason = last_rest_error_.empty() ? "rest timeout" : last_rest_error_;
		} else if (rest_success_streak_ < 2) {
			reason = "rest hysteresis";
		}
	}
	const std::string log_reason = sanitize_and_clip(reason);
	std::cout << "[presence] decision now=" << now_ms
		<< " last_ok=" << last_rest_ok_ts_ms_
		<< " age=" << age
		<< " rest_fail=" << rest_fail_streak_
		<< " rest_success=" << rest_success_streak_
		<< " timeout=" << timeout_ms
		<< " connected=" << (connected_ ? 1 : 0)
		<< " recording=" << (status_.recording_active ? 1 : 0)
		<< " decision=" << (should_online ? "ONLINE" : "OFFLINE")
		<< " reason='" << (log_reason.empty() ? "-" : log_reason) << "'" << '\n';
	if (last_rest_ok_ts_ms_ > 0 && age > timeout_ms * 10) {
		std::cerr << "[presence][warn] suspicious rest age now=" << now_ms
			<< " last_ok=" << last_rest_ok_ts_ms_
			<< " age=" << age
			<< " timeout=" << timeout_ms << '\n';
	}
	publish_presence(should_online, reason);
}

LoggerResult EkinoxService::request_logger_state(const std::string& context_reason) {
	LoggerResult result = EkinoxLoggerApi::dataLoggerGet(config_, config_.rest_api);
	mark_rest_result(result, context_reason);
	if (result.ok) {
		status_.last_error.clear();
		if (result.has_recording_flag) {
			status_.recording_active = result.recording_active;
			set_state(result.recording_active ? ServiceState::kRecording : ServiceState::kIdle);
		}
		if (!result.session_name.empty()) {
			status_.session_name = result.session_name;
		} else if (!status_.recording_active) {
			status_.session_name.clear();
		}
		return result;
	}
	std::string err = rest_error_or_http(result);
	if (err.empty()) {
		err = context_reason;
	}
	if (!err.empty()) {
		set_error(err);
	}
	if (result.http_code == 404) {
		status_.recording_active = false;
		status_.session_name.clear();
		set_state(ServiceState::kIdle);
	} else {
		set_state(ServiceState::kError);
	}
	return result;
}

std::string EkinoxService::generate_session_name() const {
	const auto now = std::chrono::system_clock::now();
	const std::time_t tt = std::chrono::system_clock::to_time_t(now);
	std::tm tm_utc{};
#if defined(_WIN32)
	gmtime_s(&tm_utc, &tt);
#else
	gmtime_r(&tt, &tm_utc);
#endif
	std::ostringstream oss;
	oss << "IronSoft_" << std::put_time(&tm_utc, "%Y%m%d_%H%M%S");
	return oss.str();
}

std::string EkinoxService::extract_session_name(const Json::Value& cmd) const {
	auto read_field = [](const Json::Value& node) -> std::string {
		if (node.isString()) {
			return node.asString();
		}
		return {};
	};
	if (auto name = read_field(cmd["sessionName"]); !name.empty()) {
		return name;
	}
	if (auto name = read_field(cmd["session_name"]); !name.empty()) {
		return name;
	}
	if (const Json::Value& payload = cmd["payload"]; payload.isObject()) {
		if (auto name = read_field(payload["sessionName"]); !name.empty()) {
			return name;
		}
	}
	if (const Json::Value& params = cmd["params"]; params.isObject()) {
		if (auto name = read_field(params["sessionName"]); !name.empty()) {
			return name;
		}
	}
	return {};
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
		set_error(err);
		set_udp_link_alive(false);
		current_backoff_ms_ = std::min(current_backoff_ms_ * 2, config_.timeouts.reconnect_backoff_max_ms);
		next_sensor_attempt_ = now + std::chrono::milliseconds(current_backoff_ms_);
		return;
	}
	sensor_connected_ = true;
	rx_timeout_strikes_ = 0;
	set_udp_link_alive(true);
	current_backoff_ms_ = config_.timeouts.reconnect_backoff_ms;
	next_sensor_attempt_.reset();
#else
	sensor_connected_ = false;
	udp_link_alive_ = false;
	update_link_health(mono_ms(now));
	if (!reported_no_sbg_) {
		reported_no_sbg_ = true;
		set_error("sbgECom unavailable");
	}
#endif
}

void EkinoxService::poll_sensor(time_point now) {
	if (!sensor_connected_ || !udp_session_) {
		return;
	}
	const bool recording = (status_.state == ServiceState::kRecording) || status_.recording_active;
	if (sensor_reconnect_needed_ && !recording) {
		std::cout << "[ekinox] sensor deferred reconnect (recording finished)" << '\n';
		sensor_reconnect_needed_ = false;
		handle_sensor_disconnect("rx timeout (deferred)");
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
		set_udp_link_alive(false);
		if (recording) {
			if (rx_timeout_strikes_ >= kRxTimeoutStrikesToReconnect && !sensor_reconnect_needed_) {
				std::cout << "[ekinox] sensor rx timeout strikes=" << rx_timeout_strikes_ << " (recording) – deferring reconnect" << '\n';
				sensor_reconnect_needed_ = true;
			}
			return;
		}
		if (rx_timeout_strikes_ >= kRxTimeoutStrikesToReconnect) {
			std::cout << "[ekinox] sensor rx timeout strikes=" << rx_timeout_strikes_ << " reconnecting" << '\n';
			handle_sensor_disconnect("rx timeout");
		}
		return;
	}
	if (rx_timeout_strikes_ > 0) {
		std::cout << "[ekinox] sensor rx timeout cleared after strikes=" << rx_timeout_strikes_ << '\n';
	}
	rx_timeout_strikes_ = 0;
	if (sensor_reconnect_needed_) {
		std::cout << "[ekinox] sensor rx recovered, cancelling deferred reconnect" << '\n';
		sensor_reconnect_needed_ = false;
	}
	set_udp_link_alive(true);
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
	udp_link_alive_ = false;
	sensor_reconnect_needed_ = false;
	set_udp_link_alive(false);
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
	status_.last_error = sanitize_and_clip(message);
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

std::int64_t EkinoxService::mono_ms() {
	return mono_ms(steady_clock::now());
}

std::int64_t EkinoxService::mono_ms(time_point tp) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

std::int64_t EkinoxService::rest_age_ms(std::int64_t now_ms) const {
	if (last_rest_ok_ts_ms_ <= 0) {
		return std::numeric_limits<std::int64_t>::max();
	}
	if (now_ms < last_rest_ok_ts_ms_) {
		std::cerr << "[rest][warn] monotonic regression now=" << now_ms
			<< " last_ok=" << last_rest_ok_ts_ms_ << '\n';
		return 0;
	}
	return now_ms - last_rest_ok_ts_ms_;
}

bool EkinoxService::rest_offline_condition(std::int64_t now_ms) const {
	const auto timeout_ms = static_cast<std::int64_t>(presence_timeout_.count());
	const std::int64_t age_ms = rest_age_ms(now_ms);
	if (rest_fail_streak_ >= 3) {
		return true;
	}
	return age_ms > timeout_ms;
}

}  // namespace ironsoft::ekinox
