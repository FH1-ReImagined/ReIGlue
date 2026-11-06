#pragma once
// FH1 (#551): what the backend knows about a lost device (the Vulkan breadcrumbs), logged by
// GraphicsSystem::OnHostGpuLossFromAnyThread before the game stops, whichever thread saw the
// loss first (the presenter, the submission thread or a fence wait).
#include <functional>
#include <mutex>
#include <utility>

namespace rex::graphics::fh1_device_lost_report {

struct State {
  std::mutex mutex;
  std::function<void()> reporter;
};

inline State& Get() {
  static State state;
  return state;
}

// The backend's report (an empty function removes it, at its shutdown).
inline void Set(std::function<void()> reporter) {
  State& s = Get();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.reporter = std::move(reporter);
}

inline void Run() {
  State& s = Get();
  std::function<void()> reporter;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    reporter = s.reporter;
  }
  if (reporter) reporter();
}

}  // namespace rex::graphics::fh1_device_lost_report
