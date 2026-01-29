#pragma once
#include <string>

namespace ironsoft {

struct MqttConfig {
  std::string host;
  int port = 1883;

  std::string username;
  std::string password;

  bool tls_enabled = false;
  std::string ca_file;

  std::string drone_id = "drone01";
  int keepalive_s = 30;
};

} // namespace ironsoft
