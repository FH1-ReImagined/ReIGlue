// FH1 (#378): building the pipelines of queued draws before the command processor reaches them
// (fh1_pipeline_lookahead.h). The idea is ReGPU's (github.com/zer0ducksgiven/ReGPU, MIT;
// renderer.cpp GpuLookahead): at a pipeline miss, walk what is queued on shadow registers and
// build everything missing at once.
//
// The walk runs on the command processor thread, inside the draw whose pipeline is missing,
// because it uses the command processor's shader cache, translator and render target state. It
// puts the registers it finds into the command processor's own register file (the context
// registers only, which no other thread reads) and restores them before the draw goes on, so the
// pipeline cache works out each queued draw's pipeline with the same code as the draw itself.
// It is bounded in packets, draws, pipelines and time, and the pipelines are built on the
// pipeline creation threads, never on this one.

#include "fh1_pipeline_lookahead.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/pipeline_cache.h>
#include <rex/graphics/vulkan/primitive_processor.h>
#include <rex/graphics/vulkan/render_target_cache.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include "../fh1_command_cursor.h"

REXCVAR_DEFINE_BOOL(vulkan_pipeline_lookahead, true, "GPU/Vulkan",
                    "When a draw needs a pipeline that is not built yet, look through the draws "
                    "already queued behind it and start building every pipeline they need at "
                    "once, in the background (with synchronous creation: wait once for all)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_UINT32(vulkan_pipeline_lookahead_packets, 262144, "GPU/Vulkan",
                      "Pipeline lookahead: command packets looked at in one walk at most "
                      "(padding words included)")
    .debug_only();
REXCVAR_DEFINE_UINT32(vulkan_pipeline_lookahead_pipelines, 64, "GPU/Vulkan",
                      "Pipeline lookahead: pipelines started by one walk at most")
    .debug_only();
REXCVAR_DEFINE_UINT32(vulkan_pipeline_lookahead_draws, 4096, "GPU/Vulkan",
                      "Pipeline lookahead: draws whose pipeline one walk works out at most")
    .debug_only();
REXCVAR_DEFINE_BOOL(vulkan_pipeline_lookahead_past_waits, false, "GPU/Vulkan",
                    "Pipeline lookahead: go on past a wait for a value the game has not written "
                    "yet (the commands after it may not be complete)")
    .debug_only();
REXCVAR_DEFINE_UINT32(vulkan_pipeline_lookahead_ms, 8, "GPU/Vulkan",
                      "Pipeline lookahead: time limit of one walk (it delays the draw that "
                      "started it), in milliseconds")
    .debug_only();

// primitive_processor.cpp.
REXCVAR_DECLARE(bool, tessellated_strips_and_fans_as_lists);
REXCVAR_DECLARE(bool, skip_adaptive_triangle_tessellation);

namespace rex::graphics::vulkan {

namespace {

using namespace fh1_pipeline_lookahead;

// Where the last walk that reached the end of the queue stopped, and the state there: the next
// miss before the command processor gets there walks only what was queued since.
struct Resume {
  bool valid = false;
  uint64_t position = 0;  // ring dwords since the start
  uint32_t registers[kRegisterCount];
  Shader* vertex_shader = nullptr;
  Shader* pixel_shader = nullptr;
  uint64_t bin_mask = 0, bin_select = 0;
  RenderTargetCache::BindingPrediction bindings;
  bool scene_composited = false;
  uint32_t scene_base = UINT32_MAX;
  const void* pipeline_cache = nullptr;
  uint64_t shader_epoch = 0;
};
Resume resume;
// The furthest ring position a walk reached, and whether and when the last one stopped early.
uint64_t walked_until = 0;
bool last_walk_early = false;
std::chrono::steady_clock::time_point last_walk_time{};
uint32_t saved_registers[kRegisterCount];

// The fields of PrimitiveProcessor::Process's result that a pipeline depends on, from the
// registers alone (Process also converts and uploads indices). False where Process fails.
bool PredictPrimitiveProcessing(const RegisterFile& regs, const PrimitiveProcessor& processor,
                                PrimitiveProcessor::ProcessingResult& result) {
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  xenos::PrimitiveType guest_primitive_type = vgt_draw_initiator.prim_type;
  xenos::PrimitiveType host_primitive_type = guest_primitive_type;
  bool tessellation_enabled =
      xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable;
  xenos::TessellationMode tessellation_mode = regs.Get<reg::VGT_HOS_CNTL>().tess_mode;
  Shader::HostVertexShaderType host_vertex_shader_type = Shader::HostVertexShaderType(-1);
  if (tessellation_enabled) {
    switch (guest_primitive_type) {
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kTriangleFan:
      case xenos::PrimitiveType::kTriangleStrip:
        if (guest_primitive_type != xenos::PrimitiveType::kTriangleList) {
          if (!REXCVAR_GET(tessellated_strips_and_fans_as_lists)) {
            return false;
          }
          host_primitive_type = xenos::PrimitiveType::kTriangleList;
        }
        if (tessellation_mode == xenos::TessellationMode::kDiscrete ||
            tessellation_mode == xenos::TessellationMode::kContinuous) {
          host_vertex_shader_type = Shader::HostVertexShaderType::kTriangleDomainCPIndexed;
        }
        break;
      case xenos::PrimitiveType::kQuadList:
        if (tessellation_mode == xenos::TessellationMode::kDiscrete ||
            tessellation_mode == xenos::TessellationMode::kContinuous) {
          host_vertex_shader_type = Shader::HostVertexShaderType::kQuadDomainCPIndexed;
        }
        break;
      case xenos::PrimitiveType::kTrianglePatch:
        if (tessellation_mode == xenos::TessellationMode::kAdaptive &&
            REXCVAR_GET(skip_adaptive_triangle_tessellation)) {
          return false;
        }
        host_vertex_shader_type = Shader::HostVertexShaderType::kTriangleDomainPatchIndexed;
        break;
      case xenos::PrimitiveType::kQuadPatch:
        host_vertex_shader_type = Shader::HostVertexShaderType::kQuadDomainPatchIndexed;
        break;
      default:
        break;
    }
    if (host_vertex_shader_type == Shader::HostVertexShaderType(-1)) {
      return false;
    }
  } else {
    host_vertex_shader_type = Shader::HostVertexShaderType::kVertex;
    switch (guest_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        if (processor.IsExpandingPointSpritesInVS()) {
          host_primitive_type = xenos::PrimitiveType::kTriangleStrip;
          host_vertex_shader_type = Shader::HostVertexShaderType::kPointListAsTriangleStrip;
        }
        break;
      case xenos::PrimitiveType::kLineList:
      case xenos::PrimitiveType::kLineStrip:
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kTriangleStrip:
        break;
      case xenos::PrimitiveType::kRectangleList:
        if (processor.IsExpandingRectangleListsInVS()) {
          host_primitive_type = xenos::PrimitiveType::kTriangleStrip;
          host_vertex_shader_type = Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
        }
        break;
      case xenos::PrimitiveType::kTriangleFan:
        if (processor.IsConvertingTriangleFansToLists()) {
          host_primitive_type = xenos::PrimitiveType::kTriangleList;
        }
        break;
      case xenos::PrimitiveType::kLineLoop:
        if (processor.IsConvertingLineLoopsToStrips()) {
          host_primitive_type = xenos::PrimitiveType::kLineStrip;
        }
        break;
      case xenos::PrimitiveType::kQuadList:
        if (processor.IsConvertingQuadListsToTriangleLists()) {
          host_primitive_type = xenos::PrimitiveType::kTriangleList;
        }
        break;
      default:
        return false;
    }
  }

  uint32_t vertex_count = vgt_draw_initiator.num_indices;
  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  bool dma = vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA;
  if (dma) {
    vertex_count = std::min(vertex_count, uint32_t(vgt_dma_size.num_words));
  } else if (vgt_draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    return false;  // immediate indices: not supported by the draw either
  }
  if (!vertex_count) {
    return false;
  }
  bool primitive_reset = false;
  if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      host_vertex_shader_type == Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
    primitive_reset = true;
  } else if (dma) {
    if (tessellation_enabled && tessellation_mode == xenos::TessellationMode::kAdaptive) {
      if (vgt_draw_initiator.index_size != xenos::IndexFormat::kInt32) {
        return false;
      }
    } else if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena &&
               host_primitive_type == guest_primitive_type) {
      // Process keeps the guest's primitive reset unless it converts the primitive type.
      switch (guest_primitive_type) {
        case xenos::PrimitiveType::kLineStrip:
        case xenos::PrimitiveType::kTriangleFan:
        case xenos::PrimitiveType::kTriangleStrip:
        case xenos::PrimitiveType::kLineLoop:
        case xenos::PrimitiveType::kQuadStrip:
        case xenos::PrimitiveType::kPolygon:
        case xenos::PrimitiveType::k2DLineStrip:
        case xenos::PrimitiveType::k2DTriStrip:
          if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16) {
            xenos::Endian endian = vgt_dma_size.swap_mode;
            if (endian != xenos::Endian::kNone && endian != xenos::Endian::k8in16) {
              endian = endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16
                                                       : xenos::Endian::kNone;
            }
            primitive_reset =
                xenos::GpuSwap(regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx,
                               endian) <= UINT16_MAX;
          } else {
            primitive_reset = true;
          }
          break;
        default:
          break;
      }
    }
  }
  result.guest_primitive_type = guest_primitive_type;
  result.host_primitive_type = host_primitive_type;
  result.host_vertex_shader_type = host_vertex_shader_type;
  result.tessellation_mode = tessellation_mode;
  result.host_primitive_reset_enabled = primitive_reset;
  return true;
}

}  // namespace

void VulkanCommandProcessor::Fh1PipelineLookahead() {
  const auto walk_start = std::chrono::steady_clock::now();
  auto& cursor = fh1_command_cursor::cursor;
  const memory::RingBuffer* ring = cursor.ring;
  if (!ring || !cursor.depth || cursor.depth > fh1_command_cursor::kMaxBuffers ||
      cursor.buffers[0] != ring || !pipeline_cache_ || !render_target_cache_ ||
      !primitive_processor_) {
    return;
  }
  uint32_t ring_dwords = uint32_t(ring->capacity() / sizeof(uint32_t));
  uint32_t write_index = write_ptr_index_.load(std::memory_order_acquire);
  if (!ring_dwords || write_index == 0xBAADF00D) {
    return;
  }
  write_index %= ring_dwords;
  uint32_t cp_index = uint32_t(ring->read_offset() / sizeof(uint32_t));
  uint64_t cp_position =
      cursor.ring_done + RingDistance(cursor.ring_start, cp_index, ring_dwords);
  uint64_t write_position = cp_position + RingDistance(cp_index, write_index, ring_dwords);

  VulkanPipelineCache& pipelines = *pipeline_cache_;
  const uint64_t epoch = shader_epoch.load(std::memory_order_relaxed);
  bool resumed = resume.valid && resume.pipeline_cache == &pipelines &&
                 resume.shader_epoch == epoch && cp_position <= resume.position &&
                 resume.position <= write_position;
  if (resumed && resume.position == write_position) {
    return;  // walked up to the write pointer already, nothing queued since
  }
  if (!resumed) {
    // A walk cut short covers this point already (or, at the same ring position, inside the same
    // indirect buffers, did so moments ago).
    if (cp_position < walked_until ||
        (cp_position == walked_until && last_walk_early &&
         walk_start - last_walk_time < std::chrono::milliseconds(20))) {
      return;
    }
  }

  // Draws are looked at only when their pipeline state may differ from the previous draw's.
  struct Hooks {
    VulkanCommandProcessor& cp;
    RegisterFile& regs;
    VulkanPipelineCache& pipelines;
    Shader* vertex_shader;
    Shader* pixel_shader;
    RenderTargetCache::BindingPrediction bindings;
    bool scene_composited;
    uint32_t scene_base;
    uint32_t max_pipelines, max_draws;
    std::chrono::steady_clock::time_point deadline;
    bool dirty = true;
    uint32_t last_draw_bits = 0;
    Shader* last_vertex_shader = nullptr;
    Shader* last_pixel_shader = nullptr;
    uint32_t draws = 0, queued = 0;
    bool out = false;

    void Register(uint32_t index, uint32_t value) {
      uint32_t& r = regs.values[index];
      if (r != value) {
        r = value;
        dirty = true;
      }
    }
    const uint32_t* Translate(uint32_t physical, uint32_t bytes) {
      if (physical >= 0x20000000u || bytes > 0x20000000u - physical) {
        return nullptr;
      }
      return cp.memory_->TranslatePhysical<const uint32_t*>(physical);
    }
    void LoadShader(uint32_t type, const uint32_t* words, uint32_t dwords) {
      if (type > uint32_t(xenos::ShaderType::kPixel)) {
        return;
      }
      // A copy: the game may change the memory under the hash.
      std::vector<uint32_t> ucode(words, words + dwords);
      Shader* shader =
          pipelines.LoadShaderAhead(xenos::ShaderType(type), ucode.data(), dwords);
      (type == uint32_t(xenos::ShaderType::kVertex) ? vertex_shader : pixel_shader) = shader;
    }
    // Asked by the walker every few packets and after each draw looked at (which may have
    // translated shaders).
    bool Out() {
      if (!out) {
        out = std::chrono::steady_clock::now() >= deadline;
      }
      return out;
    }
    End Swap() {
      // With split rendering, the next frame starts in the scene partition. (The render
      // targets Update accumulates are kept across frames: the Vulkan backend does not call
      // RenderTargetCache::BeginFrame.)
      dirty = true;
      if (cp.fh1_other_render_targets_) {
        if (cp.fh1_native_hud_active_) {
          return End::kPartition;
        }
        scene_composited = false;
        scene_base = UINT32_MAX;
      }
      return End::kQueueEnd;
    }
    End Draw(uint32_t initiator, bool dma, uint32_t dma_base, uint32_t dma_size) {
      // Written by the draw packet: not state changes by themselves.
      regs.values[kRegisterDrawInitiator] = initiator;
      if (dma) {
        regs.values[kRegisterDmaBase] = dma_base;
        regs.values[kRegisterDmaSize] = dma_size;
      }
      // The same state as the previous draw looked at: the same pipeline.
      uint32_t draw_bits = (initiator & 0xFFF) | ((dma ? dma_size >> 30 : 0) << 12) |
                           (uint32_t((initiator >> 16) != 0) << 14);
      if (!dirty && draw_bits == last_draw_bits && vertex_shader == last_vertex_shader &&
          pixel_shader == last_pixel_shader) {
        return End::kQueueEnd;
      }
      dirty = false;
      last_draw_bits = draw_bits;
      last_vertex_shader = vertex_shader;
      last_pixel_shader = pixel_shader;
      if (++draws > max_draws) {
        return End::kDraws;
      }
      End end = PredictDraw();
      if (end != End::kQueueEnd) {
        return end;
      }
      return Out() ? End::kTime : End::kQueueEnd;
    }

    // As VulkanCommandProcessor::IssueDraw up to ConfigurePipeline, with predictions where
    // the draw changes state (primitive processing, render target bindings).
    End PredictDraw() {
      const RegisterFile& r = regs;
      if (r.Get<reg::PA_SC_VIZ_QUERY>().viz_query_ena &&
          r.Get<reg::PA_SC_VIZ_QUERY>().kill_pix_post_hi_z) {
        return End::kQueueEnd;  // dropped by ExecutePacketType3Draw
      }
      xenos::EdramMode edram_mode = r.Get<reg::RB_MODECONTROL>().edram_mode;
      if (edram_mode == xenos::EdramMode::kCopy || !vertex_shader) {
        return End::kQueueEnd;
      }
      auto* vs = static_cast<VulkanShader*>(vertex_shader);
      pipelines.AnalyzeShaderUcode(*vs);
      bool memexport_used_vertex = vs->memexport_eM_written() != 0;
      bool primitive_polygonal = draw_util::IsPrimitivePolygonal(r);
      bool is_rasterization_done =
          draw_util::IsRasterizationPotentiallyDone(r, primitive_polygonal);
      if (r.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0 && is_rasterization_done) {
        return End::kQueueEnd;
      }
      VulkanShader* ps = nullptr;
      if (is_rasterization_done) {
        if (edram_mode == xenos::EdramMode::kColorDepth && pixel_shader) {
          ps = static_cast<VulkanShader*>(pixel_shader);
          pipelines.AnalyzeShaderUcode(*ps);
          if (!draw_util::IsPixelShaderNeededWithRasterization(*ps, r)) {
            ps = nullptr;
          }
        }
      } else if (!memexport_used_vertex) {
        return End::kQueueEnd;
      }
      reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(r);
      uint32_t ps_param_gen_pos = UINT32_MAX;
      uint32_t interpolator_mask =
          ps ? (vs->writes_interpolators() &
                ps->GetInterpolatorInputMask(r.Get<reg::SQ_PROGRAM_CNTL>(),
                                             r.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos))
             : 0;
      // FH1's split rendering: the HUD partition has its own caches; stop where it begins.
      if (cp.fh1_other_render_targets_ && is_rasterization_done && ps &&
          !cp.fh1_native_hud_active_) {
        const auto color = r.Get<reg::RB_COLOR_INFO>();
        const auto surface = r.Get<reg::RB_SURFACE_INFO>();
        const bool front_buffer =
            surface.surface_pitch == 1280 && surface.msaa_samples == xenos::MsaaSamples::k1X;
        if (front_buffer && color.color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10) {
          scene_composited = true;
          scene_base = color.color_base;
        } else if (front_buffer &&
                   color.color_format ==
                       xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10 &&
                   (!scene_composited || color.color_base == scene_base)) {
          return End::kPartition;
        }
      }
      if (cp.fh1_other_pipelines_) {
        vs = pipelines.LoadShaderForDraw(*vs);
        pipelines.AnalyzeShaderUcode(*vs);
        if (ps) {
          ps = pipelines.LoadShaderForDraw(*ps);
          pipelines.AnalyzeShaderUcode(*ps);
        }
      }
      PrimitiveProcessor::ProcessingResult primitive_processing_result;
      if (!PredictPrimitiveProcessing(r, *cp.primitive_processor_, primitive_processing_result)) {
        return End::kQueueEnd;
      }
      Shader::HostVertexShaderType host_vertex_shader_type =
          primitive_processing_result.host_vertex_shader_type;
      if (host_vertex_shader_type != Shader::HostVertexShaderType::kVertex &&
          host_vertex_shader_type != Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
          host_vertex_shader_type != Shader::HostVertexShaderType::kRectangleListAsTriangleStrip &&
          !Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
        return End::kQueueEnd;
      }
      auto vertex_shader_modification = pipelines.GetCurrentVertexShaderModification(
          *vs, host_vertex_shader_type, interpolator_mask, ps_param_gen_pos != UINT32_MAX);
      auto pixel_shader_modification =
          ps ? pipelines.GetCurrentPixelShaderModification(*ps, interpolator_mask,
                                                           ps_param_gen_pos,
                                                           normalized_depth_control)
             : SpirvShaderTranslator::Modification(0);
      auto* vertex_shader_translation = static_cast<VulkanShader::VulkanTranslation*>(
          vs->GetOrCreateTranslation(vertex_shader_modification.value));
      auto* pixel_shader_translation =
          ps ? static_cast<VulkanShader::VulkanTranslation*>(
                   ps->GetOrCreateTranslation(pixel_shader_modification.value))
             : nullptr;
      uint32_t normalized_color_mask =
          ps ? draw_util::GetNormalizedColorMask(r, ps->writes_color_targets()) : 0;
      VulkanRenderTargetCache::RenderPassKey render_pass_key;
      if (!cp.render_target_cache_->PredictRenderPassKey(bindings, is_rasterization_done,
                                                         normalized_depth_control,
                                                         normalized_color_mask,
                                                         render_pass_key)) {
        return End::kQueueEnd;
      }
      if (pipelines.QueuePipelineAhead(vertex_shader_translation, pixel_shader_translation,
                                       primitive_processing_result, normalized_depth_control,
                                       normalized_color_mask, render_pass_key) ==
              VulkanPipelineCache::AheadResult::kQueued &&
          ++queued >= max_pipelines) {
        return End::kPipelines;
      }
      return End::kQueueEnd;
    }
  };

  uint32_t* live = register_file_->values + kFirstRegister;
  std::memcpy(saved_registers, live, sizeof(saved_registers));
  Hooks hooks{*this,
              *register_file_,
              pipelines,
              active_vertex_shader_,
              active_pixel_shader_,
              {},
              fh1_scene_composited_,
              fh1_scene_base_,
              std::max(REXCVAR_GET(vulkan_pipeline_lookahead_pipelines), uint32_t(1)),
              REXCVAR_GET(vulkan_pipeline_lookahead_draws),
              walk_start + std::chrono::milliseconds(REXCVAR_GET(vulkan_pipeline_lookahead_ms))};
  uint64_t bin_mask = bin_mask_, bin_select = bin_select_;
  uint64_t ring_position = cp_position;
  uint32_t ring_index = cp_index;
  if (resumed) {
    std::memcpy(live, resume.registers, sizeof(resume.registers));
    hooks.vertex_shader = resume.vertex_shader;
    hooks.pixel_shader = resume.pixel_shader;
    hooks.bindings = resume.bindings;
    hooks.scene_composited = resume.scene_composited;
    hooks.scene_base = resume.scene_base;
    bin_mask = resume.bin_mask;
    bin_select = resume.bin_select;
    ring_position = resume.position;
    ring_index = uint32_t((cp_index + (resume.position - cp_position)) % ring_dwords);
  } else {
    render_target_cache_->GetBindingPrediction(hooks.bindings);
  }
  Walker<Hooks> walker(hooks, REXCVAR_GET(vulkan_pipeline_lookahead_packets), bin_mask,
                       bin_select);
  walker.set_stop_at_unmet_waits(!REXCVAR_GET(vulkan_pipeline_lookahead_past_waits));
  End end = End::kQueueEnd;
  if (!resumed) {
    // First the rest of the indirect buffers being executed, innermost first. A finished one's
    // read offset is back at its start.
    for (uint32_t k = std::min(cursor.depth, fh1_command_cursor::kMaxBuffers) - 1;
         k >= 1 && end == End::kQueueEnd; --k) {
      const memory::RingBuffer* buffer = cursor.buffers[k];
      uint32_t at = uint32_t(buffer->read_offset() / sizeof(uint32_t));
      uint32_t dwords = uint32_t(buffer->capacity() / sizeof(uint32_t));
      if (!at || at >= dwords) {
        continue;
      }
      end = walker.Walk(reinterpret_cast<const uint32_t*>(buffer->buffer()), at, dwords - at, 0,
                        k);
      if (end == End::kBadPacket) {
        end = End::kQueueEnd;  // that buffer's end
      }
    }
  }
  uint64_t reached = ring_position;
  if (end == End::kQueueEnd && write_position > ring_position) {
    end = walker.Walk(reinterpret_cast<const uint32_t*>(ring->buffer()), ring_index,
                      uint32_t(write_position - ring_position), ring_dwords);
    reached = ring_position + walker.stopped();
  }

  if (end == End::kQueueEnd) {
    resume.valid = true;
    resume.position = write_position;
    std::memcpy(resume.registers, live, sizeof(resume.registers));
    resume.vertex_shader = hooks.vertex_shader;
    resume.pixel_shader = hooks.pixel_shader;
    resume.bin_mask = walker.bin_mask();
    resume.bin_select = walker.bin_select();
    resume.bindings = hooks.bindings;
    resume.scene_composited = hooks.scene_composited;
    resume.scene_base = hooks.scene_base;
    resume.pipeline_cache = &pipelines;
    resume.shader_epoch = epoch;
    walked_until = write_position;
  } else {
    resume.valid = false;
    walked_until = std::max(walked_until, reached);
  }
  last_walk_early = end != End::kQueueEnd;
  std::memcpy(live, saved_registers, sizeof(saved_registers));

  const auto walk_end = std::chrono::steady_clock::now();
  last_walk_time = walk_end;
  ++stats.walks;
  stats.walk_packets += walker.packets();
  stats.walk_draws += hooks.draws;
  stats.walk_unmet_waits += walker.unmet_waits();
  stats.queued_ahead += hooks.queued;
  ++stats.ends[uint32_t(end)];
  stats.walk_ms += std::chrono::duration<double, std::milli>(walk_end - walk_start).count();
}

namespace fh1_pipeline_lookahead {

void LogSummary() {
  static auto last_log = std::chrono::steady_clock::time_point{};
  static uint64_t last_created = 0, last_skipped_draws = 0, last_placeholders = 0;
  const auto now = std::chrono::steady_clock::now();
  if (now - last_log < std::chrono::seconds(10)) {
    return;
  }
  uint64_t created = stats.created.load(std::memory_order_relaxed);
  uint64_t placeholders = stats.placeholders.load(std::memory_order_relaxed);
  if (created == last_created && stats.skipped_draws == last_skipped_draws &&
      placeholders == last_placeholders) {
    return;
  }
  last_log = now;
  last_created = created;
  last_skipped_draws = stats.skipped_draws;
  last_placeholders = placeholders;
  std::string ends;
  for (uint32_t i = 0; i < uint32_t(End::kCount); ++i) {
    if (stats.ends[i]) {
      ends += fmt::format("{}{} {}", ends.empty() ? "" : ", ", EndName(End(i)), stats.ends[i]);
    }
  }
  REXGPU_INFO(
      "Pipelines: {} built, {} of them ahead of their draws ({} drawn with so far, {} failed), "
      "{} placeholders; {} draws skipped and {} of {} frames not shown while pipelines were "
      "built ({} of them past the hold for a draw that must not be missing: skipped depth only "
      "{}, screen passes {}, front buffer or HUD {}, VR stereo {}; {} shown without draws; {} "
      "copied back for the CPU without draws); lookahead: {} walks ({}), {} packets, {} draws, "
      "{} queued, {} unmet waits, {:.1f} ms; {} waits {:.1f} ms",
      created, stats.created_ahead.load(std::memory_order_relaxed), stats.drawn_ahead,
      stats.failed_ahead.load(std::memory_order_relaxed), placeholders, stats.skipped_draws,
      stats.skipped_frames, stats.frames, stats.held_for_stale_contents, stats.skipped_depth,
      stats.skipped_screen_passes, stats.skipped_front_buffer, stats.skipped_stereo,
      stats.incomplete_frames_shown, stats.incomplete_readbacks, stats.walks,
      ends.empty() ? "none" : ends,
      stats.walk_packets, stats.walk_draws, stats.queued_ahead, stats.walk_unmet_waits,
      stats.walk_ms, stats.waits,
      stats.wait_ms);
}

}  // namespace fh1_pipeline_lookahead

}  // namespace rex::graphics::vulkan
