#pragma once

#include <cstdint>
#include <string>

#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)
#include "sbgECom.h"
#include "sbgErrorCodes.h"
#else
struct SbgEComHandle;
#endif

namespace ironsoft::ekinox {

struct RestApiConfig;

struct LoggerResult {
	bool ok = false;
	int error_code = 0;
	std::string error_string;
};

class EkinoxLoggerApi {
public:
	static LoggerResult start(SbgEComHandle* handle, const RestApiConfig& api);
	static LoggerResult stop(SbgEComHandle* handle, const RestApiConfig& api);
	static LoggerResult status(SbgEComHandle* handle, std::uint32_t& out_status, const RestApiConfig& api);
};

}  // namespace ironsoft::ekinox
