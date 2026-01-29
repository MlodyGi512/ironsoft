#pragma once
#include <string>

namespace ironsoft {

struct Topics {
  std::string prefix;    // ironsoft/uav/<drone_id>
  std::string presence;  // .../presence
  std::string status;    // .../status
  std::string heartbeat; // .../heartbeat
  std::string cmd;       // .../cmd
  std::string ack;       // .../ack
};

inline Topics make_topics(const std::string& drone_id) {
  Topics t;
  t.prefix    = "ironsoft/uav/" + drone_id;
  t.presence  = t.prefix + "/presence";
  t.status    = t.prefix + "/status";
  t.heartbeat = t.prefix + "/heartbeat";
  t.cmd       = t.prefix + "/cmd";
  t.ack       = t.prefix + "/ack";
  return t;
}

} // namespace ironsoft
