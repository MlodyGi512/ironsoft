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
	const std::string root = "ironsoft/uav/" + drone_id;
	EkinoxTopics t;
	t.prefix = root + "/ekinox";
	t.presence = t.prefix + "/presence";
	t.status = t.prefix + "/status";
	t.heartbeat = t.prefix + "/heartbeat";
	t.cmd = root + "/cmd";
	t.ack = root + "/ack";
	return t;
}

}  // namespace ironsoft::ekinox
