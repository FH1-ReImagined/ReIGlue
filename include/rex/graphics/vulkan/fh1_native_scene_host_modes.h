#pragma once
#include <string_view>
namespace rex::graphics::vulkan::fh1 {
// These optional flags belong to the host. Empty means the host does not
// register the corresponding setting; this policy does not mutate any cvar.
struct NativeSceneHostModes {
  std::string_view scene_resolution, vr_mode, ultrawide, present_effect;
  int native_width=0, native_height=0;
};
constexpr bool CompatibleNativeSceneHostModes(const NativeSceneHostModes& m) {
  return (m.scene_resolution.empty() || m.scene_resolution == "720") &&
         (m.vr_mode.empty() || m.vr_mode == "off") &&
         (m.ultrawide.empty() || m.ultrawide == "false") &&
         m.present_effect != "fsr3" && m.native_width == 0 && m.native_height == 0;
}
}
