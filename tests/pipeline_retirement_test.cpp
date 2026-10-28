#ifdef NDEBUG
#undef NDEBUG
#endif
#include <rex/graphics/vulkan/pipeline_retirement_queue.h>

#include <algorithm>
#include <cassert>
#include <thread>
#include <vector>

using rex::graphics::vulkan::PipelineRetirementQueue;

int main() {
  PipelineRetirementQueue<int> queue;
  // Creation began in submission 1 but the placeholder was still used in 4.
  // A worker finishing after a timeout must not retire it against submission 1.
  std::thread worker([&] { queue.Retire(10); });
  worker.join();
  assert(queue.Collect(4, 3).empty());
  assert(queue.Collect(5, 3).empty());
  assert((queue.Collect(5, 4) == std::vector<int>{10}));
  assert(queue.Collect(5, 4).empty());

  // Multiple replacements, including one published after the last drain.
  queue.Retire(20);
  queue.Retire(21);
  assert(queue.Collect(7, 6).empty());
  queue.Retire(22);
  assert((queue.Collect(8, 7) == std::vector<int>{20, 21}));
  assert((queue.Collect(9, 8) == std::vector<int>{22}));

  // No later frame: shutdown joins workers, then force-drains both collections.
  queue.Retire(30);
  assert(queue.Collect(10, 9).empty());
  std::thread last_worker([&] { queue.Retire(31); });
  last_worker.join();
  assert((queue.Collect(10, 9, true) == std::vector<int>{30, 31}));
  assert(queue.Collect(10, 9, true).empty());

  // Concurrent publishers and repeated CP drains retain every handle exactly once.
  std::thread a([&] { for (int i = 100; i < 1100; ++i) queue.Retire(i); });
  std::thread b([&] { for (int i = 1100; i < 2100; ++i) queue.Retire(i); });
  std::vector<int> destroyed;
  for (uint64_t submission = 11; submission < 100; ++submission) {
    auto ready = queue.Collect(submission, submission - 1);
    destroyed.insert(destroyed.end(), ready.begin(), ready.end());
  }
  a.join(); b.join();
  auto tail = queue.Collect(100, 99, true);
  destroyed.insert(destroyed.end(), tail.begin(), tail.end());
  std::sort(destroyed.begin(), destroyed.end());
  assert(destroyed.size() == 2000);
  for (int i = 0; i < 2000; ++i) assert(destroyed[i] == i + 100);
}
