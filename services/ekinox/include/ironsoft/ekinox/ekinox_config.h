#pragma once

#include <string>

#include <jsoncpp/json/json.h>


namespace ironsoft::ekinox {

struct MqttCfg {
	std::string host;
	int port = 1883;
	std::string drone_id;
	std::string username;
	std::string password;
	bool tls_enabled = false;
	std::string ca_file;
};

struct EkinoxNetCfg {
	std::string ip;
	int udp_port = 0;
	int udp_local_port = 0;
	int rest_port = 0;
};

struct EkinoxTimeouts {
	int rx_dead_ms = 1500;
	int start_timeout_ms = 10000;
	int stop_timeout_ms = 10000;
	int reconnect_backoff_ms = 250;
	int reconnect_backoff_max_ms = 2000;
};

struct EkinoxConfig {
	MqttCfg mqtt;
	EkinoxNetCfg ekinox;
	EkinoxTimeouts timeouts;
};

bool load_json_file(const std::string& path, Json::Value& out, std::string& err);
bool loadEkinoxConfig(const std::string& path, EkinoxConfig& cfg, std::string& err);

}  // namespace ironsoft::ekinox
