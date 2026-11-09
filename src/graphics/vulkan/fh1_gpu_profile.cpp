// FH1 (#640): GPU time per kind of work (fh1_gpu_profile.h).
#include <rex/graphics/vulkan/fh1_gpu_profile.h>

#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>

#include <algorithm>
#include <cinttypes>

#include <fmt/format.h>

namespace rex::graphics::vulkan {

namespace {
const char* const kFixedNames[Fh1GpuProfile::kFixedCount] = {
    "other", "upload", "texture load", "rt transfer", "resolve copy", "resolve dump",
    "resolve direct", "resolve clear", "readback", "swap", "fh1 effects",
};
}  // namespace

bool Fh1GpuProfile::Initialize(const ui::vulkan::VulkanDevice* device, double period_s,
                               const std::string& path) {
  device_ = device;
  period_s_ = std::max(period_s, 1.0);
  const auto& ifn = device->vulkan_instance()->functions();
  VkPhysicalDeviceProperties properties;
  ifn.vkGetPhysicalDeviceProperties(device->physical_device(), &properties);
  uint32_t family_count = 0;
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(device->physical_device(), &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(device->physical_device(), &family_count,
                                               families.data());
  const uint32_t family = device->queue_family_graphics_compute();
  const uint32_t valid_bits = family < family_count ? families[family].timestampValidBits : 0;
  if (!valid_bits || properties.limits.timestampPeriod <= 0.0f) {
    REXGPU_WARN("FH1 GPU profile: the graphics queue has no timestamps; off");
    return false;
  }
  tick_mask_ = valid_bits >= 64 ? ~uint64_t(0) : (uint64_t(1) << valid_bits) - 1;
  ms_per_tick_ = double(properties.limits.timestampPeriod) * 1e-6;
  write_timestamp_ = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
      ifn.vkGetDeviceProcAddr(device->device(), "vkCmdWriteTimestamp"));
  get_results_ = reinterpret_cast<PFN_vkGetQueryPoolResults>(
      ifn.vkGetDeviceProcAddr(device->device(), "vkGetQueryPoolResults"));
  if (!write_timestamp_ || !get_results_) return false;
  const auto& dfn = device->functions();
  VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  info.queryCount = kQueriesPerSlot;
  for (Slot& slot : slots_) {
    if (dfn.vkCreateQueryPool(device->device(), &info, nullptr, &slot.pool) != VK_SUCCESS) {
      REXGPU_WARN("FH1 GPU profile: no query pool; off");
      Shutdown();
      return false;
    }
    slot.zones.resize(kQueriesPerSlot);
  }
  results_.resize(kQueriesPerSlot);
  sums_.resize(kFirstDrawZone);
  if (!path.empty()) {
    file_ = std::fopen(path.c_str(), "a");
    if (file_) std::fputs("window,t_s,frames,busy_ms,zone,ms_per_frame,intervals_per_frame\n", file_);
  }
  start_ = window_start_ = std::chrono::steady_clock::now();
  pools_ready_ = true;
  REXGPU_INFO("FH1 GPU profile: on, {:.3f} ns a tick, {} valid bits, a report every {:.0f} s{}",
              properties.limits.timestampPeriod, valid_bits, period_s_,
              file_ ? " (also to " + path + ")" : std::string());
  return true;
}

void Fh1GpuProfile::Shutdown() {
  if (device_) {
    const auto& dfn = device_->functions();
    for (Slot& slot : slots_) {
      if (slot.pool != VK_NULL_HANDLE) {
        dfn.vkDestroyQueryPool(device_->device(), slot.pool, nullptr);
        slot.pool = VK_NULL_HANDLE;
      }
    }
  }
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  pools_ready_ = false;
  current_ = nullptr;
}

void Fh1GpuProfile::BeginSubmission(DeferredCommandBuffer& commands, uint64_t submission,
                                    uint64_t frame) {
  current_ = nullptr;
  if (!pools_ready_) return;
  Slot& slot = slots_[submission % kSlots];
  if (slot.pending) {
    // Its results were not read yet (more submissions in flight than slots): not measured.
    ++skipped_;
    return;
  }
  slot.submission = submission;
  slot.frame = frame;
  slot.count = 0;
  // With current_ null, a zone change pending from before puts no timestamp before the reset.
  commands.CmdVkResetQueryPool(slot.pool, 0, kQueriesPerSlot);
  current_ = &slot;
  Mark(commands, commands.fh1_gpu_zone());
}

void Fh1GpuProfile::EndSubmission(DeferredCommandBuffer& commands) {
  if (!current_) return;
  // The last timestamp ends the last zone; its own zone is not used.
  if (current_->count < kQueriesPerSlot) {
    Mark(commands, kOther);
  }
  current_->pending = current_->count >= 2;
  current_ = nullptr;
}

void Fh1GpuProfile::Mark(DeferredCommandBuffer& commands, uint16_t zone) {
  commands.Fh1GpuZoneMarked(zone);
  Slot* slot = current_;
  if (!slot) return;
  // Keep the last query for the end of the submission.
  if (slot->count >= kQueriesPerSlot - 1) {
    if (slot->count == kQueriesPerSlot - 1) ++overflowed_;
    return;
  }
  slot->zones[slot->count] = zone;
  commands.CmdFh1Timestamp(write_timestamp_, slot->pool, slot->count);
  ++slot->count;
}

void Fh1GpuProfile::Completed(uint64_t completed) {
  if (!pools_ready_) return;
  const VkDevice device = device_->device();
  while (true) {
    // In submission order, so that frames are counted once.
    Slot* next = nullptr;
    for (Slot& s : slots_) {
      if (s.pending && s.submission <= completed && (!next || s.submission < next->submission))
        next = &s;
    }
    if (!next) break;
    Slot& slot = *next;
    slot.pending = false;
    const VkResult result =
        get_results_(device, slot.pool, 0, slot.count, sizeof(uint64_t) * slot.count,
                     results_.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS) {
      ++skipped_;
      continue;
    }
    ++submissions_;
    if (slot.frame != last_frame_) {
      last_frame_ = slot.frame;
      ++frames_;
    }
    for (uint32_t i = 1; i < slot.count; ++i) {
      const double ms = double((results_[i] - results_[i - 1]) & tick_mask_) * ms_per_tick_;
      // A wrapped or reordered pair (should not happen within a command buffer).
      if (ms > 1000.0) continue;
      const uint16_t zone = slot.zones[i - 1];
      if (zone >= sums_.size()) sums_.resize(zone + 1);
      sums_[zone].ms += ms;
      ++sums_[zone].intervals;
      busy_ms_ += ms;
    }
  }
  const auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration<double>(now - window_start_).count() >= period_s_) {
    Report();
    window_start_ = now;
  }
}

const char* Fh1GpuProfile::ZoneName(uint16_t zone) const {
  if (zone < kFixedCount) return kFixedNames[zone];
  if (zone >= kFirstDrawZone && size_t(zone - kFirstDrawZone) < draw_names_.size())
    return draw_names_[zone - kFirstDrawZone].c_str();
  return "?";
}

void Fh1GpuProfile::Report() {
  if (frames_) {
    const double frames = double(frames_);
    std::vector<uint16_t> order;
    for (uint16_t z = 0; z < sums_.size(); ++z) {
      if (sums_[z].intervals) order.push_back(z);
    }
    std::sort(order.begin(), order.end(),
              [this](uint16_t a, uint16_t b) { return sums_[a].ms > sums_[b].ms; });
    // Kinds: all draws together, then each fixed zone; then the costliest draw zones.
    double draws = 0.0;
    for (uint16_t z = kFirstDrawZone; z < sums_.size(); ++z) {
      if (ZoneName(z)[0] != '~') draws += sums_[z].ms;
    }
    std::string kinds = fmt::format("draws {:.2f}", draws / frames);
    for (uint16_t z = 0; z < kFixedCount && z < sums_.size(); ++z) {
      if (sums_[z].intervals) kinds += fmt::format(", {} {:.2f}", kFixedNames[z], sums_[z].ms / frames);
    }
    std::string top;
    uint32_t shown = 0;
    for (uint16_t z : order) {
      if (z < kFirstDrawZone) continue;
      if (shown++ == 12) break;
      top += fmt::format("{}[{}] {:.2f} ({:.0f})", top.empty() ? "" : "; ", ZoneName(z),
                         sums_[z].ms / frames, double(sums_[z].intervals) / frames);
    }
    REXGPU_INFO(
        "FH1 GPU profile: {} frames, {:.2f} ms a frame in the command processor's submissions "
        "({:.1f} submissions a frame, {} not measured, {} full): {}",
        frames_, busy_ms_ / frames, double(submissions_) / frames, skipped_, overflowed_, kinds);
    REXGPU_INFO("FH1 GPU profile: costliest render passes, ms a frame (intervals): {}", top);
    if (file_) {
      const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
      for (uint16_t z : order) {
        std::string name = ZoneName(z);
        std::replace(name.begin(), name.end(), ',', ' ');
        std::fprintf(file_, "%u,%.1f,%" PRIu64 ",%.3f,%s,%.4f,%.2f\n", window_, t, frames_,
                     busy_ms_ / frames, name.c_str(), sums_[z].ms / frames,
                     double(sums_[z].intervals) / frames);
      }
      std::fflush(file_);
    }
  }
  ++window_;
  for (ZoneSum& sum : sums_) sum = ZoneSum();
  busy_ms_ = 0.0;
  submissions_ = skipped_ = overflowed_ = frames_ = 0;
}

uint16_t Fh1GpuProfile::AddZone(uint64_t key, std::string name) {
  auto it = draw_zones_.find(key);
  if (it != draw_zones_.end()) return it->second;
  if (draw_names_.size() >= 0xFFF0 - kFirstDrawZone) return kOther;
  const uint16_t zone = uint16_t(kFirstDrawZone + draw_names_.size());
  draw_names_.push_back(std::move(name));
  draw_zones_.emplace(key, zone);
  return zone;
}

uint16_t Fh1GpuProfile::DrawZone(uint64_t key, uint32_t width, uint32_t height,
                                 uint32_t msaa_log2, const char* depth,
                                 const char* const* colors, uint32_t color_count,
                                 uint64_t pixel_shader) {
  const uint64_t full = key ^ (pixel_shader * 0x9E3779B97F4A7C15ull) ^
                        (uint64_t(width) << 40) ^ (uint64_t(height) << 52);
  auto it = draw_zones_.find(full);
  if (it != draw_zones_.end()) return it->second;
  if (draw_names_.size() >= 0xFFFF - kFirstDrawZone) return kOther;
  std::string name = fmt::format("{}x{} {}x", width, height, 1u << msaa_log2);
  if (depth) name += fmt::format(" {}", depth);
  for (uint32_t i = 0; i < color_count; ++i) {
    if (colors[i]) name += fmt::format(" {}", colors[i]);
  }
  if (!depth && !color_count) name += " no targets";
  if (pixel_shader) name += fmt::format(" ps {:016X}", pixel_shader);
  const uint16_t zone = uint16_t(kFirstDrawZone + draw_names_.size());
  draw_names_.push_back(std::move(name));
  draw_zones_.emplace(full, zone);
  return zone;
}

}  // namespace rex::graphics::vulkan
