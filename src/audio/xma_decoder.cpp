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

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/decoder.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/math.h>
#include <rex/memory/ring_buffer.h>
#include <rex/string/buffer.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>

extern "C" {
#include "libavutil/log.h"
}  // extern "C"

REXCVAR_DEFINE_BOOL(ffmpeg_verbose, false, "Audio", "Verbose FFmpeg output (debug and above)");
REXCVAR_DEFINE_BOOL(xma_worker_decode, false, "Audio",
                    "Decode a kicked XMA context on the XMA decoder thread instead of the thread "
                    "that kicked it; the game's next kick, lock, clear or release of the context "
                    "waits for that decode");

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal XmaDecoder handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc
//
// XMA details:
// https://devel.nuclex.org/external/svn/directx/trunk/include/xma2defs.h
// https://github.com/gdawg/fsbext/blob/master/src/xma_header.h
//
// XAudio2 uses XMA under the covers, and seems to map with the same
// restrictions of frame/subframe/etc:
// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.xaudio2.xaudio2_buffer(v=vs.85).aspx
//
// XMA contexts are 64b in size and tight bitfields. They are in physical
// memory not usually available to games. Games will use MmMapIoSpace to get
// the 64b pointer in user memory so they can party on it. If the game doesn't
// do this, it's likely they are either passing the context to XAudio or
// using the XMA* functions.

namespace rex::audio {

XmaDecoder::XmaDecoder(runtime::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->memory()), function_dispatcher_(function_dispatcher) {}

XmaDecoder::~XmaDecoder() = default;

void av_log_callback(void* avcl, int level, const char* fmt, va_list va) {
  if (!REXCVAR_GET(ffmpeg_verbose) && level > AV_LOG_WARNING) {
    return;
  }

  string::StringBuffer buff;
  buff.AppendVarargs(fmt, va);
  auto msg = buff.to_string_view();

  switch (level) {
    case AV_LOG_ERROR:
      REXAPU_ERROR("ffmpeg: {}", msg);
      break;
    case AV_LOG_WARNING:
      REXAPU_WARN("ffmpeg: {}", msg);
      break;
    case AV_LOG_INFO:
      REXAPU_INFO("ffmpeg: {}", msg);
      break;
    case AV_LOG_VERBOSE:
    case AV_LOG_DEBUG:
    default:
      REXAPU_DEBUG("ffmpeg: {}", msg);
      break;
  }
}

X_STATUS XmaDecoder::Setup(system::KernelState* kernel_state) {
  // Setup ffmpeg logging callback
  av_log_set_callback(av_log_callback);

  // Register APU/XMA MMIO handlers
  // XMA registers are at 0x7FEA0000-0x7FEAFFFF
  memory()->AddVirtualMappedRange(
      0x7FEA0000,  // base address
      0xFFFF0000,  // mask
      0x0000FFFF,  // size (64KB)
      this,        // context (XmaDecoder*)
      reinterpret_cast<runtime::MMIOReadCallback>(MMIOReadRegisterThunk),
      reinterpret_cast<runtime::MMIOWriteCallback>(MMIOWriteRegisterThunk));
  REXAPU_DEBUG("XMA: Registered MMIO handlers at 0x7FEA0000-0x7FEAFFFF");

  // Setup XMA context data.
  // The Xbox 360 kernel allocates the contexts with X_PAGE_NOCACHE |
  // X_PAGE_READWRITE and writes MmGetPhysicalAddress for the address to the
  // register.
  context_data_first_ptr_ = memory()->SystemHeapAlloc(sizeof(XMA_CONTEXT_DATA) * kContextCount, 256,
                                                      memory::kSystemHeapPhysical);
  context_data_last_ptr_ = context_data_first_ptr_ + (sizeof(XMA_CONTEXT_DATA) * kContextCount - 1);
  register_file_[XmaRegister::ContextArrayAddress] =
      memory()->GetPhysicalAddress(context_data_first_ptr_);

  // Setup XMA contexts.
  for (size_t i = 0; i < kContextCount; ++i) {
    uint32_t guest_ptr = context_data_first_ptr_ + i * sizeof(XMA_CONTEXT_DATA);
    XmaContext& context = contexts_[i];
    if (context.Setup(i, memory(), guest_ptr)) {
      assert_always();
    }
  }
  register_file_[XmaRegister::NextContextIndex] = 1;
  context_bitmap_.Resize(kContextCount);

  worker_decode_ = REXCVAR_GET(xma_worker_decode);
  for (auto& state : job_state_) {
    state.store(JobState::kIdle, std::memory_order_relaxed);
  }
  job_next_.fill(-1);
  job_prev_.fill(-1);
  if (worker_decode_) {
    REXAPU_INFO("XMA: kicked contexts are decoded on the decoder thread (xma_worker_decode)");
  }

  worker_running_ = true;
  work_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(work_event_);
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));
  worker_thread_->set_name("XMA Decoder");

  worker_thread_->Create();

  return X_STATUS_SUCCESS;
}

void XmaDecoder::WorkerThreadMain() {
  while (worker_running_) {
    bool did_work = false;
    if (worker_decode_) {
      // The kicks' jobs, in kick order. Every enabled context has one (only a kick enables a
      // context), so the sweep below would find nothing more.
      did_work = RunQueuedJobs();
      if (did_work && !first_stats_logged_) {
        const auto now = std::chrono::steady_clock::now();
        if (first_job_time_ == std::chrono::steady_clock::time_point{}) {
          first_job_time_ = now;
        } else if (now - first_job_time_ >= std::chrono::seconds(60)) {
          first_stats_logged_ = true;
          LogJobStats();
        }
      }
    } else {
      // Okay, let's loop through XMA contexts to find ones we need to decode!
      for (uint32_t n = 0; n < kContextCount && worker_running_; n++) {
        XmaContext& context = contexts_[n];
        bool worked = context.Work();
        if (worked) {
          context.SignalWorkDone();
          PROFILE_XMA_FRAME_DECODED();
        }
        did_work = did_work || worked;
      }
    }

    if (paused_) {
      pause_fence_.Signal();
      resume_fence_.Wait();
    }

    if (did_work) {
      continue;
    }
    // No work done this iteration, block until signaled.
    rex::thread::Wait(work_event_.get(), false);
  }
}

void XmaDecoder::Shutdown() {
  if (!worker_thread_) {
    return;
  }

  worker_running_ = false;

  if (work_event_) {
    work_event_->Set();
  }

  if (paused_) {
    Resume();
  }

  // Wait up to 2 seconds for worker thread to exit gracefully.
  auto result = rex::thread::Wait(worker_thread_->thread(), false, std::chrono::milliseconds(2000));
  if (result == rex::thread::WaitResult::kTimeout) {
    REXAPU_WARN("XMA: Worker thread did not exit within 2s, abandoning");
  }
  worker_thread_.reset();
  if (worker_decode_) {
    LogJobStats();
  }

  if (context_data_first_ptr_) {
    memory()->SystemHeapFree(context_data_first_ptr_);
  }

  context_data_first_ptr_ = 0;
  context_data_last_ptr_ = 0;
}

int XmaDecoder::GetContextId(uint32_t guest_ptr) {
  static_assert_size(XMA_CONTEXT_DATA, 64);
  if (guest_ptr < context_data_first_ptr_ || guest_ptr > context_data_last_ptr_) {
    return -1;
  }
  assert_zero(guest_ptr & 0x3F);
  return (guest_ptr - context_data_first_ptr_) >> 6;
}

uint32_t XmaDecoder::AllocateContext() {
  size_t index = context_bitmap_.Acquire();
  if (index == -1) {
    // Out of contexts.
    return 0;
  }

  XmaContext& context = contexts_[index];
  assert_false(context.is_allocated());
  context.set_is_allocated(true);
  return context.guest_ptr();
}

void XmaDecoder::ReleaseContext(uint32_t guest_ptr) {
  auto context_id = GetContextId(guest_ptr);
  assert_true(context_id >= 0);

  if (worker_decode_) {
    JoinContext(context_id);
  }
  XmaContext& context = contexts_[context_id];
  assert_true(context.is_allocated());
  context.Release();
  context_bitmap_.Release(context_id);
}

bool XmaDecoder::BlockOnContext(uint32_t guest_ptr, bool poll) {
  auto context_id = GetContextId(guest_ptr);
  assert_true(context_id >= 0);

  XmaContext& context = contexts_[context_id];
  return context.Block(poll);
}

uint32_t XmaDecoder::ReadRegister(uint32_t addr) {
  auto r = (addr & 0xFFFF) / 4;

  assert_true(r < XmaRegisterFile::kRegisterCount);

  switch (r) {
    case XmaRegister::ContextArrayAddress:
      break;
    case XmaRegister::CurrentContextIndex: {
      // 0606h (1818h) is rotating context processing # set to hardware ID of
      // context being processed.
      // If bit 200h is set, the locking code will possibly collide on hardware
      // IDs and error out, so we should never set it (I think?).
      uint32_t& current_context_index = register_file_[XmaRegister::CurrentContextIndex];
      uint32_t& next_context_index = register_file_[XmaRegister::NextContextIndex];
      // To prevent games from seeing a stuck XMA context, return a rotating
      // number.
      current_context_index = next_context_index;
      next_context_index = (next_context_index + 1) % kContextCount;
      break;
    }
    default:
      const auto register_info = register_file_.GetRegisterInfo(r);
      if (register_info) {
        REXAPU_DEBUG("XMA: Read from unhandled register ({:04X}, {})", r, register_info->name);
      } else {
        REXAPU_DEBUG("XMA: Read from unknown register ({:04X})", r);
      }
      break;
  }

  return rex::byte_swap(register_file_[r]);
}

void XmaDecoder::WriteRegister(uint32_t addr, uint32_t value) {
  SCOPE_profile_cpu_f("apu");

  uint32_t r = (addr & 0xFFFF) / 4;
  value = rex::byte_swap(value);

  assert_true(r < XmaRegisterFile::kRegisterCount);
  register_file_[r] = value;

  if (r >= XmaRegister::Context0Kick && r <= XmaRegister::Context9Kick) {
    // Context kick command.
    // This will kick off the given hardware contexts.
    // Basically, this kicks the SPU and says "hey, decode that audio!"
    // XMAEnableContext

    // The context ID is a bit in the range of the entire context array.
    uint32_t base_context_id = (r - XmaRegister::Context0Kick) * 32;
    uint32_t kicked_value = value;
    if (worker_decode_) {
      QueueJobs(base_context_id, kicked_value);
      work_event_->Set();
      return;
    }
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        uint32_t context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        context.Enable();
      }
    }
    // Decode inline. waiting on the worker sweep stalls the realtime
    // audio thread 50-100ms during kick bursts.
    for (int i = 0; kicked_value && i < 32; ++i, kicked_value >>= 1) {
      if (kicked_value & 1) {
        uint32_t context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        if (context.Work()) {
          context.SignalWorkDone();
        }
      }
    }
    work_event_->Set();
  } else if (r >= XmaRegister::Context0Lock && r <= XmaRegister::Context9Lock) {
    // Context lock command.
    // This requests a lock by flagging the context.
    // XMADisableContext
    uint32_t base_context_id = (r - XmaRegister::Context0Lock) * 32;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        uint32_t context_id = base_context_id + i;
        if (worker_decode_) {
          // The decode the last kick asked for comes first, as when it ran at the kick.
          JoinContext(context_id);
        }
        auto& context = contexts_[context_id];
        context.Disable();
        // [XMA fix] Added Block(false) after Disable(). Without this, the game
        // could call XMADisableContext and start modifying the context struct
        // while a decode was still in progress on the worker thread. Block()
        // waits for the context mutex to be free (poll=false means wait, not spin).
        context.Block(false);
      }
    }
    // Signal the decoder thread to start processing.
    // work_event_->Set();
  } else if (r >= XmaRegister::Context0Clear && r <= XmaRegister::Context9Clear) {
    // Context clear command.
    // This will reset the given hardware contexts.
    uint32_t base_context_id = (r - XmaRegister::Context0Clear) * 32;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        uint32_t context_id = base_context_id + i;
        if (worker_decode_) {
          JoinContext(context_id);
        }
        XmaContext& context = contexts_[context_id];
        context.Clear();
      }
    }
  } else {
    // 0601h (1804h) is written to with 0x02000000 and 0x03000000 around a lock
    // operation
    switch (r) {
      default: {
        const auto register_info = register_file_.GetRegisterInfo(r);
        if (register_info) {
          REXAPU_DEBUG("XMA: Write to unhandled register ({:04X}, {}): {:08X}", r,
                       register_info->name, value);
        } else {
          REXAPU_DEBUG("XMA: Write to unknown register ({:04X}): {:08X}", r, value);
        }
        break;
      }
#pragma warning(suppress : 4065)
    }
  }
}

void XmaDecoder::QueueJobs(uint32_t base_context_id, uint32_t kicked_value) {
  // A context kicked again before its last job ran gets that decode first, so that each kick
  // has its own, as at the kick.
  for (uint32_t i = 0, bits = kicked_value; bits && i < 32; ++i, bits >>= 1) {
    if (bits & 1) {
      JoinContext(base_context_id + i);
    }
  }
  for (uint32_t i = 0, bits = kicked_value; bits && i < 32; ++i, bits >>= 1) {
    if (bits & 1) {
      contexts_[base_context_id + i].Enable();
    }
  }
  std::unique_lock<std::mutex> lock(job_mutex_);
  for (uint32_t i = 0, bits = kicked_value; bits && i < 32; ++i, bits >>= 1) {
    if (!(bits & 1)) {
      continue;
    }
    const uint32_t context_id = base_context_id + i;
    while (job_state_[context_id].load(std::memory_order_relaxed) != JobState::kIdle) {
      // Another thread kicked it since the join above. Its job may have run before this
      // thread's Enable(): this kick still needs one of its own.
      lock.unlock();
      JoinContext(context_id);
      lock.lock();
    }
    job_state_[context_id].store(JobState::kQueued, std::memory_order_relaxed);
    job_next_[context_id] = -1;
    job_prev_[context_id] = job_tail_;
    if (job_tail_ >= 0) {
      job_next_[job_tail_] = static_cast<int16_t>(context_id);
    } else {
      job_head_ = static_cast<int16_t>(context_id);
    }
    job_tail_ = static_cast<int16_t>(context_id);
    ++jobs_queued_;
  }
}

void XmaDecoder::UnlinkJobLocked(uint32_t context_id) {
  const int16_t prev = job_prev_[context_id];
  const int16_t next = job_next_[context_id];
  if (prev >= 0) {
    job_next_[prev] = next;
  } else {
    job_head_ = next;
  }
  if (next >= 0) {
    job_prev_[next] = prev;
  } else {
    job_tail_ = prev;
  }
  job_next_[context_id] = -1;
  job_prev_[context_id] = -1;
}

void XmaDecoder::RunJob(uint32_t context_id) {
  // As the inline decode at the kick.
  XmaContext& context = contexts_[context_id];
  if (context.Work()) {
    context.SignalWorkDone();
  }
}

void XmaDecoder::FinishJob(uint32_t context_id) {
  std::lock_guard<std::mutex> lock(job_mutex_);
  // Release: a join that sees kIdle without the mutex also sees what the decode wrote.
  job_state_[context_id].store(JobState::kIdle, std::memory_order_release);
  if (job_waiters_) {
    job_done_.notify_all();
  }
}

bool XmaDecoder::RunQueuedJobs() {
  bool did_work = false;
  while (worker_running_) {
    uint32_t context_id;
    {
      std::lock_guard<std::mutex> lock(job_mutex_);
      if (job_head_ < 0) {
        break;
      }
      context_id = static_cast<uint32_t>(job_head_);
      UnlinkJobLocked(context_id);
      job_state_[context_id].store(JobState::kRunning, std::memory_order_relaxed);
    }
    RunJob(context_id);
    FinishJob(context_id);
    did_work = true;
  }
  return did_work;
}

void XmaDecoder::JoinContext(uint32_t context_id) {
  std::unique_lock<std::mutex> lock(job_mutex_);
  const JobState state = job_state_[context_id].load(std::memory_order_relaxed);
  if (state == JobState::kIdle) {
    return;
  }
  if (state == JobState::kQueued) {
    // The decoder thread has not reached it: decode it here, as the kick would have. The game
    // often locks a context again right after kicking it, to take the next frame.
    UnlinkJobLocked(context_id);
    job_state_[context_id].store(JobState::kRunning, std::memory_order_relaxed);
    ++jobs_taken_;
    lock.unlock();
    RunJob(context_id);
    FinishJob(context_id);
    return;
  }
  // Running on the decoder thread (or on another thread that joined it). A decode takes a few
  // microseconds: poll a little before sleeping, which costs a wakeup.
  lock.unlock();
  const auto start = std::chrono::steady_clock::now();
  auto end = start;
  bool done = false;
  for (uint32_t polls = 0;; ++polls) {
    if (job_state_[context_id].load(std::memory_order_acquire) == JobState::kIdle) {
      done = true;
      break;
    }
    if ((polls & 7) == 7) {
      end = std::chrono::steady_clock::now();
      if (end - start >= std::chrono::microseconds(50)) {
        break;
      }
    }
    rex::thread::MaybeYield();
  }
  lock.lock();
  if (!done) {
    ++job_waiters_;
    job_done_.wait(lock, [&] {
      return job_state_[context_id].load(std::memory_order_relaxed) == JobState::kIdle;
    });
    --job_waiters_;
  }
  end = std::chrono::steady_clock::now();
  const uint64_t us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
  ++jobs_waited_;
  job_wait_total_us_ += us;
  job_wait_max_us_ = std::max(job_wait_max_us_, us);
  bool log = false;
  if (us >= 1000) {
    // A wait this long can delay the game's audio frame.
    ++jobs_waited_long_;
    log = end - long_wait_log_time_ >= std::chrono::seconds(60);
  }
  lock.unlock();
  if (log) {
    LogJobStats();
  }
}

void XmaDecoder::LogJobStats() {
  std::lock_guard<std::mutex> lock(job_mutex_);
  long_wait_log_time_ = std::chrono::steady_clock::now();
  REXAPU_INFO(
      "XMA decoder thread: {} decodes, {} of them on the game's thread at its next use of the "
      "context; {} waited for ({} of 1 ms or more, {} since the last line; longest {} us, {} us "
      "in all)",
      jobs_queued_, jobs_taken_, jobs_waited_, jobs_waited_long_,
      jobs_waited_long_ - jobs_waited_long_logged_, job_wait_max_us_, job_wait_total_us_);
  jobs_waited_long_logged_ = jobs_waited_long_;
}

void XmaDecoder::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  if (work_event_) {
    work_event_->Set();
  }
  pause_fence_.Wait();
}

void XmaDecoder::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  resume_fence_.Signal();
}

}  // namespace rex::audio
