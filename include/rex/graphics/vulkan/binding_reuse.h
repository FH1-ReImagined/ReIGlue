#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace rex::graphics::vulkan {
// Set by the command processor at each frame start from vulkan_binding_request_reuse (or
// FORZA_VK_BINDING_REUSE=0/1, which overrides it).
inline bool binding_request_reuse_enabled = false;
inline bool BindingReuseEnabled() { return binding_request_reuse_enabled; }
inline bool BindingReuseEnvOverride(bool cvar_value) {
  static const int env = [] {
    const char* p = std::getenv("FORZA_VK_BINDING_REUSE");
    return p && *p ? int(p[0] == '1') : -1;
  }();
  return env < 0 ? cvar_value : env != 0;
}
// vulkan_texture_descriptor_reuse, or FORZA_VK_TEXTURE_SET_REUSE=0/1, which overrides it.
inline bool Fh1TextureSetReuseEnvOverride(bool cvar_value) {
  static const int env = [] {
    const char* p = std::getenv("FORZA_VK_TEXTURE_SET_REUSE");
    return p && *p ? int(p[0] == '1') : -1;
  }();
  return env < 0 ? cvar_value : env != 0;
}
// Bumped whenever an image view or a sampler a guest texture descriptor may hold is destroyed:
// a handle made later may have the same value, so descriptor sets remembered by their handles
// are forgotten (patch 0444).
inline std::atomic<uint64_t> fh1_descriptor_handles_destroyed{0};
inline void Fh1DescriptorHandlesDestroyed() {
  fh1_descriptor_handles_destroyed.fetch_add(1, std::memory_order_relaxed);
}
inline bool BindingReuseVerify() {
  static const bool enabled = [] { const char* p = std::getenv("FORZA_VK_BINDING_VERIFY"); return p && p[0] == '1'; }();
  return enabled;
}
inline bool BindingReuseStats() {
  static const bool enabled = [] { const char* p = std::getenv("FORZA_VK_BINDING_STATS"); return p && p[0] == '1'; }();
  return enabled;
}
// Set N is compatible only when layouts 0 through N are compatible.
inline uint32_t BindingCompatiblePrefixMask(uint32_t first_incompatible) {
  return first_incompatible >= 32 ? UINT32_MAX :
      ((UINT32_C(1) << first_incompatible) - 1);
}
struct SamplerBindingReuse {
  const void* shader = nullptr;
  const void* cache = nullptr;
  uint64_t submission = UINT64_MAX;
  int32_t anisotropy = INT32_MIN;
  uint32_t dirty = UINT32_MAX;
  bool Matches(const void* next_shader, const void* next_cache,
               uint64_t next_submission, int32_t next_anisotropy,
               uint32_t sampled_mask) const {
    return shader == next_shader && cache == next_cache &&
           submission == next_submission && anisotropy == next_anisotropy &&
           !(dirty & sampled_mask);
  }
};
// A request has already marked these resources used and transitioned them in
// this submission. Any invalidation or intervening usage change rejects reuse.
struct TextureRequestReuse {
  uint64_t submission = UINT64_MAX;
  uint64_t usage = 0;
  uint32_t mask = 0;
  bool Matches(uint64_t next_submission, uint64_t next_usage,
               uint32_t requested, uint32_t in_sync, bool outdated) const {
    return submission == next_submission && usage == next_usage &&
           !(requested & ~mask) && !(requested & ~in_sync) && !outdated;
  }
  void Remember(uint64_t next_submission, uint64_t next_usage, uint32_t requested) {
    submission = next_submission; usage = next_usage; mask = requested;
  }
};
}  // namespace rex::graphics::vulkan
