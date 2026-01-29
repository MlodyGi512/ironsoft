#pragma once
#include <atomic>
#include <string>

namespace ironsoft {

struct BackendState {
  std::atomic<bool> running{true};
  std::atomic<int> heartbeat_seq{0};

  std::atomic<bool> connected{false};
  std::string mode = "IDLE";
  std::string last_error;
};

} // namespace ironsoft
