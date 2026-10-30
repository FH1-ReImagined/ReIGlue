#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace rex::graphics::vulkan {

// Creation workers publish replaced handles without reading command-processor
// submission state. Collect runs only on the command processor, after recording
// draws, and assigns a fence at least as late as every use of those handles.
template <typename Handle>
class PipelineRetirementQueue {
 public:
  void Retire(Handle handle) {
    std::lock_guard lock(pending_lock_);
    pending_.push_back(handle);
  }

  std::vector<Handle> Collect(uint64_t current_submission, uint64_t completed_submission,
                              bool force_all = false) {
    {
      std::lock_guard lock(pending_lock_);
      for (Handle handle : pending_) {
        scheduled_.emplace_back(current_submission, handle);
      }
      pending_.clear();
    }
    std::vector<Handle> ready;
    while (!scheduled_.empty() &&
           (force_all || scheduled_.front().first <= completed_submission)) {
      ready.push_back(scheduled_.front().second);
      scheduled_.pop_front();
    }
    return ready;
  }

 private:
  std::mutex pending_lock_;
  std::vector<Handle> pending_;
  // Command-processor thread only, ordered by its monotonically increasing index.
  std::deque<std::pair<uint64_t, Handle>> scheduled_;
};

}  // namespace rex::graphics::vulkan
