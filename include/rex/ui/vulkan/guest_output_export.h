/**
 * Patch 0393: what the GPU plugin's command processor hands the guest output export (patch 0391,
 * src/ui/vulkan/vulkan_presenter.cpp) for a VR client: per frame, the scene before the HUD and
 * the HUD alone, made in the same submission as the guest output (and, patch 0394, both eyes).
 * The client asks for them with rexui_SetGuestOutputExport's flags and gets them with the frame
 * (RexGuestOutputFrame and the client-facing exports are in vulkan_presenter.cpp). Internal to the
 * SDK: the game never includes it. C functions, as patch 0391's rexui_SetNextGuestOutputTag, so
 * the plugin finds them in the runtime on every platform.
 */
#pragma once

#include <cstdint>

#include <rex/ui/vulkan/api.h>

// rexui_SetGuestOutputExport flags.
#define REX_GUEST_OUTPUT_EXPORT_FRAMES 0x1u  // the guest output (patch 0391)
#define REX_GUEST_OUTPUT_EXPORT_LAYERS 0x2u  // the scene before the HUD and the HUD alone
#define REX_GUEST_OUTPUT_EXPORT_STEREO 0x4u  // both eyes from the scene and its depth (patch 0394)

// RexGuestOutputLayers::layers bits.
#define REX_GUEST_OUTPUT_LAYER_SCENE 0x1u
#define REX_GUEST_OUTPUT_LAYER_HUD 0x2u
#define REX_GUEST_OUTPUT_LAYER_STEREO 0x4u

extern "C" {
struct RexGuestOutputLayers {
  uint32_t layers;
  // The scene before the first HUD draw, through the same gamma ramp and in the same format and
  // channel order as the guest output (A2B10G10R10).
  uint32_t scene_width, scene_height;
  VkImage scene;
  // The HUD alone at 1280 x 720: R8G8B8A8_UNORM, unpremultiplied colour and its coverage.
  uint32_t hud_width, hud_height;
  VkImage hud;
  // Both eyes side by side, the left eye in the left half (A2B10G10R10).
  uint32_t stereo_width, stereo_height;
  VkImage stereo;
};

// The flags the client set (0: no export).
uint32_t rexui_GuestOutputExportFlags(void);
// Stereo (patch 0394): the eyes' distance in metres and the most a pixel may shift, a fraction of
// the picture's width.
void rexui_GuestOutputStereoParams(float* eye_distance, float* max_shift);
// Patch 0395: how much the scene layer is sharpened, 0 (not at all) to 1 (the most): a
// contrast-adaptive sharpening pass on the scene before it is handed over (the desktop window's
// FSR/CAS present effect never reaches the headset's picture).
float rexui_GuestOutputSharpen(void);
// Command processor thread, inside the guest output refresher: the layers of the frame being
// refreshed. `keep` (a const std::shared_ptr<void>*) holds their images while the frame is the
// newest or held by the client. The export forgets them after this refresh.
void rexui_SetNextGuestOutputLayers(const void* keep, const RexGuestOutputLayers* layers);
}
