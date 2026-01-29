#pragma once
#include <chrono>
#include <cstdint>

namespace ironsoft {

inline std::int64_t now_unix_s() {
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

inline std::int64_t now_steady_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace ironsoft
