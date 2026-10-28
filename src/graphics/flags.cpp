/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/graphics/flags.h>
#include <rex/logging.h>
#include <rex/ui/renderdoc_api.h>

// Default true as in Xenia.
REXCVAR_DEFINE_BOOL(gpu_allow_invalid_fetch_constants, true, "GPU",
                    "Allow invalid fetch constants");
REXCVAR_DEFINE_BOOL(native_2x_msaa, true, "GPU", "Enable native 2x MSAA");
REXCVAR_DEFINE_BOOL(depth_float24_round, false, "GPU", "Round float24 depth values");
REXCVAR_DEFINE_BOOL(depth_float24_convert_in_pixel_shader, false, "GPU",
                    "Convert float24 depth in pixel shader");
REXCVAR_DEFINE_BOOL(depth_transfer_not_equal_test, true, "GPU",
                    "Use not-equal test for depth transfer");
REXCVAR_DEFINE_BOOL(gamma_render_target_as_unorm16, true, "GPU",
                    "Use R16G16B16A16_UNORM for gamma render targets (more accurate than sRGB)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(dump_shaders, "", "GPU", "Path to dump shaders to");
REXCVAR_DEFINE_BOOL(use_fuzzy_alpha_epsilon, false, "GPU",
                    "Use approximate compare for alpha test values to prevent "
                    "flickering on NVIDIA graphics cards");
REXCVAR_DEFINE_BOOL(gpu_debug_markers, false, "GPU",
                    "Insert debug markers into GPU command streams for tools "
                    "like PIX and RenderDoc. Automatically enabled when "
                    "RenderDoc is detected.");

bool IsGpuDebugMarkersEnabled() {
  static bool cached = false;
  static bool result = false;
  if (!cached) {
    cached = true;
    if (REXCVAR_GET(gpu_debug_markers)) {
      result = true;
      REXLOG_INFO("GPU debug markers enabled via CVar");
    } else {
      auto renderdoc_api = rex::ui::RenderDocAPI::CreateIfConnected();
      if (renderdoc_api) {
        result = true;
        REXLOG_INFO("GPU debug markers auto-enabled (RenderDoc detected)");
      }
    }
  }
  return result;
}

REXCVAR_DEFINE_INT32(forza_native_width, 0, "GPU",
                     "FH1 active drawing width (0 uses integer scale)")
    .range(0, 8960)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(forza_native_height, 0, "GPU",
                     "FH1 active drawing height (0 uses integer scale)")
    .range(0, 5040)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Stable host configuration names for the FH1 presentation interface.
REXCVAR_DEFINE_BOOL(forza_present_16_9, false, "GPU",
                    "Use FH1's 16:9 source aspect before presentation");
REXCVAR_DEFINE_DOUBLE(forza_ultrawide_hud_x, 1.0, "GPU", "FH1 HUD X scale")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(forza_ultrawide_hud_y, 1.0, "GPU", "FH1 HUD Y scale")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(forza_ultrawide_hud_offset_x, 0.0, "GPU", "FH1 HUD X offset")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(forza_ultrawide_hud_offset_y, 0.0, "GPU", "FH1 HUD Y offset")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(forza_ultrawide_hud_edge_shift, 0.0, "GPU", "FH1 horizontal HUD edge-anchor distance")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
