#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <mqtt/async_client.h>

#include "ironsoft/mqtt_config.h"
#include "ironsoft/topics.h"
#include "state.h"

namespace ironsoft {

struct CmdHandlerResult {
  bool ok = true;
  std::string message;
  std::string error;
};

using CmdHandler = std::function<CmdHandlerResult(const std::string& cmd_json)>;

class MqttBackendClient {
public:
  MqttBackendClient(MqttConfig cfg, Topics topics, BackendState& st);
  ~MqttBackendClient();

  void start(CmdHandler on_cmd);
  void stop();

private:
  void run_connect_loop();
  void publish_presence_online();
  void publish_status_retained();
  void publish_heartbeat_loop();
  void subscribe_cmd();

  MqttConfig cfg_;
  Topics topics_;
  BackendState& st_;

  std::unique_ptr<mqtt::async_client> client_;
  mqtt::connect_options connopts_;

  std::thread connect_thread_;
  std::thread heartbeat_thread_;
  std::atomic<bool> stop_requested_{false};

  CmdHandler on_cmd_;
};

} // namespace ironsoft
