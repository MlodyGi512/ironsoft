#include <atomic>
#include <csignal>
#include <iostream>
#include <string>

#include "ironsoft/ekinox/ekinox_config.h"
#include "ironsoft/ekinox/ekinox_service.h"
#include "ironsoft/ekinox/sbg_smoke.h"

namespace {
std::atomic_bool g_stop{false};

void signal_handler(int) {
	g_stop.store(true);
}

void usage() {
	std::cout << "Usage: ekinox_service --config <path>" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
	std::string config_path;
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg == "--config" && i + 1 < argc) {
			config_path = argv[++i];
		}
	}

	if (config_path.empty()) {
		usage();
		return 1;
	}

	ironsoft::ekinox::EkinoxConfig config;
	std::string err;
	if (!ironsoft::ekinox::loadEkinoxConfig(config_path, config, err)) {
		std::cerr << "Failed to load config: " << err << std::endl;
		return 2;
	}

	std::signal(SIGINT, signal_handler);
	std::signal(SIGTERM, signal_handler);

	ironsoft::ekinox::sbg_smoke_version();

	ironsoft::ekinox::EkinoxService service(config);
	std::cout << "[ekinox] starting service, press Ctrl+C to stop" << std::endl;
	service.run(g_stop);
	std::cout << "[ekinox] stopped" << std::endl;
	return 0;
}
