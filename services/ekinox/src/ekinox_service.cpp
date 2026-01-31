#include "ironsoft/ekinox/ekinox_service.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

namespace ironsoft::ekinox {

namespace {
constexpr std::chrono::milliseconds kLoopSleep{50};
constexpr std::chrono::milliseconds kStateTransitionDelay{500};
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
	status_.api_ok = true;
	status_.link_alive = true;
	presence_.online = false;
	presence_.reason.clear();
	presence_.timestamp = unix_ts();

	start_tp_ = steady_clock::now();
	last_rx_tp_ = start_tp_;
	next_status_pub_ = start_tp_;
	next_heartbeat_pub_ = start_tp_;
	next_simulated_rx_ = start_tp_;
	next_reconnect_attempt_ = start_tp_;
	current_backoff_ms_ = config_.timeouts.reconnect_backoff_ms;
}

EkinoxService::~EkinoxService() {
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
			simulate_link_tick(now);
			update_watchdog(now);
			apply_pending_transition(now);

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

	if (connected_) {
		publish_presence(false, "shutdown");
		publish_status();
	}

	disconnect();
	return true;
}

void EkinoxService::connection_lost(const std::string& cause) {
	std::cerr << "[ekinox] MQTT connection lost: " << cause << '\n';
	connected_ = false;
	presence_online_ = false;
	status_.link_alive = false;
	set_state(ServiceState::kDisconnected);
	next_reconnect_attempt_ = steady_clock::now();
}

void EkinoxService::message_arrived(mqtt::const_message_ptr msg) {
	std::lock_guard<std::mutex> lock(commands_mutex_);
	pending_commands_.push(msg->get_payload_str());
}

bool EkinoxService::connect_once() {
	try {
		set_state(ServiceState::kConnecting);
		std::cout << "[ekinox] Connecting to " << server_uri_ << '\n';
		client_.connect(conn_opts_)->wait();
		client_.subscribe(topics_.cmd, 1)->wait();
		connected_ = true;
		presence_online_ = false;
		status_.link_alive = true;
		status_.api_ok = true;
		status_.recording_active = false;
		pending_transition_.reset();
		set_state(ServiceState::kIdle);
		publish_presence(true, "");
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
	presence_.online = online;
	presence_.reason = reason;
	presence_.timestamp = unix_ts();
	Json::Value payload = build_presence_json(presence_);
	auto msg = mqtt::make_message(topics_.presence, serialize_json(payload));
	msg->set_qos(1);
	msg->set_retained(true);
	client_.publish(msg);
	presence_online_ = online;
}

void EkinoxService::publish_status() {
	Json::Value payload = build_status_json(status_);
	auto msg = mqtt::make_message(topics_.status, serialize_json(payload));
	msg->set_qos(1);
	msg->set_retained(true);
	client_.publish(msg);
}

void EkinoxService::publish_heartbeat(std::int64_t uptime_s) {
	Json::Value payload = build_heartbeat_json(++heartbeat_seq_, uptime_s);
	auto msg = mqtt::make_message(topics_.heartbeat, serialize_json(payload));
	msg->set_qos(0);
	msg->set_retained(false);
	client_.publish(msg);
}

void EkinoxService::publish_ack(const std::string& payload) {
	auto msg = mqtt::make_message(topics_.ack, payload);
	msg->set_qos(1);
	msg->set_retained(false);
	client_.publish(msg);
}

void EkinoxService::drain_commands() {
	std::queue<std::string> local;
	{
		std::lock_guard<std::mutex> lock(commands_mutex_);
		std::swap(local, pending_commands_);
	}
	while (!local.empty()) {
		handle_command_message(local.front());
		local.pop();
	}
}

void EkinoxService::handle_command_message(const std::string& payload) {
	Json::CharReaderBuilder rb;
	rb["collectComments"] = false;
	Json::Value root;
	std::string errs;
	std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
	if (!reader->parse(payload.data(), payload.data() + payload.size(), &root, &errs)) {
		std::cerr << "[ekinox] bad cmd json: " << errs << '\n';
		Json::Value ack = build_ack_json("", false, "", "BAD_JSON");
		publish_ack(serialize_json(ack));
		return;
	}
	const std::string id = root.get("id", "").asString();
	const std::string type = root.get("type", "").asString();
	CommandType cmd_type = CommandType::kUnknown;
	try_parse_command_type(type, cmd_type);

	if (id.empty()) {
		Json::Value ack = build_ack_json("", false, "", "MISSING_ID");
		publish_ack(serialize_json(ack));
		return;
	}

	switch (cmd_type) {
	case CommandType::kPing:
		handle_ping(id);
		break;
	case CommandType::kStartLogger:
		handle_start_logger(id);
		break;
	case CommandType::kStopLogger:
		handle_stop_logger(id);
		break;
	default:
		Json::Value ack = build_ack_json(id, false, "", "UNKNOWN_CMD");
		publish_ack(serialize_json(ack));
		break;
	}
}

void EkinoxService::handle_start_logger(const std::string& id) {
	if (status_.state != ServiceState::kIdle) {
		set_error("start_logger invalid state");
		Json::Value ack = build_ack_json(id, false, "", "INVALID_STATE");
		publish_ack(serialize_json(ack));
		return;
	}
	set_state(ServiceState::kStarting);
	// TODO: Replace stub delay with real sbgECom logger start once SDK is wired (prompt 4/6).
	schedule_transition(ServiceState::kRecording, true);
	Json::Value ack = build_ack_json(id, true, "logger starting", "");
	publish_ack(serialize_json(ack));
}

void EkinoxService::handle_stop_logger(const std::string& id) {
	if (status_.state != ServiceState::kRecording) {
		set_error("stop_logger invalid state");
		Json::Value ack = build_ack_json(id, false, "", "INVALID_STATE");
		publish_ack(serialize_json(ack));
		return;
	}
	set_state(ServiceState::kStopping);
	// TODO: Replace stub delay with real sbgECom logger stop once SDK is wired (prompt 4/6).
	schedule_transition(ServiceState::kIdle, false);
	Json::Value ack = build_ack_json(id, true, "logger stopping", "");
	publish_ack(serialize_json(ack));
}

void EkinoxService::handle_ping(const std::string& id) {
	Json::Value ack = build_ack_json(id, true, "pong", "");
	publish_ack(serialize_json(ack));
}

void EkinoxService::schedule_transition(ServiceState target_state, bool recording_active) {
	PendingTransition pending;
	pending.target = target_state;
	pending.recording_active = recording_active;
	pending.due = steady_clock::now() + kStateTransitionDelay;
	pending_transition_ = pending;
}

void EkinoxService::apply_pending_transition(time_point now) {
	if (!pending_transition_.has_value()) {
		return;
	}
	if (now < pending_transition_->due) {
		return;
	}
	status_.recording_active = pending_transition_->recording_active;
	set_state(pending_transition_->target);
	pending_transition_.reset();
}

void EkinoxService::simulate_link_tick(time_point now) {
	if (now >= next_simulated_rx_) {
		// TODO: In prompt 4/6 feed the real UDP RX timestamp from the transport instead of this stub timer.
		last_rx_tp_ = now;
		status_.link_alive = true;
		next_simulated_rx_ = now + std::chrono::milliseconds(250);
	}
}

void EkinoxService::update_watchdog(time_point now) {
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_rx_tp_);
	if (elapsed.count() > config_.timeouts.rx_dead_ms) {
		status_.link_alive = false;
		set_state(ServiceState::kDisconnected);
		return;
	}
	status_.link_alive = true;
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

}  // namespace ironsoft::ekinox
