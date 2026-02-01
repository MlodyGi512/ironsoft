#include "ironsoft/ekinox/ekinox_logger_api.h"

#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)
#include "sbgEComCmdApi.h"
#endif

namespace ironsoft::ekinox {

namespace {
#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)
LoggerResult make_result(SbgErrorCode code) {
	LoggerResult result{};
	result.error_code = static_cast<int>(code);
	const char* text = sbgErrorCodeToString(code);
	result.error_string = text ? text : "";
	result.ok = (code == SBG_NO_ERROR);
	return result;
}
#endif

LoggerResult make_handle_error() {
	LoggerResult result{};
	result.ok = false;
	result.error_code = -1;
	result.error_string = "SDK disabled or session closed";
	return result;
}
}  // namespace

LoggerResult EkinoxLoggerApi::start(SbgEComHandle* handle) {
	if (handle == nullptr) {
		return make_handle_error();
	}
#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)
	return make_result(sbgEComCmdApiStartLogger(handle));
#else
	(void)handle;
	return make_handle_error();
#endif
}

LoggerResult EkinoxLoggerApi::stop(SbgEComHandle* handle) {
	if (handle == nullptr) {
		return make_handle_error();
	}
#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)
	return make_result(sbgEComCmdApiStopLogger(handle));
#else
	(void)handle;
	return make_handle_error();
#endif
}

LoggerResult EkinoxLoggerApi::status(SbgEComHandle* handle, std::uint32_t& out_status) {
	out_status = 0;
	if (handle == nullptr) {
		return make_handle_error();
	}
#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)
	SbgErrorCode code = sbgEComCmdApiGetLoggerStatus(handle, &out_status);
	return make_result(code);
#else
	(void)handle;
	return make_handle_error();
#endif
}

}  // namespace ironsoft::ekinox
