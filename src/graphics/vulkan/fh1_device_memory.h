#pragma once
// FH1 (#268): device memory placement on integrated GPUs and allocation failure reports for
// the Vulkan backend. The policy is in fh1_memory_placement.h; this is its state for the
// one Vulkan device the game uses: the memory layout, the device-local use and budget
// (VK_EXT_memory_budget, refreshed at frame ends), what was moved to system memory, and
// which failures were reported. Discrete GPUs only get the reports (and the heap line at
// start); their memory goes where it always went.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <rex/ui/vulkan/device.h>

#include "fh1_memory_placement.h"

namespace rex::graphics::vulkan::fh1_device_memory {

namespace placement = fh1_memory_placement;

// What an allocation is for: textures move first, the others only when device-local memory
// is (nearly) full.
enum class Use : uint32_t { kTexture, kRenderTarget, kSharedMemory, kScaledResolve, kCount };

const char* UseName(Use use);
inline placement::Kind KindOf(Use use) {
  return use == Use::kTexture ? placement::Kind::kTexture : placement::Kind::kHot;
}

class State {
 public:
  // At the command processor's setup: reads the memory layout and logs it.
  void Initialize(const ui::vulkan::VulkanDevice* device);
  void Shutdown();

  // Memory moves to system memory: an integrated GPU (or the test budget cap) and
  // vulkan_integrated_memory_spill.
  bool active() const;
  const placement::Layout& layout() const { return layout_; }

  // The device-local use and budget, all heaps and per heap (the test cap applied), queried
  // now.
  struct Budget {
    uint64_t usage = 0;
    uint64_t budget = 0;
    uint64_t system_usage = 0;  // the system memory heap's
    uint64_t system_budget = 0;
    placement::HeapUse heaps;
  };
  Budget Query();
  // The last query plus what was allocated in device-local memory since.
  Budget Estimate();
  // Queries the budget for the next placements (at frame ends, and after a trim freed memory).
  void Refresh();
  // A device-local heap is at or over its whole budget.
  bool HeapOverBudget(const Budget& b) const { return placement::HeapOverBudget(layout_, b.heaps); }
  // The memory of `use` now in system memory.
  uint64_t SystemBytes(Use use) const { return counters_[size_t(use)].system_bytes; }

  // At every frame end: when active, refreshes the budget and logs a summary now and then.
  void EndFrame();

  // The memory type order for a resource of `size` bytes accepting `allowed` types, each
  // device-local heap checked against its own budget. allocator_blocks: the texture cache's
  // allocator (VMA), which makes a new block when an allocation does not fit its blocks.
  placement::Order Choose(Use use, uint32_t allowed, uint64_t size, bool allocator_blocks);
  // An allocation was made (or freed) in memory type `type`.
  void Allocated(Use use, uint32_t type, uint64_t size);
  void Freed(Use use, uint32_t type, uint64_t size);
  // Allocations whose type the owner does not keep (render targets, sparse pages): Forget
  // before freeing them.
  void Remember(VkDeviceMemory memory, Use use, uint32_t type, uint64_t size);
  void Forget(VkDeviceMemory memory);

  // An allocation failed in every memory type tried: logged in full the first time for each
  // use, counted afterwards (reported with the summary).
  void ReportFailure(Use use, const char* what, uint64_t size, uint32_t allowed, int32_t result);

  bool IsDeviceLocalType(uint32_t type) const {
    return type < 32 && (layout_.device_local_types >> type & 1);
  }

 private:
  void LogSummary();

  const ui::vulkan::VulkanDevice* device_ = nullptr;
  placement::Layout layout_;
  uint32_t type_heap_[placement::kMaxTypes] = {};
  bool have_budget_ = false;
  uint32_t cap_mb_ = 0;  // vulkan_device_local_budget_mb

  std::mutex mutex_;
  Budget last_;
  // Allocated in each device-local heap since the last query.
  std::atomic<uint64_t> since_refresh_[placement::kMaxHeaps];
  std::atomic<bool> summary_due_{false};

  struct Counter {
    std::atomic<uint64_t> device_local_bytes{0};
    std::atomic<uint64_t> system_bytes{0};
    std::atomic<uint32_t> system_count{0};
    std::atomic<uint32_t> moved{0};  // allocations placed in system memory, ever
    std::atomic<uint32_t> failures{0};
    uint32_t failures_reported = 0;
    bool move_logged = false;
  };
  Counter counters_[size_t(Use::kCount)];

  std::mutex remembered_mutex_;
  struct Remembered {
    Use use;
    uint32_t type;
    uint64_t size;
  };
  std::unordered_map<VkDeviceMemory, Remembered> remembered_;
};

State& Get();

// A dedicated image allocation in the order Choose gives (render targets): as
// ui::vulkan::util::CreateDedicatedAllocationImage, which it calls when !active().
bool CreateDedicatedImage(const ui::vulkan::VulkanDevice* device, const VkImageCreateInfo& info,
                          Use use, VkImage& image_out, VkDeviceMemory& memory_out,
                          VkDeviceSize* size_out);
// Reports a failed image allocation (ReportFailure) with the image's size and memory types.
void ReportImageFailure(const ui::vulkan::VulkanDevice* device, Use use,
                        const VkImageCreateInfo& info, int32_t result);
// A memory allocation for a sparse buffer that accepts `allowed` types, `default_type` when
// !active(). Writes the type used.
VkResult AllocateSparseMemory(const ui::vulkan::VulkanDevice* device, Use use, uint32_t allowed,
                              uint32_t default_type, VkDeviceSize size, VkDeviceMemory& memory_out,
                              uint32_t& type_out);

}  // namespace rex::graphics::vulkan::fh1_device_memory
