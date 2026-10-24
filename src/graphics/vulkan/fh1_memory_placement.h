#pragma once
// FH1 (#268): where the Vulkan backend puts new memory on integrated GPUs.
//
// An APU's device-local memory is a carve-out of system RAM (the BIOS "UMA frame buffer",
// often 512 MB) and its other heap is the rest of RAM, which the GPU reads about as fast
// (write-combined system memory). The backend used to put everything in device-local memory:
// in free roam about 1 GB against a 486 MB budget. A Windows driver then pages device-local
// memory in and out: textures read black and the driver can time out.
//
// So on an integrated GPU with a small carve-out (or with the test budget cap), new textures
// go to a device-local heap only while its use stays under kTexturePercent of its own budget,
// and render targets and guest memory pages under kHotPercent; past that they go to a memory
// type of the system memory heap that the resource accepts. The shares leave room in the
// carve-out for the rest of the system: AMD's Windows driver reports a fixed budget (95% of
// each heap) that ignores the compositor and other programs, and with the carve-out nearly
// full the driver paged until one submission ran past Windows' 2 s timeout (#268). The device-local heaps are tried
// one by one, plain device-local types before host-visible ones: AMD's Windows driver splits
// a Vega APU's 512 MB carve-out into a 256 MB heap and a 256 MB host-visible heap, each with
// its own budget. Discrete GPUs, and APUs whose device-local heap is not small next to the
// system heap (RADV reports two thirds of the shared memory as device-local), are not affected.
//
// Free of the Vulkan headers so that tools/test_vulkan_memory_placement.cpp can test it;
// the flag values are Vulkan's.
#include <cstdint>

namespace rex::graphics::vulkan::fh1_memory_placement {

// VkMemoryPropertyFlagBits.
constexpr uint32_t kPropertyDeviceLocal = 0x1;
constexpr uint32_t kPropertyHostVisible = 0x2;
constexpr uint32_t kPropertyHostCoherent = 0x4;
constexpr uint32_t kPropertyHostCached = 0x8;
// Lazily allocated, protected, AMD device-coherent and device-uncached, NVIDIA RDMA: types
// that need a feature or an extension of their own.
constexpr uint32_t kPropertySpecial = 0x10 | 0x20 | 0x40 | 0x80 | 0x100;
// VkMemoryHeapFlagBits.
constexpr uint32_t kHeapDeviceLocal = 0x1;
// VkPhysicalDeviceType.
constexpr uint32_t kDeviceTypeIntegrated = 1;
constexpr uint32_t kDeviceTypeDiscrete = 2;
constexpr uint32_t kDeviceTypeCpu = 4;

constexpr uint32_t kMaxTypes = 32;
constexpr uint32_t kMaxHeaps = 16;
constexpr uint32_t kMaxTiers = 8;
constexpr uint32_t kMaxOrder = 2 * kMaxTiers + 2;

// The default shares (vulkan_integrated_texture_percent, vulkan_integrated_device_local_percent):
// textures never go to device-local memory there (they are read-only and the GPU samples
// system memory about as fast); render targets and guest memory pages while a heap's use
// (with the new allocation) stays under half its budget.
constexpr uint32_t kTexturePercent = 0;
constexpr uint32_t kHotPercent = 50;
// A carve-out is small when the system memory heap is at least this many times its size.
constexpr uint64_t kSmallCarveOutRatio = 2;

struct MemoryInfo {
  uint32_t device_type = 0;
  uint32_t type_count = 0;
  uint32_t type_flags[kMaxTypes] = {};
  uint32_t type_heap[kMaxTypes] = {};
  uint32_t heap_count = 0;
  uint64_t heap_size[kMaxHeaps] = {};
  uint32_t heap_flags[kMaxHeaps] = {};
};

struct Layout {
  // Not a discrete GPU (nor a CPU), with device-local heaps that are small next to a system
  // memory heap to move memory to.
  bool integrated = false;
  uint32_t device_local_types = 0;  // ordinary memory types with the device-local property
  uint32_t device_local_heaps = 0;  // heaps with the device-local flag (a mask)
  uint64_t device_local_bytes = 0;  // their size
  uint32_t system_heap = UINT32_MAX;  // the largest heap without the device-local flag
  uint64_t system_heap_bytes = 0;
  uint32_t system_types = 0;         // its write-combined (not host-cached) types
  uint32_t system_types_cached = 0;  // its host-cached types (snooped: slower for the GPU)
  // The device-local types in tiers of one heap each, in order of preference: plain
  // device-local types first, then host-visible ones.
  uint32_t tier_count = 0;
  uint32_t tier_types[kMaxTiers] = {};
  uint32_t tier_heap[kMaxTiers] = {};
  uint32_t heap_count = 0;
  uint64_t heap_bytes[kMaxHeaps] = {};
  bool can_move() const { return (system_types | system_types_cached) != 0; }
};

// Use and budget per heap (VK_EXT_memory_budget).
struct HeapUse {
  uint64_t usage[kMaxHeaps] = {};
  uint64_t budget[kMaxHeaps] = {};
};

inline uint32_t LowestType(uint32_t mask) {
  for (uint32_t i = 0; i < kMaxTypes; ++i) {
    if (mask & (uint32_t(1) << i)) return i;
  }
  return UINT32_MAX;
}

inline Layout Describe(const MemoryInfo& m) {
  Layout l;
  const uint32_t heap_count = m.heap_count < kMaxHeaps ? m.heap_count : kMaxHeaps;
  const uint32_t type_count = m.type_count < kMaxTypes ? m.type_count : kMaxTypes;
  l.heap_count = heap_count;
  for (uint32_t h = 0; h < heap_count; ++h) {
    l.heap_bytes[h] = m.heap_size[h];
    if (m.heap_flags[h] & kHeapDeviceLocal) {
      l.device_local_heaps |= uint32_t(1) << h;
      l.device_local_bytes += m.heap_size[h];
    }
  }
  // The system memory heap: the largest heap without the device-local flag that has a type
  // usable for ordinary resources.
  for (uint32_t h = 0; h < heap_count; ++h) {
    if (m.heap_flags[h] & kHeapDeviceLocal) continue;
    bool usable = false;
    for (uint32_t t = 0; t < type_count; ++t) {
      usable |= m.type_heap[t] == h &&
                !(m.type_flags[t] & (kPropertySpecial | kPropertyDeviceLocal));
    }
    if (usable && (l.system_heap == UINT32_MAX || m.heap_size[h] > l.system_heap_bytes)) {
      l.system_heap = h;
      l.system_heap_bytes = m.heap_size[h];
    }
  }
  for (uint32_t t = 0; t < type_count; ++t) {
    const uint32_t flags = m.type_flags[t];
    const uint32_t bit = uint32_t(1) << t;
    if (flags & kPropertySpecial) continue;
    if (flags & kPropertyDeviceLocal) l.device_local_types |= bit;
    if (m.type_heap[t] != l.system_heap || (flags & kPropertyDeviceLocal)) {
      continue;
    }
    if (flags & kPropertyHostCached) {
      l.system_types_cached |= bit;
    } else {
      l.system_types |= bit;
    }
  }
  // The tiers: per heap, its plain device-local types; then, per heap, its host-visible ones.
  for (uint32_t pass = 0; pass < 2; ++pass) {
    for (uint32_t h = 0; h < heap_count; ++h) {
      uint32_t types = 0;
      for (uint32_t t = 0; t < type_count; ++t) {
        if (!(l.device_local_types >> t & 1) || m.type_heap[t] != h) continue;
        if (bool(m.type_flags[t] & kPropertyHostVisible) == bool(pass)) types |= uint32_t(1) << t;
      }
      if (types && l.tier_count < kMaxTiers) {
        l.tier_types[l.tier_count] = types;
        l.tier_heap[l.tier_count] = h;
        ++l.tier_count;
      }
    }
  }
  const bool small_carve_out =
      l.device_local_bytes && l.device_local_bytes * kSmallCarveOutRatio <= l.system_heap_bytes;
  l.integrated = l.can_move() && l.tier_count && small_carve_out &&
                 m.device_type != kDeviceTypeDiscrete && m.device_type != kDeviceTypeCpu;
  return l;
}

// The budget the code sees: the driver's, or at most the test cap.
inline uint64_t CappedBudget(uint64_t budget, uint32_t cap_mb) {
  if (!cap_mb) return budget;
  const uint64_t cap = uint64_t(cap_mb) << 20;
  return budget && budget < cap ? budget : cap;
}

// The test cap per heap: the device-local heaps' budgets scaled down so that they add up to
// the cap (in proportion to the heaps' sizes when the driver reports no budget).
inline void ApplyCap(const Layout& l, HeapUse& u, uint32_t cap_mb) {
  if (!cap_mb) return;
  const uint64_t cap = uint64_t(cap_mb) << 20;
  uint64_t total = 0;
  for (uint32_t h = 0; h < l.heap_count; ++h) {
    if (l.device_local_heaps >> h & 1) total += u.budget[h];
  }
  const bool by_size = !total;
  if (by_size) total = l.device_local_bytes;
  if (!total || (!by_size && total <= cap)) return;
  for (uint32_t h = 0; h < l.heap_count; ++h) {
    if (!(l.device_local_heaps >> h & 1)) continue;
    const uint64_t share = by_size ? l.heap_bytes[h] : u.budget[h];
    u.budget[h] = uint64_t(double(share) / double(total) * double(cap));
  }
}

// The device memory block the texture cache's allocator (VMA) makes when an allocation does
// not fit its existing blocks: an eighth of a heap of up to 1 GB, else 256 MB.
inline uint64_t AllocatorBlockBytes(uint64_t heap_bytes) {
  if (heap_bytes && heap_bytes <= (uint64_t(1) << 30)) return (heap_bytes / 8 + 31) & ~uint64_t(31);
  return uint64_t(256) << 20;
}

// Whether `growth` more bytes of device-local memory keep a heap's use within `percent` of
// its budget. Without a budget (no VK_EXT_memory_budget and no cap) everything fits.
inline bool Fits(uint32_t percent, uint64_t usage, uint64_t growth, uint64_t budget) {
  if (!budget) return true;
  return (usage + growth) * 100 <= budget * uint64_t(percent);
}

// The device-local heaps (a mask) that can take `size` more bytes within `percent` of their
// budgets. With allocator_blocks (VMA), a new block of that heap counts instead when it is
// larger.
inline uint32_t FittingHeaps(const Layout& l, uint32_t percent, const HeapUse& u, uint64_t size,
                             bool allocator_blocks) {
  uint32_t heaps = 0;
  for (uint32_t h = 0; h < l.heap_count; ++h) {
    if (!(l.device_local_heaps >> h & 1)) continue;
    uint64_t growth = size;
    if (allocator_blocks) {
      const uint64_t block = AllocatorBlockBytes(l.heap_bytes[h]);
      growth = block > size ? block : size;
    }
    if (Fits(percent, u.usage[h], growth, u.budget[h])) heaps |= uint32_t(1) << h;
  }
  return heaps;
}

// Whether a device-local heap is at or over its whole budget.
inline bool HeapOverBudget(const Layout& l, const HeapUse& u) {
  for (uint32_t h = 0; h < l.heap_count; ++h) {
    if ((l.device_local_heaps >> h & 1) && u.budget[h] && u.usage[h] >= u.budget[h]) return true;
  }
  return false;
}

// The memory type masks to try, in order, for a resource that accepts `allowed` types.
struct Order {
  uint32_t masks[kMaxOrder] = {};
  uint32_t count = 0;
  bool system_first = false;  // no device-local heap has room: system memory first
  void Add(uint32_t mask) {
    if (mask && count < kMaxOrder) masks[count++] = mask;
  }
};

// The device-local tiers whose heap has room, then system memory (write-combined, then
// cached), then the other device-local tiers (better than no memory at all).
inline Order Choose(const Layout& l, uint32_t allowed, uint32_t fitting_heaps) {
  Order o;
  for (uint32_t i = 0; i < l.tier_count; ++i) {
    if (fitting_heaps >> l.tier_heap[i] & 1) o.Add(allowed & l.tier_types[i]);
  }
  o.system_first = o.count == 0 && (allowed & (l.system_types | l.system_types_cached)) != 0;
  o.Add(allowed & l.system_types);
  o.Add(allowed & l.system_types_cached);
  for (uint32_t i = 0; i < l.tier_count; ++i) {
    if (!(fitting_heaps >> l.tier_heap[i] & 1)) o.Add(allowed & l.tier_types[i]);
  }
  return o;
}

}  // namespace rex::graphics::vulkan::fh1_memory_placement
