#include "ironsoft/ekinox/ekinox_logger_api.h"

#include <algorithm>
#include <cstdint>
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

std::once_flag g_curl_init_once;

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

int effective_rest_port(const EkinoxConfig& config) {
	return (config.ekinox.rest_port > 0) ? config.ekinox.rest_port : 80;
}

std::string normalize_path(const std::string& path) {
	if (path.empty()) {
		return std::string{"/"};
	}
	if (path.front() == '/') {
		return path;
	}
	return std::string{"/"} + path;
}

std::string build_url(const EkinoxConfig& config, const std::string& path) {
	std::ostringstream oss;
	oss << "http://" << config.ekinox.ip << ':' << effective_rest_port(config);
	oss << normalize_path(path);
	return oss.str();
}

LoggerResult perform_http_request(const std::string& method, const std::string& url, const std::string& payload) {
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
	curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(handle, CURLOPT_TIMEOUT, 10L);
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
			curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, payload.size());
		} else if (method == "POST") {
			curl_easy_setopt(handle, CURLOPT_POSTFIELDS, "");
			curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, 0L);
		}
	}
	if (method == "POST") {
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
	if (http_code == 200 || http_code == 204) {
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
	return result;
}

LoggerResult parse_status_payload(LoggerResult base_result, std::uint32_t& out_status, bool& out_recording) {
	Json::Value root;
	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;
	std::string errs;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	if (!reader->parse(base_result.body.data(), base_result.body.data() + base_result.body.size(), &root, &errs)) {
		base_result.ok = false;
		base_result.error_code = -10;
		base_result.error_string = "bad_reply: " + errs;
		return base_result;
	}
	if (!root.isObject()) {
		base_result.ok = false;
		base_result.error_code = -11;
		base_result.error_string = "bad_reply: payload not object";
		return base_result;
	}
	bool found_flag = false;
	bool recording = false;
	if (const Json::Value& recording_value = root["recording"]; recording_value.isBool()) {
		recording = recording_value.asBool();
		found_flag = true;
	}
	if (!found_flag) {
		if (const Json::Value& recording_active = root["recording_active"]; recording_active.isBool()) {
			recording = recording_active.asBool();
			found_flag = true;
		}
	}
	if (!found_flag) {
		if (const Json::Value& recording_alt = root["recordingActive"]; recording_alt.isBool()) {
			recording = recording_alt.asBool();
			found_flag = true;
		}
	}
	if (!found_flag) {
		base_result.ok = false;
		base_result.error_code = -12;
		base_result.error_string = "bad_reply: missing recording flag";
		return base_result;
	}
	if (const Json::Value& status_value = root["status"]; status_value.isUInt64()) {
		out_status = static_cast<std::uint32_t>(status_value.asUInt64());
	} else if (const Json::Value& status_signed = root["status"]; status_signed.isInt()) {
		out_status = static_cast<std::uint32_t>(status_signed.asInt());
	}
	out_recording = recording;
	base_result.ok = true;
	base_result.error_code = 0;
	if (base_result.message.empty()) {
		base_result.message = recording ? "recording active" : "recording inactive";
	}
	base_result.error_string.clear();
	return base_result;
}

}  // namespace

LoggerResult EkinoxLoggerApi::start_http(const EkinoxConfig& config, const RestApiConfig& api) {
	if (config.ekinox.ip.empty()) {
		return make_config_error("ekinox.ip");
	}
	if (api.start_path.empty()) {
		return make_config_error("rest_api.start_path");
	}
	const std::string url = build_url(config, api.start_path);
	return perform_http_request("POST", url, std::string{});
}

LoggerResult EkinoxLoggerApi::stop_http(const EkinoxConfig& config, const RestApiConfig& api) {
	if (config.ekinox.ip.empty()) {
		return make_config_error("ekinox.ip");
	}
	if (api.stop_path.empty()) {
		return make_config_error("rest_api.stop_path");
	}
	const std::string url = build_url(config, api.stop_path);
	return perform_http_request("POST", url, std::string{});
}

LoggerResult EkinoxLoggerApi::status_http(
	const EkinoxConfig& config,
	const RestApiConfig& api,
	std::uint32_t& out_status,
	bool& out_recording_active) {
	out_status = 0;
	out_recording_active = false;
	if (config.ekinox.ip.empty()) {
		return make_config_error("ekinox.ip");
	}
	if (api.status_path.empty()) {
		return make_config_error("rest_api.status_path");
	}
	const std::string url = build_url(config, api.status_path);
	LoggerResult result = perform_http_request("GET", url, std::string{});
	if (!result.ok) {
		return result;
	}
	return parse_status_payload(std::move(result), out_status, out_recording_active);
}

}  // namespace ironsoft::ekinox
