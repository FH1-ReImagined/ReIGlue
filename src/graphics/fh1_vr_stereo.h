// FH1 (#333, patch 0396): VR true stereo's decisions, without the GPU, so tools/test_vr_stereo.cpp
// can check them. The command processor executes the frame's scene segment once per eye
// (command_processor.cpp Fh1Stereo*, vulkan/command_processor.cpp Fh1Stereo*): from the clear
// before the main view's scene to the first HUD draw. Here: which draws start and end that
// segment, which draws take an eye's offset, the offsets, each eye's part of the picture and the
// packets the second pass leaves out.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rex::graphics::fh1_vr_stereo {

// Xenos values used here (xenos.h): RB_MODECONTROL edram_mode, ColorRenderTargetFormat,
// CompareFunction, MsaaSamples, PrimitiveType.
inline constexpr uint32_t kModeColorDepth = 4, kModeDepthOnly = 5;
inline constexpr uint32_t kFormat_2_10_10_10 = 2, kFormat_2_10_10_10_AS_10_10_10_10 = 10;
inline constexpr uint32_t kCompareAlways = 7, kCompareGreaterEqual = 6;
inline constexpr uint32_t kMsaa1X = 0, kMsaa2X = 1, kMsaa4X = 2;
inline constexpr uint32_t kPrimitiveRectangleList = 8;

// A draw as the registers describe it before it is issued.
struct DrawState {
  uint32_t mode = 0;  // RB_MODECONTROL edram_mode
  uint32_t pitch = 0, msaa = kMsaa1X;  // RB_SURFACE_INFO
  uint32_t color_base = 0, color_format = 0;  // RB_COLOR_INFO
  uint32_t depth_base = 0;  // RB_DEPTH_INFO
  bool z_enable = false, z_write = false;
  uint32_t zfunc = 0;  // RB_DEPTHCONTROL
  bool window_space = false;  // PA_CL_VTE_CNTL vtx_xy_fmt
  bool projected = false;  // vtx_xy_fmt, vtx_z_fmt off, vtx_w0_fmt on, viewport x and y scale on
  uint32_t primitive = 0;  // VGT_DRAW_INITIATOR prim_type
  float viewport_x_scale = 0, viewport_y_scale = 0, viewport_x_offset = 0;  // PA_CL_VPORT_*
};

enum class Draw : uint8_t { kOther, kClear, kSegmentStart, kSegmentEnd };

// What the classification remembers within a frame.
struct Tracker {
  uint32_t depth_base = UINT32_MAX;  // the last main-view-sized clear's depth buffer
  bool composite_seen = false;  // the scene's composite into the front buffer, while recording
  uint32_t composite_base = 0;
};

// The frame (TU4, the cockpit): ... the scene's clear (a window-space rectangle writing depth
// everywhere, 640 x 4x MSAA = 1280 samples across) and its first draw (colour and reversed depth
// on that depth buffer, the 1280-wide viewport), ... post-processing, the composite into the
// front buffer (k_2_10_10_10, 1280, 1x), the first HUD draw into it (k_2_10_10_10_AS_10_10_10_10,
// the same base): the segment ends there. The depth pre-pass after its own clear draws depth only,
// so it does not start the segment.
inline Draw Classify(const DrawState& d, bool recording, Tracker& t) {
  if (d.mode != kModeColorDepth && d.mode != kModeDepthOnly) return Draw::kOther;
  if (recording) {
    if (d.pitch == 1280 && d.msaa == kMsaa1X && d.mode == kModeColorDepth) {
      if (d.color_format == kFormat_2_10_10_10) {
        t.composite_seen = true;
        t.composite_base = d.color_base;
      } else if (d.color_format == kFormat_2_10_10_10_AS_10_10_10_10 && t.composite_seen &&
                 d.color_base == t.composite_base) {
        return Draw::kSegmentEnd;
      }
    }
    return Draw::kOther;
  }
  const uint32_t across = d.pitch * (d.msaa == kMsaa4X ? 2 : 1);
  if (d.primitive == kPrimitiveRectangleList && d.window_space && d.z_enable && d.z_write &&
      d.zfunc == kCompareAlways && across == 1280) {
    t.depth_base = d.depth_base;
    return Draw::kClear;
  }
  if (d.mode == kModeColorDepth && d.pitch == 1280 && !d.window_space && d.z_enable &&
      d.z_write && d.zfunc == kCompareGreaterEqual && d.depth_base == t.depth_base &&
      std::fabs(d.viewport_x_scale) == 640.0f)
    return Draw::kSegmentStart;
  return Draw::kOther;
}

// Whether a draw while an eye is drawn takes the eye's offset: the scene's 3D draws (depth-tested
// on the scene's depth buffer, a projected position in the 16:9 viewport at full or quarter size).
// Full-screen passes, the depth pre-pass and the shadow mask (another depth buffer, before the
// segment anyway) stay as drawn.
inline bool TakesOffset(const DrawState& d, const Tracker& t) {
  if (!d.z_enable || d.zfunc == kCompareAlways || d.depth_base != t.depth_base || !d.projected)
    return false;
  const float x = std::fabs(d.viewport_x_scale), y = std::fabs(d.viewport_y_scale);
  return (x == 640.0f || x == 320.0f) && y * 16.0f == x * 9.0f;
}

// Each eye's offset, taken from the clip-space x of the scene's vertices. An eye d = half the eye
// distance to the side of the view's origin sees a point P00 * d / w across from where the middle
// sees it (a view-space point's W is 1, so the clip-space offset is P00 * d): near things move
// toward the other eye's side, to the right in the left eye. The game draws the main view
// mirrored across (its shadow passes' c0.x is -P00; post-processing turns the picture back): there
// the left eye's clip-space x grows (the offset taken is -P00 * d).
inline void EyeOffsets(float p00, float eye_distance, bool mirrored, float out[2]) {
  const float shift = p00 * 0.5f * eye_distance;
  out[0] = mirrored ? shift : -shift;
  out[1] = -out[0];
}

// The render target's columns [first, last) of a `width`-pixel picture for an eye that sees
// [lo, hi) of its width (fractions, left to right as shown): in the mirrored main view the
// picture's left is the render target's right.
inline void EyeColumns(float lo, float hi, bool mirrored, uint32_t width, uint32_t out[2]) {
  lo = std::clamp(lo, 0.0f, 1.0f);
  hi = std::clamp(hi, 0.0f, 1.0f);
  if (mirrored) {
    const float flipped_lo = 1.0f - hi;
    hi = 1.0f - lo;
    lo = flipped_lo;
  }
  out[0] = uint32_t(std::floor(lo * float(width)));
  out[1] = uint32_t(std::ceil(hi * float(width)));
}

// A scissor's columns limited to the eye's (an empty range when they do not meet).
inline void ClipColumns(uint32_t& offset, uint32_t& extent, const uint32_t columns[2]) {
  const uint32_t x0 = std::max(offset, columns[0]);
  const uint32_t x1 = std::min(offset + extent, columns[1]);
  offset = x0;
  extent = x1 > x0 ? x1 - x0 : 0;
}

// PM4 type-3 opcodes (xenos.h) the right eye's pass leaves out: those that signal or wait for the
// CPU (the left eye's pass did), occlusion queries, the swap, no-ops and indirect buffers (their
// packets are logged one by one as they run).
inline bool SkipsOpcode(uint32_t opcode) {
  switch (opcode) {
    case 0x3F:  // INDIRECT_BUFFER
    case 0x37:  // INDIRECT_BUFFER_PFD
    case 0x48:  // ME_INIT
    case 0x54:  // INTERRUPT
    case 0x64:  // XE_SWAP
    case 0x3C:  // WAIT_REG_MEM
    case 0x3E:  // REG_TO_MEM
    case 0x3D:  // MEM_WRITE
    case 0x45:  // COND_WRITE
    case 0x58:  // EVENT_WRITE_SHD
    case 0x5A:  // EVENT_WRITE_EXT
    case 0x5B:  // EVENT_WRITE_ZPD
    case 0x23:  // VIZ_QUERY
    case 0x26:  // WAIT_FOR_IDLE
    case 0x5E:  // CONTEXT_UPDATE
    case 0x10:  // NOP
      return true;
    default:
      return false;
  }
}

// A logged packet's length in words (header included) for type 0, 1 and 3 packets; 0 for type 2.
inline uint32_t PacketWords(uint32_t packet) {
  switch (packet >> 30) {
    case 0:
    case 3:
      return 1 + ((packet >> 16) & 0x3FFF) + 1;
    case 1:
      return 3;
    default:
      return 0;
  }
}

// LOAD_ALU_CONSTANT's register range as a type-0 packet header: its type (ALU, fetch, bool, loop,
// registers) and offset, `size` registers. 0 when it does not fit the register file.
inline uint32_t LoadedConstantsHeader(uint32_t offset_type, uint32_t size, uint32_t register_count,
                                      uint32_t* first_out) {
  static constexpr uint32_t kBase[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
  const uint32_t type = (offset_type >> 16) & 0xFF;
  if (type > 4 || !size || size > 0x4000) return 0;
  const uint32_t first = kBase[type] + (offset_type & 0x7FF);
  if (first + size > register_count) return 0;
  *first_out = first;
  return ((size - 1) << 16) | first;
}

}  // namespace rex::graphics::fh1_vr_stereo
