#pragma once

#include <string>

namespace ironsoft::ekinox {

struct EkinoxTopics {
	std::string root;
	std::string prefix;
	std::string presence;
	std::string status;
	std::string heartbeat;
	std::string cmd;
	std::string ack;
	std::string cmd_legacy;
	std::string ack_legacy;
};

inline EkinoxTopics make_topics(const std::string& drone_id) {
	const std::string root = "ironsoft/uav/" + drone_id;
	EkinoxTopics t;
	t.root = root;
	t.prefix = root + "/ekinox";
	t.presence = t.prefix + "/presence";
	t.status = t.prefix + "/status";
	t.heartbeat = t.prefix + "/heartbeat";
	t.cmd = root + "/cmd";
	t.ack = root + "/ack";
	t.cmd_legacy = t.prefix + "/cmd";
	t.ack_legacy = t.prefix + "/ack";
	return t;
}

}  // namespace ironsoft::ekinox
