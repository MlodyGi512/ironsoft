#include "ironsoft/ekinox/ekinox_config.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {
constexpr const char* kUsage = "Usage: ekinox_config_dump --config <path>";
}

int main(int argc, char** argv) {
	std::string config_path;
	for (int i = 1; i < argc; ++i) {
		const std::string_view arg(argv[i]);
		if (arg == "--config" && i + 1 < argc) {
			config_path = argv[++i];
			continue;
		}
		std::cerr << kUsage << '\n';
		return 1;
	}

	if (config_path.empty()) {
		std::cerr << kUsage << '\n';
		return 1;
	}

	ironsoft::ekinox::EkinoxConfig config;
	std::string error;
	if (!ironsoft::ekinox::loadEkinoxConfig(config_path, config, error)) {
		std::cerr << "failed to load config: " << error << '\n';
		return 1;
	}

	std::cout << "MQTT: " << config.mqtt.host << ':' << config.mqtt.port << " drone_id=" << config.mqtt.drone_id << '\n';
	std::cout << "  auth user='" << config.mqtt.username << "' tls=" << (config.mqtt.tls_enabled ? "on" : "off") << '\n';
	std::cout << "Ekinox IP: " << config.ekinox.ip << " udp=" << config.ekinox.udp_port << " rest=" << config.ekinox.rest_port << '\n';
	std::cout << "REST API: base='" << config.rest_api.base_path << "' datalogger='" << config.rest_api.datalogger_path << "'" << '\n';
	std::cout << "Timeouts: rx_dead=" << config.timeouts.rx_dead_ms << "ms start=" << config.timeouts.start_timeout_ms
	          << "ms stop=" << config.timeouts.stop_timeout_ms << "ms" << '\n';
	std::cout << "  udp_rx_timeout=" << config.timeouts.udp_rx_timeout_ms << "ms"
		<< " udp_rx_fail_threshold=" << config.timeouts.udp_rx_fail_threshold
		<< " udp_rx_ok_to_clear_threshold=" << config.timeouts.udp_rx_ok_to_clear_threshold << '\n';
	std::cout << "  reconnect_backoff=" << config.timeouts.reconnect_backoff_ms << "ms max=" << config.timeouts.reconnect_backoff_max_ms << "ms" << '\n';

	return 0;
}
