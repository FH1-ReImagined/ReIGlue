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

#include "fh1_command_cursor.h"
#include "fh1_frame_cost.h"
#include "fh1_vr_stereo.h"
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include <rex/chrono/clock.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/sampler_info.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>

#if REX_PLATFORM_LINUX
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <climits>
#elif REX_PLATFORM_WIN32
// synchapi.h, declared here to keep windows.h's macros out of this file.
extern "C" __declspec(dllimport) void __stdcall WakeByAddressAll(void* address);
#pragma comment(lib, "synchronization.lib")
#endif

namespace {

// Wakes the threads blocked on a guest word the command processor just wrote. The CPU's waits
// for the GPU poll such words (fences, the scratch register and read pointer write-backs); a
// game can block on the word until it changes instead (Forza's src/hooks/gpu_wait.cpp). The
// address is the word's host address in the physical view (TranslatePhysical), as here.
inline void WakeWordWaiters(void* host_address) {
#if REX_PLATFORM_LINUX
  syscall(SYS_futex, host_address, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
#elif REX_PLATFORM_WIN32
  WakeByAddressAll(host_address);
#else
  (void)host_address;
#endif
}

}  // namespace

REXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");

// Off as in Xenia, where it is a fix for Team Ninja games (missing character models). On, every
// page the CPU wrote and the GPU read is uploaded again after every frame, written or not: in a
// Forza race that is ~500,000 pages (2 GB) a second of copies on the command processor thread.
// Pages the CPU writes are still caught by the write watches and uploaded again when used.
REXCVAR_DEFINE_BOOL(clear_memory_page_state, false, "GPU",
                    "Refresh page-valid state from GPU-written memory at frame end, so that "
                    "everything the CPU wrote is uploaded again (Xenia uses it for Team Ninja "
                    "games). Costs a full re-upload of the frame's CPU-written GPU data.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(occlusion_query_enable, true, "GPU", "Enable host occlusion query handling")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(readback_resolve, "none", "GPU",
                      "Controls CPU readback of render-to-texture resolve results.\n"
                      " none: Disable readback (default)\n"
                      " fast: Read previous frame (delayed, copy every frame)\n"
                      " some: Read previous frame (delayed, copy on cache miss)\n"
                      " full: Immediate sync readback (accurate but stalls)")
    .allowed({"none", "fast", "some", "full"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT64(readback_resolve_wait_range, 0, "GPU",
                      "readback_resolve some and fast: resolves into this guest physical range, "
                      "(address << 32) | length in bytes (0: none), wait for the GPU and are "
                      "copied back as with full. A game sets it while the CPU reads what it "
                      "resolves there (Forza Horizon's photo mode Big Shot reads each of its tiles "
                      "from a front buffer, which the game resolves every frame).");

REXCVAR_DEFINE_BOOL(readback_resolve_half_pixel_offset, false, "GPU",
                    "When draw resolution scaling is active, sample from the center of each "
                    "scaled block during resolve readback downscale")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Default false as in Xenia.
REXCVAR_DEFINE_BOOL(readback_memexport, false, "GPU",
                    "Enable CPU readback of shader memexport writes for guest memory "
                    "coherency (can reduce correctness issues, but may add GPU/CPU sync cost)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_memexport_fast, true, "GPU",
                    "Use fast double-buffered memexport readback when possible, with "
                    "automatic fallback to full synchronous readback")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(query_occlusion_fake_sample_count, 1000, "GPU",
                     "Fake sample count for occlusion queries")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(async_shader_compilation, true, "GPU",
                    "Compile shaders and create pipelines asynchronously in background "
                    "threads. This reduces stutter but may cause brief visual artifacts while "
                    "pipelines are being prepared.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(sync_pipeline_creation, false, "GPU",
                    "While set, draws wait for their real pipeline (created at once, or waited "
                    "for when it is queued) instead of being skipped with a placeholder, "
                    "whatever async_shader_compilation says. A game sets it around a render that "
                    "is made once and read back (a car's picture), which would otherwise lose "
                    "the skipped draws for good.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// FH1 (#474), after Pinyon Shift's gpu_record_thread (ShiftGlue 9362a07, BSD-3).
REXCVAR_DEFINE_BOOL(gpu_split_command_processor, false, "GPU",
                    "Decode the game's GPU commands on one thread and apply them (register "
                    "writes, draws, copies, swaps, fences) on a second one, \"GPU Recorder\", "
                    "for CPUs whose GPU command thread sets the frame rate (Steam Deck, 4 "
                    "cores). Vulkan only; VR's true stereo is off while it is on")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// After Pinyon Shift (ShiftGlue 9bf779f, BSD-3).
REXCVAR_DEFINE_BOOL(gpu_split_elide_unchanged_registers, true, "GPU",
                    "With gpu_split_command_processor: the decoder records only the context "
                    "and shader constant registers whose value changes (the game rewrites most "
                    "of them with the values they have)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Patch 0391 (src/ui/vulkan/vulkan_presenter.cpp, the runtime): the next guest output's tag.
extern "C" void rexui_SetNextGuestOutputTag(uint32_t tag);

namespace rex::graphics {

using namespace rex::graphics::xenos;

namespace {

ReadbackResolveMode ParseReadbackResolveMode(std::string_view value) {
  if (value == "fast") {
    return ReadbackResolveMode::kFast;
  }
  if (value == "some") {
    return ReadbackResolveMode::kSome;
  }
  if (value == "full") {
    return ReadbackResolveMode::kFull;
  }
  return ReadbackResolveMode::kDisabled;
}

}  // namespace

CommandProcessor::CommandProcessor(GraphicsSystem* graphics_system,
                                   system::KernelState* kernel_state)
    : memory_(graphics_system->memory()),
      kernel_state_(kernel_state),
      graphics_system_(graphics_system),
      register_file_(graphics_system_->register_file()),
      worker_running_(true),
      write_ptr_index_event_(rex::thread::Event::CreateAutoResetEvent(false)),
      vblank_event_(rex::thread::Event::CreateAutoResetEvent(false)),
      write_ptr_index_(0) {
  assert_not_null(write_ptr_index_event_);
  assert_not_null(vblank_event_);
}

CommandProcessor::~CommandProcessor() = default;

bool CommandProcessor::Initialize() {
  // Initialize the gamma ramps to their default (linear) values - taken from
  // what games set when starting with the sRGB (return value 1)
  // VdGetCurrentDisplayGamma.
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i * 0x3FF / 0xFF;
    reg::DC_LUT_30_COLOR& gamma_ramp_entry = gamma_ramp_256_entry_table_[i];
    gamma_ramp_entry.color_10_blue = value;
    gamma_ramp_entry.color_10_green = value;
    gamma_ramp_entry.color_10_red = value;
  }
  for (uint32_t i = 0; i < 128; ++i) {
    reg::DC_LUT_PWL_DATA gamma_ramp_entry = {};
    gamma_ramp_entry.base = (i * 0xFFFF / 0x7F) & ~UINT32_C(0x3F);
    gamma_ramp_entry.delta = i < 0x7F ? 0x200 : 0;
    for (uint32_t j = 0; j < 3; ++j) {
      gamma_ramp_pwl_rgb_[i][j] = gamma_ramp_entry;
    }
  }

  worker_running_ = true;
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));
  worker_thread_->set_name("GPU Commands");
  worker_thread_->Create();

  return true;
}

void CommandProcessor::Shutdown() {
  worker_running_ = false;
  write_ptr_index_event_->Set();
  worker_thread_->Wait(0, 0, 0, nullptr);
  worker_thread_.reset();
}

void CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                               uint32_t title_id, bool blocking) {}

void CommandProcessor::CallInThread(std::function<void()> fn) {
  if (pending_fns_.empty() && system::XThread::IsInThread(worker_thread_.get())) {
    fn();
  } else {
    pending_fns_.push(std::move(fn));
  }
}

void CommandProcessor::ClearCaches() {}

void CommandProcessor::InvalidateGpuMemory() {}

ReadbackResolveMode CommandProcessor::GetReadbackResolveMode(
    bool legacy_readback_resolve_enabled) const {
  ReadbackResolveMode shared_mode = ParseReadbackResolveMode(REXCVAR_GET(readback_resolve));
  bool shared_mode_overrides_legacy = shared_mode != ReadbackResolveMode::kDisabled ||
                                      rex::cvar::HasNonDefaultValue("readback_resolve");
  if (shared_mode_overrides_legacy) {
    return shared_mode;
  }
  return legacy_readback_resolve_enabled ? ReadbackResolveMode::kFast
                                         : ReadbackResolveMode::kDisabled;
}

// 0411 (#552): "some" copies a range back only when its key is new, and "fast" a frame late;
// a range the game resolves every frame and now and then reads on the CPU needs the wait.
bool CommandProcessor::IsReadbackWaitRange(uint32_t written_address,
                                           uint32_t written_length) const {
  const uint64_t range = REXCVAR_GET(readback_resolve_wait_range);
  const uint64_t start = range >> 32, length = uint32_t(range);
  return length && written_address < start + length &&
         start < uint64_t(written_address) + written_length;
}

uint64_t CommandProcessor::GetReadbackResolveKey(uint32_t written_address,
                                                 uint32_t written_length) const {
  const RegisterFile& regs = *register_file_;
  reg::RB_COPY_CONTROL copy_control = regs.Get<reg::RB_COPY_CONTROL>();
  reg::RB_COPY_DEST_INFO dest_info = regs.Get<reg::RB_COPY_DEST_INFO>();
  // Not the exponent bias, the EDRAM base or the size: they may change from frame to frame
  // in the same pass, and each change would be a GPU wait.
  uint64_t signature = uint64_t(copy_control.copy_src_select >= 4) |
                       (uint64_t(dest_info.copy_dest_format) << 1) |
                       (uint64_t(dest_info.copy_dest_number) << 7) |
                       (uint64_t(dest_info.copy_dest_endian) << 10) |
                       (uint64_t(dest_info.copy_dest_swap) << 13) |
                       (uint64_t(dest_info.copy_dest_array) << 14) |
                       (uint64_t(dest_info.copy_dest_slice) << 15);
  // Distinct keys for distinct signatures; signature 0 (a color copy to k_1_REVERSE) keeps
  // the plain (address, length) key.
  return ((uint64_t(written_address) << 32) | uint64_t(written_length)) ^
         (signature * UINT64_C(0x9E3779B97F4A7C15));
}

bool CommandProcessor::IsReadbackMemexportEnabled(bool legacy_backend_flag) const {
  if (legacy_readback_memexport_cvar_name_ &&
      rex::cvar::HasNonDefaultValue(legacy_readback_memexport_cvar_name_)) {
    return legacy_backend_flag;
  }
  return REXCVAR_GET(readback_memexport);
}

void CommandProcessor::SetDesiredSwapPostEffect(SwapPostEffect swap_post_effect) {
  if (swap_post_effect_desired_ == swap_post_effect) {
    return;
  }
  swap_post_effect_desired_ = swap_post_effect;
  CallInThread([this, swap_post_effect]() { swap_post_effect_actual_ = swap_post_effect; });
}

void CommandProcessor::WorkerThreadMain() {
  if (!SetupContext()) {
    rex::FatalError("Unable to setup command processor internal state");
    return;
  }

  if (REXCVAR_GET(gpu_split_command_processor)) {
    if (SupportsRecordThread()) {
      StartRecordThread();
    } else {
      REXGPU_INFO("gpu_split_command_processor: not supported by this GPU backend, ignored");
    }
  }

  while (worker_running_) {
    while (!pending_fns_.empty()) {
      auto fn = std::move(pending_fns_.front());
      pending_fns_.pop();
      // #474: backend work (cache clears, the shader storage, the post effect), or Pause, which
      // suspends this thread: run here once the recorder has run everything before it.
      if (record_split_) {
        RecordSync(kSyncCallInThread);
      }
      fn();
    }

    uint32_t write_ptr_index = write_ptr_index_.load();
    if (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index) {
      SCOPE_profile_cpu_i("gpu", "rex::graphics::CommandProcessor::Stall");
      // We've run out of commands to execute.
      // We spin here waiting for new ones, as the overhead of waiting on our
      // event is too high.
      PublishRecordBatch();
      PrepareForWait();
      uint32_t loop_count = 0;
      do {
        // If we spin around too much, revert to a "low-power" state.
        if (loop_count > 500) {
          const int wait_time_ms = 5;
          rex::thread::Wait(write_ptr_index_event_.get(), true,
                            std::chrono::milliseconds(wait_time_ms));
        }

        rex::thread::MaybeYield();
        loop_count++;
        write_ptr_index = write_ptr_index_.load();
      } while (worker_running_ && pending_fns_.empty() &&
               (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index));
      ReturnFromWait();
      if (!worker_running_ || !pending_fns_.empty()) {
        continue;
      }
    }
    assert_true(read_ptr_index_ != write_ptr_index);

    // Execute. Note that we handle wraparound transparently.
    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);

    // TODO(benvanik): use reader->Read_update_freq_ and only issue after moving
    //     that many indices.
    // #474: split, what was decoded goes to the recorder; the read pointer is written at once,
    // as Pinyon Shift does: the decoder has copied what it needs from the ring (the game may
    // reuse it now), and the game's resources are guarded by the fences, which are recorded.
    PublishRecordBatch();
    if (read_ptr_writeback_ptr_) {
      auto* writeback = memory_->TranslatePhysical(read_ptr_writeback_ptr_);
      memory::store_and_swap<uint32_t>(writeback, read_ptr_index_);
      WakeWordWaiters(writeback);
    }

    // FIXME: We're supposed to process the WAIT_UNTIL register at this point,
    // but no games seem to actually use it.
  }

  StopRecordThread();
  ShutdownContext();
}

// FH1 (#474): the decode/record split. The idea, its ordering rules and the shadow register
// file are Pinyon Shift's gpu_record_thread (ShiftGlue 9362a07, BSD-3; draws as plain records
// as in its 7440c33); this is written for ReIGlue's command processor.

namespace {
inline void SpinPause() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield");
#endif
}
// About 20-40 us of polling before a thread sleeps on the other: the gaps between batches are
// mostly shorter than a sleep and a wakeup.
constexpr uint32_t kRecordSpin = 1024;
// Published batches the recorder has not run yet, at most: the decoder waits beyond that.
constexpr uint64_t kRecordMaxQueued = 512;
// A batch is published after this many draws (and other backend work) or words.
constexpr uint32_t kRecordBatchActions = 32;
constexpr size_t kRecordBatchWords = 32768;
}  // namespace

void CommandProcessor::StartRecordThread() {
  decode_register_file_ = std::make_unique<RegisterFile>();
  std::memcpy(decode_register_file_->values, register_file_->values,
              sizeof(decode_register_file_->values));
  record_batch_ = std::make_unique<RecordBatch>();
  record_elide_ = REXCVAR_GET(gpu_split_elide_unchanged_registers);
  record_stop_ = false;
  record_published_ = 0;
  record_published_shared_ = 0;
  record_completed_ = 0;
  record_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        RecordThreadMain();
        return 0;
      }));
  record_thread_->set_name("GPU Recorder");
  record_thread_->Create();
  record_split_ = true;
  REXGPU_INFO("gpu_split_command_processor: GPU commands decoded on this thread, applied on "
              "\"GPU Recorder\"");
}

void CommandProcessor::StopRecordThread() {
  if (!record_split_) {
    return;
  }
  RecordSync(kSyncCallInThread);
  {
    std::lock_guard<std::mutex> lock(record_mutex_);
    record_stop_ = true;
  }
  record_ready_cond_.notify_all();
  record_thread_->Wait(0, 0, 0, nullptr);
  record_thread_.reset();
  record_split_ = false;
}

void CommandProcessor::RecordThreadMain() {
  uint64_t taken = 0;
  for (;;) {
    for (uint32_t i = 0;
         i < kRecordSpin && record_published_shared_.load(std::memory_order_acquire) == taken; ++i) {
      SpinPause();
    }
    std::unique_ptr<RecordBatch> batch;
    {
      std::unique_lock<std::mutex> lock(record_mutex_);
      if (record_queue_.empty()) {
        if (record_stop_) {
          return;
        }
        record_recorder_sleeping_ = true;
        record_ready_cond_.wait(lock, [this]() { return record_stop_ || !record_queue_.empty(); });
        record_recorder_sleeping_ = false;
        if (record_queue_.empty()) {
          return;
        }
      }
      batch = std::move(record_queue_.front());
      record_queue_.pop_front();
    }
    ++taken;
    ExecuteRecordBatch(*batch);
    batch->words.clear();
    batch->fns.clear();
    batch->actions = 0;
    bool wake;
    {
      std::lock_guard<std::mutex> lock(record_mutex_);
      record_free_.push_back(std::move(batch));
      record_completed_.store(taken, std::memory_order_release);
      wake = record_decoder_waiting_;
    }
    if (wake) {
      record_done_cond_.notify_one();
    }
  }
}

void CommandProcessor::ExecuteRecordBatch(RecordBatch& batch) {
  const uint32_t* words = batch.words.data();
  const size_t count = batch.words.size();
  for (size_t i = 0; i < count;) {
    switch (words[i]) {
      case kRecordRun: {
        const uint32_t start = words[i + 1], run = words[i + 2];
        WriteRegistersFromMem(start, const_cast<uint32_t*>(words + i + 3), run);
        i += 3 + run;
      } break;
      case kRecordOne:
        WriteRegister(words[i + 1], words[i + 2]);
        i += 3;
        break;
      case kRecordDraw: {
        DrawRecord draw;
        std::memcpy(&draw, words + i + 1, sizeof(draw));
        ExecuteDraw(draw);
        i += 1 + sizeof(DrawRecord) / sizeof(uint32_t);
      } break;
      case kRecordShader: {
        const auto shader_type = static_cast<xenos::ShaderType>(words[i + 1]);
        const uint32_t address = words[i + 2], dwords = words[i + 3];
        Shader* shader =
            LoadShader(shader_type, address, memory_->TranslatePhysical<uint32_t*>(address), dwords);
        (shader_type == xenos::ShaderType::kVertex ? active_vertex_shader_ : active_pixel_shader_) =
            shader;
        i += 4;
      } break;
      case kRecordShaderImmediate: {
        const auto shader_type = static_cast<xenos::ShaderType>(words[i + 1]);
        const uint32_t dwords = words[i + 2];
        // The guest address is the ring's, which the backends do not use.
        Shader* shader = LoadShader(shader_type, 0, words + i + 3, dwords);
        (shader_type == xenos::ShaderType::kVertex ? active_vertex_shader_ : active_pixel_shader_) =
            shader;
        i += 3 + dwords;
      } break;
      case kRecordStore: {
        auto* host = memory_->TranslatePhysical(words[i + 1]);
        memory::store(host, words[i + 2]);
        WakeWordWaiters(host);
        i += 3;
      } break;
      case kRecordInterrupt: {
        const uint32_t cpu_mask = words[i + 1];
        for (int n = 0; n < 6; n++) {
          if ((cpu_mask & (1 << n)) && graphics_system_) {
            graphics_system_->DispatchInterruptCallback(1, n);
          }
        }
        i += 2;
      } break;
      case kRecordCall:
        batch.fns[words[i + 1]]();
        i += 2;
        break;
      default:
        assert_always();
        return;
    }
  }
}

uint32_t* CommandProcessor::RecordWords(uint32_t count) {
  std::vector<uint32_t>& words = record_batch_->words;
  const size_t offset = words.size();
  words.resize(offset + count);
  return words.data() + offset;
}

void CommandProcessor::RecordAction() {
  if (++record_batch_->actions >= kRecordBatchActions) {
    PublishRecordBatch();
  }
}

namespace {
// gpu_split_elide_unchanged_registers: from here on the registers are context state and shader
// constants, which the packets alone write and whose write only stores the value (the backend
// derives everything else from the values). The initiators are recorded whatever their value.
constexpr uint32_t kElideFirst = 0x2000;
inline bool ElidableOne(uint32_t index) {
  return index >= kElideFirst && index < RegisterFile::kRegisterCount &&
         index != XE_GPU_REG_VGT_EVENT_INITIATOR && index != XE_GPU_REG_VGT_DRAW_INITIATOR;
}
// Unchanged registers between two changed ones that are recorded with them rather than starting
// a new run (a run costs more than storing a few values again).
constexpr uint32_t kElideGap = 8;
}  // namespace

void CommandProcessor::PacketWriteRegister(uint32_t index, uint32_t value) {
  if (!record_split_) {
    WriteRegister(index, value);
    return;
  }
  ++record_registers_in_;
  if (record_elide_ && ElidableOne(index) && decode_register_file_->values[index] == value) {
    return;
  }
  ++record_registers_out_;
  if (index < RegisterFile::kRegisterCount) {
    uint32_t shadow_value = value;
    if (index == XE_GPU_REG_COHER_STATUS_HOST) {
      // As WriteRegister: dirty until a WAIT_REG_MEM makes it coherent.
      shadow_value |= UINT32_C(0x80000000);
    }
    decode_register_file_->values[index] = shadow_value;
  }
  uint32_t* record = RecordWords(3);
  record[0] = kRecordOne;
  record[1] = index;
  record[2] = value;
}

void CommandProcessor::PacketWriteRegistersFromMem(uint32_t start_index, const uint32_t* base,
                                                   uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  if (!record_split_) {
    WriteRegistersFromMem(start_index, const_cast<uint32_t*>(base), num_registers);
    return;
  }
  if (uint64_t(start_index) + num_registers > RegisterFile::kRegisterCount) {
    // Out of the register file: one at a time, as WriteRegistersFromMem does.
    for (uint32_t i = 0; i < num_registers; ++i) {
      PacketWriteRegister(start_index + i, memory::load_and_swap<uint32_t>(base + i));
    }
    return;
  }
  record_registers_in_ += num_registers;
  if (record_elide_ && start_index >= kElideFirst &&
      !(start_index <= XE_GPU_REG_VGT_DRAW_INITIATOR &&
        start_index + num_registers > XE_GPU_REG_VGT_EVENT_INITIATOR)) {
    // After Pinyon Shift (ShiftGlue 9bf779f): only the parts of the range that change a value.
    uint32_t* shadow = decode_register_file_->values + start_index;
    uint32_t i = 0;
    while (i < num_registers) {
      while (i < num_registers && shadow[i] == memory::load_and_swap<uint32_t>(base + i)) {
        ++i;
      }
      if (i >= num_registers) {
        break;
      }
      const uint32_t first = i;
      uint32_t last = i;
      for (uint32_t j = i + 1, gap = 0; j < num_registers; ++j) {
        if (shadow[j] != memory::load_and_swap<uint32_t>(base + j)) {
          last = j;
          gap = 0;
        } else if (++gap > kElideGap) {
          break;
        }
      }
      const uint32_t run = last - first + 1;
      memory::copy_and_swap(shadow + first, base + first, run);
      uint32_t* record = RecordWords(3 + run);
      record[0] = kRecordRun;
      record[1] = start_index + first;
      record[2] = run;
      std::memcpy(record + 3, base + first, sizeof(uint32_t) * run);
      record_registers_out_ += run;
      i = last + 1;
    }
    if (record_batch_->words.size() >= kRecordBatchWords) {
      PublishRecordBatch();
    }
    return;
  }
  record_registers_out_ += num_registers;
  memory::copy_and_swap(decode_register_file_->values + start_index, base, num_registers);
  if (start_index <= XE_GPU_REG_COHER_STATUS_HOST &&
      start_index + num_registers > XE_GPU_REG_COHER_STATUS_HOST) {
    decode_register_file_->values[XE_GPU_REG_COHER_STATUS_HOST] |= UINT32_C(0x80000000);
  }
  uint32_t* record = RecordWords(3 + num_registers);
  record[0] = kRecordRun;
  record[1] = start_index;
  record[2] = num_registers;
  std::memcpy(record + 3, base, sizeof(uint32_t) * num_registers);
  if (record_batch_->words.size() >= kRecordBatchWords) {
    PublishRecordBatch();
  }
}

void CommandProcessor::PacketWriteRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  const size_t range_bytes = size_t(num_registers) * sizeof(uint32_t);
  const size_t read_offset = ring->read_offset();
  if (read_offset + range_bytes < ring->capacity()) {
    PacketWriteRegistersFromMem(
        base, reinterpret_cast<const uint32_t*>(ring->buffer() + read_offset), num_registers);
    ring->set_read_offset_within(read_offset + range_bytes);
    return;
  }
  memory::RingBuffer::ReadRange range = ring->BeginRead(range_bytes);
  if (range.first_length != 0) {
    uint32_t first_count = uint32_t(range.first_length / sizeof(uint32_t));
    PacketWriteRegistersFromMem(base, reinterpret_cast<const uint32_t*>(range.first), first_count);
    base += first_count;
  }
  if (range.second_length != 0) {
    PacketWriteRegistersFromMem(base, reinterpret_cast<const uint32_t*>(range.second),
                                uint32_t(range.second_length / sizeof(uint32_t)));
  }
  ring->EndRead(range);
}

uint32_t CommandProcessor::DecoderRegisterValue(uint32_t index) {
  if (!record_split_) {
    return ReadRegisterValue(index);
  }
  if (index >= 0x2000 && index < RegisterFile::kRegisterCount) {
    // The context registers and the constant blocks (0x2000 on): written by packets only.
    return decode_register_file_->values[index];
  }
  // The others may be written by the CPU (MMIO) or by the backend (coherency status, the gamma
  // ramp's index): the register file's value, once the recorder has run what came before.
  RecordSync(kSyncRegisterRead);
  const uint32_t value = ReadRegisterValue(index);
  if (index < RegisterFile::kRegisterCount) {
    decode_register_file_->values[index] = value;
  }
  return value;
}

void CommandProcessor::RecordCall(std::function<void()> fn) {
  if (!record_split_) {
    fn();
    return;
  }
  RecordBatch& batch = *record_batch_;
  uint32_t* record = RecordWords(2);
  record[0] = kRecordCall;
  record[1] = uint32_t(batch.fns.size());
  batch.fns.push_back(std::move(fn));
  RecordAction();
}

void CommandProcessor::RecordStore(uint32_t address, uint32_t guest_value) {
  uint32_t* record = RecordWords(3);
  record[0] = kRecordStore;
  record[1] = address;
  record[2] = guest_value;
}

void CommandProcessor::PublishRecordBatch() {
  if (!record_split_ || record_batch_->words.empty()) {
    return;
  }
  if (record_published_ - record_completed_.load(std::memory_order_acquire) >=
      kRecordMaxQueued) {
    // Far ahead of the recorder: let it catch up halfway first (the queue's memory).
    ++record_syncs_[kSyncQueueFull];
    const uint64_t target = record_published_ - kRecordMaxQueued / 2;
    std::unique_lock<std::mutex> lock(record_mutex_);
    record_decoder_waiting_ = true;
    record_done_cond_.wait(lock, [&]() {
      return record_completed_.load(std::memory_order_acquire) >= target;
    });
    record_decoder_waiting_ = false;
  }
  std::unique_ptr<RecordBatch> next;
  bool wake;
  {
    std::lock_guard<std::mutex> lock(record_mutex_);
    record_queue_.push_back(std::move(record_batch_));
    ++record_published_;
    record_published_shared_.store(record_published_, std::memory_order_release);
    wake = record_recorder_sleeping_;
    if (!record_free_.empty()) {
      next = std::move(record_free_.back());
      record_free_.pop_back();
    }
  }
  if (wake) {
    record_ready_cond_.notify_one();
  }
  record_batch_ = next ? std::move(next) : std::make_unique<RecordBatch>();
  ++record_batches_;
}

void CommandProcessor::RecordSync(RecordSyncReason reason) {
  if (!record_split_) {
    return;
  }
  ++record_syncs_[reason];
  PublishRecordBatch();
  const uint64_t target = record_published_;
  for (uint32_t i = 0;
       i < kRecordSpin && record_completed_.load(std::memory_order_acquire) < target; ++i) {
    SpinPause();
  }
  if (record_completed_.load(std::memory_order_acquire) >= target) {
    // The recorder's stores before it are visible (the release in RecordThreadMain).
    std::atomic_thread_fence(std::memory_order_acquire);
    return;
  }
  std::unique_lock<std::mutex> lock(record_mutex_);
  record_decoder_waiting_ = true;
  record_done_cond_.wait(
      lock, [&]() { return record_completed_.load(std::memory_order_acquire) >= target; });
  record_decoder_waiting_ = false;
}

void CommandProcessor::RecordWaitProgress() {
  if (wait_progress_first_) {
    ++record_syncs_[kSyncWaitMemory];
  }
  PublishRecordBatch();
  const uint64_t completed = record_completed_.load(std::memory_order_acquire);
  if (completed >= record_published_) {
    return;
  }
  for (uint32_t i = 0;
       i < kRecordSpin && record_completed_.load(std::memory_order_acquire) == completed; ++i) {
    SpinPause();
  }
  if (record_completed_.load(std::memory_order_acquire) != completed) {
    return;
  }
  std::unique_lock<std::mutex> lock(record_mutex_);
  record_decoder_waiting_ = true;
  record_done_cond_.wait_for(lock, std::chrono::microseconds(100), [&]() {
    return record_completed_.load(std::memory_order_acquire) != completed;
  });
  record_decoder_waiting_ = false;
}

void CommandProcessor::LogRecordSplit() {
  // Once a minute at 30 fps: how often the decoder waited for the recorder, by reason.
  if (++record_swaps_ % 1800 != 0) {
    return;
  }
  static const char* const names[kSyncReasonCount] = {
      "mem_write", "cond_write", "reg_to_mem", "occlusion", "wait_register",
      "wait_memory", "register_read", "call", "queue_full"};
  std::string line;
  for (uint32_t i = 0; i < kSyncReasonCount; ++i) {
    if (record_syncs_[i]) {
      line += fmt::format(" {}={:.2f}", names[i], double(record_syncs_[i]) / 1800.0);
    }
    record_syncs_[i] = 0;
  }
  if (record_sync_register_) {
    line += fmt::format(" (last register waited for {:04X})", record_sync_register_);
  }
  REXGPU_INFO("gpu_split_command_processor: {:.1f} batches a frame, {:.0f} of {:.0f} register "
              "writes recorded a frame; waits for the recorder a frame:{}",
              double(record_batches_) / 1800.0, double(record_registers_out_) / 1800.0,
              double(record_registers_in_) / 1800.0, line.empty() ? std::string(" none") : line);
  record_batches_ = 0;
  record_registers_in_ = record_registers_out_ = 0;
}

void CommandProcessor::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  thread::Fence fence;
  CallInThread([&fence]() {
    fence.Signal();
    thread::Thread::GetCurrentThread()->Suspend();
  });

  fence.Wait();
}

void CommandProcessor::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  worker_thread_->thread()->Resume();
}

bool CommandProcessor::Save(::rex::stream::ByteStream* stream) {
  assert_true(paused_);

  stream->Write<uint32_t>(primary_buffer_ptr_);
  stream->Write<uint32_t>(primary_buffer_size_);
  stream->Write<uint32_t>(read_ptr_index_);
  stream->Write<uint32_t>(read_ptr_update_freq_);
  stream->Write<uint32_t>(read_ptr_writeback_ptr_);
  stream->Write<uint32_t>(write_ptr_index_.load());

  return true;
}

bool CommandProcessor::Restore(::rex::stream::ByteStream* stream) {
  assert_true(paused_);

  primary_buffer_ptr_ = stream->Read<uint32_t>();
  primary_buffer_size_ = stream->Read<uint32_t>();
  read_ptr_index_ = stream->Read<uint32_t>();
  read_ptr_update_freq_ = stream->Read<uint32_t>();
  read_ptr_writeback_ptr_ = stream->Read<uint32_t>();
  write_ptr_index_.store(stream->Read<uint32_t>());

  return true;
}

bool CommandProcessor::SetupContext() {
  return true;
}

void CommandProcessor::ShutdownContext() {}

void CommandProcessor::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  read_ptr_index_ = 0;
  primary_buffer_ptr_ = ptr;
  primary_buffer_size_ = uint32_t(1) << (size_log2 + 3);
}

void CommandProcessor::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  // CP_RB_RPTR_ADDR Ring Buffer Read Pointer Address 0x70C
  // ptr = RB_RPTR_ADDR, pointer to write back the address to.
  read_ptr_writeback_ptr_ = ptr;
  // CP_RB_CNTL Ring Buffer Control 0x704
  // block_size = RB_BLKSZ, log2 of number of quadwords read between updates of
  //              the read pointer.
  read_ptr_update_freq_ = uint32_t(1) << block_size_log2 >> 2;
}

void CommandProcessor::UpdateWritePointer(uint32_t value) {
  write_ptr_index_ = value;
  write_ptr_index_event_->Set();
}

uint32_t CommandProcessor::ReadRegisterValue(uint32_t index) const {
  if (index < RegisterFile::kRegisterCount) {
    return register_file_->values[index];
  }
  auto it = extended_register_values_.find(index);
  return it != extended_register_values_.end() ? it->second : 0;
}

namespace {
// RegisterFile::GetRegisterInfo is a switch over every known register. Calling it
// on each register write, only to log writes to unknown registers, cost more than
// the rest of the write; look the registers up once instead.
class KnownRegisters {
 public:
  KnownRegisters() {
    for (uint32_t i = 0; i < RegisterFile::kRegisterCount; ++i) {
      if (RegisterFile::GetRegisterInfo(i)) {
        bits_[i >> 6] |= uint64_t(1) << (i & 63);
      }
    }
  }
  bool IsKnown(uint32_t index) const { return (bits_[index >> 6] >> (index & 63)) & 1; }

 private:
  uint64_t bits_[(RegisterFile::kRegisterCount + 63) / 64] = {};
};
const KnownRegisters known_registers;
}  // namespace

void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  RegisterFile& regs = *register_file_;
  if (index >= RegisterFile::kRegisterCount) {
    auto [it, inserted] = extended_register_values_.insert_or_assign(index, value);
    (void)it;
    if (inserted) {
      REXGPU_WARN(
          "CommandProcessor::WriteRegister index out of bounds: {} (stored as extended register)",
          index);
    }
    return;
  }

  // Volatile for the WAIT_REG_MEM loop.
  const_cast<volatile uint32_t&>(regs.values[index]) = value;
  // Patch 0396: the right eye's pass repeats state, not the signals to the CPU (scratch
  // write-back) or the gamma ramp's sequential writes, which the left eye's pass did.
  if (fh1_stereo_state_ == Fh1StereoState::kReplaying) [[unlikely]] {
    return;
  }
  if (!known_registers.IsKnown(index)) {
    REXGPU_DEBUG("GPU: Write to unknown register ({:04X} = {:08X})", index, value);
  }

  // The Vulkan backend stores the other registers of a range itself, without this
  // function (PlainRegisters, FH1 patch 0140): a register that gets handling here must
  // be added to its list.
  // Scratch register writeback.
  if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) {
    uint32_t scratch_reg = index - XE_GPU_REG_SCRATCH_REG0;
    if ((1 << scratch_reg) & regs.values[XE_GPU_REG_SCRATCH_UMSK]) {
      // Enabled - write to address.
      uint32_t scratch_addr = regs.values[XE_GPU_REG_SCRATCH_ADDR];
      uint32_t mem_addr = scratch_addr + (scratch_reg * 4);
      auto* host = memory_->TranslatePhysical(mem_addr);
      memory::store_and_swap<uint32_t>(host, value);
      WakeWordWaiters(host);
    }
  } else {
    switch (index) {
      // If this is a COHER register, set the dirty flag.
      // This will block the command processor the next time it WAIT_REG_MEMs
      // and allow us to synchronize the memory.
      case XE_GPU_REG_COHER_STATUS_HOST: {
        const_cast<volatile uint32_t&>(regs.values[index]) |= UINT32_C(0x80000000);
      } break;

      case XE_GPU_REG_DC_LUT_RW_INDEX: {
        // Reset the sequential read / write component index (see the M56
        // DC_LUT_SEQ_COLOR documentation).
        gamma_ramp_rw_component_ = 0;
      } break;

      case XE_GPU_REG_DC_LUT_SEQ_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // DC_LUT_SEQ_COLOR is in the red, green, blue order, but the write
        // enable mask is blue, green, red.
        bool write_gamma_ramp_component = (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                                           (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          // Bits 0:5 are hardwired to zero.
          uint32_t gamma_ramp_seq_color = regs.Get<reg::DC_LUT_SEQ_COLOR>().seq_color >> 6;
          switch (gamma_ramp_rw_component_) {
            case 0:
              gamma_ramp_entry.color_10_red = gamma_ramp_seq_color;
              break;
            case 1:
              gamma_ramp_entry.color_10_green = gamma_ramp_seq_color;
              break;
            case 2:
              gamma_ramp_entry.color_10_blue = gamma_ramp_seq_color;
              break;
          }
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          ++new_gamma_ramp_rw_index.rw_index;
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_PWL_DATA: {
        // Should be in the PWL writing mode.
        assert_not_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // Bit 7 of the index is ignored for PWL.
        uint32_t gamma_ramp_rw_index_pwl = gamma_ramp_rw_index.rw_index & 0x7F;
        // DC_LUT_PWL_DATA is likely in the red, green, blue order because
        // DC_LUT_SEQ_COLOR is, but the write enable mask is blue, green, red.
        bool write_gamma_ramp_component = (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                                           (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_PWL_DATA& gamma_ramp_entry =
              gamma_ramp_pwl_rgb_[gamma_ramp_rw_index_pwl][gamma_ramp_rw_component_];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_PWL_DATA>();
          // Bits 0:5 are hardwired to zero.
          gamma_ramp_entry.base = gamma_ramp_value.base & ~UINT32_C(0x3F);
          gamma_ramp_entry.delta = gamma_ramp_value.delta & ~UINT32_C(0x3F);
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          // TODO(Triang3l): Should this increase beyond 7 bits for PWL?
          // Direct3D 9 explicitly sets rw_index to 0x80 after writing the last
          // PWL entry. However, the DC_LUT_RW_INDEX documentation says that for
          // PWL, the bit 7 is ignored.
          new_gamma_ramp_rw_index.rw_index = (gamma_ramp_rw_index.rw_index & ~UINT32_C(0x7F)) |
                                             ((gamma_ramp_rw_index_pwl + 1) & 0x7F);
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRampPWLValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_30_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        uint32_t gamma_ramp_write_enable_mask = regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 0b111;
        if (gamma_ramp_write_enable_mask) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_30_COLOR>();
          if (gamma_ramp_write_enable_mask & 0b001) {
            gamma_ramp_entry.color_10_blue = gamma_ramp_value.color_10_blue;
          }
          if (gamma_ramp_write_enable_mask & 0b010) {
            gamma_ramp_entry.color_10_green = gamma_ramp_value.color_10_green;
          }
          if (gamma_ramp_write_enable_mask & 0b100) {
            gamma_ramp_entry.color_10_red = gamma_ramp_value.color_10_red;
          }
        }
        // TODO(Triang3l): Should this reset the component write index? If this
        // increase is assumed to behave like a full DC_LUT_RW_INDEX write, it
        // probably should. Currently this also calls WriteRegister for
        // DC_LUT_RW_INDEX, which resets gamma_ramp_rw_component_ as well.
        gamma_ramp_rw_component_ = 0;
        reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
        ++new_gamma_ramp_rw_index.rw_index;
        WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                      rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        if (gamma_ramp_write_enable_mask) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;
    }
  }
}

void CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = memory::load_and_swap<uint32_t>(base + i);
    WriteRegister(start_index + i, data);
  }
}

void CommandProcessor::WriteRegisterRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  if (record_split_) {
    PacketWriteRangeFromRing(ring, base, num_registers);
    return;
  }
  // Nearly every range is contiguous in the ring: write it and advance the read
  // offset here, as BeginRead and EndRead would (their no-wrap case), without two
  // calls into the runtime for each of the hundred thousand ranges of a frame.
  const size_t range_bytes = size_t(num_registers) * sizeof(uint32_t);
  const size_t read_offset = ring->read_offset();
  if (read_offset + range_bytes < ring->capacity()) {
    WriteRegistersFromMem(base, reinterpret_cast<uint32_t*>(ring->buffer() + read_offset),
                          num_registers);
    // FH1 (#45, patch 0360): inside the buffer, so no division.
    ring->set_read_offset_within(read_offset + range_bytes);
    return;
  }
  memory::RingBuffer::ReadRange range = ring->BeginRead(size_t(num_registers) * sizeof(uint32_t));
  if (range.first_length != 0) {
    uint32_t first_count = uint32_t(range.first_length / sizeof(uint32_t));
    WriteRegistersFromMem(base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
                          first_count);
    base += first_count;
  }
  if (range.second_length != 0) {
    WriteRegistersFromMem(base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.second)),
                          uint32_t(range.second_length / sizeof(uint32_t)));
  }
  ring->EndRead(range);
}

bool CommandProcessor::Fh1WritePlainRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                  uint32_t count) {
  // Only a range that does not reach the end of the ring (as WriteRegisterRangeFromRing's
  // direct path), and only if every register in it is in the backend's list: then the
  // backend would only store the values (fh1_plain_registers_). Otherwise nothing is read.
  const uint64_t* const plain = fh1_plain_registers_;
  const uint32_t end = base + count;
  // #474: split, the recorder's WriteRegistersFromMem takes these.
  if (!count || end > RegisterFile::kRegisterCount || record_split_) {
    return false;
  }
  const size_t range_bytes = size_t(count) * sizeof(uint32_t);
  const size_t read_offset = ring->read_offset();
  if (read_offset + range_bytes >= ring->capacity()) {
    return false;
  }
  for (uint32_t i = base; i < end; ++i) {
    if (!((plain[i >> 6] >> (i & 63)) & 1)) {
      return false;
    }
  }
  const uint32_t* source = reinterpret_cast<const uint32_t*>(ring->buffer() + read_offset);
  uint32_t* values = register_file_->values;
  for (uint32_t i = 0; i < count; ++i) {
    // Volatile like CommandProcessor::WriteRegister's store (WAIT_REG_MEM polls).
    const_cast<volatile uint32_t&>(values[base + i]) = memory::load_and_swap<uint32_t>(source + i);
  }
  ring->set_read_offset_within(read_offset + range_bytes);
  return true;
}

void CommandProcessor::WriteALURangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                             uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4000, num_registers);
}

void CommandProcessor::WriteFetchRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                               uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4800, num_registers);
}

void CommandProcessor::WriteBoolRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4900, num_registers);
}

void CommandProcessor::WriteLoopRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4908, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                   uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x2000, num_registers);
}

void CommandProcessor::WriteALURangeFromMem(uint32_t start_index, uint32_t* base,
                                            uint32_t num_registers) {
  PacketWriteRegistersFromMem(start_index + 0x4000, base, num_registers);
}

void CommandProcessor::WriteFetchRangeFromMem(uint32_t start_index, uint32_t* base,
                                              uint32_t num_registers) {
  PacketWriteRegistersFromMem(start_index + 0x4800, base, num_registers);
}

void CommandProcessor::WriteBoolRangeFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  PacketWriteRegistersFromMem(start_index + 0x4900, base, num_registers);
}

void CommandProcessor::WriteLoopRangeFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  PacketWriteRegistersFromMem(start_index + 0x4908, base, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  PacketWriteRegistersFromMem(start_index + 0x2000, base, num_registers);
}

void CommandProcessor::MakeCoherent() {
  SCOPE_profile_cpu_f("gpu");

  // Status host often has 0x01000000 or 0x03000000.
  // This is likely toggling VC (vertex cache) or TC (texture cache).
  // Or, it also has a direction in here maybe - there is probably
  // some way to check for dest coherency (what all the COHER_DEST_BASE_*
  // registers are for).
  // Best docs I've found on this are here:
  // https://web.archive.org/web/20160711162346/https://amd-dev.wpengine.netdna-cdn.com/wordpress/media/2013/10/R6xx_R7xx_3D.pdf
  // https://cgit.freedesktop.org/xorg/driver/xf86-video-radeonhd/tree/src/r6xx_accel.c?id=3f8b6eccd9dba116cc4801e7f80ce21a879c67d2#n454

  // Volatile because this may be called from the WAIT_REG_MEM loop.
  volatile uint32_t* regs_volatile = register_file_->values;
  auto status_host = rex::memory::Reinterpret<reg::COHER_STATUS_HOST>(
      uint32_t(regs_volatile[XE_GPU_REG_COHER_STATUS_HOST]));
  uint32_t base_host = regs_volatile[XE_GPU_REG_COHER_BASE_HOST];
  uint32_t size_host = regs_volatile[XE_GPU_REG_COHER_SIZE_HOST];

  if (!status_host.status) {
    return;
  }

  const char* action = "N/A";
  if (status_host.vc_action_ena && status_host.tc_action_ena) {
    action = "VC | TC";
  } else if (status_host.tc_action_ena) {
    action = "TC";
  } else if (status_host.vc_action_ena) {
    action = "VC";
  }

  // TODO(benvanik): notify resource cache of base->size and type.
  REXGPU_TRACE("Make {:08X} -> {:08X} ({}b) coherent, action = {}", base_host,
               base_host + size_host, size_host, action);

  // Mark coherent.
  regs_volatile[XE_GPU_REG_COHER_STATUS_HOST] = 0;
}

void CommandProcessor::PrepareForWait() {}

void CommandProcessor::ReturnFromWait() {}

uint32_t CommandProcessor::ExecutePrimaryBuffer(uint32_t read_index, uint32_t write_index) {
  SCOPE_profile_cpu_f("gpu");

  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(primary_buffer_ptr_), primary_buffer_size_);
  reader.set_read_offset(read_index * sizeof(uint32_t));
  reader.set_write_offset(write_index * sizeof(uint32_t));
  {
    // FH1 (#378): the pipeline lookahead's view of the ring (fh1_command_cursor.h).
    fh1_command_cursor::cursor.ring_start = read_index;
    fh1_command_cursor::cursor.ring = &reader;
    fh1_command_cursor::Scope cursor_scope(&reader);
    do {
      if (!ExecutePacket(&reader)) {
        // This probably should be fatal - but we're going to continue anyways.
        REXGPU_ERROR("**** PRIMARY RINGBUFFER: Failed to execute packet.");
        assert_always();
        break;
      }
    } while (reader.read_count());
    fh1_command_cursor::cursor.ring = nullptr;
  }
  fh1_command_cursor::cursor.ring_done +=
      (write_index + primary_buffer_size_ / sizeof(uint32_t) - read_index) %
      (primary_buffer_size_ / sizeof(uint32_t));

  if (record_split_) {
    RecordCall([this]() { OnPrimaryBufferEnd(); });
  } else {
    OnPrimaryBufferEnd();
  }

  return write_index;
}

void CommandProcessor::ExecuteIndirectBuffer(uint32_t ptr, uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  fh1_command_cursor::Scope cursor_scope(&reader);  // FH1 (#378)
  do {
    if (!ExecutePacket(&reader)) {
      // Return up a level if we encounter a bad packet.
      REXGPU_ERROR("**** INDIRECT RINGBUFFER: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());
}

void CommandProcessor::ExecutePacket(uint32_t ptr, uint32_t count) {
  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  fh1_command_cursor::Scope cursor_scope(&reader);  // FH1 (#378)
  do {
    if (!ExecutePacket(&reader)) {
      REXGPU_ERROR("**** ExecutePacket: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());
}

namespace {
// Out of line, so the packet loop below needs no stack frame for the log call.
#if defined(_MSC_VER) && !defined(__clang__)
__declspec(noinline) void WarnUninitializedPacket() {
#else
__attribute__((noinline, cold)) void WarnUninitializedPacket() {
#endif
  REXGPU_WARN("GPU packet is CDCDCDCD - probably read uninitialized memory!");
}
}  // namespace


namespace fh1_frame_trace {
// Patch 0396 (VR true stereo): FORZA_GPU_FRAME_TRACE=<file> writes one whole guest frame's packets
// and draws to <file> each time <file>.trigger appears (the trigger is removed): the frame's
// structure (passes, resolves, fences) that the stereo replay builds on. A test aid.
FILE* file = nullptr;
bool armed = false;
uint32_t draws = 0;
inline const char* Path() {
  static const char* path = std::getenv("FORZA_GPU_FRAME_TRACE");
  return path && *path ? path : nullptr;
}
// At a swap: ends a traced frame, or starts one when the trigger file is there.
inline void OnSwap() {
  if (file) {
    std::fprintf(file, "E frame end, %u draws\n", draws);
    std::fclose(file);
    file = nullptr;
  }
  const char* path = Path();
  if (!path) return;
  std::string trigger = std::string(path) + ".trigger";
  if (FILE* t = std::fopen(trigger.c_str(), "rb")) {
    std::fclose(t);
    std::remove(trigger.c_str());
    file = std::fopen(path, "wb");
    draws = 0;
  }
}
}  // namespace fh1_frame_trace

bool CommandProcessor::ExecutePacket(memory::RingBuffer* reader) {
  uint32_t packet = reader->ReadAndSwap<uint32_t>();
  // Zero words and type-2 packets do nothing. The D3D library pads its command
  // buffers with type-2 packets (about two million a second in a race, more than
  // the draws and constant writes together): skip a whole run of them here
  // instead of returning to the caller's loop for each one.
  while (packet == 0 || (packet >> 30) == 0x02) {
    if (!reader->read_count()) {
      return true;
    }
    packet = reader->ReadAndSwap<uint32_t>();
  }

  if (packet == 0xCDCDCDCD) [[unlikely]] {
    WarnUninitializedPacket();
  }

  // Patch 0396: where the packet starts, for the stereo log. FH1 (#607, patch 0435): the read
  // offset only; the division that wraps it is done when the packet is logged, not for each
  // of the about 24 packets of every draw.
  const auto packet_read_offset = reader->read_offset();
  bool result;
  switch (packet >> 30) {
    case 0x03:
      result = ExecutePacketType3(reader, packet);
      break;
    case 0x00:
      result = ExecutePacketType0(reader, packet);
      break;
    default:
      result = ExecutePacketType1(reader, packet);
      break;
  }
  if ((fh1_stereo_state_ == Fh1StereoState::kSpeculative ||
       fh1_stereo_state_ == Fh1StereoState::kRecording) &&
      result) [[unlikely]] {
    const uint32_t packet_start = uint32_t(
        (packet_read_offset + reader->capacity() - sizeof(uint32_t)) % reader->capacity());
    Fh1StereoLogPacket(reader, packet_start, packet);
  }
  return result;
}

bool CommandProcessor::ExecutePacketType0(memory::RingBuffer* reader, uint32_t packet) {
  // Type-0 packet.
  // Write count registers in sequence to the registers starting at
  // (base_index << 2).

  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ExecutePacketType0 overflow (read count {:08X}, packet count {:08X})",
                 reader->read_count(), count * sizeof(uint32_t));
    return false;
  }

  uint32_t base_index = (packet & 0x7FFF);
  uint32_t write_one_reg = (packet >> 15) & 0x1;
  if (!write_one_reg) {
    // FH1 (#45, patch 0360): render state the backend only stores, straight from the ring.
    if (fh1_plain_registers_ && Fh1WritePlainRangeFromRing(reader, base_index, count)) {
      return true;
    }
    // Consecutive registers: as a range, so the backend can take the constant
    // blocks (most of these packets) in one go rather than one virtual
    // WriteRegister call per register.
    WriteRegisterRangeFromRing(reader, base_index, count);
    return true;
  }
  // FH1 (#45, patch 0360): every word to one register the backend only stores (the last one
  // stays), without a virtual call for each.
  if (fh1_plain_registers_ && !record_split_ && base_index < RegisterFile::kRegisterCount &&
      ((fh1_plain_registers_[base_index >> 6] >> (base_index & 63)) & 1)) {
    volatile uint32_t& value = const_cast<volatile uint32_t&>(register_file_->values[base_index]);
    for (uint32_t m = 0; m < count; m++) {
      value = reader->ReadAndSwap<uint32_t>();
    }
    return true;
  }
  for (uint32_t m = 0; m < count; m++) {
    uint32_t reg_data = reader->ReadAndSwap<uint32_t>();
    PacketWriteRegister(base_index, reg_data);
  }

  return true;
}

bool CommandProcessor::ExecutePacketType1(memory::RingBuffer* reader, uint32_t packet) {
  // Type-1 packet.
  // Contains two registers of data. Type-0 should be more common.
  uint32_t reg_index_1 = packet & 0x7FF;
  uint32_t reg_index_2 = (packet >> 11) & 0x7FF;
  uint32_t reg_data_1 = reader->ReadAndSwap<uint32_t>();
  uint32_t reg_data_2 = reader->ReadAndSwap<uint32_t>();
  PacketWriteRegister(reg_index_1, reg_data_1);
  PacketWriteRegister(reg_index_2, reg_data_2);
  return true;
}

bool CommandProcessor::ExecutePacketType2(memory::RingBuffer* reader, uint32_t packet) {
  // Type-2 packet.
  // No-op. Do nothing.
  return true;
}

bool CommandProcessor::ExecutePacketType3(memory::RingBuffer* reader, uint32_t packet) {
  // Type-3 packet.
  uint32_t opcode = (packet >> 8) & 0x7F;
  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  auto data_start_offset = reader->read_offset();

  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ExecutePacketType3 overflow (read count {:08X}, packet count {:08X})",
                 reader->read_count(), count * sizeof(uint32_t));
    return false;
  }

  // & 1 == predicate - when set, we do bin check to see if we should execute
  // the packet. Only type 3 packets are affected.
  // We also skip predicated swaps, as they are never valid (probably?).
  if (packet & 1) {
    bool any_pass = (bin_select_ & bin_mask_) != 0;
    if (!any_pass || opcode == PM4_XE_SWAP) {
      reader->AdvanceRead(count * sizeof(uint32_t));
      return true;
    }
  }

  if (fh1_frame_trace::file && !record_split_ && opcode != PM4_SET_CONSTANT && opcode != PM4_SET_CONSTANT2 &&
      opcode != PM4_SET_SHADER_CONSTANTS && opcode != PM4_DRAW_INDX && opcode != PM4_DRAW_INDX_2 &&
      opcode != PM4_NOP) {
    memory::RingBuffer peek = *reader;
    std::fprintf(fh1_frame_trace::file, "P %02X n=%u pred=%u", opcode, count, packet & 1);
    for (uint32_t i = 0; i < std::min<uint32_t>(count, 4); ++i)
      std::fprintf(fh1_frame_trace::file, " %08X", peek.ReadAndSwap<uint32_t>());
    std::fprintf(fh1_frame_trace::file, " binsel=%llX binmask=%llX\n",
                 (unsigned long long)bin_select_, (unsigned long long)bin_mask_);
  }
  bool result = false;
  switch (opcode) {
    case PM4_ME_INIT:
      result = ExecutePacketType3_ME_INIT(reader, packet, count);
      break;
    case PM4_NOP:
      result = ExecutePacketType3_NOP(reader, packet, count);
      break;
    case PM4_INTERRUPT:
      result = ExecutePacketType3_INTERRUPT(reader, packet, count);
      break;
    case PM4_XE_SWAP:
      result = ExecutePacketType3_XE_SWAP(reader, packet, count);
      break;
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
      result = ExecutePacketType3_INDIRECT_BUFFER(reader, packet, count);
      break;
    case PM4_WAIT_REG_MEM:
      result = ExecutePacketType3_WAIT_REG_MEM(reader, packet, count);
      break;
    case PM4_REG_RMW:
      result = ExecutePacketType3_REG_RMW(reader, packet, count);
      break;
    case PM4_REG_TO_MEM:
      result = ExecutePacketType3_REG_TO_MEM(reader, packet, count);
      break;
    case PM4_MEM_WRITE:
      result = ExecutePacketType3_MEM_WRITE(reader, packet, count);
      break;
    case PM4_COND_WRITE:
      result = ExecutePacketType3_COND_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE:
      result = ExecutePacketType3_EVENT_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_SHD:
      result = ExecutePacketType3_EVENT_WRITE_SHD(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_EXT:
      result = ExecutePacketType3_EVENT_WRITE_EXT(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_ZPD:
      if (record_split_) {
        // #474: the occlusion query's report is read by the CPU, and the backend's query state
        // follows the draws: run once the recorder has caught up (it then waits).
        RecordSync(kSyncOcclusionQuery);
        result = ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
        decode_register_file_->values[XE_GPU_REG_VGT_EVENT_INITIATOR] =
            register_file_->values[XE_GPU_REG_VGT_EVENT_INITIATOR];
      } else {
        result = ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
      }
      break;
    case PM4_DRAW_INDX:
      result = ExecutePacketType3_DRAW_INDX(reader, packet, count);
      break;
    case PM4_DRAW_INDX_2:
      result = ExecutePacketType3_DRAW_INDX_2(reader, packet, count);
      break;
    case PM4_SET_CONSTANT:
      result = ExecutePacketType3_SET_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_CONSTANT2:
      result = ExecutePacketType3_SET_CONSTANT2(reader, packet, count);
      break;
    case PM4_LOAD_ALU_CONSTANT:
      result = ExecutePacketType3_LOAD_ALU_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_SHADER_CONSTANTS:
      result = ExecutePacketType3_SET_SHADER_CONSTANTS(reader, packet, count);
      break;
    case PM4_IM_LOAD:
      result = ExecutePacketType3_IM_LOAD(reader, packet, count);
      break;
    case PM4_IM_LOAD_IMMEDIATE:
      result = ExecutePacketType3_IM_LOAD_IMMEDIATE(reader, packet, count);
      break;
    case PM4_INVALIDATE_STATE:
      result = ExecutePacketType3_INVALIDATE_STATE(reader, packet, count);
      break;
    case PM4_VIZ_QUERY:
      result = ExecutePacketType3_VIZ_QUERY(reader, packet, count);
      break;

    case PM4_SET_BIN_MASK_LO: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_MASK_HI: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_LO: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_HI: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_MASK: {
      assert_true(count == 2);
      uint64_t val_hi = reader->ReadAndSwap<uint32_t>();
      uint64_t val_lo = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT: {
      assert_true(count == 2);
      uint64_t val_hi = reader->ReadAndSwap<uint32_t>();
      uint64_t val_lo = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_CONTEXT_UPDATE: {
      assert_true(count == 1);
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      REXGPU_INFO("GPU context update = {:08X}", value);
      assert_true(value == 0);
      result = true;
      break;
    }
    case PM4_WAIT_FOR_IDLE: {
      // This opcode is used by 5454084E while going / being ingame.
      assert_true(count == 1);
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      REXGPU_INFO("GPU wait for idle = {:08X}", value);
      result = true;
      break;
    }

    default:
      REXGPU_INFO("Unimplemented GPU OPCODE: 0x{:02X}\t\tCOUNT: {}\n", opcode, count);
      assert_always();
      reader->AdvanceRead(count * sizeof(uint32_t));
      break;
  }

  assert_true(reader->read_offset() ==
              (data_start_offset + (count * sizeof(uint32_t))) % reader->capacity());
  return result;
}

// Patch 0396: VR true stereo by executing the main view's segment of the frame twice
// (fh1_vr_stereo.h).

void CommandProcessor::Fh1StereoLogPacket(memory::RingBuffer* reader, uint32_t start_offset,
                                          uint32_t packet) {
  const uint32_t words = fh1_vr_stereo::PacketWords(packet);
  if (!words) return;
  if ((packet >> 30) == 0x03) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    if (fh1_vr_stereo::SkipsOpcode(opcode)) return;
    if (opcode == PM4_LOAD_ALU_CONSTANT) {
      // The constants as they were loaded: the CPU may rewrite their memory before the right
      // eye's pass. A type-0 packet with the registers' values now.
      memory::RingBuffer peek = *reader;
      peek.set_read_offset(start_offset + sizeof(uint32_t));
      peek.ReadAndSwap<uint32_t>();  // address
      const uint32_t offset_type = peek.ReadAndSwap<uint32_t>();
      const uint32_t size = peek.ReadAndSwap<uint32_t>() & 0xFFF;
      uint32_t first = 0;
      const uint32_t header = fh1_vr_stereo::LoadedConstantsHeader(
          offset_type, size, RegisterFile::kRegisterCount, &first);
      if (!header) return;
      fh1_stereo_log_.push_back(rex::byte_swap(header));
      for (uint32_t i = 0; i < size; ++i)
        fh1_stereo_log_.push_back(rex::byte_swap(register_file_->values[first + i]));
      return;
    }
  }
  const size_t at = fh1_stereo_log_.size();
  fh1_stereo_log_.resize(at + words);
  memory::RingBuffer peek = *reader;
  peek.set_read_offset(start_offset);
  peek.Read(reinterpret_cast<uint8_t*>(fh1_stereo_log_.data() + at), words * sizeof(uint32_t));
}

void CommandProcessor::Fh1StereoOnDraw() {
  const Fh1StereoDraw kind = Fh1StereoClassify();
  auto snapshot = [&]() {
    fh1_stereo_log_.clear();
    if (SharedMemory* shared_memory = Fh1StereoSharedMemory())
      shared_memory->Fh1StartUploadLog(&fh1_stereo_log_);
    fh1_stereo_registers_start_.assign(register_file_->values,
                                       register_file_->values + RegisterFile::kRegisterCount);
    fh1_stereo_shaders_start_[0] = active_vertex_shader_;
    fh1_stereo_shaders_start_[1] = active_pixel_shader_;
    fh1_stereo_bins_start_[0] = bin_select_;
    fh1_stereo_bins_start_[1] = bin_mask_;
  };
  switch (fh1_stereo_state_) {
    case Fh1StereoState::kOff:
      // A clear of a main-view-sized depth buffer may start the scene: log from it on.
      if (kind == Fh1StereoDraw::kClear) {
        snapshot();
        fh1_stereo_state_ = Fh1StereoState::kSpeculative;
      }
      break;
    case Fh1StereoState::kSpeculative:
      if (kind == Fh1StereoDraw::kSegmentStart) {
        fh1_stereo_state_ = Fh1StereoState::kRecording;
        Fh1StereoBegin();
      } else if (kind == Fh1StereoDraw::kClear) {
        snapshot();
      } else {
        fh1_stereo_state_ = Fh1StereoState::kOff;
        fh1_stereo_log_.clear();
        if (SharedMemory* shared_memory = Fh1StereoSharedMemory())
          shared_memory->Fh1StopUploadLog();
      }
      break;
    case Fh1StereoState::kRecording:
      if (kind == Fh1StereoDraw::kSegmentEnd) Fh1StereoReplay();
      break;
    default:
      break;
  }
}

void CommandProcessor::Fh1StereoReplay() {
  fh1_stereo_frame_done_ = true;
  SharedMemory* shared_memory = Fh1StereoSharedMemory();
  if (shared_memory) shared_memory->Fh1StopUploadLog();
  if ((shared_memory && shared_memory->fh1_upload_log_overflow()) || !Fh1StereoLeftDone()) {
    fh1_stereo_state_ = Fh1StereoState::kOff;
    fh1_stereo_log_.clear();
    ++fh1_stereo_dropped_;
    Fh1StereoEnd(false);
    return;
  }
  RegisterFile& regs = *register_file_;
  // The state this draw has, to continue with after the right eye.
  fh1_stereo_registers_end_.assign(regs.values, regs.values + RegisterFile::kRegisterCount);
  Shader* const shaders_end[2] = {active_vertex_shader_, active_pixel_shader_};
  const uint64_t bins_end[2] = {bin_select_, bin_mask_};

  std::memcpy(regs.values, fh1_stereo_registers_start_.data(),
              sizeof(uint32_t) * RegisterFile::kRegisterCount);
  active_vertex_shader_ = fh1_stereo_shaders_start_[0];
  active_pixel_shader_ = fh1_stereo_shaders_start_[1];
  bin_select_ = fh1_stereo_bins_start_[0];
  bin_mask_ = fh1_stereo_bins_start_[1];
  Fh1StereoRegistersReplaced();
  fh1_stereo_state_ = Fh1StereoState::kReplaying;
  Fh1StereoReplayBegin();
  bool complete = !fh1_stereo_log_.empty();
  if (complete) {
    memory::RingBuffer log(reinterpret_cast<uint8_t*>(fh1_stereo_log_.data()),
                           fh1_stereo_log_.size() * sizeof(uint32_t));
    log.set_write_offset(fh1_stereo_log_.size() * sizeof(uint32_t));
    fh1_command_cursor::Scope cursor_scope(&log);
    // The left eye's uploads, each before the packet it was made for.
    static const std::vector<SharedMemory::Fh1Upload> no_uploads;
    const std::vector<SharedMemory::Fh1Upload>& uploads =
        shared_memory ? shared_memory->fh1_uploads() : no_uploads;
    size_t next_upload = 0;
    do {
      const size_t position = log.read_offset() / sizeof(uint32_t);
      while (next_upload < uploads.size() && uploads[next_upload].position <= position) {
        const SharedMemory::Fh1Upload& upload = uploads[next_upload++];
        shared_memory->Fh1UploadData(upload.start, shared_memory->fh1_upload_data() + upload.data_offset,
                                     upload.length);
      }
      if (!ExecutePacket(&log)) {
        REXGPU_WARN("VR stereo: the right eye's pass failed at a packet");
        complete = false;
        break;
      }
    } while (log.read_count());
  }

  std::memcpy(regs.values, fh1_stereo_registers_end_.data(),
              sizeof(uint32_t) * RegisterFile::kRegisterCount);
  active_vertex_shader_ = shaders_end[0];
  active_pixel_shader_ = shaders_end[1];
  bin_select_ = bins_end[0];
  bin_mask_ = bins_end[1];
  Fh1StereoRegistersReplaced();
  fh1_stereo_state_ = Fh1StereoState::kOff;
  if (complete) {
    ++fh1_stereo_replays_;
  } else {
    ++fh1_stereo_dropped_;
  }
  Fh1StereoEnd(complete);
  static uint64_t logged_at = 0;
  if (fh1_stereo_replays_ == 1 || fh1_stereo_replays_ >= logged_at + 3600) {
    logged_at = fh1_stereo_replays_;
    REXGPU_INFO("VR stereo: right eye replayed from {} logged words and {} uploads of {} KB "
                "({} frames, {} dropped)",
                fh1_stereo_log_.size(), shared_memory ? shared_memory->fh1_uploads().size() : 0,
                shared_memory ? shared_memory->fh1_upload_bytes() >> 10 : 0, fh1_stereo_replays_,
                fh1_stereo_dropped_);
  }
  fh1_stereo_log_.clear();
}

void CommandProcessor::Fh1StereoOnSwap() {
  if (SharedMemory* shared_memory = Fh1StereoSharedMemory()) shared_memory->Fh1StopUploadLog();
  if (fh1_stereo_state_ == Fh1StereoState::kRecording) {
    // The frame ended without a HUD draw: only the left eye was drawn.
    ++fh1_stereo_dropped_;
    Fh1StereoEnd(false);
  }
  fh1_stereo_state_ = Fh1StereoState::kOff;
  fh1_stereo_log_.clear();
  fh1_stereo_frame_done_ = false;
}

bool CommandProcessor::ExecutePacketType3_ME_INIT(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  // initialize CP's micro-engine
  me_bin_.clear();
  for (uint32_t i = 0; i < count; i++) {
    me_bin_.push_back(reader->ReadAndSwap<uint32_t>());
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_NOP(memory::RingBuffer* reader, uint32_t packet,
                                              uint32_t count) {
  // skip N 32-bit words to get to the next packet
  // No-op, ignore some data.
  reader->AdvanceRead(count * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INTERRUPT(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // generate interrupt from the command stream
  uint32_t cpu_mask = reader->ReadAndSwap<uint32_t>();
  if (record_split_) {
    // #474: in order with the draws before it, as a fence.
    uint32_t* record = RecordWords(2);
    record[0] = kRecordInterrupt;
    record[1] = cpu_mask;
    PublishRecordBatch();
    return true;
  }
  for (int n = 0; n < 6; n++) {
    if (cpu_mask & (1 << n)) {
      if (graphics_system_) {
        graphics_system_->DispatchInterruptCallback(1, n);
      }
    }
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_XE_SWAP(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  {
    static uint64_t last_frame_tick = 0;
    uint64_t now = rex::chrono::Clock::QueryHostTickCount();
    if (last_frame_tick) {
      uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();
      int64_t dt_us = static_cast<int64_t>((now - last_frame_tick) * 1000000 / freq);
      PROFILE_FRAME_TIME_US(dt_us);
      PROFILE_FPS(freq / (now - last_frame_tick));
    }
    last_frame_tick = now;
  }
#endif
  rex::perf::Profiler::Flip();

  // Xenia-specific VdSwap hook.
  // VdSwap will post this to tell us we need to swap the screen/fire an
  // interrupt.
  // 63 words here, but only the first has any data.
  uint32_t magic = reader->ReadAndSwap<memory::fourcc_t>();
  assert_true(magic == kSwapSignature);

  // TODO(benvanik): only swap frontbuffer ptr.
  uint32_t frontbuffer_ptr = reader->ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_width = reader->ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_height = reader->ReadAndSwap<uint32_t>();
  // Patch 0391: a client may lengthen the packet by one dword, a tag for the frame (the game's
  // OpenXR path pairs each guest output with the head pose it was drawn with).
  uint32_t frame_tag = 0;
  if (count >= 5) {
    frame_tag = reader->ReadAndSwap<uint32_t>();
    reader->AdvanceRead((count - 5) * sizeof(uint32_t));
  } else {
    reader->AdvanceRead((count - 4) * sizeof(uint32_t));
  }

  if (record_split_) {
    // #474: the swap on the recorder, after the frame's draws, with its tag; the time the
    // decoder waited for the game in this frame goes into the frame's cost there. The frame
    // trace and the stereo replay are off while split.
    RecordCall([this, frontbuffer_ptr, frontbuffer_width, frontbuffer_height, frame_tag]() {
      fh1_frame_cost::Current().guest_wait_ms +=
          double(record_guest_wait_ns_.exchange(0, std::memory_order_relaxed)) / 1e6;
      rexui_SetNextGuestOutputTag(frame_tag);
      IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);
      rexui_SetNextGuestOutputTag(0);
    });
    PublishRecordBatch();
    LogRecordSplit();
    ++counter_;
    return true;
  }
  rexui_SetNextGuestOutputTag(frame_tag);
  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);
  rexui_SetNextGuestOutputTag(0);
  fh1_frame_trace::OnSwap();  // patch 0396
  Fh1StereoOnSwap();

  ++counter_;
  return true;
}

bool CommandProcessor::ExecutePacketType3_INDIRECT_BUFFER(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // indirect buffer dispatch
  uint32_t list_ptr = CpuToGpu(reader->ReadAndSwap<uint32_t>());
  uint32_t list_length = reader->ReadAndSwap<uint32_t>();
  assert_zero(list_length & ~0xFFFFF);
  list_length &= 0xFFFFF;
  ExecuteIndirectBuffer(GpuToCpu(list_ptr), list_length);
  return true;
}

bool CommandProcessor::ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer* reader, uint32_t packet,
                                                       uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // wait until a register or memory location is a specific value

  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t wait = reader->ReadAndSwap<uint32_t>();

  bool is_memory = (wait_info & 0x10) != 0;
  // #474: split, the coherency wait (Direct3D's after each resource it changes, about a hundred
  // a frame) is answered on the shadow, as Pinyon Shift does: MakeCoherent only clears the
  // status, and nothing the recorder does sets it. A wait for another register reads the
  // register file, which the CPU may write, once the recorder has run everything before (it
  // then waits); a wait for memory lets the recorder go on with what was decoded while this
  // waits.
  const bool shadow_coherency =
      record_split_ && !is_memory && poll_reg_addr == XE_GPU_REG_COHER_STATUS_HOST;
  if (record_split_ && !is_memory && !shadow_coherency) {
    record_sync_register_ = poll_reg_addr;
    RecordSync(kSyncWaitRegister);
  }

  bool matched = false;
  // FH1 (#45): the time spent waiting for the game (fh1_frame_cost.h), from the first miss.
  std::chrono::steady_clock::time_point wait_start{};
  do {
    uint32_t value = 0;
    if (is_memory) {
      value =
          *reinterpret_cast<uint32_t*>(memory_->TranslatePhysical(poll_reg_addr & ~uint32_t(0x3)));
      value = xenos::GpuSwap(value, static_cast<xenos::Endian>(poll_reg_addr & 0x3));
    } else if (shadow_coherency) {
      uint32_t& status = decode_register_file_->values[XE_GPU_REG_COHER_STATUS_HOST];
      if (rex::memory::Reinterpret<reg::COHER_STATUS_HOST>(status).status) {
        status = 0;
        // The register file's copy too, in order (nothing reads it there but the CPU).
        RecordCall([this]() {
          const_cast<volatile uint32_t&>(register_file_->values[XE_GPU_REG_COHER_STATUS_HOST]) = 0;
        });
      }
      value = status;
    } else {
      value = ReadRegisterValue(poll_reg_addr);
      if (poll_reg_addr == XE_GPU_REG_COHER_STATUS_HOST) {
        MakeCoherent();
        value = ReadRegisterValue(poll_reg_addr);
      }
    }
    switch (wait_info & 0x7) {
      case 0x0:  // Never.
        matched = false;
        break;
      case 0x1:  // Less than reference.
        matched = (value & mask) < ref;
        break;
      case 0x2:  // Less than or equal to reference.
        matched = (value & mask) <= ref;
        break;
      case 0x3:  // Equal to reference.
        matched = (value & mask) == ref;
        break;
      case 0x4:  // Not equal to reference.
        matched = (value & mask) != ref;
        break;
      case 0x5:  // Greater than or equal to reference.
        matched = (value & mask) >= ref;
        break;
      case 0x6:  // Greater than reference.
        matched = (value & mask) > ref;
        break;
      case 0x7:  // Always
        matched = true;
        break;
    }
    if (!matched) {
      wait_progress_first_ = wait_start == std::chrono::steady_clock::time_point{};
      if (wait_start == std::chrono::steady_clock::time_point{}) {
        wait_start = std::chrono::steady_clock::now();
      }
      if (record_split_ && is_memory &&
          (!record_batch_->words.empty() ||
           record_completed_.load(std::memory_order_acquire) < record_published_)) {
        // #474: the word may be one the recorder is still to write (a fence, the scratch
        // write-back), and the sleep below lasts until a vblank: while the recorder has work,
        // poll again each time it finishes a batch (or after 100 us, the game may write it).
        RecordWaitProgress();
        continue;
      }
      // Wait.
      if (wait >= 0x100) {
        PrepareForWait();
        if (!REXCVAR_GET(vsync)) {
          // User wants it fast and dangerous.
          rex::thread::MaybeYield();
        } else {
          // Sleep, but wake up after a guest vblank interrupt: games wait here
          // for values their interrupt handler changes (the frame flip), and a
          // full sleep added up to a millisecond to every such wait.
          rex::thread::Wait(vblank_event_.get(), false, std::chrono::milliseconds(wait / 0x100));
        }
        rex::thread::SyncMemory();
        ReturnFromWait();

        if (!worker_running_) {
          // Short-circuited exit.
          return false;
        }
      } else {
        rex::thread::MaybeYield();
      }
    }
  } while (!matched);
  if (record_split_ && !is_memory && !shadow_coherency) {
    // What MakeCoherent wrote, and a register the CPU writes.
    decode_register_file_->values[XE_GPU_REG_COHER_STATUS_HOST] =
        register_file_->values[XE_GPU_REG_COHER_STATUS_HOST];
    if (poll_reg_addr < RegisterFile::kRegisterCount) {
      decode_register_file_->values[poll_reg_addr] = register_file_->values[poll_reg_addr];
    }
  }
  if (wait_start != std::chrono::steady_clock::time_point{}) {
    if (record_split_) {
      record_guest_wait_ns_.fetch_add(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                               wait_start)
              .count(),
          std::memory_order_relaxed);
    } else {
      fh1_frame_cost::Current().guest_wait_ms +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start)
              .count();
    }
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_RMW(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  // register read/modify/write
  // ? (used during shader upload and edram setup)
  uint32_t rmw_info = reader->ReadAndSwap<uint32_t>();
  uint32_t and_mask = reader->ReadAndSwap<uint32_t>();
  uint32_t or_mask = reader->ReadAndSwap<uint32_t>();
  // #474: DecoderRegisterValue (the shadow while split).
  uint32_t value = DecoderRegisterValue(rmw_info & 0x1FFF);
  if ((rmw_info >> 31) & 0x1) {
    // & reg
    value &= DecoderRegisterValue(and_mask & 0x1FFF);
  } else {
    // & imm
    value &= and_mask;
  }
  if ((rmw_info >> 30) & 0x1) {
    // | reg
    value |= DecoderRegisterValue(or_mask & 0x1FFF);
  } else {
    // | imm
    value |= or_mask;
  }
  PacketWriteRegister(rmw_info & 0x1FFF, value);
  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_TO_MEM(memory::RingBuffer* reader, uint32_t packet,
                                                     uint32_t count) {
  // Copy Register to Memory (?)
  // Count is 2, assuming a Register Addr and a Memory Addr.

  uint32_t reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t mem_addr = reader->ReadAndSwap<uint32_t>();

  // #474: the CPU reads the result; the register file once the recorder has caught up.
  RecordSync(kSyncRegToMem);
  uint32_t reg_val = ReadRegisterValue(reg_addr);

  auto endianness = static_cast<xenos::Endian>(mem_addr & 0x3);
  mem_addr &= ~0x3;
  reg_val = GpuSwap(reg_val, endianness);
  auto* host = memory_->TranslatePhysical(mem_addr);
  memory::store(host, reg_val);
  WakeWordWaiters(host);

  return true;
}

bool CommandProcessor::ExecutePacketType3_MEM_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  uint32_t write_addr = reader->ReadAndSwap<uint32_t>();
  // #474: the CPU reads these words: written once the recorder has run what came before.
  RecordSync(kSyncMemWrite);
  for (uint32_t i = 0; i < count - 1; i++) {
    uint32_t write_data = reader->ReadAndSwap<uint32_t>();

    auto endianness = static_cast<xenos::Endian>(write_addr & 0x3);
    auto addr = write_addr & ~0x3;
    write_data = GpuSwap(write_data, endianness);
    auto* host = memory_->TranslatePhysical(addr);
    memory::store(host, write_data);
    WakeWordWaiters(host);
    write_addr += 4;
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_COND_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                     uint32_t count) {
  // conditional write to memory or register
  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t write_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t write_data = reader->ReadAndSwap<uint32_t>();
  // #474: reads the register file or memory and may write memory the CPU reads: once the
  // recorder has caught up (it then waits, so the register file may be used here).
  RecordSync(kSyncCondWrite);
  uint32_t value;
  if (wait_info & 0x10) {
    // Memory.
    auto endianness = static_cast<xenos::Endian>(poll_reg_addr & 0x3);
    poll_reg_addr &= ~0x3;
    value = memory::load<uint32_t>(memory_->TranslatePhysical(poll_reg_addr));
    value = GpuSwap(value, endianness);
  } else {
    // Register.
    value = ReadRegisterValue(poll_reg_addr);
  }
  bool matched = false;
  switch (wait_info & 0x7) {
    case 0x0:  // Never.
      matched = false;
      break;
    case 0x1:  // Less than reference.
      matched = (value & mask) < ref;
      break;
    case 0x2:  // Less than or equal to reference.
      matched = (value & mask) <= ref;
      break;
    case 0x3:  // Equal to reference.
      matched = (value & mask) == ref;
      break;
    case 0x4:  // Not equal to reference.
      matched = (value & mask) != ref;
      break;
    case 0x5:  // Greater than or equal to reference.
      matched = (value & mask) >= ref;
      break;
    case 0x6:  // Greater than reference.
      matched = (value & mask) > ref;
      break;
    case 0x7:  // Always
      matched = true;
      break;
  }
  if (matched) {
    // Write.
    if (wait_info & 0x100) {
      // Memory.
      auto endianness = static_cast<xenos::Endian>(write_reg_addr & 0x3);
      write_reg_addr &= ~0x3;
      write_data = GpuSwap(write_data, endianness);
      auto* host = memory_->TranslatePhysical(write_reg_addr);
      memory::store(host, write_data);
      WakeWordWaiters(host);
    } else {
      // Register.
      WriteRegister(write_reg_addr, write_data);
      if (record_split_ && write_reg_addr < RegisterFile::kRegisterCount) {
        decode_register_file_->values[write_reg_addr] = register_file_->values[write_reg_addr];
      }
    }
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                      uint32_t count) {
  // generate an event that creates a write to memory when completed
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  PacketWriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  if (count == 1) {
    // Just an event flag? Where does this write?
  } else {
    // Write to an address.
    assert_always();
    reader->AdvanceRead((count - 1) * sizeof(uint32_t));
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_SHD(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // generate a VS|PS_done event
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  uint32_t value = reader->ReadAndSwap<uint32_t>();

  // Writeback initiator.
  PacketWriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  uint32_t data_value;
  if ((initiator >> 31) & 0x1) {
    // Write counter (GPU vblank counter?).
    data_value = counter_;
  } else {
    // Write value.
    data_value = value;
  }
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;
  data_value = GpuSwap(data_value, endianness);
  if (record_split_) {
    // #474: the fence is recorded, not waited for: the game sees it once the recorder has run
    // the draws before it, whose memory it may reuse then.
    RecordStore(address, data_value);
    PublishRecordBatch();
    return true;
  }
  auto* host = memory_->TranslatePhysical(address);
  memory::store(host, data_value);
  WakeWordWaiters(host);
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_EXT(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // generate a screen extent event
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  PacketWriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;

  // Let us hope we can fake this.
  // This callback tells the driver the xy coordinates affected by a previous
  // drawcall.
  // https://www.google.com/patents/US20060055701
  uint16_t extents[] = {
      0 >> 3,                                    // min x
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max x
      0 >> 3,                                    // min y
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max y
      0,                                         // min z
      1,                                         // max z
  };
  assert_true(endianness == xenos::Endian::k8in16);
  // #474: in order with the draws while split.
  RecordCall([this, address, extents]() {
    memory::copy_and_swap_16_unaligned(memory_->TranslatePhysical(address), extents,
                                       rex::countof(extents));
  });
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // Set by D3D as BE but struct ABI is LE
  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  // Occlusion queries:
  // This command is send on query begin and end.
  // As a workaround report some fixed amount of passed samples.
  auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
  if (fake_sample_count >= 0) {
    auto* pSampleCounts = memory_->TranslatePhysical<xe_gpu_depth_sample_counts*>(
        register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR]);
    if (!pSampleCounts) {
      return true;
    }
    // 0xFFFFFEED is written to this two locations by D3D only on D3DISSUE_END
    // and used to detect a finished query.
    bool is_end_via_z_pass =
        pSampleCounts->ZPass_A == kQueryFinished || pSampleCounts->ZPass_B == kQueryFinished;
    // Older versions of D3D also checks for ZFail (4D5307D5).
    bool is_end_via_z_fail =
        pSampleCounts->ZFail_A == kQueryFinished || pSampleCounts->ZFail_B == kQueryFinished;
    std::memset(pSampleCounts, 0, sizeof(xe_gpu_depth_sample_counts));
    if (is_end_via_z_pass || is_end_via_z_fail) {
      pSampleCounts->ZPass_A = fake_sample_count;
      pSampleCounts->Total_A = fake_sample_count;
    }
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3Draw(memory::RingBuffer* reader, uint32_t packet,
                                              const char* opcode_name, uint32_t viz_query_condition,
                                              uint32_t count_remaining) {
  // if viz_query_condition != 0, this is a conditional draw based on viz query.
  // This ID matches the one issued in PM4_VIZ_QUERY
  // uint32_t viz_id = viz_query_condition & 0x3F;
  // when true, render conditionally based on query result
  // uint32_t viz_use = viz_query_condition & 0x100;

  assert_not_zero(count_remaining);
  if (!count_remaining) {
    REXGPU_ERROR("{}: Packet too small, can't read VGT_DRAW_INITIATOR", opcode_name);
    return false;
  }
  reg::VGT_DRAW_INITIATOR vgt_draw_initiator;
  vgt_draw_initiator.value = reader->ReadAndSwap<uint32_t>();
  --count_remaining;
  PacketWriteRegister(XE_GPU_REG_VGT_DRAW_INITIATOR, vgt_draw_initiator.value);

  bool draw_succeeded = true;
  // TODO(Triang3l): Remove IndexBufferInfo and replace handling of all this
  // with PrimitiveProcessor when the old Vulkan renderer is removed.
  bool is_indexed = false;
  IndexBufferInfo index_buffer_info;
  switch (vgt_draw_initiator.source_select) {
    case xenos::SourceSelect::kDMA: {
      // Indexed draw.
      is_indexed = true;

      // Two separate bounds checks so if there's only one missing register
      // value out of two, one uint32_t will be skipped in the command buffer,
      // not two.
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_BASE", opcode_name);
        return false;
      }
      uint32_t vgt_dma_base = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      PacketWriteRegister(XE_GPU_REG_VGT_DMA_BASE, vgt_dma_base);
      reg::VGT_DMA_SIZE vgt_dma_size;
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_SIZE", opcode_name);
        return false;
      }
      vgt_dma_size.value = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      PacketWriteRegister(XE_GPU_REG_VGT_DMA_SIZE, vgt_dma_size.value);

      uint32_t index_size_bytes = vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16
                                      ? sizeof(uint16_t)
                                      : sizeof(uint32_t);
      // The base address must already be word-aligned according to the R6xx
      // documentation, but for safety.
      index_buffer_info.guest_base = vgt_dma_base & ~(index_size_bytes - 1);
      index_buffer_info.endianness = vgt_dma_size.swap_mode;
      index_buffer_info.format = vgt_draw_initiator.index_size;
      index_buffer_info.length = vgt_dma_size.num_words * index_size_bytes;
      index_buffer_info.count = vgt_draw_initiator.num_indices;
    } break;
    case xenos::SourceSelect::kImmediate: {
      // TODO(Triang3l): VGT_IMMED_DATA.
      REXGPU_ERROR(
          "{}: Using immediate vertex indices, which are not supported yet. "
          "Report the game to Xenia developers!",
          opcode_name, uint32_t(vgt_draw_initiator.source_select));
      draw_succeeded = false;
      assert_always();
    } break;
    case xenos::SourceSelect::kAutoIndex: {
      // Auto draw.
      index_buffer_info.guest_base = 0;
      index_buffer_info.length = 0;
    } break;
    default: {
      // Invalid source selection.
      draw_succeeded = false;
      assert_unhandled_case(vgt_draw_initiator.source_select);
    } break;
  }

  // Skip to the next command, for example, if there are immediate indexes that
  // we don't support yet.
  reader->AdvanceRead(count_remaining * sizeof(uint32_t));

  if (draw_succeeded) {
    DrawRecord draw;
    draw.initiator = vgt_draw_initiator.value;
    draw.flags = (is_indexed ? kDrawIndexed : 0) |
                 (std::strcmp(opcode_name, "PM4_DRAW_INDX_2") == 0 ? kDrawIndx2 : 0);
    draw.index_format = uint32_t(index_buffer_info.format);
    draw.index_endianness = uint32_t(index_buffer_info.endianness);
    draw.index_count = index_buffer_info.count;
    draw.index_guest_base = index_buffer_info.guest_base;
    draw.index_length = uint32_t(index_buffer_info.length);
    if (record_split_) {
      // #474: on the recorder, with the register file as the packets before it left it.
      uint32_t* record = RecordWords(1 + sizeof(DrawRecord) / sizeof(uint32_t));
      record[0] = kRecordDraw;
      std::memcpy(record + 1, &draw, sizeof(draw));
      RecordAction();
      return true;
    }
    // Patch 0396: the stereo segment's start and end (may replay the segment for the right eye
    // before this draw).
    if (fh1_stereo_state_ == Fh1StereoState::kSpeculative ||
        fh1_stereo_state_ == Fh1StereoState::kRecording ||
        (fh1_stereo_state_ == Fh1StereoState::kOff && !fh1_stereo_frame_done_ &&
         Fh1StereoWanted())) [[unlikely]] {
      Fh1StereoOnDraw();
    }
    ExecuteDraw(draw);
  }

  // If read the packed correctly, but merely couldn't execute it (because of,
  // for instance, features not supported by the host), don't terminate command
  // buffer processing as that would leave rendering in a way more inconsistent
  // state than just a single dropped draw command.
  return true;
}

bool CommandProcessor::ExecuteDraw(const DrawRecord& draw) {
  reg::VGT_DRAW_INITIATOR vgt_draw_initiator;
  vgt_draw_initiator.value = draw.initiator;
  const char* opcode_name = (draw.flags & kDrawIndx2) ? "PM4_DRAW_INDX_2" : "PM4_DRAW_INDX";
  const bool is_indexed = (draw.flags & kDrawIndexed) != 0;
  IndexBufferInfo index_buffer_info;
  index_buffer_info.format = xenos::IndexFormat(draw.index_format);
  index_buffer_info.endianness = xenos::Endian(draw.index_endianness);
  index_buffer_info.count = draw.index_count;
  index_buffer_info.guest_base = draw.index_guest_base;
  index_buffer_info.length = draw.index_length;
  bool draw_succeeded = true;
  auto viz_query = register_file_->Get<reg::PA_SC_VIZ_QUERY>();
  if (!(viz_query.viz_query_ena && viz_query.kill_pix_post_hi_z)) {
    // TODO(Triang3l): Don't drop the draw call completely if the vertex
    // shader has memexport.
    // TODO(Triang3l || JoelLinn): Handle this properly in the render
    // backends.

    if (fh1_frame_trace::file && !record_split_) {
      const RegisterFile& r = *register_file_;
      auto f = [&](uint32_t reg) { return r.Get<float>(reg); };
      std::fprintf(
          fh1_frame_trace::file,
          "D %u prim=%u n=%u src=%u vs=%016llX ps=%016llX surf=%08X mode=%08X c0=%08X c1=%08X "
          "c2=%08X c3=%08X mask=%08X depth=%08X dctl=%08X vte=%08X vp=%g,%g,%g,%g,%g,%g "
          "woff=%08X wsc=%08X,%08X ssc=%08X,%08X sc=%08X copy=%08X dest=%08X dpitch=%08X "
          "dinfo=%08X binsel=%llX binmask=%llX tf0=%08X,%08X,%08X dma=%08X\n",
          fh1_frame_trace::draws++, uint32_t(vgt_draw_initiator.prim_type),
          uint32_t(vgt_draw_initiator.num_indices), uint32_t(vgt_draw_initiator.source_select),
          (unsigned long long)(active_vertex_shader_ ? active_vertex_shader_->ucode_data_hash() : 0),
          (unsigned long long)(active_pixel_shader_ ? active_pixel_shader_->ucode_data_hash() : 0),
          r[XE_GPU_REG_RB_SURFACE_INFO], r[XE_GPU_REG_RB_MODECONTROL],
          r[XE_GPU_REG_RB_COLOR_INFO], r[XE_GPU_REG_RB_COLOR1_INFO], r[XE_GPU_REG_RB_COLOR2_INFO],
          r[XE_GPU_REG_RB_COLOR3_INFO], r[XE_GPU_REG_RB_COLOR_MASK], r[XE_GPU_REG_RB_DEPTH_INFO],
          r[XE_GPU_REG_RB_DEPTHCONTROL], r[XE_GPU_REG_PA_CL_VTE_CNTL],
          f(XE_GPU_REG_PA_CL_VPORT_XSCALE), f(XE_GPU_REG_PA_CL_VPORT_XOFFSET),
          f(XE_GPU_REG_PA_CL_VPORT_YSCALE), f(XE_GPU_REG_PA_CL_VPORT_YOFFSET),
          f(XE_GPU_REG_PA_CL_VPORT_ZSCALE), f(XE_GPU_REG_PA_CL_VPORT_ZOFFSET),
          r[XE_GPU_REG_PA_SC_WINDOW_OFFSET], r[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
          r[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR], r[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL],
          r[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR], r[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
          r[XE_GPU_REG_RB_COPY_CONTROL], r[XE_GPU_REG_RB_COPY_DEST_BASE],
          r[XE_GPU_REG_RB_COPY_DEST_PITCH], r[XE_GPU_REG_RB_COPY_DEST_INFO],
          (unsigned long long)bin_select_, (unsigned long long)bin_mask_,
          r[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], r[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 1],
          r[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2],
          is_indexed ? index_buffer_info.guest_base : 0u);
    }
    bool major_mode_explicit =
        xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type);
    draw_succeeded = IssueDraw(vgt_draw_initiator.prim_type, vgt_draw_initiator.num_indices,
                               is_indexed ? &index_buffer_info : nullptr, major_mode_explicit);
    if (!draw_succeeded) {
      auto vgt_output_path_cntl = register_file_->Get<reg::VGT_OUTPUT_PATH_CNTL>();
      auto vgt_hos_cntl = register_file_->Get<reg::VGT_HOS_CNTL>();
      auto rb_modecontrol = register_file_->Get<reg::RB_MODECONTROL>();
      REXGPU_ERROR(
          "{}({}, {}, {}): Failed in backend "
          "(major_mode={}, explicit_major={}, path_select={}, tess_mode={}, edram_mode={})",
          opcode_name, static_cast<uint32_t>(vgt_draw_initiator.num_indices),
          uint32_t(vgt_draw_initiator.prim_type), uint32_t(vgt_draw_initiator.source_select),
          uint32_t(vgt_draw_initiator.major_mode), uint32_t(major_mode_explicit),
          uint32_t(vgt_output_path_cntl.path_select), uint32_t(vgt_hos_cntl.tess_mode),
          uint32_t(rb_modecontrol.edram_mode));
    }
  }
  return draw_succeeded;
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  // "initiate fetch of index buffer and draw"
  // Generally used by Xbox 360 Direct3D 9 for kDMA and kAutoIndex sources.
  // With a viz query token as the first one.
  uint32_t count_remaining = count;
  assert_not_zero(count_remaining);
  if (!count_remaining) {
    REXGPU_ERROR("PM4_DRAW_INDX: Packet too small, can't read the viz query token");
    return false;
  }
  uint32_t viz_query_condition = reader->ReadAndSwap<uint32_t>();
  --count_remaining;
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX", viz_query_condition,
                                count_remaining);
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX_2(memory::RingBuffer* reader, uint32_t packet,
                                                      uint32_t count) {
  // "draw using supplied indices in packet"
  // Generally used by Xbox 360 Direct3D 9 for kAutoIndex source.
  // No viz query token.
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX_2", 0, count);
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT(memory::RingBuffer* reader, uint32_t packet,
                                                       uint32_t count) {
  // load constant into chip and to memory
  // PM4_REG(reg) ((0x4 << 16) | (GSL_HAL_SUBBLOCK_OFFSET(reg)))
  //                                     reg - 0x2000
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t count_registers = count - 1;
  switch (type) {
    case 0:  // ALU
      WriteALURangeFromRing(reader, index, count_registers);
      break;
    case 1:  // FETCH
      WriteFetchRangeFromRing(reader, index, count_registers);
      break;
    case 2:  // BOOL
      WriteBoolRangeFromRing(reader, index, count_registers);
      break;
    case 3:  // LOOP
      WriteLoopRangeFromRing(reader, index, count_registers);
      break;
    case 4:  // REGISTERS
      // FH1 (#45, patch 0360): as in ExecutePacketType0.
      if (fh1_plain_registers_ &&
          Fh1WritePlainRangeFromRing(reader, index + 0x2000, count_registers)) {
        break;
      }
      WriteREGISTERSRangeFromRing(reader, index, count_registers);
      break;
    default:
      assert_always();
      reader->AdvanceRead((count - 1) * sizeof(uint32_t));
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT2(memory::RingBuffer* reader, uint32_t packet,
                                                        uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  // FH1 (#45, patch 0360): as in ExecutePacketType0.
  if (fh1_plain_registers_ && Fh1WritePlainRangeFromRing(reader, index, count - 1)) {
    return true;
  }
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_LOAD_ALU_CONSTANT(memory::RingBuffer* reader,
                                                            uint32_t packet, uint32_t count) {
  // load constants from memory
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  address &= 0x3FFFFFFF;
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t size_dwords = reader->ReadAndSwap<uint32_t>();
  size_dwords &= 0xFFF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t* xlat_address = memory_->TranslatePhysical<uint32_t*>(address);
  switch (type) {
    case 0:  // ALU
      WriteALURangeFromMem(index, xlat_address, size_dwords);
      break;
    case 1:  // FETCH
      WriteFetchRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 2:  // BOOL
      WriteBoolRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 3:  // LOOP
      WriteLoopRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 4:  // REGISTERS
      WriteREGISTERSRangeFromMem(index, xlat_address, size_dwords);
      break;
    default:
      assert_always();
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_SHADER_CONSTANTS(memory::RingBuffer* reader,
                                                               uint32_t packet, uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (pointer-based)
  uint32_t addr_type = reader->ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(addr_type & 0x3);
  uint32_t addr = addr_type & ~0x3;
  uint32_t start_size = reader->ReadAndSwap<uint32_t>();
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);

  if (record_split_) {
    // #474: the shader is looked up (memo 0140) and translated on the recorder.
    if (shader_type != xenos::ShaderType::kVertex && shader_type != xenos::ShaderType::kPixel) {
      assert_unhandled_case(shader_type);
      return false;
    }
    uint32_t* record = RecordWords(4);
    record[0] = kRecordShader;
    record[1] = uint32_t(shader_type);
    record[2] = addr;
    record[3] = size_dwords;
    return true;
  }
  auto shader =
      LoadShader(shader_type, addr, memory_->TranslatePhysical<uint32_t*>(addr), size_dwords);
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      assert_unhandled_case(shader_type);
      return false;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD_IMMEDIATE(memory::RingBuffer* reader,
                                                            uint32_t packet, uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (code embedded in packet)
  uint32_t dword0 = reader->ReadAndSwap<uint32_t>();
  uint32_t dword1 = reader->ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(dword0);
  uint32_t start_size = dword1;
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);
  assert_true(reader->read_count() >= size_dwords * 4);
  assert_true(count - 2 >= size_dwords);
  if (record_split_) {
    // #474: the microcode is in the ring, which the game reuses: the recorder gets a copy.
    if (shader_type != xenos::ShaderType::kVertex && shader_type != xenos::ShaderType::kPixel) {
      assert_unhandled_case(shader_type);
      return false;
    }
    uint32_t* record = RecordWords(3 + size_dwords);
    record[0] = kRecordShaderImmediate;
    record[1] = uint32_t(shader_type);
    record[2] = size_dwords;
    std::memcpy(record + 3, reinterpret_cast<const void*>(reader->read_ptr()),
                size_dwords * sizeof(uint32_t));
    reader->AdvanceRead(size_dwords * sizeof(uint32_t));
    return true;
  }
  auto shader = LoadShader(shader_type, uint32_t(reader->read_ptr()),
                           reinterpret_cast<uint32_t*>(reader->read_ptr()), size_dwords);
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      assert_unhandled_case(shader_type);
      return false;
  }
  reader->AdvanceRead(size_dwords * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INVALIDATE_STATE(memory::RingBuffer* reader,
                                                           uint32_t packet, uint32_t count) {
  // selective invalidation of state pointers
  /*uint32_t mask =*/reader->ReadAndSwap<uint32_t>();
  // driver_->InvalidateState(mask);
  return true;
}

bool CommandProcessor::ExecutePacketType3_VIZ_QUERY(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  // begin/end initiator for viz query extent processing
  // https://www.google.com/patents/US20050195186
  assert_true(count == 1);

  uint32_t dword0 = reader->ReadAndSwap<uint32_t>();

  uint32_t id = dword0 & 0x3F;
  uint32_t end = dword0 & 0x100;
  if (!end) {
    // begin a new viz query @ id
    // On hardware this clears the internal state of the scan converter (which
    // is different to the register)
    PacketWriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, VIZQUERY_START);
    REXGPU_DEBUG("Begin viz query ID {:02X}", id);
  } else {
    // end the viz query
    PacketWriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, VIZQUERY_END);
    REXGPU_DEBUG("End viz query ID {:02X}", id);
    // The scan converter writes the internal result back to
    // PA_SC_VIZ_QUERY_STATUS here. Leave it untouched as Xenia does rather than
    // faking every query as visible (the bits were never cleared again).
  }

  return true;
}

}  // namespace rex::graphics
