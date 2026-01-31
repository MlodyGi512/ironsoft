#pragma once

#include <cstdint>
#include <string>

#include <json/json.h>

#include "ironsoft/ekinox/ekinox_types.h"

namespace ironsoft::ekinox {

struct PresencePayload {
	bool online = false;
	std::string reason;
	std::int64_t timestamp = 0;
};

struct StatusPayload {
	bool link_alive = false;
	bool api_ok = false;
	bool recording_active = false;
	ServiceState state = ServiceState::kDisconnected;
	std::string last_error;
	std::int64_t last_error_ts = 0;
};

Json::Value build_presence_json(const PresencePayload& payload);
Json::Value build_status_json(const StatusPayload& payload);
Json::Value build_heartbeat_json(std::int64_t seq, std::int64_t uptime_s);
Json::Value build_ack_json(const std::string& id, bool ok, const std::string& message, const std::string& error);

}  // namespace ironsoft::ekinox
