/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/Include/Types.h>
#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/util/fh1_hud.h>
#include <rex/graphics/util/presentation_transform.h>
#include <rex/ui/vulkan/image_compute.h>
#include <rex/ui/vulkan/copy_target.h>
#include <rex/graphics/util/scene_capture.h>
#include <rex/graphics/util/draw_trace_budget.h>
#include "fh1_device_memory.h"
#include "fh1_render_target_trim.h"
#include "fh1_pipeline_lookahead.h"
#include "fh1_submission_watch.h"
#include <rex/platform.h>
#include "../fh1_frame_cost.h"
#if REX_PLATFORM_MAC
#include <mach/mach.h>
#endif
#if REX_PLATFORM_LINUX
#include <unistd.h>
#endif
#include "fh1_readback_history.h"
REXCVAR_DECLARE(bool, vulkan_render_target_update_reuse_verify);
REXCVAR_DECLARE(int32_t, forza_native_width);
REXCVAR_DECLARE(int32_t, forza_native_height);
REXCVAR_DECLARE(bool, forza_present_16_9);
REXCVAR_DECLARE(double, forza_ultrawide_hud_edge_shift);
REXCVAR_DECLARE(double, forza_ultrawide_hud_x);
REXCVAR_DECLARE(double, forza_ultrawide_hud_y);
REXCVAR_DECLARE(double, forza_ultrawide_hud_offset_x);
REXCVAR_DECLARE(double, forza_ultrawide_hud_offset_y);
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/util/fh1_jitter.h>
#include <rex/graphics/vulkan/fh1_dlss.h>
#include <rex/graphics/vulkan/fh1_fsr3.h>
#include <rex/graphics/pipeline_prebuild_file.h>
#include <rex/graphics/vulkan/pipeline_cache.h>
#include <rex/graphics/vulkan/render_target_cache.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/types.h>
#include <rex/memory/utils.h>
#include <rex/ui/flags.h>
#include <rex/ui/vulkan/guest_output_export.h>

#include "fh1_world_ao.h"
#include "../fh1_vr_stereo.h"

#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

// Legacy backend compatibility aliases for shared readback controls.
REXCVAR_DEFINE_BOOL(shader_prebuild_only, false, "GPU/Vulkan",
                    "Create the stored and prebuild-listed pipelines, save the driver's cache "
                    "and exit before the game starts (the launcher's Prebuild shaders step)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_fast_resolve_copy, true, "GPU/Vulkan",
    "Use bit-exact whole-word FH1 cross-scale resolve copies")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(fh1_scene_720p_native_hud, false, "GPU/Vulkan",
                    "Experimental FH1: 720p world with a separate native 1440p HUD")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// One immutable request prevents the GPU observing half of a settings change.
REXCVAR_DEFINE_UINT32(fh1_native_scene_scale, 1, "GPU/Vulkan", "Native scene scale with an independent HUD")
    .range(1, 3).lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(fh1_graphics_request, "", "GPU/Vulkan", "FH1 live graphics request")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(fh1_graphics_result, "", "GPU/Vulkan", "FH1 live graphics result")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(fh1_upscaler, "off", "GPU/Vulkan", "FH1 scene reconstruction: off, smaa; NVIDIA DLSS quality, balanced, performance (dlss), dlaa; AMD FSR 3 fsr3_quality, fsr3_balanced, fsr3_performance, fsr3_native; Apple metalfx_temporal, metalfx_spatial").lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(fh1_output_width, 2560, "GPU/Vulkan", "FH1 reconstruction output width").lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(fh1_output_height, 1440, "GPU/Vulkan", "FH1 reconstruction output height")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(vulkan_readback_resolve, false, "GPU/Vulkan",
                    "Read render-to-texture results on the CPU")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_readback_memexport, false, "GPU/Vulkan",
                    "Read data written by memory export in shaders on the CPU")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_async_skip_incomplete_frames, true, "GPU/Vulkan",
                    "When async shader compilation is enabled, skip presenting frames that "
                    "used placeholder pipelines to avoid visible flashing")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(vulkan_async_skip_incomplete_frames_ms, 100, "GPU/Vulkan",
                      "How long vulkan_async_skip_incomplete_frames holds frames back while the "
                      "pipelines they need are built, in milliseconds; after that they are shown "
                      "without the object draws whose pipelines are not ready yet (a frame that "
                      "misses a full-screen, depth or stencil, front-buffer or HUD draw is held "
                      "until it is built; 0: every frame until all are)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(vulkan_async_frame_end_wait_ms, 33, "GPU/Vulkan",
                     "Asynchronous pipeline creation: how long one frame's submission ends may "
                     "wait for the pipelines being built, in milliseconds, all of them together "
                     "(0: not at all; -1: every submission end waits until all are built, the "
                     "queued ones on the command processor, however long the driver takes)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_submit_on_primary_buffer_end, true, "GPU/Vulkan",
                    "Submit command buffer when PM4 primary buffer ends")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_threaded_submission, true, "GPU/Vulkan",
                    "Record the Vulkan command buffers from the deferred commands and submit "
                    "them on a thread of their own instead of the GPU command processor's")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(vulkan_max_frames_in_flight, 0, "GPU/Vulkan",
                     "Maximum queued frames (1-3); nonpositive uses 2 on NVIDIA, 3 otherwise")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(vulkan_hoist_shared_memory_uploads, true, "GPU/Vulkan",
                    "Record shared memory uploads (vertex and index data the CPU rewrote) "
                    "before the current render pass when no draw in it has read the uploaded "
                    "pages, instead of ending the render pass for every upload")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_readback_skip_render_targets, true, "GPU/Vulkan",
                    "readback_resolve = some: do not copy back again a resolve that was made in "
                    "150 frames in a row (a scene render target) when it comes back after its "
                    "readback buffer was dropped (after the map or a menu)")
    .debug_only();

REXCVAR_DEFINE_STRING(fh1_frame_cost_csv, "", "GPU/Vulkan",
                      "Testing: write what the command processor did in every frame (draws, "
                      "textures, uploads, pipelines, render targets, waits) to this CSV file")
    .debug_only();

REXCVAR_DEFINE_INT32(vulkan_submission_draw_limit, -1, "GPU/Vulkan",
                     "End a GPU submission after this many draws (a frame then reaches the GPU in "
                     "several parts, so no single part can run into Windows' 2 s GPU timeout): "
                     "-1 = 1000 on integrated GPUs with a small carve-out, else none; 0 = none")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

REXCVAR_DEFINE_UINT32(vulkan_render_target_trim_percent, 90, "GPU/Vulkan",
                      "Free the cached render targets that hold no EDRAM data when device-local "
                      "memory use reaches this percentage of the driver's budget "
                      "(VK_EXT_memory_budget), or after a device-local allocation failed; 0 "
                      "keeps every render target for the session")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(gpu_plain_register_ranges, true, "GPU/Vulkan",
                    "Write a range of render state registers that only store their value (no "
                    "side effect, outside the shader constants) in the command processor's "
                    "packet loop, without the backend's register write calls (read at each frame "
                    "start)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(gpu_shader_translation_memo, true, "GPU/Vulkan",
                    "Each shader answers a draw that needs the same translation (modification) "
                    "as the last draw that used it without looking its translations up (read at "
                    "each frame start)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(gpu_shader_translation_memo_verify, false, "GPU/Vulkan",
                    "With gpu_shader_translation_memo, still look each translation up and log a "
                    "difference (testing)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_dynamic_rendering, true, "GPU/Vulkan",
                    "Use VK_KHR_dynamic_rendering for Vulkan GPU emulation when supported by the "
                    "device (falls back to render passes otherwise)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// The existing opt-in one-frame packet tracer owns and closes this stream.
namespace rex::graphics::fh1_frame_trace {
extern FILE* file;
extern uint32_t draws;
}

namespace rex::graphics::vulkan {

namespace {

bool Fh1NativeEnabled() {
  return REXCVAR_GET(forza_native_width) > 0 && REXCVAR_GET(forza_native_height) > 0;
}

// FH1 reuses screen-pitched EDRAM for reflection faces and bloom intermediates.
// A pitch match alone does not make a draw part of the native display.
bool Fh1ScreenViewport(const RegisterFile& regs) {
  const auto pitch = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch;
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  const auto tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
  const auto br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  return fh1::IsNativeScreenViewport(pitch, vte.vport_x_scale_ena, vte.vport_y_scale_ena,
                                     regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE),
                                     regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE),
                                     br.br_x - tl.tl_x, br.br_y - tl.tl_y);
}

// glslang default built-in resource limits.
constexpr TBuiltInResource kGlslangDefaultTBuiltInResource = {
    /* .maxLights = */ 32,
    /* .maxClipPlanes = */ 6,
    /* .maxTextureUnits = */ 32,
    /* .maxTextureCoords = */ 32,
    /* .maxVertexAttribs = */ 64,
    /* .maxVertexUniformComponents = */ 4096,
    /* .maxVaryingFloats = */ 64,
    /* .maxVertexTextureImageUnits = */ 32,
    /* .maxCombinedTextureImageUnits = */ 80,
    /* .maxTextureImageUnits = */ 32,
    /* .maxFragmentUniformComponents = */ 4096,
    /* .maxDrawBuffers = */ 32,
    /* .maxVertexUniformVectors = */ 128,
    /* .maxVaryingVectors = */ 8,
    /* .maxFragmentUniformVectors = */ 16,
    /* .maxVertexOutputVectors = */ 16,
    /* .maxFragmentInputVectors = */ 15,
    /* .minProgramTexelOffset = */ -8,
    /* .maxProgramTexelOffset = */ 7,
    /* .maxClipDistances = */ 8,
    /* .maxComputeWorkGroupCountX = */ 65535,
    /* .maxComputeWorkGroupCountY = */ 65535,
    /* .maxComputeWorkGroupCountZ = */ 65535,
    /* .maxComputeWorkGroupSizeX = */ 1024,
    /* .maxComputeWorkGroupSizeY = */ 1024,
    /* .maxComputeWorkGroupSizeZ = */ 64,
    /* .maxComputeUniformComponents = */ 1024,
    /* .maxComputeTextureImageUnits = */ 16,
    /* .maxComputeImageUniforms = */ 8,
    /* .maxComputeAtomicCounters = */ 8,
    /* .maxComputeAtomicCounterBuffers = */ 1,
    /* .maxVaryingComponents = */ 60,
    /* .maxVertexOutputComponents = */ 64,
    /* .maxGeometryInputComponents = */ 64,
    /* .maxGeometryOutputComponents = */ 128,
    /* .maxFragmentInputComponents = */ 128,
    /* .maxImageUnits = */ 8,
    /* .maxCombinedImageUnitsAndFragmentOutputs = */ 8,
    /* .maxCombinedShaderOutputResources = */ 8,
    /* .maxImageSamples = */ 0,
    /* .maxVertexImageUniforms = */ 0,
    /* .maxTessControlImageUniforms = */ 0,
    /* .maxTessEvaluationImageUniforms = */ 0,
    /* .maxGeometryImageUniforms = */ 0,
    /* .maxFragmentImageUniforms = */ 8,
    /* .maxCombinedImageUniforms = */ 8,
    /* .maxGeometryTextureImageUnits = */ 16,
    /* .maxGeometryOutputVertices = */ 256,
    /* .maxGeometryTotalOutputComponents = */ 1024,
    /* .maxGeometryUniformComponents = */ 1024,
    /* .maxGeometryVaryingComponents = */ 64,
    /* .maxTessControlInputComponents = */ 128,
    /* .maxTessControlOutputComponents = */ 128,
    /* .maxTessControlTextureImageUnits = */ 16,
    /* .maxTessControlUniformComponents = */ 1024,
    /* .maxTessControlTotalOutputComponents = */ 4096,
    /* .maxTessEvaluationInputComponents = */ 128,
    /* .maxTessEvaluationOutputComponents = */ 128,
    /* .maxTessEvaluationTextureImageUnits = */ 16,
    /* .maxTessEvaluationUniformComponents = */ 1024,
    /* .maxTessPatchComponents = */ 120,
    /* .maxPatchVertices = */ 32,
    /* .maxTessGenLevel = */ 64,
    /* .maxViewports = */ 16,
    /* .maxVertexAtomicCounters = */ 0,
    /* .maxTessControlAtomicCounters = */ 0,
    /* .maxTessEvaluationAtomicCounters = */ 0,
    /* .maxGeometryAtomicCounters = */ 0,
    /* .maxFragmentAtomicCounters = */ 8,
    /* .maxCombinedAtomicCounters = */ 8,
    /* .maxAtomicCounterBindings = */ 1,
    /* .maxVertexAtomicCounterBuffers = */ 0,
    /* .maxTessControlAtomicCounterBuffers = */ 0,
    /* .maxTessEvaluationAtomicCounterBuffers = */ 0,
    /* .maxGeometryAtomicCounterBuffers = */ 0,
    /* .maxFragmentAtomicCounterBuffers = */ 1,
    /* .maxCombinedAtomicCounterBuffers = */ 1,
    /* .maxAtomicCounterBufferSize = */ 16384,
    /* .maxTransformFeedbackBuffers = */ 4,
    /* .maxTransformFeedbackInterleavedComponents = */ 64,
    /* .maxCullDistances = */ 8,
    /* .maxCombinedClipAndCullDistances = */ 8,
    /* .maxSamples = */ 4,
    /* .maxMeshOutputVerticesNV = */ 256,
    /* .maxMeshOutputPrimitivesNV = */ 512,
    /* .maxMeshWorkGroupSizeX_NV = */ 32,
    /* .maxMeshWorkGroupSizeY_NV = */ 1,
    /* .maxMeshWorkGroupSizeZ_NV = */ 1,
    /* .maxTaskWorkGroupSizeX_NV = */ 32,
    /* .maxTaskWorkGroupSizeY_NV = */ 1,
    /* .maxTaskWorkGroupSizeZ_NV = */ 1,
    /* .maxMeshViewCountNV = */ 4,
    /* .maxDualSourceDrawBuffersEXT = */ 1,
    /* .limits = */
    {
        /* .nonInductiveForLoops = */ 1,
        /* .whileLoops = */ 1,
        /* .doWhileLoops = */ 1,
        /* .generalUniformIndexing = */ 1,
        /* .generalAttributeMatrixVectorIndexing = */ 1,
        /* .generalVaryingIndexing = */ 1,
        /* .generalSamplerIndexing = */ 1,
        /* .generalVariableIndexing = */ 1,
        /* .generalConstantMatrixVectorIndexing = */ 1,
    },
};

const char* GetSwapFxaaComputeSource(bool extreme_quality) {
  return extreme_quality ? R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_fxaa_size;
  vec2 xe_fxaa_size_inv;
};

layout(set = 0, binding = 0) uniform sampler2D xe_fxaa_source;
layout(set = 1, binding = 0, rgb10_a2) writeonly uniform image2D xe_fxaa_dest;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);
const float kEdgeThreshold = 0.063;
const float kEdgeThresholdMin = 0.0312;
const float kSpanMax = 12.0;
const float kDirReduceMul = 0.125;
const float kDirReduceMin = 1.0 / 128.0;

float SampleLuma(vec2 uv) {
  return textureLod(xe_fxaa_source, uv, 0.0).a;
}

void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel, xe_fxaa_size))) {
    return;
  }

  vec2 uv = (vec2(pixel) + vec2(0.5)) * xe_fxaa_size_inv;
  vec2 texel = xe_fxaa_size_inv;
  vec4 rgbm = textureLod(xe_fxaa_source, uv, 0.0);

  float lumaM = rgbm.a;
  float lumaNW = SampleLuma(uv + vec2(-texel.x, -texel.y));
  float lumaNE = SampleLuma(uv + vec2( texel.x, -texel.y));
  float lumaSW = SampleLuma(uv + vec2(-texel.x,  texel.y));
  float lumaSE = SampleLuma(uv + vec2( texel.x,  texel.y));

  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
  float lumaRange = lumaMax - lumaMin;

  vec3 result = rgbm.rgb;
  if (lumaRange >= max(kEdgeThresholdMin, lumaMax * kEdgeThreshold)) {
    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max(
        (lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * kDirReduceMul),
        kDirReduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-kSpanMax), vec2(kSpanMax)) * texel;

    vec3 rgbA =
        0.5 *
        (textureLod(xe_fxaa_source, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
         textureLod(xe_fxaa_source, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    vec3 rgbB =
        rgbA * 0.5 +
        0.25 *
            (textureLod(xe_fxaa_source, uv + dir * -0.5, 0.0).rgb +
             textureLod(xe_fxaa_source, uv + dir * 0.5, 0.0).rgb);
    float lumaB = dot(rgbB, kLumaWeights);
    result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
  }

  imageStore(xe_fxaa_dest, ivec2(pixel), vec4(result, 1.0));
}
)"
                         : R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_fxaa_size;
  vec2 xe_fxaa_size_inv;
};

layout(set = 0, binding = 0) uniform sampler2D xe_fxaa_source;
layout(set = 1, binding = 0, rgb10_a2) writeonly uniform image2D xe_fxaa_dest;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);
const float kEdgeThreshold = 0.166;
const float kEdgeThresholdMin = 0.0833;
const float kSpanMax = 8.0;
const float kDirReduceMul = 0.125;
const float kDirReduceMin = 1.0 / 128.0;

float SampleLuma(vec2 uv) {
  return textureLod(xe_fxaa_source, uv, 0.0).a;
}

void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel, xe_fxaa_size))) {
    return;
  }

  vec2 uv = (vec2(pixel) + vec2(0.5)) * xe_fxaa_size_inv;
  vec2 texel = xe_fxaa_size_inv;
  vec4 rgbm = textureLod(xe_fxaa_source, uv, 0.0);

  float lumaM = rgbm.a;
  float lumaNW = SampleLuma(uv + vec2(-texel.x, -texel.y));
  float lumaNE = SampleLuma(uv + vec2( texel.x, -texel.y));
  float lumaSW = SampleLuma(uv + vec2(-texel.x,  texel.y));
  float lumaSE = SampleLuma(uv + vec2( texel.x,  texel.y));

  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
  float lumaRange = lumaMax - lumaMin;

  vec3 result = rgbm.rgb;
  if (lumaRange >= max(kEdgeThresholdMin, lumaMax * kEdgeThreshold)) {
    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max(
        (lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * kDirReduceMul),
        kDirReduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-kSpanMax), vec2(kSpanMax)) * texel;

    vec3 rgbA =
        0.5 *
        (textureLod(xe_fxaa_source, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
         textureLod(xe_fxaa_source, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    vec3 rgbB =
        rgbA * 0.5 +
        0.25 *
            (textureLod(xe_fxaa_source, uv + dir * -0.5, 0.0).rgb +
             textureLod(xe_fxaa_source, uv + dir * 0.5, 0.0).rgb);
    float lumaB = dot(rgbB, kLumaWeights);
    result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
  }

  imageStore(xe_fxaa_dest, ivec2(pixel), vec4(result, 1.0));
}
)";
}

const char* GetSwapApplyGammaTablePixelRbSwapSource() {
  return R"(#version 450
layout(set = 0, binding = 0) uniform textureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;

layout(location = 0) out vec4 xe_apply_gamma_color;

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 255.0 + vec3(0.5)).bgr;
  xe_apply_gamma_color = vec4(
      texelFetch(xe_apply_gamma_ramp, int(source.r)).b,
      texelFetch(xe_apply_gamma_ramp, int(source.g)).g,
      texelFetch(xe_apply_gamma_ramp, int(source.b)).r,
      1.0);
}
)";
}

const char* GetSwapApplyGammaPwlPixelRbSwapSource() {
  return R"(#version 450
layout(set = 0, binding = 0) uniform utextureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;

layout(location = 0) out vec4 xe_apply_gamma_color;

float ApplyPwl(uint source_value_10b, uint channel) {
  uint source_value_base = source_value_10b >> 3u;
  uvec4 pwl = texelFetch(xe_apply_gamma_ramp, int(source_value_base * 3u + channel));
  float value = float(pwl.x) + float((source_value_10b & 7u) * pwl.y) * 0.125;
  return clamp(value * 1.52737048e-05, 0.0, 1.0);
}

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 1023.0 + vec3(0.5)).bgr;
  xe_apply_gamma_color = vec4(
      ApplyPwl(source.r, 0u),
      ApplyPwl(source.g, 1u),
      ApplyPwl(source.b, 2u),
      1.0);
}
)";
}

const char* GetSwapApplyGammaTableComputeRbSwapSource() {
  return R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_apply_gamma_size;
};

layout(set = 0, binding = 0) uniform textureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;
layout(set = 2, binding = 0, rgb10_a2) writeonly uniform image2D xe_apply_gamma_dest;

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), xe_apply_gamma_size))) {
    return;
  }

  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 255.0 + vec3(0.5)).bgr;
  imageStore(
      xe_apply_gamma_dest, pixel,
      vec4(texelFetch(xe_apply_gamma_ramp, int(source.r)).b,
           texelFetch(xe_apply_gamma_ramp, int(source.g)).g,
           texelFetch(xe_apply_gamma_ramp, int(source.b)).r, 1.0));
}
)";
}

const char* GetSwapApplyGammaPwlComputeRbSwapSource() {
  return R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_apply_gamma_size;
};

layout(set = 0, binding = 0) uniform utextureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;
layout(set = 2, binding = 0, rgb10_a2) writeonly uniform image2D xe_apply_gamma_dest;

float ApplyPwl(uint source_value_10b, uint channel) {
  uint source_value_base = source_value_10b >> 3u;
  uvec4 pwl = texelFetch(xe_apply_gamma_ramp, int(source_value_base * 3u + channel));
  float value = float(pwl.x) + float((source_value_10b & 7u) * pwl.y) * 0.125;
  return clamp(value * 1.52737048e-05, 0.0, 1.0);
}

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), xe_apply_gamma_size))) {
    return;
  }

  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 1023.0 + vec3(0.5)).bgr;
  imageStore(xe_apply_gamma_dest, pixel,
             vec4(ApplyPwl(source.r, 0u), ApplyPwl(source.g, 1u),
                  ApplyPwl(source.b, 2u), 1.0));
}
)";
}

const char* GetSwapApplyGammaTableFxaaLumaComputeRbSwapSource() {
  return R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_apply_gamma_size;
};

layout(set = 0, binding = 0) uniform textureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;
layout(set = 2, binding = 0, rgba16f) writeonly uniform image2D xe_apply_gamma_dest;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), xe_apply_gamma_size))) {
    return;
  }

  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 255.0 + vec3(0.5)).bgr;
  vec3 rgb = vec3(texelFetch(xe_apply_gamma_ramp, int(source.r)).b,
                  texelFetch(xe_apply_gamma_ramp, int(source.g)).g,
                  texelFetch(xe_apply_gamma_ramp, int(source.b)).r);
  imageStore(xe_apply_gamma_dest, pixel, vec4(rgb, dot(rgb, kLumaWeights)));
}
)";
}

const char* GetSwapApplyGammaPwlFxaaLumaComputeRbSwapSource() {
  return R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_apply_gamma_size;
};

layout(set = 0, binding = 0) uniform utextureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;
layout(set = 2, binding = 0, rgba16f) writeonly uniform image2D xe_apply_gamma_dest;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);

float ApplyPwl(uint source_value_10b, uint channel) {
  uint source_value_base = source_value_10b >> 3u;
  uvec4 pwl = texelFetch(xe_apply_gamma_ramp, int(source_value_base * 3u + channel));
  float value = float(pwl.x) + float((source_value_10b & 7u) * pwl.y) * 0.125;
  return clamp(value * 1.52737048e-05, 0.0, 1.0);
}

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), xe_apply_gamma_size))) {
    return;
  }

  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 1023.0 + vec3(0.5)).bgr;
  vec3 rgb = vec3(ApplyPwl(source.r, 0u), ApplyPwl(source.g, 1u),
                  ApplyPwl(source.b, 2u));
  imageStore(xe_apply_gamma_dest, pixel, vec4(rgb, dot(rgb, kLumaWeights)));
}
)";
}

bool CompileGlslToSpirvInternal(EShLanguage stage, std::string_view source,
                                std::vector<uint32_t>& spirv_out, std::string& error_out) {
  static std::once_flag glslang_initialize_once;
  std::call_once(glslang_initialize_once, []() { glslang::InitializeProcess(); });

  const char* source_c_str = source.data();
  glslang::TShader shader(stage);
  shader.setStrings(&source_c_str, 1);
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

  EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(&kGlslangDefaultTBuiltInResource, 450, false, messages)) {
    error_out = "glslang shader parse failed";
    if (const char* shader_log = shader.getInfoLog();
        shader_log != nullptr && shader_log[0] != '\0') {
      error_out += ": ";
      error_out += shader_log;
    }
    return false;
  }

  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    error_out = "glslang program link failed";
    if (const char* program_log = program.getInfoLog();
        program_log != nullptr && program_log[0] != '\0') {
      error_out += ": ";
      error_out += program_log;
    }
    return false;
  }

  const glslang::TIntermediate* intermediate = program.getIntermediate(stage);
  if (intermediate == nullptr) {
    error_out = "glslang produced no stage intermediate";
    return false;
  }

  glslang::SpvOptions spv_options = {};
  spv_options.disableOptimizer = true;
  spv_options.optimizeSize = false;
  glslang::GlslangToSpv(*intermediate, spirv_out, &spv_options);
  if (spirv_out.empty()) {
    error_out = "glslang produced empty SPIR-V";
    return false;
  }
  return true;
}

// A host wait for the GPU longer than this is logged: the queue made no progress for that
// long (presentation holding it, for instance).
constexpr int64_t kSlowSubmissionWaitMs = 2000;

// FH1 (#268): the submissions' draws and the slow waits for them (fh1_submission_watch.h).
// One command processor per process; its thread alone touches these.
fh1_submission_watch::Watch fh1_submission_watch_state;
uint32_t fh1_draws_in_submission = 0;
uint64_t fh1_submission_splits = 0;

// FORZA_TEST_SLOW_WAIT_MS=<n>: log waits for the GPU from n ms (at most one every 10 s), to
// see the submissions' draws on a fast GPU.
struct Fh1SlowWaitTestHook {
  Fh1SlowWaitTestHook() {
    if (const char* v = std::getenv("FORZA_TEST_SLOW_WAIT_MS"); v && *v) {
      fh1_submission_watch_state.SetSlowWaitMs(std::max<int64_t>(0, std::atoll(v)));
    }
  }
} fh1_slow_wait_test_hook;

int64_t Fh1NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Test hook: FORZA_TEST_GPU_LOSS_AT_FRAME=<n> makes the submission fence check report
// VK_ERROR_DEVICE_LOST from frame n on, to exercise the device-loss reporting path.
uint64_t TestGpuLossFrame() {
  static const uint64_t frame = [] {
    const char* value = std::getenv("FORZA_TEST_GPU_LOSS_AT_FRAME");
    return value && *value ? uint64_t(std::strtoull(value, nullptr, 10)) : uint64_t(0);
  }();
  return frame;
}

}  // namespace

bool CompileFh1Compute(std::string_view source, std::vector<uint32_t>& spirv, std::string& error) {
  return CompileGlslToSpirvInternal(EShLangCompute, source, spirv, error);
}

bool CompileFh1ComputeReflected(std::string_view source, std::string_view preamble,
                                const Fh1ShaderInclude& include, std::vector<uint32_t>& spirv,
                                std::vector<Fh1ShaderBinding>& bindings, std::string& error) {
  static std::once_flag glslang_initialize_once;
  std::call_once(glslang_initialize_once, []() { glslang::InitializeProcess(); });
  class Includer : public glslang::TShader::Includer {
   public:
    explicit Includer(const Fh1ShaderInclude& include) : include_(include) {}
    IncludeResult* includeLocal(const char* name, const char* includer, size_t) override {
      std::string resolved;
      std::string_view text;
      if (!include_(name, includer ? includer : "", resolved, text)) return nullptr;
      return new IncludeResult(resolved, text.data(), text.size(), nullptr);
    }
    IncludeResult* includeSystem(const char* name, const char* includer, size_t depth) override {
      return includeLocal(name, includer, depth);
    }
    void releaseInclude(IncludeResult* result) override { delete result; }

   private:
    const Fh1ShaderInclude& include_;
  } includer(include);
  const std::string preamble_text(preamble);
  const char* source_c_str = source.data();
  const int source_length = int(source.size());
  glslang::TShader shader(EShLangCompute);
  shader.setStringsWithLengths(&source_c_str, &source_length, 1);
  shader.setPreamble(preamble_text.c_str());
  shader.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 450);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
  const EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(&kGlslangDefaultTBuiltInResource, 450, false, messages, includer)) {
    error = std::string("parse failed: ") + shader.getInfoLog();
    return false;
  }
  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages) || !program.buildReflection()) {
    error = std::string("link failed: ") + program.getInfoLog();
    return false;
  }
  bindings.clear();
  for (int i = 0; i < program.getNumUniformVariables(); ++i) {
    const auto& uniform = program.getUniform(i);
    const glslang::TType* type = uniform.getType();
    if (!type || type->getBasicType() != glslang::EbtSampler || uniform.getBinding() < 0) continue;
    bindings.push_back({uniform.name, uint32_t(uniform.getBinding()), false});
  }
  for (int i = 0; i < program.getNumUniformBlocks(); ++i) {
    const auto& block = program.getUniformBlock(i);
    if (block.getBinding() >= 0) bindings.push_back({block.name, uint32_t(block.getBinding()), true});
  }
  glslang::SpvOptions options = {};
  options.disableOptimizer = true;
  spirv.clear();
  glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), spirv, &options);
  if (spirv.empty()) {
    error = "glslang produced empty SPIR-V";
    return false;
  }
  return true;
}

// Generated with `xb buildshaders`.
namespace shaders {
#include "../shaders/vulkan_spirv/apply_gamma_pwl_cs.h"
#include "../shaders/vulkan_spirv/apply_gamma_pwl_fxaa_luma_ps.h"
#include "../shaders/vulkan_spirv/apply_gamma_pwl_fxaa_luma_cs.h"
#include "../shaders/vulkan_spirv/apply_gamma_pwl_ps.h"
#include "../shaders/vulkan_spirv/apply_gamma_table_cs.h"
#include "../shaders/vulkan_spirv/apply_gamma_table_fxaa_luma_ps.h"
#include "../shaders/vulkan_spirv/apply_gamma_table_fxaa_luma_cs.h"
#include "../shaders/vulkan_spirv/apply_gamma_table_ps.h"
#include "../shaders/vulkan_spirv/fullscreen_cw_vs.h"
#include "../shaders/vulkan_spirv/hdr_hud_mask_cs.h"
#include "../shaders/vulkan_spirv/resolve_downscale_cs.h"
}  // namespace shaders

const VkDescriptorPoolSize VulkanCommandProcessor::kDescriptorPoolSizeUniformBuffer = {
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    SpirvShaderTranslator::kConstantBufferCount* kLinkedTypeDescriptorPoolSetCount};

const VkDescriptorPoolSize VulkanCommandProcessor::kDescriptorPoolSizeStorageBuffer = {
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kLinkedTypeDescriptorPoolSetCount};

const VkDescriptorPoolSize VulkanCommandProcessor::kDescriptorPoolSizeStorageImage = {
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kLinkedTypeDescriptorPoolSetCount};

// 2x descriptors for texture images because of unsigned and signed bindings.
const VkDescriptorPoolSize VulkanCommandProcessor::kDescriptorPoolSizeTextures[2] = {
    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * kLinkedTypeDescriptorPoolSetCount},
    {VK_DESCRIPTOR_TYPE_SAMPLER, kLinkedTypeDescriptorPoolSetCount},
};

VulkanCommandProcessor::VulkanCommandProcessor(VulkanGraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state),
      deferred_command_buffer_(*this),
      transient_descriptor_allocator_uniform_buffer_(
          static_cast<const ui::vulkan::VulkanProvider*>(graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeUniformBuffer, 1, kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_storage_buffer_(
          static_cast<const ui::vulkan::VulkanProvider*>(graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeStorageBuffer, 1, kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_storage_image_(
          static_cast<const ui::vulkan::VulkanProvider*>(graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeStorageImage, 1, kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_textures_(
          static_cast<const ui::vulkan::VulkanProvider*>(graphics_system->provider())
              ->vulkan_device(),
          kDescriptorPoolSizeTextures, uint32_t(rex::countof(kDescriptorPoolSizeTextures)),
          kLinkedTypeDescriptorPoolSetCount) {
  legacy_readback_memexport_cvar_name_ = "vulkan_readback_memexport";
  fh1_vulkan_device_ =
      static_cast<const ui::vulkan::VulkanProvider*>(graphics_system->provider())->vulkan_device();
}

VulkanCommandProcessor::~VulkanCommandProcessor() = default;

namespace {

// FH1 (#45, #400): the command processor thread's CPU time, in ms since it started, from a
// precise clock and a coarse one (negative where there is none; fh1_frame_cost.h CpuClock
// decides which one times the frames). Linux and macOS: CLOCK_THREAD_CPUTIME_ID, no coarse one.
// Windows: the thread's cycles (QueryThreadCycleTime) at the time stamp counter's rate, measured
// once for 2 ms against QueryPerformanceCounter, only with an invariant time stamp counter
// (CPUID 0x80000007 EDX bit 8); and GetThreadTimes, which CpuClock checks the cycles against.
double ThreadCpuPreciseMs() {
#if REX_PLATFORM_WIN32
#if REX_ARCH_AMD64
  static const double cycles_per_ms = [] {
    int regs[4] = {};
    __cpuid(regs, int(0x80000000u));
    const uint32_t max_extended_leaf = uint32_t(regs[0]);
    uint32_t edx = 0;
    if (max_extended_leaf >= 0x80000007u) {
      __cpuid(regs, int(0x80000007u));
      edx = uint32_t(regs[3]);
    }
    if (!fh1_frame_cost::InvariantTsc(max_extended_leaf, edx)) return 0.0;
    LARGE_INTEGER frequency, first, last;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&first)) return 0.0;
    const uint64_t tsc_first = __rdtsc();
    do {
      QueryPerformanceCounter(&last);
    } while (last.QuadPart - first.QuadPart < frequency.QuadPart / 500);
    const uint64_t tsc_last = __rdtsc();
    return fh1_frame_cost::CyclesPerMs(tsc_first, tsc_last, first.QuadPart, last.QuadPart,
                                       frequency.QuadPart);
  }();
  ULONG64 cycles = 0;
  if (cycles_per_ms <= 0 || !QueryThreadCycleTime(GetCurrentThread(), &cycles)) return -1;
  return fh1_frame_cost::CyclesToMs(cycles, cycles_per_ms);
#else
  return -1;
#endif
#else
  timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return -1;
  return double(ts.tv_sec) * 1000.0 + double(ts.tv_nsec) / 1.0e6;
#endif
}

double ThreadCpuCoarseMs() {
#if REX_PLATFORM_WIN32
  FILETIME creation, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &creation, &exited, &kernel, &user)) return -1;
  const auto units = [](const FILETIME& f) {
    return uint64_t(f.dwHighDateTime) << 32 | uint64_t(f.dwLowDateTime);
  };
  return fh1_frame_cost::ThreadTimesToMs(units(kernel), units(user));
#else
  return -1;
#endif
}

// FH1 (#45): the process's resident memory, in bytes (0 where not known: Windows).
uint64_t ProcessResidentBytes() {
#if REX_PLATFORM_LINUX
  std::FILE* statm = std::fopen("/proc/self/statm", "r");
  if (!statm) return 0;
  unsigned long long pages_total = 0, pages_resident = 0;
  const int read = std::fscanf(statm, "%llu %llu", &pages_total, &pages_resident);
  std::fclose(statm);
  const long page_size = sysconf(_SC_PAGESIZE);
  return read == 2 && page_size > 0 ? uint64_t(pages_resident) * uint64_t(page_size) : 0;
#elif REX_PLATFORM_MAC
  mach_task_basic_info_data_t info = {};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                &count) != KERN_SUCCESS) {
    return 0;
  }
  return uint64_t(info.resident_size);
#else
  return 0;
#endif
}

// FH1 (#45): clocks, temperature and power for the minute summary (Linux sysfs; 0 elsewhere or
// when not found): the fastest CPU's clock (cpufreq), and the GPU's shader clock, temperature
// and power from the hwmon of the DRM card with the Vulkan device's PCI ids (amdgpu: sclk, edge,
// PPT; on an APU such as the Steam Deck's the power is the whole chip's).
#if REX_PLATFORM_LINUX
bool ReadSysfsNumber(const std::string& path, double* value) {
  std::FILE* file = std::fopen(path.c_str(), "r");
  if (!file) return false;
  char text[64] = {};
  const bool read = std::fgets(text, sizeof(text), file) != nullptr;
  std::fclose(file);
  if (!read) return false;
  // Decimal, or 0x... (the PCI ids): strtod reads both.
  char* end = nullptr;
  const double number = std::strtod(text, &end);
  if (end == text) return false;
  *value = number;
  return true;
}
#endif

void ReadSensors(uint32_t vendor_id, uint32_t device_id, fh1_frame_cost::Snapshot& snapshot) {
#if REX_PLATFORM_LINUX
  double khz_max = 0;
  for (uint32_t cpu = 0; cpu < 1024; ++cpu) {
    double khz = 0;
    if (!ReadSysfsNumber("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                             "/cpufreq/scaling_cur_freq",
                         &khz)) {
      if (cpu >= 64) break;
      continue;
    }
    khz_max = std::max(khz_max, khz);
  }
  snapshot.cpu_ghz = khz_max / 1.0e6;
  static std::string hwmon;
  static bool searched = false;
  if (!searched) {
    searched = true;
    for (uint32_t card = 0; card < 16 && hwmon.empty(); ++card) {
      const std::string device = "/sys/class/drm/card" + std::to_string(card) + "/device/";
      double vendor = 0, id = 0;
      if (!ReadSysfsNumber(device + "vendor", &vendor) || !ReadSysfsNumber(device + "device", &id) ||
          uint32_t(vendor) != vendor_id || uint32_t(id) != device_id) {
        continue;
      }
      std::error_code error;
      for (const auto& entry : std::filesystem::directory_iterator(device + "hwmon", error)) {
        hwmon = entry.path().string() + "/";
        break;
      }
    }
  }
  if (hwmon.empty()) return;
  double value = 0;
  if (ReadSysfsNumber(hwmon + "freq1_input", &value)) snapshot.gpu_ghz = value / 1.0e9;
  if (ReadSysfsNumber(hwmon + "temp1_input", &value)) snapshot.gpu_celsius = value / 1000.0;
  if (ReadSysfsNumber(hwmon + "power1_average", &value) ||
      ReadSysfsNumber(hwmon + "power1_input", &value)) {
    snapshot.gpu_watts = value / 1.0e6;
  }
#else
  (void)vendor_id;
  (void)device_id;
  (void)snapshot;
#endif
}

struct FrameCostState {
  bool started = false;
  std::chrono::steady_clock::time_point start, last;
#if REX_PLATFORM_WIN32
  // Windows' cycle conversion times frames only once checked against GetThreadTimes (#400).
  fh1_frame_cost::CpuClock cpu{fh1_frame_cost::CpuClock::Precise::kChecked};
#else
  fh1_frame_cost::CpuClock cpu{fh1_frame_cost::CpuClock::Precise::kDirect};
#endif
  fh1_frame_cost::Report report;
  fh1_frame_cost::Summary summary;
  std::FILE* csv = nullptr;
  bool csv_tried = false;
};

FrameCostState& GetFrameCostState() {
  static FrameCostState state;
  return state;
}

// FH1 (#45): the resolves made every frame (fh1_readback_history.h). The game has one command
// processor.
fh1_readback_history::History& GetReadbackHistory() {
  static fh1_readback_history::History history;
  return history;
}

// FH1 (#45): closes the frame's cost counters (fh1_frame_cost.h); a line in the log when
// frames were much slower than usual, every frame to fh1_frame_cost_csv, and once a minute a
// summary of the minute's frames and what the caches hold (fill_snapshot completes it).
template <typename FillSnapshot>
void Fh1EndFrameCost(uint64_t frame_index, uint64_t render_target_bytes,
                     uint32_t render_target_count, FillSnapshot&& fill_snapshot) {
  FrameCostState& state = GetFrameCostState();
  fh1_frame_cost::Frame& frame = fh1_frame_cost::Current();
  const auto now = std::chrono::steady_clock::now();
  const double precise_cpu_ms = ThreadCpuPreciseMs();
  const double coarse_cpu_ms = ThreadCpuCoarseMs();
  if (!state.started) {
    state.started = true;
    state.start = state.last = now;
    state.cpu.EndFrame(precise_cpu_ms, coarse_cpu_ms, frame);  // the clocks' first readings
    const fh1_frame_cost::Sizes& sizes = fh1_frame_cost::GetSizes();
    state.summary.SetStart(sizes.watch_nodes_visited.load(std::memory_order_relaxed),
                           sizes.watch_fires.load(std::memory_order_relaxed));
    frame = fh1_frame_cost::Frame();
    return;
  }
  frame.ms = std::chrono::duration<double, std::milli>(now - state.last).count();
  const fh1_frame_cost::CpuTimeCheck::State check = state.cpu.check().state();
  state.cpu.EndFrame(precise_cpu_ms, coarse_cpu_ms, frame);
  if (state.cpu.check().state() != check) {
    // Windows (#400): the cycles' conversion checked against GetThreadTimes (logged when the
    // verdict changes: the first agreement, a disagreement).
    if (state.cpu.check().state() == fh1_frame_cost::CpuTimeCheck::State::kAgrees) {
      REXGPU_INFO("Vulkan: command processor CPU time from thread cycles is {:.0f}% of "
                  "GetThreadTimes'; timed per frame",
                  100.0 * state.cpu.check().ratio());
    } else {
      REXGPU_WARN("Vulkan: command processor CPU time from thread cycles is {:.0f}% of "
                  "GetThreadTimes'; not timed per frame from now on",
                  100.0 * state.cpu.check().ratio());
    }
  }
  state.last = now;
  const double t = std::chrono::duration<double>(now - state.start).count();
  if (!state.csv_tried) {
    state.csv_tried = true;
    const std::string path = REXCVAR_GET(fh1_frame_cost_csv);
    if (!path.empty()) {
      state.csv = std::fopen(path.c_str(), "w");
      if (state.csv) {
        std::fputs(
            "epoch,t,ms,cpu_ms,gpu_wait_ms,guest_wait_ms,draws,textures_created,texture_created_bytes,"
            "texture_create_ms,texture_loads,texture_load_bytes,texture_load_ms,textures_evicted,"
            "texture_evicted_bytes,upload_bytes,upload_ms,shaders,shader_ms,pipelines,pipeline_ms,"
            "render_targets,render_target_bytes,render_target_ms,render_target_trims,"
            "texture_total_mb,render_target_total_mb,render_target_count,readbacks,readback_bytes,"
            "readback_ms,readback_wait_ms,readbacks_skipped,cpu_per_frame\n",
            state.csv);
      }
    }
  }
  if (state.csv) {
    std::fprintf(state.csv,
                 "%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%u,%u,%llu,%.3f,%u,%llu,%.3f,%u,%llu,%llu,%.3f,%u,"
                 "%.3f,%u,%.3f,%u,%llu,%.3f,%u,%.1f,%.1f,%u,%u,%llu,%.3f,%.3f,%u,%d\n",
                 std::chrono::duration<double>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count(),
                 t, frame.ms, frame.cpu_ms, frame.gpu_wait_ms, frame.guest_wait_ms, frame.draws,
                 frame.textures_created, (unsigned long long)frame.texture_created_bytes,
                 frame.texture_create_ms, frame.texture_loads,
                 (unsigned long long)frame.texture_load_bytes, frame.texture_load_ms,
                 frame.textures_evicted, (unsigned long long)frame.texture_evicted_bytes,
                 (unsigned long long)frame.upload_bytes, frame.upload_ms, frame.shaders,
                 frame.shader_ms, frame.pipelines, frame.pipeline_ms, frame.render_targets,
                 (unsigned long long)frame.render_target_bytes, frame.render_target_ms,
                 frame.render_target_trims, double(fh1_frame_cost::TextureBytes()) / 1048576.0,
                 double(render_target_bytes) / 1048576.0, render_target_count, frame.readbacks,
                 (unsigned long long)frame.readback_bytes, frame.readback_ms,
                 frame.readback_wait_ms, frame.readbacks_skipped, int(frame.cpu_per_frame));
    if (frame_index % 300 == 0) std::fflush(state.csv);
  }
  if (state.report.OnFrame(frame, t)) {
    REXGPU_WARN("{}", state.report.Line(t));
    state.report.Taken();
  }
  if (state.summary.OnFrame(frame, t)) {
    fh1_frame_cost::Snapshot snapshot;
    const fh1_frame_cost::Sizes& sizes = fh1_frame_cost::GetSizes();
    snapshot.textures = sizes.textures.load(std::memory_order_relaxed);
    snapshot.texture_bytes = fh1_frame_cost::TextureBytes();
    snapshot.render_targets = render_target_count;
    snapshot.render_target_bytes = render_target_bytes;
    snapshot.shaders = sizes.shaders.load(std::memory_order_relaxed);
    snapshot.pipelines = sizes.pipelines.load(std::memory_order_relaxed);
    snapshot.framebuffers = sizes.framebuffers.load(std::memory_order_relaxed);
    snapshot.watch_ranges = sizes.watch_ranges.load(std::memory_order_relaxed);
    snapshot.watch_nodes = sizes.watch_nodes.load(std::memory_order_relaxed);
    snapshot.watch_nodes_visited = sizes.watch_nodes_visited.load(std::memory_order_relaxed);
    snapshot.watch_fires = sizes.watch_fires.load(std::memory_order_relaxed);
    snapshot.readback_keys = uint32_t(GetReadbackHistory().size());
    snapshot.process_resident_bytes = ProcessResidentBytes();
    fill_snapshot(snapshot);
    REXGPU_INFO("{}", state.summary.Line(t, snapshot));
  }
  frame = fh1_frame_cost::Frame();
}

}  // namespace

void VulkanCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  InvalidateAllVertexBufferResidency();
  cache_clear_requested_ = true;
}

void VulkanCommandProcessor::TrimRenderTargetsIfNeeded() {
  namespace trim = fh1_render_target_trim;
  fh1_device_memory::State& device_memory = fh1_device_memory::Get();
  device_memory.EndFrame();
  const uint32_t percent = REXCVAR_GET(vulkan_render_target_trim_percent);
  if (!render_target_cache_ || !percent) {
    render_target_trim_requested_ = false;
    return;
  }
  if (render_target_cache_->TakeRenderTargetAllocationFailure() ||
      (fh1_other_render_targets_ && fh1_other_render_targets_->TakeRenderTargetAllocationFailure())) {
    render_target_trim_requested_ = true;
  }
  const bool failed = render_target_trim_requested_;
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const bool have_budget = vulkan_device->extensions().ext_EXT_memory_budget;
  if (!trim::ShouldQuery(frame_current_, render_target_trim_next_check_frame_,
                         render_target_trim_next_allowed_frame_, percent, failed, have_budget)) {
    return;
  }
  // The device-local use and budget (and the test cap, vulkan_device_local_budget_mb).
  const fh1_device_memory::State::Budget memory = device_memory.Query();
  const uint64_t usage = memory.usage, budget = memory.budget;
  // FH1 (#268): where new textures go to system memory (integrated GPUs with a small
  // carve-out) a nearly full device-local heap is no reason to free render targets; render
  // targets that went to system memory past their limit are.
  const bool integrated = device_memory.active();
  trim::Memory trim_memory;
  trim_memory.usage = usage;
  trim_memory.budget = budget;
  trim_memory.heap_over_budget = integrated && device_memory.HeapOverBudget(memory);
  trim_memory.system_usage = memory.system_usage;
  trim_memory.system_budget = memory.system_budget;
  trim_memory.render_targets_in_system =
      device_memory.SystemBytes(fh1_device_memory::Use::kRenderTarget);
  const trim::Reason reason = trim::ShouldTrim(integrated, failed, trim_memory, percent);
  if (reason == trim::Reason::kNone) {
    return;
  }
  if (!AwaitAllQueueOperationsCompletion()) {
    return;
  }
  render_target_trim_requested_ = false;
  ++fh1_frame_cost::Current().render_target_trims;
  uint64_t before = render_target_cache_->render_target_memory_bytes();
  uint32_t count_before = render_target_cache_->render_target_count();
  render_target_cache_->TrimRenderTargets();
  uint64_t after = render_target_cache_->render_target_memory_bytes();
  uint32_t count_after = render_target_cache_->render_target_count();
  if (fh1_other_render_targets_) {
    before += fh1_other_render_targets_->render_target_memory_bytes();
    count_before += fh1_other_render_targets_->render_target_count();
    fh1_other_render_targets_->TrimRenderTargets();
    after += fh1_other_render_targets_->render_target_memory_bytes();
    count_after += fh1_other_render_targets_->render_target_count();
  }
  render_target_trim_next_allowed_frame_ =
      trim::NextAllowedFrame(frame_current_, failed, before - after);
  // FH1 (#268): the next placements see the memory the trim freed (render targets are made
  // again in the next frame).
  if (integrated) device_memory.Refresh();
  REXGPU_INFO(
      "Render targets trimmed ({}): {} -> {} ({} -> {} MB); device-local memory {} of {} MB{}",
      reason == trim::Reason::kAllocationFailed ? "an allocation failed"
      : reason == trim::Reason::kRenderTargetsInSystemMemory
          ? fmt::format("{} MB of them in system memory", trim_memory.render_targets_in_system >> 20)
          : std::string("memory nearly full"),
      count_before, count_after, before >> 20, after >> 20, usage >> 20, budget >> 20,
      integrated ? fmt::format(", system memory {} of {} MB", memory.system_usage >> 20,
                               memory.system_budget >> 20)
                 : std::string());
}

void VulkanCommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void VulkanCommandProcessor::InvalidateAllVertexBufferResidency() {
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;
  for (VertexBufferState& state : vertex_buffer_states_) {
    state.address = UINT32_MAX;
    state.size = UINT32_MAX;
  }
}

void VulkanCommandProcessor::InvalidateVertexBufferResidency(uint32_t vfetch_index) {
  if (vfetch_index >= vertex_buffer_states_.size()) {
    return;
  }
  vertex_buffers_in_sync_[vfetch_index >> 6] &= ~(uint64_t(1) << (vfetch_index & 63));
}

void VulkanCommandProcessor::InvalidateVertexBufferResidencyRange(uint32_t first_vfetch,
                                                                  uint32_t last_vfetch) {
  if (first_vfetch > last_vfetch) {
    std::swap(first_vfetch, last_vfetch);
  }
  if (first_vfetch >= vertex_buffer_states_.size()) {
    return;
  }
  last_vfetch = std::min(last_vfetch, uint32_t(vertex_buffer_states_.size() - 1));
  for (uint32_t vfetch_index = first_vfetch; vfetch_index <= last_vfetch; ++vfetch_index) {
    InvalidateVertexBufferResidency(vfetch_index);
  }
}

void VulkanCommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                     uint32_t title_id, bool blocking) {
  fh1_storage_root_ = cache_root;
  fh1_storage_title_ = title_id;
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking);
  const auto storage_root = fh1_split_enabled_ ? cache_root / ("fh1-live-v1-" +
      std::to_string(REXCVAR_GET(fh1_output_width)) + "x" + std::to_string(REXCVAR_GET(fh1_output_height)) +
      "-" + std::to_string(fh1_other_render_targets_->draw_resolution_scale_x()) + "x" +
      std::to_string(fh1_other_render_targets_->draw_resolution_scale_y())) : cache_root;
  namespace prebuild = pipeline_prebuild_file;
  pipeline_cache_->SetShaderPrebuildSource(
      cache_root, fh1_split_enabled_ ? prebuild::kPartitionUi : prebuild::kPartitionMain);
  pipeline_cache_->InitializeShaderStorage(fh1_split_enabled_ ? storage_root / "ui" : storage_root, title_id, blocking);
  if (fh1_other_pipelines_) {
    fh1_other_pipelines_->SetShaderPrebuildSource(cache_root, prebuild::kPartitionScene);
    fh1_other_pipelines_->InitializeShaderStorage(storage_root / "scene", title_id, blocking);
  }
  if (REXCVAR_GET(shader_prebuild_only)) {
    // The launcher's "Prebuild shaders": every stored and listed pipeline is now created and its
    // driver data saved (InitializeShaderStorage). The guest has not started; nothing else is
    // needed from this run.
    REXGPU_INFO("SHADER_PREBUILD: complete, exiting");
    rex::FlushLogging();
    std::fflush(nullptr);
    std::_Exit(0);
  }
}

// Occlusion queries (EVENT_WRITE_ZPD), as Xenia's default "fast" mode does them.
// D3D writes a report at query begin and one at query end, usually at different
// addresses, and takes end minus begin; on the Xenos each report is a snapshot of
// a running sample counter. A begin writes the counter and opens a host query; an
// end closes it and, without waiting for the GPU, adds a guess (the query's last
// real result, or the fake count) to the counter and writes that. Once the GPU
// has finished, the reports are rewritten with the real counter, unless the guest
// has reused their memory meanwhile.
namespace {
struct ZpdReport {
  uint32_t address;
  bool is_end;
  uint32_t host_index;     // end reports: the host query measuring the delta
  uint64_t submission;     // end reports: the submission the query ends in
  uint64_t guessed_delta;  // end reports
  uint64_t written_value;  // what the guest report holds now
};
std::deque<ZpdReport> zpd_reports;
std::unordered_map<uint32_t, uint64_t> zpd_last_delta;  // by end report address
uint64_t zpd_real_counter = 0;
uint64_t zpd_guessed_counter = 0;
}  // namespace

bool VulkanCommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                                uint32_t packet, uint32_t count) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
  }

  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  uint32_t sample_count_addr = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
  auto* sample_counts =
      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(sample_count_addr);
  if (!sample_counts) {
    return true;
  }
  // D3D writes 0xFFFFFEED into the end report before issuing the event.
  bool is_end = sample_counts->ZPass_A == kQueryFinished ||
                sample_counts->ZPass_B == kQueryFinished ||
                sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;

  // Rewrites a report the guest has not reused since it was last written.
  auto rewrite = [this](ZpdReport& report, uint64_t value) {
    auto* counts =
        memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(report.address);
    if (counts && counts->ZPass_A == uint32_t(report.written_value) &&
        counts->Total_A == uint32_t(report.written_value)) {
      WriteGuestOcclusionResult(report.address, value);
    }
    report.written_value = value;
  };

  // Retire reports whose queries have completed, in order.
  CheckSubmissionFenceAndDeviceLoss(0);
  bool rebase = false;
  while (!zpd_reports.empty()) {
    ZpdReport& front = zpd_reports.front();
    if (front.is_end && front.host_index == UINT32_MAX) {
      zpd_real_counter += front.guessed_delta;
    } else if (front.is_end) {
      if (front.submission > GetCompletedSubmission()) {
        break;
      }
      const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
      if (!(vulkan_device->memory_types().host_coherent &
            (uint32_t(1) << occlusion_query_readback_memory_type_))) {
        VkMappedMemoryRange memory_range = {};
        memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        memory_range.memory = occlusion_query_readback_memory_;
        memory_range.offset = 0;
        memory_range.size =
            std::min(rex::round_up(VkDeviceSize(sizeof(uint64_t) * kMaxOcclusionQueries),
                                   vulkan_device->properties().nonCoherentAtomSize),
                     occlusion_query_readback_memory_size_);
        vulkan_device->functions().vkInvalidateMappedMemoryRanges(vulkan_device->device(), 1,
                                                                  &memory_range);
      }
      const uint64_t* results =
          reinterpret_cast<const uint64_t*>(occlusion_query_readback_mapping_);
      uint64_t delta = NormalizeOcclusionSamples(results[front.host_index]);
      zpd_real_counter += delta;
      if (zpd_last_delta.size() >= 4096 && !zpd_last_delta.count(front.address)) {
        zpd_last_delta.clear();
      }
      zpd_last_delta[front.address] = delta;
    }
    if (front.written_value != zpd_real_counter) {
      rewrite(front, zpd_real_counter);
      rebase = true;
    }
    zpd_reports.pop_front();
  }
  if (rebase || zpd_reports.empty()) {
    uint64_t running = zpd_real_counter;
    for (ZpdReport& report : zpd_reports) {
      if (report.is_end) {
        running += report.guessed_delta;
      }
      if (report.written_value != running) {
        rewrite(report, running);
      }
    }
    zpd_guessed_counter = running;
  }

  if (!is_end) {
    if (active_occlusion_query_.valid) {
      // A begin without an end: drop the open host query.
      if (BeginSubmission(true)) {
        EndRenderPass();
        deferred_command_buffer().CmdVkEndQuery(occlusion_query_pool_,
                                                active_occlusion_query_.host_index);
      }
      active_occlusion_query_ = {};
    }
    WriteGuestOcclusionResult(sample_count_addr, zpd_guessed_counter);
    zpd_reports.push_back({sample_count_addr, false, 0, 0, 0, zpd_guessed_counter});
    BeginGuestOcclusionQuery(sample_count_addr);
    return true;
  }

  // The guess: the last real result of this query (also zero, as Xenia's
  // "fast-alt": the game reads results before the GPU has produced them, and a
  // visible guess for a hidden surface makes Reach draw it, e.g. the
  // adaptive-tessellation water), else the fake count.
  uint64_t guess = uint64_t(std::max(REXCVAR_GET(query_occlusion_fake_sample_count), 1));
  auto last = zpd_last_delta.find(sample_count_addr);
  if (last != zpd_last_delta.end()) {
    guess = last->second;
  }
  zpd_guessed_counter += guess;
  if (active_occlusion_query_.valid && BeginSubmission(true)) {
    uint32_t host_index = active_occlusion_query_.host_index;
    EndRenderPass();
    DeferredCommandBuffer& command_buffer = deferred_command_buffer();
    command_buffer.CmdVkEndQuery(occlusion_query_pool_, host_index);
    command_buffer.CmdVkCopyQueryPoolResults(
        occlusion_query_pool_, host_index, 1, occlusion_query_readback_buffer_,
        sizeof(uint64_t) * host_index, sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    zpd_reports.push_back(
        {sample_count_addr, true, host_index, GetCurrentSubmission(), guess, zpd_guessed_counter});
  } else {
    // No host query (its begin was not seen): the guess is the answer.
    zpd_reports.push_back(
        {sample_count_addr, true, UINT32_MAX, 0, guess, zpd_guessed_counter});
  }
  active_occlusion_query_ = {};
  WriteGuestOcclusionResult(sample_count_addr, zpd_guessed_counter);
  return true;
}

std::string VulkanCommandProcessor::GetWindowTitleText() const {
  std::ostringstream title;
  title << "Vulkan";
  if (render_target_cache_) {
    switch (render_target_cache_->GetPath()) {
      case RenderTargetCache::Path::kHostRenderTargets:
        title << " - FBO";
        break;
      case RenderTargetCache::Path::kPixelShaderInterlock:
        title << " - FSI";
        break;
      default:
        break;
    }
    uint32_t draw_resolution_scale_x =
        texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
    uint32_t draw_resolution_scale_y =
        texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
    if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
      title << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
    }
  }
  title << " - HEAVILY INCOMPLETE, early development";
  return title.str();
}

bool VulkanCommandProcessor::CompileGlslToSpirv(VkShaderStageFlagBits stage,
                                                std::string_view source,
                                                std::vector<uint32_t>& spirv_out,
                                                std::string& error_out) const {
  EShLanguage glslang_stage;
  switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT:
      glslang_stage = EShLangVertex;
      break;
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
      glslang_stage = EShLangTessControl;
      break;
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
      glslang_stage = EShLangTessEvaluation;
      break;
    case VK_SHADER_STAGE_GEOMETRY_BIT:
      glslang_stage = EShLangGeometry;
      break;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
      glslang_stage = EShLangFragment;
      break;
    case VK_SHADER_STAGE_COMPUTE_BIT:
      glslang_stage = EShLangCompute;
      break;
    default:
      error_out = fmt::format("Unsupported Vulkan shader stage mask {}", uint32_t(stage));
      return false;
  }
  return CompileGlslToSpirvInternal(glslang_stage, source, spirv_out, error_out);
}

bool VulkanCommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    REXGPU_ERROR("Failed to initialize base command processor context");
    return false;
  }
  InvalidateAllVertexBufferResidency();

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();

  // The unconditional inclusion of the vertex shader stage also covers the case
  // of manual index / factor buffer fetch (the system constants and the shared
  // memory are needed for that) in the tessellation vertex shader when
  // fullDrawIndexUint32 is not supported.
  guest_shader_pipeline_stages_ =
      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  guest_shader_vertex_stages_ = VK_SHADER_STAGE_VERTEX_BIT;
  if (device_properties.tessellationShader) {
    guest_shader_pipeline_stages_ |= VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                                     VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
    guest_shader_vertex_stages_ |=
        VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  }
  if (!device_properties.vertexPipelineStoresAndAtomics) {
    // For memory export from vertex shaders converted to compute shaders.
    guest_shader_pipeline_stages_ |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    guest_shader_vertex_stages_ |= VK_SHADER_STAGE_COMPUTE_BIT;
  }

  // 16384 is bigger than any single uniform buffer that Xenia needs, but is the
  // minimum maxUniformBufferRange, thus the safe minimum amount.
  uniform_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      rex::align(std::max(ui::GraphicsUploadBufferPool::kDefaultPageSize, size_t(16384)),
                 size_t(device_properties.minUniformBufferOffsetAlignment)));

  // Descriptor set layouts that don't depend on the setup of other subsystems.
  VkShaderStageFlags guest_shader_stages =
      guest_shader_vertex_stages_ | VK_SHADER_STAGE_FRAGMENT_BIT;
  // Empty.
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = 0;
  descriptor_set_layout_create_info.pBindings = nullptr;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_empty_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create an empty Vulkan descriptor set layout");
    return false;
  }
  // Guest draw constants.
  VkDescriptorSetLayoutBinding
      descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferCount] = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    VkDescriptorSetLayoutBinding& constants_binding = descriptor_set_layout_bindings_constants[i];
    constants_binding.binding = i;
    constants_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    constants_binding.descriptorCount = 1;
    constants_binding.pImmutableSamplers = nullptr;
  }
  descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferSystem]
      .stageFlags =
      guest_shader_stages |
      (device_properties.tessellationShader ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT : 0) |
      (device_properties.geometryShader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);
  descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferFloatVertex]
      .stageFlags = guest_shader_vertex_stages_;
  descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferFloatPixel]
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferBoolLoop]
      .stageFlags = guest_shader_stages;
  descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferFetch].stageFlags =
      guest_shader_stages;
  descriptor_set_layout_create_info.bindingCount =
      uint32_t(rex::countof(descriptor_set_layout_bindings_constants));
  descriptor_set_layout_create_info.pBindings = descriptor_set_layout_bindings_constants;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_constants_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create a Vulkan descriptor set layout for guest draw "
        "constant buffers");
    return false;
  }
  // Transient: storage buffer for compute shaders.
  VkDescriptorSetLayoutBinding descriptor_set_layout_binding_transient;
  descriptor_set_layout_binding_transient.binding = 0;
  descriptor_set_layout_binding_transient.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_set_layout_binding_transient.descriptorCount = 1;
  descriptor_set_layout_binding_transient.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_binding_transient.pImmutableSamplers = nullptr;
  descriptor_set_layout_create_info.bindingCount = 1;
  descriptor_set_layout_create_info.pBindings = &descriptor_set_layout_binding_transient;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageBufferCompute)]) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create a Vulkan descriptor set layout for a storage buffer "
        "bound to the compute shader");
    return false;
  }
  descriptor_set_layout_binding_transient.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageImageCompute)]) != VK_SUCCESS) {
    return false;
  }
  descriptor_set_layout_binding_transient.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  // Transient: two storage buffers for compute shaders.
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings_transient_pair[2];
  descriptor_set_layout_bindings_transient_pair[0] = descriptor_set_layout_binding_transient;
  descriptor_set_layout_bindings_transient_pair[1] = descriptor_set_layout_binding_transient;
  descriptor_set_layout_bindings_transient_pair[1].binding = 1;
  descriptor_set_layout_create_info.bindingCount = 2;
  descriptor_set_layout_create_info.pBindings = descriptor_set_layout_bindings_transient_pair;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageBufferPairCompute)]) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create a Vulkan descriptor set layout for two storage "
        "buffers bound to the compute shader");
    return false;
  }

  // FH1 (#268): the memory layout, before the first allocation it places.
  fh1_device_memory::Get().Initialize(vulkan_device);

  shared_memory_ =
      std::make_unique<VulkanSharedMemory>(*this, *memory_, guest_shader_pipeline_stages_);
  if (!shared_memory_->Initialize()) {
    REXGPU_ERROR("Failed to initialize shared memory");
    return false;
  }
  {
    size_t hoist_page_words =
        ((size_t(SharedMemory::kBufferSize) >> shared_memory_->page_size_log2()) + 63) / 64;
    hoist_read_pages_.assign(hoist_page_words, 0);
    hoist_read_pages_serial_.assign(hoist_page_words, 0);
  }

  primitive_processor_ =
      std::make_unique<VulkanPrimitiveProcessor>(*register_file_, *memory_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the geometric primitive processor");
    return false;
  }

  uint32_t shared_memory_binding_count_log2 =
      SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
          device_properties.maxStorageBufferRange);
  uint32_t shared_memory_binding_count = UINT32_C(1) << shared_memory_binding_count_log2;

  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  bool draw_resolution_scale_not_clamped =
      TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x, draw_resolution_scale_y);
  if (!draw_resolution_scale_not_clamped) {
    REXGPU_WARN(
        "The requested draw resolution scale is not supported by the "
        "emulator, reducing to {}x{}",
        draw_resolution_scale_x, draw_resolution_scale_y);
  }
  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    REXGPU_WARN(
        "Vulkan draw resolution scaling is experimental and may not affect all "
        "titles correctly");
  }
  if (!device_properties.fragmentStoresAndAtomics) {
    REXGPU_ERROR(
        "Vulkan fragmentStoresAndAtomics is required for GPU emulation and "
        "D3D12 parity, but unsupported by the selected device");
    return false;
  }
  if (!device_properties.vertexPipelineStoresAndAtomics) {
    REXGPU_ERROR(
        "Vulkan vertexPipelineStoresAndAtomics is required for GPU emulation and "
        "D3D12 parity, but unsupported by the selected device");
    return false;
  }
  if (!device_properties.geometryShader) {
    if (REXCVAR_GET(vulkan_require_geometry_shader)) {
      REXGPU_ERROR(
          "Vulkan geometryShader is required for GPU emulation "
          "(vulkan_require_geometry_shader=true), but unsupported by the "
          "selected device");
      return false;
    }
    REXGPU_WARN(
        "Vulkan geometryShader is not supported by the device; primitive "
        "fallback conversion/expansion paths will be used");
  }
  if (!device_properties.fillModeNonSolid) {
    if (REXCVAR_GET(vulkan_require_fill_mode_non_solid)) {
      REXGPU_ERROR(
          "Vulkan fillModeNonSolid is required for GPU emulation "
          "(vulkan_require_fill_mode_non_solid=true), but unsupported by the "
          "selected device");
      return false;
    }
    REXGPU_WARN(
        "Vulkan fillModeNonSolid is not supported by the device; line/point "
        "polygon modes will fall back to solid fill");
  }

  // Requires the transient descriptor set layouts.
  render_target_cache_ = std::make_unique<VulkanRenderTargetCache>(
      *register_file_, *memory_, draw_resolution_scale_x, draw_resolution_scale_y, *this);
  if (!render_target_cache_->Initialize(shared_memory_binding_count)) {
    REXGPU_ERROR("Failed to initialize the render target cache");
    return false;
  }
  if (Fh1NativeEnabled() &&
      (render_target_cache_->GetPath() != RenderTargetCache::Path::kHostRenderTargets ||
       uint32_t(REXCVAR_GET(forza_native_width)) > 1280 * draw_resolution_scale_x ||
       uint32_t(REXCVAR_GET(forza_native_height)) > 720 * draw_resolution_scale_y)) {
    REXGPU_ERROR("FH1 native resolution requires host render targets and sufficient tiled storage");
    return false;
  }

  // Shared memory and EDRAM descriptor set layout.
  bool edram_fragment_shader_interlock =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
  VkDescriptorSetLayoutBinding shared_memory_and_edram_descriptor_set_layout_bindings[2];
  shared_memory_and_edram_descriptor_set_layout_bindings[0].binding = 0;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].descriptorCount =
      shared_memory_binding_count;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].stageFlags = guest_shader_stages;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].pImmutableSamplers = nullptr;
  VkDescriptorSetLayoutCreateInfo shared_memory_and_edram_descriptor_set_layout_create_info;
  shared_memory_and_edram_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  shared_memory_and_edram_descriptor_set_layout_create_info.pNext = nullptr;
  shared_memory_and_edram_descriptor_set_layout_create_info.flags = 0;
  shared_memory_and_edram_descriptor_set_layout_create_info.pBindings =
      shared_memory_and_edram_descriptor_set_layout_bindings;
  if (edram_fragment_shader_interlock) {
    // EDRAM.
    shared_memory_and_edram_descriptor_set_layout_bindings[1].binding = 1;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].descriptorCount = 1;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].pImmutableSamplers = nullptr;
    shared_memory_and_edram_descriptor_set_layout_create_info.bindingCount = 2;
  } else {
    shared_memory_and_edram_descriptor_set_layout_create_info.bindingCount = 1;
  }
  if (dfn.vkCreateDescriptorSetLayout(
          device, &shared_memory_and_edram_descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_shared_memory_and_edram_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create a Vulkan descriptor set layout for the shared memory "
        "and the EDRAM");
    return false;
  }


  if (!Fh1CreateRenderCaches(draw_resolution_scale_x, draw_resolution_scale_y)) return false;

  // Shared memory and EDRAM common bindings.
  VkDescriptorPoolSize descriptor_pool_sizes[1];
  descriptor_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_pool_sizes[0].descriptorCount =
      shared_memory_binding_count + uint32_t(edram_fragment_shader_interlock);
  VkDescriptorPoolCreateInfo descriptor_pool_create_info;
  descriptor_pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  descriptor_pool_create_info.pNext = nullptr;
  descriptor_pool_create_info.flags = 0;
  descriptor_pool_create_info.maxSets = 1;
  descriptor_pool_create_info.poolSizeCount = 1;
  descriptor_pool_create_info.pPoolSizes = descriptor_pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &descriptor_pool_create_info, nullptr,
                                 &shared_memory_and_edram_descriptor_pool_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create the Vulkan descriptor pool for shared memory and "
        "EDRAM");
    return false;
  }
  VkDescriptorSetAllocateInfo descriptor_set_allocate_info;
  descriptor_set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  descriptor_set_allocate_info.pNext = nullptr;
  descriptor_set_allocate_info.descriptorPool = shared_memory_and_edram_descriptor_pool_;
  descriptor_set_allocate_info.descriptorSetCount = 1;
  descriptor_set_allocate_info.pSetLayouts = &descriptor_set_layout_shared_memory_and_edram_;
  if (dfn.vkAllocateDescriptorSets(device, &descriptor_set_allocate_info,
                                   &shared_memory_and_edram_descriptor_set_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to allocate the Vulkan descriptor set for shared memory and "
        "EDRAM");
    return false;
  }
  VkDescriptorBufferInfo
      shared_memory_descriptor_buffers_info[SharedMemory::kBufferSize / (128 << 20)];
  uint32_t shared_memory_binding_range =
      SharedMemory::kBufferSize >> shared_memory_binding_count_log2;
  for (uint32_t i = 0; i < shared_memory_binding_count; ++i) {
    VkDescriptorBufferInfo& shared_memory_descriptor_buffer_info =
        shared_memory_descriptor_buffers_info[i];
    shared_memory_descriptor_buffer_info.buffer = shared_memory_->buffer();
    shared_memory_descriptor_buffer_info.offset = shared_memory_binding_range * i;
    shared_memory_descriptor_buffer_info.range = shared_memory_binding_range;
  }
  VkWriteDescriptorSet write_descriptor_sets[2];
  VkWriteDescriptorSet& write_descriptor_set_shared_memory = write_descriptor_sets[0];
  write_descriptor_set_shared_memory.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write_descriptor_set_shared_memory.pNext = nullptr;
  write_descriptor_set_shared_memory.dstSet = shared_memory_and_edram_descriptor_set_;
  write_descriptor_set_shared_memory.dstBinding = 0;
  write_descriptor_set_shared_memory.dstArrayElement = 0;
  write_descriptor_set_shared_memory.descriptorCount = shared_memory_binding_count;
  write_descriptor_set_shared_memory.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write_descriptor_set_shared_memory.pImageInfo = nullptr;
  write_descriptor_set_shared_memory.pBufferInfo = shared_memory_descriptor_buffers_info;
  write_descriptor_set_shared_memory.pTexelBufferView = nullptr;
  VkDescriptorBufferInfo edram_descriptor_buffer_info;
  if (edram_fragment_shader_interlock) {
    edram_descriptor_buffer_info.buffer = render_target_cache_->edram_buffer();
    edram_descriptor_buffer_info.offset = 0;
    edram_descriptor_buffer_info.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet& write_descriptor_set_edram = write_descriptor_sets[1];
    write_descriptor_set_edram.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_descriptor_set_edram.pNext = nullptr;
    write_descriptor_set_edram.dstSet = shared_memory_and_edram_descriptor_set_;
    write_descriptor_set_edram.dstBinding = 1;
    write_descriptor_set_edram.dstArrayElement = 0;
    write_descriptor_set_edram.descriptorCount = 1;
    write_descriptor_set_edram.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write_descriptor_set_edram.pImageInfo = nullptr;
    write_descriptor_set_edram.pBufferInfo = &edram_descriptor_buffer_info;
    write_descriptor_set_edram.pTexelBufferView = nullptr;
  }
  dfn.vkUpdateDescriptorSets(device, 1 + uint32_t(edram_fragment_shader_interlock),
                             write_descriptor_sets, 0, nullptr);

  // Swap objects.

  // Gamma ramp, either device-local and host-visible at once, or separate
  // device-local texel buffer and host-visible upload buffer.
  gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
  gamma_ramp_pwl_current_frame_ = UINT32_MAX;
  // Try to create a device-local host-visible buffer first, to skip copying.
  constexpr uint32_t kGammaRampSize256EntryTable = sizeof(uint32_t) * 256;
  constexpr uint32_t kGammaRampSizePWL = sizeof(uint16_t) * 2 * 3 * 128;
  constexpr uint32_t kGammaRampSize = kGammaRampSize256EntryTable + kGammaRampSizePWL;
  VkBufferCreateInfo gamma_ramp_host_visible_buffer_create_info;
  gamma_ramp_host_visible_buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  gamma_ramp_host_visible_buffer_create_info.pNext = nullptr;
  gamma_ramp_host_visible_buffer_create_info.flags = 0;
  gamma_ramp_host_visible_buffer_create_info.size = kGammaRampSize * kMaxFramesInFlight;
  gamma_ramp_host_visible_buffer_create_info.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
  gamma_ramp_host_visible_buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  gamma_ramp_host_visible_buffer_create_info.queueFamilyIndexCount = 0;
  gamma_ramp_host_visible_buffer_create_info.pQueueFamilyIndices = nullptr;
  if (dfn.vkCreateBuffer(device, &gamma_ramp_host_visible_buffer_create_info, nullptr,
                         &gamma_ramp_buffer_) == VK_SUCCESS) {
    bool use_gamma_ramp_host_visible_buffer = false;
    VkMemoryRequirements gamma_ramp_host_visible_buffer_memory_requirements;
    dfn.vkGetBufferMemoryRequirements(device, gamma_ramp_buffer_,
                                      &gamma_ramp_host_visible_buffer_memory_requirements);
    uint32_t gamma_ramp_host_visible_buffer_memory_types =
        gamma_ramp_host_visible_buffer_memory_requirements.memoryTypeBits &
        (vulkan_device->memory_types().device_local & vulkan_device->memory_types().host_visible);
    VkMemoryAllocateInfo gamma_ramp_host_visible_buffer_memory_allocate_info;
    // Prefer a host-uncached (because it's write-only) memory type, but try a
    // host-cached host-visible device-local one as well.
    if (rex::bit_scan_forward(
            gamma_ramp_host_visible_buffer_memory_types &
                ~vulkan_device->memory_types().host_cached,
            &(gamma_ramp_host_visible_buffer_memory_allocate_info.memoryTypeIndex)) ||
        rex::bit_scan_forward(
            gamma_ramp_host_visible_buffer_memory_types,
            &(gamma_ramp_host_visible_buffer_memory_allocate_info.memoryTypeIndex))) {
      VkMemoryAllocateInfo* gamma_ramp_host_visible_buffer_memory_allocate_info_last =
          &gamma_ramp_host_visible_buffer_memory_allocate_info;
      gamma_ramp_host_visible_buffer_memory_allocate_info.sType =
          VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      gamma_ramp_host_visible_buffer_memory_allocate_info.pNext = nullptr;
      gamma_ramp_host_visible_buffer_memory_allocate_info.allocationSize =
          gamma_ramp_host_visible_buffer_memory_requirements.size;
      VkMemoryDedicatedAllocateInfo gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info;
      if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
        gamma_ramp_host_visible_buffer_memory_allocate_info_last->pNext =
            &gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info;
        gamma_ramp_host_visible_buffer_memory_allocate_info_last =
            reinterpret_cast<VkMemoryAllocateInfo*>(
                &gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info);
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.sType =
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.pNext = nullptr;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.image = VK_NULL_HANDLE;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.buffer = gamma_ramp_buffer_;
      }
      if (dfn.vkAllocateMemory(device, &gamma_ramp_host_visible_buffer_memory_allocate_info,
                               nullptr, &gamma_ramp_buffer_memory_) == VK_SUCCESS) {
        if (dfn.vkBindBufferMemory(device, gamma_ramp_buffer_, gamma_ramp_buffer_memory_, 0) ==
            VK_SUCCESS) {
          if (dfn.vkMapMemory(device, gamma_ramp_buffer_memory_, 0, VK_WHOLE_SIZE, 0,
                              &gamma_ramp_upload_mapping_) == VK_SUCCESS) {
            use_gamma_ramp_host_visible_buffer = true;
            gamma_ramp_upload_memory_size_ =
                gamma_ramp_host_visible_buffer_memory_allocate_info.allocationSize;
            gamma_ramp_upload_memory_type_ =
                gamma_ramp_host_visible_buffer_memory_allocate_info.memoryTypeIndex;
          }
        }
        if (!use_gamma_ramp_host_visible_buffer) {
          dfn.vkFreeMemory(device, gamma_ramp_buffer_memory_, nullptr);
          gamma_ramp_buffer_memory_ = VK_NULL_HANDLE;
        }
      }
    }
    if (!use_gamma_ramp_host_visible_buffer) {
      dfn.vkDestroyBuffer(device, gamma_ramp_buffer_, nullptr);
      gamma_ramp_buffer_ = VK_NULL_HANDLE;
    }
  }
  if (gamma_ramp_buffer_ == VK_NULL_HANDLE) {
    // Create separate buffers for the shader and uploading.
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kGammaRampSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal, gamma_ramp_buffer_,
            gamma_ramp_buffer_memory_)) {
      REXGPU_ERROR("Failed to create the gamma ramp buffer");
      return false;
    }
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kGammaRampSize * kMaxFramesInFlight, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            ui::vulkan::util::MemoryPurpose::kUpload, gamma_ramp_upload_buffer_,
            gamma_ramp_upload_buffer_memory_, &gamma_ramp_upload_memory_type_,
            &gamma_ramp_upload_memory_size_)) {
      REXGPU_ERROR("Failed to create the gamma ramp upload buffer");
      return false;
    }
    if (dfn.vkMapMemory(device, gamma_ramp_upload_buffer_memory_, 0, VK_WHOLE_SIZE, 0,
                        &gamma_ramp_upload_mapping_) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to map the gamma ramp upload buffer");
      return false;
    }
  }

  // Gamma ramp buffer views.
  uint32_t gamma_ramp_frame_count =
      gamma_ramp_upload_buffer_ == VK_NULL_HANDLE ? kMaxFramesInFlight : 1;
  VkBufferViewCreateInfo gamma_ramp_buffer_view_create_info;
  gamma_ramp_buffer_view_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
  gamma_ramp_buffer_view_create_info.pNext = nullptr;
  gamma_ramp_buffer_view_create_info.flags = 0;
  gamma_ramp_buffer_view_create_info.buffer = gamma_ramp_buffer_;
  // 256-entry table.
  gamma_ramp_buffer_view_create_info.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  gamma_ramp_buffer_view_create_info.range = kGammaRampSize256EntryTable;
  for (uint32_t i = 0; i < gamma_ramp_frame_count; ++i) {
    gamma_ramp_buffer_view_create_info.offset = kGammaRampSize * i;
    if (dfn.vkCreateBufferView(device, &gamma_ramp_buffer_view_create_info, nullptr,
                               &gamma_ramp_buffer_views_[i * 2]) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to create a 256-entry table gamma ramp buffer view");
      return false;
    }
  }
  // Piecewise linear.
  gamma_ramp_buffer_view_create_info.format = VK_FORMAT_R16G16_UINT;
  gamma_ramp_buffer_view_create_info.range = kGammaRampSizePWL;
  for (uint32_t i = 0; i < gamma_ramp_frame_count; ++i) {
    gamma_ramp_buffer_view_create_info.offset = kGammaRampSize * i + kGammaRampSize256EntryTable;
    if (dfn.vkCreateBufferView(device, &gamma_ramp_buffer_view_create_info, nullptr,
                               &gamma_ramp_buffer_views_[i * 2 + 1]) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to create a PWL gamma ramp buffer view");
      return false;
    }
  }

  // Swap descriptor set layouts.
  VkDescriptorSetLayoutBinding swap_descriptor_set_layout_binding;
  swap_descriptor_set_layout_binding.binding = 0;
  swap_descriptor_set_layout_binding.descriptorCount = 1;
  swap_descriptor_set_layout_binding.stageFlags =
      VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
  swap_descriptor_set_layout_binding.pImmutableSamplers = nullptr;
  VkDescriptorSetLayoutCreateInfo swap_descriptor_set_layout_create_info;
  swap_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  swap_descriptor_set_layout_create_info.pNext = nullptr;
  swap_descriptor_set_layout_create_info.flags = 0;
  swap_descriptor_set_layout_create_info.bindingCount = 1;
  swap_descriptor_set_layout_create_info.pBindings = &swap_descriptor_set_layout_binding;
  swap_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(device, &swap_descriptor_set_layout_create_info, nullptr,
                                      &swap_descriptor_set_layout_sampled_image_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create the presentation sampled image descriptor set "
        "layout");
    return false;
  }
  swap_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  if (dfn.vkCreateDescriptorSetLayout(device, &swap_descriptor_set_layout_create_info, nullptr,
                                      &swap_descriptor_set_layout_uniform_texel_buffer_) !=
      VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create the presentation uniform texel buffer descriptor set "
        "layout");
    return false;
  }
  swap_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  swap_descriptor_set_layout_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  if (dfn.vkCreateDescriptorSetLayout(device, &swap_descriptor_set_layout_create_info, nullptr,
                                      &swap_descriptor_set_layout_combined_image_sampler_) !=
      VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create the presentation combined image sampler descriptor "
        "set layout");
    return false;
  }
  swap_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(device, &swap_descriptor_set_layout_create_info, nullptr,
                                      &swap_descriptor_set_layout_storage_image_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create the presentation storage image descriptor set "
        "layout");
    return false;
  }

  // Swap descriptor pool.
  std::array<VkDescriptorPoolSize, 4> swap_descriptor_pool_sizes;
  VkDescriptorPoolCreateInfo swap_descriptor_pool_create_info;
  swap_descriptor_pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  swap_descriptor_pool_create_info.pNext = nullptr;
  swap_descriptor_pool_create_info.flags = 0;
  swap_descriptor_pool_create_info.maxSets = 0;
  swap_descriptor_pool_create_info.poolSizeCount = 0;
  swap_descriptor_pool_create_info.pPoolSizes = swap_descriptor_pool_sizes.data();
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_sampled_image =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info.poolSizeCount++];
    swap_descriptor_pool_size_sampled_image.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    // Source images.
    swap_descriptor_pool_size_sampled_image.descriptorCount = kMaxFramesInFlight;
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight;
  }
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_combined_image_sampler =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info.poolSizeCount++];
    swap_descriptor_pool_size_combined_image_sampler.type =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    swap_descriptor_pool_size_combined_image_sampler.descriptorCount = kMaxFramesInFlight;
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight;
  }
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_storage_image =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info.poolSizeCount++];
    swap_descriptor_pool_size_storage_image.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    swap_descriptor_pool_size_storage_image.descriptorCount = kMaxFramesInFlight * 2;
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight * 2;
  }
  // 256-entry table and PWL gamma ramps. If the gamma ramp buffer is
  // host-visible, for multiple frames.
  uint32_t gamma_ramp_buffer_view_count = 2 * gamma_ramp_frame_count;
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_uniform_texel_buffer =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info.poolSizeCount++];
    swap_descriptor_pool_size_uniform_texel_buffer.type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    swap_descriptor_pool_size_uniform_texel_buffer.descriptorCount = gamma_ramp_buffer_view_count;
    swap_descriptor_pool_create_info.maxSets += gamma_ramp_buffer_view_count;
  }
  if (dfn.vkCreateDescriptorPool(device, &swap_descriptor_pool_create_info, nullptr,
                                 &swap_descriptor_pool_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the presentation descriptor pool");
    return false;
  }

  // Swap descriptor set allocation.
  VkDescriptorSetAllocateInfo swap_descriptor_set_allocate_info;
  swap_descriptor_set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  swap_descriptor_set_allocate_info.pNext = nullptr;
  swap_descriptor_set_allocate_info.descriptorPool = swap_descriptor_pool_;
  swap_descriptor_set_allocate_info.descriptorSetCount = 1;
  swap_descriptor_set_allocate_info.pSetLayouts = &swap_descriptor_set_layout_uniform_texel_buffer_;
  for (uint32_t i = 0; i < gamma_ramp_buffer_view_count; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_gamma_ramp_[i]) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to allocate the gamma ramp descriptor sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts = &swap_descriptor_set_layout_sampled_image_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_source_[i]) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to allocate the presentation source image descriptor sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_combined_image_sampler_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_fxaa_source_[i]) != VK_SUCCESS) {
      REXGPU_ERROR(
          "Failed to allocate the presentation FXAA source image descriptor "
          "sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts = &swap_descriptor_set_layout_storage_image_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_destination_storage_[i]) != VK_SUCCESS) {
      REXGPU_ERROR(
          "Failed to allocate the presentation destination storage image "
          "descriptor sets");
      return false;
    }
  }
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_fxaa_destination_storage_[i]) !=
        VK_SUCCESS) {
      REXGPU_ERROR(
          "Failed to allocate the presentation FXAA destination storage image "
          "descriptor sets");
      return false;
    }
  }

  // Gamma ramp descriptor sets.
  VkWriteDescriptorSet gamma_ramp_write_descriptor_set;
  gamma_ramp_write_descriptor_set.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  gamma_ramp_write_descriptor_set.pNext = nullptr;
  gamma_ramp_write_descriptor_set.dstBinding = 0;
  gamma_ramp_write_descriptor_set.dstArrayElement = 0;
  gamma_ramp_write_descriptor_set.descriptorCount = 1;
  gamma_ramp_write_descriptor_set.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  gamma_ramp_write_descriptor_set.pImageInfo = nullptr;
  gamma_ramp_write_descriptor_set.pBufferInfo = nullptr;
  for (uint32_t i = 0; i < gamma_ramp_buffer_view_count; ++i) {
    gamma_ramp_write_descriptor_set.dstSet = swap_descriptors_gamma_ramp_[i];
    gamma_ramp_write_descriptor_set.pTexelBufferView = &gamma_ramp_buffer_views_[i];
    dfn.vkUpdateDescriptorSets(device, 1, &gamma_ramp_write_descriptor_set, 0, nullptr);
  }

  // Linear sampler for FXAA.
  VkSamplerCreateInfo swap_sampler_create_info;
  swap_sampler_create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  swap_sampler_create_info.pNext = nullptr;
  swap_sampler_create_info.flags = 0;
  swap_sampler_create_info.magFilter = VK_FILTER_LINEAR;
  swap_sampler_create_info.minFilter = VK_FILTER_LINEAR;
  swap_sampler_create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  swap_sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  swap_sampler_create_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  swap_sampler_create_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  swap_sampler_create_info.mipLodBias = 0.0f;
  swap_sampler_create_info.anisotropyEnable = VK_FALSE;
  swap_sampler_create_info.maxAnisotropy = 1.0f;
  swap_sampler_create_info.compareEnable = VK_FALSE;
  swap_sampler_create_info.compareOp = VK_COMPARE_OP_NEVER;
  swap_sampler_create_info.minLod = 0.0f;
  swap_sampler_create_info.maxLod = 0.0f;
  swap_sampler_create_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  swap_sampler_create_info.unnormalizedCoordinates = VK_FALSE;
  if (dfn.vkCreateSampler(device, &swap_sampler_create_info, nullptr,
                          &swap_sampler_linear_clamp_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the presentation FXAA sampler");
    return false;
  }

  // Gamma ramp application pipeline layout.
  std::array<VkDescriptorSetLayout, kSwapApplyGammaDescriptorSetCount>
      swap_apply_gamma_descriptor_set_layouts{};
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetRamp] =
      swap_descriptor_set_layout_uniform_texel_buffer_;
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetSource] =
      swap_descriptor_set_layout_sampled_image_;
  VkPipelineLayoutCreateInfo swap_apply_gamma_pipeline_layout_create_info;
  swap_apply_gamma_pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  swap_apply_gamma_pipeline_layout_create_info.pNext = nullptr;
  swap_apply_gamma_pipeline_layout_create_info.flags = 0;
  swap_apply_gamma_pipeline_layout_create_info.setLayoutCount =
      uint32_t(swap_apply_gamma_descriptor_set_layouts.size());
  swap_apply_gamma_pipeline_layout_create_info.pSetLayouts =
      swap_apply_gamma_descriptor_set_layouts.data();
  swap_apply_gamma_pipeline_layout_create_info.pushConstantRangeCount = 0;
  swap_apply_gamma_pipeline_layout_create_info.pPushConstantRanges = nullptr;
  if (dfn.vkCreatePipelineLayout(device, &swap_apply_gamma_pipeline_layout_create_info, nullptr,
                                 &swap_apply_gamma_pipeline_layout_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the gamma ramp application pipeline layout");
    return false;
  }

  // Gamma ramp application compute pipeline layout.
  std::array<VkDescriptorSetLayout, kSwapApplyGammaComputeDescriptorSetCount>
      swap_apply_gamma_compute_descriptor_set_layouts{};
  swap_apply_gamma_compute_descriptor_set_layouts[kSwapApplyGammaComputeDescriptorSetRamp] =
      swap_descriptor_set_layout_uniform_texel_buffer_;
  swap_apply_gamma_compute_descriptor_set_layouts[kSwapApplyGammaComputeDescriptorSetSource] =
      swap_descriptor_set_layout_sampled_image_;
  swap_apply_gamma_compute_descriptor_set_layouts[kSwapApplyGammaComputeDescriptorSetDestination] =
      swap_descriptor_set_layout_storage_image_;
  VkPushConstantRange swap_apply_gamma_compute_push_constant_range;
  swap_apply_gamma_compute_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  swap_apply_gamma_compute_push_constant_range.offset = 0;
  swap_apply_gamma_compute_push_constant_range.size = sizeof(SwapApplyGammaConstants);
  swap_apply_gamma_pipeline_layout_create_info.setLayoutCount =
      uint32_t(swap_apply_gamma_compute_descriptor_set_layouts.size());
  swap_apply_gamma_pipeline_layout_create_info.pSetLayouts =
      swap_apply_gamma_compute_descriptor_set_layouts.data();
  swap_apply_gamma_pipeline_layout_create_info.pushConstantRangeCount = 1;
  swap_apply_gamma_pipeline_layout_create_info.pPushConstantRanges =
      &swap_apply_gamma_compute_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &swap_apply_gamma_pipeline_layout_create_info, nullptr,
                                 &swap_apply_gamma_compute_pipeline_layout_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the gamma ramp application compute pipeline layout");
    return false;
  }

  // FXAA compute pipeline layout.
  std::array<VkDescriptorSetLayout, kSwapFxaaDescriptorSetCount> swap_fxaa_descriptor_set_layouts{};
  swap_fxaa_descriptor_set_layouts[kSwapFxaaDescriptorSetSource] =
      swap_descriptor_set_layout_combined_image_sampler_;
  swap_fxaa_descriptor_set_layouts[kSwapFxaaDescriptorSetDestination] =
      swap_descriptor_set_layout_storage_image_;
  VkPushConstantRange swap_fxaa_push_constant_range;
  swap_fxaa_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  swap_fxaa_push_constant_range.offset = 0;
  swap_fxaa_push_constant_range.size = sizeof(SwapFxaaConstants);
  swap_apply_gamma_pipeline_layout_create_info.setLayoutCount =
      uint32_t(swap_fxaa_descriptor_set_layouts.size());
  swap_apply_gamma_pipeline_layout_create_info.pSetLayouts =
      swap_fxaa_descriptor_set_layouts.data();
  swap_apply_gamma_pipeline_layout_create_info.pushConstantRangeCount = 1;
  swap_apply_gamma_pipeline_layout_create_info.pPushConstantRanges = &swap_fxaa_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &swap_apply_gamma_pipeline_layout_create_info, nullptr,
                                 &swap_fxaa_pipeline_layout_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the FXAA compute pipeline layout");
    return false;
  }

  // Gamma application render pass. Doesn't make assumptions about outer usage
  // (explicit barriers must be used instead) for simplicity of use in different
  // scenarios with different pipelines.
  VkAttachmentDescription swap_apply_gamma_render_pass_attachment;
  swap_apply_gamma_render_pass_attachment.flags = 0;
  swap_apply_gamma_render_pass_attachment.format = ui::vulkan::VulkanPresenter::kGuestOutputFormat;
  swap_apply_gamma_render_pass_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  swap_apply_gamma_render_pass_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  swap_apply_gamma_render_pass_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  swap_apply_gamma_render_pass_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  swap_apply_gamma_render_pass_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  swap_apply_gamma_render_pass_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  swap_apply_gamma_render_pass_attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference swap_apply_gamma_render_pass_color_attachment;
  swap_apply_gamma_render_pass_color_attachment.attachment = 0;
  swap_apply_gamma_render_pass_color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkSubpassDescription swap_apply_gamma_render_pass_subpass = {};
  swap_apply_gamma_render_pass_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  swap_apply_gamma_render_pass_subpass.colorAttachmentCount = 1;
  swap_apply_gamma_render_pass_subpass.pColorAttachments =
      &swap_apply_gamma_render_pass_color_attachment;
  VkSubpassDependency swap_apply_gamma_render_pass_dependencies[2];
  for (uint32_t i = 0; i < 2; ++i) {
    VkSubpassDependency& swap_apply_gamma_render_pass_dependency =
        swap_apply_gamma_render_pass_dependencies[i];
    swap_apply_gamma_render_pass_dependency.srcSubpass = i ? 0 : VK_SUBPASS_EXTERNAL;
    swap_apply_gamma_render_pass_dependency.dstSubpass = i ? VK_SUBPASS_EXTERNAL : 0;
    swap_apply_gamma_render_pass_dependency.srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    swap_apply_gamma_render_pass_dependency.dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    swap_apply_gamma_render_pass_dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    swap_apply_gamma_render_pass_dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    swap_apply_gamma_render_pass_dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
  }
  VkRenderPassCreateInfo swap_apply_gamma_render_pass_create_info;
  swap_apply_gamma_render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  swap_apply_gamma_render_pass_create_info.pNext = nullptr;
  swap_apply_gamma_render_pass_create_info.flags = 0;
  swap_apply_gamma_render_pass_create_info.attachmentCount = 1;
  swap_apply_gamma_render_pass_create_info.pAttachments = &swap_apply_gamma_render_pass_attachment;
  swap_apply_gamma_render_pass_create_info.subpassCount = 1;
  swap_apply_gamma_render_pass_create_info.pSubpasses = &swap_apply_gamma_render_pass_subpass;
  swap_apply_gamma_render_pass_create_info.dependencyCount =
      uint32_t(rex::countof(swap_apply_gamma_render_pass_dependencies));
  swap_apply_gamma_render_pass_create_info.pDependencies =
      swap_apply_gamma_render_pass_dependencies;
  if (dfn.vkCreateRenderPass(device, &swap_apply_gamma_render_pass_create_info, nullptr,
                             &swap_apply_gamma_render_pass_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the gamma ramp application render pass");
    return false;
  }

  // Gamma ramp application pipeline.
  // Using a graphics pipeline, not a compute one, because storage image support
  // is optional for VK_FORMAT_A2B10G10R10_UNORM_PACK32.

  enum SwapApplyGammaPixelShader {
    kSwapApplyGammaPixelShader256EntryTable,
    kSwapApplyGammaPixelShaderPWL,
    kSwapApplyGammaPixelShader256EntryTableFxaaLuma,
    kSwapApplyGammaPixelShaderPWLFxaaLuma,

    kSwapApplyGammaPixelShaderCount,
  };
  std::array<VkShaderModule, kSwapApplyGammaPixelShaderCount> swap_apply_gamma_pixel_shaders{};
  bool swap_apply_gamma_pixel_shaders_created =
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShader256EntryTable] =
           ui::vulkan::util::CreateShaderModule(vulkan_device, shaders::apply_gamma_table_ps,
                                                sizeof(shaders::apply_gamma_table_ps))) !=
          VK_NULL_HANDLE &&
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShaderPWL] =
           ui::vulkan::util::CreateShaderModule(vulkan_device, shaders::apply_gamma_pwl_ps,
                                                sizeof(shaders::apply_gamma_pwl_ps))) !=
          VK_NULL_HANDLE &&
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShader256EntryTableFxaaLuma] =
           ui::vulkan::util::CreateShaderModule(
               vulkan_device, shaders::apply_gamma_table_fxaa_luma_ps,
               sizeof(shaders::apply_gamma_table_fxaa_luma_ps))) != VK_NULL_HANDLE &&
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShaderPWLFxaaLuma] =
           ui::vulkan::util::CreateShaderModule(
               vulkan_device, shaders::apply_gamma_pwl_fxaa_luma_ps,
               sizeof(shaders::apply_gamma_pwl_fxaa_luma_ps))) != VK_NULL_HANDLE;
  if (!swap_apply_gamma_pixel_shaders_created) {
    REXGPU_ERROR("Failed to create the gamma ramp application pixel shader modules");
    for (VkShaderModule swap_apply_gamma_pixel_shader : swap_apply_gamma_pixel_shaders) {
      if (swap_apply_gamma_pixel_shader != VK_NULL_HANDLE) {
        dfn.vkDestroyShaderModule(device, swap_apply_gamma_pixel_shader, nullptr);
      }
    }
    return false;
  }

  VkPipelineShaderStageCreateInfo swap_apply_gamma_pipeline_stages[2];
  swap_apply_gamma_pipeline_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  swap_apply_gamma_pipeline_stages[0].pNext = nullptr;
  swap_apply_gamma_pipeline_stages[0].flags = 0;
  swap_apply_gamma_pipeline_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  swap_apply_gamma_pipeline_stages[0].module = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::fullscreen_cw_vs, sizeof(shaders::fullscreen_cw_vs));
  if (swap_apply_gamma_pipeline_stages[0].module == VK_NULL_HANDLE) {
    REXGPU_ERROR("Failed to create the gamma ramp application vertex shader module");
    for (VkShaderModule swap_apply_gamma_pixel_shader : swap_apply_gamma_pixel_shaders) {
      assert_true(swap_apply_gamma_pixel_shader != VK_NULL_HANDLE);
      dfn.vkDestroyShaderModule(device, swap_apply_gamma_pixel_shader, nullptr);
    }
    return false;
  }
  swap_apply_gamma_pipeline_stages[0].pName = "main";
  swap_apply_gamma_pipeline_stages[0].pSpecializationInfo = nullptr;
  swap_apply_gamma_pipeline_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  swap_apply_gamma_pipeline_stages[1].pNext = nullptr;
  swap_apply_gamma_pipeline_stages[1].flags = 0;
  swap_apply_gamma_pipeline_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  // The fragment shader module will be specified later.
  swap_apply_gamma_pipeline_stages[1].pName = "main";
  swap_apply_gamma_pipeline_stages[1].pSpecializationInfo = nullptr;

  VkPipelineVertexInputStateCreateInfo swap_apply_gamma_pipeline_vertex_input_state = {};
  swap_apply_gamma_pipeline_vertex_input_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo swap_apply_gamma_pipeline_input_assembly_state;
  swap_apply_gamma_pipeline_input_assembly_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_input_assembly_state.pNext = nullptr;
  swap_apply_gamma_pipeline_input_assembly_state.flags = 0;
  swap_apply_gamma_pipeline_input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  swap_apply_gamma_pipeline_input_assembly_state.primitiveRestartEnable = VK_FALSE;

  VkPipelineViewportStateCreateInfo swap_apply_gamma_pipeline_viewport_state;
  swap_apply_gamma_pipeline_viewport_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_viewport_state.pNext = nullptr;
  swap_apply_gamma_pipeline_viewport_state.flags = 0;
  swap_apply_gamma_pipeline_viewport_state.viewportCount = 1;
  swap_apply_gamma_pipeline_viewport_state.pViewports = nullptr;
  swap_apply_gamma_pipeline_viewport_state.scissorCount = 1;
  swap_apply_gamma_pipeline_viewport_state.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo swap_apply_gamma_pipeline_rasterization_state = {};
  swap_apply_gamma_pipeline_rasterization_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
  swap_apply_gamma_pipeline_rasterization_state.cullMode = VK_CULL_MODE_NONE;
  swap_apply_gamma_pipeline_rasterization_state.frontFace = VK_FRONT_FACE_CLOCKWISE;
  swap_apply_gamma_pipeline_rasterization_state.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo swap_apply_gamma_pipeline_multisample_state = {};
  swap_apply_gamma_pipeline_multisample_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState swap_apply_gamma_pipeline_color_blend_attachment_state = {};
  swap_apply_gamma_pipeline_color_blend_attachment_state.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
      VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo swap_apply_gamma_pipeline_color_blend_state = {};
  swap_apply_gamma_pipeline_color_blend_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_color_blend_state.attachmentCount = 1;
  swap_apply_gamma_pipeline_color_blend_state.pAttachments =
      &swap_apply_gamma_pipeline_color_blend_attachment_state;

  static const VkDynamicState kSwapApplyGammaPipelineDynamicStates[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };
  VkPipelineDynamicStateCreateInfo swap_apply_gamma_pipeline_dynamic_state;
  swap_apply_gamma_pipeline_dynamic_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_dynamic_state.pNext = nullptr;
  swap_apply_gamma_pipeline_dynamic_state.flags = 0;
  swap_apply_gamma_pipeline_dynamic_state.dynamicStateCount =
      uint32_t(rex::countof(kSwapApplyGammaPipelineDynamicStates));
  swap_apply_gamma_pipeline_dynamic_state.pDynamicStates = kSwapApplyGammaPipelineDynamicStates;

  VkGraphicsPipelineCreateInfo swap_apply_gamma_pipeline_create_info;
  swap_apply_gamma_pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  swap_apply_gamma_pipeline_create_info.pNext = nullptr;
  swap_apply_gamma_pipeline_create_info.flags = 0;
  swap_apply_gamma_pipeline_create_info.stageCount =
      uint32_t(rex::countof(swap_apply_gamma_pipeline_stages));
  swap_apply_gamma_pipeline_create_info.pStages = swap_apply_gamma_pipeline_stages;
  swap_apply_gamma_pipeline_create_info.pVertexInputState =
      &swap_apply_gamma_pipeline_vertex_input_state;
  swap_apply_gamma_pipeline_create_info.pInputAssemblyState =
      &swap_apply_gamma_pipeline_input_assembly_state;
  swap_apply_gamma_pipeline_create_info.pTessellationState = nullptr;
  swap_apply_gamma_pipeline_create_info.pViewportState = &swap_apply_gamma_pipeline_viewport_state;
  swap_apply_gamma_pipeline_create_info.pRasterizationState =
      &swap_apply_gamma_pipeline_rasterization_state;
  swap_apply_gamma_pipeline_create_info.pMultisampleState =
      &swap_apply_gamma_pipeline_multisample_state;
  swap_apply_gamma_pipeline_create_info.pDepthStencilState = nullptr;
  swap_apply_gamma_pipeline_create_info.pColorBlendState =
      &swap_apply_gamma_pipeline_color_blend_state;
  swap_apply_gamma_pipeline_create_info.pDynamicState = &swap_apply_gamma_pipeline_dynamic_state;
  swap_apply_gamma_pipeline_create_info.layout = swap_apply_gamma_pipeline_layout_;
  swap_apply_gamma_pipeline_create_info.renderPass = swap_apply_gamma_render_pass_;
  swap_apply_gamma_pipeline_create_info.subpass = 0;
  swap_apply_gamma_pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  swap_apply_gamma_pipeline_create_info.basePipelineIndex = -1;
  std::array<VkShaderModule, 4> swap_apply_gamma_pipeline_pixel_shaders = {
      swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShader256EntryTable],
      swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShaderPWL],
      swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShader256EntryTableFxaaLuma],
      swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShaderPWLFxaaLuma],
  };
  std::array<VkPipeline*, 4> swap_apply_gamma_pipelines = {
      &swap_apply_gamma_256_entry_table_pipeline_,
      &swap_apply_gamma_pwl_pipeline_,
      &swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_,
      &swap_apply_gamma_pwl_fxaa_luma_pipeline_,
  };
  std::array<VkResult, 4> swap_apply_gamma_pipeline_create_results;
  for (size_t i = 0; i < swap_apply_gamma_pipelines.size(); ++i) {
    swap_apply_gamma_pipeline_stages[1].module = swap_apply_gamma_pipeline_pixel_shaders[i];
    swap_apply_gamma_pipeline_create_results[i] = dfn.vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &swap_apply_gamma_pipeline_create_info, nullptr,
        swap_apply_gamma_pipelines[i]);
  }
  if (!vulkan_device->properties().imageViewFormatSwizzle) {
    auto create_rb_swap_pipeline = [&](const char* source, VkPipeline& pipeline_out,
                                       const char* pipeline_name) {
      std::vector<uint32_t> pixel_spirv;
      std::string compile_error;
      if (!CompileGlslToSpirv(VK_SHADER_STAGE_FRAGMENT_BIT, source, pixel_spirv, compile_error)) {
        REXGPU_WARN("Failed to compile {} shader to SPIR-V: {}", pipeline_name, compile_error);
        return;
      }
      VkShaderModule pixel_shader_module = ui::vulkan::util::CreateShaderModule(
          vulkan_device, pixel_spirv.data(), sizeof(uint32_t) * pixel_spirv.size());
      if (pixel_shader_module == VK_NULL_HANDLE) {
        REXGPU_WARN("Failed to create {} shader module", pipeline_name);
        return;
      }
      swap_apply_gamma_pipeline_stages[1].module = pixel_shader_module;
      if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                        &swap_apply_gamma_pipeline_create_info, nullptr,
                                        &pipeline_out) != VK_SUCCESS) {
        REXGPU_WARN("Failed to create {} pipeline", pipeline_name);
        pipeline_out = VK_NULL_HANDLE;
      }
      dfn.vkDestroyShaderModule(device, pixel_shader_module, nullptr);
    };
    create_rb_swap_pipeline(GetSwapApplyGammaTablePixelRbSwapSource(),
                            swap_apply_gamma_256_entry_table_rb_swap_pipeline_,
                            "swap gamma table red/blue fallback");
    create_rb_swap_pipeline(GetSwapApplyGammaPwlPixelRbSwapSource(),
                            swap_apply_gamma_pwl_rb_swap_pipeline_,
                            "swap gamma PWL red/blue fallback");
  }
  dfn.vkDestroyShaderModule(device, swap_apply_gamma_pipeline_stages[0].module, nullptr);
  for (VkShaderModule swap_apply_gamma_pixel_shader : swap_apply_gamma_pixel_shaders) {
    assert_true(swap_apply_gamma_pixel_shader != VK_NULL_HANDLE);
    dfn.vkDestroyShaderModule(device, swap_apply_gamma_pixel_shader, nullptr);
  }
  if (std::any_of(swap_apply_gamma_pipeline_create_results.begin(),
                  swap_apply_gamma_pipeline_create_results.end(),
                  [](VkResult result) { return result != VK_SUCCESS; })) {
    REXGPU_ERROR("Failed to create the gamma ramp application pipelines");
    return false;
  }

  // Gamma ramp application compute pipelines.
  swap_apply_gamma_compute_256_entry_table_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, swap_apply_gamma_compute_pipeline_layout_, shaders::apply_gamma_table_cs,
      sizeof(shaders::apply_gamma_table_cs));
  if (swap_apply_gamma_compute_256_entry_table_pipeline_ == VK_NULL_HANDLE) {
    REXGPU_WARN(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline, keeping graphics fallback");
  }
  swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_ =
      ui::vulkan::util::CreateComputePipeline(
          vulkan_device, swap_apply_gamma_compute_pipeline_layout_,
          shaders::apply_gamma_table_fxaa_luma_cs, sizeof(shaders::apply_gamma_table_fxaa_luma_cs));
  if (swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_ == VK_NULL_HANDLE) {
    REXGPU_WARN(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline with luma output");
  }
  swap_apply_gamma_compute_pwl_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, swap_apply_gamma_compute_pipeline_layout_, shaders::apply_gamma_pwl_cs,
      sizeof(shaders::apply_gamma_pwl_cs));
  if (swap_apply_gamma_compute_pwl_pipeline_ == VK_NULL_HANDLE) {
    REXGPU_WARN("Failed to create the PWL gamma ramp application compute pipeline");
  }
  swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, swap_apply_gamma_compute_pipeline_layout_,
      shaders::apply_gamma_pwl_fxaa_luma_cs, sizeof(shaders::apply_gamma_pwl_fxaa_luma_cs));
  if (swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_ == VK_NULL_HANDLE) {
    REXGPU_WARN(
        "Failed to create the PWL gamma ramp application compute pipeline with "
        "luma output");
  }
  if (!vulkan_device->properties().imageViewFormatSwizzle) {
    auto create_rb_swap_compute_pipeline = [&](const char* source, VkPipeline& pipeline_out,
                                               const char* pipeline_name) {
      std::vector<uint32_t> compute_spirv;
      std::string compile_error;
      if (!CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, source, compute_spirv, compile_error)) {
        REXGPU_WARN("Failed to compile {} shader to SPIR-V: {}", pipeline_name, compile_error);
        return;
      }
      pipeline_out = ui::vulkan::util::CreateComputePipeline(
          vulkan_device, swap_apply_gamma_compute_pipeline_layout_, compute_spirv.data(),
          sizeof(uint32_t) * compute_spirv.size());
      if (pipeline_out == VK_NULL_HANDLE) {
        REXGPU_WARN("Failed to create {} pipeline", pipeline_name);
      }
    };
    create_rb_swap_compute_pipeline(GetSwapApplyGammaTableComputeRbSwapSource(),
                                    swap_apply_gamma_compute_256_entry_table_rb_swap_pipeline_,
                                    "swap gamma table compute red/blue fallback");
    create_rb_swap_compute_pipeline(GetSwapApplyGammaPwlComputeRbSwapSource(),
                                    swap_apply_gamma_compute_pwl_rb_swap_pipeline_,
                                    "swap gamma PWL compute red/blue fallback");
    create_rb_swap_compute_pipeline(
        GetSwapApplyGammaTableFxaaLumaComputeRbSwapSource(),
        swap_apply_gamma_compute_256_entry_table_fxaa_luma_rb_swap_pipeline_,
        "swap gamma table FXAA-luma compute red/blue fallback");
    create_rb_swap_compute_pipeline(GetSwapApplyGammaPwlFxaaLumaComputeRbSwapSource(),
                                    swap_apply_gamma_compute_pwl_fxaa_luma_rb_swap_pipeline_,
                                    "swap gamma PWL FXAA-luma compute red/blue fallback");
  }

  // FXAA compute pipelines, compiled to SPIR-V at runtime.
  std::vector<uint32_t> swap_fxaa_spirv;
  std::string swap_fxaa_compile_error;
  if (!CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, GetSwapFxaaComputeSource(false),
                          swap_fxaa_spirv, swap_fxaa_compile_error)) {
    REXGPU_WARN("Failed to compile FXAA compute shader to SPIR-V: {}", swap_fxaa_compile_error);
  } else {
    swap_fxaa_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, swap_fxaa_pipeline_layout_, swap_fxaa_spirv.data(),
        sizeof(uint32_t) * swap_fxaa_spirv.size());
    if (swap_fxaa_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_WARN("Failed to create the FXAA compute pipeline");
    }
  }

  std::vector<uint32_t> swap_fxaa_extreme_spirv;
  std::string swap_fxaa_extreme_compile_error;
  if (!CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, GetSwapFxaaComputeSource(true),
                          swap_fxaa_extreme_spirv, swap_fxaa_extreme_compile_error)) {
    REXGPU_WARN("Failed to compile extreme FXAA compute shader to SPIR-V: {}",
                swap_fxaa_extreme_compile_error);
  } else {
    swap_fxaa_extreme_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, swap_fxaa_pipeline_layout_, swap_fxaa_extreme_spirv.data(),
        sizeof(uint32_t) * swap_fxaa_extreme_spirv.size());
    if (swap_fxaa_extreme_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_WARN("Failed to create the extreme-quality FXAA compute pipeline");
    }
  }

  VkPipelineLayoutCreateInfo resolve_downscale_layout_create_info = {};
  resolve_downscale_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  VkDescriptorSetLayout resolve_downscale_set_layout =
      descriptor_set_layouts_single_transient_[size_t(
          SingleTransientDescriptorLayout::kStorageBufferPairCompute)];
  resolve_downscale_layout_create_info.setLayoutCount = 1;
  resolve_downscale_layout_create_info.pSetLayouts = &resolve_downscale_set_layout;
  VkPushConstantRange resolve_downscale_push_constant_range = {};
  resolve_downscale_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  resolve_downscale_push_constant_range.offset = 0;
  resolve_downscale_push_constant_range.size = sizeof(ResolveDownscaleConstants);
  resolve_downscale_layout_create_info.pushConstantRangeCount = 1;
  resolve_downscale_layout_create_info.pPushConstantRanges = &resolve_downscale_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &resolve_downscale_layout_create_info, nullptr,
                                 &resolve_downscale_pipeline_layout_) == VK_SUCCESS) {
    resolve_downscale_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_downscale_pipeline_layout_, shaders::resolve_downscale_cs,
        sizeof(shaders::resolve_downscale_cs));
    if (resolve_downscale_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_WARN("Failed to create Vulkan resolve-downscale readback pipeline");
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                             resolve_downscale_pipeline_layout_);
    }
  } else {
    REXGPU_WARN("Failed to create Vulkan resolve-downscale pipeline layout");
  }

  occlusion_query_resources_available_ = InitializeOcclusionQueryResources();

  // Just not to expose uninitialized memory.
  std::memset(&system_constants_, 0, sizeof(system_constants_));

  if (REXCVAR_GET(vulkan_threaded_submission)) {
    submission_thread_shutdown_ = false;
    submission_thread_failed_ = false;
    submission_thread_device_lost_ = false;
    submission_thread_submitted_ = 0;
    submission_thread_ = rex::thread::Thread::Create({}, [this]() { SubmissionThread(); });
    if (submission_thread_) {
      submission_thread_->set_name("Vulkan Submission");
    } else {
      REXGPU_WARN("Failed to create the Vulkan submission thread, submitting on the GPU thread");
    }
  }

  Fh1WorldAoInitialize();
  return true;
}

VkResult VulkanCommandProcessor::ReplayAndSubmit(
    const CommandBuffer& buffer, const DeferredCommandBuffer::Streams& streams,
    VkFence fence, const std::vector<VkSemaphore>& waits,
    const std::vector<VkPipelineStageFlags>& stages) {
  const auto* device = GetVulkanDevice();
  const auto& f = device->functions();
  VkResult result = f.vkResetCommandPool(device->device(), buffer.pool, 0);
  if (result != VK_SUCCESS) return result;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  result = f.vkBeginCommandBuffer(buffer.buffer, &begin);
  if (result != VK_SUCCESS) return result;
  // Ordinary submissions retain the existing allocation-free replay path.
  if (!streams.native_upscale) {
    VkCommandBuffer command = deferred_command_buffer_.ExecuteStreams(buffer.buffer, streams);
    if (!command) return VK_ERROR_DEVICE_LOST;
    result = f.vkEndCommandBuffer(command);
    if (result != VK_SUCCESS) return result;
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.waitSemaphoreCount = uint32_t(waits.size());
    info.pWaitSemaphores = waits.data(); info.pWaitDstStageMask = stages.data();
    info.commandBufferCount = 1; info.pCommandBuffers = &command;
    auto queue = device->AcquireQueue(device->queue_family_graphics_compute(), 0);
    return f.vkQueueSubmit(queue.queue(), 1, &info, fence);
  }
  std::vector<VkSemaphore> pending_waits = waits;
  std::vector<VkPipelineStageFlags> pending_stages = stages;
  std::vector<uint64_t> pending_values(waits.size(), 0);
  bool timeline_wait = false, split_used = false;
  auto submit = [&](VkCommandBuffer command, VkSemaphore signal, uint64_t value, VkFence completion) {
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount = uint32_t(pending_values.size());
    timeline.pWaitSemaphoreValues = pending_values.data();
    timeline.signalSemaphoreValueCount = signal ? 1 : 0;
    timeline.pSignalSemaphoreValues = signal ? &value : nullptr;
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (signal || timeline_wait) info.pNext = &timeline;
    info.waitSemaphoreCount = uint32_t(pending_waits.size());
    info.pWaitSemaphores = pending_waits.data(); info.pWaitDstStageMask = pending_stages.data();
    info.commandBufferCount = 1; info.pCommandBuffers = &command;
    info.signalSemaphoreCount = signal ? 1 : 0; info.pSignalSemaphores = signal ? &signal : nullptr;
    auto queue = device->AcquireQueue(device->queue_family_graphics_compute(), 0);
    return f.vkQueueSubmit(queue.queue(), 1, &info, completion);
  };
  Fh1Dlss::SplitSubmission split = [&](VkCommandBuffer& command, const Fh1Dlss::NativeSubmission& native) {
    // One reconstruction point per submission. Both buffers live until the final fence.
    if (split_used || !buffer.continuation || !native.ready || !native.complete || !native.encode)
      return false;
    result = f.vkEndCommandBuffer(command);
    if (result != VK_SUCCESS) return false;
    result = submit(command, native.ready, native.value, VK_NULL_HANDLE);
    if (result != VK_SUCCESS || !native.encode()) return false;
    command = buffer.continuation;
    result = f.vkBeginCommandBuffer(command, &begin);
    if (result != VK_SUCCESS) return false;
    pending_waits = {native.complete}; pending_values = {native.value};
    pending_stages = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    timeline_wait = split_used = true;
    return true;
  };
  VkCommandBuffer command = deferred_command_buffer_.ExecuteStreams(buffer.buffer, streams, split);
  if (!command) return result == VK_SUCCESS ? VK_ERROR_DEVICE_LOST : result;
  result = f.vkEndCommandBuffer(command);
  if (result != VK_SUCCESS) return result;
  return submit(command, VK_NULL_HANDLE, 0, fence);
}

void VulkanCommandProcessor::SubmissionThread() {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  while (true) {
    SubmissionJob job;
    {
      std::unique_lock<std::mutex> lock(submission_thread_mutex_);
      submission_thread_request_cond_.wait(lock, [this] {
        return submission_thread_shutdown_ || !submission_thread_jobs_.empty();
      });
      if (submission_thread_jobs_.empty()) {
        return;
      }
      job = std::move(submission_thread_jobs_.front());
      submission_thread_jobs_.pop_front();
    }
    // After a failure the following submissions are dropped: their fences are never waited for
    // (the command processor treats the device as lost).
    if (!submission_thread_failed_.load(std::memory_order_relaxed)) {
      const VkResult result = ReplayAndSubmit(job.command_buffer, job.streams, job.fence,
                                              job.wait_semaphores, job.wait_stage_masks);
      const char* failure = result == VK_SUCCESS ? nullptr : "Failed to replay/submit Vulkan work";
      if (failure) {
        REXGPU_ERROR("{} (submission {}, threaded submission)", failure, job.submission);
        if (result == VK_ERROR_DEVICE_LOST) {
          submission_thread_device_lost_ = true;
        }
        submission_thread_failed_ = true;
      }
    }
    job.streams.main.clear();
    job.streams.hoisted.clear();
    {
      std::lock_guard<std::mutex> lock(submission_thread_mutex_);
      submission_thread_free_streams_.push_back(std::move(job.streams));
      submission_thread_submitted_.store(job.submission, std::memory_order_release);
    }
    submission_thread_done_cond_.notify_all();
  }
}

void VulkanCommandProcessor::AwaitSubmissionThread(uint64_t submission) {
  if (!submission_thread_ ||
      submission_thread_submitted_.load(std::memory_order_acquire) >= submission) {
    return;
  }
  std::unique_lock<std::mutex> lock(submission_thread_mutex_);
  submission_thread_done_cond_.wait(lock, [this, submission] {
    return submission_thread_submitted_.load(std::memory_order_acquire) >= submission;
  });
}

void VulkanCommandProcessor::ShutdownSubmissionThread() {
  if (!submission_thread_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(submission_thread_mutex_);
    submission_thread_shutdown_ = true;
  }
  submission_thread_request_cond_.notify_all();
  rex::thread::Wait(submission_thread_.get(), false);
  submission_thread_.reset();
  submission_thread_jobs_.clear();
  submission_thread_free_streams_.clear();
}

void VulkanCommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();
  Fh1WorldAoShutdown();
  ShutdownSubmissionThread();
  InvalidateAllVertexBufferResidency();
  ShutdownOcclusionQueryResources();

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  DestroyScratchBuffer();

  for (auto& readback_pair : readback_buffers_) {
    ReadbackBuffer& readback = readback_pair.second;
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.mapped_data[i] && readback.memories[i] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[i]);
      }
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, readback.buffers[i]);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, readback.memories[i]);
      readback.mapped_data[i] = nullptr;
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  readback_buffers_.clear();
  for (auto& readback_pair : memexport_readback_buffers_) {
    ReadbackBuffer& readback = readback_pair.second;
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.mapped_data[i] && readback.memories[i] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[i]);
      }
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, readback.buffers[i]);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, readback.memories[i]);
      readback.mapped_data[i] = nullptr;
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  memexport_readback_buffers_.clear();

  resolve_downscale_buffer_size_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, resolve_downscale_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         resolve_downscale_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         resolve_downscale_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_downscale_pipeline_layout_);

  for (SwapFramebuffer& swap_framebuffer : swap_framebuffers_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                           swap_framebuffer.framebuffer);
  }
  DestroySwapFxaaSourceImage();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_fxaa_extreme_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, swap_fxaa_pipeline_);
  for (auto& scene : hdr_scenes_)
    DestroyHdrScene(scene);
  Fh1VrDestroy();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, swap_hud_mask_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_hud_mask_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         swap_hud_mask_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         swap_hud_mask_set_layout_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_compute_pwl_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_compute_pwl_fxaa_luma_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_compute_pwl_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device, swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device,
      swap_apply_gamma_compute_256_entry_table_fxaa_luma_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_compute_256_entry_table_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device, swap_apply_gamma_compute_256_entry_table_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_pwl_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_256_entry_table_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_pwl_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_pwl_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_256_entry_table_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device,
                                         swap_apply_gamma_render_pass_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_fxaa_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_apply_gamma_compute_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_apply_gamma_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroySampler, device, swap_sampler_linear_clamp_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         swap_descriptor_pool_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         swap_descriptor_set_layout_uniform_texel_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         swap_descriptor_set_layout_storage_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         swap_descriptor_set_layout_combined_image_sampler_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         swap_descriptor_set_layout_sampled_image_);
  for (VkBufferView& gamma_ramp_buffer_view : gamma_ramp_buffer_views_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBufferView, device, gamma_ramp_buffer_view);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, gamma_ramp_upload_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         gamma_ramp_upload_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, gamma_ramp_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, gamma_ramp_buffer_memory_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         shared_memory_and_edram_descriptor_pool_);

  fh1_dlss_.reset();
  if (fh1_resolve_pipeline_) dfn.vkDestroyPipeline(device, fh1_resolve_pipeline_, nullptr);
  if (fh1_resolve_layout_) dfn.vkDestroyPipelineLayout(device, fh1_resolve_layout_, nullptr);
  fh1_resolve_pipeline_ = VK_NULL_HANDLE;
  fh1_resolve_layout_ = VK_NULL_HANDLE;
  fh1_other_textures_.reset();
  texture_cache_.reset();

  fh1_other_pipelines_.reset();
  pipeline_cache_.reset();

  fh1_other_render_targets_.reset();
  render_target_cache_.reset();

  primitive_processor_.reset();

  shared_memory_.reset();

  ClearTransientDescriptorPools();

  for (const auto& pipeline_layout_pair : pipeline_layouts_) {
    dfn.vkDestroyPipelineLayout(device, pipeline_layout_pair.second.GetPipelineLayout(), nullptr);
  }
  pipeline_layouts_.clear();
  for (const auto& descriptor_set_layout_pair : descriptor_set_layouts_textures_) {
    dfn.vkDestroyDescriptorSetLayout(device, descriptor_set_layout_pair.second, nullptr);
  }
  descriptor_set_layouts_textures_.clear();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_shared_memory_and_edram_);
  for (VkDescriptorSetLayout& descriptor_set_layout_single_transient :
       descriptor_set_layouts_single_transient_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                           descriptor_set_layout_single_transient);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_constants_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_empty_);

  uniform_buffer_pool_.reset();

  sparse_bind_wait_stage_mask_ = 0;
  sparse_buffer_binds_.clear();
  sparse_memory_binds_.clear();

  deferred_command_buffer_.Reset();
  for (const auto& command_buffer_pair : command_buffers_submitted_) {
    dfn.vkDestroyCommandPool(device, command_buffer_pair.second.pool, nullptr);
  }
  command_buffers_submitted_.clear();
  for (const CommandBuffer& command_buffer : command_buffers_writable_) {
    dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
  }
  command_buffers_writable_.clear();

  for (const auto& destroy_pair : destroy_framebuffers_) {
    dfn.vkDestroyFramebuffer(device, destroy_pair.second, nullptr);
  }
  destroy_framebuffers_.clear();
  for (const auto& destroy_pair : destroy_image_views_) {
    dfn.vkDestroyImageView(device, destroy_pair.second, nullptr);
  }
  destroy_image_views_.clear();
  for (const auto& destroy_pair : destroy_buffers_) {
    dfn.vkDestroyBuffer(device, destroy_pair.second, nullptr);
  }
  destroy_buffers_.clear();
  for (const auto& destroy_pair : destroy_images_) {
    dfn.vkDestroyImage(device, destroy_pair.second, nullptr);
  }
  destroy_images_.clear();
  for (const auto& destroy_pair : destroy_memory_) {
    dfn.vkFreeMemory(device, destroy_pair.second, nullptr);
  }
  destroy_memory_.clear();

  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));
  frame_completed_ = 0;
  frame_current_ = 1;
  frame_open_ = false;

  for (const auto& semaphore : submissions_in_flight_semaphores_) {
    dfn.vkDestroySemaphore(device, semaphore.second, nullptr);
  }
  submissions_in_flight_semaphores_.clear();
  for (VkFence& fence : submissions_in_flight_fences_) {
    dfn.vkDestroyFence(device, fence, nullptr);
  }
  submissions_in_flight_fences_.clear();
  current_submission_wait_stage_masks_.clear();
  for (VkSemaphore semaphore : current_submission_wait_semaphores_) {
    dfn.vkDestroySemaphore(device, semaphore, nullptr);
  }
  current_submission_wait_semaphores_.clear();
  submission_completed_ = 0;
  submission_open_ = false;

  for (VkSemaphore semaphore : semaphores_free_) {
    dfn.vkDestroySemaphore(device, semaphore, nullptr);
  }
  semaphores_free_.clear();
  for (VkFence fence : fences_free_) {
    dfn.vkDestroyFence(device, fence, nullptr);
  }
  fences_free_.clear();

  device_lost_ = false;
  fh1_device_memory::Get().Shutdown();

  CommandProcessor::ShutdownContext();
}

void VulkanCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  // Only the shader constant blocks are handled here (WriteRegistersFromMem
  // relies on that for the other registers).
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    if (frame_open_) {
      uint32_t float_constant_index = (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (float_constant_index >= 256) {
        float_constant_index -= 256;
        if (current_float_constant_map_pixel_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
        }
      } else {
        if (current_float_constant_map_vertex_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
        }
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
                                                  6);
      if (fh1_other_textures_) fh1_other_textures_->TextureFetchConstantWritten(
          (index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6);
    }
    InvalidateVertexBufferResidency((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 2);
  }
}

namespace {
// Registers outside the shader constant blocks whose write only stores the value:
// known to the register table, and none of those CommandProcessor::WriteRegister
// acts on (scratch write-back, coherency status, gamma ramp; keep in sync with it).
// Unknown and out-of-range registers still take that function, which logs them.
class PlainRegisters {
 public:
  PlainRegisters() {
    for (uint32_t i = 0; i < RegisterFile::kRegisterCount; ++i) {
      bool side_effect = (i >= XE_GPU_REG_SCRATCH_REG0 && i <= XE_GPU_REG_SCRATCH_REG7) ||
                         i == XE_GPU_REG_COHER_STATUS_HOST || i == XE_GPU_REG_DC_LUT_RW_INDEX ||
                         i == XE_GPU_REG_DC_LUT_SEQ_COLOR || i == XE_GPU_REG_DC_LUT_PWL_DATA ||
                         i == XE_GPU_REG_DC_LUT_30_COLOR;
      // FH1 (#45, patch 0360): not the shader constant blocks either (WriteRegistersFromMem
      // takes those first and never asks for them; the packet loop's own writes,
      // CommandProcessor::fh1_plain_registers_, must not skip their handling).
      bool constant = (i >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
                       i <= XE_GPU_REG_SHADER_CONSTANT_511_W) ||
                      (i >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
                       i <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) ||
                      (i >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
                       i <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31);
      if (!side_effect && !constant && RegisterFile::GetRegisterInfo(i)) {
        bits_[i >> 6] |= uint64_t(1) << (i & 63);
      }
    }
  }
  bool IsPlain(uint32_t index) const {
    return index < RegisterFile::kRegisterCount && ((bits_[index >> 6] >> (index & 63)) & 1);
  }
  const uint64_t* bits() const { return bits_; }

 private:
  uint64_t bits_[(RegisterFile::kRegisterCount + 63) / 64] = {};
};
const PlainRegisters plain_registers;
}  // namespace

void VulkanCommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                   uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  uint32_t end_index = start_index + num_registers - 1;

  auto range_has_any_constant_usage = [](const uint64_t* usage_map, uint32_t first_constant,
                                         uint32_t last_constant) -> bool {
    if (first_constant > last_constant) {
      return false;
    }
    uint32_t first_word = first_constant >> 6;
    uint32_t last_word = last_constant >> 6;
    uint32_t first_bit = first_constant & 63;
    uint32_t last_bit = last_constant & 63;
    if (first_word == last_word) {
      uint32_t bit_count = last_bit - first_bit + 1;
      uint64_t mask = bit_count == 64 ? UINT64_MAX : ((UINT64_C(1) << bit_count) - 1) << first_bit;
      return (usage_map[first_word] & mask) != 0;
    }
    if (usage_map[first_word] & (UINT64_MAX << first_bit)) {
      return true;
    }
    for (uint32_t word = first_word + 1; word < last_word; ++word) {
      if (usage_map[word]) {
        return true;
      }
    }
    uint64_t last_mask = last_bit == 63 ? UINT64_MAX : ((UINT64_C(1) << (last_bit + 1)) - 1);
    return (usage_map[last_word] & last_mask) != 0;
  };

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    if (frame_open_) {
      uint32_t first_float_constant = (start_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      uint32_t last_float_constant = (end_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (first_float_constant < 256) {
        uint32_t last_vertex_constant = std::min(last_float_constant, 255u);
        if (range_has_any_constant_usage(current_float_constant_map_vertex_, first_float_constant,
                                         last_vertex_constant)) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
        }
      }
      if (last_float_constant >= 256) {
        uint32_t first_pixel_constant =
            first_float_constant >= 256 ? first_float_constant - 256 : 0;
        uint32_t last_pixel_constant = last_float_constant - 256;
        if (range_has_any_constant_usage(current_float_constant_map_pixel_, first_pixel_constant,
                                         last_pixel_constant)) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
        }
      }
    }
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    // Games rewrite many fetch constants with the values they already have. The
    // fetch constant buffer, the texture bindings and the vertex buffer
    // residency only depend on the values, so invalidate them for the dwords
    // that change only.
    uint32_t* values = register_file_->values + start_index;
    uint32_t first_changed = UINT32_MAX, last_changed = 0;
    for (uint32_t i = 0; i < num_registers; ++i) {
      uint32_t value = memory::load_and_swap<uint32_t>(base + i);
      if (values[i] != value) {
        values[i] = value;
        if (first_changed == UINT32_MAX) {
          first_changed = i;
        }
        last_changed = i;
      }
    }
    if (first_changed == UINT32_MAX) {
      return;
    }
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);
    uint32_t first_fetch_dword =
        start_index + first_changed - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    uint32_t last_fetch_dword = start_index + last_changed - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantsWritten(first_fetch_dword / 6, last_fetch_dword / 6);
      if (fh1_other_textures_) fh1_other_textures_->TextureFetchConstantsWritten(
          first_fetch_dword / 6, last_fetch_dword / 6);
    }
    InvalidateVertexBufferResidencyRange(first_fetch_dword / 2, last_fetch_dword / 2);
    return;
  }

  // A range with other registers, or across blocks: split it. The parts in the
  // constant blocks take the paths above; WriteRegister handles only those
  // blocks itself, so the other registers go straight to the common handling,
  // without a virtual call for each.
  uint32_t index = start_index;
  const uint32_t range_end = start_index + num_registers;
  while (index < range_end) {
    uint32_t part_end;
    bool constant_block = true;
    if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
      part_end = XE_GPU_REG_SHADER_CONSTANT_511_W + 1;
    } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
               index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
      part_end = XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 + 1;
    } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
               index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
      part_end = XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1;
    } else {
      constant_block = false;
      if (index < XE_GPU_REG_SHADER_CONSTANT_000_X) {
        part_end = XE_GPU_REG_SHADER_CONSTANT_000_X;
      } else if (index < XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) {
        part_end = XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
      } else if (index < XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031) {
        part_end = XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031;
      } else {
        part_end = range_end;
      }
    }
    part_end = std::min(part_end, range_end);
    uint32_t* part_base = base + (index - start_index);
    if (constant_block) {
      VulkanCommandProcessor::WriteRegistersFromMem(index, part_base, part_end - index);
    } else {
      // Most of these (render state, about 3 million a second in a race) only store
      // the value: do that here, as CommandProcessor::WriteRegister would.
      for (uint32_t i = index; i < part_end; ++i) {
        uint32_t value = memory::load_and_swap<uint32_t>(part_base + (i - index));
        if (plain_registers.IsPlain(i)) {
          // Volatile like CommandProcessor::WriteRegister's store (WAIT_REG_MEM polls).
          const_cast<volatile uint32_t&>(register_file_->values[i]) = value;
        } else {
          CommandProcessor::WriteRegister(i, value);
        }
      }
    }
    index = part_end;
  }
}

void VulkanCommandProcessor::SparseBindBuffer(VkBuffer buffer, uint32_t bind_count,
                                              const VkSparseMemoryBind* binds,
                                              VkPipelineStageFlags wait_stage_mask) {
  if (!bind_count) {
    return;
  }
  SparseBufferBind& buffer_bind = sparse_buffer_binds_.emplace_back();
  buffer_bind.buffer = buffer;
  buffer_bind.bind_offset = sparse_memory_binds_.size();
  buffer_bind.bind_count = bind_count;
  sparse_memory_binds_.reserve(sparse_memory_binds_.size() + bind_count);
  sparse_memory_binds_.insert(sparse_memory_binds_.end(), binds, binds + bind_count);
  sparse_bind_wait_stage_mask_ |= wait_stage_mask;
}

void VulkanCommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
}

void VulkanCommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_current_frame_ = UINT32_MAX;
}

bool VulkanCommandProcessor::EnsureSwapHudMask() {
  if (swap_hud_mask_pipeline_) return true;
  if (swap_hud_mask_failed_) return false;
  swap_hud_mask_failed_ = true;
  const std::array signature{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
  auto objects = ui::vulkan::ImageComputeObjects::Create(*GetVulkanDevice(), shaders::hdr_hud_mask_cs,
      signature, kMaxFramesInFlight, sizeof(uint32_t) * 4, ui::vulkan::BuildImageComputePipeline);
  if (!objects) {
    REXGPU_WARN("HDR coverage pipeline construction failed; retaining paper-white output");
    return false;
  }
  swap_hud_mask_set_layout_ = objects->set_layout;
  swap_hud_mask_pipeline_layout_ = objects->layout;
  swap_hud_mask_descriptor_pool_ = objects->pool;
  swap_hud_mask_pipeline_ = objects->pipeline;
  std::copy(objects->sets.begin(), objects->sets.end(), swap_hud_mask_descriptor_sets_.begin());
  objects->Release();
  swap_hud_mask_failed_ = false;
  return true;
}

void VulkanCommandProcessor::DestroyHdrScene(HdrScene& scene) {
  const auto* gpu = GetVulkanDevice();
  const auto& dfn = gpu->functions();
  const auto device = gpu->device();
  if (scene.view)
    dfn.vkDestroyImageView(device, scene.view, nullptr);
  if (scene.image)
    dfn.vkDestroyImage(device, scene.image, nullptr);
  if (scene.memory)
    dfn.vkFreeMemory(device, scene.memory, nullptr);
  scene = {};
}

// Patch 0393: VR layers. The HDR path's pre-HUD snapshot (CaptureHdrScene, also taken while a VR
// client asks for layers) goes through the swap's own gamma pipeline into a slot's scene image, so
// it matches the guest output (gamma ramp, fades, channel order); then the HUD alone is recovered
// from the two pictures at the HUD's 1280 x 720. Both are recorded in the swap refresher, in the
// guest output's submission, and handed to the export with the frame. A slot is reused once the
// export and its client have let go of it and this processor's last use of it is complete.
struct VulkanCommandProcessor::Fh1VrSlot {
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };
  Image scene, hud;
  Image stereo, keys;  // patch 0394: both eyes side by side, and the splat's depth keys
  Image sharp;         // patch 0395: the sharpened scene (then swapped with `scene`)
  Image left;          // patch 0396: true stereo's left eye through the gamma ramp
  uint32_t width = 0, height = 0;
  uint64_t submission = 0;
};

namespace {
constexpr uint32_t kFh1VrHudWidth = 1280, kFh1VrHudHeight = 720;
constexpr size_t kFh1VrMaxSlots = 6;

// Patch 0394: the depth pre-pass as view distance in metres (0: the sky), with the shadow
// passes' own constants (fh1_world_ao.h, Projection).
constexpr char kFh1VrDepthShader[] = R"glsl(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8) in;
layout(push_constant) uniform Params { vec4 proj; uvec4 size; } pc;
layout(set = 0, binding = 0) uniform texture2DArray depth;  // the pre-pass, 0 far .. 1 near
layout(set = 0, binding = 1, r32f) uniform writeonly image2D z;
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(p), pc.size.xy))) return;
  float d = texelFetch(depth, ivec3(p, 0), 0).r;
  float den = 1.0 - pc.proj.x - d;
  float v = abs(pc.proj.y / (abs(den) > 1e-20 ? den : 1e-20));
  imageStore(z, p, vec4(d <= 0.0 || isinf(v) || isnan(v) ? 0.0 : v));
}
)glsl";

// Patch 0394: each eye from the one (centre) view and its depth. Splat: every scene pixel moves
// by its disparity to each eye, the nearest per target pixel winning (a 20-bit log depth over
// 12 bits of the move). Resolve: a pixel no scene pixel reached (behind a nearer object as seen
// from the centre) takes the background beside the hole, continued with its own move.
constexpr char kFh1VrStereoCommon[] = R"glsl(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8) in;
// eye: half the eyes' distance (m), the focal length (pixels), the most a pixel moves (pixels);
// size: scene width, height.
layout(push_constant) uniform Params { vec4 eye; uvec4 size; } pc;
layout(set = 0, binding = 0) uniform texture2D z;
layout(set = 0, binding = 1, r32ui) uniform uimage2D keys;  // 2W x H
layout(set = 0, binding = 2) uniform texture2D scene;
layout(set = 0, binding = 3, rgb10_a2) uniform writeonly image2D stereo;  // 2W x H
const uint kEmpty = 0xFFFFFFFFu;
float Move(float d) { return d <= 0.0 ? 0.0 : min(pc.eye.x * pc.eye.y / d, pc.eye.z); }
uint Key(float d, int move) {
  float t = d <= 0.0 ? 1.0 : clamp(log2(max(d, 0.01) / 0.01) / 20.0, 0.0, 1.0);
  return (uint(t * 1048575.0) << 12) | uint(clamp(move + 2048, 0, 4095));
}
)glsl";

constexpr char kFh1VrStereoSplat[] = R"glsl(
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  int w = int(pc.size.x);
  if (p.x >= w || p.y >= int(pc.size.y)) return;
  float d = texelFetch(z, p, 0).r;
  int m = int(round(Move(d)));
  // The left eye sees a point further right than the centre does, the right eye further left.
  int xl = p.x + m, xr = p.x - m;
  if (xl < w) imageAtomicMin(keys, ivec2(xl, p.y), Key(d, m));
  if (xr >= 0) imageAtomicMin(keys, ivec2(w + xr, p.y), Key(d, -m));
}
)glsl";

constexpr char kFh1VrStereoResolve[] = R"glsl(
void main() {
  ivec2 q = ivec2(gl_GlobalInvocationID.xy);
  int w = int(pc.size.x);
  if (q.x >= 2 * w || q.y >= int(pc.size.y)) return;
  int base = q.x >= w ? w : 0;
  int x = q.x - base;
  uint k = imageLoad(keys, q).r;
  if (k == kEmpty) {
    // A hole: the farther of the nearest reached pixels on either side is the background.
    uint best = 0u;
    int reach = int(pc.eye.z) + 2;
    for (int i = 1; i <= reach; ++i) {
      if (x - i >= 0) {
        uint l = imageLoad(keys, ivec2(base + x - i, q.y)).r;
        if (l != kEmpty) { best = max(best, l); break; }
      }
    }
    for (int i = 1; i <= reach; ++i) {
      if (x + i < w) {
        uint r = imageLoad(keys, ivec2(base + x + i, q.y)).r;
        if (r != kEmpty) {
          if ((r >> 12) > (best >> 12) || best == 0u) best = r;
          break;
        }
      }
    }
    k = best == 0u ? (2048u) : best;
  }
  int move = int(k & 4095u) - 2048;
  int src = clamp(x - move, 0, w - 1);
  imageStore(stereo, q, vec4(texelFetch(scene, ivec2(src, q.y), 0).rgb, 1.0));
}
)glsl";

// The HUD alone from the frame with it (`frame`) and the same frame before it (`scene`), both
// after the gamma ramp. The game blends the HUD over the scene: frame = scene * (1 - a) + c * a.
// Per pixel the least coverage a that explains the change with a colour c in [0, 1] (a dark
// translucent backdrop gives black at its own opacity, white text white), then a box filter down
// to the HUD's own size in premultiplied colour. Unchanged pixels (and changes within 10-bit
// rounding) are transparent.
constexpr char kFh1VrHudShader[] = R"glsl(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8) in;
layout(push_constant) uniform Params { uvec4 size; } pc;  // frame width, height; HUD width, height
layout(set = 0, binding = 0) uniform texture2D frame;
layout(set = 0, binding = 1) uniform texture2D scene;
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D hud;
void main() {
  uvec2 p = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(p, pc.size.zw))) return;
  uvec2 lo = p * pc.size.xy / pc.size.zw;
  uvec2 hi = max((p + 1u) * pc.size.xy / pc.size.zw, lo + 1u);
  vec4 sum = vec4(0.0);
  for (uint y = lo.y; y < hi.y; ++y) {
    for (uint x = lo.x; x < hi.x; ++x) {
      vec3 f = texelFetch(frame, ivec2(x, y), 0).rgb;
      vec3 s = texelFetch(scene, ivec2(x, y), 0).rgb;
      vec3 d = f - s;
      if (max(abs(d.r), max(abs(d.g), abs(d.b))) < 0.004) continue;
      vec3 darker = -d / max(s, vec3(1e-3));
      vec3 lighter = d / max(vec3(1.0) - s, vec3(1e-3));
      vec3 a3 = max(darker, lighter);
      float a = clamp(max(a3.r, max(a3.g, a3.b)), 0.0, 1.0);
      sum += vec4(clamp(f - s * (1.0 - a), 0.0, 1.0), a);
    }
  }
  sum /= float((hi.x - lo.x) * (hi.y - lo.y));
  vec3 colour = sum.a > 0.0 ? clamp(sum.rgb / sum.a, 0.0, 1.0) : vec3(0.0);
  imageStore(hud, ivec2(p), vec4(colour, sum.a));
}
)glsl";
}  // namespace

bool VulkanCommandProcessor::Fh1VrLayersWanted() const {
  return !fh1_vr_failed_ &&
         (rexui_GuestOutputExportFlags() & REX_GUEST_OUTPUT_EXPORT_LAYERS) != 0;
}

bool VulkanCommandProcessor::Fh1VrEnsurePipelines() {
  if (fh1_vr_hud_pipeline_) return true;
  if (fh1_vr_failed_) return false;
  fh1_vr_failed_ = true;
  std::vector<uint32_t> shader;
  std::string error;
  if (!CompileFh1Compute(kFh1VrHudShader, shader, error)) {
    REXGPU_WARN("VR HUD shader compilation failed: {}", error);
    return false;
  }
  const auto& device = *GetVulkanDevice();
  const std::array signature{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
  // The gamma conversion uses one sampled and one storage descriptor set per
  // frame, in addition to the three-image HUD separation effect.
  const std::array extra{
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxFramesInFlight},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxFramesInFlight}};
  auto objects = ui::vulkan::ImageComputeObjects::Create(device, shader, signature,
      kMaxFramesInFlight, sizeof(uint32_t) * 4, ui::vulkan::BuildImageComputePipeline,
      2 * kMaxFramesInFlight, extra);
  std::array<VkDescriptorSet, kMaxFramesInFlight> gamma_sources{}, gamma_destinations{};
  if (!objects ||
      !ui::vulkan::AllocateImageSets(device, objects->pool, swap_descriptor_set_layout_sampled_image_,
                                    gamma_sources) ||
      !ui::vulkan::AllocateImageSets(device, objects->pool, swap_descriptor_set_layout_storage_image_,
                                    gamma_destinations)) {
    REXGPU_WARN("VR HUD effect allocation failed");
    return false;
  }
  fh1_vr_gamma_source_sets_ = gamma_sources;
  fh1_vr_gamma_destination_sets_ = gamma_destinations;
  fh1_vr_hud_set_layout_ = objects->set_layout;
  fh1_vr_hud_pipeline_layout_ = objects->layout;
  fh1_vr_descriptor_pool_ = objects->pool;
  fh1_vr_hud_pipeline_ = objects->pipeline;
  std::copy(objects->sets.begin(), objects->sets.end(), fh1_vr_hud_sets_.begin());
  objects->Release();
  fh1_vr_failed_ = false;
  REXGPU_INFO("VR HUD separation effect initialized");
  return true;
}

std::shared_ptr<VulkanCommandProcessor::Fh1VrSlot> VulkanCommandProcessor::Fh1VrTakeSlot(
    uint32_t width, uint32_t height) {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const uint64_t completed = GetCompletedSubmission();
  auto is_free = [&](const std::shared_ptr<Fh1VrSlot>& slot) {
    return slot.use_count() == 1 && slot->submission <= completed;
  };
  auto destroy = [&](Fh1VrSlot& slot) {
    for (Fh1VrSlot::Image* image : {&slot.scene, &slot.hud, &slot.stereo, &slot.keys, &slot.sharp, &slot.left}) {
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, image->view);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, image->image);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, image->memory);
    }
  };
  // Patch 0396: true stereo's images come with the slots made while it is asked for (the client
  // asks only while its cockpit is shown); a free slot without them makes way for one with them.
  const bool true_stereo_wanted =
      (rexui_GuestOutputExportFlags() & REX_GUEST_OUTPUT_EXPORT_TRUE_STEREO) != 0;
  for (auto& slot : fh1_vr_slots_) {
    if (is_free(slot) && slot->width == width && slot->height == height &&
        (!true_stereo_wanted || slot->left.image))
      return slot;
  }
  // Free slots of another size (the render scale changed) go.
  for (auto it = fh1_vr_slots_.begin(); it != fh1_vr_slots_.end();) {
    if (is_free(*it) && ((*it)->width != width || (*it)->height != height ||
                         (true_stereo_wanted && !(*it)->left.image))) {
      destroy(**it);
      it = fh1_vr_slots_.erase(it);
    } else {
      ++it;
    }
  }
  if (fh1_vr_slots_.size() >= kFh1VrMaxSlots) return nullptr;
  auto slot = std::make_shared<Fh1VrSlot>();
  auto make = [&](Fh1VrSlot::Image& out, VkFormat format, uint32_t w, uint32_t h) {
    return ui::vulkan::InstallCopyTarget(*vulkan_device, w, h, out, format,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  };
  const bool stereo = Fh1VrStereoWanted();
  // Patch 0396: true stereo's left eye and both eyes side by side.
  const bool true_stereo = true_stereo_wanted;
  if (!make(slot->scene, VK_FORMAT_A2B10G10R10_UNORM_PACK32, width, height) ||
      !make(slot->hud, VK_FORMAT_R8G8B8A8_UNORM, kFh1VrHudWidth, kFh1VrHudHeight) ||
      (stereo && (!make(slot->stereo, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 2 * width, height) ||
                  !make(slot->keys, VK_FORMAT_R32_UINT, 2 * width, height))) ||
      (true_stereo && !stereo &&
       (!make(slot->stereo, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 2 * width, height) ||
        !make(slot->left, VK_FORMAT_A2B10G10R10_UNORM_PACK32, width, height))) ||
      (rexui_GuestOutputSharpen() > 0.0f &&
       !make(slot->sharp, VK_FORMAT_A2B10G10R10_UNORM_PACK32, width, height))) {
    destroy(*slot);
    REXGPU_WARN("VR layers: no memory for a {}x{} slot", width, height);
    return nullptr;
  }
  slot->width = width;
  slot->height = height;
  fh1_vr_slots_.push_back(slot);
  if (fh1_vr_slots_.size() == 1)
    REXGPU_INFO("VR layers: {}x{} scene and {}x{} HUD", width, height, kFh1VrHudWidth,
                kFh1VrHudHeight);
  return slot;
}

void VulkanCommandProcessor::Fh1VrLayers(VkImage guest_output, VkImageView guest_output_view,
                                         uint32_t width, uint32_t height,
                                         VkImageView snapshot_view, VkPipeline gamma_pipeline,
                                         VkDescriptorSet gamma_ramp_set, uint32_t frame_index) {
  if (gamma_pipeline == VK_NULL_HANDLE || !Fh1VrEnsurePipelines()) return;
  std::shared_ptr<Fh1VrSlot> slot = Fh1VrTakeSlot(width, height);
  if (!slot) return;
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  // This frame index's sets were last used kMaxFramesInFlight frames ago, like the swap's own.
  const VkDescriptorImageInfo images[5] = {
      {VK_NULL_HANDLE, snapshot_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, slot->scene.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, guest_output_view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, slot->scene.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, slot->hud.view, VK_IMAGE_LAYOUT_GENERAL},
  };
  const VkDescriptorSet sets[5] = {fh1_vr_gamma_source_sets_[frame_index],
                                   fh1_vr_gamma_destination_sets_[frame_index],
                                   fh1_vr_hud_sets_[frame_index], fh1_vr_hud_sets_[frame_index],
                                   fh1_vr_hud_sets_[frame_index]};
  const uint32_t binding[5] = {0, 0, 0, 1, 2};
  VkWriteDescriptorSet writes[5] = {};
  for (uint32_t i = 0; i < 5; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = sets[i];
    writes[i].dstBinding = binding[i];
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = i == 1 || i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  dfn.vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);

  Fh1VrEnsureDepthImages();
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  PushImageMemoryBarrier(guest_output, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL);
  for (VkImage image : {slot->scene.image, slot->hud.image}) {
    // Contents are fully overwritten; the last readers (the client's copy, this processor's
    // earlier frame) are complete.
    PushImageMemoryBarrier(image, range, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  }
  SubmitBarriers(true);

  // The scene through the swap's gamma pipeline (its ramp, its channel order).
  std::array<VkDescriptorSet, kSwapApplyGammaComputeDescriptorSetCount> gamma_sets{};
  gamma_sets[kSwapApplyGammaComputeDescriptorSetRamp] = gamma_ramp_set;
  gamma_sets[kSwapApplyGammaComputeDescriptorSetSource] = sets[0];
  gamma_sets[kSwapApplyGammaComputeDescriptorSetDestination] = sets[1];
  deferred_command_buffer_.CmdVkBindDescriptorSets(
      VK_PIPELINE_BIND_POINT_COMPUTE, swap_apply_gamma_compute_pipeline_layout_, 0,
      uint32_t(gamma_sets.size()), gamma_sets.data(), 0, nullptr);
  SwapApplyGammaConstants gamma_constants = {{width, height}};
  deferred_command_buffer_.CmdVkPushConstants(swap_apply_gamma_compute_pipeline_layout_,
                                              VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                              sizeof(gamma_constants), &gamma_constants);
  BindExternalComputePipeline(gamma_pipeline);
  deferred_command_buffer_.CmdVkDispatch((width + 15) / 16, (height + 7) / 8, 1);
  PushImageMemoryBarrier(slot->scene.image, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL);
  SubmitBarriers(true);

  // The HUD alone.
  deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                                   fh1_vr_hud_pipeline_layout_, 0, 1, &sets[2],
                                                   0, nullptr);
  const uint32_t hud_constants[4] = {width, height, kFh1VrHudWidth, kFh1VrHudHeight};
  deferred_command_buffer_.CmdVkPushConstants(fh1_vr_hud_pipeline_layout_,
                                              VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                              sizeof(hud_constants), hud_constants);
  BindExternalComputePipeline(fh1_vr_hud_pipeline_);
  deferred_command_buffer_.CmdVkDispatch((kFh1VrHudWidth + 7) / 8, (kFh1VrHudHeight + 7) / 8, 1);

  // Patch 0395: the scene sharpened (after the HUD pass, which needs it as the game drew it).
  Fh1VrSharpen(*slot, width, height, frame_index);

  // Patch 0394: both eyes, when the client asks and this frame's depth was captured.
  // Patch 0396: or both eyes as the game drew them, when this frame was drawn twice.
  const bool stereo =
      Fh1VrStereo(*slot, width, height, frame_index) ||
      Fh1StereoLayers(*slot, width, height, gamma_pipeline, gamma_ramp_set, frame_index);

  // Handed over like the guest output: shader-read-only, last written before the fragment
  // shader and transfer stages the client starts from.
  for (VkImage image : {slot->scene.image, slot->hud.image}) {
    PushImageMemoryBarrier(image, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  slot->submission = GetCurrentSubmission();

  RexGuestOutputLayers layers = {};
  layers.layers = REX_GUEST_OUTPUT_LAYER_SCENE | REX_GUEST_OUTPUT_LAYER_HUD;
  layers.scene_width = width;
  layers.scene_height = height;
  layers.scene = slot->scene.image;
  layers.hud_width = kFh1VrHudWidth;
  layers.hud_height = kFh1VrHudHeight;
  layers.hud = slot->hud.image;
  if (stereo) {
    layers.layers |= REX_GUEST_OUTPUT_LAYER_STEREO;
    layers.stereo_width = 2 * width;
    layers.stereo_height = height;
    layers.stereo = slot->stereo.image;
  }
  const std::shared_ptr<void> keep = slot;
  rexui_SetNextGuestOutputLayers(&keep, &layers);
}

// Patch 0395: contrast-adaptive sharpening (after AMD FidelityFX CAS, MIT) of the VR scene
// layer: each pixel against its 3 x 3 neighbourhood, the weight of the negative lobe scaled down
// where the neighbourhood already has contrast (no halos on edges), in the picture's own gamma
// encoding. The desktop window's FSR/CAS present effect works on the window's picture, which the
// headset never sees.
namespace {
constexpr char kFh1VrSharpenShader[] = R"glsl(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8) in;
layout(push_constant) uniform Params { uvec2 size; float sharpness; } pc;
layout(set = 0, binding = 0) uniform texture2D src;
layout(set = 0, binding = 1, rgb10_a2) uniform writeonly image2D dst;
vec3 at(ivec2 p) { return texelFetch(src, clamp(p, ivec2(0), ivec2(pc.size) - 1), 0).rgb; }
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(p), pc.size))) return;
  vec3 a = at(p + ivec2(-1, -1)), b = at(p + ivec2(0, -1)), c = at(p + ivec2(1, -1));
  vec3 d = at(p + ivec2(-1, 0)), f = at(p + ivec2(1, 0));
  vec3 g = at(p + ivec2(-1, 1)), h = at(p + ivec2(0, 1)), i = at(p + ivec2(1, 1));
  vec4 e4 = texelFetch(src, p, 0);
  vec3 e = e4.rgb;
  vec3 mn = min(min(min(d, e), min(f, b)), h);
  mn += min(mn, min(min(a, c), min(g, i)));
  vec3 mx = max(max(max(d, e), max(f, b)), h);
  mx += max(mx, max(max(a, c), max(g, i)));
  vec3 amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, vec3(1e-5)), 0.0, 1.0));
  vec3 w = amp * (-1.0 / mix(8.0, 5.0, clamp(pc.sharpness, 0.0, 1.0)));
  imageStore(dst, p, vec4(clamp(((b + d + f + h) * w + e) / (1.0 + 4.0 * w), 0.0, 1.0), e4.a));
}
)glsl";
}  // namespace

bool VulkanCommandProcessor::Fh1VrSharpenEnsurePipeline() {
  if (fh1_vr_sharpen_pipeline_) return true;
  if (fh1_vr_sharpen_failed_) return false;
  fh1_vr_sharpen_failed_ = true;
  std::vector<uint32_t> shader;
  std::string error;
  if (!CompileFh1Compute(kFh1VrSharpenShader, shader, error)) {
    REXGPU_WARN("VR sharpening compilation failed: {}", error);
    return false;
  }
  const std::array signature{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
  auto objects = ui::vulkan::ImageComputeObjects::Create(*GetVulkanDevice(), shader, signature,
      kMaxFramesInFlight, sizeof(uint32_t) * 3, ui::vulkan::BuildImageComputePipeline);
  if (!objects) return false;
  fh1_vr_sharpen_set_layout_ = objects->set_layout;
  fh1_vr_sharpen_pipeline_layout_ = objects->layout;
  fh1_vr_sharpen_descriptor_pool_ = objects->pool;
  fh1_vr_sharpen_pipeline_ = objects->pipeline;
  std::copy(objects->sets.begin(), objects->sets.end(), fh1_vr_sharpen_sets_.begin());
  objects->Release();
  fh1_vr_sharpen_failed_ = false;
  REXGPU_INFO("VR sharpening effect initialized at strength {:.2f}", rexui_GuestOutputSharpen());
  return true;
}

void VulkanCommandProcessor::Fh1VrSharpen(Fh1VrSlot& slot, uint32_t width, uint32_t height,
                                          uint32_t frame_index) {
  const float sharpness = rexui_GuestOutputSharpen();
  if (sharpness <= 0.0f || !slot.sharp.image || !Fh1VrSharpenEnsurePipeline()) return;
  Fh1VrSharpenImage(slot.scene.view, slot.sharp.image, slot.sharp.view,
                    fh1_vr_sharpen_sets_[frame_index], width, height);
  // From here on (stereo, the hand-over) the sharpened picture is the scene.
  std::swap(slot.scene, slot.sharp);
}

// Patch 0396: the pass on any picture (the scene, true stereo's left eye) with its own set.
void VulkanCommandProcessor::Fh1VrSharpenImage(VkImageView source, VkImage destination,
                                               VkImageView destination_view, VkDescriptorSet set,
                                               uint32_t width, uint32_t height) {
  const float sharpness = rexui_GuestOutputSharpen();
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const VkDescriptorImageInfo images[2] = {
      {VK_NULL_HANDLE, source, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, destination_view, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkWriteDescriptorSet writes[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  dfn.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  // Fully overwritten; its last readers (the client's copy of an earlier frame) are complete.
  PushImageMemoryBarrier(destination, range, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  SubmitBarriers(true);
  deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                                   fh1_vr_sharpen_pipeline_layout_, 0, 1, &set, 0,
                                                   nullptr);
  uint32_t constants[3] = {width, height, 0};
  std::memcpy(&constants[2], &sharpness, sizeof(float));
  deferred_command_buffer_.CmdVkPushConstants(fh1_vr_sharpen_pipeline_layout_,
                                              VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                                              constants);
  BindExternalComputePipeline(fh1_vr_sharpen_pipeline_);
  deferred_command_buffer_.CmdVkDispatch((width + 7) / 8, (height + 7) / 8, 1);
  PushImageMemoryBarrier(destination, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL);
  SubmitBarriers(true);
}

// Patch 0396: VR true stereo. CommandProcessor::Fh1Stereo* executes the frame's main view
// twice. The frame (TU4, the cockpit): the HUD's render-to-texture, the four sun shadow
// cascades, the 1280 x 720 view's depth pre-pass and the sun's shadow mask on it, the rear-view
// mirror, sometimes a face of the reflection cube, then the segment drawn for each eye: the
// scene in two predicated EDRAM tiles (its own clear and depth buffer), post-processing, the
// minimap, the composite into the front buffer; then the HUD, once. In each eye the scene's 3D
// draws (on the scene's depth buffer, the 16:9 viewport, a projected position) move by the eye's
// offset in clip space. The passes before the scene stay the middle's: the scene's shaders find
// the shadow mask and the depth at the screen position they compute from the middle's position.
bool VulkanCommandProcessor::Fh1StereoWanted() {
  if (fh1_stereo_failed_ || !Fh1VrLayersWanted() ||
      !(rexui_GuestOutputExportFlags() & REX_GUEST_OUTPUT_EXPORT_TRUE_STEREO) ||
      render_target_cache_->GetPath() != RenderTargetCache::Path::kHostRenderTargets ||
      fh1_other_render_targets_)
    return false;
  float eye_distance = 0, p00 = 0;
  rexui_GuestOutputTrueStereoParams(&eye_distance, &p00);
  return p00 > 0.0f && eye_distance > 0.0f;
}

namespace {
fh1_vr_stereo::DrawState Fh1StereoDrawState(const RegisterFile& regs) {
  fh1_vr_stereo::DrawState d;
  d.mode = uint32_t(regs.Get<reg::RB_MODECONTROL>().edram_mode);
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  d.pitch = surface.surface_pitch;
  d.msaa = uint32_t(surface.msaa_samples);
  const auto color = regs.Get<reg::RB_COLOR_INFO>();
  d.color_base = color.color_base;
  d.color_format = uint32_t(color.color_format);
  d.depth_base = regs.Get<reg::RB_DEPTH_INFO>().depth_base;
  const auto depth_control = regs.Get<reg::RB_DEPTHCONTROL>();
  d.z_enable = depth_control.z_enable;
  d.z_write = depth_control.z_write_enable;
  d.zfunc = uint32_t(depth_control.zfunc);
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  d.window_space = vte.vtx_xy_fmt;
  d.projected = !vte.vtx_xy_fmt && !vte.vtx_z_fmt && vte.vtx_w0_fmt && vte.vport_x_scale_ena &&
                vte.vport_y_scale_ena;
  d.primitive = uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type);
  d.viewport_x_scale = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE);
  d.viewport_y_scale = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE);
  d.viewport_x_offset = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET);
  return d;
}
}  // namespace

CommandProcessor::Fh1StereoDraw VulkanCommandProcessor::Fh1StereoClassify() {
  fh1_vr_stereo::Tracker tracker{fh1_stereo_depth_base_, fh1_stereo_composite_seen_,
                                 fh1_stereo_composite_base_};
  const fh1_vr_stereo::Draw draw =
      fh1_vr_stereo::Classify(Fh1StereoDrawState(*register_file_),
                              fh1_stereo_state_ == Fh1StereoState::kRecording, tracker);
  fh1_stereo_depth_base_ = tracker.depth_base;
  fh1_stereo_composite_seen_ = tracker.composite_seen;
  fh1_stereo_composite_base_ = tracker.composite_base;
  switch (draw) {
    case fh1_vr_stereo::Draw::kClear:
      return Fh1StereoDraw::kClear;
    case fh1_vr_stereo::Draw::kSegmentStart:
      return Fh1StereoDraw::kSegmentStart;
    case fh1_vr_stereo::Draw::kSegmentEnd:
      return Fh1StereoDraw::kSegmentEnd;
    default:
      return Fh1StereoDraw::kOther;
  }
}

void VulkanCommandProcessor::Fh1StereoBegin() {
  float eye_distance = 0.064f, p00 = 0.0f;
  rexui_GuestOutputTrueStereoParams(&eye_distance, &p00);
  // The main view is drawn mirrored across (fh1_vr_stereo.h EyeOffsets); before the frame's
  // shadow passes say so, as measured in the cockpit (TU4).
  const bool mirrored = fh1_stereo_handedness_known_ ? fh1_stereo_mirrored_ : true;
  // A test aid: FORZA_VR_STEREO_SHIFT_SCALE scales the eyes' offset (0: both eyes drawn from the
  // middle, which makes the right eye's pass reproduce the left's).
  static const float shift_scale = [] {
    const char* value = std::getenv("FORZA_VR_STEREO_SHIFT_SCALE");
    return value && *value ? float(std::atof(value)) : 1.0f;
  }();
  float offsets[2];
  fh1_vr_stereo::EyeOffsets(p00, eye_distance * shift_scale, mirrored, offsets);
  // Each eye's part of the picture (0 to 1 across: all of it) as render target columns.
  float eye_x[4] = {0, 1, 0, 1};
  rexui_GuestOutputTrueStereoEyes(eye_x);
  for (int eye = 0; eye < 2; ++eye) {
    fh1_stereo_eye_clip_[eye][0] = offsets[eye];
    fh1_stereo_eye_clip_[eye][1] = fh1_stereo_eye_clip_[eye][2] = fh1_stereo_eye_clip_[eye][3] = 0;
    fh1_vr_stereo::EyeColumns(eye_x[2 * eye], eye_x[2 * eye + 1], mirrored, 1280,
                              fh1_stereo_scissor_x_[eye]);
  }
  fh1_stereo_eye_ = 0;
  fh1_stereo_composite_seen_ = false;
  fh1_stereo_offset_draws_ = 0;
}

bool VulkanCommandProcessor::Fh1StereoLeftDone() {
  // The left eye's composite, before the HUD (the draw before this one wrote it).
  uint32_t width = 1280 * render_target_cache_->draw_resolution_scale_x();
  uint32_t height = 720 * render_target_cache_->draw_resolution_scale_y();
  auto& left = fh1_stereo_left_[frame_current_ % kMaxFramesInFlight];
  left.frame = UINT64_MAX;
  if (left.submission) CheckSubmissionFenceAndDeviceLoss(left.submission);
  if (left.image && (left.width != width || left.height != height)) DestroyHdrScene(left);
  const bool existing = left.image != VK_NULL_HANDLE;
  if (!existing && !ui::vulkan::InstallCopyTarget(*GetVulkanDevice(), width, height, left)) {
    fh1_stereo_failed_ = true;
    REXGPU_WARN("VR stereo image allocation failed at {}x{}", width, height);
    return false;
  }
  EndRenderPass();
  const auto range = ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
  PushImageMemoryBarrier(
      left.image, range,
      existing ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, existing ? VK_ACCESS_SHADER_READ_BIT : 0,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      existing ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const bool copied = render_target_cache_->CopyColorSnapshot({left.image,
      VK_FORMAT_A2B10G10R10_UNORM_PACK32, left.width, left.height, VK_SAMPLE_COUNT_1_BIT,
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
  PushImageMemoryBarrier(left.image, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  SubmitBarriers(true);
  left.submission = GetCurrentSubmission();
  if (!copied) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXGPU_WARN("VR stereo: the left eye's composite was not where expected: mono frame");
    }
    return false;
  }
  left.frame = frame_current_;
  static bool logged = false;
  if (!logged) {
    logged = true;
    float eye_distance = 0, p00 = 0;
    rexui_GuestOutputTrueStereoParams(&eye_distance, &p00);
    REXGPU_INFO("VR stereo: both eyes drawn ({}x{} each, eye distance {:.1f} mm, P00 {:.4f}, "
                "{} draws moved for the left eye)",
                width, height, eye_distance * 1000.0f, p00, fh1_stereo_offset_draws_);
  }
  return true;
}

void VulkanCommandProcessor::Fh1StereoReplayBegin() {
  fh1_stereo_eye_ = 1;
  fh1_stereo_composite_seen_ = false;
  // The right eye's pass reads what the left eye's read (the CPU may have rewritten memory
  // since), and computes the frame's occlusion again from its own depth.
  shared_memory_->Fh1SetKeepUploaded(true);
}

void VulkanCommandProcessor::Fh1StereoEnd(bool right_eye_done) {
  fh1_stereo_eye_ = -1;
  fh1_stereo_draw_scissor_ = false;
  shared_memory_->Fh1SetKeepUploaded(false);
  if (!right_eye_done) fh1_stereo_left_[frame_current_ % kMaxFramesInFlight].frame = UINT64_MAX;
  std::memset(fh1_stereo_draw_eye_, 0, sizeof(fh1_stereo_draw_eye_));
}

void VulkanCommandProcessor::Fh1StereoRegistersReplaced() {
  current_constant_buffers_up_to_date_ = 0;
  if (texture_cache_) texture_cache_->TextureFetchConstantsWritten(0, 31);
  if (fh1_other_textures_) fh1_other_textures_->TextureFetchConstantsWritten(0, 31);
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;
}

SharedMemory* VulkanCommandProcessor::Fh1StereoSharedMemory() { return shared_memory_.get(); }

void VulkanCommandProcessor::Fh1StereoDrawOffset(
    const reg::RB_DEPTHCONTROL& normalized_depth_control) {
  const float* offset = nullptr;
  if (fh1_stereo_eye_ >= 0) {
    fh1_vr_stereo::DrawState d = Fh1StereoDrawState(*register_file_);
    d.z_enable = normalized_depth_control.z_enable;
    d.zfunc = uint32_t(normalized_depth_control.zfunc);
    const fh1_vr_stereo::Tracker tracker{fh1_stereo_depth_base_, false, 0};
    if (fh1_vr_stereo::TakesOffset(d, tracker)) {
      offset = fh1_stereo_eye_clip_[fh1_stereo_eye_];
      if (fh1_stereo_eye_ == 0) ++fh1_stereo_offset_draws_;
    }
    // The eye's part of the 1280-wide picture (full-size viewports only).
    fh1_stereo_draw_scissor_ =
        offset && fh1_stereo_scissor_x_[fh1_stereo_eye_][1] > fh1_stereo_scissor_x_[fh1_stereo_eye_][0] &&
        std::fabs(d.viewport_x_scale) == 640.0f && d.viewport_x_offset == 640.0f;
  } else {
    fh1_stereo_draw_scissor_ = false;
  }
  if (offset) {
    std::memcpy(fh1_stereo_draw_eye_, offset, sizeof(fh1_stereo_draw_eye_));
  } else {
    std::memset(fh1_stereo_draw_eye_, 0, sizeof(fh1_stereo_draw_eye_));
  }
}

bool VulkanCommandProcessor::Fh1StereoEnsureDescriptors() {
  if (fh1_stereo_descriptor_pool_ != VK_NULL_HANDLE) return true;
  if (fh1_stereo_failed_) return false;
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const VkDescriptorPoolSize sizes[2] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * kMaxFramesInFlight},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kMaxFramesInFlight}};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 3 * kMaxFramesInFlight;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = sizes;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &fh1_stereo_descriptor_pool_) !=
      VK_SUCCESS) {
    fh1_stereo_failed_ = true;
    return false;
  }
  auto allocate = [&](VkDescriptorSetLayout layout,
                      std::array<VkDescriptorSet, kMaxFramesInFlight>& sets) {
    std::array<VkDescriptorSetLayout, kMaxFramesInFlight> layouts;
    layouts.fill(layout);
    VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = fh1_stereo_descriptor_pool_;
    info.descriptorSetCount = kMaxFramesInFlight;
    info.pSetLayouts = layouts.data();
    return dfn.vkAllocateDescriptorSets(device, &info, sets.data()) == VK_SUCCESS;
  };
  if (!allocate(swap_descriptor_set_layout_sampled_image_, fh1_stereo_gamma_source_sets_) ||
      !allocate(swap_descriptor_set_layout_storage_image_, fh1_stereo_gamma_destination_sets_) ||
      (fh1_vr_sharpen_set_layout_ != VK_NULL_HANDLE &&
       !allocate(fh1_vr_sharpen_set_layout_, fh1_stereo_sharpen_sets_))) {
    fh1_stereo_failed_ = true;
    REXGPU_WARN("VR stereo: no descriptor sets: mono");
    return false;
  }
  return true;
}

// At the swap, after the scene layer (the right eye: the frame's last pass) and its sharpening:
// the left eye's composite through the same gamma ramp and sharpening into `slot.sharp` (free
// once the scene is sharpened) or straight into the left half, then both side by side.
bool VulkanCommandProcessor::Fh1StereoLayers(Fh1VrSlot& slot, uint32_t width, uint32_t height,
                                             VkPipeline gamma_pipeline,
                                             VkDescriptorSet gamma_ramp_set,
                                             uint32_t frame_index) {
  const HdrScene& left = fh1_stereo_left_[frame_current_ % kMaxFramesInFlight];
  if (left.frame != frame_current_) return false;  // this frame was drawn once
  if (left.width != width || left.height != height || !slot.stereo.image || !slot.left.image ||
      !Fh1StereoEnsureDescriptors()) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXGPU_WARN("VR stereo: both eyes drawn but not handed over (left {}x{}, picture {}x{}, "
                  "images {} {}, sets {})",
                  left.width, left.height, width, height, slot.stereo.image != VK_NULL_HANDLE,
                  slot.left.image != VK_NULL_HANDLE, !fh1_stereo_failed_);
    }
    return false;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  const VkDescriptorImageInfo images[2] = {
      {VK_NULL_HANDLE, left.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, slot.left.view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet writes[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = i ? fh1_stereo_gamma_destination_sets_[frame_index]
                         : fh1_stereo_gamma_source_sets_[frame_index];
    writes[i].dstBinding = 0;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  dfn.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
  PushImageMemoryBarrier(slot.left.image, range, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  SubmitBarriers(true);
  std::array<VkDescriptorSet, kSwapApplyGammaComputeDescriptorSetCount> gamma_sets{};
  gamma_sets[kSwapApplyGammaComputeDescriptorSetRamp] = gamma_ramp_set;
  gamma_sets[kSwapApplyGammaComputeDescriptorSetSource] = fh1_stereo_gamma_source_sets_[frame_index];
  gamma_sets[kSwapApplyGammaComputeDescriptorSetDestination] =
      fh1_stereo_gamma_destination_sets_[frame_index];
  deferred_command_buffer_.CmdVkBindDescriptorSets(
      VK_PIPELINE_BIND_POINT_COMPUTE, swap_apply_gamma_compute_pipeline_layout_, 0,
      uint32_t(gamma_sets.size()), gamma_sets.data(), 0, nullptr);
  SwapApplyGammaConstants gamma_constants = {{width, height}};
  deferred_command_buffer_.CmdVkPushConstants(swap_apply_gamma_compute_pipeline_layout_,
                                              VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                              sizeof(gamma_constants), &gamma_constants);
  BindExternalComputePipeline(gamma_pipeline);
  deferred_command_buffer_.CmdVkDispatch((width + 15) / 16, (height + 7) / 8, 1);
  PushImageMemoryBarrier(slot.left.image, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL);
  SubmitBarriers(true);
  Fh1VrSlot::Image* left_picture = &slot.left;
  // The scene was sharpened into `sharp` and swapped: `sharp` holds the unsharpened scene now.
  const float sharpness = rexui_GuestOutputSharpen();
  if (sharpness > 0.0f && slot.sharp.image && fh1_vr_sharpen_pipeline_ != VK_NULL_HANDLE &&
      fh1_stereo_sharpen_sets_[frame_index] != VK_NULL_HANDLE) {
    Fh1VrSharpenImage(slot.left.view, slot.sharp.image, slot.sharp.view,
                      fh1_stereo_sharpen_sets_[frame_index], width, height);
    left_picture = &slot.sharp;
  }
  // Side by side: the left eye in the left half.
  for (VkImage image : {left_picture->image, slot.scene.image}) {
    PushImageMemoryBarrier(image, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  }
  PushImageMemoryBarrier(slot.stereo.image, range, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  SubmitBarriers(true);
  for (int eye = 0; eye < 2; ++eye) {
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(width), int32_t(height), 1};
    blit.dstOffsets[0] = {eye ? int32_t(width) : 0, 0, 0};
    blit.dstOffsets[1] = {eye ? int32_t(2 * width) : int32_t(width), int32_t(height), 1};
    deferred_command_buffer_.CmdVkBlitImage(eye ? slot.scene.image : left_picture->image,
                                            slot.stereo.image, blit);
  }
  for (VkImage image : {left_picture->image, slot.scene.image}) {
    PushImageMemoryBarrier(image, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_IMAGE_LAYOUT_GENERAL);
  }
  PushImageMemoryBarrier(slot.stereo.image, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  SubmitBarriers(true);
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXGPU_INFO("VR stereo: both eyes handed over side by side ({}x{})", 2 * width, height);
  }
  return true;
}

void VulkanCommandProcessor::Fh1VrDestroy() {
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  // Patch 0396.
  for (HdrScene& left : fh1_stereo_left_) DestroyHdrScene(left);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         fh1_stereo_descriptor_pool_);
  for (auto& slot : fh1_vr_slots_) {
    for (Fh1VrSlot::Image* image : {&slot->scene, &slot->hud, &slot->stereo, &slot->keys, &slot->sharp, &slot->left}) {
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, image->view);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, image->image);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, image->memory);
    }
  }
  fh1_vr_slots_.clear();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, fh1_vr_sharpen_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         fh1_vr_sharpen_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         fh1_vr_sharpen_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         fh1_vr_sharpen_set_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, fh1_vr_hud_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         fh1_vr_hud_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         fh1_vr_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         fh1_vr_hud_set_layout_);
  for (Fh1VrDepth& depth : fh1_vr_depths_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, depth.view);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, depth.image);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, depth.memory);
  }
  for (VkPipeline* pipeline : {&fh1_vr_depth_pipeline_, &fh1_vr_stereo_splat_pipeline_,
                               &fh1_vr_stereo_resolve_pipeline_}) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, *pipeline);
  }
  for (VkPipelineLayout* layout : {&fh1_vr_depth_pipeline_layout_, &fh1_vr_stereo_pipeline_layout_}) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, *layout);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         fh1_vr_stereo_descriptor_pool_);
  for (VkDescriptorSetLayout* layout : {&fh1_vr_depth_set_layout_, &fh1_vr_stereo_set_layout_}) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, *layout);
  }
}

bool VulkanCommandProcessor::Fh1VrStereoWanted() const {
  return !fh1_vr_stereo_failed_ && Fh1VrLayersWanted() &&
         (rexui_GuestOutputExportFlags() & REX_GUEST_OUTPUT_EXPORT_STEREO) != 0;
}

bool VulkanCommandProcessor::Fh1VrStereoEnsurePipelines() {
  if (fh1_vr_stereo_resolve_pipeline_ != VK_NULL_HANDLE) return true;
  if (fh1_vr_stereo_failed_) return false;
  fh1_vr_stereo_failed_ = true;
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  auto set_layout = [&](std::initializer_list<VkDescriptorType> types, VkDescriptorSetLayout& out) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (VkDescriptorType type : types) {
      VkDescriptorSetLayoutBinding b = {};
      b.binding = uint32_t(bindings.size());
      b.descriptorType = type;
      b.descriptorCount = 1;
      b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      bindings.push_back(b);
    }
    VkDescriptorSetLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = uint32_t(bindings.size());
    info.pBindings = bindings.data();
    return dfn.vkCreateDescriptorSetLayout(device, &info, nullptr, &out) == VK_SUCCESS;
  };
  auto pipeline_layout = [&](VkDescriptorSetLayout set, uint32_t push, VkPipelineLayout& out) {
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
    VkPipelineLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &set;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    return dfn.vkCreatePipelineLayout(device, &info, nullptr, &out) == VK_SUCCESS;
  };
  auto pipeline = [&](std::string source, VkPipelineLayout layout, VkPipeline& out) {
    std::vector<uint32_t> spirv;
    std::string error;
    if (!CompileFh1Compute(source, spirv, error)) {
      REXGPU_WARN("VR stereo: a shader did not compile ({})", error);
      return false;
    }
    out = ui::vulkan::util::CreateComputePipeline(vulkan_device, layout, spirv.data(),
                                                  spirv.size() * sizeof(uint32_t));
    return out != VK_NULL_HANDLE;
  };
  if (!set_layout({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                  fh1_vr_depth_set_layout_) ||
      !set_layout({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                   VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                  fh1_vr_stereo_set_layout_) ||
      !pipeline_layout(fh1_vr_depth_set_layout_, 32, fh1_vr_depth_pipeline_layout_) ||
      !pipeline_layout(fh1_vr_stereo_set_layout_, 32, fh1_vr_stereo_pipeline_layout_)) {
    REXGPU_WARN("VR stereo: no layouts; off");
    return false;
  }
  VkDescriptorPoolSize pool_sizes[2] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * kMaxFramesInFlight},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 * kMaxFramesInFlight},
  };
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 2 * kMaxFramesInFlight;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &fh1_vr_stereo_descriptor_pool_) !=
      VK_SUCCESS) {
    REXGPU_WARN("VR stereo: no descriptor pool; off");
    return false;
  }
  auto allocate = [&](VkDescriptorSetLayout layout,
                      std::array<VkDescriptorSet, kMaxFramesInFlight>& sets) {
    std::array<VkDescriptorSetLayout, kMaxFramesInFlight> layouts;
    layouts.fill(layout);
    VkDescriptorSetAllocateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = fh1_vr_stereo_descriptor_pool_;
    info.descriptorSetCount = kMaxFramesInFlight;
    info.pSetLayouts = layouts.data();
    return dfn.vkAllocateDescriptorSets(device, &info, sets.data()) == VK_SUCCESS;
  };
  if (!allocate(fh1_vr_depth_set_layout_, fh1_vr_depth_sets_) ||
      !allocate(fh1_vr_stereo_set_layout_, fh1_vr_stereo_sets_) ||
      !pipeline(kFh1VrDepthShader, fh1_vr_depth_pipeline_layout_, fh1_vr_depth_pipeline_) ||
      !pipeline(std::string(kFh1VrStereoCommon) + kFh1VrStereoSplat,
                fh1_vr_stereo_pipeline_layout_, fh1_vr_stereo_splat_pipeline_) ||
      !pipeline(std::string(kFh1VrStereoCommon) + kFh1VrStereoResolve,
                fh1_vr_stereo_pipeline_layout_, fh1_vr_stereo_resolve_pipeline_)) {
    REXGPU_WARN("VR stereo: no pipelines; off");
    return false;
  }
  fh1_vr_stereo_failed_ = false;
  REXGPU_INFO("VR stereo: on (each eye from the scene and the depth pre-pass)");
  return true;
}

// At the swap (outside any draw): the depth images at the size the last capture asked for.
void VulkanCommandProcessor::Fh1VrEnsureDepthImages() {
  if (!Fh1VrStereoWanted() || !fh1_vr_depth_width_ || !Fh1VrStereoEnsurePipelines()) return;
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (Fh1VrDepth& depth : fh1_vr_depths_) {
    if (depth.image && depth.width == fh1_vr_depth_width_ && depth.height == fh1_vr_depth_height_)
      continue;
    if (depth.image) {
      if (depth.submission) CheckSubmissionFenceAndDeviceLoss(depth.submission);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, depth.view);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, depth.image);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, depth.memory);
    }
    depth = {};
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R32_SFLOAT;
    info.extent = {fh1_vr_depth_width_, fh1_vr_depth_height_, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(
            vulkan_device, info, ui::vulkan::util::MemoryPurpose::kDeviceLocal, depth.image,
            depth.memory))
      return;
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = depth.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (dfn.vkCreateImageView(device, &view, nullptr, &depth.view) != VK_SUCCESS) return;
    depth.width = fh1_vr_depth_width_;
    depth.height = fh1_vr_depth_height_;
    depth.fresh = true;
  }
}

// At the frame's first shadow pass: the main view's depth pre-pass (the shadow pass's texture
// 0, as the AO finds it) as view distance into this frame's depth image.
void VulkanCommandProcessor::Fh1VrDepthBeforeDraw(const Shader* pixel_shader,
                                                  uint32_t used_texture_mask) {
  if (!pixel_shader || !Fh1VrStereoWanted() ||
      !fh1_world_ao::IsShadowPass(pixel_shader->ucode_data_hash()))
    return;
  const uint64_t frame = GetCurrentFrame();
  Fh1VrDepth& depth = fh1_vr_depths_[frame % kMaxFramesInFlight];
  if (depth.frame == frame) return;
  const RegisterFile& regs = *register_file_;
  const auto fetch = regs.GetTextureFetch(0);
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  if (!(used_texture_mask & 1u) || fetch.format != xenos::TextureFormat::k_24_8_FLOAT ||
      uint32_t(fetch.size_2d.width) + 1 != uint32_t(surface.surface_pitch) ||
      surface.msaa_samples != xenos::MsaaSamples::k1X)
    return;
  fh1_vr_depth_width_ =
      (uint32_t(fetch.size_2d.width) + 1) * texture_cache_->draw_resolution_scale_x();
  fh1_vr_depth_height_ =
      (uint32_t(fetch.size_2d.height) + 1) * texture_cache_->draw_resolution_scale_y();
  if (!depth.image || depth.width != fh1_vr_depth_width_ || depth.height != fh1_vr_depth_height_)
    return;  // made at the next swap
  float projection[4];
  auto constant = [&](uint32_t index, uint32_t component) {
    return regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + 4 * index + component);
  };
  if (!fh1_world_ao::Projection(constant(0, 0), constant(1, 1), constant(25, 0), constant(25, 1),
                                projection))
    return;
  const VkImageView view =
      texture_cache_->GetActiveBindingOrNullImageView(0, xenos::FetchOpDimension::k2D, false);
  if (!view) return;
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  // The frame index's set was last used kMaxFramesInFlight frames ago.
  const VkDescriptorSet set = fh1_vr_depth_sets_[frame % kMaxFramesInFlight];
  const VkDescriptorImageInfo images[2] = {
      {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, depth.view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet writes[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  dfn.vkUpdateDescriptorSets(vulkan_device->device(), 2, writes, 0, nullptr);
  EndRenderPass();
  SubmitBarriers(true);
  // Whatever wrote the depth texture, and the last frame's reads of this image, first.
  VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  VkImageMemoryBarrier layout{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  layout.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  layout.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  layout.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  layout.srcQueueFamilyIndex = layout.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  layout.image = depth.image;
  layout.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  deferred_command_buffer_.CmdVkPipelineBarrier(
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory, 0,
      nullptr, depth.fresh ? 1 : 0, &layout);
  depth.fresh = false;
  struct {
    float proj[4];
    uint32_t size[4];
  } constants = {{projection[0], projection[1], projection[2], projection[3]},
                 {depth.width, depth.height, 0, 0}};
  BindExternalComputePipeline(fh1_vr_depth_pipeline_);
  deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                                   fh1_vr_depth_pipeline_layout_, 0, 1, &set, 0,
                                                   nullptr);
  deferred_command_buffer_.CmdVkPushConstants(fh1_vr_depth_pipeline_layout_,
                                              VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                                              &constants);
  deferred_command_buffer_.CmdVkDispatch((depth.width + 7) / 8, (depth.height + 7) / 8, 1);
  memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  deferred_command_buffer_.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                                &memory, 0, nullptr, 0, nullptr);
  std::memcpy(depth.projection, projection, sizeof(projection));
  depth.frame = frame;
  depth.submission = GetCurrentSubmission();
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXGPU_INFO("VR stereo: depth {}x{}, view z = {} / (1 - {} - depth), tan of half the view "
                "{:.3f} x {:.3f}",
                depth.width, depth.height, projection[1], projection[0], projection[2],
                projection[3]);
  }
}

// In the swap refresher after the scene's gamma pass: both eyes into the slot.
bool VulkanCommandProcessor::Fh1VrStereo(Fh1VrSlot& slot, uint32_t width, uint32_t height,
                                         uint32_t frame_index) {
  if (!Fh1VrStereoWanted() || !slot.stereo.image || !Fh1VrStereoEnsurePipelines()) return false;
  const Fh1VrDepth& depth = fh1_vr_depths_[frame_current_ % kMaxFramesInFlight];
  if (depth.frame != frame_current_ || depth.width != width || depth.height != height)
    return false;
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDescriptorSet set = fh1_vr_stereo_sets_[frame_index];
  const VkDescriptorImageInfo images[4] = {
      {VK_NULL_HANDLE, depth.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, slot.keys.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, slot.scene.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, slot.stereo.view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet writes[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = i & 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                     : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  dfn.vkUpdateDescriptorSets(vulkan_device->device(), 4, writes, 0, nullptr);
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  for (VkImage image : {slot.keys.image, slot.stereo.image}) {
    PushImageMemoryBarrier(image, range, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  }
  SubmitBarriers(true);
  VkClearColorValue empty = {};
  empty.uint32[0] = 0xFFFFFFFFu;
  deferred_command_buffer_.CmdVkClearColorImage(slot.keys.image, VK_IMAGE_LAYOUT_GENERAL, &empty, 1,
                                                &range);
  PushImageMemoryBarrier(slot.keys.image, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
  SubmitBarriers(true);
  float eye_distance = 0.064f, max_shift = 0.03f;
  rexui_GuestOutputStereoParams(&eye_distance, &max_shift);
  const float focal = 0.5f * float(width) / std::max(depth.projection[2], 1e-3f);
  struct {
    float eye[4];
    uint32_t size[4];
  } constants = {{0.5f * eye_distance, focal, std::max(1.0f, max_shift * float(width)), 0},
                 {width, height, 0, 0}};
  deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                                   fh1_vr_stereo_pipeline_layout_, 0, 1, &set, 0,
                                                   nullptr);
  deferred_command_buffer_.CmdVkPushConstants(fh1_vr_stereo_pipeline_layout_,
                                              VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                                              &constants);
  BindExternalComputePipeline(fh1_vr_stereo_splat_pipeline_);
  deferred_command_buffer_.CmdVkDispatch((width + 7) / 8, (height + 7) / 8, 1);
  PushImageMemoryBarrier(slot.keys.image, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL);
  SubmitBarriers(true);
  BindExternalComputePipeline(fh1_vr_stereo_resolve_pipeline_);
  deferred_command_buffer_.CmdVkDispatch((2 * width + 7) / 8, (height + 7) / 8, 1);
  PushImageMemoryBarrier(slot.stereo.image, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  return true;
}

void VulkanCommandProcessor::CaptureHdrScene() {
  using namespace scene_capture;
  const auto surface = register_file_->Get<reg::RB_SURFACE_INFO>();
  const auto color = register_file_->Get<reg::RB_COLOR_INFO>();
  const DrawKind kind = color.color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10
      ? DrawKind::Composite
      : color.color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10
          ? DrawKind::Overlay : DrawKind::Other;
  const bool eligible = surface.surface_pitch == 1280 && surface.msaa_samples == xenos::MsaaSamples::k1X;
  switch (Evaluate(kind, eligible, color.color_base, hdr_scene_base_, hdr_capture_attempted_)) {
    case Action::Ignore: return;
    case Action::RememberSurface: hdr_scene_base_ = color.color_base; return;
    case Action::Capture: hdr_capture_attempted_ = true; break;
  }
  if (render_target_cache_->GetPath() != RenderTargetCache::Path::kHostRenderTargets) return;
  const auto [width, height] = Resolution(render_target_cache_->draw_resolution_scale_x(),
      render_target_cache_->draw_resolution_scale_y(), REXCVAR_GET(forza_native_width),
      REXCVAR_GET(forza_native_height));
  auto& scene = hdr_scenes_[frame_current_ % kMaxFramesInFlight];
  // A slot is never overwritten while an earlier swap can still sample it.
  if (scene.submission)
    CheckSubmissionFenceAndDeviceLoss(scene.submission);
  if (scene.image && (scene.width != width || scene.height != height))
    DestroyHdrScene(scene);
  const bool existing = scene.image != VK_NULL_HANDLE;
  if (!existing && !ui::vulkan::InstallCopyTarget(*GetVulkanDevice(), width, height, scene)) return;
  EndRenderPass();
  const auto range = ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
  PushImageMemoryBarrier(
      scene.image, range,
      existing ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, existing ? VK_ACCESS_SHADER_READ_BIT : 0,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      existing ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  // Copy only the post-tonemap scene, after reconstruction/ownership transfer
  // and before this first HUD draw. The ordinary renderer resumes unchanged.
  const bool copied = render_target_cache_->CopyColorSnapshot({scene.image,
      VK_FORMAT_A2B10G10R10_UNORM_PACK32, scene.width, scene.height, VK_SAMPLE_COUNT_1_BIT,
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
  PushImageMemoryBarrier(scene.image, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  SubmitBarriers(true);
  scene.submission = GetCurrentSubmission();
  scene.frame = copied ? frame_current_ : UINT64_MAX;
  if (fh1_frame_trace::file) {
    ThreadDrawTraceBudget().Write(fh1_frame_trace::file, frame_current_,
        "S draw=%u frame=%llu image=%016llX copied=%u extent=%u,%u\n",
        fh1_frame_trace::draws ? fh1_frame_trace::draws - 1 : 0,
        static_cast<unsigned long long>(frame_current_),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(
            render_target_cache_->ColorAttachmentImageForDiagnostics())),
        uint32_t(copied), width, height);
  }
  if (copied && !existing)
    REXGPU_INFO("HDR: captured {}x{} scene before HUD", width, height);
}

void VulkanCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  SCOPE_profile_cpu_f("gpu");
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;

  if (!graphics_system_)
    return;
  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    REXGPU_ERROR("XELOG_GPU PRESENT: NO PRESENTER");
    return;
  }

  // In case the swap command is the only one in the frame.
  if (!BeginSubmission(true)) {
    REXGPU_ERROR("XELOG_GPU PRESENT: BeginSubmission FAILED");
    return;
  }

  bool skip_present_due_async_placeholder = REXCVAR_GET(async_shader_compilation) &&
                                            REXCVAR_GET(vulkan_async_skip_incomplete_frames) &&
                                            frame_used_async_placeholder_pipeline_;
  // FH1 (#167): the pipelines are built in the background across frames now (a frame waits for
  // them only briefly, Fh1PipelineWaitMs), so frames that need one are held back for at most
  // vulkan_async_skip_incomplete_frames_ms, then shown without those draws until it is ready.
  // Only object draws, though: a frame that skipped a draw whose absence leaves stale contents
  // (Fh1SkipMustHoldFrame) is held however long the build takes.
  bool held_for_stale_contents = false;
  if (!frame_used_async_placeholder_pipeline_) {
    fh1_incomplete_frames_ = false;
  } else {
    const auto now = std::chrono::steady_clock::now();
    if (!fh1_incomplete_frames_) {
      fh1_incomplete_frames_ = true;
      fh1_incomplete_since_ = now;
    }
    const uint32_t hold_ms = REXCVAR_GET(vulkan_async_skip_incomplete_frames_ms);
    if (skip_present_due_async_placeholder && hold_ms &&
        now - fh1_incomplete_since_ >= std::chrono::milliseconds(hold_ms)) {
      if (fh1_frame_skipped_critical_) {
        held_for_stale_contents = true;
      } else {
        skip_present_due_async_placeholder = false;
      }
    }
  }
  // FH1 (#378): the log's pipeline summary (fh1_pipeline_lookahead.h).
  ++fh1_pipeline_lookahead::stats.frames;
  if (skip_present_due_async_placeholder) {
    ++fh1_pipeline_lookahead::stats.skipped_frames;
    if (held_for_stale_contents) ++fh1_pipeline_lookahead::stats.held_for_stale_contents;
  } else if (frame_used_async_placeholder_pipeline_ &&
             REXCVAR_GET(async_shader_compilation)) {
    // Shown with draws missing: after the hold, or with vulkan_async_skip_incomplete_frames off.
    ++fh1_pipeline_lookahead::stats.incomplete_frames_shown;
  }
  fh1_pipeline_lookahead::LogSummary();
  if (skip_present_due_async_placeholder) {
    static bool skipped_incomplete_frame_logged = false;
    if (!skipped_incomplete_frame_logged) {
      skipped_incomplete_frame_logged = true;
      REXGPU_WARN(
          "Skipping Vulkan frame presentation due to async placeholder draw "
          "usage in this frame");
    }
    EndSubmission(true);
    return;
  }

  SwapPostEffect swap_post_effect = GetActualSwapPostEffect();

  // Obtain the actual swap source texture size (resolution-scaled if it's a
  // resolve destination, or not otherwise).
  uint32_t frontbuffer_width_scaled, frontbuffer_height_scaled;
  uint32_t frontbuffer_width_unscaled = 0, frontbuffer_height_unscaled = 0;
  xenos::TextureFormat frontbuffer_format;
  bool swap_source_needs_rb_swap = false;
  VkImageView swap_texture_view = texture_cache_->RequestSwapTexture(
      frontbuffer_width_scaled, frontbuffer_height_scaled, frontbuffer_format,
      &frontbuffer_width_unscaled, &frontbuffer_height_unscaled, &swap_source_needs_rb_swap);
  if (swap_texture_view == VK_NULL_HANDLE) {
    REXGPU_ERROR("XELOG_GPU PRESENT: swap_texture_view=NULL");
    return;
  }
  // The swap gamma / FXAA pass samples source texels by pixel index, but swap
  // textures may be allocation-padded. Prefer the active frontbuffer region
  // from the swap packet, scaled proportionally to the actual source texture.
  auto get_active_swap_dimension = [](uint32_t packet_unscaled, uint32_t source_unscaled,
                                      uint32_t source_scaled) -> uint32_t {
    if (!source_scaled) {
      return 0;
    }
    uint32_t active_unscaled = packet_unscaled ? packet_unscaled : source_unscaled;
    if (!active_unscaled) {
      return source_scaled;
    }
    if (source_unscaled) {
      active_unscaled = std::min(active_unscaled, source_unscaled);
      uint64_t active_scaled =
          (uint64_t(active_unscaled) * source_scaled + (source_unscaled >> 1)) / source_unscaled;
      return uint32_t(std::clamp<uint64_t>(active_scaled, 1, source_scaled));
    }
    return std::min(active_unscaled, source_scaled);
  };
  uint32_t guest_output_width = get_active_swap_dimension(
      frontbuffer_width, frontbuffer_width_unscaled, frontbuffer_width_scaled);
  uint32_t guest_output_height = get_active_swap_dimension(
      frontbuffer_height, frontbuffer_height_unscaled, frontbuffer_height_scaled);
  if (!guest_output_width) {
    guest_output_width = frontbuffer_width_scaled
                             ? frontbuffer_width_scaled
                             : (frontbuffer_width ? frontbuffer_width : frontbuffer_width_unscaled);
  }
  if (!guest_output_height) {
    guest_output_height =
        frontbuffer_height_scaled
            ? frontbuffer_height_scaled
            : (frontbuffer_height ? frontbuffer_height : frontbuffer_height_unscaled);
  }
  if (Fh1NativeEnabled() && frontbuffer_width_unscaled == 1280 && frontbuffer_width_scaled > 1280) {
    guest_output_width = std::min(guest_output_width, uint32_t(REXCVAR_GET(forza_native_width)));
    guest_output_height = std::min(guest_output_height, uint32_t(REXCVAR_GET(forza_native_height)));
  }
  if (fh1_split_enabled_) {
    guest_output_width = REXCVAR_GET(fh1_output_width);
    guest_output_height = REXCVAR_GET(fh1_output_height);
  }
  bool swap_source_scaled = frontbuffer_width_unscaled && frontbuffer_height_unscaled &&
                            (frontbuffer_width_scaled != frontbuffer_width_unscaled ||
                             frontbuffer_height_scaled != frontbuffer_height_unscaled);
  if (texture_cache_->IsDrawResolutionScaled()) {
    static bool draw_scale_cache_mismatch_logged = false;
    uint32_t texture_scale_x = texture_cache_->draw_resolution_scale_x();
    uint32_t texture_scale_y = texture_cache_->draw_resolution_scale_y();
    uint32_t rt_scale_x =
        render_target_cache_ ? render_target_cache_->draw_resolution_scale_x() : texture_scale_x;
    uint32_t rt_scale_y =
        render_target_cache_ ? render_target_cache_->draw_resolution_scale_y() : texture_scale_y;
    if (!draw_scale_cache_mismatch_logged &&
        (texture_scale_x != rt_scale_x || texture_scale_y != rt_scale_y)) {
      draw_scale_cache_mismatch_logged = true;
      REXGPU_WARN(
          "Vulkan draw-scale mismatch: texture cache is {}x{}, render target "
          "cache is {}x{}",
          texture_scale_x, texture_scale_y, rt_scale_x, rt_scale_y);
    }
    static bool draw_scale_swap_sizes_logged = false;
    if (!draw_scale_swap_sizes_logged) {
      draw_scale_swap_sizes_logged = true;
      REXGPU_WARN(
          "Vulkan draw-scale swap sizing: packet={}x{}, src_scaled={}x{}, "
          "src_unscaled={}x{}, active={}x{}",
          frontbuffer_width, frontbuffer_height, frontbuffer_width_scaled,
          frontbuffer_height_scaled, frontbuffer_width_unscaled, frontbuffer_height_unscaled,
          guest_output_width, guest_output_height);
    }
  }
  if (texture_cache_->IsDrawResolutionScaled() && !swap_source_scaled) {
    static bool draw_scale_swap_unscaled_logged = false;
    if (!draw_scale_swap_unscaled_logged) {
      draw_scale_swap_unscaled_logged = true;
      REXGPU_WARN(
          "Vulkan draw resolution scaling is enabled, but the swap source is "
          "unscaled ({}x{}). This title may be presenting from an unscaled "
          "resolve path.",
          frontbuffer_width_scaled, frontbuffer_height_scaled);
    }
  }
  REXGPU_DEBUG(
      "XELOG_GPU PRESENT: swap_texture_view={:p} packet_size={}x{} src_size={}x{} "
      "src_unscaled={}x{} guest_output_size={}x{} format={}",
      static_cast<void*>(swap_texture_view), frontbuffer_width, frontbuffer_height,
      frontbuffer_width_scaled, frontbuffer_height_scaled, frontbuffer_width_unscaled,
      frontbuffer_height_unscaled, guest_output_width, guest_output_height,
      static_cast<uint32_t>(frontbuffer_format));

  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  uint32_t display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  uint32_t display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));
  if (REXCVAR_GET(forza_present_16_9)) {
    display_width = 16;
    display_height = 9;
  }

  auto& hdr_scene = hdr_scenes_[frame_current_ % kMaxFramesInFlight];
  VkImageView hud_snapshot_view =
      ui::vulkan::GetHdrOutputState().active && hdr_scene.frame == frame_current_ &&
              hdr_scene.width == guest_output_width && hdr_scene.height == guest_output_height
          ? hdr_scene.view
          : VK_NULL_HANDLE;

  // Patch 0393: this frame's pre-HUD snapshot for the VR layers.
  const VkImageView vr_snapshot_view =
      Fh1VrLayersWanted() && hdr_scene.frame == frame_current_ &&
              hdr_scene.width == guest_output_width && hdr_scene.height == guest_output_height
          ? hdr_scene.view
          : VK_NULL_HANDLE;

  presenter->RefreshGuestOutput(
      guest_output_width, guest_output_height, display_width, display_height,
      [this, guest_output_width, guest_output_height, frontbuffer_format, swap_texture_view,
       swap_post_effect, swap_source_needs_rb_swap, hud_snapshot_view,
       vr_snapshot_view](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        // In case the swap command is the only one in the frame.
        if (!BeginSubmission(true)) {
          return false;
        }

        auto& vulkan_context =
            static_cast<ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        uint64_t guest_output_image_version = vulkan_context.image_version();

        const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
        const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
        const VkDevice device = vulkan_device->device();

        uint32_t swap_frame_index = uint32_t(frame_current_ % kMaxFramesInFlight);
        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||
                        swap_post_effect == SwapPostEffect::kFxaaExtreme;

        // This is according to D3D::InitializePresentationParameters from a
        // game executable, which initializes the 256-entry table gamma ramp for
        // 8_8_8_8 output and the PWL gamma ramp for 2_10_10_10.
        // TODO(Triang3l): Choose between the table and PWL based on
        // DC_LUTA_CONTROL, support both for all formats (and also different
        // increments for PWL).
        bool use_pwl_gamma_ramp =
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
        bool swap_source_requires_compute_rb_swap =
            !vulkan_device->properties().imageViewFormatSwizzle && swap_source_needs_rb_swap;
        auto select_swap_apply_gamma_compute_pipeline = [&](bool use_pwl,
                                                            bool use_fxaa_luma) -> VkPipeline {
          if (use_pwl) {
            if (use_fxaa_luma) {
              return swap_source_requires_compute_rb_swap
                         ? swap_apply_gamma_compute_pwl_fxaa_luma_rb_swap_pipeline_
                         : swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_;
            }
            return swap_source_requires_compute_rb_swap
                       ? swap_apply_gamma_compute_pwl_rb_swap_pipeline_
                       : swap_apply_gamma_compute_pwl_pipeline_;
          }
          if (use_fxaa_luma) {
            return swap_source_requires_compute_rb_swap
                       ? swap_apply_gamma_compute_256_entry_table_fxaa_luma_rb_swap_pipeline_
                       : swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_;
          }
          return swap_source_requires_compute_rb_swap
                     ? swap_apply_gamma_compute_256_entry_table_rb_swap_pipeline_
                     : swap_apply_gamma_compute_256_entry_table_pipeline_;
        };
        VkPipeline swap_apply_gamma_compute_pipeline =
            select_swap_apply_gamma_compute_pipeline(use_pwl_gamma_ramp, use_fxaa);

        if (use_fxaa) {
          if (swap_apply_gamma_compute_pipeline == VK_NULL_HANDLE ||
              swap_fxaa_pipeline_ == VK_NULL_HANDLE ||
              swap_fxaa_extreme_pipeline_ == VK_NULL_HANDLE) {
            static bool fxaa_pipelines_unavailable_logged = false;
            if (!fxaa_pipelines_unavailable_logged) {
              if (swap_source_requires_compute_rb_swap) {
                REXGPU_WARN(
                    "Vulkan FXAA swap effect requested but FXAA compute "
                    "pipelines (including RB-swap fallback) are unavailable, "
                    "falling back to gamma only");
              } else {
                REXGPU_WARN(
                    "Vulkan FXAA swap effect requested but FXAA compute "
                    "pipelines are unavailable, falling back to gamma only");
              }
              fxaa_pipelines_unavailable_logged = true;
            }
            use_fxaa = false;
          } else if (!EnsureSwapFxaaSourceImage(guest_output_width, guest_output_height)) {
            static bool fxaa_source_image_failed_logged = false;
            if (!fxaa_source_image_failed_logged) {
              REXGPU_WARN(
                  "Failed to create the Vulkan FXAA source image, falling "
                  "back to gamma-only presentation");
              fxaa_source_image_failed_logged = true;
            }
            use_fxaa = false;
          }
        }
        if (!use_fxaa) {
          swap_apply_gamma_compute_pipeline =
              select_swap_apply_gamma_compute_pipeline(use_pwl_gamma_ramp, false);
        }
        if (swap_source_requires_compute_rb_swap &&
            swap_apply_gamma_compute_pipeline == VK_NULL_HANDLE) {
          static bool compute_rb_swap_fallback_missing_logged = false;
          if (!compute_rb_swap_fallback_missing_logged) {
            compute_rb_swap_fallback_missing_logged = true;
            REXGPU_WARN(
                "Vulkan imageViewFormatSwizzle is unavailable and the swap "
                "source needs red/blue swizzle, but the compute fallback "
                "pipeline is unavailable; falling back to graphics "
                "presentation path");
          }
        }
        bool use_compute_gamma = swap_apply_gamma_compute_pipeline != VK_NULL_HANDLE;
        if (fh1_dlss_ && !use_compute_gamma) {
          REXGPU_ERROR("DLSS: native output requires the texel-addressed compute presentation path");
          return false;
        }

        // TODO(Triang3l): FXAA can result in more than 8 bits of precision.
        context.SetIs8bpc(!use_pwl_gamma_ramp && !use_fxaa);

        // Update the gamma ramp if it's out of date.
        uint32_t& gamma_ramp_frame_index_ref = use_pwl_gamma_ramp
                                                   ? gamma_ramp_pwl_current_frame_
                                                   : gamma_ramp_256_entry_table_current_frame_;
        if (gamma_ramp_frame_index_ref == UINT32_MAX) {
          constexpr uint32_t kGammaRampSize256EntryTable = sizeof(uint32_t) * 256;
          constexpr uint32_t kGammaRampSizePWL = sizeof(uint16_t) * 2 * 3 * 128;
          constexpr uint32_t kGammaRampSize = kGammaRampSize256EntryTable + kGammaRampSizePWL;
          uint32_t gamma_ramp_offset_in_frame =
              use_pwl_gamma_ramp ? kGammaRampSize256EntryTable : 0;
          uint32_t gamma_ramp_upload_offset =
              kGammaRampSize * swap_frame_index + gamma_ramp_offset_in_frame;
          uint32_t gamma_ramp_size =
              use_pwl_gamma_ramp ? kGammaRampSizePWL : kGammaRampSize256EntryTable;
          void* gamma_ramp_frame_upload =
              reinterpret_cast<uint8_t*>(gamma_ramp_upload_mapping_) + gamma_ramp_upload_offset;
          if (std::endian::native != std::endian::little && use_pwl_gamma_ramp) {
            // R16G16 is first R16, where the shader expects the base, and
            // second G16, where the delta should be, but gamma_ramp_pwl_rgb()
            // is an array of 32-bit DC_LUT_PWL_DATA registers - swap 16 bits in
            // each 32.
            auto gamma_ramp_pwl_upload =
                reinterpret_cast<reg::DC_LUT_PWL_DATA*>(gamma_ramp_frame_upload);
            const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = gamma_ramp_pwl_rgb();
            for (size_t i = 0; i < 128 * 3; ++i) {
              reg::DC_LUT_PWL_DATA& gamma_ramp_pwl_upload_entry = gamma_ramp_pwl_upload[i];
              reg::DC_LUT_PWL_DATA gamma_ramp_pwl_entry = gamma_ramp_pwl[i];
              gamma_ramp_pwl_upload_entry.base = gamma_ramp_pwl_entry.delta;
              gamma_ramp_pwl_upload_entry.delta = gamma_ramp_pwl_entry.base;
            }
          } else {
            std::memcpy(gamma_ramp_frame_upload,
                        use_pwl_gamma_ramp ? static_cast<const void*>(gamma_ramp_pwl_rgb())
                                           : static_cast<const void*>(gamma_ramp_256_entry_table()),
                        gamma_ramp_size);
          }
          bool gamma_ramp_has_upload_buffer = gamma_ramp_upload_buffer_memory_ != VK_NULL_HANDLE;
          VkPipelineStageFlags gamma_ramp_read_stage_mask =
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
          ui::vulkan::util::FlushMappedMemoryRange(
              vulkan_device,
              gamma_ramp_has_upload_buffer ? gamma_ramp_upload_buffer_memory_
                                           : gamma_ramp_buffer_memory_,
              gamma_ramp_upload_memory_type_, gamma_ramp_upload_offset,
              gamma_ramp_upload_memory_size_, gamma_ramp_size);
          if (gamma_ramp_has_upload_buffer) {
            // Copy from the host-visible buffer to the device-local one.
            PushBufferMemoryBarrier(gamma_ramp_buffer_, gamma_ramp_offset_in_frame, gamma_ramp_size,
                                    gamma_ramp_read_stage_mask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
            SubmitBarriers(true);
            VkBufferCopy gamma_ramp_buffer_copy;
            gamma_ramp_buffer_copy.srcOffset = gamma_ramp_upload_offset;
            gamma_ramp_buffer_copy.dstOffset = gamma_ramp_offset_in_frame;
            gamma_ramp_buffer_copy.size = gamma_ramp_size;
            deferred_command_buffer_.CmdVkCopyBuffer(gamma_ramp_upload_buffer_, gamma_ramp_buffer_,
                                                     1, &gamma_ramp_buffer_copy);
            PushBufferMemoryBarrier(gamma_ramp_buffer_, gamma_ramp_offset_in_frame, gamma_ramp_size,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, gamma_ramp_read_stage_mask,
                                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
          }
          // The device-local, but not host-visible, gamma ramp buffer doesn't
          // have per-frame sets of gamma ramps.
          gamma_ramp_frame_index_ref = gamma_ramp_has_upload_buffer ? 0 : swap_frame_index;
        }

        VkDescriptorSet swap_descriptor_source = swap_descriptors_source_[swap_frame_index];
        VkDescriptorImageInfo swap_descriptor_source_image_info;
        swap_descriptor_source_image_info.sampler = VK_NULL_HANDLE;
        swap_descriptor_source_image_info.imageView = swap_texture_view;
        swap_descriptor_source_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet swap_descriptor_source_write;
        swap_descriptor_source_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        swap_descriptor_source_write.pNext = nullptr;
        swap_descriptor_source_write.dstSet = swap_descriptor_source;
        swap_descriptor_source_write.dstBinding = 0;
        swap_descriptor_source_write.dstArrayElement = 0;
        swap_descriptor_source_write.descriptorCount = 1;
        swap_descriptor_source_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        swap_descriptor_source_write.pImageInfo = &swap_descriptor_source_image_info;
        swap_descriptor_source_write.pBufferInfo = nullptr;
        swap_descriptor_source_write.pTexelBufferView = nullptr;
        dfn.vkUpdateDescriptorSets(device, 1, &swap_descriptor_source_write, 0, nullptr);

        VkImageSubresourceRange guest_output_subresource_range =
            ui::vulkan::util::InitializeSubresourceRange();
        if (use_compute_gamma) {
          // Transition the destination image for compute writes. Contents are
          // fully overwritten, so old layout can always be UNDEFINED.
          PushImageMemoryBarrier(vulkan_context.image(), guest_output_subresource_range,
                                 vulkan_context.image_ever_written_previously()
                                     ? ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask
                                     : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 vulkan_context.image_ever_written_previously()
                                     ? ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask
                                     : 0,
                                 VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_GENERAL);
          if (use_fxaa) {
            PushImageMemoryBarrier(swap_fxaa_source_image_, guest_output_subresource_range,
                                   swap_fxaa_source_stage_mask_ ? swap_fxaa_source_stage_mask_
                                                                : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   swap_fxaa_source_access_mask_, VK_ACCESS_SHADER_WRITE_BIT,
                                   swap_fxaa_source_layout_, VK_IMAGE_LAYOUT_GENERAL);
          }
          SubmitBarriers(true);

          if (use_fxaa) {
            swap_fxaa_source_stage_mask_ = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            swap_fxaa_source_access_mask_ = VK_ACCESS_SHADER_WRITE_BIT;
            swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_GENERAL;
            swap_fxaa_source_image_submission_ = GetCurrentSubmission();
          }

          VkDescriptorSet swap_descriptor_destination_storage =
              swap_descriptors_destination_storage_[swap_frame_index];
          VkDescriptorImageInfo swap_descriptor_destination_storage_image_info;
          swap_descriptor_destination_storage_image_info.sampler = VK_NULL_HANDLE;
          swap_descriptor_destination_storage_image_info.imageView =
              use_fxaa ? swap_fxaa_source_image_view_ : vulkan_context.image_view();
          swap_descriptor_destination_storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
          VkWriteDescriptorSet swap_descriptor_destination_storage_write;
          swap_descriptor_destination_storage_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          swap_descriptor_destination_storage_write.pNext = nullptr;
          swap_descriptor_destination_storage_write.dstSet = swap_descriptor_destination_storage;
          swap_descriptor_destination_storage_write.dstBinding = 0;
          swap_descriptor_destination_storage_write.dstArrayElement = 0;
          swap_descriptor_destination_storage_write.descriptorCount = 1;
          swap_descriptor_destination_storage_write.descriptorType =
              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          swap_descriptor_destination_storage_write.pImageInfo =
              &swap_descriptor_destination_storage_image_info;
          swap_descriptor_destination_storage_write.pBufferInfo = nullptr;
          swap_descriptor_destination_storage_write.pTexelBufferView = nullptr;
          dfn.vkUpdateDescriptorSets(device, 1, &swap_descriptor_destination_storage_write, 0,
                                     nullptr);

          std::array<VkDescriptorSet, kSwapApplyGammaComputeDescriptorSetCount>
              swap_apply_gamma_compute_descriptor_sets{};
          swap_apply_gamma_compute_descriptor_sets[kSwapApplyGammaComputeDescriptorSetRamp] =
              swap_descriptors_gamma_ramp_[2 * gamma_ramp_frame_index_ref +
                                           uint32_t(use_pwl_gamma_ramp)];
          swap_apply_gamma_compute_descriptor_sets[kSwapApplyGammaComputeDescriptorSetSource] =
              swap_descriptor_source;
          swap_apply_gamma_compute_descriptor_sets[kSwapApplyGammaComputeDescriptorSetDestination] =
              swap_descriptor_destination_storage;
          deferred_command_buffer_.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_COMPUTE, swap_apply_gamma_compute_pipeline_layout_, 0,
              uint32_t(swap_apply_gamma_compute_descriptor_sets.size()),
              swap_apply_gamma_compute_descriptor_sets.data(), 0, nullptr);
          SwapApplyGammaConstants swap_apply_gamma_constants = {
              {guest_output_width, guest_output_height}};
          deferred_command_buffer_.CmdVkPushConstants(
              swap_apply_gamma_compute_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
              sizeof(swap_apply_gamma_constants), &swap_apply_gamma_constants);
          BindExternalComputePipeline(swap_apply_gamma_compute_pipeline);
          uint32_t group_count_x = (guest_output_width + 15) / 16;
          uint32_t group_count_y = (guest_output_height + 7) / 8;
          deferred_command_buffer_.CmdVkDispatch(group_count_x, group_count_y, 1);

          if (use_fxaa) {
            // Make the FXAA source image readable and bind a separate
            // destination storage descriptor targeting the guest output image.
            PushImageMemoryBarrier(swap_fxaa_source_image_, guest_output_subresource_range,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            SubmitBarriers(true);
            swap_fxaa_source_stage_mask_ = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            swap_fxaa_source_access_mask_ = VK_ACCESS_SHADER_READ_BIT;
            swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            swap_fxaa_source_image_submission_ = GetCurrentSubmission();

            VkDescriptorSet swap_descriptor_fxaa_source =
                swap_descriptors_fxaa_source_[swap_frame_index];
            VkDescriptorImageInfo swap_descriptor_fxaa_source_image_info;
            swap_descriptor_fxaa_source_image_info.sampler = swap_sampler_linear_clamp_;
            swap_descriptor_fxaa_source_image_info.imageView = swap_fxaa_source_image_view_;
            swap_descriptor_fxaa_source_image_info.imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet swap_descriptor_fxaa_source_write;
            swap_descriptor_fxaa_source_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            swap_descriptor_fxaa_source_write.pNext = nullptr;
            swap_descriptor_fxaa_source_write.dstSet = swap_descriptor_fxaa_source;
            swap_descriptor_fxaa_source_write.dstBinding = 0;
            swap_descriptor_fxaa_source_write.dstArrayElement = 0;
            swap_descriptor_fxaa_source_write.descriptorCount = 1;
            swap_descriptor_fxaa_source_write.descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            swap_descriptor_fxaa_source_write.pImageInfo = &swap_descriptor_fxaa_source_image_info;
            swap_descriptor_fxaa_source_write.pBufferInfo = nullptr;
            swap_descriptor_fxaa_source_write.pTexelBufferView = nullptr;
            dfn.vkUpdateDescriptorSets(device, 1, &swap_descriptor_fxaa_source_write, 0, nullptr);

            VkDescriptorSet swap_descriptor_fxaa_destination_storage =
                swap_descriptors_fxaa_destination_storage_[swap_frame_index];
            VkDescriptorImageInfo swap_descriptor_fxaa_destination_storage_image_info;
            swap_descriptor_fxaa_destination_storage_image_info.sampler = VK_NULL_HANDLE;
            swap_descriptor_fxaa_destination_storage_image_info.imageView =
                vulkan_context.image_view();
            swap_descriptor_fxaa_destination_storage_image_info.imageLayout =
                VK_IMAGE_LAYOUT_GENERAL;
            VkWriteDescriptorSet swap_descriptor_fxaa_destination_storage_write =
                swap_descriptor_destination_storage_write;
            swap_descriptor_fxaa_destination_storage_write.dstSet =
                swap_descriptor_fxaa_destination_storage;
            swap_descriptor_fxaa_destination_storage_write.pImageInfo =
                &swap_descriptor_fxaa_destination_storage_image_info;
            dfn.vkUpdateDescriptorSets(device, 1, &swap_descriptor_fxaa_destination_storage_write,
                                       0, nullptr);

            std::array<VkDescriptorSet, kSwapFxaaDescriptorSetCount> swap_fxaa_descriptor_sets{};
            swap_fxaa_descriptor_sets[kSwapFxaaDescriptorSetSource] = swap_descriptor_fxaa_source;
            swap_fxaa_descriptor_sets[kSwapFxaaDescriptorSetDestination] =
                swap_descriptor_fxaa_destination_storage;
            deferred_command_buffer_.CmdVkBindDescriptorSets(
                VK_PIPELINE_BIND_POINT_COMPUTE, swap_fxaa_pipeline_layout_, 0,
                uint32_t(swap_fxaa_descriptor_sets.size()), swap_fxaa_descriptor_sets.data(), 0,
                nullptr);
            SwapFxaaConstants swap_fxaa_constants = {
                {guest_output_width, guest_output_height},
                {1.0f / float(guest_output_width), 1.0f / float(guest_output_height)}};
            deferred_command_buffer_.CmdVkPushConstants(
                swap_fxaa_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(swap_fxaa_constants), &swap_fxaa_constants);
            BindExternalComputePipeline(swap_post_effect == SwapPostEffect::kFxaaExtreme
                                            ? swap_fxaa_extreme_pipeline_
                                            : swap_fxaa_pipeline_);
            deferred_command_buffer_.CmdVkDispatch(group_count_x, group_count_y, 1);
          }

          if (hud_snapshot_view != VK_NULL_HANDLE && EnsureSwapHudMask()) {
            PushImageMemoryBarrier(vulkan_context.image(), guest_output_subresource_range,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            SubmitBarriers(true);
            const auto set = swap_hud_mask_descriptor_sets_[swap_frame_index];
            const std::array images{
                ui::vulkan::ComputeImageBinding{swap_texture_view, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                ui::vulkan::ComputeImageBinding{hud_snapshot_view, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                ui::vulkan::ComputeImageBinding{vulkan_context.image_view(), VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                VK_IMAGE_LAYOUT_GENERAL}};
            ui::vulkan::WriteComputeImages(*GetVulkanDevice(), set, images);
            BindExternalComputePipeline(swap_hud_mask_pipeline_);
            ui::vulkan::RecordCoverageDispatch(deferred_command_buffer_, swap_hud_mask_pipeline_layout_,
                set, guest_output_width, guest_output_height,
                !vulkan_device->properties().imageViewFormatSwizzle && swap_source_needs_rb_swap,
                frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
                    frontbuffer_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16
                    ? 1.0f / 1023 : 1.0f / 255);
          }

          if (vr_snapshot_view != VK_NULL_HANDLE) {
            Fh1VrLayers(vulkan_context.image(), vulkan_context.image_view(), guest_output_width,
                        guest_output_height, vr_snapshot_view,
                        select_swap_apply_gamma_compute_pipeline(use_pwl_gamma_ramp, false),
                        swap_descriptors_gamma_ramp_[2 * gamma_ramp_frame_index_ref +
                                                     uint32_t(use_pwl_gamma_ramp)],
                        swap_frame_index);
          }

          // Insert the release barrier.
          PushImageMemoryBarrier(vulkan_context.image(), guest_output_subresource_range,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                                 VK_ACCESS_SHADER_WRITE_BIT,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
                                 VK_IMAGE_LAYOUT_GENERAL,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout);
        } else {
          // Make sure a framebuffer is available for the current guest output
          // image version.
          size_t swap_framebuffer_index = SIZE_MAX;
          size_t swap_framebuffer_new_index = SIZE_MAX;
          // Try to find the existing framebuffer for the current guest output
          // image version, or an unused (without an existing framebuffer, or
          // with one, but that has never actually been used dynamically) slot.
          for (size_t i = 0; i < swap_framebuffers_.size(); ++i) {
            const SwapFramebuffer& existing_swap_framebuffer = swap_framebuffers_[i];
            if (existing_swap_framebuffer.framebuffer != VK_NULL_HANDLE &&
                existing_swap_framebuffer.version == guest_output_image_version) {
              swap_framebuffer_index = i;
              break;
            }
            if (existing_swap_framebuffer.framebuffer == VK_NULL_HANDLE ||
                !existing_swap_framebuffer.last_submission) {
              swap_framebuffer_new_index = i;
            }
          }
          if (swap_framebuffer_index == SIZE_MAX) {
            if (swap_framebuffer_new_index == SIZE_MAX) {
              // Replace the earliest used framebuffer.
              swap_framebuffer_new_index = 0;
              for (size_t i = 1; i < swap_framebuffers_.size(); ++i) {
                if (swap_framebuffers_[i].last_submission <
                    swap_framebuffers_[swap_framebuffer_new_index].last_submission) {
                  swap_framebuffer_new_index = i;
                }
              }
            }
            swap_framebuffer_index = swap_framebuffer_new_index;
            SwapFramebuffer& new_swap_framebuffer = swap_framebuffers_[swap_framebuffer_new_index];
            if (new_swap_framebuffer.framebuffer != VK_NULL_HANDLE) {
              if (submission_completed_ >= new_swap_framebuffer.last_submission) {
                dfn.vkDestroyFramebuffer(device, new_swap_framebuffer.framebuffer, nullptr);
              } else {
                destroy_framebuffers_.emplace_back(new_swap_framebuffer.last_submission,
                                                   new_swap_framebuffer.framebuffer);
              }
              new_swap_framebuffer.framebuffer = VK_NULL_HANDLE;
            }
            VkImageView guest_output_image_view = vulkan_context.image_view();
            VkFramebufferCreateInfo swap_framebuffer_create_info;
            swap_framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            swap_framebuffer_create_info.pNext = nullptr;
            swap_framebuffer_create_info.flags = 0;
            swap_framebuffer_create_info.renderPass = swap_apply_gamma_render_pass_;
            swap_framebuffer_create_info.attachmentCount = 1;
            swap_framebuffer_create_info.pAttachments = &guest_output_image_view;
            swap_framebuffer_create_info.width = guest_output_width;
            swap_framebuffer_create_info.height = guest_output_height;
            swap_framebuffer_create_info.layers = 1;
            if (dfn.vkCreateFramebuffer(device, &swap_framebuffer_create_info, nullptr,
                                        &new_swap_framebuffer.framebuffer) != VK_SUCCESS) {
              REXGPU_ERROR("Failed to create the Vulkan framebuffer for presentation");
              return false;
            }
            new_swap_framebuffer.version = guest_output_image_version;
            // The actual submission index will be set if the framebuffer is
            // actually used, not dropped due to some error.
            new_swap_framebuffer.last_submission = 0;
          }

          if (vulkan_context.image_ever_written_previously()) {
            // Insert a barrier after the last presenter's usage of the guest
            // output image. Will be overwriting all the contents, so oldLayout
            // layout is UNDEFINED. The render pass will do the layout
            // transition, but newLayout must not be UNDEFINED.
            PushImageMemoryBarrier(vulkan_context.image(), guest_output_subresource_range,
                                   ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
          }

          // End the current render pass before inserting barriers and starting
          // a new one, and insert the barrier.
          SubmitBarriers(true);

          SwapFramebuffer& swap_framebuffer = swap_framebuffers_[swap_framebuffer_index];
          swap_framebuffer.last_submission = GetCurrentSubmission();

          VkRenderPassBeginInfo render_pass_begin_info;
          render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
          render_pass_begin_info.pNext = nullptr;
          render_pass_begin_info.renderPass = swap_apply_gamma_render_pass_;
          render_pass_begin_info.framebuffer = swap_framebuffer.framebuffer;
          render_pass_begin_info.renderArea.offset.x = 0;
          render_pass_begin_info.renderArea.offset.y = 0;
          render_pass_begin_info.renderArea.extent.width = guest_output_width;
          render_pass_begin_info.renderArea.extent.height = guest_output_height;
          render_pass_begin_info.clearValueCount = 0;
          render_pass_begin_info.pClearValues = nullptr;
          deferred_command_buffer_.CmdVkBeginRenderPass(&render_pass_begin_info,
                                                        VK_SUBPASS_CONTENTS_INLINE);

          VkViewport viewport;
          viewport.x = 0.0f;
          viewport.y = 0.0f;
          viewport.width = float(guest_output_width);
          viewport.height = float(guest_output_height);
          viewport.minDepth = 0.0f;
          viewport.maxDepth = 1.0f;
          SetViewport(viewport);
          VkRect2D scissor;
          scissor.offset.x = 0;
          scissor.offset.y = 0;
          scissor.extent.width = guest_output_width;
          scissor.extent.height = guest_output_height;
          SetScissor(scissor);

          VkPipeline swap_apply_gamma_pipeline = use_pwl_gamma_ramp
                                                     ? swap_apply_gamma_pwl_pipeline_
                                                     : swap_apply_gamma_256_entry_table_pipeline_;
          if (!vulkan_device->properties().imageViewFormatSwizzle && swap_source_needs_rb_swap) {
            VkPipeline swap_apply_gamma_rb_swap_pipeline =
                use_pwl_gamma_ramp ? swap_apply_gamma_pwl_rb_swap_pipeline_
                                   : swap_apply_gamma_256_entry_table_rb_swap_pipeline_;
            if (swap_apply_gamma_rb_swap_pipeline != VK_NULL_HANDLE) {
              swap_apply_gamma_pipeline = swap_apply_gamma_rb_swap_pipeline;
            } else {
              static bool swap_rb_swap_fallback_missing_logged = false;
              if (!swap_rb_swap_fallback_missing_logged) {
                swap_rb_swap_fallback_missing_logged = true;
                REXGPU_WARN(
                    "Vulkan imageViewFormatSwizzle is unavailable and the "
                    "swap source needs red/blue swizzle, but the graphics "
                    "fallback RB-swap shader pipeline is unavailable");
              }
            }
          }
          BindExternalGraphicsPipeline(swap_apply_gamma_pipeline);

          std::array<VkDescriptorSet, kSwapApplyGammaDescriptorSetCount> swap_descriptor_sets{};
          swap_descriptor_sets[kSwapApplyGammaDescriptorSetRamp] =
              swap_descriptors_gamma_ramp_[2 * gamma_ramp_frame_index_ref +
                                           uint32_t(use_pwl_gamma_ramp)];
          swap_descriptor_sets[kSwapApplyGammaDescriptorSetSource] = swap_descriptor_source;
          deferred_command_buffer_.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, swap_apply_gamma_pipeline_layout_, 0,
              uint32_t(swap_descriptor_sets.size()), swap_descriptor_sets.data(), 0, nullptr);

          deferred_command_buffer_.CmdVkDraw(3, 1, 0, 0);
          deferred_command_buffer_.CmdVkEndRenderPass();

          // Insert the release barrier.
          PushImageMemoryBarrier(vulkan_context.image(), guest_output_subresource_range,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout);
        }

        // Need to submit all the commands before giving the image back to the
        // presenter so it can submit its own commands for displaying it to the
        // queue, and also need to submit the release barrier.
        EndSubmission(true);
        // With threaded submission, they must be in the queue, not only handed over.
        AwaitSubmissionThread(GetCurrentSubmission() - 1);
        return true;
      });

  // End the frame even if did not present for any reason (the image refresher
  // was not called), to prevent leaking per-frame resources.
  EndSubmission(true);
}

bool VulkanCommandProcessor::PushBufferMemoryBarrier(
    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, VkPipelineStageFlags src_stage_mask,
    VkPipelineStageFlags dst_stage_mask, VkAccessFlags src_access_mask,
    VkAccessFlags dst_access_mask, uint32_t src_queue_family_index, uint32_t dst_queue_family_index,
    bool skip_if_equal) {
  if (skip_if_equal && src_stage_mask == dst_stage_mask && src_access_mask == dst_access_mask &&
      src_queue_family_index == dst_queue_family_index) {
    return false;
  }

  // Separate different barriers for overlapping buffer ranges into different
  // pipeline barrier commands.
  for (const VkBufferMemoryBarrier& other_buffer_memory_barrier :
       pending_barriers_buffer_memory_barriers_) {
    if (other_buffer_memory_barrier.buffer != buffer ||
        (size != VK_WHOLE_SIZE && offset + size <= other_buffer_memory_barrier.offset) ||
        (other_buffer_memory_barrier.size != VK_WHOLE_SIZE &&
         other_buffer_memory_barrier.offset + other_buffer_memory_barrier.size <= offset)) {
      continue;
    }
    if (other_buffer_memory_barrier.offset == offset && other_buffer_memory_barrier.size == size &&
        other_buffer_memory_barrier.srcAccessMask == src_access_mask &&
        other_buffer_memory_barrier.dstAccessMask == dst_access_mask &&
        other_buffer_memory_barrier.srcQueueFamilyIndex == src_queue_family_index &&
        other_buffer_memory_barrier.dstQueueFamilyIndex == dst_queue_family_index) {
      // The barrier is already pending.
      current_pending_barrier_.src_stage_mask |= src_stage_mask;
      current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
      return true;
    }
    SplitPendingBarrier();
    break;
  }

  current_pending_barrier_.src_stage_mask |= src_stage_mask;
  current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
  VkBufferMemoryBarrier& buffer_memory_barrier =
      pending_barriers_buffer_memory_barriers_.emplace_back();
  buffer_memory_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  buffer_memory_barrier.pNext = nullptr;
  buffer_memory_barrier.srcAccessMask = src_access_mask;
  buffer_memory_barrier.dstAccessMask = dst_access_mask;
  buffer_memory_barrier.srcQueueFamilyIndex = src_queue_family_index;
  buffer_memory_barrier.dstQueueFamilyIndex = dst_queue_family_index;
  buffer_memory_barrier.buffer = buffer;
  buffer_memory_barrier.offset = offset;
  buffer_memory_barrier.size = size;
  return true;
}

bool VulkanCommandProcessor::PushImageMemoryBarrier(
    VkImage image, const VkImageSubresourceRange& subresource_range,
    VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
    VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask, VkImageLayout old_layout,
    VkImageLayout new_layout, uint32_t src_queue_family_index, uint32_t dst_queue_family_index,
    bool skip_if_equal) {
  if (skip_if_equal && src_stage_mask == dst_stage_mask && src_access_mask == dst_access_mask &&
      old_layout == new_layout && src_queue_family_index == dst_queue_family_index) {
    return false;
  }

  // Separate different barriers for overlapping image subresource ranges into
  // different pipeline barrier commands.
  for (const VkImageMemoryBarrier& other_image_memory_barrier :
       pending_barriers_image_memory_barriers_) {
    if (other_image_memory_barrier.image != image ||
        !(other_image_memory_barrier.subresourceRange.aspectMask & subresource_range.aspectMask) ||
        (subresource_range.levelCount != VK_REMAINING_MIP_LEVELS &&
         subresource_range.baseMipLevel + subresource_range.levelCount <=
             other_image_memory_barrier.subresourceRange.baseMipLevel) ||
        (other_image_memory_barrier.subresourceRange.levelCount != VK_REMAINING_MIP_LEVELS &&
         other_image_memory_barrier.subresourceRange.baseMipLevel +
                 other_image_memory_barrier.subresourceRange.levelCount <=
             subresource_range.baseMipLevel) ||
        (subresource_range.layerCount != VK_REMAINING_ARRAY_LAYERS &&
         subresource_range.baseArrayLayer + subresource_range.layerCount <=
             other_image_memory_barrier.subresourceRange.baseArrayLayer) ||
        (other_image_memory_barrier.subresourceRange.layerCount != VK_REMAINING_ARRAY_LAYERS &&
         other_image_memory_barrier.subresourceRange.baseArrayLayer +
                 other_image_memory_barrier.subresourceRange.layerCount <=
             subresource_range.baseArrayLayer)) {
      continue;
    }
    if (other_image_memory_barrier.subresourceRange.aspectMask == subresource_range.aspectMask &&
        other_image_memory_barrier.subresourceRange.baseMipLevel ==
            subresource_range.baseMipLevel &&
        other_image_memory_barrier.subresourceRange.levelCount == subresource_range.levelCount &&
        other_image_memory_barrier.subresourceRange.baseArrayLayer ==
            subresource_range.baseArrayLayer &&
        other_image_memory_barrier.subresourceRange.layerCount == subresource_range.layerCount &&
        other_image_memory_barrier.srcAccessMask == src_access_mask &&
        other_image_memory_barrier.dstAccessMask == dst_access_mask &&
        other_image_memory_barrier.oldLayout == old_layout &&
        other_image_memory_barrier.newLayout == new_layout &&
        other_image_memory_barrier.srcQueueFamilyIndex == src_queue_family_index &&
        other_image_memory_barrier.dstQueueFamilyIndex == dst_queue_family_index) {
      // The barrier is already pending.
      current_pending_barrier_.src_stage_mask |= src_stage_mask;
      current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
      return true;
    }
    SplitPendingBarrier();
    break;
  }

  current_pending_barrier_.src_stage_mask |= src_stage_mask;
  current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
  VkImageMemoryBarrier& image_memory_barrier =
      pending_barriers_image_memory_barriers_.emplace_back();
  image_memory_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  image_memory_barrier.pNext = nullptr;
  image_memory_barrier.srcAccessMask = src_access_mask;
  image_memory_barrier.dstAccessMask = dst_access_mask;
  image_memory_barrier.oldLayout = old_layout;
  image_memory_barrier.newLayout = new_layout;
  image_memory_barrier.srcQueueFamilyIndex = src_queue_family_index;
  image_memory_barrier.dstQueueFamilyIndex = dst_queue_family_index;
  image_memory_barrier.image = image;
  image_memory_barrier.subresourceRange = subresource_range;
  return true;
}

bool VulkanCommandProcessor::SubmitBarriers(bool force_end_render_pass) {
  assert_true(submission_open_);
  SplitPendingBarrier();
  if (pending_barriers_.empty()) {
    if (force_end_render_pass) {
      EndRenderPass();
    }
    return false;
  }
  EndRenderPass();
  for (auto it = pending_barriers_.cbegin(); it != pending_barriers_.cend(); ++it) {
    auto it_next = std::next(it);
    bool is_last = it_next == pending_barriers_.cend();
    // .data() + offset, not &[offset], for buffer and image barriers, because
    // if there are no buffer or image memory barriers in the last pipeline
    // barriers, the offsets may be equal to the sizes of the vectors.
    deferred_command_buffer_.CmdVkPipelineBarrier(
        it->src_stage_mask ? it->src_stage_mask : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        it->dst_stage_mask ? it->dst_stage_mask : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
        nullptr,
        uint32_t((is_last ? pending_barriers_buffer_memory_barriers_.size()
                          : it_next->buffer_memory_barriers_offset) -
                 it->buffer_memory_barriers_offset),
        pending_barriers_buffer_memory_barriers_.data() + it->buffer_memory_barriers_offset,
        uint32_t((is_last ? pending_barriers_image_memory_barriers_.size()
                          : it_next->image_memory_barriers_offset) -
                 it->image_memory_barriers_offset),
        pending_barriers_image_memory_barriers_.data() + it->image_memory_barriers_offset);
  }
  pending_barriers_.clear();
  pending_barriers_buffer_memory_barriers_.clear();
  pending_barriers_image_memory_barriers_.clear();
  current_pending_barrier_.buffer_memory_barriers_offset = 0;
  current_pending_barrier_.image_memory_barriers_offset = 0;
  return true;
}

void VulkanCommandProcessor::SubmitBarriersAndEnterRenderTargetCacheRenderPass(
    VkRenderPass render_pass, const VulkanRenderTargetCache::Framebuffer* framebuffer) {
  SubmitBarriers(false);
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  bool use_dynamic_rendering =
      REXCVAR_GET(vulkan_dynamic_rendering) && vulkan_device->properties().dynamicRendering;

  if (use_dynamic_rendering) {
    if (in_render_pass_ && current_framebuffer_ == framebuffer &&
        current_render_pass_ == VK_NULL_HANDLE) {
      return;
    }
  } else {
    if (current_render_pass_ == render_pass && current_framebuffer_ == framebuffer) {
      return;
    }
  }

  if (in_render_pass_) {
    if (use_dynamic_rendering) {
      deferred_command_buffer_.CmdVkEndRendering();
    } else {
      deferred_command_buffer_.CmdVkEndRenderPass();
    }
    in_render_pass_ = false;
    hoist_render_pass_ = false;
  }

  current_render_pass_ = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass;
  current_framebuffer_ = framebuffer;

  // Shared memory uploads for the draws in this render pass may be recorded here, after the
  // barriers before it (CanHoistSharedMemoryUpload).
  if (REXCVAR_GET(vulkan_hoist_shared_memory_uploads) && !hoist_read_pages_.empty()) {
    hoist_point_ = deferred_command_buffer_.CmdHoistPoint();
    ++hoist_render_pass_serial_;
    hoist_render_pass_ = true;
  }

  if (use_dynamic_rendering) {
    VkRenderingAttachmentInfo color_attachments[xenos::kMaxColorRenderTargets];
    VkRenderingAttachmentInfo depth_attachment;
    VkRenderingAttachmentInfo stencil_attachment;
    uint32_t color_attachment_count = 0;
    render_target_cache_->GetLastUpdateRenderingAttachments(
        color_attachments, &color_attachment_count, &depth_attachment, &stencil_attachment);
    bool has_depth = depth_attachment.sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    bool has_stencil = stencil_attachment.sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;

    VkRenderingInfo rendering_info = {};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.pNext = nullptr;
    rendering_info.flags = 0;
    rendering_info.renderArea.offset.x = 0;
    rendering_info.renderArea.offset.y = 0;
    rendering_info.renderArea.extent = framebuffer->host_extent;
    rendering_info.layerCount = 1;
    rendering_info.viewMask = 0;
    rendering_info.colorAttachmentCount = color_attachment_count;
    rendering_info.pColorAttachments = color_attachment_count ? color_attachments : nullptr;
    rendering_info.pDepthAttachment = has_depth ? &depth_attachment : nullptr;
    rendering_info.pStencilAttachment = has_stencil ? &stencil_attachment : nullptr;
    deferred_command_buffer_.CmdVkBeginRendering(&rendering_info);
  } else {
    VkRenderPassBeginInfo render_pass_begin_info;
    render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin_info.pNext = nullptr;
    render_pass_begin_info.renderPass = render_pass;
    render_pass_begin_info.framebuffer = framebuffer->framebuffer;
    render_pass_begin_info.renderArea.offset.x = 0;
    render_pass_begin_info.renderArea.offset.y = 0;
    // TODO(Triang3l): Actual dirty width / height in the deferred command
    // buffer.
    render_pass_begin_info.renderArea.extent = framebuffer->host_extent;
    render_pass_begin_info.clearValueCount = 0;
    render_pass_begin_info.pClearValues = nullptr;
    deferred_command_buffer_.CmdVkBeginRenderPass(&render_pass_begin_info,
                                                  VK_SUBPASS_CONTENTS_INLINE);
  }
  in_render_pass_ = true;
}

void VulkanCommandProcessor::SubmitBarriersAndEnterRenderTargetCacheRenderPass(
    VkRenderPass render_pass, const VulkanRenderTargetCache::Framebuffer* framebuffer,
    VkImageView transfer_dest_view, bool transfer_dest_is_depth) {
  SubmitBarriers(false);
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  bool use_dynamic_rendering =
      REXCVAR_GET(vulkan_dynamic_rendering) && vulkan_device->properties().dynamicRendering;

  if (use_dynamic_rendering) {
    if (in_render_pass_ && current_framebuffer_ == framebuffer &&
        current_render_pass_ == VK_NULL_HANDLE) {
      return;
    }
  } else {
    if (current_render_pass_ == render_pass && current_framebuffer_ == framebuffer) {
      return;
    }
  }

  if (in_render_pass_) {
    if (use_dynamic_rendering) {
      deferred_command_buffer_.CmdVkEndRendering();
    } else {
      deferred_command_buffer_.CmdVkEndRenderPass();
    }
    in_render_pass_ = false;
    hoist_render_pass_ = false;
  }

  current_render_pass_ = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass;
  current_framebuffer_ = framebuffer;

  if (use_dynamic_rendering) {
    VkRenderingAttachmentInfo color_attachment = {};
    VkRenderingAttachmentInfo depth_attachment = {};
    VkRenderingAttachmentInfo stencil_attachment = {};

    if (transfer_dest_is_depth) {
      depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      depth_attachment.pNext = nullptr;
      depth_attachment.imageView = transfer_dest_view;
      depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      depth_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
      depth_attachment.resolveImageView = VK_NULL_HANDLE;
      depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      depth_attachment.clearValue = {};
      stencil_attachment = depth_attachment;
    } else {
      color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      color_attachment.pNext = nullptr;
      color_attachment.imageView = transfer_dest_view;
      color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      color_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
      color_attachment.resolveImageView = VK_NULL_HANDLE;
      color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color_attachment.clearValue = {};
    }

    VkRenderingInfo rendering_info = {};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.pNext = nullptr;
    rendering_info.flags = 0;
    rendering_info.renderArea.offset.x = 0;
    rendering_info.renderArea.offset.y = 0;
    rendering_info.renderArea.extent = framebuffer->host_extent;
    rendering_info.layerCount = 1;
    rendering_info.viewMask = 0;
    rendering_info.colorAttachmentCount = transfer_dest_is_depth ? 0 : 1;
    rendering_info.pColorAttachments = transfer_dest_is_depth ? nullptr : &color_attachment;
    rendering_info.pDepthAttachment = transfer_dest_is_depth ? &depth_attachment : nullptr;
    rendering_info.pStencilAttachment = transfer_dest_is_depth ? &stencil_attachment : nullptr;
    deferred_command_buffer_.CmdVkBeginRendering(&rendering_info);
  } else {
    VkRenderPassBeginInfo render_pass_begin_info;
    render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin_info.pNext = nullptr;
    render_pass_begin_info.renderPass = render_pass;
    render_pass_begin_info.framebuffer = framebuffer->framebuffer;
    render_pass_begin_info.renderArea.offset.x = 0;
    render_pass_begin_info.renderArea.offset.y = 0;
    render_pass_begin_info.renderArea.extent = framebuffer->host_extent;
    render_pass_begin_info.clearValueCount = 0;
    render_pass_begin_info.pClearValues = nullptr;
    deferred_command_buffer_.CmdVkBeginRenderPass(&render_pass_begin_info,
                                                  VK_SUBPASS_CONTENTS_INLINE);
  }
  in_render_pass_ = true;
}

bool VulkanCommandProcessor::CanHoistSharedMemoryUpload(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) const {
  if (!hoist_render_pass_ || !in_render_pass_) {
    return false;
  }
  for (const std::pair<uint32_t, uint32_t>& range : upload_page_ranges) {
    if (!range.second) {
      continue;
    }
    uint32_t page_first = range.first;
    uint32_t page_last = range.first + range.second - 1;
    if ((page_last >> 6) >= hoist_read_pages_.size()) {
      return false;
    }
    for (uint32_t word = page_first >> 6; word <= (page_last >> 6); ++word) {
      if (hoist_read_pages_serial_[word] != hoist_render_pass_serial_) {
        continue;
      }
      uint64_t mask = ~uint64_t(0);
      if (word == (page_first >> 6)) {
        mask &= ~uint64_t(0) << (page_first & 63);
      }
      if (word == (page_last >> 6)) {
        mask &= ~uint64_t(0) >> (63 - (page_last & 63));
      }
      if (hoist_read_pages_[word] & mask) {
        return false;
      }
    }
  }
  return true;
}

void VulkanCommandProcessor::MarkSharedMemoryReadInRenderPass(uint32_t start, uint32_t length) {
  if (!hoist_render_pass_ || !length || start >= SharedMemory::kBufferSize) {
    return;
  }
  length = std::min(length, SharedMemory::kBufferSize - start);
  uint32_t page_size_log2 = shared_memory_->page_size_log2();
  uint32_t page_first = start >> page_size_log2;
  uint32_t page_last = (start + length - 1) >> page_size_log2;
  for (uint32_t word = page_first >> 6; word <= (page_last >> 6); ++word) {
    uint64_t mask = ~uint64_t(0);
    if (word == (page_first >> 6)) {
      mask &= ~uint64_t(0) << (page_first & 63);
    }
    if (word == (page_last >> 6)) {
      mask &= ~uint64_t(0) >> (63 - (page_last & 63));
    }
    if (hoist_read_pages_serial_[word] != hoist_render_pass_serial_) {
      hoist_read_pages_serial_[word] = hoist_render_pass_serial_;
      hoist_read_pages_[word] = 0;
    }
    hoist_read_pages_[word] |= mask;
  }
}

void VulkanCommandProcessor::EndRenderPass() {
  assert_true(submission_open_);
  if (!in_render_pass_) {
    return;
  }
  if (current_render_pass_ == VK_NULL_HANDLE) {
    deferred_command_buffer_.CmdVkEndRendering();
  } else {
    deferred_command_buffer_.CmdVkEndRenderPass();
  }
  current_render_pass_ = VK_NULL_HANDLE;
  current_framebuffer_ = nullptr;
  in_render_pass_ = false;
  hoist_render_pass_ = false;
}

VkDescriptorSet VulkanCommandProcessor::AllocateSingleTransientDescriptor(
    SingleTransientDescriptorLayout transient_descriptor_layout) {
  assert_true(frame_open_);
  VkDescriptorSet descriptor_set;
  std::vector<VkDescriptorSet>& transient_descriptors_free =
      single_transient_descriptors_free_[size_t(transient_descriptor_layout)];
  if (!transient_descriptors_free.empty()) {
    descriptor_set = transient_descriptors_free.back();
    transient_descriptors_free.pop_back();
  } else {
    const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    [[maybe_unused]] const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    [[maybe_unused]] const VkDevice device = vulkan_device->device();
    bool is_storage_image =
        transient_descriptor_layout == SingleTransientDescriptorLayout::kStorageImageCompute;
    bool is_storage_buffer =
        transient_descriptor_layout == SingleTransientDescriptorLayout::kStorageBufferCompute ||
        transient_descriptor_layout == SingleTransientDescriptorLayout::kStorageBufferPairCompute;
    ui::vulkan::LinkedTypeDescriptorSetAllocator& transient_descriptor_allocator =
        is_storage_image    ? transient_descriptor_allocator_storage_image_
        : is_storage_buffer ? transient_descriptor_allocator_storage_buffer_
                            : transient_descriptor_allocator_uniform_buffer_;
    VkDescriptorPoolSize descriptor_count;
    descriptor_count.type = is_storage_image    ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                            : is_storage_buffer ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptor_count.descriptorCount =
        transient_descriptor_layout == SingleTransientDescriptorLayout::kStorageBufferPairCompute
            ? 2
            : 1;
    descriptor_set = transient_descriptor_allocator.Allocate(
        GetSingleTransientDescriptorLayout(transient_descriptor_layout), &descriptor_count, 1);
    if (descriptor_set == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }
  }
  UsedSingleTransientDescriptor used_descriptor;
  used_descriptor.frame = frame_current_;
  used_descriptor.layout = transient_descriptor_layout;
  used_descriptor.set = descriptor_set;
  single_transient_descriptors_used_.emplace_back(used_descriptor);
  return descriptor_set;
}

VkDescriptorSetLayout VulkanCommandProcessor::GetTextureDescriptorSetLayout(bool is_vertex,
                                                                            size_t texture_count,
                                                                            size_t sampler_count) {
  size_t binding_count = texture_count + sampler_count;
  if (!binding_count) {
    return descriptor_set_layout_empty_;
  }

  TextureDescriptorSetLayoutKey texture_descriptor_set_layout_key;
  texture_descriptor_set_layout_key.texture_count = uint32_t(texture_count);
  texture_descriptor_set_layout_key.sampler_count = uint32_t(sampler_count);
  texture_descriptor_set_layout_key.is_vertex = uint32_t(is_vertex);
  auto it_existing = descriptor_set_layouts_textures_.find(texture_descriptor_set_layout_key);
  if (it_existing != descriptor_set_layouts_textures_.end()) {
    return it_existing->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  descriptor_set_layout_bindings_.clear();
  descriptor_set_layout_bindings_.reserve(binding_count);
  VkShaderStageFlags stage_flags =
      is_vertex ? guest_shader_vertex_stages_ : VK_SHADER_STAGE_FRAGMENT_BIT;
  for (size_t i = 0; i < texture_count; ++i) {
    VkDescriptorSetLayoutBinding& descriptor_set_layout_binding =
        descriptor_set_layout_bindings_.emplace_back();
    descriptor_set_layout_binding.binding = uint32_t(i);
    descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_layout_binding.descriptorCount = 1;
    descriptor_set_layout_binding.stageFlags = stage_flags;
  }
  for (size_t i = 0; i < sampler_count; ++i) {
    VkDescriptorSetLayoutBinding& descriptor_set_layout_binding =
        descriptor_set_layout_bindings_.emplace_back();
    descriptor_set_layout_binding.binding = uint32_t(texture_count + i);
    descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_set_layout_binding.descriptorCount = 1;
    descriptor_set_layout_binding.stageFlags = stage_flags;
  }
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = uint32_t(binding_count);
  descriptor_set_layout_create_info.pBindings = descriptor_set_layout_bindings_.data();
  VkDescriptorSetLayout texture_descriptor_set_layout;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &texture_descriptor_set_layout) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  descriptor_set_layouts_textures_.emplace(texture_descriptor_set_layout_key,
                                           texture_descriptor_set_layout);
  return texture_descriptor_set_layout;
}

const VulkanPipelineCache::PipelineLayoutProvider* VulkanCommandProcessor::GetPipelineLayout(
    size_t texture_count_pixel, size_t sampler_count_pixel, size_t texture_count_vertex,
    size_t sampler_count_vertex) {
  PipelineLayoutKey pipeline_layout_key;
  pipeline_layout_key.texture_count_pixel = uint16_t(texture_count_pixel);
  pipeline_layout_key.sampler_count_pixel = uint16_t(sampler_count_pixel);
  pipeline_layout_key.texture_count_vertex = uint16_t(texture_count_vertex);
  pipeline_layout_key.sampler_count_vertex = uint16_t(sampler_count_vertex);
  {
    auto it = pipeline_layouts_.find(pipeline_layout_key);
    if (it != pipeline_layouts_.end()) {
      return &it->second;
    }
  }

  VkDescriptorSetLayout descriptor_set_layout_textures_vertex =
      GetTextureDescriptorSetLayout(true, texture_count_vertex, sampler_count_vertex);
  if (descriptor_set_layout_textures_vertex == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "Failed to obtain a Vulkan descriptor set layout for {} sampled images "
        "and {} samplers for guest vertex shaders",
        texture_count_vertex, sampler_count_vertex);
    return nullptr;
  }
  VkDescriptorSetLayout descriptor_set_layout_textures_pixel =
      GetTextureDescriptorSetLayout(false, texture_count_pixel, sampler_count_pixel);
  if (descriptor_set_layout_textures_pixel == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "Failed to obtain a Vulkan descriptor set layout for {} sampled images "
        "and {} samplers for guest pixel shaders",
        texture_count_pixel, sampler_count_pixel);
    return nullptr;
  }

  VkDescriptorSetLayout descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetCount];
  // Immutable layouts.
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
      descriptor_set_layout_shared_memory_and_edram_;
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetConstants] =
      descriptor_set_layout_constants_;
  // Mutable layouts.
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesVertex] =
      descriptor_set_layout_textures_vertex;
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
      descriptor_set_layout_textures_pixel;

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkPipelineLayoutCreateInfo pipeline_layout_create_info;
  pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_create_info.pNext = nullptr;
  pipeline_layout_create_info.flags = 0;
  pipeline_layout_create_info.setLayoutCount = uint32_t(rex::countof(descriptor_set_layouts));
  pipeline_layout_create_info.pSetLayouts = descriptor_set_layouts;
  pipeline_layout_create_info.pushConstantRangeCount = 0;
  pipeline_layout_create_info.pPushConstantRanges = nullptr;
  VkPipelineLayout pipeline_layout;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr, &pipeline_layout) !=
      VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create a Vulkan pipeline layout for guest drawing with {} "
        "pixel shader and {} vertex shader textures",
        texture_count_pixel, texture_count_vertex);
    return nullptr;
  }
  auto emplaced_pair = pipeline_layouts_.emplace(
      std::piecewise_construct, std::forward_as_tuple(pipeline_layout_key),
      std::forward_as_tuple(pipeline_layout, descriptor_set_layout_textures_vertex,
                            descriptor_set_layout_textures_pixel));
  // unordered_map insertion doesn't invalidate element references.
  return &emplaced_pair.first->second;
}

VulkanCommandProcessor::ScratchBufferAcquisition VulkanCommandProcessor::AcquireScratchGpuBuffer(
    VkDeviceSize size, VkPipelineStageFlags initial_stage_mask, VkAccessFlags initial_access_mask) {
  assert_true(submission_open_);
  assert_false(scratch_buffer_used_);
  if (!submission_open_ || scratch_buffer_used_ || !size) {
    return ScratchBufferAcquisition();
  }

  uint64_t submission_current = GetCurrentSubmission();

  if (scratch_buffer_ != VK_NULL_HANDLE && size <= scratch_buffer_size_) {
    // Already used previously - transition.
    PushBufferMemoryBarrier(scratch_buffer_, 0, VK_WHOLE_SIZE, scratch_buffer_last_stage_mask_,
                            initial_stage_mask, scratch_buffer_last_access_mask_,
                            initial_access_mask);
    scratch_buffer_last_stage_mask_ = initial_stage_mask;
    scratch_buffer_last_access_mask_ = initial_access_mask;
    scratch_buffer_last_usage_submission_ = submission_current;
    scratch_buffer_used_ = true;
    return ScratchBufferAcquisition(*this, scratch_buffer_, initial_stage_mask,
                                    initial_access_mask);
  }

  size = rex::align(size, kScratchBufferSizeIncrement);

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();

  VkDeviceMemory new_scratch_buffer_memory;
  VkBuffer new_scratch_buffer;
  // VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT for
  // texture loading.
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, size,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, new_scratch_buffer,
          new_scratch_buffer_memory)) {
    REXGPU_ERROR("VulkanCommandProcessor: Failed to create a {} MB scratch GPU buffer", size >> 20);
    return ScratchBufferAcquisition();
  }

  if (submission_completed_ >= scratch_buffer_last_usage_submission_) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    if (scratch_buffer_ != VK_NULL_HANDLE) {
      dfn.vkDestroyBuffer(device, scratch_buffer_, nullptr);
    }
    if (scratch_buffer_memory_ != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, scratch_buffer_memory_, nullptr);
    }
  } else {
    if (scratch_buffer_ != VK_NULL_HANDLE) {
      destroy_buffers_.emplace_back(scratch_buffer_last_usage_submission_, scratch_buffer_);
    }
    if (scratch_buffer_memory_ != VK_NULL_HANDLE) {
      destroy_memory_.emplace_back(scratch_buffer_last_usage_submission_, scratch_buffer_memory_);
    }
  }

  scratch_buffer_memory_ = new_scratch_buffer_memory;
  scratch_buffer_ = new_scratch_buffer;
  scratch_buffer_size_ = size;
  // Not used yet, no need for a barrier.
  scratch_buffer_last_stage_mask_ = initial_access_mask;
  scratch_buffer_last_access_mask_ = initial_stage_mask;
  scratch_buffer_last_usage_submission_ = submission_current;
  scratch_buffer_used_ = true;
  return ScratchBufferAcquisition(*this, new_scratch_buffer, initial_stage_mask,
                                  initial_access_mask);
}

void VulkanCommandProcessor::BindExternalGraphicsPipeline(VkPipeline pipeline,
                                                          bool keep_dynamic_depth_bias,
                                                          bool keep_dynamic_blend_constants,
                                                          bool keep_dynamic_stencil_mask_ref) {
  if (!keep_dynamic_depth_bias) {
    dynamic_depth_bias_update_needed_ = true;
  }
  if (!keep_dynamic_blend_constants) {
    dynamic_blend_constants_update_needed_ = true;
  }
  if (!keep_dynamic_stencil_mask_ref) {
    dynamic_stencil_compare_mask_front_update_needed_ = true;
    dynamic_stencil_compare_mask_back_update_needed_ = true;
    dynamic_stencil_write_mask_front_update_needed_ = true;
    dynamic_stencil_write_mask_back_update_needed_ = true;
    dynamic_stencil_reference_front_update_needed_ = true;
    dynamic_stencil_reference_back_update_needed_ = true;
  }
  if (current_external_graphics_pipeline_ == pipeline) {
    return;
  }
  deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  current_external_graphics_pipeline_ = pipeline;
  current_guest_graphics_pipeline_ = VK_NULL_HANDLE;
  current_guest_graphics_pipeline_layout_ = VK_NULL_HANDLE;
}

void VulkanCommandProcessor::BindExternalComputePipeline(VkPipeline pipeline) {
  if (current_external_compute_pipeline_ == pipeline) {
    return;
  }
  deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  current_external_compute_pipeline_ = pipeline;
}

void VulkanCommandProcessor::SetViewport(const VkViewport& viewport) {
  if (!dynamic_viewport_update_needed_) {
    dynamic_viewport_update_needed_ |= dynamic_viewport_.x != viewport.x;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.y != viewport.y;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.width != viewport.width;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.height != viewport.height;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.minDepth != viewport.minDepth;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.maxDepth != viewport.maxDepth;
  }
  if (dynamic_viewport_update_needed_) {
    dynamic_viewport_ = viewport;
    deferred_command_buffer_.CmdVkSetViewport(0, 1, &dynamic_viewport_);
    dynamic_viewport_update_needed_ = false;
  }
}

void VulkanCommandProcessor::SetScissor(const VkRect2D& scissor) {
  if (!dynamic_scissor_update_needed_) {
    dynamic_scissor_update_needed_ |= dynamic_scissor_.offset.x != scissor.offset.x;
    dynamic_scissor_update_needed_ |= dynamic_scissor_.offset.y != scissor.offset.y;
    dynamic_scissor_update_needed_ |= dynamic_scissor_.extent.width != scissor.extent.width;
    dynamic_scissor_update_needed_ |= dynamic_scissor_.extent.height != scissor.extent.height;
  }
  if (dynamic_scissor_update_needed_) {
    dynamic_scissor_ = scissor;
    deferred_command_buffer_.CmdVkSetScissor(0, 1, &dynamic_scissor_);
    dynamic_scissor_update_needed_ = false;
  }
}

void VulkanCommandProcessor::OnPrimaryBufferEnd() {
  if (REXCVAR_GET(vulkan_submit_on_primary_buffer_end) && submission_open_ &&
      CanEndSubmissionImmediately()) {
    EndSubmission(false);
  }
}

Shader* VulkanCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

bool VulkanCommandProcessor::Fh1CreateRenderCaches(uint32_t draw_resolution_scale_x,
                                                       uint32_t draw_resolution_scale_y) {
  const auto& props = GetVulkanDevice()->properties();
  const uint32_t shared_memory_binding_count = UINT32_C(1) <<
      SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(props.maxStorageBufferRange);
  if (!render_target_cache_) {
    render_target_cache_ = std::make_unique<VulkanRenderTargetCache>(
        *register_file_, *memory_, draw_resolution_scale_x, draw_resolution_scale_y, *this);
    if (!render_target_cache_->Initialize(shared_memory_binding_count)) return false;
  }
  // Requires the transient descriptor set layouts.
  texture_cache_ =
      VulkanTextureCache::Create(*register_file_, *shared_memory_, draw_resolution_scale_x,
                                 draw_resolution_scale_y, *this, guest_shader_pipeline_stages_);
  if (!texture_cache_) {
    REXGPU_ERROR("Failed to initialize the texture cache");
    return false;
  }

  if (REXCVAR_GET(fh1_scene_720p_native_hud) || REXCVAR_GET(fh1_upscaler) != "off") {
    const bool fh1_fsr3 = REXCVAR_GET(fh1_upscaler).rfind("fsr3_", 0) == 0;
    const bool fh1_metalfx = REXCVAR_GET(fh1_upscaler).rfind("metalfx", 0) == 0;
    const std::string fh1_backend = fh1_metalfx ? "Apple MetalFX" : (fh1_fsr3 ? "AMD FSR 3" : "NVIDIA DLSS");
    const uint32_t output_width = REXCVAR_GET(fh1_output_width);
    const uint32_t output_height = REXCVAR_GET(fh1_output_height);
    const uint32_t output_scale = (output_height + 719) / 720;
    const bool supported_output =
#if REX_PLATFORM_MAC
        (output_width == 1280 && output_height == 720 && REXCVAR_GET(fh1_upscaler) == "off") ||
#endif
        (output_width == 1920 && output_height == 1080) ||
        (output_width == 2560 && output_height == 1440) || (output_width == 3840 && output_height == 2160);
    const bool compatible = supported_output && (!fh1_metalfx || output_height % 720 == 0) &&
        draw_resolution_scale_x == output_scale &&
        draw_resolution_scale_y == output_scale &&
        render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets;
    if (!compatible) {
      Fh1Dlss::ReportStatus("error: " + fh1_backend + " output/scale or render-target path is incompatible; continuing with native rendering.");
      REXGPU_WARN("FH1 reconstruction disabled: incompatible output, scale or render-target path; using native rendering");
    } else {
      bool ready = true;
      uint32_t scene_width = 1280 * REXCVAR_GET(fh1_native_scene_scale),
          scene_height = 720 * REXCVAR_GET(fh1_native_scene_scale);
      if (REXCVAR_GET(fh1_upscaler) != "off") {
        fh1_dlss_ = std::make_unique<Fh1Dlss>(GetVulkanDevice(), output_width, output_height,
            REXCVAR_GET(fh1_upscaler));
        ready = fh1_dlss_->Initialize();
        if (ready) { scene_width = fh1_dlss_->width(); scene_height = fh1_dlss_->height(); }
      }
      // Guest EDRAM resolves require an integer scene scale. Use a ceiling
      // raster and resample into NGX's exact recommended input dimensions.
      // Do not claim that fractional modes shade only the NGX input pixels.
      const uint32_t scene_scale = std::max((scene_width + 1279) / 1280, (scene_height + 719) / 720);
      if (ready) {
        fh1_split_enabled_ = true;
        render_target_cache_->SetRasterSize(output_width, output_height);
        fh1_other_render_targets_ = std::make_unique<VulkanRenderTargetCache>(
            *register_file_, *memory_, scene_scale, scene_scale, *this);
        fh1_other_render_targets_->SetRasterSize(1280 * scene_scale, 720 * scene_scale);
        ready = fh1_other_render_targets_->Initialize(shared_memory_binding_count);
        if (fh1_dlss_) fh1_dlss_->SetSceneBacking(scene_scale);
      }
      if (ready && scene_scale != output_scale) {
        fh1_other_textures_ = VulkanTextureCache::Create(*register_file_, *shared_memory_,
            scene_scale, scene_scale, *this, guest_shader_pipeline_stages_);
        ready = bool(fh1_other_textures_);
      }
      if (ready) {
        fh1_other_pipelines_ = std::make_unique<VulkanPipelineCache>(
            *this, *register_file_, *fh1_other_render_targets_, guest_shader_vertex_stages_);
        ready = fh1_other_pipelines_->Initialize();
      }
      if (!ready) {
        Fh1Dlss::ReportStatus("error: " + fh1_backend + " initialization failed; continuing with native rendering. See the game log.");
        REXGPU_WARN("{} unavailable: retaining native rendering", fh1_backend);
        fh1_dlss_.reset();
        fh1_other_pipelines_.reset();
        fh1_other_render_targets_.reset();
        fh1_other_textures_.reset();
        fh1_split_enabled_ = false;
        render_target_cache_->SetRasterSize(1280 * draw_resolution_scale_x, 720 * draw_resolution_scale_y);
      } else {
        REXGPU_INFO("FH1 split rendering initialized: scene={}x{}, UI={}x{}; independent pipeline caches/translators", 1280 * scene_scale, 720 * scene_scale, output_width, output_height);
        REXGPU_INFO("FH1 reconstruction input={}x{}; raster={}x{}", scene_width, scene_height, 1280 * scene_scale, 720 * scene_scale);
      }
    }
  }

  pipeline_cache_ = std::make_unique<VulkanPipelineCache>(
      *this, *register_file_, *render_target_cache_, guest_shader_vertex_stages_);
  if (!pipeline_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the graphics pipeline cache");
    return false;
  }

  return true;
}

void VulkanCommandProcessor::Fh1ApplyGraphicsRequest() {
  const std::string request = rex::cvar::GetFlagByName("fh1_graphics_request");
  if (request.empty() || request == fh1_last_graphics_request_) return;
  fh1_last_graphics_request_ = request;
  auto result = [&](const std::string& text) {
    rex::cvar::SetFlagByName("fh1_graphics_result", request + "|" + text);
  };
  std::istringstream input(request);
  uint64_t id;
  uint32_t width, height, scene_scale;
  std::string mode, extra;
  if (!(input >> id >> width >> height >> scene_scale >> mode) || (input >> extra) ||
      !((width == 1920 && height == 1080) || (width == 2560 && height == 1440) ||
#if REX_PLATFORM_MAC
        (width == 1280 && height == 720 && mode == "off") ||
#endif
        (width == 3840 && height == 2160)) || scene_scale < 1 || scene_scale > 3 ||
      !(mode == "off" || mode == "quality" || mode == "balanced" || mode == "dlss" ||
        mode == "smaa" || mode == "dlaa" || mode == "fsr3_quality" || mode == "fsr3_balanced" ||
        mode == "fsr3_performance" || mode == "fsr3_native"
#if REX_PLATFORM_MAC
        || mode == "metalfx_temporal" || mode == "metalfx_spatial"
#endif
        )) {
    result("error: unsupported graphics settings");
    return;
  }
  if ((mode == "metalfx_temporal" || mode == "metalfx_spatial") && height % 720 != 0) {
    result("error: MetalFX currently requires 1440p or 4K output");
    return;
  }
  if (width == REXCVAR_GET(fh1_output_width) && height == REXCVAR_GET(fh1_output_height) &&
      mode == REXCVAR_GET(fh1_upscaler) && fh1_split_enabled_ &&
      (mode != "off" || scene_scale == REXCVAR_GET(fh1_native_scene_scale))) {
    result("ok");
    return;
  }
  if (render_target_cache_->GetPath() != RenderTargetCache::Path::kHostRenderTargets ||
      REXCVAR_GET(forza_native_width) > 0) {
    result("error: live graphics currently requires standard 16:9 host rendering");
    return;
  }
  if (!AwaitAllQueueOperationsCompletion()) {
    result("error: could not finish pending GPU work");
    return;
  }
  // At a closed frame, all submissions are complete and no new draws can refer
  // to these caches. Keep the old caches until the replacements initialize.
  Fh1SelectNativeHud(true);
  auto old_rt = std::move(render_target_cache_);
  auto old_textures = std::move(texture_cache_);
  auto old_pipelines = std::move(pipeline_cache_);
  auto old_scene_rt = std::move(fh1_other_render_targets_);
  auto old_scene_textures = std::move(fh1_other_textures_);
  auto old_scene_pipelines = std::move(fh1_other_pipelines_);
  const bool old_split = fh1_split_enabled_;
  const bool had_dlss = bool(fh1_dlss_);
  const uint32_t old_backing = had_dlss ?
      std::max((fh1_dlss_->width() + 1279) / 1280, (fh1_dlss_->height() + 719) / 720) : 1;
  const char* names[] = {"fh1_upscaler", "fh1_output_width", "fh1_output_height",
      "fh1_scene_720p_native_hud", "fh1_native_scene_scale", "resolution_scale",
      "draw_resolution_scale_x", "draw_resolution_scale_y", "swap_post_effect"};
  std::vector<std::string> previous;
  for (auto name : names) previous.push_back(rex::cvar::GetFlagByName(name));
  // NGX initialization/shutdown is per device. Release the old feature before
  // initializing a new mode; do not shut down NGX underneath the new feature.
  fh1_dlss_.reset();
  fh1_split_enabled_ = false;
  const uint32_t output_scale = (height + 719) / 720;
  rex::cvar::SetFlagByName("fh1_upscaler", mode);
  if (mode != "off") rex::cvar::SetFlagByName("swap_post_effect", "none");
  rex::cvar::SetFlagByName("fh1_output_width", std::to_string(width));
  rex::cvar::SetFlagByName("fh1_output_height", std::to_string(height));
  rex::cvar::SetFlagByName("fh1_scene_720p_native_hud", "true");
  rex::cvar::SetFlagByName("fh1_native_scene_scale", std::to_string(scene_scale));
  rex::cvar::SetFlagByName("resolution_scale", std::to_string(output_scale));
  rex::cvar::SetFlagByName("draw_resolution_scale_x", "1");
  rex::cvar::SetFlagByName("draw_resolution_scale_y", "1");
  const bool ready = Fh1CreateRenderCaches(output_scale, output_scale) &&
      fh1_split_enabled_ && (mode == "off" || fh1_dlss_);
  if (!ready) {
    fh1_dlss_.reset();
    fh1_other_pipelines_.reset();
    pipeline_cache_.reset();
    fh1_other_textures_.reset();
    texture_cache_.reset();
    fh1_other_render_targets_.reset();
    render_target_cache_.reset();
    for (size_t i = 0; i < previous.size(); ++i)
      rex::cvar::SetFlagByName(names[i], previous[i]);
    render_target_cache_ = std::move(old_rt);
    texture_cache_ = std::move(old_textures);
    pipeline_cache_ = std::move(old_pipelines);
    fh1_other_render_targets_ = std::move(old_scene_rt);
    fh1_other_textures_ = std::move(old_scene_textures);
    fh1_other_pipelines_ = std::move(old_scene_pipelines);
    fh1_split_enabled_ = old_split;
    if (had_dlss) {
      fh1_dlss_ = std::make_unique<Fh1Dlss>(GetVulkanDevice(), std::stoul(previous[1]),
          std::stoul(previous[2]), previous[0]);
      if (!fh1_dlss_->Initialize()) {
        fh1_dlss_.reset();
        result("error: reconstruction and recovery failed; return to native rendering");
        return;
      }
      fh1_dlss_->SetSceneBacking(old_backing);
    }
    result("error: initialization failed; previous settings restored");
    return;
  }
  // Active shader pointers belong to the old cache even if the guest doesn't
  // rebind them next frame. Clone their microcode before releasing that cache.
  if (active_vertex_shader_)
    active_vertex_shader_ = pipeline_cache_->LoadShaderForDraw(*active_vertex_shader_);
  if (active_pixel_shader_)
    active_pixel_shader_ = pipeline_cache_->LoadShaderForDraw(*active_pixel_shader_);
  old_scene_pipelines.reset();
  old_pipelines.reset();
  old_scene_textures.reset();
  old_textures.reset();
  old_scene_rt.reset();
  old_rt.reset();
  const auto& f = GetVulkanDevice()->functions();
  for (auto& framebuffer : swap_framebuffers_)
    ui::vulkan::util::DestroyAndNullHandle(f.vkDestroyFramebuffer,
        GetVulkanDevice()->device(), framebuffer.framebuffer);
  ClearTransientDescriptorPools();
  current_constant_buffers_up_to_date_ = 0;
  current_graphics_descriptor_set_values_up_to_date_ = 0;
  Fh1InvalidateBindings();
  if (!fh1_storage_root_.empty())
    InitializeShaderStorage(fh1_storage_root_, fh1_storage_title_, false);
  REXGPU_INFO("FH1 live graphics applied: {}x{}, mode={}, native scene scale={}",
              width, height, mode, scene_scale);
  result("ok");
}

void VulkanCommandProcessor::Fh1SelectNativeHud(bool native_hud) {
  if (!fh1_other_render_targets_ || native_hud == fh1_native_hud_active_) return;
  EndRenderPass();
  if (fh1_other_textures_) std::swap(texture_cache_, fh1_other_textures_);
  std::swap(render_target_cache_, fh1_other_render_targets_);
  std::swap(pipeline_cache_, fh1_other_pipelines_);
  Fh1InvalidateBindings();
  fh1_native_hud_active_ = native_hud;
  dynamic_viewport_update_needed_ = true;
  dynamic_scissor_update_needed_ = true;
  dynamic_depth_bias_update_needed_ = true;
  current_constant_buffers_up_to_date_ = 0;
  current_graphics_descriptor_set_values_up_to_date_ =
      UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram;
  current_graphics_descriptor_sets_bound_up_to_date_ = 0;
}

bool VulkanCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  ++fh1_frame_cost::Current().draws;
  // FH1 (#268): on integrated GPUs, at most so many draws in one submission. Not inside an
  // occlusion query, which a submission's end would cut short.
  if (submission_open_ && frame_open_ && !active_occlusion_query_.valid &&
      fh1_submission_watch::ShouldSplit(
          fh1_draws_in_submission,
          fh1_submission_watch::DrawLimit(REXCVAR_GET(vulkan_submission_draw_limit),
                                          fh1_device_memory::Get().integrated()))) {
    if (EndSubmission(false)) ++fh1_submission_splits;
  }
  ++fh1_draws_in_submission;
  const RegisterFile& regs = *register_file_;
  (void)index_buffer_info;
  auto draw_fail = [&](const char* stage) {
    auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
    REXGPU_ERROR(
        "Vulkan IssueDraw failed at {} "
        "(prim_type={}, index_count={}, source_select={}, major_mode={}, explicit_major={}, "
        "path_select={}, tess_mode={}, edram_mode={})",
        stage, uint32_t(prim_type), index_count, uint32_t(vgt_draw_initiator.source_select),
        uint32_t(vgt_draw_initiator.major_mode), uint32_t(major_mode_explicit),
        uint32_t(regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select),
        uint32_t(regs.Get<reg::VGT_HOS_CNTL>().tess_mode),
        uint32_t(regs.Get<reg::RB_MODECONTROL>().edram_mode));
    return false;
  };

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    // Special copy handling.
    return IssueCopy();
  }

  bool surface_pitch_is_zero = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0;

  const ui::vulkan::VulkanDevice::Properties& device_properties = GetVulkanDevice()->properties();

  memexport_ranges_.clear();

  // Vertex shader analysis.
  auto vertex_shader = static_cast<VulkanShader*>(active_vertex_shader());
  if (!vertex_shader) {
    // Always need a vertex shader.
    return draw_fail("missing_vertex_shader");
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;
  if (memexport_used_vertex) {
    if (!device_properties.vertexPipelineStoresAndAtomics) {
      REXGPU_ERROR(
          "Vertex shader memexport draw encountered without "
          "vertexPipelineStoresAndAtomics support");
      return false;
    }
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (surface_pitch_is_zero && is_rasterization_done) {
    // Doesn't actually draw.
    // Unlikely that zero would even really be legal though.
    return true;
  }
  VulkanShader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    // See xenos::EdramMode for explanation why the pixel shader is only used
    // when it's kColorDepth here.
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<VulkanShader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // Disabling pixel shader for this case is also required by the pipeline
    // cache.
    if (!memexport_used_vertex) {
      // This draw has no effect.
      return true;
    }
  }
  bool memexport_used_pixel = pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  if (memexport_used_pixel) {
    if (!device_properties.fragmentStoresAndAtomics) {
      REXGPU_ERROR(
          "Pixel shader memexport draw encountered without "
          "fragmentStoresAndAtomics support");
      return false;
    }
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;

  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  SpirvShaderTranslator::Modification vertex_shader_modification;
  SpirvShaderTranslator::Modification pixel_shader_modification;
  VulkanShader::VulkanTranslation* vertex_shader_translation;
  VulkanShader::VulkanTranslation* pixel_shader_translation;
  bool memexport_writes_possible = memexport_used_vertex || memexport_used_pixel;

  // Two iterations because a submission (even the current one - in which case
  // it needs to be ended, and a new one must be started) may need to be awaited
  // in case of a sampler count overflow, and if that happens, all subsystem
  // updates done previously must be performed again because the updates done
  // before the awaiting may be referencing objects destroyed by
  // CompletedSubmissionUpdated.
  for (uint32_t i = 0; i < 2; ++i) {
    if (!BeginSubmission(true)) {
      return draw_fail("begin_submission");
    }

    if (fh1_other_render_targets_ && is_rasterization_done && pixel_shader) {
      const auto color = regs.Get<reg::RB_COLOR_INFO>();
      const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
      static const bool aa_diagnostics = std::getenv("FORZA_AA_DIAGNOSTICS") != nullptr;
      if (aa_diagnostics) {
        static uint32_t seen = 0;
        const auto samples = uint32_t(surface.msaa_samples);
        if (samples < 3 && !(seen & (1u << samples))) {
          seen |= 1u << samples;
          REXGPU_INFO("FH1 AA: guest requests {}x MSAA, pitch={}, color format={}",
              1u << samples, surface.surface_pitch, uint32_t(color.color_format));
        }
      }
      // The same packed formats also occur in small intermediate targets in
      // the title sequence. Only the full front buffer marks the scene/UI split.
      const bool front_buffer = surface.surface_pitch == 1280 &&
          surface.msaa_samples == xenos::MsaaSamples::k1X;
      if (front_buffer &&
          color.color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10 &&
          !fh1_native_hud_active_) {
        fh1_scene_composited_ = true;
        fh1_scene_base_ = color.color_base;
      } else if (front_buffer && color.color_format ==
                 xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10 &&
                 !fh1_native_hud_active_ &&
                 (!fh1_scene_composited_ || color.color_base == fh1_scene_base_)) {
        fh1_seed_pending_ = fh1_scene_composited_;
        Fh1SelectNativeHud(true);
      }
    }

    // Shader translations and render passes belong to the active render scale.
    // Never share a 2x VPOS/memexport translation with the 1x scene.
    if (fh1_other_pipelines_) {
      vertex_shader = pipeline_cache_->LoadShaderForDraw(*vertex_shader);
      pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
      if (pixel_shader) {
        pixel_shader = pipeline_cache_->LoadShaderForDraw(*pixel_shader);
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
      }
    }

    // Process primitives.
    if (!primitive_processor_->Process(primitive_processing_result)) {
      return draw_fail("primitive_processing");
    }
    if (!primitive_processing_result.host_draw_vertex_count) {
      // Nothing to draw.
      return true;
    }
    if (primitive_processing_result.host_primitive_type == xenos::PrimitiveType::kTriangleFan) {
      // Vulkan uses the same fan-to-list conversion policy as D3D12.
      REXGPU_ERROR(
          "PrimitiveProcessor returned triangle fan for Vulkan draw; expected "
          "triangle list conversion for D3D12 parity");
      assert_always();
      return false;
    }
    // Tessellation and rectangle expansion variants for rasterization are
    // produced by the primitive processor and are handled by the Vulkan
    // pipeline cache.
    Shader::HostVertexShaderType host_vertex_shader_type =
        primitive_processing_result.host_vertex_shader_type;
    if (host_vertex_shader_type != Shader::HostVertexShaderType::kVertex &&
        host_vertex_shader_type != Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        host_vertex_shader_type != Shader::HostVertexShaderType::kRectangleListAsTriangleStrip &&
        !Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
      REXGPU_ERROR("Unsupported Vulkan host vertex shader type {}",
                   uint32_t(host_vertex_shader_type));
      return false;
    }

    // Shader modifications.
    vertex_shader_modification = pipeline_cache_->GetCurrentVertexShaderModification(
        *vertex_shader, primitive_processing_result.host_vertex_shader_type, interpolator_mask,
        ps_param_gen_pos != UINT32_MAX);
    pixel_shader_modification = pixel_shader ? pipeline_cache_->GetCurrentPixelShaderModification(
                                                   *pixel_shader, interpolator_mask,
                                                   ps_param_gen_pos, normalized_depth_control)
                                             : SpirvShaderTranslator::Modification(0);

    // Translate the shaders now to obtain the sampler bindings.
    vertex_shader_translation = static_cast<VulkanShader::VulkanTranslation*>(
        vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value));
    pixel_shader_translation =
        pixel_shader ? static_cast<VulkanShader::VulkanTranslation*>(
                           pixel_shader->GetOrCreateTranslation(pixel_shader_modification.value))
                     : nullptr;
    if (!pipeline_cache_->EnsureShadersTranslated(vertex_shader_translation,
                                                  pixel_shader_translation)) {
      return draw_fail("shader_translation");
    }

    // Obtain the samplers. Note that the bindings don't depend on the shader
    // modification, so if on the second iteration of this loop it becomes
    // different for some reason (like a race condition with the guest in index
    // buffer processing in the primitive processor resulting in different host
    // vertex shader types), the bindings will stay the same.
    // TODO(Triang3l): Sampler caching and reuse for adjacent draws within one
    // submission.
    uint32_t samplers_overflowed_count = 0;
    for (uint32_t j = 0; j < 2; ++j) {
      std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>& shader_samplers =
          j ? current_samplers_pixel_ : current_samplers_vertex_;
      if (!i) {
        shader_samplers.clear();
      }
      const VulkanShader* shader = j ? pixel_shader : vertex_shader;
      if (!shader) {
        continue;
      }
      const std::vector<VulkanShader::SamplerBinding>& shader_sampler_bindings =
          shader->GetSamplerBindingsAfterTranslation();
      if (!i) {
        shader_samplers.reserve(shader_sampler_bindings.size());
        for (const VulkanShader::SamplerBinding& shader_sampler_binding : shader_sampler_bindings) {
          shader_samplers.emplace_back(texture_cache_->GetSamplerParameters(shader_sampler_binding),
                                       VK_NULL_HANDLE);
        }
      }
      for (std::pair<VulkanTextureCache::SamplerParameters, VkSampler>& shader_sampler_pair :
           shader_samplers) {
        // UseSampler calls are needed even on the second iteration in case the
        // submission was broken (and thus the last usage submission indices for
        // the used samplers need to be updated) due to an overflow within one
        // submission. Though sampler overflow is a very rare situation overall.
        bool sampler_overflowed;
        VkSampler shader_sampler =
            texture_cache_->UseSampler(shader_sampler_pair.first, sampler_overflowed);
        shader_sampler_pair.second = shader_sampler;
        if (shader_sampler == VK_NULL_HANDLE) {
          if (!sampler_overflowed || i) {
            // If !sampler_overflowed, just failed to create a sampler for some
            // reason.
            // If i == 1, an overflow has happened twice, can't recover from it
            // anymore (would enter an infinite loop otherwise if the number of
            // attempts was not limited to 2). Possibly too many unique samplers
            // in one draw, or failed to await submission completion.
            return draw_fail("sampler_acquisition");
          }
          ++samplers_overflowed_count;
        }
      }
    }
    if (!samplers_overflowed_count) {
      break;
    }
    assert_zero(i);
    // Free space for as many samplers as how many haven't been allocated
    // successfully - obtain the submission index that needs to be awaited to
    // reuse `samplers_overflowed_count` slots. This must be done after all the
    // UseSampler calls, not inside the loop calling UseSampler, because earlier
    // UseSampler calls may "mark for deletion" some samplers that later
    // UseSampler calls in the loop may actually demand.
    uint64_t sampler_overflow_await_submission =
        texture_cache_->GetSubmissionToAwaitOnSamplerOverflow(samplers_overflowed_count);
    assert_true(sampler_overflow_await_submission <= GetCurrentSubmission());
    CheckSubmissionFenceAndDeviceLoss(sampler_overflow_await_submission);
  }

  uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;

  // Update the textures before most other work in the submission because
  // samplers depend on this (and in case of sampler overflow in a submission,
  // submissions must be split) - may perform dispatches and copying.
  uint32_t used_texture_mask =
      vertex_shader->GetUsedTextureMaskAfterTranslation() |
      (pixel_shader != nullptr ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
  texture_cache_->RequestTextures(used_texture_mask);


  if (fh1_dlss_ && pixel_shader && pixel_shader->ucode_data_hash() == 0xE68FE55ECD482EC5ull) {
    std::array<float, 136 * 4> constants;
    std::memcpy(constants.data(), &regs[XE_GPU_REG_SHADER_CONSTANT_256_X], sizeof(constants));
    fh1_dlss_->Observe(texture_cache_->GetActiveBindingOrNullImageView(
        2, xenos::FetchOpDimension::k2D, false), constants.data());
    static bool dumped = false;
    static const char* folder = std::getenv("FORZA_DLSS_DIAGNOSTICS");
    if (!dumped && folder && *folder) {
      {
        std::ofstream(std::filesystem::u8path(folder) / "velocity-shader.txt") << pixel_shader->ucode_disassembly();
        std::ofstream(std::filesystem::u8path(folder) / "velocity-vertex-shader.txt") << vertex_shader->ucode_disassembly();
        std::ofstream constants(std::filesystem::u8path(folder) / "velocity-constants.txt");
        constants << "texture-mask=" << used_texture_mask << '\n';
        for (unsigned i = 0; i < 256; ++i) {
          constants << i;
          for (unsigned c = 0; c < 4; ++c)
            constants << ',' << regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + 4 * i + c);
          constants << '\n';
        }
        dumped = true;
      }
    }
  }

  // FH1 (#380): may record the world AO compute passes (outside the render pass).
  // Patch 0394: the main view's depth for VR stereo (the AO's hook point).
  Fh1VrDepthBeforeDraw(pixel_shader, used_texture_mask);
  // Patch 0396: the main view's handedness, from the sun's shadow passes, which rebuild a pixel's
  // view x as z * ndc.x / c0.x (fh1_world_ao.h): a negative c0.x means the scene is drawn
  // mirrored across (post-processing turns it back), so the eyes' offsets and parts flip.
  if (pixel_shader && fh1_world_ao::IsShadowPass(pixel_shader->ucode_data_hash()) &&
      Fh1StereoWanted()) {
    const float c0x = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X);
    const bool mirrored = c0x < 0.0f;
    if (mirrored != fh1_stereo_mirrored_ || !fh1_stereo_handedness_known_)
      REXGPU_INFO("VR stereo: the main view is drawn {} (shadow pass c0.x {:.4f})",
                  mirrored ? "mirrored across" : "as shown", c0x);
    fh1_stereo_mirrored_ = mirrored;
    fh1_stereo_handedness_known_ = true;
  }
  if (!Fh1WorldAoBeforeDraw(pixel_shader, used_texture_mask)) {
    return draw_fail("world_ao_submission");
  }

  const VulkanPipelineCache::PipelineLayoutProvider* pipeline_layout_provider;
  // Set up the render targets - this may perform dispatches and draws.
  if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,
                                    normalized_color_mask, *vertex_shader)) {
    return draw_fail("render_target_update");
  }
  // A depth-only draw can precede the first HUD color export. Keep the scene
  // transfer pending until there is a native color attachment to seed.
  if (fh1_seed_pending_ && render_target_cache_->HasNativeHudColorTarget()) {
    fh1_seed_pending_ = false;
    if (!render_target_cache_->SeedNativeHud(*fh1_other_render_targets_, 1280, 720))
      return draw_fail("native_hud_seed");
    if (frame_current_ % 120 == 0)
      REXGPU_INFO("FH1 frame {}: world={}x{}, HUD={}x{}, scene transferred before UI", frame_current_,
          1280 * fh1_other_render_targets_->draw_resolution_scale_x(), 720 * fh1_other_render_targets_->draw_resolution_scale_y(),
          REXCVAR_GET(fh1_output_width), REXCVAR_GET(fh1_output_height));
  }

  bool hdr_menu_background = false;
  // Patch 0393: the pre-HUD snapshot also feeds a VR client's layers.
  if (is_rasterization_done && normalized_color_mask &&
      (ui::vulkan::GetHdrOutputState().active || Fh1VrLayersWanted())) {
    const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
    const auto color = regs.Get<reg::RB_COLOR_INFO>();
    const bool ui_target = surface.surface_pitch == 1280 &&
        surface.msaa_samples == xenos::MsaaSamples::k1X &&
        color.color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10;
    const uint64_t shader = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
    // FH1 enters the UI target before drawing the title/menu movie. Its first
    // untextured backdrop draw is not the scene/HUD boundary: capturing there
    // makes the entire movie look like UI to the difference mask (#214).
    const bool menu_backdrop = ui_target && shader == 0xA39E23E2FD1F8A8Bull;
    if (ui_target && shader == 0xCAE1DB68AFFA9D3Cull) {
      const auto texture = regs.GetTextureFetch(0);
      hdr_menu_background = texture.dimension == xenos::DataDimension::k2DOrStacked &&
          texture.size_2d.width == 1279 && texture.size_2d.height == 719 &&
          texture.format == xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16;
    }
    if (!menu_backdrop && !hdr_menu_background)
      CaptureHdrScene();
  }

  // Create the pipeline (for this, need the render pass from the render target
  // cache), translating the shaders - doing this now to obtain the used
  // textures.
  VkPipeline pipeline;
  void* pipeline_handle = nullptr;
  if (!pipeline_cache_->ConfigurePipeline(vertex_shader_translation, pixel_shader_translation,
                                          primitive_processing_result, normalized_depth_control,
                                          normalized_color_mask,
                                          render_target_cache_->last_update_render_pass_key(),
                                          pipeline, pipeline_layout_provider, &pipeline_handle)) {
    return draw_fail("configure_pipeline");
  }
  bool pipeline_is_placeholder = false;
  // Reload the current handle state to observe async hot-swap completions that
  // may happen between pipeline configuration and binding.
  pipeline_cache_->GetPipelineAndLayoutByHandle(pipeline_handle, pipeline, pipeline_layout_provider,
                                                &pipeline_is_placeholder);
  if (pipeline_is_placeholder && REXCVAR_GET(async_shader_compilation) &&
      REXCVAR_GET(sync_pipeline_creation)) {
    // A render that is made once (a car's picture) cannot lose this draw: wait for the
    // creation thread to swap the real pipeline in (a stuck creation falls back to the skip).
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (pipeline_is_placeholder && std::chrono::steady_clock::now() < wait_deadline) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      pipeline_cache_->GetPipelineAndLayoutByHandle(pipeline_handle, pipeline,
                                                    pipeline_layout_provider,
                                                    &pipeline_is_placeholder);
    }
  }
  if (REXCVAR_GET(async_shader_compilation) && pipeline_is_placeholder) {
    frame_used_async_placeholder_pipeline_ = true;
    ++fh1_pipeline_lookahead::stats.skipped_draws;  // FH1 (#378)
    if (Fh1SkipMustHoldFrame(pixel_shader != nullptr, normalized_color_mask,
                             primitive_processing_result.host_draw_vertex_count)) {
      fh1_frame_skipped_critical_ = true;
    }
    return true;
  }
  if (pipeline == VK_NULL_HANDLE || pipeline_layout_provider == nullptr) {
    return draw_fail("pipeline_lookup");
  }
  if (current_guest_graphics_pipeline_ != pipeline) {
    deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    current_guest_graphics_pipeline_ = pipeline;
    current_external_graphics_pipeline_ = VK_NULL_HANDLE;
  }

  // Update the graphics pipeline, and if the new graphics pipeline has a
  // different layout, invalidate incompatible descriptor sets before updating
  // current_guest_graphics_pipeline_layout_.
  auto pipeline_layout = static_cast<const PipelineLayout*>(pipeline_layout_provider);
  if (current_guest_graphics_pipeline_layout_ != pipeline_layout) {
    if (current_guest_graphics_pipeline_layout_) {
      // Keep descriptor set layouts for which the new pipeline layout is
      // compatible with the previous one (pipeline layouts are compatible for
      // set N if set layouts 0 through N are compatible).
      uint32_t descriptor_sets_kept = uint32_t(SpirvShaderTranslator::kDescriptorSetCount);
      if (current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_vertex_ref() !=
          pipeline_layout->descriptor_set_layout_textures_vertex_ref()) {
        descriptor_sets_kept = std::min(
            descriptor_sets_kept, uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesVertex));
      }
      if (current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_pixel_ref() !=
          pipeline_layout->descriptor_set_layout_textures_pixel_ref()) {
        descriptor_sets_kept = std::min(
            descriptor_sets_kept, uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesPixel));
      }
    } else {
      // No or unknown pipeline layout previously bound - all bindings are in an
      // indeterminate state.
      current_graphics_descriptor_sets_bound_up_to_date_ = 0;
    }
    current_guest_graphics_pipeline_layout_ = pipeline_layout;
  }

  bool host_render_targets_used =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets;
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  // Get dynamic rasterizer state.
  draw_util::ViewportInfo viewport_info;
  // Just handling maxViewportDimensions is enough - viewportBoundsRange[1] must
  // be at least 2 * max(maxViewportDimensions[0...1]) - 1, and
  // maxViewportDimensions must be greater than or equal to the size of the
  // largest possible framebuffer attachment (if the viewport has positive
  // offset and is between maxViewportDimensions and viewportBoundsRange[1],
  // GetHostViewportInfo will adjust ndc_scale/ndc_offset to clamp it, and the
  // clamped range will be outside the largest possible framebuffer anyway.
  // FIXME(Triang3l): Possibly handle maxViewportDimensions and
  // viewportBoundsRange separately because when using fragment shader
  // interlocks, framebuffers are not used, while the range may be wider than
  // dimensions? Though viewport bigger than 4096 - the smallest possible
  // maximum dimension (which is below the 8192 texture size limit on the Xbox
  // 360) - and with offset, is probably a situation that never happens in real
  // life. Or even disregard the viewport bounds range in the fragment shader
  // interlocks case completely - apply the viewport and the scissor offset
  // directly to pixel address and to things like ps_param_gen.
  draw_util::GetHostViewportInfo(
      regs, draw_resolution_scale_x, draw_resolution_scale_y, false,
      device_properties.maxViewportDimensions[0], device_properties.maxViewportDimensions[1], true,
      normalized_depth_control,
      host_render_targets_used && render_target_cache_->depth_float24_convert_in_pixel_shader(),
      host_render_targets_used, pixel_shader && pixel_shader->writes_depth(), viewport_info);

  // Keep window-space clears fixed so jitter cannot leave stale edge texels
  // for bloom. Projected backgrounds still need the same jitter as geometry:
  // depth-always draws may use a camera projection rather than screen space.
  if (fh1_dlss_ && fh1_dlss_->Evaluating() && !fh1_native_hud_active_ &&
      normalized_depth_control.z_enable &&
      !regs.Get<reg::PA_CL_VTE_CNTL>().vtx_xy_fmt &&
      regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 1280) {
    const auto& input = fh1_dlss_->input();
    if (viewport_info.xy_extent[0] && viewport_info.xy_extent[1]) {
      // Guest EDRAM tiling may crop the host viewport. Convert the desired
      // full-scene pixel displacement using that viewport's actual extent, so
      // each tile receives the same jitter instead of a fraction of it.
      viewport_info.ndc_offset[0] += Fh1ViewportJitter(input.jitter_x,
          fh1_dlss_->width(), 1280 * draw_resolution_scale_x, viewport_info.xy_extent[0]);
      viewport_info.ndc_offset[1] += Fh1ViewportJitter(input.jitter_y,
          fh1_dlss_->height(), 720 * draw_resolution_scale_y, viewport_info.xy_extent[1]);
    }
  }

  // Patch 0396: VR true stereo's eye offset (and the eye's part of the picture) for this draw.
  Fh1StereoDrawOffset(normalized_depth_control);

  // Update dynamic graphics pipeline state.
  UpdateDynamicState(viewport_info, primitive_polygonal, normalized_depth_control);

  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  // Whether to load the guest 32-bit (usually big-endian) vertex index
  // indirectly in the vertex shader if full 32-bit indices are not supported by
  // the host.
  bool shader_32bit_index_dma =
      !device_properties.fullDrawIndexUint32 &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
      vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32 &&
      primitive_processing_result.host_vertex_shader_type == Shader::HostVertexShaderType::kVertex;
  if (!device_properties.fullDrawIndexUint32 &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
      vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32 &&
      primitive_processing_result.host_vertex_shader_type !=
          Shader::HostVertexShaderType::kVertex) {
    // PrimitiveProcessor is expected to pre-convert this case to
    // ProcessedIndexBufferType::kHostConverted with host-endian 24-bit
    // indices. Keep drawing, but report if that pre-conversion didn't happen.
    static bool non_vertex_32bit_guest_dma_no_full_uint32_logged = false;
    if (!non_vertex_32bit_guest_dma_no_full_uint32_logged) {
      REXGPU_WARN(
          "Vulkan draw has host vertex shader type {} with guest DMA 32-bit "
          "indices on a device without fullDrawIndexUint32; expected "
          "PrimitiveProcessor to return host-converted indices",
          uint32_t(primitive_processing_result.host_vertex_shader_type));
      non_vertex_32bit_guest_dma_no_full_uint32_logged = true;
    }
  }

  // Keep screen-space HUD proportions and position separate from the widened world.
  if (normalized_color_mask && !normalized_depth_control.z_enable &&
      regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 1280 &&
      regs.Get<reg::RB_COLOR_INFO>(XE_GPU_REG_RB_COLOR_INFO).color_format ==
          xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10) {
    float anchor = 0;
    const int slot = Fh1HudTranslationRegister(vertex_shader->ucode_data_hash());
    if (slot >= 0) {
      std::array<float, 4> transform_origin;
      std::memcpy(transform_origin.data(), &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + 4 * slot],
                  sizeof(transform_origin));
      anchor = Fh1HudEdgeAnchor(transform_origin[0], transform_origin[3]);
    }
    PresentationTransform::Hud(
        REXCVAR_GET(forza_ultrawide_hud_x), REXCVAR_GET(forza_ultrawide_hud_y),
        REXCVAR_GET(forza_ultrawide_hud_offset_x), REXCVAR_GET(forza_ultrawide_hud_offset_y),
        REXCVAR_GET(forza_ultrawide_hud_edge_shift), anchor).Apply(viewport_info);
  }

  // Update system constants before uploading them.
  UpdateSystemConstantValues(primitive_polygonal, primitive_processing_result,
                             shader_32bit_index_dma, 0, viewport_info, used_texture_mask,
                             normalized_depth_control, normalized_color_mask);

  // Update uniform buffers and descriptor sets after binding the pipeline with
  // the new layout.
  if (!UpdateBindings(vertex_shader, pixel_shader)) {
    return draw_fail("update_bindings");
  }

  // Ensure vertex buffers are resident.
  const Shader::ConstantRegisterMap& constant_map_vertex = vertex_shader->constant_register_map();
  for (uint32_t i = 0; i < rex::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits_remaining = constant_map_vertex.vertex_fetch_bitmap[i];
    uint32_t j;
    while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      uint64_t vfetch_bit = uint64_t(1) << (vfetch_index & 63);
      if (vertex_buffers_in_sync_[vfetch_index >> 6] & vfetch_bit) {
        continue;
      }
      xenos::xe_gpu_vertex_fetch_t vfetch_constant = regs.GetVertexFetch(vfetch_index);
      switch (vfetch_constant.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
            break;
          }
          REXGPU_WARN(
              "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" type! "
              "This "
              "is incorrect behavior, but you can try bypassing this by "
              "launching Xenia with --gpu_allow_invalid_fetch_constants=true.",
              vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
        default:
          REXGPU_WARN("Vertex fetch constant {} ({:08X} {:08X}) is completely invalid!",
                      vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
      }
      VertexBufferState& state = vertex_buffer_states_[vfetch_index];
      if (state.address == vfetch_constant.address && state.size == vfetch_constant.size) {
        vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
        continue;
      }
      if (!shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2)) {
        REXGPU_ERROR(
            "Failed to request vertex buffer at 0x{:08X} (size {}) in the shared "
            "memory",
            vfetch_constant.address << 2, vfetch_constant.size << 2);
        return false;
      }
      state.address = vfetch_constant.address;
      state.size = vfetch_constant.size;
      vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
    }
  }

  // Synchronize the memory pages backing memory scatter export streams, and
  // calculate the range that includes the streams for the buffer barrier.
  uint32_t memexport_extent_start = UINT32_MAX, memexport_extent_end = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    uint32_t memexport_range_base_bytes = memexport_range.base_address_dwords << 2;
    if (!shared_memory_->RequestRange(memexport_range_base_bytes, memexport_range.size_bytes)) {
      REXGPU_ERROR(
          "Failed to request memexport stream at 0x{:08X} (size {}) in the "
          "shared memory",
          memexport_range_base_bytes, memexport_range.size_bytes);
      return false;
    }
    memexport_extent_start = std::min(memexport_extent_start, memexport_range_base_bytes);
    memexport_extent_end =
        std::max(memexport_extent_end, memexport_range_base_bytes + memexport_range.size_bytes);
  }
  if (memexport_writes_possible && memexport_ranges_.empty()) {
    if (!shared_memory_->RequestRange(0, SharedMemory::kBufferSize)) {
      REXGPU_ERROR(
          "Failed to request full shared memory residency for unresolved "
          "memexport destinations");
      return false;
    }
  }

  ScratchBufferAcquisition guest_dma_index_scratch_buffer;
  if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
      !shader_32bit_index_dma && memexport_writes_possible) {
    // Align with D3D12 behavior where memexporting draws using guest DMA indices
    // read a snapshot of indices to avoid read/write aliasing through shared
    // memory.
    VkDeviceSize guest_dma_index_size =
        VkDeviceSize(primitive_processing_result.host_draw_vertex_count) *
        (primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
             ? sizeof(uint16_t)
             : sizeof(uint32_t));
    if (guest_dma_index_size) {
      guest_dma_index_scratch_buffer = AcquireScratchGpuBuffer(
          guest_dma_index_size, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      if (guest_dma_index_scratch_buffer.buffer() == VK_NULL_HANDLE) {
        return draw_fail("guest_dma_index_scratch_buffer");
      }

      shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
      SubmitBarriers(true);

      VkBufferCopy guest_dma_index_copy_region = {};
      guest_dma_index_copy_region.srcOffset = primitive_processing_result.guest_index_base;
      guest_dma_index_copy_region.dstOffset = 0;
      guest_dma_index_copy_region.size = guest_dma_index_size;
      deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(),
                                               guest_dma_index_scratch_buffer.buffer(), 1,
                                               &guest_dma_index_copy_region);
      PushBufferMemoryBarrier(
          guest_dma_index_scratch_buffer.buffer(), 0, guest_dma_index_size,
          guest_dma_index_scratch_buffer.GetStageMask(), VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
          guest_dma_index_scratch_buffer.GetAccessMask(), VK_ACCESS_INDEX_READ_BIT);
      guest_dma_index_scratch_buffer.SetStageMask(VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
      guest_dma_index_scratch_buffer.SetAccessMask(VK_ACCESS_INDEX_READ_BIT);
    }
  }

  // Insert the shared memory barrier if needed.
  // TODO(Triang3l): Find some PM4 command that can be used for indication of
  // when memexports should be awaited instead of inserting the barrier in Use
  // every time if memory export was done in the previous draw?
  if (memexport_extent_start < memexport_extent_end) {
    shared_memory_->Use(
        VulkanSharedMemory::Usage::kGuestDrawReadWrite,
        std::make_pair(memexport_extent_start, memexport_extent_end - memexport_extent_start));
  } else if (memexport_writes_possible) {
    // Stream constants can be invalid or dynamic, making the exact destination
    // unknown. Keep synchronization conservative similarly to D3D12 UAV
    // handling for memexport-capable draws.
    shared_memory_->Use(VulkanSharedMemory::Usage::kGuestDrawReadWrite,
                        std::make_pair(0u, SharedMemory::kBufferSize));
  } else {
    shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  }

  // After all commands that may dispatch, copy or insert barriers, submit
  // the barriers (may end the render pass), and (re)enter the render pass
  // before drawing.
  SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      render_target_cache_->last_update_render_pass(),
      render_target_cache_->last_update_framebuffer());

  // What this draw reads from the shared memory: no upload into it can go before this render
  // pass any more (CanHoistSharedMemoryUpload).
  if (hoist_render_pass_) {
    for (const VulkanShader* shader : {static_cast<const VulkanShader*>(vertex_shader),
                                       static_cast<const VulkanShader*>(pixel_shader)}) {
      if (!shader) {
        continue;
      }
      const Shader::ConstantRegisterMap& constant_map = shader->constant_register_map();
      for (uint32_t i = 0; i < rex::countof(constant_map.vertex_fetch_bitmap); ++i) {
        uint32_t vfetch_bits_remaining = constant_map.vertex_fetch_bitmap[i];
        uint32_t j;
        while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
          vfetch_bits_remaining &= ~(uint32_t(1) << j);
          xenos::xe_gpu_vertex_fetch_t vfetch_constant = regs.GetVertexFetch(i * 32 + j);
          MarkSharedMemoryReadInRenderPass(vfetch_constant.address << 2,
                                           vfetch_constant.size << 2);
        }
      }
    }
    std::pair<uint32_t, uint32_t> guest_index_range =
        primitive_processor_->last_guest_index_range();
    MarkSharedMemoryReadInRenderPass(guest_index_range.first, guest_index_range.second);
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      MarkSharedMemoryReadInRenderPass(memexport_range.base_address_dwords << 2,
                                       memexport_range.size_bytes);
    }
  }

  // Draw.
  if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kNone ||
      shader_32bit_index_dma) {
    deferred_command_buffer_.CmdVkDraw(primitive_processing_result.host_draw_vertex_count, 1, 0, 0);
  } else {
    std::pair<VkBuffer, VkDeviceSize> index_buffer;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
        if (guest_dma_index_scratch_buffer.buffer() != VK_NULL_HANDLE) {
          index_buffer.first = guest_dma_index_scratch_buffer.buffer();
          index_buffer.second = 0;
        } else {
          index_buffer.first = shared_memory_->buffer();
          index_buffer.second = primitive_processing_result.guest_index_base;
        }
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        index_buffer = primitive_processor_->GetConvertedIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer = primitive_processor_->GetBuiltinIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        assert_unhandled_case(primitive_processing_result.index_buffer_type);
        return draw_fail("unexpected_index_buffer_type");
    }
    deferred_command_buffer_.CmdVkBindIndexBuffer(
        index_buffer.first, index_buffer.second,
        primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
            ? VK_INDEX_TYPE_UINT16
            : VK_INDEX_TYPE_UINT32);
    deferred_command_buffer_.CmdVkDrawIndexed(primitive_processing_result.host_draw_vertex_count, 1,
                                              0, 0, 0);
  }

  // Evidence only: a guest D record can be skipped by asynchronous pipeline
  // creation. V records are written only after an actual Vulkan draw was queued.
  // This does not select a scene snapshot or change rendering policy.
  if (fh1_frame_trace::file && fh1_frame_trace::draws <= 16384) {
    const bool recorded_draw = ThreadDrawTraceBudget().Write(fh1_frame_trace::file, frame_current_,
        "V draw=%u frame=%llu eye=%u vs=%016llX ps=%016llX image=%016llX cache=%p "
        "hostvp=%u,%u,%u,%u scale=%u,%u raster=%g,%g mask=%08X dctl=%08X "
        "blend=%08X ctl=%08X textures=%08X native=%u hdr=%u vr=%u\n",
        fh1_frame_trace::draws ? fh1_frame_trace::draws - 1 : 0,
        static_cast<unsigned long long>(frame_current_), fh1_stereo_eye_,
        static_cast<unsigned long long>(vertex_shader->ucode_data_hash()),
        static_cast<unsigned long long>(pixel_shader ? pixel_shader->ucode_data_hash() : 0),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(
            render_target_cache_->ColorAttachmentImageForDiagnostics())),
        static_cast<void*>(render_target_cache_.get()),
        viewport_info.xy_offset[0], viewport_info.xy_offset[1],
        viewport_info.xy_extent[0], viewport_info.xy_extent[1],
        render_target_cache_->draw_resolution_scale_x(), render_target_cache_->draw_resolution_scale_y(),
        render_target_cache_->raster_scale_x(), render_target_cache_->raster_scale_y(),
        normalized_color_mask, normalized_depth_control.value, regs[XE_GPU_REG_RB_BLENDCONTROL0],
        regs[XE_GPU_REG_RB_COLORCONTROL], used_texture_mask, uint32_t(Fh1NativeEnabled()),
        uint32_t(ui::vulkan::GetHdrOutputState().active), uint32_t(Fh1VrLayersWanted()));
    for (uint32_t slot = 0; slot < 32; ++slot) {
      if (!recorded_draw || !(used_texture_mask & (uint32_t(1) << slot))) continue;
      const auto& fetch = regs.GetTextureFetch(slot);
      ThreadDrawTraceBudget().Write(fh1_frame_trace::file, frame_current_,
          "T draw=%u slot=%u words=%08X,%08X,%08X,%08X,%08X,%08X\n",
          fh1_frame_trace::draws ? fh1_frame_trace::draws - 1 : 0, slot,
          fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
    }
  }

  // FH1 (#380): after the shadow mask's blur, multiplies the world AO into it.
  Fh1WorldAoAfterDraw(pixel_shader);

  if (hdr_menu_background) {
    // Include the decoded 1280x720 movie, but not the following logo, text or
    // selection controls. The title movie need not have a preceding 3D draw.
    // CaptureHdrScene's per-frame guard still permits only one snapshot.
    hdr_scene_base_ = regs.Get<reg::RB_COLOR_INFO>().color_base;
    CaptureHdrScene();
  }

  // Invalidate textures in memexported memory and watch for changes.
  if (!memexport_ranges_.empty()) {
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,
                                        memexport_range.size_bytes);
    }
  } else if (memexport_writes_possible) {
    // Stream constants can be invalid or dynamic, so exact destinations may be
    // unknown. Keep invalidation conservative in this case.
    shared_memory_->RangeWrittenByGpu(0, SharedMemory::kBufferSize);
  }

  // Empty first: the setting check looks the legacy setting up by name (a lock, two
  // strings and a hash lookup), which was 15% of the command processor in a race.
  if (!memexport_ranges_.empty() &&
      IsReadbackMemexportEnabled(REXCVAR_GET(vulkan_readback_memexport))) {
    uint32_t memexport_total_size = 0;
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      memexport_total_size += memexport_range.size_bytes;
    }
    if (memexport_total_size) {
      if (REXCVAR_GET(readback_memexport_fast)) {
        IssueDraw_MemexportReadbackFastPath(memexport_total_size);
      } else {
        IssueDraw_MemexportReadbackFullPath(memexport_total_size);
      }
    }
  }

  return true;
}

bool VulkanCommandProcessor::IssueDraw_MemexportReadbackFullPath(uint32_t total_size) {
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkBuffer readback_buffer = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory = VK_NULL_HANDLE;
  uint32_t readback_memory_type = UINT32_MAX;
  VkDeviceSize readback_memory_size = 0;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, total_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, readback_buffer, readback_memory,
          &readback_memory_type, &readback_memory_size)) {
    REXGPU_ERROR("Failed to create a Vulkan memexport readback buffer");
    return true;
  }

  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  SubmitBarriers(true);

  uint32_t readback_offset = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    VkBufferCopy readback_region = {};
    readback_region.srcOffset = memexport_range.base_address_dwords << 2;
    readback_region.dstOffset = readback_offset;
    readback_region.size = memexport_range.size_bytes;
    deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(), readback_buffer, 1,
                                             &readback_region);
    readback_offset += memexport_range.size_bytes;
  }
  PushBufferMemoryBarrier(readback_buffer, 0, total_size, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_HOST_READ_BIT);

  if (!AwaitAllQueueOperationsCompletion()) {
    REXGPU_ERROR("Failed to await completion of Vulkan memexport readback");
    dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
    dfn.vkFreeMemory(device, readback_memory, nullptr);
    return true;
  }

  void* readback_mapping = nullptr;
  if (dfn.vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0, &readback_mapping) !=
      VK_SUCCESS) {
    REXGPU_ERROR("Failed to map a Vulkan memexport readback buffer");
    dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
    dfn.vkFreeMemory(device, readback_memory, nullptr);
    return true;
  }

  if (!(vulkan_device->memory_types().host_coherent & (uint32_t(1) << readback_memory_type))) {
    VkMappedMemoryRange readback_memory_range = {};
    readback_memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    readback_memory_range.memory = readback_memory;
    readback_memory_range.offset = 0;
    readback_memory_range.size = std::min(
        rex::round_up(VkDeviceSize(total_size), vulkan_device->properties().nonCoherentAtomSize),
        readback_memory_size);
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &readback_memory_range);
  }

  const uint8_t* readback_bytes = reinterpret_cast<const uint8_t*>(readback_mapping);
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }
  dfn.vkUnmapMemory(device, readback_memory);
  dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
  dfn.vkFreeMemory(device, readback_memory, nullptr);
  return true;
}

bool VulkanCommandProcessor::IssueDraw_MemexportReadbackFastPath(uint32_t total_size) {
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  const uint64_t readback_key =
      MakeMemexportReadbackKey(memexport_ranges_.front().base_address_dwords, total_size);
  ReadbackBuffer& readback = memexport_readback_buffers_[readback_key];
  readback.last_used_frame = frame_current_;

  auto ensure_readback_slot = [&](uint32_t index, uint32_t size) -> bool {
    if (readback.buffers[index] != VK_NULL_HANDLE && readback.memories[index] != VK_NULL_HANDLE &&
        readback.mapped_data[index] != nullptr && size <= readback.sizes[index]) {
      return true;
    }

    VkBuffer new_buffer = VK_NULL_HANDLE;
    VkDeviceMemory new_memory = VK_NULL_HANDLE;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            ui::vulkan::util::MemoryPurpose::kReadback, new_buffer, new_memory)) {
      return false;
    }

    void* new_mapping = nullptr;
    if (dfn.vkMapMemory(device, new_memory, 0, VK_WHOLE_SIZE, 0, &new_mapping) != VK_SUCCESS) {
      dfn.vkDestroyBuffer(device, new_buffer, nullptr);
      dfn.vkFreeMemory(device, new_memory, nullptr);
      return false;
    }

    if (readback.buffers[index] != VK_NULL_HANDLE || readback.memories[index] != VK_NULL_HANDLE) {
      if (!AwaitAllQueueOperationsCompletion()) {
        dfn.vkUnmapMemory(device, new_memory);
        dfn.vkDestroyBuffer(device, new_buffer, nullptr);
        dfn.vkFreeMemory(device, new_memory, nullptr);
        return false;
      }
      if (readback.mapped_data[index] != nullptr && readback.memories[index] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[index]);
      }
      if (readback.buffers[index] != VK_NULL_HANDLE) {
        dfn.vkDestroyBuffer(device, readback.buffers[index], nullptr);
      }
      if (readback.memories[index] != VK_NULL_HANDLE) {
        dfn.vkFreeMemory(device, readback.memories[index], nullptr);
      }
    }

    readback.buffers[index] = new_buffer;
    readback.memories[index] = new_memory;
    readback.mapped_data[index] = new_mapping;
    readback.sizes[index] = size;
    readback.submission_written[index] = 0;
    readback.written_size[index] = 0;
    return true;
  };

  const uint32_t write_index = readback.current_index;
  const uint32_t read_index = 1 - write_index;
  const uint32_t readback_size = AlignReadbackBufferSize(total_size);
  if (!ensure_readback_slot(write_index, readback_size)) {
    return IssueDraw_MemexportReadbackFullPath(total_size);
  }

  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  SubmitBarriers(true);

  uint32_t readback_offset = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    VkBufferCopy readback_region = {};
    readback_region.srcOffset = memexport_range.base_address_dwords << 2;
    readback_region.dstOffset = readback_offset;
    readback_region.size = memexport_range.size_bytes;
    deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(),
                                             readback.buffers[write_index], 1, &readback_region);
    readback_offset += memexport_range.size_bytes;
  }
  PushBufferMemoryBarrier(readback.buffers[write_index], 0, total_size,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
  readback.submission_written[write_index] = GetCurrentSubmission();
  readback.written_size[write_index] = total_size;

  CheckSubmissionFenceAndDeviceLoss(0);
  bool previous_slot_ready =
      readback.buffers[read_index] != VK_NULL_HANDLE &&
      readback.memories[read_index] != VK_NULL_HANDLE &&
      readback.mapped_data[read_index] != nullptr && total_size <= readback.sizes[read_index] &&
      total_size <= readback.written_size[read_index] && readback.submission_written[read_index] &&
      readback.submission_written[read_index] <= submission_completed_;
  if (!previous_slot_ready) {
    IssueDraw_MemexportReadbackFullPath(total_size);
    readback.current_index = read_index;
    return true;
  }

  VkMappedMemoryRange readback_memory_range = {};
  readback_memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  readback_memory_range.memory = readback.memories[read_index];
  readback_memory_range.offset = 0;
  readback_memory_range.size = VK_WHOLE_SIZE;
  dfn.vkInvalidateMappedMemoryRanges(device, 1, &readback_memory_range);

  const uint8_t* readback_bytes =
      reinterpret_cast<const uint8_t*>(readback.mapped_data[read_index]);
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }

  readback.current_index = read_index;
  return true;
}

bool VulkanCommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (!BeginSubmission(true)) {
    return false;
  }

  ReadbackResolveMode readback_mode = GetReadbackResolveMode(REXCVAR_GET(vulkan_readback_resolve));
  if (readback_mode == ReadbackResolveMode::kDisabled) {
    uint32_t written_address, written_length;
    const bool resolved = render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,
                                         written_address, written_length);
    if (resolved && written_length && !Fh1SynchronizeResolve(written_address, written_length))
      REXGPU_ERROR("FH1 cross-scale resolve synchronization failed");
    return resolved;
  }

  return IssueCopy_ReadbackResolvePath();
}

// Scaled resolves repeat 16-byte quads, with scale_x*scale_y host quads
// for each guest quad. Mirror these blocks on the GPU when the scene/UI caches
// have different integer scales. This keeps minimap and other render-to-texture
// HUD inputs coherent without a CPU readback or borrowing foreign image views.
bool VulkanCommandProcessor::Fh1SynchronizeResolve(uint32_t address, uint32_t length) {
  if (!fh1_split_enabled_ || !fh1_other_textures_ ||
      !fh1_other_textures_->IsDrawResolutionScaled() || !length) return true;
  if (texture_cache_->IsDrawResolutionScaled() &&
      !texture_cache_->HasScaledResolveRange(address, length)) return true;
  // Resolve storage and the copy shader operate on complete 16-byte quads.
  if ((address | length) & 15u) return false;
  auto& source = *texture_cache_;
  auto& destination = *fh1_other_textures_;
  const auto* vulkan_device = GetVulkanDevice();
  const auto& f = vulkan_device->functions(); const auto device = vulkan_device->device();
  const auto dest_info = register_file_->Get<reg::RB_COPY_DEST_INFO>();
  const auto bpp = FormatInfo::Get(uint32_t(dest_info.copy_dest_format))->bits_per_pixel;
  if (bpp != 8 && bpp != 16 && bpp != 32 && bpp != 64 && bpp != 128) return false;
  const uint32_t pixel_bytes = bpp / 8;
  const uint32_t source_scale = source.draw_resolution_scale_x();
  const uint32_t destination_scale = destination.draw_resolution_scale_x();
  if (source.draw_resolution_scale_y() != source_scale ||
      destination.draw_resolution_scale_y() != destination_scale ||
      length > UINT32_MAX / (destination_scale * destination_scale)) return false;
  // A 1x scene writes resolves to shared memory, while the native HUD
  // reads its own scaled resolve buffer. Mirror that source too (DLSS
  // Performance at 1440p is 1x scene / 2x HUD). The reverse direction keeps
  // the existing policy: never overwrite the live scene's shared buffer.
  uint64_t source_start = address, source_length = length;
  VkBuffer source_buffer = shared_memory_->buffer();
  if (source.IsDrawResolutionScaled()) {
    if (!source.GetScaledResolveRange(address, length, 0, source_start, source_length)) return false;
    source_buffer = source.scaled_resolve_buffer();
  }
  uint64_t dest_start, dest_length;
  if (!destination.CommitScaledResolveRange(address, length) ||
      !destination.GetScaledResolveRange(address, length, 0, dest_start, dest_length)) return false;
  if (!fh1_resolve_pipeline_) {
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 * sizeof(uint32_t)};
    const auto set_layout = GetSingleTransientDescriptorLayout(SingleTransientDescriptorLayout::kStorageBufferPairCompute);
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1; layout.pSetLayouts = &set_layout;
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
    if (f.vkCreatePipelineLayout(device, &layout, nullptr, &fh1_resolve_layout_) != VK_SUCCESS) return false;
    static constexpr char shader[] = R"glsl(#version 450
layout(local_size_x=256) in;
layout(binding=0,std430) readonly buffer Source { uint src[]; };
layout(binding=1,std430) writeonly buffer Destination { uint dst[]; };
layout(push_constant) uniform Parameters { uint sourceScale; uint destScale; uint pixelBytes;
  uint byteCount; uint sourceOffset; uint destOffset; uint fastCopy; uint endianMask; };
uint sourceByteOffset(uint offset) {
  // Resolve storage repeats 16-byte quads, not individual texels.
  uint quad = (offset >> 4u) / (destScale * destScale);
  uint subquad = (offset >> 4u) % (destScale * destScale);
  uint component = (offset & 15u) ^ endianMask;
  uint pixelsPerQuad = 16u / pixelBytes;
  uint destX = (subquad / destScale) * pixelsPerQuad + component / pixelBytes;
  uint destY = subquad % destScale;
  uint sourceX = (destX * 2u + 1u) * sourceScale / (destScale * 2u);
  uint sourceY = (destY * 2u + 1u) * sourceScale / (destScale * 2u);
  uint sourceQuad = quad * sourceScale * sourceScale +
      (sourceX / pixelsPerQuad) * sourceScale + sourceY;
  uint sourceComponent = ((sourceX % pixelsPerQuad) * pixelBytes +
      component % pixelBytes) ^ endianMask;
  return sourceOffset + sourceQuad * 16u + sourceComponent;
}
uint loadByte(uint offset) {
  uint sourceByte = sourceByteOffset(offset);
  return (src[sourceByte>>2u]>>((sourceByte&3u)*8u))&255u;
}
void main() {
  uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*65535u*256u;
  uint offset=i*4u;
  if(offset>=byteCount) return;
  // Preserve the packed-word optimization only where the quad mapping keeps
  // all four bytes together. Sub-word pixels and wide endian modes use bytes.
  if (fastCopy != 0u && pixelBytes >= 4u && endianMask < 4u &&
      (sourceOffset & 3u) == 0u) {
    dst[(destOffset+offset)>>2u] = src[sourceByteOffset(offset)>>2u];
    return;
  }
  uint value=0u;
  for(uint b=0u;b<4u;++b) value|=loadByte(offset+b)<<(8u*b);
  dst[(destOffset+offset)>>2u]=value;
})glsl";
    std::vector<uint32_t> code; std::string error;
    if (!CompileFh1Compute(shader, code, error)) { REXGPU_ERROR("FH1 resolve shader: {}", error); return false; }
    fh1_resolve_pipeline_ = ui::vulkan::util::CreateComputePipeline(vulkan_device, fh1_resolve_layout_,
        code.data(), code.size()*sizeof(uint32_t));
    if (!fh1_resolve_pipeline_) return false;
  }
  const auto set = AllocateSingleTransientDescriptor(SingleTransientDescriptorLayout::kStorageBufferPairCompute);
  if (!set) return false;
  const uint64_t alignment = vulkan_device->properties().minStorageBufferOffsetAlignment;
  const uint64_t source_base = source_start & ~(alignment-1), dest_base = dest_start & ~(alignment-1);
  const VkDescriptorBufferInfo buffers[] = {
      {source_buffer, source_base, source_length+source_start-source_base},
      {destination.scaled_resolve_buffer(), dest_base, dest_length+dest_start-dest_base}};
  VkWriteDescriptorSet writes[2]{};
  for (unsigned i=0;i<2;++i) {
    writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=set;
    writes[i].dstBinding=i; writes[i].descriptorCount=1;
    writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo=&buffers[i];
  }
  f.vkUpdateDescriptorSets(device,2,writes,0,nullptr);
  if (source.IsDrawResolutionScaled()) source.UseScaledResolveBufferForRead();
  else shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  destination.UseScaledResolveBufferForWrite(dest_start,dest_length);
  SubmitBarriers(true);
  BindExternalComputePipeline(fh1_resolve_pipeline_);
  static constexpr uint32_t endian_masks[] = {0, 1, 3, 2, 7, 15};
  const uint32_t endian = uint32_t(dest_info.copy_dest_endian);
  if (endian >= std::size(endian_masks)) return false;
  const uint32_t constants[] = {source_scale,destination_scale,pixel_bytes,uint32_t(dest_length),
      uint32_t(source_start-source_base),uint32_t(dest_start-dest_base),
      uint32_t(REXCVAR_GET(fh1_fast_resolve_copy)),endian_masks[endian]};
  deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,fh1_resolve_layout_,0,1,&set,0,nullptr);
  deferred_command_buffer_.CmdVkPushConstants(fh1_resolve_layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(constants),constants);
  const uint32_t groups = uint32_t((dest_length+1023)/1024);
  deferred_command_buffer_.CmdVkDispatch(std::min(groups,65535u),(groups+65534)/65535,1);
  destination.MarkRangeAsResolved(address,length);
  return true;
}

bool VulkanCommandProcessor::IssueCopy_ReadbackResolvePath() {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  uint32_t written_address, written_length;
  if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_, written_address,
                                     written_length)) {
    return false;
  }

  if (written_length && !Fh1SynchronizeResolve(written_address, written_length))
    REXGPU_ERROR("FH1 cross-scale resolve synchronization failed");
  if (!written_length) {
    return true;
  }

  if (!memory_->TranslatePhysical(written_address)) {
    return true;
  }

  ReadbackResolveMode readback_mode = GetReadbackResolveMode(REXCVAR_GET(vulkan_readback_resolve));
  if (readback_mode == ReadbackResolveMode::kDisabled) {
    return true;
  }

  auto ensure_readback_slot = [&](ReadbackBuffer& readback, uint32_t index, uint32_t size) -> bool {
    if (readback.buffers[index] != VK_NULL_HANDLE && size <= readback.sizes[index] &&
        readback.mapped_data[index] != nullptr) {
      return true;
    }

    VkBuffer new_buffer = VK_NULL_HANDLE;
    VkDeviceMemory new_memory = VK_NULL_HANDLE;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            ui::vulkan::util::MemoryPurpose::kReadback, new_buffer, new_memory)) {
      REXGPU_ERROR("Failed to create a {} MB Vulkan resolve readback buffer", size >> 20);
      return false;
    }

    void* new_mapping = nullptr;
    if (dfn.vkMapMemory(device, new_memory, 0, VK_WHOLE_SIZE, 0, &new_mapping) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to map a Vulkan resolve readback buffer");
      dfn.vkDestroyBuffer(device, new_buffer, nullptr);
      dfn.vkFreeMemory(device, new_memory, nullptr);
      return false;
    }

    if (readback.buffers[index] != VK_NULL_HANDLE || readback.memories[index] != VK_NULL_HANDLE) {
      if (!AwaitAllQueueOperationsCompletion()) {
        dfn.vkUnmapMemory(device, new_memory);
        dfn.vkDestroyBuffer(device, new_buffer, nullptr);
        dfn.vkFreeMemory(device, new_memory, nullptr);
        return false;
      }
      if (readback.mapped_data[index] != nullptr && readback.memories[index] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[index]);
      }
      if (readback.buffers[index] != VK_NULL_HANDLE) {
        dfn.vkDestroyBuffer(device, readback.buffers[index], nullptr);
      }
      if (readback.memories[index] != VK_NULL_HANDLE) {
        dfn.vkFreeMemory(device, readback.memories[index], nullptr);
      }
    }

    readback.buffers[index] = new_buffer;
    readback.memories[index] = new_memory;
    readback.mapped_data[index] = new_mapping;
    readback.sizes[index] = size;
    return true;
  };

  // FH1 reuses the three 1280x720 RGBA8 thumbnail staging surfaces for
  // the small tile and the large car card within a few frames. "some" would
  // retain the first (small-tile) CPU copy for the later card, saving a stale
  // or empty car. Refresh these partial color resolves and the 768x288 atlas
  // immediately; the continuously rendered scene/depth/float targets retain
  // the normal cache behavior.
  const auto dest_pitch = register_file_->Get<reg::RB_COPY_DEST_PITCH>();
  const auto dest_info = register_file_->Get<reg::RB_COPY_DEST_INFO>();
  const auto copy_control = register_file_->Get<reg::RB_COPY_CONTROL>();
  const bool thumbnail_readback = copy_control.copy_src_select < 4 &&
      dest_info.copy_dest_format == xenos::ColorFormat::k_8_8_8_8 &&
      ((dest_pitch.copy_dest_pitch == 768 && dest_pitch.copy_dest_height == 288) ||
       (dest_pitch.copy_dest_pitch == 1280 && dest_pitch.copy_dest_height == 720 &&
        (written_length == 1310720 || written_length == 1146880)));
  if (thumbnail_readback) {
    readback_mode = ReadbackResolveMode::kFull;
  }
  bool is_scaled = texture_cache_->IsDrawResolutionScaled();
  uint64_t resolve_key = GetReadbackResolveKey(written_address, written_length);
  ReadbackBuffer& readback = readback_buffers_[resolve_key];
  readback.last_used_frame = frame_current_;
  fh1_frame_cost::Frame& cost = fh1_frame_cost::Current();
  if (readback_mode == ReadbackResolveMode::kSome) {
    // FH1 (#45): a scene render target back after its buffer was dropped (the map, a long
    // menu) was copied back before and is not read by the CPU: no GPU wait and copy again.
    fh1_readback_history::History& history = GetReadbackHistory();
    if (!readback.resolve_cpu_copy_valid && history.IsHot(resolve_key) &&
        REXCVAR_GET(vulkan_readback_skip_render_targets)) {
      readback.resolve_cpu_copy_valid = true;
      ++cost.readbacks_skipped;
    }
    history.Resolved(resolve_key, frame_current_);
    if (frame_current_ % 600 == 0) history.Forget(frame_current_);
  }
  // "some" only needs the first CPU copy of this address/size/format key.
  // Keep the normal GPU resolve above, but avoid downscaling and transferring
  // every subsequent result to a host buffer that the CPU will never read.
  // Touch last_used_frame first so active entries retain the existing lifetime.
  if (readback_mode == ReadbackResolveMode::kSome && readback.resolve_cpu_copy_valid) {
    return true;
  }
  uint32_t write_index = readback.current_index;
  uint32_t readback_size = AlignReadbackBufferSize(written_length);
  bool slot_ready;
  {
    fh1_frame_cost::Timer cost_timer(cost.readback_ms);
    slot_ready = ensure_readback_slot(readback, write_index, readback_size);
  }
  if (!slot_ready) {
    return true;
  }

  if (is_scaled) {
    if (!resolve_downscale_pipeline_ || !resolve_downscale_pipeline_layout_) {
      return true;
    }

    reg::RB_COPY_DEST_INFO copy_dest_info = register_file_->Get<reg::RB_COPY_DEST_INFO>();
    const FormatInfo* format_info = FormatInfo::Get(uint32_t(copy_dest_info.copy_dest_format));
    uint32_t bits_per_pixel = format_info->bits_per_pixel;
    if (bits_per_pixel != 8 && bits_per_pixel != 16 && bits_per_pixel != 32 &&
        bits_per_pixel != 64) {
      return true;
    }

    uint32_t pixel_size_log2;
    if (!rex::bit_scan_forward(bits_per_pixel >> 3, &pixel_size_log2)) {
      return true;
    }
    uint32_t tile_size_1x = 32 * 32 * (uint32_t(1) << pixel_size_log2);
    uint32_t tile_count = written_length / tile_size_1x;
    if (!tile_count) {
      return true;
    }

    uint64_t scaled_start = 0, scaled_length = 0;
    if (!texture_cache_->GetScaledResolveRange(written_address, written_length, 0, scaled_start,
                                               scaled_length)) {
      return true;
    }
    if (!scaled_length) {
      return true;
    }

    VkBuffer scaled_resolve_buffer = texture_cache_->scaled_resolve_buffer();
    if (scaled_resolve_buffer == VK_NULL_HANDLE) {
      return true;
    }

    // At a draw resolution scale of 3 (and above) the scaled resolve buffer (512 MB * scale^2)
    // is larger than maxStorageBufferRange (4 GB on AMD), so binding all of it (VK_WHOLE_SIZE)
    // is invalid and reads come back as zeros (a black thumbnail, #102), and the 32-bit byte
    // offsets in the shader can't address the upper part. Bind only a window of the buffer
    // around the range then. Smaller buffers keep the whole-buffer binding as before.
    const uint64_t scaled_buffer_size =
        uint64_t(SharedMemory::kBufferSize) * texture_cache_->draw_resolution_scale_x() *
        texture_cache_->draw_resolution_scale_y();
    const bool scaled_source_windowed =
        scaled_buffer_size > uint64_t(vulkan_device->properties().maxStorageBufferRange);
    uint64_t scaled_source_window_start = 0;
    VkDeviceSize scaled_source_window_range = VK_WHOLE_SIZE;
    if (scaled_source_windowed) {
      const uint64_t window_alignment =
          std::max<uint64_t>(vulkan_device->properties().minStorageBufferOffsetAlignment, 4096);
      scaled_source_window_start = scaled_start & ~(window_alignment - 1);
      scaled_source_window_range =
          rex::align(scaled_start - scaled_source_window_start + scaled_length, uint64_t(4));
      scaled_source_window_range =
          std::min<uint64_t>(scaled_source_window_range,
                             scaled_buffer_size - scaled_source_window_start);
      if (scaled_start - scaled_source_window_start > uint64_t(UINT32_MAX) ||
          scaled_source_window_range > uint64_t(vulkan_device->properties().maxStorageBufferRange)) {
        return true;
      }
    } else if (scaled_start > uint64_t(UINT32_MAX)) {
      return true;
    }

    uint32_t downscale_buffer_size = AlignReadbackBufferSize(written_length);
    if (downscale_buffer_size > resolve_downscale_buffer_size_) {
      VkBuffer new_buffer = VK_NULL_HANDLE;
      VkDeviceMemory new_memory = VK_NULL_HANDLE;
      if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device, downscale_buffer_size,
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
              ui::vulkan::util::MemoryPurpose::kDeviceLocal, new_buffer, new_memory)) {
        REXGPU_ERROR("Failed to create a {} MB Vulkan resolve downscale buffer",
                     downscale_buffer_size >> 20);
        return true;
      }
      if (resolve_downscale_buffer_ != VK_NULL_HANDLE ||
          resolve_downscale_buffer_memory_ != VK_NULL_HANDLE) {
        if (!AwaitAllQueueOperationsCompletion()) {
          dfn.vkDestroyBuffer(device, new_buffer, nullptr);
          dfn.vkFreeMemory(device, new_memory, nullptr);
          return true;
        }
        if (resolve_downscale_buffer_ != VK_NULL_HANDLE) {
          dfn.vkDestroyBuffer(device, resolve_downscale_buffer_, nullptr);
        }
        if (resolve_downscale_buffer_memory_ != VK_NULL_HANDLE) {
          dfn.vkFreeMemory(device, resolve_downscale_buffer_memory_, nullptr);
        }
      }
      resolve_downscale_buffer_ = new_buffer;
      resolve_downscale_buffer_memory_ = new_memory;
      resolve_downscale_buffer_size_ = downscale_buffer_size;
    }
    if (resolve_downscale_buffer_ == VK_NULL_HANDLE) {
      return true;
    }

    VkDescriptorSet descriptor_set = AllocateSingleTransientDescriptor(
        SingleTransientDescriptorLayout::kStorageBufferPairCompute);
    if (descriptor_set == VK_NULL_HANDLE) {
      return true;
    }

    VkDescriptorBufferInfo buffer_infos[2] = {};
    buffer_infos[0].buffer = scaled_resolve_buffer;
    buffer_infos[0].offset = scaled_source_window_start;
    buffer_infos[0].range = scaled_source_window_range;
    buffer_infos[1].buffer = resolve_downscale_buffer_;
    buffer_infos[1].offset = 0;
    buffer_infos[1].range = written_length;

    VkWriteDescriptorSet descriptor_writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
      descriptor_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      descriptor_writes[i].dstSet = descriptor_set;
      descriptor_writes[i].dstBinding = i;
      descriptor_writes[i].descriptorCount = 1;
      descriptor_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      descriptor_writes[i].pBufferInfo = &buffer_infos[i];
    }
    dfn.vkUpdateDescriptorSets(device, 2, descriptor_writes, 0, nullptr);

    texture_cache_->UseScaledResolveBufferForRead();
    SubmitBarriers(true);

    VkBufferMemoryBarrier pre_barrier = {};
    pre_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    pre_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    pre_barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    pre_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre_barrier.buffer = resolve_downscale_buffer_;
    pre_barrier.offset = 0;
    pre_barrier.size = written_length;
    deferred_command_buffer_.CmdVkPipelineBarrier(
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &pre_barrier, 0, nullptr);

    BindExternalComputePipeline(resolve_downscale_pipeline_);

    ResolveDownscaleConstants constants;
    constants.scale_x = texture_cache_->draw_resolution_scale_x();
    constants.scale_y = texture_cache_->draw_resolution_scale_y();
    constants.pixel_size_log2 = pixel_size_log2;
    constants.tile_count = tile_count;
    constants.source_offset_bytes = uint32_t(scaled_start - scaled_source_window_start);
    constants.half_pixel_offset = (REXCVAR_GET(readback_resolve_half_pixel_offset) &&
                                   (constants.scale_x > 1 || constants.scale_y > 1))
                                      ? 1u
                                      : 0u;
    // RGBA8 car thumbnails are saved at guest resolution. Average their
    // supersampled pixels rather than discarding all but one sample.
    if (thumbnail_readback) constants.half_pixel_offset = 2u;
    deferred_command_buffer_.CmdVkPushConstants(resolve_downscale_pipeline_layout_,
                                                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                                                &constants);
    deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                                     resolve_downscale_pipeline_layout_, 0, 1,
                                                     &descriptor_set, 0, nullptr);
    deferred_command_buffer_.CmdVkDispatch(tile_count, 1, 1);

    VkBufferMemoryBarrier downscale_barrier = {};
    downscale_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    downscale_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    downscale_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    downscale_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    downscale_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    downscale_barrier.buffer = resolve_downscale_buffer_;
    downscale_barrier.offset = 0;
    downscale_barrier.size = written_length;
    deferred_command_buffer_.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                                                  &downscale_barrier, 0, nullptr);

    VkBufferCopy readback_region = {};
    readback_region.srcOffset = 0;
    readback_region.dstOffset = 0;
    readback_region.size = written_length;
    deferred_command_buffer_.CmdVkCopyBuffer(resolve_downscale_buffer_,
                                             readback.buffers[write_index], 1, &readback_region);
  } else {
    shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
    SubmitBarriers(true);

    VkBufferCopy readback_region = {};
    readback_region.srcOffset = written_address;
    readback_region.dstOffset = 0;
    readback_region.size = written_length;
    deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(),
                                             readback.buffers[write_index], 1, &readback_region);
  }

  PushBufferMemoryBarrier(readback.buffers[write_index], 0, written_length,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);

  bool use_delayed_sync =
      readback_mode == ReadbackResolveMode::kFast || readback_mode == ReadbackResolveMode::kSome;
  uint32_t read_index = write_index;
  if (use_delayed_sync) {
    read_index = 1 - write_index;
  } else {
    fh1_frame_cost::Timer cost_timer(cost.readback_wait_ms);
    if (!AwaitAllQueueOperationsCompletion()) {
      return true;
    }
  }

  bool is_cache_miss = false;
  if (use_delayed_sync && (readback.buffers[read_index] == VK_NULL_HANDLE ||
                           written_length > readback.sizes[read_index] ||
                           readback.mapped_data[read_index] == nullptr)) {
    is_cache_miss = true;
    read_index = write_index;
    fh1_frame_cost::Timer cost_timer(cost.readback_wait_ms);
    if (!AwaitAllQueueOperationsCompletion()) {
      return true;
    }
  }

  bool should_copy = (readback_mode == ReadbackResolveMode::kSome) ? is_cache_miss : true;
  if (should_copy && readback.buffers[read_index] != VK_NULL_HANDLE &&
      written_length <= readback.sizes[read_index] && readback.mapped_data[read_index] != nullptr) {
    fh1_frame_cost::Timer cost_timer(cost.readback_ms);
    ++cost.readbacks;
    cost.readback_bytes += written_length;
    // FH1 (#167): this resolve is copied back for the CPU (readback_resolve), and draws of this
    // frame were skipped for pipelines still being built: what the game may read misses them.
    // Renders made once and kept (a car's picture, its livery, a photo) are made with
    // sync_pipeline_creation for that. A cold cache's boot and loads copy many first-seen
    // resolves this way, as before 0364.
    if (frame_used_async_placeholder_pipeline_ && REXCVAR_GET(async_shader_compilation) &&
        !REXCVAR_GET(sync_pipeline_creation)) {
      const uint64_t incomplete = ++fh1_pipeline_lookahead::stats.incomplete_readbacks;
      if (incomplete <= 10 || incomplete % 100 == 0) {
        REXGPU_WARN("Pipelines: a resolve copied back for the CPU ({} bytes at {:08X}) missed "
                    "draws whose pipelines were still being built ({} so far)",
                    written_length, written_address, incomplete);
      }
    }
    VkMappedMemoryRange readback_memory_range = {};
    readback_memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    readback_memory_range.memory = readback.memories[read_index];
    readback_memory_range.offset = 0;
    readback_memory_range.size = VK_WHOLE_SIZE;
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &readback_memory_range);

    uint8_t* destination = memory_->TranslatePhysical(written_address);
    if (destination) {
      std::memcpy(destination, readback.mapped_data[read_index], written_length);
      readback.resolve_cpu_copy_valid = true;
    }
  }

  readback.current_index = 1 - readback.current_index;
  return true;
}

void VulkanCommandProcessor::EvictOldReadbackBuffers(
    std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map) {
  if (buffer_map.empty()) {
    return;
  }

  const uint64_t eviction_frame_floor = (frame_current_ > kReadbackBufferEvictionAgeFrames)
                                            ? (frame_current_ - kReadbackBufferEvictionAgeFrames)
                                            : 0;
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (auto it = buffer_map.begin(); it != buffer_map.end();) {
    ReadbackBuffer& readback = it->second;
    bool evict =
        buffer_map.size() > kMaxReadbackBuffers || readback.last_used_frame < eviction_frame_floor;
    if (!evict) {
      ++it;
      continue;
    }
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.mapped_data[i] && readback.memories[i] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[i]);
      }
      if (readback.buffers[i] != VK_NULL_HANDLE) {
        dfn.vkDestroyBuffer(device, readback.buffers[i], nullptr);
      }
      if (readback.memories[i] != VK_NULL_HANDLE) {
        dfn.vkFreeMemory(device, readback.memories[i], nullptr);
      }
      readback.buffers[i] = VK_NULL_HANDLE;
      readback.memories[i] = VK_NULL_HANDLE;
      readback.mapped_data[i] = nullptr;
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
    it = buffer_map.erase(it);
  }
}

bool VulkanCommandProcessor::InitializeOcclusionQueryResources() {
  active_occlusion_query_ = {};
  occlusion_query_cursor_ = 0;
  occlusion_query_resources_available_ = false;
  occlusion_query_pool_ = VK_NULL_HANDLE;
  occlusion_query_readback_buffer_ = VK_NULL_HANDLE;
  occlusion_query_readback_memory_ = VK_NULL_HANDLE;
  occlusion_query_readback_memory_type_ = UINT32_MAX;
  occlusion_query_readback_memory_size_ = 0;
  occlusion_query_readback_mapping_ = nullptr;

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  VkQueryPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  pool_info.queryType = VK_QUERY_TYPE_OCCLUSION;
  pool_info.queryCount = kMaxOcclusionQueries;
  if (dfn.vkCreateQueryPool(device, &pool_info, nullptr, &occlusion_query_pool_) != VK_SUCCESS) {
    REXGPU_WARN(
        "VulkanCommandProcessor: Failed to create occlusion query pool, using fake sample counts");
    return false;
  }

  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, sizeof(uint64_t) * kMaxOcclusionQueries, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, occlusion_query_readback_buffer_,
          occlusion_query_readback_memory_, &occlusion_query_readback_memory_type_,
          &occlusion_query_readback_memory_size_)) {
    REXGPU_WARN(
        "VulkanCommandProcessor: Failed to create occlusion query readback buffer, using fake "
        "sample counts");
    ShutdownOcclusionQueryResources();
    return false;
  }

  if (dfn.vkMapMemory(device, occlusion_query_readback_memory_, 0, VK_WHOLE_SIZE, 0,
                      reinterpret_cast<void**>(&occlusion_query_readback_mapping_)) != VK_SUCCESS) {
    REXGPU_WARN(
        "VulkanCommandProcessor: Failed to map occlusion query readback buffer, using fake sample "
        "counts");
    ShutdownOcclusionQueryResources();
    return false;
  }

  occlusion_query_resources_available_ = true;
  return true;
}

void VulkanCommandProcessor::ShutdownOcclusionQueryResources() {
  DisableHostOcclusionQueries();

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  if (occlusion_query_readback_mapping_ && occlusion_query_readback_memory_ != VK_NULL_HANDLE) {
    dfn.vkUnmapMemory(device, occlusion_query_readback_memory_);
  }
  occlusion_query_readback_mapping_ = nullptr;
  if (occlusion_query_readback_buffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, occlusion_query_readback_buffer_, nullptr);
  }
  if (occlusion_query_readback_memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, occlusion_query_readback_memory_, nullptr);
  }
  if (occlusion_query_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyQueryPool(device, occlusion_query_pool_, nullptr);
  }

  occlusion_query_pool_ = VK_NULL_HANDLE;
  occlusion_query_readback_buffer_ = VK_NULL_HANDLE;
  occlusion_query_readback_memory_ = VK_NULL_HANDLE;
  occlusion_query_readback_memory_type_ = UINT32_MAX;
  occlusion_query_readback_memory_size_ = 0;
}

bool VulkanCommandProcessor::AcquireOcclusionQueryIndex(uint32_t& host_index_out) {
  if (occlusion_query_cursor_ >= kMaxOcclusionQueries) {
    occlusion_query_cursor_ = 0;
  }
  host_index_out = occlusion_query_cursor_++;
  return true;
}

void VulkanCommandProcessor::DisableHostOcclusionQueries() {
  if (active_occlusion_query_.valid && occlusion_query_pool_ != VK_NULL_HANDLE) {
    if (BeginSubmission(true)) {
      deferred_command_buffer_.CmdVkEndQuery(occlusion_query_pool_,
                                             active_occlusion_query_.host_index);
      EndSubmission(false);
    }
  }
  active_occlusion_query_ = {};
  occlusion_query_cursor_ = 0;
  occlusion_query_resources_available_ = false;
}

bool VulkanCommandProcessor::BeginGuestOcclusionQuery(uint32_t sample_count_address) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_ ||
      occlusion_query_pool_ == VK_NULL_HANDLE || occlusion_query_readback_mapping_ == nullptr) {
    return false;
  }
  if (active_occlusion_query_.valid) {
    REXGPU_WARN(
        "VulkanCommandProcessor: Occlusion query begin issued while another query is active");
    DisableHostOcclusionQueries();
    return false;
  }

  uint32_t host_index = 0;
  if (!AcquireOcclusionQueryIndex(host_index)) {
    return false;
  }
  if (!BeginSubmission(true)) {
    return false;
  }

  EndRenderPass();

  DeferredCommandBuffer& command_buffer = deferred_command_buffer();
  command_buffer.CmdVkResetQueryPool(occlusion_query_pool_, host_index, 1);
  command_buffer.CmdVkBeginQuery(occlusion_query_pool_, host_index, 0);
  active_occlusion_query_.sample_count_address = sample_count_address;
  active_occlusion_query_.host_index = host_index;
  active_occlusion_query_.valid = true;
  return true;
}

bool VulkanCommandProcessor::EndGuestOcclusionQuery(uint32_t sample_count_address) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_ ||
      !active_occlusion_query_.valid || occlusion_query_pool_ == VK_NULL_HANDLE ||
      occlusion_query_readback_mapping_ == nullptr) {
    return false;
  }

  uint32_t host_index = active_occlusion_query_.host_index;
  active_occlusion_query_ = {};

  if (!BeginSubmission(true)) {
    return false;
  }

  EndRenderPass();

  DeferredCommandBuffer& command_buffer = deferred_command_buffer();
  command_buffer.CmdVkEndQuery(occlusion_query_pool_, host_index);
  command_buffer.CmdVkCopyQueryPoolResults(occlusion_query_pool_, host_index, 1,
                                           occlusion_query_readback_buffer_,
                                           sizeof(uint64_t) * host_index, sizeof(uint64_t),
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

  if (!EndSubmission(false)) {
    return false;
  }
  if (!AwaitAllQueueOperationsCompletion()) {
    return false;
  }

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (!(vulkan_device->memory_types().host_coherent &
        (uint32_t(1) << occlusion_query_readback_memory_type_))) {
    VkMappedMemoryRange memory_range = {};
    memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    memory_range.memory = occlusion_query_readback_memory_;
    memory_range.offset = 0;
    memory_range.size =
        std::min(rex::round_up(VkDeviceSize(sizeof(uint64_t) * kMaxOcclusionQueries),
                               vulkan_device->properties().nonCoherentAtomSize),
                 occlusion_query_readback_memory_size_);
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &memory_range);
  }

  const uint64_t* results = reinterpret_cast<const uint64_t*>(occlusion_query_readback_mapping_);
  uint64_t samples = NormalizeOcclusionSamples(results[host_index]);
  WriteGuestOcclusionResult(sample_count_address, samples);
  return true;
}

uint64_t VulkanCommandProcessor::NormalizeOcclusionSamples(uint64_t samples) const {
  if (samples == 0 || !texture_cache_) {
    return samples;
  }
  uint64_t scale_x = texture_cache_->draw_resolution_scale_x();
  uint64_t scale_y = texture_cache_->draw_resolution_scale_y();
  uint64_t scale = scale_x * scale_y;
  if (scale <= 1) {
    return samples;
  }
  return (samples + (scale >> 1)) / scale;
}

void VulkanCommandProcessor::WriteGuestOcclusionResult(uint32_t sample_count_address,
                                                       uint64_t samples) {
  auto* sample_counts =
      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(sample_count_address);
  if (!sample_counts) {
    return;
  }
  // A counter snapshot: the guest subtracts 32-bit values, so wrap, don't clamp.
  uint32_t clamped = uint32_t(samples);
  sample_counts->Total_A = clamped;
  sample_counts->Total_B = 0;
  sample_counts->ZPass_A = clamped;
  sample_counts->ZPass_B = 0;
  sample_counts->ZFail_A = 0;
  sample_counts->ZFail_B = 0;
  sample_counts->StencilFail_A = 0;
  sample_counts->StencilFail_B = 0;
}

void VulkanCommandProcessor::CheckSubmissionFenceAndDeviceLoss(uint64_t await_submission) {
  // Only report once, no need to retry a wait that won't succeed anyway.
  if (device_lost_) {
    return;
  }

  if (await_submission >= GetCurrentSubmission()) {
    if (submission_open_) {
      EndSubmission(false);
    }
    // A submission won't be ended if it hasn't been started, or if ending
    // has failed - clamp the index.
    await_submission = GetCurrentSubmission() - 1;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  size_t fences_total = submissions_in_flight_fences_.size();
  size_t fences_awaited = 0;
  const auto wait_start = std::chrono::steady_clock::now();
  auto waited_ms = [&wait_start] {
    return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - wait_start)
                       .count());
  };
  if (submission_thread_) {
    // Only the fences of submissions the thread has submitted can be waited for.
    if (await_submission > submission_completed_) {
      fh1_frame_cost::Timer cost_timer(fh1_frame_cost::Current().gpu_wait_ms);
      AwaitSubmissionThread(await_submission);
    }
    if (submission_thread_failed_.load(std::memory_order_acquire)) {
      if (submission_thread_device_lost_.load(std::memory_order_acquire)) {
        REXGPU_ERROR("Vulkan device lost (threaded submission)");
      }
      device_lost_ = true;
      if (graphics_system_) {
        graphics_system_->OnHostGpuLossFromAnyThread(true);
      }
      return;
    }
    uint64_t submitted = submission_thread_submitted_.load(std::memory_order_acquire);
    fences_total = std::min(fences_total, size_t(submitted > submission_completed_
                                                     ? submitted - submission_completed_
                                                     : 0));
  }
  if (await_submission > submission_completed_) {
    // Await in a blocking way if requested.
    // TODO(Triang3l): Await only one fence. "Fence signal operations that are
    // defined by vkQueueSubmit additionally include in the first
    // synchronization scope all commands that occur earlier in submission
    // order."
    VkResult wait_result;
    {
      fh1_frame_cost::Timer cost_timer(fh1_frame_cost::Current().gpu_wait_ms);
      wait_result =
          dfn.vkWaitForFences(device, uint32_t(await_submission - submission_completed_),
                              submissions_in_flight_fences_.data(), VK_TRUE, UINT64_MAX);
    }
    if (wait_result == VK_SUCCESS) {
      fences_awaited += await_submission - submission_completed_;
      const int64_t ms = waited_ms();
      // FH1 (#268): what the submissions waited for held, and how full the device-local
      // memory was.
      const bool log_slow =
          ms >= kSlowSubmissionWaitMs ||
          fh1_submission_watch_state.ShouldLogWait(ms, Fh1NowMs());
      if (log_slow) {
        std::string held;
        const int64_t now_ms = Fh1NowMs();
        for (uint64_t s = submission_completed_ + 1; s <= await_submission; ++s) {
          if (const auto* r = fh1_submission_watch_state.Find(s)) {
            held += fmt::format("{}{}: {} draws, submitted {} ms ago", held.empty() ? "" : "; ",
                                s, r->draws, now_ms - r->submitted_ms);
          }
        }
        const fh1_device_memory::State::Budget memory = fh1_device_memory::Get().Query();
        const std::string context = fmt::format(
            " ({}; device-local {} of {} MB, system {} of {} MB; {} slow waits since the last "
            "report, {} submissions split)",
            held.empty() ? std::string("no record") : held, memory.usage >> 20,
            memory.budget >> 20, memory.system_usage >> 20, memory.system_budget >> 20,
            fh1_submission_watch_state.TakeSlowWaits(), fh1_submission_splits);
        REXGPU_WARN("Vulkan: waited {} ms for submission {} to finish on the GPU{}", ms,
                    await_submission, context);
      }
    } else {
      REXGPU_ERROR("Failed to await submission completion Vulkan fences (VkResult {}, after {} ms)",
                   int(wait_result), waited_ms());
      if (wait_result == VK_ERROR_DEVICE_LOST) {
        device_lost_ = true;
      }
    }
  }
  // Check how far into the submissions the GPU currently is, in order because
  // submission themselves can be executed out of order, but Xenia serializes
  // that for simplicity.
  while (fences_awaited < fences_total) {
    VkResult fence_status =
        dfn.vkWaitForFences(device, 1, &submissions_in_flight_fences_[fences_awaited], VK_TRUE, 0);
    if (TestGpuLossFrame() && frame_current_ >= TestGpuLossFrame()) {
      fence_status = VK_ERROR_DEVICE_LOST;
    }
    if (fence_status != VK_SUCCESS) {
      if (fence_status == VK_ERROR_DEVICE_LOST) {
        // The driver lost the device (a GPU reset, or the driver gave up on it); without this
        // line the abort that follows leaves nothing in the log.
        REXGPU_ERROR(
            "Vulkan device lost: the driver returned VK_ERROR_DEVICE_LOST checking submission {} "
            "(frame {}, {} submission(s) in flight)",
            submission_completed_ + fences_awaited + 1, frame_current_,
            fences_total - fences_awaited);
        device_lost_ = true;
      }
      break;
    }
    ++fences_awaited;
  }
  if (device_lost_) {
    if (graphics_system_) {
      graphics_system_->OnHostGpuLossFromAnyThread(true);
    }
    return;
  }
  if (!fences_awaited) {
    // Not updated - no need to reclaim or download things.
    return;
  }
  // Reclaim fences.
  fences_free_.reserve(fences_free_.size() + fences_awaited);
  auto submissions_in_flight_fences_awaited_end = submissions_in_flight_fences_.cbegin();
  std::advance(submissions_in_flight_fences_awaited_end, fences_awaited);
  fences_free_.insert(fences_free_.cend(), submissions_in_flight_fences_.cbegin(),
                      submissions_in_flight_fences_awaited_end);
  submissions_in_flight_fences_.erase(submissions_in_flight_fences_.cbegin(),
                                      submissions_in_flight_fences_awaited_end);
  submission_completed_ += fences_awaited;

  // Reclaim semaphores.
  while (!submissions_in_flight_semaphores_.empty()) {
    const auto& semaphore_submission = submissions_in_flight_semaphores_.front();
    if (semaphore_submission.first > submission_completed_) {
      break;
    }
    semaphores_free_.push_back(semaphore_submission.second);
    submissions_in_flight_semaphores_.pop_front();
  }

  // Reclaim command pools.
  while (!command_buffers_submitted_.empty()) {
    const auto& command_buffer_pair = command_buffers_submitted_.front();
    if (command_buffer_pair.first > submission_completed_) {
      break;
    }
    command_buffers_writable_.push_back(command_buffer_pair.second);
    command_buffers_submitted_.pop_front();
  }

  shared_memory_->CompletedSubmissionUpdated();

  primitive_processor_->CompletedSubmissionUpdated();

  render_target_cache_->CompletedSubmissionUpdated();
  if (fh1_other_render_targets_) fh1_other_render_targets_->CompletedSubmissionUpdated();

  texture_cache_->CompletedSubmissionUpdated(submission_completed_);
  if (fh1_other_textures_) fh1_other_textures_->CompletedSubmissionUpdated(submission_completed_);

  // Destroy objects scheduled for destruction.
  while (!destroy_framebuffers_.empty()) {
    const auto& destroy_pair = destroy_framebuffers_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkDestroyFramebuffer(device, destroy_pair.second, nullptr);
    destroy_framebuffers_.pop_front();
  }
  while (!destroy_image_views_.empty()) {
    const auto& destroy_pair = destroy_image_views_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkDestroyImageView(device, destroy_pair.second, nullptr);
    destroy_image_views_.pop_front();
  }
  while (!destroy_buffers_.empty()) {
    const auto& destroy_pair = destroy_buffers_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkDestroyBuffer(device, destroy_pair.second, nullptr);
    destroy_buffers_.pop_front();
  }
  while (!destroy_images_.empty()) {
    const auto& destroy_pair = destroy_images_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkDestroyImage(device, destroy_pair.second, nullptr);
    destroy_images_.pop_front();
  }
  while (!destroy_memory_.empty()) {
    const auto& destroy_pair = destroy_memory_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkFreeMemory(device, destroy_pair.second, nullptr);
    destroy_memory_.pop_front();
  }
}

bool VulkanCommandProcessor::Fh1SkipMustHoldFrame(bool has_pixel_shader, uint32_t color_mask,
                                                  uint32_t host_vertex_count) {
  // FH1 (#167): a skipped draw whose absence would leave stale or wrong contents on screen, not
  // just an object missing for a few frames, keeps its frame from being shown past the hold.
  // The kinds are counted for the pipeline summary line (skipped draws, not frames).
  auto& stats = fh1_pipeline_lookahead::stats;
  // VR true stereo: the right eye replays the left eye's draws and may find the pipeline built,
  // which would show the object in one eye only.
  if (fh1_stereo_state_ != Fh1StereoState::kOff) {
    ++stats.skipped_stereo;
    return true;
  }
  // Depth or stencil only (shadow casters, Z, stencil masks): later passes read what it writes.
  if (!has_pixel_shader || !color_mask) {
    ++stats.skipped_depth;
    return true;
  }
  // A screen-space pass (a full-screen effect or composite, a movie frame, a HUD element): it
  // replaces what its target holds instead of adding to it.
  constexpr uint32_t kScreenPassVertices = 6;
  if (host_vertex_count <= kScreenPassVertices) {
    ++stats.skipped_screen_passes;
    return true;
  }
  // The front buffer and UI target (1280 wide, no MSAA, 10-bit color), and the native HUD.
  const RegisterFile& regs = *register_file_;
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  const auto color = regs.Get<reg::RB_COLOR_INFO>();
  if (fh1_native_hud_active_ ||
      (surface.surface_pitch == 1280 && surface.msaa_samples == xenos::MsaaSamples::k1X &&
       (color.color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
        color.color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10))) {
    ++stats.skipped_front_buffer;
    return true;
  }
  return false;
}

int32_t VulkanCommandProcessor::Fh1PipelineWaitMs() const {
  // Synchronous creation (async_shader_compilation off, or sync_pipeline_creation for a render
  // made once) records draws with the pipelines it waited for: every submission end waits
  // until all are built, as before.
  const int32_t budget = REXCVAR_GET(vulkan_async_frame_end_wait_ms);
  if (!REXCVAR_GET(async_shader_compilation) || REXCVAR_GET(sync_pipeline_creation) ||
      budget < 0) {
    return -1;
  }
  // FH1 (#167): what is left of the frame's budget (swap to swap), shared by every submission
  // end of the frame (draw-limit splits, readbacks) and by both pipeline caches.
  return std::max(int32_t(fh1_pipeline_wait_left_ms_), int32_t(0));
}

bool VulkanCommandProcessor::CanEndSubmissionImmediately() const {
  // FH1 (#167): pipelines being built keep the submission open where its end would wait for them.
  const bool waits = Fh1PipelineWaitMs() != 0;
  auto creating = [](const VulkanPipelineCache* cache) {
    return cache && cache->IsCreatingPipelines();
  };
  return !submission_open_ ||
         (!scratch_buffer_used_ &&
          !(waits && (creating(pipeline_cache_.get()) || creating(fh1_other_pipelines_.get()))));
}

bool VulkanCommandProcessor::BeginSubmission(bool is_guest_command) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (device_lost_) {
    return false;
  }

  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return true;
  }

  // Check the fence - needed for all kinds of submissions (to reclaim transient
  // resources early) and specifically for frames (not to queue too many), and
  // await the availability of the current frame. Also check whether the device
  // is still available, and whether the await was successful.
  uint64_t await_submission = 0;
  if (is_opening_frame) {
    const int32_t configured_depth = REXCVAR_GET(vulkan_max_frames_in_flight);
    const uint32_t queue_depth =
        configured_depth > 0
            ? std::min(uint32_t(configured_depth), kMaxFramesInFlight)
            : (GetVulkanDevice()->properties().vendorID == 0x10DE ? 2u : kMaxFramesInFlight);
    // Keep the three-slot resource ring. Waiting on a more recent closed frame
    // limits CPU lead without reusing resources before their fences complete.
    // On NVIDIA, two queued frames shorten first-readback waits while retaining
    // the copy and the existing readback-cache lifetime.
    const uint64_t wait_slot =
        (frame_current_ + kMaxFramesInFlight - queue_depth) % kMaxFramesInFlight;
    await_submission = closed_frame_submissions_[wait_slot];
  }
  CheckSubmissionFenceAndDeviceLoss(await_submission);
  if (device_lost_ || submission_completed_ < await_submission) {
    return false;
  }

  if (is_opening_frame) {
    // Update the completed frame index, also obtaining the actual completed
    // frame number (since the CPU may be actually less than 3 frames behind)
    // before reclaiming resources tracked with the frame number.
    frame_completed_ = std::max(frame_current_, uint64_t(kMaxFramesInFlight)) - kMaxFramesInFlight;
    for (uint64_t frame = frame_completed_ + 1; frame < frame_current_; ++frame) {
      if (closed_frame_submissions_[frame % kMaxFramesInFlight] > submission_completed_) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  if (!submission_open_) {
    submission_open_ = true;
    fh1_draws_in_submission = 0;

    // Start a new deferred command buffer - will submit it to the real one in
    // the end of the submission (when async pipeline object creation requests
    // are fulfilled).
    deferred_command_buffer_.Reset();

    // Reset cached state of the command buffer.
    dynamic_viewport_update_needed_ = true;
    dynamic_scissor_update_needed_ = true;
    dynamic_depth_bias_update_needed_ = true;
    dynamic_blend_constants_update_needed_ = true;
    dynamic_stencil_compare_mask_front_update_needed_ = true;
    dynamic_stencil_compare_mask_back_update_needed_ = true;
    dynamic_stencil_write_mask_front_update_needed_ = true;
    dynamic_stencil_write_mask_back_update_needed_ = true;
    dynamic_stencil_reference_front_update_needed_ = true;
    dynamic_stencil_reference_back_update_needed_ = true;
    current_render_pass_ = VK_NULL_HANDLE;
    current_framebuffer_ = nullptr;
    in_render_pass_ = false;
    hoist_render_pass_ = false;
    current_guest_graphics_pipeline_ = VK_NULL_HANDLE;
    current_external_graphics_pipeline_ = VK_NULL_HANDLE;
    current_external_compute_pipeline_ = VK_NULL_HANDLE;
    current_guest_graphics_pipeline_layout_ = nullptr;
    current_graphics_descriptor_sets_bound_up_to_date_ = 0;

    primitive_processor_->BeginSubmission();

    texture_cache_->BeginSubmission(GetCurrentSubmission());
    if (fh1_other_textures_) fh1_other_textures_->BeginSubmission(GetCurrentSubmission());
  }

  if (is_opening_frame) {
    frame_open_ = true;
    // FH1 (#45, patch 0360): the packet loop stores the plain registers itself (the same
    // stores WriteRegistersFromMem and WriteRegister do for them).
    fh1_plain_registers_ = REXCVAR_GET(gpu_plain_register_ranges) ? plain_registers.bits() : nullptr;
    // FH1 (#45, patch 0362).
    Shader::fh1_translation_memo_enabled = REXCVAR_GET(gpu_shader_translation_memo);
    Shader::fh1_translation_memo_verify = REXCVAR_GET(gpu_shader_translation_memo_verify);
    if (fh1_other_render_targets_) Fh1SelectNativeHud(false);
    if (fh1_dlss_) fh1_dlss_->BeginFrame(frame_current_);
    fh1_scene_composited_ = false;
    fh1_seed_pending_ = false;
    fh1_scene_base_ = UINT32_MAX;
    hdr_scene_base_ = UINT32_MAX;
    hdr_capture_attempted_ = false;
    frame_used_async_placeholder_pipeline_ = false;
    // FH1 (#167): the frame's pipeline wait budget and its skipped draws.
    fh1_frame_skipped_critical_ = false;
    fh1_pipeline_wait_left_ms_ = double(std::max(REXCVAR_GET(vulkan_async_frame_end_wait_ms), 0));

    // Reset bindings that depend on transient data.
    std::memset(current_float_constant_map_vertex_, 0, sizeof(current_float_constant_map_vertex_));
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
    std::memset(current_graphics_descriptor_sets_, 0, sizeof(current_graphics_descriptor_sets_));
    current_constant_buffers_up_to_date_ = 0;
    current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
        shared_memory_and_edram_descriptor_set_;
    current_graphics_descriptor_set_values_up_to_date_ =
        UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram;

    // Reclaim pool pages - no need to do this every small submission since some
    // may be reused.
    // FIXME(Triang3l): This will result in a memory leak if the guest is not
    // presenting.
    uniform_buffer_pool_->Reclaim(frame_completed_);
    while (!single_transient_descriptors_used_.empty()) {
      const UsedSingleTransientDescriptor& used_transient_descriptor =
          single_transient_descriptors_used_.front();
      if (used_transient_descriptor.frame > frame_completed_) {
        break;
      }
      single_transient_descriptors_free_[size_t(used_transient_descriptor.layout)].push_back(
          used_transient_descriptor.set);
      single_transient_descriptors_used_.pop_front();
    }
    while (!constants_transient_descriptors_used_.empty()) {
      const std::pair<uint64_t, VkDescriptorSet>& used_transient_descriptor =
          constants_transient_descriptors_used_.front();
      if (used_transient_descriptor.first > frame_completed_) {
        break;
      }
      constants_transient_descriptors_free_.push_back(used_transient_descriptor.second);
      constants_transient_descriptors_used_.pop_front();
    }
    while (!texture_transient_descriptor_sets_used_.empty()) {
      const UsedTextureTransientDescriptorSet& used_transient_descriptor_set =
          texture_transient_descriptor_sets_used_.front();
      if (used_transient_descriptor_set.frame > frame_completed_) {
        break;
      }
      auto it = texture_transient_descriptor_sets_free_.find(used_transient_descriptor_set.layout);
      if (it == texture_transient_descriptor_sets_free_.end()) {
        it = texture_transient_descriptor_sets_free_
                 .emplace(std::piecewise_construct,
                          std::forward_as_tuple(used_transient_descriptor_set.layout),
                          std::forward_as_tuple())
                 .first;
      }
      it->second.push_back(used_transient_descriptor_set.set);
      texture_transient_descriptor_sets_used_.pop_front();
    }

    EvictOldReadbackBuffers(readback_buffers_);
    EvictOldReadbackBuffers(memexport_readback_buffers_);

    primitive_processor_->BeginFrame();

    texture_cache_->BeginFrame();
    if (fh1_other_textures_) fh1_other_textures_->BeginFrame();
  }

  return true;
}

bool VulkanCommandProcessor::EndSubmission(bool is_swap) {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Make sure everything needed for submitting exist.
  if (submission_open_) {
    if (fences_free_.empty()) {
      VkFenceCreateInfo fence_create_info;
      fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      fence_create_info.pNext = nullptr;
      fence_create_info.flags = 0;
      VkFence fence;
      if (dfn.vkCreateFence(device, &fence_create_info, nullptr, &fence) != VK_SUCCESS) {
        REXGPU_ERROR("Failed to create a Vulkan fence");
        // Try to submit later. Completely dropping the submission is not
        // permitted because resources would be left in an undefined state.
        return false;
      }
      fences_free_.push_back(fence);
    }
    if (!sparse_memory_binds_.empty() && semaphores_free_.empty()) {
      VkSemaphoreCreateInfo semaphore_create_info;
      semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      semaphore_create_info.pNext = nullptr;
      semaphore_create_info.flags = 0;
      VkSemaphore semaphore;
      if (dfn.vkCreateSemaphore(device, &semaphore_create_info, nullptr, &semaphore) !=
          VK_SUCCESS) {
        REXGPU_ERROR("Failed to create a Vulkan semaphore");
        return false;
      }
      semaphores_free_.push_back(semaphore);
    }
    if (command_buffers_writable_.empty()) {
      CommandBuffer command_buffer;
      VkCommandPoolCreateInfo command_pool_create_info;
      command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      command_pool_create_info.pNext = nullptr;
      command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      command_pool_create_info.queueFamilyIndex = vulkan_device->queue_family_graphics_compute();
      if (dfn.vkCreateCommandPool(device, &command_pool_create_info, nullptr,
                                  &command_buffer.pool) != VK_SUCCESS) {
        REXGPU_ERROR("Failed to create a Vulkan command pool");
        return false;
      }
      VkCommandBufferAllocateInfo command_buffer_allocate_info;
      command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      command_buffer_allocate_info.pNext = nullptr;
      command_buffer_allocate_info.commandPool = command_buffer.pool;
      command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      // Reserve the continuation before a session can enable MetalFX live.
      const bool native = REX_PLATFORM_MAC;
      command_buffer_allocate_info.commandBufferCount = native ? 2 : 1;
      VkCommandBuffer buffers[2]{};
      if (dfn.vkAllocateCommandBuffers(device, &command_buffer_allocate_info,
                                       buffers) != VK_SUCCESS) {
        REXGPU_ERROR("Failed to allocate a Vulkan command buffer");
        dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
        return false;
      }
      command_buffer.buffer = buffers[0];
      command_buffer.continuation = buffers[1];
      command_buffers_writable_.push_back(command_buffer);
    }
  }

  bool is_closing_frame = is_swap && frame_open_;

  if (is_closing_frame) {
    texture_cache_->EndFrame();
    if (fh1_other_textures_) fh1_other_textures_->EndFrame();

    primitive_processor_->EndFrame();
  }

  if (submission_open_) {
    assert_false(scratch_buffer_used_);

    if (active_occlusion_query_.valid && occlusion_query_pool_ != VK_NULL_HANDLE) {
      deferred_command_buffer_.CmdVkEndQuery(occlusion_query_pool_,
                                             active_occlusion_query_.host_index);
      active_occlusion_query_ = {};
    }

    EndRenderPass();

    // FH1 (#167): both caches wait within the frame's one budget.
    const int32_t pipeline_wait_ms = Fh1PipelineWaitMs();
    double pipeline_waited_ms = pipeline_cache_->EndSubmission(pipeline_wait_ms);
    if (fh1_other_pipelines_) {
      pipeline_waited_ms += fh1_other_pipelines_->EndSubmission(
          pipeline_wait_ms < 0 ? -1
                               : std::max(int32_t(pipeline_wait_ms - pipeline_waited_ms), 0));
    }
    if (pipeline_wait_ms >= 0) fh1_pipeline_wait_left_ms_ -= pipeline_waited_ms;

    render_target_cache_->EndSubmission();
    if (fh1_other_render_targets_) fh1_other_render_targets_->EndSubmission();

    primitive_processor_->EndSubmission();

    shared_memory_->EndSubmission();

    uniform_buffer_pool_->FlushWrites();

    // Submit sparse binds earlier, before executing the deferred command
    // buffer, to reduce latency.
    if (!sparse_memory_binds_.empty()) {
      sparse_buffer_bind_infos_temp_.clear();
      sparse_buffer_bind_infos_temp_.reserve(sparse_buffer_binds_.size());
      for (const SparseBufferBind& sparse_buffer_bind : sparse_buffer_binds_) {
        VkSparseBufferMemoryBindInfo& sparse_buffer_bind_info =
            sparse_buffer_bind_infos_temp_.emplace_back();
        sparse_buffer_bind_info.buffer = sparse_buffer_bind.buffer;
        sparse_buffer_bind_info.bindCount = sparse_buffer_bind.bind_count;
        sparse_buffer_bind_info.pBinds =
            sparse_memory_binds_.data() + sparse_buffer_bind.bind_offset;
      }
      assert_false(semaphores_free_.empty());
      VkSemaphore bind_sparse_semaphore = semaphores_free_.back();
      VkBindSparseInfo bind_sparse_info;
      bind_sparse_info.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
      bind_sparse_info.pNext = nullptr;
      bind_sparse_info.waitSemaphoreCount = 0;
      bind_sparse_info.pWaitSemaphores = nullptr;
      bind_sparse_info.bufferBindCount = uint32_t(sparse_buffer_bind_infos_temp_.size());
      bind_sparse_info.pBufferBinds =
          !sparse_buffer_bind_infos_temp_.empty() ? sparse_buffer_bind_infos_temp_.data() : nullptr;
      bind_sparse_info.imageOpaqueBindCount = 0;
      bind_sparse_info.pImageOpaqueBinds = nullptr;
      bind_sparse_info.imageBindCount = 0;
      bind_sparse_info.pImageBinds = 0;
      bind_sparse_info.signalSemaphoreCount = 1;
      bind_sparse_info.pSignalSemaphores = &bind_sparse_semaphore;
      VkResult bind_sparse_result;
      {
        ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
            vulkan_device->AcquireQueue(vulkan_device->queue_family_sparse_binding(), 0);
        bind_sparse_result =
            dfn.vkQueueBindSparse(queue_acquisition.queue(), 1, &bind_sparse_info, VK_NULL_HANDLE);
      }
      if (bind_sparse_result != VK_SUCCESS) {
        REXGPU_ERROR("Failed to submit Vulkan sparse binds");
        return false;
      }
      current_submission_wait_semaphores_.push_back(bind_sparse_semaphore);
      semaphores_free_.pop_back();
      current_submission_wait_stage_masks_.push_back(sparse_bind_wait_stage_mask_);
      sparse_bind_wait_stage_mask_ = 0;
      sparse_buffer_binds_.clear();
      sparse_memory_binds_.clear();
    }

    SubmitBarriers(true);

    fh1_submission_watch_state.Submitted(GetCurrentSubmission(), fh1_draws_in_submission,
                                         Fh1NowMs());

    assert_false(command_buffers_writable_.empty());
    CommandBuffer command_buffer = command_buffers_writable_.back();
    assert_false(fences_free_.empty());
    VkFence fence = fences_free_.back();
    if (submission_thread_) {
      if (dfn.vkResetFences(device, 1, &fence) != VK_SUCCESS) {
        REXGPU_ERROR("Failed to reset a Vulkan submission fence");
        return false;
      }
      SubmissionJob job;
      job.submission = GetCurrentSubmission();
      job.command_buffer = command_buffer;
      job.fence = fence;
      job.wait_semaphores = current_submission_wait_semaphores_;
      job.wait_stage_masks = current_submission_wait_stage_masks_;
      {
        std::lock_guard<std::mutex> lock(submission_thread_mutex_);
        if (!submission_thread_free_streams_.empty()) {
          job.streams = std::move(submission_thread_free_streams_.back());
          submission_thread_free_streams_.pop_back();
        }
      }
      deferred_command_buffer_.TakeStreams(job.streams);
      {
        std::lock_guard<std::mutex> lock(submission_thread_mutex_);
        submission_thread_jobs_.push_back(std::move(job));
      }
      submission_thread_request_cond_.notify_one();
    } else if (fh1_dlss_ && fh1_dlss_->UsesMetalFx()) {
      if (dfn.vkResetFences(device, 1, &fence) != VK_SUCCESS) return false;
      DeferredCommandBuffer::Streams streams;
      deferred_command_buffer_.TakeStreams(streams);
      const VkResult result = ReplayAndSubmit(command_buffer, streams, fence,
          current_submission_wait_semaphores_, current_submission_wait_stage_masks_);
      if (result != VK_SUCCESS) {
        REXGPU_ERROR("MetalFX: submission failed ({})", int(result));
        device_lost_ = true; return false;
      }
    } else {
      if (dfn.vkResetCommandPool(device, command_buffer.pool, 0) != VK_SUCCESS) {
        REXGPU_ERROR("Failed to reset a Vulkan command pool");
        return false;
      }
      VkCommandBufferBeginInfo command_buffer_begin_info;
      command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      command_buffer_begin_info.pNext = nullptr;
      command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      command_buffer_begin_info.pInheritanceInfo = nullptr;
      if (dfn.vkBeginCommandBuffer(command_buffer.buffer, &command_buffer_begin_info) !=
          VK_SUCCESS) {
        REXGPU_ERROR("Failed to begin a Vulkan command buffer");
        return false;
      }
      deferred_command_buffer_.Execute(command_buffer.buffer);
      if (dfn.vkEndCommandBuffer(command_buffer.buffer) != VK_SUCCESS) {
        REXGPU_ERROR("Failed to end a Vulkan command buffer");
        return false;
      }

      VkSubmitInfo submit_info;
      submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit_info.pNext = nullptr;
      if (!current_submission_wait_semaphores_.empty()) {
        submit_info.waitSemaphoreCount = uint32_t(current_submission_wait_semaphores_.size());
        submit_info.pWaitSemaphores = current_submission_wait_semaphores_.data();
        submit_info.pWaitDstStageMask = current_submission_wait_stage_masks_.data();
      } else {
        submit_info.waitSemaphoreCount = 0;
        submit_info.pWaitSemaphores = nullptr;
        submit_info.pWaitDstStageMask = nullptr;
      }
      submit_info.commandBufferCount = 1;
      submit_info.pCommandBuffers = &command_buffer.buffer;
      submit_info.signalSemaphoreCount = 0;
      submit_info.pSignalSemaphores = nullptr;
      if (dfn.vkResetFences(device, 1, &fence) != VK_SUCCESS) {
        REXGPU_ERROR("Failed to reset a Vulkan submission fence");
        return false;
      }
      VkResult submit_result;
      {
        ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
            vulkan_device->AcquireQueue(vulkan_device->queue_family_graphics_compute(), 0);
        submit_result = dfn.vkQueueSubmit(queue_acquisition.queue(), 1, &submit_info, fence);
      }
      if (submit_result != VK_SUCCESS) {
        REXGPU_ERROR("Failed to submit a Vulkan command buffer");
        if (submit_result == VK_ERROR_DEVICE_LOST && !device_lost_) {
          device_lost_ = true;
          if (graphics_system_) {
            graphics_system_->OnHostGpuLossFromAnyThread(true);
          }
        }
        return false;
      }
    }
    uint64_t submission_current = GetCurrentSubmission();
    current_submission_wait_stage_masks_.clear();
    for (VkSemaphore semaphore : current_submission_wait_semaphores_) {
      submissions_in_flight_semaphores_.emplace_back(submission_current, semaphore);
    }
    current_submission_wait_semaphores_.clear();
    command_buffers_submitted_.emplace_back(submission_current, command_buffer);
    command_buffers_writable_.pop_back();
    // Increments the current submission number, going to the next submission.
    submissions_in_flight_fences_.push_back(fence);
    fences_free_.pop_back();

    submission_open_ = false;
  }

  if (is_closing_frame) {
    if (REXCVAR_GET(clear_memory_page_state) && shared_memory_) {
      shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
    }
    frame_open_ = false;
    // Submission already closed now, so minus 1.
    closed_frame_submissions_[(frame_current_++) % kMaxFramesInFlight] = GetCurrentSubmission() - 1;

    if (!cache_clear_requested_) {
      TrimRenderTargetsIfNeeded();
    }
    Fh1EndFrameCost(
        frame_current_,
        render_target_cache_ ? render_target_cache_->render_target_memory_bytes() : 0,
        render_target_cache_ ? render_target_cache_->render_target_count() : 0,
        [this](fh1_frame_cost::Snapshot& snapshot) {
          for (const auto* buffers : {&readback_buffers_, &memexport_readback_buffers_}) {
            for (const auto& entry : *buffers) {
              ++snapshot.readback_buffers;
              snapshot.readback_buffer_bytes +=
                  uint64_t(entry.second.sizes[0]) + uint64_t(entry.second.sizes[1]);
            }
          }
          if (memory_) {
            for (uint32_t base : {UINT32_C(0x00000000), UINT32_C(0x40000000),
                                  UINT32_C(0x80000000), UINT32_C(0x90000000)}) {
              if (const memory::BaseHeap* heap = memory_->LookupHeap(base)) {
                snapshot.guest_virtual_bytes +=
                    uint64_t(heap->reserved_page_count()) * heap->page_size();
              }
            }
            if (const memory::VirtualHeap* physical = memory_->GetPhysicalHeap()) {
              snapshot.guest_physical_bytes =
                  uint64_t(physical->reserved_page_count()) * physical->page_size();
            }
          }
          const fh1_device_memory::State::Budget budget = fh1_device_memory::Get().Query();
          snapshot.device_local_usage = budget.usage;
          snapshot.device_local_budget = budget.budget;
          snapshot.system_usage = budget.system_usage;
          const ui::vulkan::VulkanDevice::Properties& properties =
              GetVulkanDevice()->properties();
          ReadSensors(properties.vendorID, properties.deviceID, snapshot);
          // FH1 (#45): how many draws kept the previous render target setup, once a minute.
          for (VulkanRenderTargetCache* rt_cache :
               {render_target_cache_.get(), fh1_other_render_targets_.get()}) {
            if (!rt_cache) {
              continue;
            }
            uint64_t updates, reused, mismatches;
            rt_cache->TakeUpdateReuseStats(updates, reused, mismatches);
            if (updates) {
              REXGPU_INFO(
                  "Render target update reuse: {} of {} draws ({:.1f}%){}", reused, updates,
                  100.0 * double(reused) / double(updates),
                  REXCVAR_GET(vulkan_render_target_update_reuse_verify)
                      ? fmt::format(", verified, {} differed", mismatches)
                      : std::string());
            }
          }
        });
    if (cache_clear_requested_ && AwaitAllQueueOperationsCompletion()) {
      cache_clear_requested_ = false;

      DestroyScratchBuffer();

      for (SwapFramebuffer& swap_framebuffer : swap_framebuffers_) {
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                               swap_framebuffer.framebuffer);
      }

      assert_true(command_buffers_submitted_.empty());
      for (const CommandBuffer& command_buffer : command_buffers_writable_) {
        dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
      }
      command_buffers_writable_.clear();

      ClearTransientDescriptorPools();

      uniform_buffer_pool_->ClearCache();

      texture_cache_->ClearCache();
      if (fh1_other_textures_) fh1_other_textures_->ClearCache();

      render_target_cache_->ClearCache();
      if (fh1_other_render_targets_) fh1_other_render_targets_->ClearCache();

      // Not clearing the pipeline layouts and the descriptor set layouts as
      // they're referenced by pipelines, which are not destroyed.

      primitive_processor_->ClearCache();

      shared_memory_->ClearCache();
    }
  }

  if (is_closing_frame) Fh1ApplyGraphicsRequest();
  return true;
}

void VulkanCommandProcessor::ClearTransientDescriptorPools() {
  texture_transient_descriptor_sets_free_.clear();
  texture_transient_descriptor_sets_used_.clear();
  transient_descriptor_allocator_textures_.Reset();

  constants_transient_descriptors_free_.clear();
  constants_transient_descriptors_used_.clear();
  for (std::vector<VkDescriptorSet>& transient_descriptors_free :
       single_transient_descriptors_free_) {
    transient_descriptors_free.clear();
  }
  single_transient_descriptors_used_.clear();
  transient_descriptor_allocator_storage_image_.Reset();
  transient_descriptor_allocator_storage_buffer_.Reset();
  transient_descriptor_allocator_uniform_buffer_.Reset();
}

void VulkanCommandProcessor::SplitPendingBarrier() {
  size_t pending_buffer_memory_barrier_count = pending_barriers_buffer_memory_barriers_.size();
  size_t pending_image_memory_barrier_count = pending_barriers_image_memory_barriers_.size();
  if (!current_pending_barrier_.src_stage_mask && !current_pending_barrier_.dst_stage_mask &&
      current_pending_barrier_.buffer_memory_barriers_offset >=
          pending_buffer_memory_barrier_count &&
      current_pending_barrier_.image_memory_barriers_offset >= pending_image_memory_barrier_count) {
    return;
  }
  pending_barriers_.emplace_back(current_pending_barrier_);
  current_pending_barrier_.src_stage_mask = 0;
  current_pending_barrier_.dst_stage_mask = 0;
  current_pending_barrier_.buffer_memory_barriers_offset = pending_buffer_memory_barrier_count;
  current_pending_barrier_.image_memory_barriers_offset = pending_image_memory_barrier_count;
}

void VulkanCommandProcessor::DestroyScratchBuffer() {
  assert_false(scratch_buffer_used_);

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  scratch_buffer_last_usage_submission_ = 0;
  scratch_buffer_last_access_mask_ = 0;
  scratch_buffer_last_stage_mask_ = 0;
  scratch_buffer_size_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, scratch_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, scratch_buffer_memory_);
}

bool VulkanCommandProcessor::EnsureSwapFxaaSourceImage(uint32_t width, uint32_t height) {
  assert_true(submission_open_);
  if (!width || !height) {
    return false;
  }
  if (swap_fxaa_source_image_ != VK_NULL_HANDLE && swap_fxaa_source_image_width_ == width &&
      swap_fxaa_source_image_height_ == height) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  if (swap_fxaa_source_image_ != VK_NULL_HANDLE) {
    const uint64_t destroy_submission = swap_fxaa_source_image_submission_;
    const uint64_t deferred_destroy_submission = GetCurrentSubmission();
    if (submission_completed_ >= destroy_submission) {
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device,
                                             swap_fxaa_source_image_view_);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, swap_fxaa_source_image_);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                             swap_fxaa_source_image_memory_);
    } else {
      if (swap_fxaa_source_image_view_ != VK_NULL_HANDLE) {
        destroy_image_views_.emplace_back(deferred_destroy_submission,
                                          swap_fxaa_source_image_view_);
        swap_fxaa_source_image_view_ = VK_NULL_HANDLE;
      }
      if (swap_fxaa_source_image_ != VK_NULL_HANDLE) {
        destroy_images_.emplace_back(deferred_destroy_submission, swap_fxaa_source_image_);
        swap_fxaa_source_image_ = VK_NULL_HANDLE;
      }
      if (swap_fxaa_source_image_memory_ != VK_NULL_HANDLE) {
        destroy_memory_.emplace_back(deferred_destroy_submission, swap_fxaa_source_image_memory_);
        swap_fxaa_source_image_memory_ = VK_NULL_HANDLE;
      }
    }
  }
  swap_fxaa_source_image_width_ = 0;
  swap_fxaa_source_image_height_ = 0;
  swap_fxaa_source_image_submission_ = 0;
  swap_fxaa_source_stage_mask_ = 0;
  swap_fxaa_source_access_mask_ = 0;
  swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImageCreateInfo image_create_info;
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.pNext = nullptr;
  image_create_info.flags = 0;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  image_create_info.extent.width = width;
  image_create_info.extent.height = height;
  image_create_info.extent.depth = 1;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.queueFamilyIndexCount = 0;
  image_create_info.pQueueFamilyIndices = nullptr;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_create_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
          swap_fxaa_source_image_, swap_fxaa_source_image_memory_)) {
    REXGPU_ERROR("Failed to create the FXAA source image");
    return false;
  }

  VkImageViewCreateInfo image_view_create_info;
  image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  image_view_create_info.pNext = nullptr;
  image_view_create_info.flags = 0;
  image_view_create_info.image = swap_fxaa_source_image_;
  image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  image_view_create_info.format = image_create_info.format;
  image_view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &image_view_create_info, nullptr,
                            &swap_fxaa_source_image_view_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the FXAA source image view");
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, swap_fxaa_source_image_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           swap_fxaa_source_image_memory_);
    return false;
  }

  swap_fxaa_source_image_width_ = width;
  swap_fxaa_source_image_height_ = height;
  swap_fxaa_source_image_submission_ = 0;
  swap_fxaa_source_stage_mask_ = 0;
  swap_fxaa_source_access_mask_ = 0;
  swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  return true;
}

void VulkanCommandProcessor::DestroySwapFxaaSourceImage() {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device,
                                         swap_fxaa_source_image_view_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, swap_fxaa_source_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, swap_fxaa_source_image_memory_);
  swap_fxaa_source_image_width_ = 0;
  swap_fxaa_source_image_height_ = 0;
  swap_fxaa_source_image_submission_ = 0;
  swap_fxaa_source_stage_mask_ = 0;
  swap_fxaa_source_access_mask_ = 0;
  swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VulkanCommandProcessor::UpdateDynamicState(const draw_util::ViewportInfo& viewport_info,
                                                bool primitive_polygonal,
                                                reg::RB_DEPTHCONTROL normalized_depth_control) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  uint32_t draw_resolution_scale_x = texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y = texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;

  // Window parameters.
  // http://ftp.tku.edu.tw/NetBSD/NetBSD-current/xsrc/external/mit/xf86-video-ati/dist/src/r600_reg_auto_r6xx.h
  // See r200UpdateWindow:
  // https://github.com/freedreno/mesa/blob/master/src/mesa/drivers/dri/r200/r200_state.c
  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();

  const bool fh1_screen = Fh1NativeEnabled() && Fh1ScreenViewport(regs);
  const float native_x =
      fh1_screen ? float(REXCVAR_GET(forza_native_width)) / (1280 * draw_resolution_scale_x) : 1.0f;
  const float native_y =
      fh1_screen ? float(REXCVAR_GET(forza_native_height)) / (720 * draw_resolution_scale_y) : 1.0f;
  // Viewport.
  VkViewport viewport;
  if (viewport_info.xy_extent[0] && viewport_info.xy_extent[1]) {
    viewport.x = float(viewport_info.xy_offset[0]);
    viewport.y = float(viewport_info.xy_offset[1]);
    viewport.width = float(viewport_info.xy_extent[0]);
    viewport.height = float(viewport_info.xy_extent[1]);
  } else {
    // Vulkan viewport width must be greater than 0.0f, but the Xenia  viewport
    // may be empty for various reasons - set the viewport to outside the
    // framebuffer.
    viewport.x = -1.0f;
    viewport.y = -1.0f;
    viewport.width = 1.0f;
    viewport.height = 1.0f;
  }
  if (render_target_cache_->UsesFractionalRaster()) {
  viewport.x *= render_target_cache_->raster_scale_x();
  viewport.y *= render_target_cache_->raster_scale_y();
  viewport.width *= render_target_cache_->raster_scale_x();
  viewport.height *= render_target_cache_->raster_scale_y();
  }
  viewport.minDepth = viewport_info.z_min;
  viewport.maxDepth = viewport_info.z_max;
  viewport.x *= native_x;
  viewport.y *= native_y;
  viewport.width *= native_x;
  viewport.height *= native_y;
  SetViewport(viewport);

  // Scissor.
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  // Patch 0396: VR true stereo draws the scene only across the part of the picture the eye sees
  // (the cockpit's picture covers both eyes' fields of view; each eye's is about 70 % of it).
  if (fh1_stereo_draw_scissor_)
    fh1_vr_stereo::ClipColumns(scissor.offset[0], scissor.extent[0],
                               fh1_stereo_scissor_x_[fh1_stereo_eye_]);
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;
  if (render_target_cache_->UsesFractionalRaster()) {
  scissor.offset[0] = uint32_t(float(scissor.offset[0]) * render_target_cache_->raster_scale_x());
  scissor.offset[1] = uint32_t(float(scissor.offset[1]) * render_target_cache_->raster_scale_y());
  scissor.extent[0] = uint32_t(std::ceil(float(scissor.extent[0]) * render_target_cache_->raster_scale_x()));
  scissor.extent[1] = uint32_t(std::ceil(float(scissor.extent[1]) * render_target_cache_->raster_scale_y()));
  }
  VkRect2D scissor_rect;
  scissor_rect.offset.x = int32_t(scissor.offset[0]);
  scissor_rect.offset.y = int32_t(scissor.offset[1]);
  scissor_rect.extent.width = scissor.extent[0];
  scissor_rect.extent.height = scissor.extent[1];
  scissor_rect.offset.x = int32_t(std::floor(scissor_rect.offset.x * native_x));
  scissor_rect.offset.y = int32_t(std::floor(scissor_rect.offset.y * native_y));
  scissor_rect.extent.width = uint32_t(std::ceil(scissor_rect.extent.width * native_x));
  scissor_rect.extent.height = uint32_t(std::ceil(scissor_rect.extent.height * native_y));
  SetScissor(scissor_rect);

  if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    // Depth bias.
    float depth_bias_constant_factor, depth_bias_slope_factor;
    draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal, depth_bias_slope_factor,
                                             depth_bias_constant_factor);
    depth_bias_constant_factor *=
        regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24S8
            ? draw_util::kD3D10PolygonOffsetFactorUnorm24
            : draw_util::kD3D10PolygonOffsetFactorFloat24;
    // With non-square resolution scaling, make sure the worst-case impact is
    // reverted (slope only along the scaled axis), thus max. More bias is
    // better than less bias, because less bias means Z fighting with the
    // background is more likely.
    depth_bias_slope_factor *= xenos::kPolygonOffsetScaleSubpixelUnit *
                               float(std::max(draw_resolution_scale_x, draw_resolution_scale_y));
    // std::memcmp instead of != so in case of NaN, every draw won't be
    // invalidating it.
    dynamic_depth_bias_update_needed_ |=
        std::memcmp(&dynamic_depth_bias_constant_factor_, &depth_bias_constant_factor,
                    sizeof(float)) != 0;
    dynamic_depth_bias_update_needed_ |= std::memcmp(&dynamic_depth_bias_slope_factor_,
                                                     &depth_bias_slope_factor, sizeof(float)) != 0;
    if (dynamic_depth_bias_update_needed_) {
      dynamic_depth_bias_constant_factor_ = depth_bias_constant_factor;
      dynamic_depth_bias_slope_factor_ = depth_bias_slope_factor;
      deferred_command_buffer_.CmdVkSetDepthBias(dynamic_depth_bias_constant_factor_, 0.0f,
                                                 dynamic_depth_bias_slope_factor_);
      dynamic_depth_bias_update_needed_ = false;
    }

    // Blend constants.
    float blend_constants[] = {
        regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
    };
    dynamic_blend_constants_update_needed_ |=
        std::memcmp(dynamic_blend_constants_, blend_constants, sizeof(float) * 4) != 0;
    if (dynamic_blend_constants_update_needed_) {
      std::memcpy(dynamic_blend_constants_, blend_constants, sizeof(float) * 4);
      deferred_command_buffer_.CmdVkSetBlendConstants(dynamic_blend_constants_);
      dynamic_blend_constants_update_needed_ = false;
    }

    // Stencil masks and references.
    // Due to pretty complex conditions involving registers not directly related
    // to stencil (primitive type, culling), changing the values only when
    // stencil is actually needed. However, due to the way dynamic state needs
    // to be set in Vulkan, which doesn't take into account whether the state
    // actually has effect on drawing, and because the masks and the references
    // are always dynamic in Xenia guest pipelines, they must be set in the
    // command buffer before any draw.
    if (normalized_depth_control.stencil_enable) {
      Register stencil_ref_mask_front_reg, stencil_ref_mask_back_reg;
      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        if (GetVulkanDevice()->properties().separateStencilMaskRef) {
          stencil_ref_mask_front_reg = XE_GPU_REG_RB_STENCILREFMASK;
          stencil_ref_mask_back_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
        } else {
          // Choose the back face values only if drawing only back faces.
          stencil_ref_mask_front_reg = regs.Get<reg::PA_SU_SC_MODE_CNTL>().cull_front
                                           ? XE_GPU_REG_RB_STENCILREFMASK_BF
                                           : XE_GPU_REG_RB_STENCILREFMASK;
          stencil_ref_mask_back_reg = stencil_ref_mask_front_reg;
        }
      } else {
        stencil_ref_mask_front_reg = XE_GPU_REG_RB_STENCILREFMASK;
        stencil_ref_mask_back_reg = XE_GPU_REG_RB_STENCILREFMASK;
      }
      auto stencil_ref_mask_front = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_front_reg);
      auto stencil_ref_mask_back = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_back_reg);
      // Compare mask.
      dynamic_stencil_compare_mask_front_update_needed_ |=
          dynamic_stencil_compare_mask_front_ != stencil_ref_mask_front.stencilmask;
      dynamic_stencil_compare_mask_front_ = stencil_ref_mask_front.stencilmask;
      dynamic_stencil_compare_mask_back_update_needed_ |=
          dynamic_stencil_compare_mask_back_ != stencil_ref_mask_back.stencilmask;
      dynamic_stencil_compare_mask_back_ = stencil_ref_mask_back.stencilmask;
      // Write mask.
      dynamic_stencil_write_mask_front_update_needed_ |=
          dynamic_stencil_write_mask_front_ != stencil_ref_mask_front.stencilwritemask;
      dynamic_stencil_write_mask_front_ = stencil_ref_mask_front.stencilwritemask;
      dynamic_stencil_write_mask_back_update_needed_ |=
          dynamic_stencil_write_mask_back_ != stencil_ref_mask_back.stencilwritemask;
      dynamic_stencil_write_mask_back_ = stencil_ref_mask_back.stencilwritemask;
      // Reference.
      dynamic_stencil_reference_front_update_needed_ |=
          dynamic_stencil_reference_front_ != stencil_ref_mask_front.stencilref;
      dynamic_stencil_reference_front_ = stencil_ref_mask_front.stencilref;
      dynamic_stencil_reference_back_update_needed_ |=
          dynamic_stencil_reference_back_ != stencil_ref_mask_back.stencilref;
      dynamic_stencil_reference_back_ = stencil_ref_mask_back.stencilref;
    }
    // Using VK_STENCIL_FACE_FRONT_AND_BACK for higher safety when running on
    // the Vulkan portability subset without separateStencilMaskRef.
    if (dynamic_stencil_compare_mask_front_update_needed_ ||
        dynamic_stencil_compare_mask_back_update_needed_) {
      if (dynamic_stencil_compare_mask_front_ == dynamic_stencil_compare_mask_back_) {
        deferred_command_buffer_.CmdVkSetStencilCompareMask(VK_STENCIL_FACE_FRONT_AND_BACK,
                                                            dynamic_stencil_compare_mask_front_);
      } else {
        if (dynamic_stencil_compare_mask_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilCompareMask(VK_STENCIL_FACE_FRONT_BIT,
                                                              dynamic_stencil_compare_mask_front_);
        }
        if (dynamic_stencil_compare_mask_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilCompareMask(VK_STENCIL_FACE_BACK_BIT,
                                                              dynamic_stencil_compare_mask_back_);
        }
      }
      dynamic_stencil_compare_mask_front_update_needed_ = false;
      dynamic_stencil_compare_mask_back_update_needed_ = false;
    }
    if (dynamic_stencil_write_mask_front_update_needed_ ||
        dynamic_stencil_write_mask_back_update_needed_) {
      if (dynamic_stencil_write_mask_front_ == dynamic_stencil_write_mask_back_) {
        deferred_command_buffer_.CmdVkSetStencilWriteMask(VK_STENCIL_FACE_FRONT_AND_BACK,
                                                          dynamic_stencil_write_mask_front_);
      } else {
        if (dynamic_stencil_write_mask_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilWriteMask(VK_STENCIL_FACE_FRONT_BIT,
                                                            dynamic_stencil_write_mask_front_);
        }
        if (dynamic_stencil_write_mask_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilWriteMask(VK_STENCIL_FACE_BACK_BIT,
                                                            dynamic_stencil_write_mask_back_);
        }
      }
      dynamic_stencil_write_mask_front_update_needed_ = false;
      dynamic_stencil_write_mask_back_update_needed_ = false;
    }
    if (dynamic_stencil_reference_front_update_needed_ ||
        dynamic_stencil_reference_back_update_needed_) {
      if (dynamic_stencil_reference_front_ == dynamic_stencil_reference_back_) {
        deferred_command_buffer_.CmdVkSetStencilReference(VK_STENCIL_FACE_FRONT_AND_BACK,
                                                          dynamic_stencil_reference_front_);
      } else {
        if (dynamic_stencil_reference_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilReference(VK_STENCIL_FACE_FRONT_BIT,
                                                            dynamic_stencil_reference_front_);
        }
        if (dynamic_stencil_reference_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilReference(VK_STENCIL_FACE_BACK_BIT,
                                                            dynamic_stencil_reference_back_);
        }
      }
      dynamic_stencil_reference_front_update_needed_ = false;
      dynamic_stencil_reference_back_update_needed_ = false;
    }
  }

  // TODO(Triang3l): VK_EXT_extended_dynamic_state and
  // VK_EXT_extended_dynamic_state2.
}

void VulkanCommandProcessor::UpdateSystemConstantValues(
    bool primitive_polygonal,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool shader_32bit_index_dma, uint32_t compute_memexport_vertex_count,
    const draw_util::ViewportInfo& viewport_info, uint32_t used_texture_mask,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_stencilrefmask = regs.Get<reg::RB_STENCILREFMASK>();
  auto rb_stencilrefmask_bf = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  auto vgt_indx_offset = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  auto vgt_max_vtx_indx = regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);
  auto vgt_min_vtx_indx = regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX);

  bool edram_fragment_shader_interlock =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  // Get the color info register values for each render target. Also, for FSI,
  // exclude components that don't exist in the format from the write mask.
  // Don't exclude fully overlapping render targets, however - two render
  // targets with the same base address are used in the lighting pass of
  // 4D5307E6, for example, with the needed one picked with dynamic control
  // flow.
  reg::RB_COLOR_INFO color_infos[xenos::kMaxColorRenderTargets];
  float rt_clamp[4][4];
  // Two UINT32_MAX if no components actually existing in the RT are written.
  uint32_t rt_keep_masks[4][2];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    color_infos[i] = color_info;
    if (edram_fragment_shader_interlock) {
      RenderTargetCache::GetPSIColorFormatInfo(
          color_info.color_format, (normalized_color_mask >> (i * 4)) & 0b1111, rt_clamp[i][0],
          rt_clamp[i][1], rt_clamp[i][2], rt_clamp[i][3], rt_keep_masks[i][0], rt_keep_masks[i][1]);
    }
  }

  // Disable depth and stencil if it aliases a color render target (for
  // instance, during the XBLA logo in 58410954, though depth writing is already
  // disabled there).
  bool depth_stencil_enabled =
      normalized_depth_control.stencil_enable || normalized_depth_control.z_enable;
  if (edram_fragment_shader_interlock && depth_stencil_enabled) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (rb_depth_info.depth_base == color_infos[i].color_base &&
          (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX)) {
        depth_stencil_enabled = false;
        break;
      }
    }
  }

  bool dirty = false;

  // Flags.
  uint32_t flags = 0;
  // Vertex index shader loading.
  if (shader_32bit_index_dma) {
    flags |= SpirvShaderTranslator::kSysFlag_VertexIndexLoad;
  }
  if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA ||
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA ||
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= SpirvShaderTranslator ::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
    // Point and rectangle expansion uses primitive restart only in the host
    // built-in strip index buffer; guest indices are not reset-based there.
    bool compute_or_primitive_vertex_index_reset =
        primitive_processing_result.host_primitive_reset_enabled &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
    if (compute_or_primitive_vertex_index_reset) {
      flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexReset;
    }
  }
  if (primitive_processing_result.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kTriangleDomainPatchIndexed) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeMemExportPatchIndexInRegister1;
  }
  if (primitive_processing_result.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kTriangleDomainCPIndexed) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeMemExportTriangleCPIndexed;
  } else if (primitive_processing_result.host_vertex_shader_type ==
             Shader::HostVertexShaderType::kQuadDomainCPIndexed) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeMemExportQuadCPIndexed;
  }
  if (primitive_processing_result.tessellation_mode == xenos::TessellationMode::kAdaptive &&
      (primitive_processing_result.host_vertex_shader_type ==
           Shader::HostVertexShaderType::kTriangleDomainPatchIndexed ||
       primitive_processing_result.host_vertex_shader_type ==
           Shader::HostVertexShaderType::kQuadDomainPatchIndexed)) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeMemExportPatchIndexFromInvocation;
  }
  // W0 division control.
  // http://www.x.org/docs/AMD/old/evergreen_3D_registers_v2.pdf
  // 8: VTX_XY_FMT = true: the incoming XY have already been multiplied by 1/W0.
  //               = false: multiply the X, Y coordinates by 1/W0.
  // 9: VTX_Z_FMT = true: the incoming Z has already been multiplied by 1/W0.
  //              = false: multiply the Z coordinate by 1/W0.
  // 10: VTX_W0_FMT = true: the incoming W0 is not 1/W0. Perform the reciprocal
  //                        to get 1/W0.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  }
  // Whether the primitive is polygonal, and gl_FrontFacing matters.
  if (primitive_polygonal) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  // Primitive type.
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  }
  // MSAA sample count.
  flags |= uint32_t(rb_surface_info.msaa_samples)
           << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  // Depth format.
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }
  // Alpha test.
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma writing.
  if (!render_target_cache_->gamma_render_target_as_unorm16()) {
    // Keep parity with D3D12: gamma targets in this path are converted via
    // explicit Xenos PWL gamma logic in shaders rather than via host sRGB
    // attachment conversion semantics.
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (color_infos[i].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }
  if (edram_fragment_shader_interlock && depth_stencil_enabled) {
    flags |= SpirvShaderTranslator::kSysFlag_FSIDepthStencil;
    if (normalized_depth_control.z_enable) {
      flags |= uint32_t(normalized_depth_control.zfunc)
               << SpirvShaderTranslator::kSysFlag_FSIDepthPassIfLess_Shift;
      if (normalized_depth_control.z_write_enable) {
        flags |= SpirvShaderTranslator::kSysFlag_FSIDepthWrite;
      }
    } else {
      // In case stencil is used without depth testing - always pass, and
      // don't modify the stored depth.
      flags |= SpirvShaderTranslator::kSysFlag_FSIDepthPassIfLess |
               SpirvShaderTranslator::kSysFlag_FSIDepthPassIfEqual |
               SpirvShaderTranslator::kSysFlag_FSIDepthPassIfGreater;
    }
    if (normalized_depth_control.stencil_enable) {
      flags |= SpirvShaderTranslator::kSysFlag_FSIStencilTest;
    }
    // Hint - if not applicable to the shader, will not have effect.
    if (alpha_test_function == xenos::CompareFunction::kAlways &&
        !rb_colorcontrol.alpha_to_mask_enable) {
      flags |= SpirvShaderTranslator::kSysFlag_FSIDepthStencilEarlyWrite;
    }
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  // Index buffer address for loading in the shaders.
  if (flags & (SpirvShaderTranslator::kSysFlag_VertexIndexLoad |
               SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)) {
    dirty |=
        system_constants_.vertex_index_load_address != primitive_processing_result.guest_index_base;
    system_constants_.vertex_index_load_address = primitive_processing_result.guest_index_base;
  }

  // Primitive reset index for shader-side index loading.
  uint32_t vertex_index_reset =
      (flags & SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexReset)
          ? regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx
          : 0;
  dirty |= system_constants_.vertex_index_reset != vertex_index_reset;
  system_constants_.vertex_index_reset = vertex_index_reset;
  dirty |= system_constants_.compute_memexport_vertex_count != compute_memexport_vertex_count;
  system_constants_.compute_memexport_vertex_count = compute_memexport_vertex_count;

  // Index or tessellation edge factor buffer endianness.
  xenos::Endian guest_index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16 &&
      guest_index_endian != xenos::Endian::kNone && guest_index_endian != xenos::Endian::k8in16) {
    guest_index_endian =
        guest_index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
  }
  xenos::Endian index_endian =
      (flags & SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)
          ? guest_index_endian
          : primitive_processing_result.host_shader_index_endian;
  dirty |= system_constants_.vertex_index_endian != index_endian;
  system_constants_.vertex_index_endian = index_endian;

  dirty |= system_constants_.line_loop_closing_index !=
           primitive_processing_result.line_loop_closing_index;
  system_constants_.line_loop_closing_index = primitive_processing_result.line_loop_closing_index;

  // Vertex index offset.
  dirty |= system_constants_.vertex_base_index != vgt_indx_offset;
  system_constants_.vertex_base_index = vgt_indx_offset;

  // Vertex index range.
  dirty |= system_constants_.vertex_index_min != vgt_min_vtx_indx;
  dirty |= system_constants_.vertex_index_max != vgt_max_vtx_indx;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  // User clip planes (UCP_ENA_#), when not CLIP_DISABLE.
  // The shader knows only the total count - tightly packing the user clip
  // planes that are actually used.
  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining, &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      const void* user_clip_plane_regs =
          &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4];
      if (std::memcmp(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float))) {
        dirty = true;
        std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float));
      }
      user_clip_plane_write_ptr += 4;
    }
  }

  // Tessellation factor range, plus 1.0 according to
  // https://www.slideshare.net/blackdevilvikas/next-generation-graphics-programming-on-xbox-360.
  float tessellation_factor_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  float tessellation_factor_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  dirty |= system_constants_.tessellation_factor_range_min != tessellation_factor_min;
  dirty |= system_constants_.tessellation_factor_range_max != tessellation_factor_max;
  system_constants_.tessellation_factor_range_min = tessellation_factor_min;
  system_constants_.tessellation_factor_range_max = tessellation_factor_max;

  // Conversion to host normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point size.
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x = float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y = float(pa_su_point_size.height) * (2.0f / 16.0f);
    dirty |= system_constants_.point_vertex_diameter_min != point_vertex_diameter_min;
    dirty |= system_constants_.point_vertex_diameter_max != point_vertex_diameter_max;
    dirty |= system_constants_.point_constant_diameter[0] != point_constant_diameter_x;
    dirty |= system_constants_.point_constant_diameter[1] != point_constant_diameter_y;
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion to avoid multiplying the per-vertex diameter by an
    // additional constant in the shader.
    float point_screen_diameter_to_ndc_radius_x =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_x)) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    float point_screen_diameter_to_ndc_radius_y =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_y)) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[0] !=
             point_screen_diameter_to_ndc_radius_x;
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[1] !=
             point_screen_diameter_to_ndc_radius_y;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  const bool fh1_native = Fh1NativeEnabled();
  const bool fh1_screen = fh1_native && Fh1ScreenViewport(regs);
  const float ratios[4] = {
      fh1_native ? float(REXCVAR_GET(forza_native_width)) / (1280 * draw_resolution_scale_x) : 1.0f,
      fh1_native ? float(REXCVAR_GET(forza_native_height)) / (720 * draw_resolution_scale_y) : 1.0f,
      fh1_screen ? float(1280 * draw_resolution_scale_x) / REXCVAR_GET(forza_native_width) : 1.0f,
      fh1_screen ? float(720 * draw_resolution_scale_y) / REXCVAR_GET(forza_native_height) : 1.0f};
  for (unsigned i = 0; i < 4; ++i) {
    dirty |= system_constants_.fh1_native[i] != ratios[i];
    system_constants_.fh1_native[i] = ratios[i];
  }
  uint32_t native_textures = 0;
  if (fh1_native)
    for (unsigned i = 0; i < 32; ++i)
      if ((used_texture_mask & (1u << i)) && texture_cache_->IsActiveFh1ScreenTexture(i))
        native_textures |= 1u << i;
  dirty |= system_constants_.fh1_native_textures != native_textures;
  system_constants_.fh1_native_textures = native_textures;
  // Patch 0396: VR true stereo's eye offset (Fh1StereoDrawOffset).
  for (unsigned i = 0; i < 4; ++i) {
    dirty |= system_constants_.fh1_vr_eye[i] != fh1_stereo_draw_eye_[i];
    system_constants_.fh1_vr_eye[i] = fh1_stereo_draw_eye_[i];
  }
  // Texture signedness / gamma.
  uint32_t textures_resolution_scaled = 0;
  {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t& texture_signs_uint = system_constants_.texture_swizzled_signs[texture_index >> 2];
      uint32_t texture_signs_shift = 8 * (texture_index & 3);
      uint8_t texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
      uint32_t texture_signs_shifted = uint32_t(texture_signs) << texture_signs_shift;
      uint32_t texture_signs_mask = ((UINT32_C(1) << 8) - 1) << texture_signs_shift;
      dirty |= (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
      texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
      textures_resolution_scaled |=
          uint32_t(texture_cache_->IsActiveTextureResolutionScaled(texture_index)) << texture_index;
    }
  }
  dirty |= system_constants_.textures_resolution_scaled != textures_resolution_scaled;
  system_constants_.textures_resolution_scaled = textures_resolution_scaled;

  // Texture host swizzle in the shader.
  if (!GetVulkanDevice()->properties().imageViewFormatSwizzle) {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t& texture_swizzles_uint = system_constants_.texture_swizzles[texture_index >> 1];
      uint32_t texture_swizzle_shift = 12 * (texture_index & 1);
      uint32_t texture_swizzle = texture_cache_->GetActiveTextureHostSwizzle(texture_index);
      uint32_t texture_swizzle_shifted = uint32_t(texture_swizzle) << texture_swizzle_shift;
      uint32_t texture_swizzle_mask = ((UINT32_C(1) << 12) - 1) << texture_swizzle_shift;
      dirty |= (texture_swizzles_uint & texture_swizzle_mask) != texture_swizzle_shifted;
      texture_swizzles_uint =
          (texture_swizzles_uint & ~texture_swizzle_mask) | texture_swizzle_shifted;
    }
  }

  // Alpha test.
  dirty |= system_constants_.alpha_test_reference != rb_alpha_ref;
  system_constants_.alpha_test_reference = rb_alpha_ref;
  uint32_t alpha_to_mask =
      rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (UINT32_C(1) << 8) : 0;
  dirty |= system_constants_.alpha_to_mask != alpha_to_mask;
  system_constants_.alpha_to_mask = alpha_to_mask;

  uint32_t edram_tile_dwords_scaled = xenos::kEdramTileWidthSamples *
                                      xenos::kEdramTileHeightSamples *
                                      (draw_resolution_scale_x * draw_resolution_scale_y);

  // EDRAM pitch for FSI render target writing.
  if (edram_fragment_shader_interlock) {
    // Align, then multiply by 32bpp tile size in dwords.
    uint32_t edram_32bpp_tile_pitch_dwords_scaled =
        ((rb_surface_info.surface_pitch *
          (rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1)) +
         (xenos::kEdramTileWidthSamples - 1)) /
        xenos::kEdramTileWidthSamples * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_32bpp_tile_pitch_dwords_scaled !=
             edram_32bpp_tile_pitch_dwords_scaled;
    system_constants_.edram_32bpp_tile_pitch_dwords_scaled = edram_32bpp_tile_pitch_dwords_scaled;
  }

  // Color exponent bias and FSI render target writing.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    // Exponent bias is in bits 20:25 of RB_COLOR_INFO.
    int32_t color_exp_bias = color_info.color_exp_bias;
    if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets &&
        (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 &&
             !render_target_cache_->IsFixedRG16TruncatedToMinus1To1() ||
         color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16 &&
             !render_target_cache_->IsFixedRGBA16TruncatedToMinus1To1())) {
      // Remap from -32...32 to -1...1 by dividing the output values by 32,
      // losing blending correctness, but getting the full range.
      color_exp_bias -= 5;
    }
    float color_exp_bias_scale;
    *reinterpret_cast<int32_t*>(&color_exp_bias_scale) =
        UINT32_C(0x3F800000) + (color_exp_bias << 23);
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
    if (edram_fragment_shader_interlock) {
      dirty |= system_constants_.edram_rt_keep_mask[i][0] != rt_keep_masks[i][0];
      system_constants_.edram_rt_keep_mask[i][0] = rt_keep_masks[i][0];
      dirty |= system_constants_.edram_rt_keep_mask[i][1] != rt_keep_masks[i][1];
      system_constants_.edram_rt_keep_mask[i][1] = rt_keep_masks[i][1];
      if (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX) {
        uint32_t rt_base_dwords_scaled = color_info.color_base * edram_tile_dwords_scaled;
        dirty |= system_constants_.edram_rt_base_dwords_scaled[i] != rt_base_dwords_scaled;
        system_constants_.edram_rt_base_dwords_scaled[i] = rt_base_dwords_scaled;
        uint32_t format_flags = RenderTargetCache::AddPSIColorFormatFlags(color_info.color_format);
        dirty |= system_constants_.edram_rt_format_flags[i] != format_flags;
        system_constants_.edram_rt_format_flags[i] = format_flags;
        uint32_t blend_factors_ops =
            regs[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
        dirty |= system_constants_.edram_rt_blend_factors_ops[i] != blend_factors_ops;
        system_constants_.edram_rt_blend_factors_ops[i] = blend_factors_ops;
        // Can't do float comparisons here because NaNs would result in always
        // setting the dirty flag.
        dirty |=
            std::memcmp(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float)) != 0;
        std::memcpy(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float));
      }
    }
  }

  if (edram_fragment_shader_interlock) {
    uint32_t depth_base_dwords_scaled = rb_depth_info.depth_base * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_depth_base_dwords_scaled != depth_base_dwords_scaled;
    system_constants_.edram_depth_base_dwords_scaled = depth_base_dwords_scaled;

    // For non-polygons, front polygon offset is used, and it's enabled if
    // POLY_OFFSET_PARA_ENABLED is set, for polygons, separate front and back
    // are used.
    float poly_offset_front_scale = 0.0f, poly_offset_front_offset = 0.0f;
    float poly_offset_back_scale = 0.0f, poly_offset_back_offset = 0.0f;
    if (primitive_polygonal) {
      if (pa_su_sc_mode_cntl.poly_offset_front_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
      }
      if (pa_su_sc_mode_cntl.poly_offset_back_enable) {
        poly_offset_back_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
        poly_offset_back_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
      }
    } else {
      if (pa_su_sc_mode_cntl.poly_offset_para_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
        poly_offset_back_scale = poly_offset_front_scale;
        poly_offset_back_offset = poly_offset_front_offset;
      }
    }
    // With non-square resolution scaling, make sure the worst-case impact is
    // reverted (slope only along the scaled axis), thus max. More bias is
    // better than less bias, because less bias means Z fighting with the
    // background is more likely.
    float poly_offset_scale_factor = xenos::kPolygonOffsetScaleSubpixelUnit *
                                     std::max(draw_resolution_scale_x, draw_resolution_scale_y);
    poly_offset_front_scale *= poly_offset_scale_factor;
    poly_offset_back_scale *= poly_offset_scale_factor;
    dirty |= system_constants_.edram_poly_offset_front_scale != poly_offset_front_scale;
    system_constants_.edram_poly_offset_front_scale = poly_offset_front_scale;
    dirty |= system_constants_.edram_poly_offset_front_offset != poly_offset_front_offset;
    system_constants_.edram_poly_offset_front_offset = poly_offset_front_offset;
    dirty |= system_constants_.edram_poly_offset_back_scale != poly_offset_back_scale;
    system_constants_.edram_poly_offset_back_scale = poly_offset_back_scale;
    dirty |= system_constants_.edram_poly_offset_back_offset != poly_offset_back_offset;
    system_constants_.edram_poly_offset_back_offset = poly_offset_back_offset;

    if (depth_stencil_enabled && normalized_depth_control.stencil_enable) {
      uint32_t stencil_front_reference_masks = rb_stencilrefmask.value & 0xFFFFFF;
      dirty |=
          system_constants_.edram_stencil_front_reference_masks != stencil_front_reference_masks;
      system_constants_.edram_stencil_front_reference_masks = stencil_front_reference_masks;
      uint32_t stencil_func_ops = (normalized_depth_control.value >> 8) & ((1 << 12) - 1);
      dirty |= system_constants_.edram_stencil_front_func_ops != stencil_func_ops;
      system_constants_.edram_stencil_front_func_ops = stencil_func_ops;

      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        uint32_t stencil_back_reference_masks = rb_stencilrefmask_bf.value & 0xFFFFFF;
        dirty |=
            system_constants_.edram_stencil_back_reference_masks != stencil_back_reference_masks;
        system_constants_.edram_stencil_back_reference_masks = stencil_back_reference_masks;
        uint32_t stencil_func_ops_bf = (normalized_depth_control.value >> 20) & ((1 << 12) - 1);
        dirty |= system_constants_.edram_stencil_back_func_ops != stencil_func_ops_bf;
        system_constants_.edram_stencil_back_func_ops = stencil_func_ops_bf;
      } else {
        dirty |= std::memcmp(system_constants_.edram_stencil_back,
                             system_constants_.edram_stencil_front, 2 * sizeof(uint32_t)) != 0;
        std::memcpy(system_constants_.edram_stencil_back, system_constants_.edram_stencil_front,
                    2 * sizeof(uint32_t));
      }
    }

    dirty |= system_constants_.edram_blend_constant[0] != regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    system_constants_.edram_blend_constant[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    dirty |=
        system_constants_.edram_blend_constant[1] != regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    system_constants_.edram_blend_constant[1] = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    dirty |= system_constants_.edram_blend_constant[2] != regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    system_constants_.edram_blend_constant[2] = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    dirty |=
        system_constants_.edram_blend_constant[3] != regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
    system_constants_.edram_blend_constant[3] = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  }

  if (dirty) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem);
  }
}

bool VulkanCommandProcessor::UpdateBindings(const VulkanShader* vertex_shader,
                                            const VulkanShader* pixel_shader) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Invalidate constant buffers and descriptors for changed data.

  // Float constants.
  // These are the constant base addresses/ranges for shaders.
  // We have these hardcoded right now cause nothing seems to differ on the Xbox
  // 360 (however, OpenGL ES on Adreno 200 on Android has different ranges).
  assert_true(regs[XE_GPU_REG_SQ_VS_CONST] == 0x000FF000 ||
              regs[XE_GPU_REG_SQ_VS_CONST] == 0x00000000);
  assert_true(regs[XE_GPU_REG_SQ_PS_CONST] == 0x000FF100 ||
              regs[XE_GPU_REG_SQ_PS_CONST] == 0x00000000);
  // Check if the float constant layout is still the same and get the counts.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  uint32_t float_constant_count_vertex = float_constant_map_vertex.float_count;
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] != float_constant_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] = float_constant_map_vertex.float_bitmap[i];
      // If no float constants at all, any buffer can be reused for them, so not
      // invalidating.
      if (float_constant_count_vertex) {
        current_constant_buffers_up_to_date_ &=
            ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
      }
    }
  }
  uint32_t float_constant_count_pixel = 0;
  if (pixel_shader != nullptr) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    float_constant_count_pixel = float_constant_map_pixel.float_count;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] != float_constant_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] = float_constant_map_pixel.float_bitmap[i];
        if (float_constant_count_pixel) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
        }
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
  }

  // Write the new constant buffers.
  constexpr uint32_t kAllConstantBuffersMask =
      (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferCount) - 1;
  assert_zero(current_constant_buffers_up_to_date_ & ~kAllConstantBuffersMask);
  if ((current_constant_buffers_up_to_date_ & kAllConstantBuffersMask) != kAllConstantBuffersMask) {
    current_graphics_descriptor_set_values_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants);
    size_t uniform_buffer_alignment =
        size_t(vulkan_device->properties().minUniformBufferOffsetAlignment);
    // System constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferSystem];
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, sizeof(SpirvShaderTranslator::SystemConstants), uniform_buffer_alignment,
          buffer_info.buffer, buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = sizeof(SpirvShaderTranslator::SystemConstants);
      std::memcpy(mapping, &system_constants_, sizeof(SpirvShaderTranslator::SystemConstants));
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SpirvShaderTranslator::kConstantBufferSystem;
    }
    // Vertex shader float constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferFloatVertex];
      // Even if the shader doesn't need any float constants, a valid binding
      // must still be provided (the pipeline layout always has float constants,
      // for both the vertex shader and the pixel shader), so if the first draw
      // in the frame doesn't have float constants at all, still allocate a
      // dummy buffer.
      size_t float_constants_size =
          sizeof(float) * 4 * std::max(float_constant_count_vertex, UINT32_C(1));
      uint8_t* mapping = uniform_buffer_pool_->Request(frame_current_, float_constants_size,
                                                       uniform_buffer_alignment, buffer_info.buffer,
                                                       buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(float_constants_size);
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t float_constant_map_entry = current_float_constant_map_vertex_[i];
        uint32_t float_constant_index;
        while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
          float_constant_map_entry &= ~(1ull << float_constant_index);
          std::memcpy(
              mapping,
              &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) + (float_constant_index << 2)],
              sizeof(float) * 4);
          mapping += sizeof(float) * 4;
        }
      }
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SpirvShaderTranslator::kConstantBufferFloatVertex;
    }
    // Pixel shader float constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferFloatPixel];
      size_t float_constants_size =
          sizeof(float) * 4 * std::max(float_constant_count_pixel, UINT32_C(1));
      uint8_t* mapping = uniform_buffer_pool_->Request(frame_current_, float_constants_size,
                                                       uniform_buffer_alignment, buffer_info.buffer,
                                                       buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(float_constants_size);
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t float_constant_map_entry = current_float_constant_map_pixel_[i];
        uint32_t float_constant_index;
        while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
          float_constant_map_entry &= ~(1ull << float_constant_index);
          std::memcpy(
              mapping,
              &regs[XE_GPU_REG_SHADER_CONSTANT_256_X + (i << 8) + (float_constant_index << 2)],
              sizeof(float) * 4);
          mapping += sizeof(float) * 4;
        }
      }
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SpirvShaderTranslator::kConstantBufferFloatPixel;
    }
    // Bool and loop constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferBoolLoop];
      constexpr size_t kBoolLoopConstantsSize = sizeof(uint32_t) * (8 + 32);
      uint8_t* mapping = uniform_buffer_pool_->Request(frame_current_, kBoolLoopConstantsSize,
                                                       uniform_buffer_alignment, buffer_info.buffer,
                                                       buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(kBoolLoopConstantsSize);
      std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], kBoolLoopConstantsSize);
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SpirvShaderTranslator::kConstantBufferBoolLoop;
    }
    // Fetch constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferFetch];
      constexpr size_t kFetchConstantsSize = sizeof(uint32_t) * 6 * 32;
      uint8_t* mapping = uniform_buffer_pool_->Request(frame_current_, kFetchConstantsSize,
                                                       uniform_buffer_alignment, buffer_info.buffer,
                                                       buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(kFetchConstantsSize);
      std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchConstantsSize);
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SpirvShaderTranslator::kConstantBufferFetch;
    }
  }

  // Textures and samplers.
  const std::vector<VulkanShader::SamplerBinding>& samplers_vertex =
      vertex_shader->GetSamplerBindingsAfterTranslation();
  const std::vector<VulkanShader::TextureBinding>& textures_vertex =
      vertex_shader->GetTextureBindingsAfterTranslation();
  uint32_t sampler_count_vertex = uint32_t(samplers_vertex.size());
  uint32_t texture_count_vertex = uint32_t(textures_vertex.size());
  const std::vector<VulkanShader::SamplerBinding>* samplers_pixel;
  const std::vector<VulkanShader::TextureBinding>* textures_pixel;
  uint32_t sampler_count_pixel, texture_count_pixel;
  if (pixel_shader) {
    samplers_pixel = &pixel_shader->GetSamplerBindingsAfterTranslation();
    textures_pixel = &pixel_shader->GetTextureBindingsAfterTranslation();
    sampler_count_pixel = uint32_t(samplers_pixel->size());
    texture_count_pixel = uint32_t(textures_pixel->size());
  } else {
    samplers_pixel = nullptr;
    textures_pixel = nullptr;
    sampler_count_pixel = 0;
    texture_count_pixel = 0;
  }
  // TODO(Triang3l): Reuse texture and sampler bindings if not changed.
  current_graphics_descriptor_set_values_up_to_date_ &=
      ~((UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex) |
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel));

  // Make sure new descriptor sets are bound to the command buffer.

  current_graphics_descriptor_sets_bound_up_to_date_ &=
      current_graphics_descriptor_set_values_up_to_date_;

  // Fill the texture and sampler write image infos.

  bool write_vertex_textures =
      (texture_count_vertex || sampler_count_vertex) &&
      !(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex));
  bool write_pixel_textures =
      (texture_count_pixel || sampler_count_pixel) &&
      !(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel));
  descriptor_write_image_info_.clear();
  descriptor_write_image_info_.reserve(
      (write_vertex_textures ? texture_count_vertex + sampler_count_vertex : 0) +
      (write_pixel_textures ? texture_count_pixel + sampler_count_pixel : 0));
  size_t vertex_texture_image_info_offset = descriptor_write_image_info_.size();
  if (write_vertex_textures && texture_count_vertex) {
    for (const VulkanShader::TextureBinding& texture_binding : textures_vertex) {
      VkDescriptorImageInfo& descriptor_image_info = descriptor_write_image_info_.emplace_back();
      descriptor_image_info.imageView = texture_cache_->GetActiveBindingOrNullImageView(
          texture_binding.fetch_constant, texture_binding.dimension,
          bool(texture_binding.is_signed));
      descriptor_image_info.imageLayout = descriptor_image_info.imageView != VK_NULL_HANDLE
                                              ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                              : VK_IMAGE_LAYOUT_UNDEFINED;
    }
  }
  size_t vertex_sampler_image_info_offset = descriptor_write_image_info_.size();
  if (write_vertex_textures && sampler_count_vertex) {
    for (const std::pair<VulkanTextureCache::SamplerParameters, VkSampler>& sampler_pair :
         current_samplers_vertex_) {
      VkDescriptorImageInfo& descriptor_image_info = descriptor_write_image_info_.emplace_back();
      descriptor_image_info.sampler = sampler_pair.second;
    }
  }
  size_t pixel_texture_image_info_offset = descriptor_write_image_info_.size();
  if (write_pixel_textures && texture_count_pixel) {
    for (const VulkanShader::TextureBinding& texture_binding : *textures_pixel) {
      VkDescriptorImageInfo& descriptor_image_info = descriptor_write_image_info_.emplace_back();
      descriptor_image_info.imageView = texture_cache_->GetActiveBindingOrNullImageView(
          texture_binding.fetch_constant, texture_binding.dimension,
          bool(texture_binding.is_signed));
      descriptor_image_info.imageLayout = descriptor_image_info.imageView != VK_NULL_HANDLE
                                              ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                              : VK_IMAGE_LAYOUT_UNDEFINED;
    }
  }
  size_t pixel_sampler_image_info_offset = descriptor_write_image_info_.size();
  if (write_pixel_textures && sampler_count_pixel) {
    for (const std::pair<VulkanTextureCache::SamplerParameters, VkSampler>& sampler_pair :
         current_samplers_pixel_) {
      VkDescriptorImageInfo& descriptor_image_info = descriptor_write_image_info_.emplace_back();
      descriptor_image_info.sampler = sampler_pair.second;
    }
  }

  // Write the new descriptor sets.

  // Consecutive bindings updated via a single VkWriteDescriptorSet must have
  // identical stage flags, but for the constants they vary. Plus vertex and
  // pixel texture images and samplers.
  std::array<VkWriteDescriptorSet, SpirvShaderTranslator::kConstantBufferCount + 2 * 2>
      write_descriptor_sets;
  uint32_t write_descriptor_set_count = 0;
  uint32_t write_descriptor_set_bits = 0;
  assert_not_zero(current_graphics_descriptor_set_values_up_to_date_ &
                  (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram));
  // Constant buffers.
  if (!(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants))) {
    VkDescriptorSet constants_descriptor_set;
    if (!constants_transient_descriptors_free_.empty()) {
      constants_descriptor_set = constants_transient_descriptors_free_.back();
      constants_transient_descriptors_free_.pop_back();
    } else {
      VkDescriptorPoolSize constants_descriptor_count;
      constants_descriptor_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      constants_descriptor_count.descriptorCount = SpirvShaderTranslator::kConstantBufferCount;
      constants_descriptor_set = transient_descriptor_allocator_uniform_buffer_.Allocate(
          descriptor_set_layout_constants_, &constants_descriptor_count, 1);
      if (constants_descriptor_set == VK_NULL_HANDLE) {
        return false;
      }
    }
    constants_transient_descriptors_used_.emplace_back(frame_current_, constants_descriptor_set);
    // Consecutive bindings updated via a single VkWriteDescriptorSet must have
    // identical stage flags, but for the constants they vary.
    for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
      VkWriteDescriptorSet& write_constants = write_descriptor_sets[write_descriptor_set_count++];
      write_constants.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write_constants.pNext = nullptr;
      write_constants.dstSet = constants_descriptor_set;
      write_constants.dstBinding = i;
      write_constants.dstArrayElement = 0;
      write_constants.descriptorCount = 1;
      write_constants.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      write_constants.pImageInfo = nullptr;
      write_constants.pBufferInfo = &current_constant_buffer_infos_[i];
      write_constants.pTexelBufferView = nullptr;
    }
    write_descriptor_set_bits |= UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants;
    current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetConstants] =
        constants_descriptor_set;
  }
  // Vertex shader textures and samplers.
  if (write_vertex_textures) {
    VkWriteDescriptorSet* write_textures =
        write_descriptor_sets.data() + write_descriptor_set_count;
    uint32_t texture_descriptor_set_write_count = WriteTransientTextureBindings(
        true, texture_count_vertex, sampler_count_vertex,
        current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_vertex_ref(),
        descriptor_write_image_info_.data() + vertex_texture_image_info_offset,
        descriptor_write_image_info_.data() + vertex_sampler_image_info_offset, write_textures);
    if (!texture_descriptor_set_write_count) {
      return false;
    }
    write_descriptor_set_count += texture_descriptor_set_write_count;
    write_descriptor_set_bits |= UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex;
    current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetTexturesVertex] =
        write_textures[0].dstSet;
  }
  // Pixel shader textures and samplers.
  if (write_pixel_textures) {
    VkWriteDescriptorSet* write_textures =
        write_descriptor_sets.data() + write_descriptor_set_count;
    uint32_t texture_descriptor_set_write_count = WriteTransientTextureBindings(
        false, texture_count_pixel, sampler_count_pixel,
        current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_pixel_ref(),
        descriptor_write_image_info_.data() + pixel_texture_image_info_offset,
        descriptor_write_image_info_.data() + pixel_sampler_image_info_offset, write_textures);
    if (!texture_descriptor_set_write_count) {
      return false;
    }
    write_descriptor_set_count += texture_descriptor_set_write_count;
    write_descriptor_set_bits |= UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel;
    current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
        write_textures[0].dstSet;
  }
  // Write.
  if (write_descriptor_set_count) {
    dfn.vkUpdateDescriptorSets(device, write_descriptor_set_count, write_descriptor_sets.data(), 0,
                               nullptr);
  }
  // Only make valid if all descriptor sets have been allocated and written
  // successfully.
  current_graphics_descriptor_set_values_up_to_date_ |= write_descriptor_set_bits;

  // Bind the new descriptor sets.
  uint32_t descriptor_sets_needed = (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetCount) - 1;
  if (!texture_count_vertex && !sampler_count_vertex) {
    descriptor_sets_needed &= ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex);
  }
  if (!texture_count_pixel && !sampler_count_pixel) {
    descriptor_sets_needed &= ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
  }
  uint32_t descriptor_sets_remaining =
      descriptor_sets_needed & ~current_graphics_descriptor_sets_bound_up_to_date_;
  uint32_t descriptor_set_index;
  while (rex::bit_scan_forward(descriptor_sets_remaining, &descriptor_set_index)) {
    uint32_t descriptor_set_mask_tzcnt =
        rex::tzcnt(~(descriptor_sets_remaining | ((UINT32_C(1) << descriptor_set_index) - 1)));
    deferred_command_buffer_.CmdVkBindDescriptorSets(
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        current_guest_graphics_pipeline_layout_->GetPipelineLayout(), descriptor_set_index,
        descriptor_set_mask_tzcnt - descriptor_set_index,
        current_graphics_descriptor_sets_ + descriptor_set_index, 0, nullptr);
    if (descriptor_set_mask_tzcnt >= 32) {
      break;
    }
    descriptor_sets_remaining &= ~((UINT32_C(1) << descriptor_set_mask_tzcnt) - 1);
  }
  current_graphics_descriptor_sets_bound_up_to_date_ |= descriptor_sets_needed;

  return true;
}

uint32_t VulkanCommandProcessor::WriteTransientTextureBindings(
    bool is_vertex, uint32_t texture_count, uint32_t sampler_count,
    VkDescriptorSetLayout descriptor_set_layout, const VkDescriptorImageInfo* texture_image_info,
    const VkDescriptorImageInfo* sampler_image_info,
    VkWriteDescriptorSet* descriptor_set_writes_out) {
  assert_true(frame_open_);
  if (!texture_count && !sampler_count) {
    return 0;
  }
  TextureDescriptorSetLayoutKey texture_descriptor_set_layout_key;
  texture_descriptor_set_layout_key.texture_count = texture_count;
  texture_descriptor_set_layout_key.sampler_count = sampler_count;
  texture_descriptor_set_layout_key.is_vertex = uint32_t(is_vertex);
  VkDescriptorSet texture_descriptor_set;
  auto textures_free_it =
      texture_transient_descriptor_sets_free_.find(texture_descriptor_set_layout_key);
  if (textures_free_it != texture_transient_descriptor_sets_free_.end() &&
      !textures_free_it->second.empty()) {
    texture_descriptor_set = textures_free_it->second.back();
    textures_free_it->second.pop_back();
  } else {
    std::array<VkDescriptorPoolSize, 2> texture_descriptor_counts;
    uint32_t texture_descriptor_counts_count = 0;
    if (texture_count) {
      VkDescriptorPoolSize& texture_descriptor_count =
          texture_descriptor_counts[texture_descriptor_counts_count++];
      texture_descriptor_count.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      texture_descriptor_count.descriptorCount = texture_count;
    }
    if (sampler_count) {
      VkDescriptorPoolSize& texture_descriptor_count =
          texture_descriptor_counts[texture_descriptor_counts_count++];
      texture_descriptor_count.type = VK_DESCRIPTOR_TYPE_SAMPLER;
      texture_descriptor_count.descriptorCount = sampler_count;
    }
    assert_not_zero(texture_descriptor_counts_count);
    texture_descriptor_set = transient_descriptor_allocator_textures_.Allocate(
        descriptor_set_layout, texture_descriptor_counts.data(), texture_descriptor_counts_count);
    if (texture_descriptor_set == VK_NULL_HANDLE) {
      return 0;
    }
  }
  UsedTextureTransientDescriptorSet& used_texture_descriptor_set =
      texture_transient_descriptor_sets_used_.emplace_back();
  used_texture_descriptor_set.frame = frame_current_;
  used_texture_descriptor_set.layout = texture_descriptor_set_layout_key;
  used_texture_descriptor_set.set = texture_descriptor_set;
  uint32_t descriptor_set_write_count = 0;
  if (texture_count) {
    VkWriteDescriptorSet& descriptor_set_write =
        descriptor_set_writes_out[descriptor_set_write_count++];
    descriptor_set_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write.pNext = nullptr;
    descriptor_set_write.dstSet = texture_descriptor_set;
    descriptor_set_write.dstBinding = 0;
    descriptor_set_write.dstArrayElement = 0;
    descriptor_set_write.descriptorCount = texture_count;
    descriptor_set_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_write.pImageInfo = texture_image_info;
    descriptor_set_write.pBufferInfo = nullptr;
    descriptor_set_write.pTexelBufferView = nullptr;
  }
  if (sampler_count) {
    VkWriteDescriptorSet& descriptor_set_write =
        descriptor_set_writes_out[descriptor_set_write_count++];
    descriptor_set_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write.pNext = nullptr;
    descriptor_set_write.dstSet = texture_descriptor_set;
    descriptor_set_write.dstBinding = texture_count;
    descriptor_set_write.dstArrayElement = 0;
    descriptor_set_write.descriptorCount = sampler_count;
    descriptor_set_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_set_write.pImageInfo = sampler_image_info;
    descriptor_set_write.pBufferInfo = nullptr;
    descriptor_set_write.pTexelBufferView = nullptr;
  }
  assert_not_zero(descriptor_set_write_count);
  return descriptor_set_write_count;
}

}  // namespace rex::graphics::vulkan
