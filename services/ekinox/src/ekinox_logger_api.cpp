#include "ironsoft/ekinox/ekinox_logger_api.h"
#include "ironsoft/ekinox/ekinox_config.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include <json/json.h>

#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)
#include "sbgEComCmdApi.h"
#endif

namespace ironsoft::ekinox {

namespace {
#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)

static std::string log_rest_reply(const char* verb,
	const std::string& path,
	const SbgEComCmdApiReply& reply) {
	const std::string body = (reply.pContent != nullptr) ? std::string(reply.pContent) : std::string();
	std::string preview = body;
	if (preview.size() > 512) {
		preview.resize(512);
		preview += "...";
	}

	std::ostringstream oss;
	oss << verb << ' ' << path << " -> HTTP " << reply.statusCode << " body=\"" << preview << "\"";
	const std::string line = oss.str();
	std::cout << "[ekinox][rest] " << line << std::endl;
	return preview;
}

static LoggerResult make_http_result(std::uint16_t status_code,
	const std::string& preview_or_msg) {
	LoggerResult r{};
	r.ok = (status_code == 200);
	if (r.ok) {
		r.error_code = 0;
		r.error_string.clear();
	} else {
		r.error_code = static_cast<int>(status_code);
		if (!preview_or_msg.empty()) {
			r.error_string = preview_or_msg;
		} else {
			r.error_string = "http_" + std::to_string(status_code);
		}
	}
	return r;
}

#endif  // DEKINOX_HAS_SBG
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

#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)

LoggerResult make_config_error(const char* field) {
	LoggerResult result{};
	result.ok = false;
	result.error_code = -2;
	result.error_string = std::string{"missing REST endpoint: "} + field;
	return result;
}

LoggerResult make_rest_error(std::uint16_t status_code, const char* content) {
	LoggerResult result{};
	result.ok = false;
	result.error_code = static_cast<int>(status_code);
	if (content && *content != '\0') {
		result.error_string = content;
	} else {
		result.error_string = "REST API error";
	}
	return result;
}

LoggerResult make_success() {
	LoggerResult result{};
	result.ok = true;
	result.error_code = 0;
	return result;
}

struct ReplyGuard {
	SbgEComCmdApiReply reply;
	ReplyGuard() {
		sbgEComCmdApiReplyConstruct(&reply);
	}
	~ReplyGuard() {
		sbgEComCmdApiReplyDestroy(&reply);
	}
};

bool is_rest_success(std::uint16_t status_code) {
	return status_code == 0 || (status_code >= 200 && status_code < 300);
}

LoggerResult parse_status_payload(const char* payload, std::uint32_t& out_status) {
	if (payload == nullptr || *payload == '\0') {
		LoggerResult result{};
		result.ok = false;
		result.error_code = -3;
		result.error_string = "empty REST status payload";
		return result;
	}
	Json::Value root;
	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;
	std::string errs;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	const char* begin = payload;
	const char* end = payload + std::strlen(payload);
	if (!reader->parse(begin, end, &root, &errs)) {
		LoggerResult result{};
		result.ok = false;
		result.error_code = -4;
		result.error_string = "invalid REST status json: " + errs;
		return result;
	}
	if (!root.isObject()) {
		LoggerResult result{};
		result.ok = false;
		result.error_code = -5;
		result.error_string = "REST status payload must be object";
		return result;
	}
	const Json::Value& status_value = root["status"];
	if (status_value.isUInt64()) {
		out_status = static_cast<std::uint32_t>(status_value.asUInt64());
		return make_success();
	}
	if (status_value.isInt()) {
		out_status = static_cast<std::uint32_t>(status_value.asInt());
		return make_success();
	}
	if (const Json::Value& running = root["running"]; running.isBool()) {
		out_status = running.asBool() ? 1u : 0u;
		return make_success();
	}
	LoggerResult result{};
	result.ok = false;
	result.error_code = -6;
	result.error_string = "REST status missing 'status' field";
	return result;
}

#endif  // DEKINOX_HAS_SBG
}  // namespace

LoggerResult EkinoxLoggerApi::start(SbgEComHandle* handle, const RestApiConfig& api) {
	if (handle == nullptr) {
		return make_handle_error();
	}
#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)
	if (api.start_path.empty()) {
		return make_config_error("start_path");
	}
	ReplyGuard reply;
	const SbgErrorCode code = sbgEComCmdApiPost(handle, api.start_path.c_str(), nullptr, nullptr, &reply.reply);
	if (code != SBG_NO_ERROR) {
		return make_result(code);
	}
	const std::string preview = log_rest_reply("POST", api.start_path, reply.reply);
	return make_http_result(reply.reply.statusCode, preview);
#else
	(void)api;
	(void)handle;
	return make_handle_error();
#endif
}

LoggerResult EkinoxLoggerApi::stop(SbgEComHandle* handle, const RestApiConfig& api) {
	if (handle == nullptr) {
		return make_handle_error();
	}
#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)
	if (api.stop_path.empty()) {
		return make_config_error("stop_path");
	}
	ReplyGuard reply;
	const SbgErrorCode code = sbgEComCmdApiPost(handle, api.stop_path.c_str(), nullptr, nullptr, &reply.reply);
	if (code != SBG_NO_ERROR) {
		return make_result(code);
	}
	const std::string preview = log_rest_reply("POST", api.stop_path, reply.reply);
	return make_http_result(reply.reply.statusCode, preview);
#else
	(void)api;
	(void)handle;
	return make_handle_error();
#endif
}

LoggerResult EkinoxLoggerApi::status(SbgEComHandle* handle, std::uint32_t& out_status, const RestApiConfig& api) {
	out_status = 0;
	if (handle == nullptr) {
		return make_handle_error();
	}
#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)
	if (api.status_path.empty()) {
		return make_config_error("status_path");
	}
	ReplyGuard reply;
	const SbgErrorCode code = sbgEComCmdApiGet(handle, api.status_path.c_str(), nullptr, &reply.reply);
	if (code != SBG_NO_ERROR) {
		return make_result(code);
	}
	const std::string preview = log_rest_reply("GET", api.status_path, reply.reply);
	LoggerResult http_result = make_http_result(reply.reply.statusCode, preview);
	if (!http_result.ok) {
		return http_result;
	}
	return parse_status_payload(reply.reply.pContent, out_status);
#else
	(void)api;
	(void)handle;
	return make_handle_error();
#endif
}

}  // namespace ironsoft::ekinox
