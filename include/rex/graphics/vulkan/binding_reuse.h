#pragma once
#include <cstdint>
#include <cstdlib>

namespace rex::graphics::vulkan {
inline bool BindingReuseEnabled() {
  static const bool enabled = [] { const char* p = std::getenv("FORZA_VK_BINDING_REUSE"); return p && p[0] == '1'; }();
  return enabled;
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
