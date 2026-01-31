#pragma once

#include <string>

namespace ironsoft::ekinox {

struct EkinoxTopics {
	std::string prefix;
	std::string presence;
	std::string status;
	std::string heartbeat;
	std::string cmd;
	std::string ack;
};

inline EkinoxTopics make_topics(const std::string& drone_id) {
	EkinoxTopics t;
	t.prefix = "ironsoft/uav/" + drone_id + "/ekinox";
	t.presence = t.prefix + "/presence";
	t.status = t.prefix + "/status";
	t.heartbeat = t.prefix + "/heartbeat";
	t.cmd = t.prefix + "/cmd";
	t.ack = t.prefix + "/ack";
	return t;
}

}  // namespace ironsoft::ekinox
