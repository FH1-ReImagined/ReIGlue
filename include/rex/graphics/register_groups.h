// FH1 (0670): the Xenos registers in groups of render state, for the pass ledger's change-mask
// counter (src/graphics/fh1_pass_ledger.h): per draw, which groups changed a value since the
// previous draw. A recorder that skips its per-draw work for unchanged groups (FH1's native
// renderer roadmap, "L1") saves at most what the unchanged share of each group says.
//
// Every register of the register file is in exactly one range below (tests/
// fh1_pass_ledger_test.cpp checks it). The fetch constants are one range; which of their bits
// are a texture's, a sampler's or a vertex buffer's depends on each slot's type, so the counter
// splits them per slot when it compares (FetchSamplerMask).
#pragma once

#include <cstdint>

namespace rex::graphics::register_groups {

enum Group : uint8_t {
  kSystem,         // outside the render state: control, MMIO, the CP's own registers
  kRenderTargets,  // surface, color and depth info, the mode, masks, MSAA
  kResolve,        // the copy (resolve) and clear registers
  kDynamicState,   // viewport, scissor, window offset, depth bias, blend color, stencil refs
  kPipelineState,  // depth, stencil, blend, raster, primitive and shader program state
  kDrawSetup,      // index buffer, initiators, bins: change with nearly every draw
  kOtherContext,   // the rest of the context registers
  kVertexConstants,
  kPixelConstants,
  kFetchConstants,  // compared per slot: textures, samplers or vertex buffers (below)
  kBoolLoopConstants,
  kRangeGroupCount,
  // Groups the counter reports from the fetch constants.
  kTextures = kRangeGroupCount,
  kSamplers,
  kVertexFetch,
  kGroupCount,
};

inline const char* GroupName(uint32_t group) {
  constexpr const char* kNames[kGroupCount] = {
      "system",     "render_targets", "resolve",          "dynamic_state",   "pipeline_state",
      "draw_setup", "other_context",  "vertex_constants", "pixel_constants", "fetch_constants",
      "bool_loop",  "textures",       "samplers",         "vertex_fetch"};
  return group < kGroupCount ? kNames[group] : "?";
}

struct Range {
  uint32_t first, last;  // inclusive
  Group group;
};

// In order, without gaps, from 0 to kRegisterEnd - 1 (RegisterFile::kRegisterCount).
constexpr uint32_t kRegisterEnd = 0x5003;
constexpr Range kRanges[] = {
    {0x0000, 0x1FFF, kSystem},
    {0x2000, 0x2005, kRenderTargets},  // RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, COLOR1-3
    {0x2006, 0x200D, kOtherContext},   // COHER_DEST_BASE_0-7
    {0x200E, 0x200F, kDynamicState},   // PA_SC_SCREEN_SCISSOR_TL/BR
    {0x2010, 0x207F, kOtherContext},
    {0x2080, 0x208B, kDynamicState},  // PA_SC_WINDOW_OFFSET, WINDOW_SCISSOR, CLIPRECT
    {0x208C, 0x20FF, kOtherContext},
    {0x2100, 0x2103, kDrawSetup},      // VGT_MAX/MIN_VTX_INDX, INDX_OFFSET, the reset index
    {0x2104, 0x2104, kRenderTargets},  // RB_COLOR_MASK
    {0x2105, 0x2108, kDynamicState},   // RB_BLEND_RED..ALPHA (blend constants)
    {0x2109, 0x210B, kOtherContext},   // RB_FOG_COLOR
    {0x210C, 0x210D, kDynamicState},   // RB_STENCILREFMASK_BF, RB_STENCILREFMASK
    {0x210E, 0x210E, kPipelineState},  // RB_ALPHA_REF
    {0x210F, 0x2114, kDynamicState},   // PA_CL_VPORT_*
    {0x2115, 0x217F, kOtherContext},
    {0x2180, 0x2184, kPipelineState},  // SQ_PROGRAM_CNTL, CONTEXT_MISC, INTERPOLATOR, WRAPPING
    {0x2185, 0x21F8, kOtherContext},   // SQ_PS/VS_PROGRAM, GFX_COPY_STATE among them
    {0x21F9, 0x21FD, kDrawSetup},      // VGT_EVENT_INITIATOR, DMA_BASE/SIZE, DRAW_INITIATOR, IMMED
    {0x21FE, 0x21FF, kOtherContext},
    {0x2200, 0x2202, kPipelineState},  // RB_DEPTHCONTROL, RB_BLENDCONTROL0, RB_COLORCONTROL
    {0x2203, 0x2203, kRenderTargets},  // RB_HIZCONTROL
    {0x2204, 0x2205, kPipelineState},  // PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL
    {0x2206, 0x2206, kDynamicState},   // PA_CL_VTE_CNTL
    {0x2207, 0x2207, kDrawSetup},      // VGT_CURRENT_BIN_ID_MIN
    {0x2208, 0x2208, kRenderTargets},  // RB_MODECONTROL
    {0x2209, 0x220B, kPipelineState},  // RB_BLENDCONTROL1-3
    {0x220C, 0x227F, kOtherContext},
    {0x2280, 0x2294, kPipelineState},  // points, lines, VGT output path, HOS, groups, VIZ query
    {0x2295, 0x22FF, kOtherContext},
    {0x2300, 0x2300, kPipelineState},  // PA_SC_LINE_CNTL
    {0x2301, 0x2301, kRenderTargets},  // PA_SC_AA_CONFIG
    {0x2302, 0x2302, kPipelineState},  // PA_SU_VTX_CNTL
    {0x2303, 0x2306, kDynamicState},   // PA_CL_GB_* clip and discard adjust
    {0x2307, 0x2311, kOtherContext},   // SQ_VS/PS_CONST, debug, unknown
    {0x2312, 0x2312, kRenderTargets},  // PA_SC_AA_MASK
    {0x2313, 0x2317, kOtherContext},
    {0x2318, 0x2325, kResolve},        // RB_COPY_*, the clears, the sample count
    {0x2326, 0x2326, kRenderTargets},  // RB_COLOR_DEST_MASK
    {0x2327, 0x237F, kOtherContext},
    {0x2380, 0x2383, kDynamicState},   // PA_SU_POLY_OFFSET_* (depth bias)
    {0x2384, 0x239F, kPipelineState},  // point sizes, the user clip planes
    {0x23A0, 0x3FFF, kOtherContext},
    {0x4000, 0x43FF, kVertexConstants},
    {0x4400, 0x47FF, kPixelConstants},
    {0x4800, 0x48BF, kFetchConstants},  // 32 slots of 6 words
    {0x48C0, 0x48FF, kSystem},
    {0x4900, 0x4927, kBoolLoopConstants},
    {0x4928, 0x5002, kSystem},
};

constexpr Group GroupOf(uint32_t index) {
  for (const Range& r : kRanges) {
    if (index >= r.first && index <= r.last)
      return r.group;
  }
  return kSystem;
}

// The context range the counter compares draw by draw (the rest of it is unused by the game).
constexpr uint32_t kContextFirst = 0x2000, kContextEnd = 0x2400;
constexpr uint32_t kConstantsFirst = 0x4000, kConstantsEnd = 0x4928;
constexpr uint32_t kFetchFirst = 0x4800, kFetchSlots = 32, kFetchSlotWords = 6;

// A texture fetch constant's sampler bits per word (xenos::xe_gpu_texture_fetch_t): the clamp
// modes; the filters, anisotropy and border size; all of word 4 (volume filters, mip levels,
// aniso walk, LOD bias, gradient bias); the border color, its W, tri clamp and aniso bias. The
// other bits are the texture's (format, address, size, swizzle, dimension, mips).
constexpr uint32_t kSamplerMask[kFetchSlotWords] = {0x0007FC00u, 0,           0,
                                                    0xFFF80000u, 0xFFFFFFFFu, 0x000001FFu};
// A slot's type (word 0, bits 0-1): 2 a texture, 3 a vertex fetch (three of two words each).
constexpr uint32_t kFetchTypeTexture = 2;

// Which groups a fetch slot's change touches: `old_words` and `new_words` are its 6 words (host
// order). Returns a mask of 1 << kTextures, kSamplers, kVertexFetch.
constexpr uint32_t FetchSlotChange(const uint32_t* old_words, const uint32_t* new_words) {
  uint32_t mask = 0;
  const bool texture = (new_words[0] & 3) == kFetchTypeTexture;
  for (uint32_t i = 0; i < kFetchSlotWords; ++i) {
    const uint32_t diff = old_words[i] ^ new_words[i];
    if (!diff)
      continue;
    if (!texture) {
      mask |= 1u << kVertexFetch;
    } else {
      if (diff & kSamplerMask[i])
        mask |= 1u << kSamplers;
      if (diff & ~kSamplerMask[i])
        mask |= 1u << kTextures;
    }
  }
  return mask;
}

}  // namespace rex::graphics::register_groups
