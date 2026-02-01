#pragma once

#include <cstdint>
#include <string>

#include "ironsoft/ekinox/ekinox_config.h"

namespace ironsoft::ekinox {

struct LoggerResult {
	bool ok = false;
	int error_code = 0;
	long http_code = 0;
	std::string message;
	std::string error_string;
	std::string body;
};

class EkinoxLoggerApi {
public:
	static LoggerResult start_http(const EkinoxConfig& config, const RestApiConfig& api);
	static LoggerResult stop_http(const EkinoxConfig& config, const RestApiConfig& api);
	static LoggerResult status_http(
		const EkinoxConfig& config,
		const RestApiConfig& api,
		std::uint32_t& out_status,
		bool& out_recording_active);
};

}  // namespace ironsoft::ekinox
