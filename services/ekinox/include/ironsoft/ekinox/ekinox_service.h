#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>

#include <mqtt/async_client.h>
#include <json/json.h>

#include "ironsoft/ekinox/ekinox_config.h"
#include "ironsoft/ekinox/ekinox_state.h"
#include "ironsoft/ekinox/ekinox_topics.h"
#include "ironsoft/ekinox/ekinox_logger_api.h"
#include "ironsoft/ekinox/ekinox_udp_session.h"
#include "ironsoft/ekinox/ekinox_types.h"

namespace ironsoft::ekinox {

class EkinoxService : public mqtt::callback {
public:
	explicit EkinoxService(EkinoxConfig config);
	~EkinoxService() override;

	bool run(std::atomic_bool& stop_flag);

	void connection_lost(const std::string& cause) override;
	void message_arrived(mqtt::const_message_ptr msg) override;

private:
	using steady_clock = std::chrono::steady_clock;
	using time_point = steady_clock::time_point;

	bool connect_once();
	void disconnect();
	void ensure_connection(time_point now);
	void publish_presence(bool online, const std::string& reason);
	void publish_status();
	void publish_heartbeat(std::int64_t uptime_s);
	void publish_ack(const std::string& type,
		const std::string& id,
		bool ok,
		int http_code,
		const std::string& message,
		const std::string& err);
	void drain_commands();
	void handle_command_message(const std::string& topic, const std::string& payload);
	void handle_logger_start(const std::string& id, const std::string& type, const Json::Value& cmd);
	void handle_logger_stop(const std::string& id, const std::string& type, const Json::Value& cmd);
	void handle_logger_status(const std::string& id, const std::string& type, const Json::Value& cmd);
	void handle_ping(const std::string& id, const std::string& type);
	void ensure_sensor_session(time_point now);
	void poll_sensor(time_point now);
	void handle_sensor_error(const std::string& reason);
	void handle_sensor_disconnect(const std::string& reason);
	void send_ack(const std::string& id,
		const std::string& type,
		bool ok,
		const std::string& message,
		const std::string& err,
		int http_code);
	bool can_execute_logger_cmd(std::string& err) const;
	bool validate_rest_endpoint(std::string& err) const;
	void mark_rest_result(const LoggerResult& result, const std::string& context_reason);
	void update_link_health(const std::string& reason);
	void refresh_rest_health(time_point now);
	void set_udp_link_alive(bool alive, const std::string& reason);
	void update_presence_state(time_point now, const std::string& reason_override = "");
	LoggerResult request_logger_state(const std::string& context_reason);
	std::string generate_session_name() const;
	std::string extract_session_name(const Json::Value& cmd) const;
	void set_state(ServiceState state);
	void set_error(const std::string& message);
	std::string serialize_json(const Json::Value& value) const;
	std::int64_t unix_ts() const;
	int wait_for_token_rc(const mqtt::token_ptr& tok) const;

private:
	static constexpr int kRxTimeoutStrikesToReconnect = 5;

	EkinoxConfig config_;
	EkinoxTopics topics_;
	std::string server_uri_;
	std::string client_id_;
	mqtt::async_client client_;
	mqtt::connect_options conn_opts_;

	StatusPayload status_{};
	PresencePayload presence_{};

	struct CommandMessage {
		std::string topic;
		std::string payload;
	};
	std::queue<CommandMessage> pending_commands_;
	std::mutex commands_mutex_;

	std::optional<std::chrono::steady_clock::time_point> next_reconnect_attempt_;
	std::optional<std::chrono::steady_clock::time_point> next_sensor_attempt_;

	bool connected_ = false;
	bool presence_online_ = false;
	bool sensor_connected_ = false;
	int rx_timeout_strikes_ = 0;
	bool udp_link_alive_ = false;
	bool rest_alive_ = false;
	time_point last_rest_success_{};
	time_point last_rest_ok_{};
	std::string last_rest_error_;
	std::int64_t heartbeat_seq_ = 0;
	time_point start_tp_{};
	time_point next_status_pub_{};
	time_point next_heartbeat_pub_{};
	int current_backoff_ms_ = 0;
	bool reported_no_sbg_ = false;
	EkinoxUdpSession::Config udp_config_{};
	std::int64_t last_presence_ts_ = 0;
	std::chrono::milliseconds presence_timeout_{5000};
	std::chrono::milliseconds rest_alive_ttl_{5000};
	std::unique_ptr<EkinoxUdpSession> udp_session_;
};

}  // namespace ironsoft::ekinox
