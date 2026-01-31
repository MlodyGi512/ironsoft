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
	void publish_ack(const std::string& payload);
	void drain_commands();
	void handle_command_message(const std::string& payload);
	void handle_start_logger(const std::string& id);
	void handle_stop_logger(const std::string& id);
	void handle_ping(const std::string& id);
	void schedule_transition(ServiceState target_state, bool recording_active);
	void apply_pending_transition(time_point now);
	void ensure_sensor_session(time_point now);
	void poll_sensor(time_point now);
	void handle_sensor_error(const std::string& reason);
	void handle_sensor_disconnect(const std::string& reason);
	void set_state(ServiceState state);
	void set_error(const std::string& message);
	std::string serialize_json(const Json::Value& value) const;
	std::int64_t unix_ts() const;

private:
	EkinoxConfig config_;
	EkinoxTopics topics_;
	std::string server_uri_;
	std::string client_id_;
	mqtt::async_client client_;
	mqtt::connect_options conn_opts_;

	StatusPayload status_{};
	PresencePayload presence_{};

	std::queue<std::string> pending_commands_;
	std::mutex commands_mutex_;

	std::optional<std::chrono::steady_clock::time_point> next_reconnect_attempt_;
	std::optional<std::chrono::steady_clock::time_point> next_sensor_attempt_;
	struct PendingTransition {
		ServiceState target = ServiceState::kIdle;
		bool recording_active = false;
		time_point due{};
	};
	std::optional<PendingTransition> pending_transition_;

	bool connected_ = false;
	bool presence_online_ = false;
	bool sensor_connected_ = false;
	std::int64_t heartbeat_seq_ = 0;
	time_point start_tp_{};
	time_point next_status_pub_{};
	time_point next_heartbeat_pub_{};
	int current_backoff_ms_ = 0;
	bool reported_no_sbg_ = false;
	EkinoxUdpSession::Config udp_config_{};
	std::unique_ptr<EkinoxUdpSession> udp_session_;
};

}  // namespace ironsoft::ekinox
