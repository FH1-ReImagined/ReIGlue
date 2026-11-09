#pragma once
// FH1 (#640): GPU time per kind of work, from Vulkan timestamps (fh1_gpu_profile seconds).
//
// The command processor names what it records ("zones": a render pass's guest draws, render
// target ownership transfers, resolves, texture loads, shared memory uploads, the swap). The
// deferred command buffer writes a timestamp before the first command of a new zone (bottom of
// pipe: when everything before it is done), so the time between two timestamps is the time the
// GPU took for the commands of the zone between them. Off (0) costs one branch per command.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace rex::graphics::vulkan {

class DeferredCommandBuffer;

class Fh1GpuProfile {
 public:
  enum Zone : uint16_t {
    kOther,          // anything not named below (draw setup, barriers)
    kUpload,         // guest memory the CPU wrote, copied into the shared memory
    kTextureLoad,    // textures untiled / converted from the shared memory
    kTransfer,       // render target ownership transfers (EDRAM aliasing)
    kResolve,        // resolve copies to the shared memory (compute)
    kResolveDump,    // render targets dumped to the EDRAM buffer for a resolve
    kResolveDirect,  // resolves copied directly from the host render target
    kResolveClear,   // the clears of resolves
    kReadback,       // resolves and memexports copied back to the CPU
    kSwap,           // the swap: gamma, the front buffer copy, the HUD masks
    kEffects,        // FH1 additions: ambient occlusion, SMAA, DLSS / FSR 3
    kFixedCount,
  };
  static constexpr uint16_t kFirstDrawZone = 32;

  Fh1GpuProfile() = default;
  Fh1GpuProfile(const Fh1GpuProfile&) = delete;
  Fh1GpuProfile& operator=(const Fh1GpuProfile&) = delete;
  // The query pools go with Shutdown (the device may be gone when this runs at exit).
  ~Fh1GpuProfile() {
    if (file_) std::fclose(file_);
  }

  // False (and inactive) when the graphics queue has no timestamps.
  bool Initialize(const ui::vulkan::VulkanDevice* device, double period_s,
                  const std::string& path);
  void Shutdown();
  bool active() const { return pools_ready_; }

  // At the start of a submission's commands (no render pass open): resets this submission's
  // queries and writes its first timestamp.
  void BeginSubmission(DeferredCommandBuffer& commands, uint64_t submission, uint64_t frame);
  // After the submission's last command.
  void EndSubmission(DeferredCommandBuffer& commands);
  // A timestamp starting `zone` (DeferredCommandBuffer calls this).
  void Mark(DeferredCommandBuffer& commands, uint16_t zone);
  // Reads the timestamps of the submissions up to `completed`; logs every period.
  void Completed(uint64_t completed);

  // The zone of a render pass's draws, named on its first use.
  uint16_t DrawZone(uint64_t key, uint32_t width, uint32_t height, uint32_t msaa_log2,
                    const char* depth, const char* const* colors, uint32_t color_count,
                    uint64_t pixel_shader);
  // Any other named zone (a kind of transfer, a texture format): kNoZone if not named yet.
  static constexpr uint16_t kNoZone = 0xFFFF;
  uint16_t FindZone(uint64_t key) const {
    auto it = draw_zones_.find(key);
    return it != draw_zones_.end() ? it->second : kNoZone;
  }
  uint16_t AddZone(uint64_t key, std::string name);

 private:
  static constexpr uint32_t kSlots = 16;
  static constexpr uint32_t kQueriesPerSlot = 8192;
  struct Slot {
    VkQueryPool pool = VK_NULL_HANDLE;
    uint64_t submission = 0;
    uint64_t frame = 0;
    uint32_t count = 0;
    bool pending = false;
    std::vector<uint16_t> zones;
  };
  struct ZoneSum {
    double ms = 0.0;
    uint64_t intervals = 0;
  };

  void Report();
  const char* ZoneName(uint16_t zone) const;

  const ui::vulkan::VulkanDevice* device_ = nullptr;
  PFN_vkCmdWriteTimestamp write_timestamp_ = nullptr;
  PFN_vkGetQueryPoolResults get_results_ = nullptr;
  double ms_per_tick_ = 0.0;
  uint64_t tick_mask_ = ~uint64_t(0);
  bool pools_ready_ = false;
  Slot slots_[kSlots];
  Slot* current_ = nullptr;  // the open submission's slot, null when not measured
  double period_s_ = 10.0;
  std::FILE* file_ = nullptr;
  std::chrono::steady_clock::time_point window_start_{};
  std::chrono::steady_clock::time_point start_{};
  std::vector<ZoneSum> sums_;
  std::vector<uint64_t> results_;
  double busy_ms_ = 0.0;
  uint64_t submissions_ = 0, skipped_ = 0, overflowed_ = 0, frames_ = 0, last_frame_ = 0;
  uint32_t window_ = 0;
  std::unordered_map<uint64_t, uint16_t> draw_zones_;
  std::vector<std::string> draw_names_;
};

}  // namespace rex::graphics::vulkan
