#include "ironsoft/ekinox/ekinox_state.h"

#include <json/json.h>

namespace ironsoft::ekinox {

Json::Value build_presence_json(const PresencePayload& payload) {
	Json::Value root;
	root["state"] = payload.online ? "online" : "offline";
	root["ts"] = static_cast<Json::Int64>(payload.timestamp);
	root["reason"] = payload.reason;
	return root;
}

Json::Value build_status_json(const StatusPayload& payload) {
	Json::Value root;
	root["link_alive"] = payload.link_alive;
	root["api_ok"] = payload.api_ok;
	root["recording_active"] = payload.recording_active;
	root["recording"] = payload.recording_active;
	root["state"] = std::string{to_string(payload.state)};
	root["mode"] = root["state"];
	root["last_error"] = payload.last_error;
	root["last_error_ts"] = static_cast<Json::Int64>(payload.last_error_ts);
	return root;
}

Json::Value build_heartbeat_json(std::int64_t seq, std::int64_t uptime_s) {
	Json::Value root;
	root["seq"] = static_cast<Json::Int64>(seq);
	root["uptime_s"] = static_cast<Json::Int64>(uptime_s);
	return root;
}

Json::Value build_ack_json(const std::string& id,
	const std::string& type,
	bool ok,
	const std::string& message,
	const std::string& error,
	int http_code,
	std::int64_t ts) {
	Json::Value root;
	root["id"] = id;
	root["type"] = type;
	root["ok"] = ok;
	root["message"] = message;
	root["err"] = error;
	root["http_code"] = http_code;
	root["ts"] = static_cast<Json::Int64>(ts);
	return root;
}

}  // namespace ironsoft::ekinox
