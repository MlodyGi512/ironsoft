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
	bool has_recording_flag = false;
	bool recording_active = false;
	std::string session_name;
	std::string status_value;
	std::string mode;
	bool has_write_speed = false;
	int write_speed = 0;
};

class EkinoxLoggerApi {
public:
	static std::string buildBaseUrl(const EkinoxConfig& config, const RestApiConfig& api);
	static LoggerResult dataLoggerGet(const EkinoxConfig& config, const RestApiConfig& api);
	static LoggerResult dataLoggerStart(const EkinoxConfig& config, const RestApiConfig& api, const std::string& session_name);
	static LoggerResult dataLoggerStop(const EkinoxConfig& config, const RestApiConfig& api);
	static LoggerResult get_datalogger_info(const EkinoxConfig& config, const RestApiConfig& api);
	static LoggerResult get_datalogger_status(const EkinoxConfig& config, const RestApiConfig& api);
	static LoggerResult start_datalogger(const EkinoxConfig& config, const RestApiConfig& api, const std::string& session_name);
	static LoggerResult stop_datalogger(const EkinoxConfig& config, const RestApiConfig& api);
};

}  // namespace ironsoft::ekinox
