// FH1 (#268): device memory placement on integrated GPUs and allocation failure reports.
// See fh1_device_memory.h and fh1_memory_placement.h.

#include "fh1_device_memory.h"

#include <algorithm>
#include <chrono>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(vulkan_integrated_memory_spill, true, "GPU/Vulkan",
                    "On integrated GPUs, put new textures in system memory once device-local "
                    "memory use reaches 80% of the driver's budget (VK_EXT_memory_budget), and "
                    "render targets and guest memory pages at 95%, instead of overfilling the "
                    "device-local memory (the BIOS carve-out). No effect on discrete GPUs")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(vulkan_integrated_texture_percent,
                      rex::graphics::vulkan::fh1_memory_placement::kTexturePercent, "GPU/Vulkan",
                      "Integrated GPUs with a small carve-out: new textures go to a device-local "
                      "heap only while its use stays under this percentage of its budget, else to "
                      "system memory")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

REXCVAR_DEFINE_UINT32(vulkan_integrated_device_local_percent,
                      rex::graphics::vulkan::fh1_memory_placement::kHotPercent, "GPU/Vulkan",
                      "Integrated GPUs with a small carve-out: new render targets and guest memory "
                      "pages go to a device-local heap only while its use stays under this "
                      "percentage of its budget, else to system memory")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

REXCVAR_DEFINE_UINT32(vulkan_device_local_budget_mb, 0, "GPU/Vulkan",
                      "Testing: treat the device-local memory budget as at most this many MB and "
                      "place memory as on an integrated GPU, also on a discrete one (0 = the "
                      "driver's budget)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .debug_only();

namespace rex::graphics::vulkan::fh1_device_memory {

namespace {

constexpr uint64_t kRefreshAfterBytes = uint64_t(32) << 20;
constexpr auto kSummaryInterval = std::chrono::seconds(60);

uint64_t Mb(uint64_t bytes) { return (bytes + (uint64_t(1) << 19)) >> 20; }

const char* ResultName(int32_t result) {
  switch (result) {
    case VK_ERROR_OUT_OF_HOST_MEMORY:
      return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_TOO_MANY_OBJECTS:
      return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FEATURE_NOT_PRESENT:
      return "VK_ERROR_FEATURE_NOT_PRESENT (no memory type fits)";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE:
      return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    default:
      return "VkResult";
  }
}

std::chrono::steady_clock::time_point next_summary_time;

}  // namespace

const char* UseName(Use use) {
  switch (use) {
    case Use::kTexture:
      return "textures";
    case Use::kRenderTarget:
      return "render targets";
    case Use::kSharedMemory:
      return "guest memory pages";
    case Use::kScaledResolve:
      return "scaled resolve memory";
    default:
      return "memory";
  }
}

uint32_t State::SharePercent(Use use) {
  return std::min(use == Use::kTexture ? REXCVAR_GET(vulkan_integrated_texture_percent)
                                       : REXCVAR_GET(vulkan_integrated_device_local_percent),
                  uint32_t(100));
}

State& Get() {
  static State state;
  return state;
}

void State::Initialize(const ui::vulkan::VulkanDevice* device) {
  device_ = device;
  const ui::vulkan::VulkanInstance::Functions& ifn = device->vulkan_instance()->functions();
  VkPhysicalDeviceProperties properties;
  ifn.vkGetPhysicalDeviceProperties(device->physical_device(), &properties);
  have_budget_ = device->extensions().ext_EXT_memory_budget &&
                 ifn.vkGetPhysicalDeviceMemoryProperties2 != nullptr;

  VkPhysicalDeviceMemoryBudgetPropertiesEXT budget_properties = {};
  budget_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
  VkPhysicalDeviceMemoryProperties2 memory_properties2 = {};
  memory_properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
  if (have_budget_) {
    memory_properties2.pNext = &budget_properties;
    ifn.vkGetPhysicalDeviceMemoryProperties2(device->physical_device(), &memory_properties2);
  } else {
    ifn.vkGetPhysicalDeviceMemoryProperties(device->physical_device(),
                                            &memory_properties2.memoryProperties);
  }
  const VkPhysicalDeviceMemoryProperties& memory = memory_properties2.memoryProperties;

  placement::MemoryInfo info;
  info.device_type = uint32_t(properties.deviceType);
  info.type_count = std::min(memory.memoryTypeCount, placement::kMaxTypes);
  for (uint32_t i = 0; i < info.type_count; ++i) {
    info.type_flags[i] = memory.memoryTypes[i].propertyFlags;
    info.type_heap[i] = memory.memoryTypes[i].heapIndex;
    type_heap_[i] = memory.memoryTypes[i].heapIndex;
  }
  info.heap_count = std::min(memory.memoryHeapCount, placement::kMaxHeaps);
  for (uint32_t i = 0; i < info.heap_count; ++i) {
    info.heap_size[i] = memory.memoryHeaps[i].size;
    info.heap_flags[i] = memory.memoryHeaps[i].flags;
  }
  layout_ = placement::Describe(info);
  cap_mb_ = REXCVAR_GET(vulkan_device_local_budget_mb);

  std::string heaps;
  for (uint32_t i = 0; i < info.heap_count; ++i) {
    if (!heaps.empty()) heaps += ", ";
    heaps += fmt::format("{}: {} MB {}", i, Mb(info.heap_size[i]),
                         (info.heap_flags[i] & placement::kHeapDeviceLocal) ? "device-local"
                                                                           : "system");
    if (have_budget_) heaps += fmt::format(" (budget {} MB)", Mb(budget_properties.heapBudget[i]));
  }
  const char* device_kind = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                                ? "integrated GPU"
                            : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                                ? "discrete GPU"
                                : "GPU";
  if (cap_mb_) {
    REXGPU_WARN(
        "Vulkan memory: device-local budget capped at {} MB for testing "
        "(vulkan_device_local_budget_mb), placement as on an integrated GPU",
        cap_mb_);
  }
  if ((layout_.integrated || cap_mb_) && layout_.can_move() &&
      !REXCVAR_GET(vulkan_integrated_memory_spill)) {
    REXGPU_INFO(
        "Vulkan memory heaps: {}; {}: GPU memory stays device-local "
        "(vulkan_integrated_memory_spill off)",
        heaps, device_kind);
  } else if ((layout_.integrated || cap_mb_) && layout_.can_move()) {
    REXGPU_INFO(
        "Vulkan memory heaps: {}; {}: new textures go to system memory (heap {}) above {}% of "
        "a device-local heap's budget, render targets and guest memory pages above {}%",
        heaps, device_kind, layout_.system_heap, SharePercent(Use::kTexture),
        SharePercent(Use::kRenderTarget));
  } else {
    REXGPU_INFO("Vulkan memory heaps: {}; {}: GPU memory stays device-local", heaps,
                device_kind);
  }
  Refresh();
  next_summary_time = std::chrono::steady_clock::now() + kSummaryInterval;
}

void State::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(remembered_mutex_);
    remembered_.clear();
  }
  device_ = nullptr;
}

bool State::active() const {
  return device_ && (layout_.integrated || cap_mb_) && layout_.can_move() &&
         REXCVAR_GET(vulkan_integrated_memory_spill);
}

State::Budget State::Query() {
  Budget b;
  if (!device_) return b;
  if (have_budget_) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget_properties = {};
    budget_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 memory_properties = {};
    memory_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    memory_properties.pNext = &budget_properties;
    device_->vulkan_instance()->functions().vkGetPhysicalDeviceMemoryProperties2(
        device_->physical_device(), &memory_properties);
    const VkPhysicalDeviceMemoryProperties& properties = memory_properties.memoryProperties;
    for (uint32_t i = 0; i < properties.memoryHeapCount; ++i) {
      if (i < placement::kMaxHeaps) {
        b.heaps.usage[i] = budget_properties.heapUsage[i];
        b.heaps.budget[i] = budget_properties.heapBudget[i];
      }
      if (properties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
        b.usage += budget_properties.heapUsage[i];
        b.budget += budget_properties.heapBudget[i];
      } else if (i == layout_.system_heap) {
        b.system_usage = budget_properties.heapUsage[i];
        b.system_budget = budget_properties.heapBudget[i];
      }
    }
  }
  b.budget = placement::CappedBudget(b.budget, cap_mb_);
  placement::ApplyCap(layout_, b.heaps, cap_mb_);
  return b;
}

void State::Refresh() {
  const Budget b = Query();
  std::lock_guard<std::mutex> lock(mutex_);
  last_ = b;
  for (std::atomic<uint64_t>& since : since_refresh_) since = 0;
}

State::Budget State::Estimate() {
  std::lock_guard<std::mutex> lock(mutex_);
  Budget b = last_;
  for (uint32_t h = 0; h < placement::kMaxHeaps; ++h) {
    const uint64_t since = since_refresh_[h];
    b.heaps.usage[h] += since;
    b.usage += since;
  }
  return b;
}

void State::EndFrame() {
  const bool is_active = active();
  bool failures_pending = false;
  for (const Counter& counter : counters_) {
    failures_pending |= counter.failures != counter.failures_reported;
  }
  if (!is_active && !failures_pending) return;
  if (is_active) Refresh();
  const auto now = std::chrono::steady_clock::now();
  if (now < next_summary_time || (!summary_due_ && !failures_pending)) return;
  next_summary_time = now + kSummaryInterval;
  summary_due_ = false;
  LogSummary();
}

void State::LogSummary() {
  const Budget b = Query();
  std::string heaps;
  if (layout_.device_local_heaps & (layout_.device_local_heaps - 1)) {
    for (uint32_t h = 0; h < layout_.heap_count; ++h) {
      if (!(layout_.device_local_heaps >> h & 1)) continue;
      heaps += fmt::format("{}heap {} {} of {}", heaps.empty() ? " [" : ", ", h,
                           Mb(b.heaps.usage[h]), Mb(b.heaps.budget[h]));
    }
    heaps += "]";
  }
  std::string device_local;
  for (size_t i = 0; i < size_t(Use::kCount); ++i) {
    const Counter& counter = counters_[i];
    if (!counter.device_local_bytes) continue;
    device_local += fmt::format("{}{} {} MB", device_local.empty() ? "" : ", ", UseName(Use(i)),
                                Mb(counter.device_local_bytes));
  }
  std::string moved;
  for (size_t i = 0; i < size_t(Use::kCount); ++i) {
    const Counter& counter = counters_[i];
    if (!counter.moved) continue;
    if (!moved.empty()) moved += ", ";
    moved += fmt::format("{} {} ({} MB)", UseName(Use(i)), counter.system_count.load(),
                         Mb(counter.system_bytes));
  }
  std::string failed;
  for (size_t i = 0; i < size_t(Use::kCount); ++i) {
    Counter& counter = counters_[i];
    const uint32_t failures = counter.failures;
    if (failures == counter.failures_reported) continue;
    if (!failed.empty()) failed += ", ";
    failed += fmt::format("{} {}", UseName(Use(i)), failures - counter.failures_reported);
    counter.failures_reported = failures;
  }
  REXGPU_INFO(
      "Vulkan memory: device-local {} of {} MB used{}{}{}{}, system memory {} of {} MB{}{}{}{}",
      Mb(b.usage), Mb(b.budget), heaps, device_local.empty() ? "" : " (", device_local,
      device_local.empty() ? "" : ")", Mb(b.system_usage), Mb(b.system_budget),
      moved.empty() ? "" : "; in system memory now: ", moved,
      failed.empty() ? "" : "; failed allocations since the last report: ", failed);
}

placement::Order State::Choose(Use use, uint32_t allowed, uint64_t size, bool allocator_blocks) {
  uint64_t since = 0;
  for (const std::atomic<uint64_t>& heap_since : since_refresh_) since += heap_since;
  if (since >= kRefreshAfterBytes) Refresh();
  const Budget b = Estimate();
  return placement::Choose(
      layout_, allowed,
      placement::FittingHeaps(layout_, SharePercent(use), b.heaps, size, allocator_blocks));
}

void State::Allocated(Use use, uint32_t type, uint64_t size) {
  Counter& counter = counters_[size_t(use)];
  if (IsDeviceLocalType(type)) {
    if (type_heap_[type] < placement::kMaxHeaps) since_refresh_[type_heap_[type]] += size;
    counter.device_local_bytes += size;
    return;
  }
  counter.system_bytes += size;
  ++counter.system_count;
  ++counter.moved;
  summary_due_ = true;
  if (!counter.move_logged) {
    counter.move_logged = true;
    const Budget b = Estimate();
    REXGPU_INFO(
        "Vulkan memory: device-local memory {} of {} MB used: new {} go to system memory "
        "(memory type {}, heap {})",
        Mb(b.usage), Mb(b.budget), UseName(use), type, type < 32 ? type_heap_[type] : 0);
  }
}

void State::Freed(Use use, uint32_t type, uint64_t size) {
  Counter& counter = counters_[size_t(use)];
  if (IsDeviceLocalType(type)) {
    counter.device_local_bytes -= size;
    return;
  }
  counter.system_bytes -= size;
  --counter.system_count;
}

void State::Remember(VkDeviceMemory memory, Use use, uint32_t type, uint64_t size) {
  std::lock_guard<std::mutex> lock(remembered_mutex_);
  remembered_[memory] = Remembered{use, type, size};
}

void State::Forget(VkDeviceMemory memory) {
  Remembered remembered;
  {
    std::lock_guard<std::mutex> lock(remembered_mutex_);
    auto it = remembered_.find(memory);
    if (it == remembered_.end()) return;
    remembered = it->second;
    remembered_.erase(it);
  }
  Freed(remembered.use, remembered.type, remembered.size);
}

void State::ReportFailure(Use use, const char* what, uint64_t size, uint32_t allowed,
                          int32_t result) {
  Counter& counter = counters_[size_t(use)];
  if (counter.failures++) return;
  // The first failure for this use: in full. Later ones are counted in the summary.
  ++counter.failures_reported;
  const Budget b = device_ ? Query() : Budget();
  std::string types;
  for (uint32_t i = 0; i < 32; ++i) {
    if (!(allowed >> i & 1)) continue;
    if (!types.empty()) types += " ";
    types += fmt::format("{}{}", i, IsDeviceLocalType(i) ? "(device-local)" : "");
  }
  REXGPU_ERROR(
      "Vulkan memory: could not allocate {} MB ({} bytes) for {} in any memory type it accepts "
      "(types {}): {} ({}); device-local memory {} of {} MB used, system memory {} of {} MB. "
      "It is left out (a texture reads black). Later failures are counted in the memory "
      "summary",
      Mb(size), size, what, types.empty() ? "none" : types, ResultName(result), result,
      Mb(b.usage), Mb(b.budget), Mb(b.system_usage), Mb(b.system_budget));
}

namespace {

// For a failure report: the memory an image would take and the types it accepts.
VkMemoryRequirements ImageRequirements(const ui::vulkan::VulkanDevice* device,
                                       const VkImageCreateInfo& info) {
  VkMemoryRequirements requirements = {};
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  VkImage image;
  if (dfn.vkCreateImage(device->device(), &info, nullptr, &image) == VK_SUCCESS) {
    dfn.vkGetImageMemoryRequirements(device->device(), image, &requirements);
    dfn.vkDestroyImage(device->device(), image, nullptr);
  }
  return requirements;
}

}  // namespace

void ReportImageFailure(const ui::vulkan::VulkanDevice* device, Use use,
                        const VkImageCreateInfo& info, int32_t result) {
  State& state = Get();
  const VkMemoryRequirements requirements = ImageRequirements(device, info);
  const std::string what = fmt::format(
      "{} (a {}x{}x{} image, {} mips, {} layers, format {})", UseName(use), info.extent.width,
      info.extent.height, info.extent.depth, info.mipLevels, info.arrayLayers,
      uint32_t(info.format));
  state.ReportFailure(use, what.c_str(), requirements.size, requirements.memoryTypeBits, result);
}

bool CreateDedicatedImage(const ui::vulkan::VulkanDevice* device, const VkImageCreateInfo& info,
                          Use use, VkImage& image_out, VkDeviceMemory& memory_out,
                          VkDeviceSize* size_out) {
  State& state = Get();
  if (!state.active()) {
    if (ui::vulkan::util::CreateDedicatedAllocationImage(
            device, info, ui::vulkan::util::MemoryPurpose::kDeviceLocal, image_out, memory_out,
            nullptr, size_out)) {
      return true;
    }
    ReportImageFailure(device, use, info, VK_ERROR_OUT_OF_DEVICE_MEMORY);
    return false;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  const VkDevice vk_device = device->device();
  VkImage image;
  VkResult result = dfn.vkCreateImage(vk_device, &info, nullptr, &image);
  if (result != VK_SUCCESS) {
    state.ReportFailure(use, "an image (vkCreateImage)", 0, 0, result);
    return false;
  }
  VkMemoryRequirements requirements;
  dfn.vkGetImageMemoryRequirements(vk_device, image, &requirements);
  const placement::Order order =
      state.Choose(use, requirements.memoryTypeBits, requirements.size, false);
  VkMemoryAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate_info.allocationSize = requirements.size;
  VkMemoryDedicatedAllocateInfo dedicated_info = {};
  if (device->extensions().ext_1_1_KHR_dedicated_allocation) {
    dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated_info.image = image;
    allocate_info.pNext = &dedicated_info;
  }
  result = VK_ERROR_FEATURE_NOT_PRESENT;
  for (uint32_t i = 0; i < order.count; ++i) {
    for (uint32_t type = 0; type < 32; ++type) {
      if (!(order.masks[i] >> type & 1)) continue;
      allocate_info.memoryTypeIndex = type;
      VkDeviceMemory memory;
      result = dfn.vkAllocateMemory(vk_device, &allocate_info, nullptr, &memory);
      if (result != VK_SUCCESS) continue;
      if (dfn.vkBindImageMemory(vk_device, image, memory, 0) != VK_SUCCESS) {
        dfn.vkFreeMemory(vk_device, memory, nullptr);
        result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        continue;
      }
      state.Allocated(use, type, requirements.size);
      state.Remember(memory, use, type, requirements.size);
      image_out = image;
      memory_out = memory;
      if (size_out) *size_out = requirements.size;
      return true;
    }
  }
  dfn.vkDestroyImage(vk_device, image, nullptr);
  ReportImageFailure(device, use, info, result);
  return false;
}

VkResult AllocateSparseMemory(const ui::vulkan::VulkanDevice* device, Use use, uint32_t allowed,
                              uint32_t default_type, VkDeviceSize size, VkDeviceMemory& memory_out,
                              uint32_t& type_out) {
  State& state = Get();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  VkMemoryAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate_info.allocationSize = size;
  VkResult result;
  if (!state.active()) {
    allocate_info.memoryTypeIndex = default_type;
    result = dfn.vkAllocateMemory(device->device(), &allocate_info, nullptr, &memory_out);
    if (result == VK_SUCCESS) {
      type_out = default_type;
    } else {
      state.ReportFailure(use, UseName(use), size, uint32_t(1) << default_type, result);
    }
    return result;
  }
  const placement::Order order = state.Choose(use, allowed, size, false);
  result = VK_ERROR_FEATURE_NOT_PRESENT;
  for (uint32_t i = 0; i < order.count; ++i) {
    for (uint32_t type = 0; type < 32; ++type) {
      if (!(order.masks[i] >> type & 1)) continue;
      allocate_info.memoryTypeIndex = type;
      result = dfn.vkAllocateMemory(device->device(), &allocate_info, nullptr, &memory_out);
      if (result != VK_SUCCESS) continue;
      state.Allocated(use, type, size);
      state.Remember(memory_out, use, type, size);
      type_out = type;
      return VK_SUCCESS;
    }
  }
  state.ReportFailure(use, UseName(use), size, allowed, result);
  return result;
}

}  // namespace rex::graphics::vulkan::fh1_device_memory
