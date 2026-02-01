#include "ironsoft/ekinox/ekinox_logger_api.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include <curl/curl.h>
#include <json/json.h>

namespace ironsoft::ekinox {

namespace {

constexpr std::size_t kMaxLoggedBody = 512;
constexpr long kConnectTimeoutSec = 2;
constexpr long kRequestTimeoutSec = 3;

std::once_flag g_curl_init_once;

struct RestPaths {
	std::string base;
	std::string start;
	std::string stop;
};

void ensure_curl_global_init() {
	std::call_once(g_curl_init_once, [] {
		const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
		if (rc != CURLE_OK) {
			std::cerr << "[ekinox][http] curl_global_init failed: " << curl_easy_strerror(rc) << '\n';
		}
	});
}

std::string make_preview(const std::string& body) {
	if (body.size() <= kMaxLoggedBody) {
		return body;
	}
	return body.substr(0, kMaxLoggedBody) + "...";
}

void log_http_call(const std::string& method, const std::string& url, long http_code, const std::string& preview) {
	std::cout << "[ekinox][http] " << method << ' ' << url
		<< " -> HTTP " << http_code
		<< " body=\"" << (preview.empty() ? std::string{"-"} : preview) << "\""
		<< '\n';
}

size_t curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
	const size_t real_size = size * nmemb;
	auto* buffer = static_cast<std::string*>(userp);
	buffer->append(static_cast<const char*>(contents), real_size);
	return real_size;
}

LoggerResult make_config_error(const std::string& field) {
	LoggerResult result{};
	result.ok = false;
	result.error_code = -2;
	result.error_string = "missing " + field;
	return result;
}

bool validate_rest_target(const EkinoxConfig& config, LoggerResult& out_error) {
	if (config.ekinox.ip.empty()) {
		out_error = make_config_error("ekinox.ip");
		return false;
	}
	if (config.ekinox.rest_port <= 0) {
		out_error = make_config_error("ekinox.rest_port");
		return false;
	}
	return true;
}

std::string ensure_leading_slash(const std::string& path) {
	if (path.empty()) {
		return std::string{"/"};
	}
	return (path.front() == '/') ? path : std::string{"/"} + path;
}

std::string trim_trailing_slash(std::string path) {
	while (path.size() > 1 && path.back() == '/') {
		path.pop_back();
	}
	return path;
}

std::string join_paths(const std::string& lhs, const std::string& rhs) {
	std::string base = trim_trailing_slash(ensure_leading_slash(lhs));
	std::string tail = rhs;
	while (!tail.empty() && tail.front() == '/') {
		tail.erase(tail.begin());
	}
	if (tail.empty()) {
		return base;
	}
	return base + '/' + tail;
}

RestPaths build_paths(const RestApiConfig& api) {
	RestPaths paths;
	paths.base = join_paths(api.base_path, api.datalogger_path);
	paths.start = join_paths(paths.base, "start");
	paths.stop = join_paths(paths.base, "stop");
	return paths;
}

std::string build_url(const EkinoxConfig& config, const std::string& path) {
	std::ostringstream oss;
	oss << "http://" << config.ekinox.ip << ':' << config.ekinox.rest_port;
	oss << ensure_leading_slash(path);
	return oss.str();
}

std::string build_datalogger_base_url(const EkinoxConfig& config, const RestApiConfig& api) {
	const auto paths = build_paths(api);
	return build_url(config, paths.base);
}

void enrich_from_json(LoggerResult& result) {
	if (result.body.empty()) {
		return;
	}
	Json::Value root;
	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;
	std::string errs;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	if (!reader->parse(result.body.data(), result.body.data() + result.body.size(), &root, &errs)) {
		return;
	}
	if (!root.isObject()) {
		return;
	}
	bool used_heuristic = false;
	bool recording_via_heuristic = false;
	if (const Json::Value& status = root["status"]; status.isString()) {
		result.status_value = status.asString();
		used_heuristic = true;
		if (result.status_value == "recording") {
			recording_via_heuristic = true;
		}
	}
	if (const Json::Value& mode = root["mode"]; mode.isString()) {
		result.mode = mode.asString();
	}
	if (const Json::Value& session = root["sessionName"]; session.isString()) {
		result.session_name = session.asString();
		if (!result.session_name.empty() && result.status_value != "ready") {
			used_heuristic = true;
			if (result.status_value != "ready") {
				recording_via_heuristic = true;
			}
		}
	}
	if (const Json::Value& write_speed = root["writeSpeed"]; write_speed.isNumeric()) {
		result.has_write_speed = true;
		result.write_speed = write_speed.asInt();
		used_heuristic = true;
		if (result.write_speed > 0) {
			recording_via_heuristic = true;
		}
	}
	if (result.status_value.empty()) {
		if (const Json::Value& state = root["state"]; state.isString()) {
			result.status_value = state.asString();
		}
	}
	if (result.message.empty()) {
		if (!result.status_value.empty()) {
			result.message = result.status_value;
		} else if (!result.mode.empty()) {
			result.message = result.mode;
		}
	}
	auto assign_bool = [&](const Json::Value& value) {
		if (value.isBool()) {
			result.has_recording_flag = true;
			result.recording_active = value.asBool();
			return true;
		}
		return false;
	};
	if (used_heuristic) {
		result.has_recording_flag = true;
		result.recording_active = recording_via_heuristic;
		return;
	}
	if (!assign_bool(root["recording"])) {
		if (!assign_bool(root["isRecording"])) {
			if (!assign_bool(root["recording_active"])) {
				assign_bool(root["active"]);
			}
		}
	}
}

LoggerResult perform_http_request(const std::string& method, const std::string& url, const std::string& payload, bool send_json_body) {
	LoggerResult result{};
	ensure_curl_global_init();
	CURL* handle = curl_easy_init();
	if (!handle) {
		result.ok = false;
		result.error_code = -1;
		result.error_string = "curl_easy_init failed";
		return result;
	}
	std::string response;
	struct curl_slist* headers = nullptr;
	curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &curl_write_cb);
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSec);
	curl_easy_setopt(handle, CURLOPT_TIMEOUT, kRequestTimeoutSec);
	curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(handle, CURLOPT_USERAGENT, "ironsoft-ekinox/1.0");
	if (method == "GET") {
		curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
	} else {
		curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method.c_str());
		if (method == "POST") {
			curl_easy_setopt(handle, CURLOPT_POST, 1L);
		}
		if (!payload.empty()) {
			curl_easy_setopt(handle, CURLOPT_POSTFIELDS, payload.c_str());
			curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
		} else if (method == "POST") {
			curl_easy_setopt(handle, CURLOPT_POSTFIELDS, "");
			curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, 0L);
		}
	}
	headers = curl_slist_append(headers, "Accept: application/json");
	if (method == "POST" && send_json_body) {
		headers = curl_slist_append(headers, "Content-Type: application/json");
	}
	if (headers) {
		curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
	}
	const CURLcode rc = curl_easy_perform(handle);
	long http_code = 0;
	if (rc == CURLE_OK) {
		curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &http_code);
	}
	std::string preview = (rc == CURLE_OK) ? make_preview(response) : std::string{curl_easy_strerror(rc)};
	log_http_call(method, url, http_code, preview);
	if (headers) {
		curl_slist_free_all(headers);
	}
	curl_easy_cleanup(handle);
	if (rc != CURLE_OK) {
		result.ok = false;
		result.error_code = static_cast<int>(rc);
		result.error_string = curl_easy_strerror(rc);
		return result;
	}
	result.body = std::move(response);
	result.http_code = http_code;
	result.message = make_preview(result.body);
	if (http_code >= 200 && http_code < 300) {
		result.ok = true;
		result.error_code = 0;
		result.error_string.clear();
	} else {
		result.ok = false;
		result.error_code = static_cast<int>(http_code);
		if (!result.message.empty()) {
			result.error_string = result.message;
		} else {
			std::ostringstream oss;
			oss << "http_" << http_code;
			result.error_string = oss.str();
		}
	}
	enrich_from_json(result);
	return result;
}

LoggerResult http_get(const EkinoxConfig& config, const std::string& path) {
	return perform_http_request("GET", build_url(config, path), std::string{}, false);
}

LoggerResult http_post(const EkinoxConfig& config, const std::string& path, const std::string& payload, bool send_json_body) {
	return perform_http_request("POST", build_url(config, path), payload, send_json_body);
}

LoggerResult call_with_validation(const EkinoxConfig& config, const std::function<LoggerResult()>& fn) {
	LoggerResult error;
	if (!validate_rest_target(config, error)) {
		return error;
	}
	return fn();
}

}  // namespace

std::string EkinoxLoggerApi::buildBaseUrl(const EkinoxConfig& config, const RestApiConfig& api) {
	LoggerResult error;
	if (!validate_rest_target(config, error)) {
		return {};
	}
	return build_datalogger_base_url(config, api);
}

LoggerResult EkinoxLoggerApi::dataLoggerGet(const EkinoxConfig& config, const RestApiConfig& api) {
	return call_with_validation(config, [&]() {
		const auto paths = build_paths(api);
		return http_get(config, paths.base);
	});
}

LoggerResult EkinoxLoggerApi::dataLoggerStart(const EkinoxConfig& config, const RestApiConfig& api, const std::string& session_name) {
	return call_with_validation(config, [&]() {
		const auto paths = build_paths(api);
		Json::Value payload_json;
		payload_json["sessionName"] = session_name;
		Json::StreamWriterBuilder builder;
		builder["indentation"] = "";
		const std::string payload = Json::writeString(builder, payload_json);
		return http_post(config, paths.start, payload, true);
	});
}

LoggerResult EkinoxLoggerApi::dataLoggerStop(const EkinoxConfig& config, const RestApiConfig& api) {
	return call_with_validation(config, [&]() {
		const auto paths = build_paths(api);
		return http_post(config, paths.stop, "{}", true);
	});
}

LoggerResult EkinoxLoggerApi::get_datalogger_info(const EkinoxConfig& config, const RestApiConfig& api) {
	return dataLoggerGet(config, api);
}

LoggerResult EkinoxLoggerApi::get_datalogger_status(const EkinoxConfig& config, const RestApiConfig& api) {
	return dataLoggerGet(config, api);
}

LoggerResult EkinoxLoggerApi::start_datalogger(const EkinoxConfig& config, const RestApiConfig& api, const std::string& session_name) {
	return dataLoggerStart(config, api, session_name);
}

LoggerResult EkinoxLoggerApi::stop_datalogger(const EkinoxConfig& config, const RestApiConfig& api) {
	return dataLoggerStop(config, api);
}

}  // namespace ironsoft::ekinox
