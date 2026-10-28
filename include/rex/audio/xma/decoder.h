/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/register_file.h>
#include <rex/bit.h>
#include <rex/kernel.h>
#include <rex/system/xthread.h>

namespace rex::runtime {
class FunctionDispatcher;
}  // namespace rex::runtime

namespace rex::audio {

struct XMA_CONTEXT_DATA;

class XmaDecoder {
 public:
  explicit XmaDecoder(runtime::FunctionDispatcher* function_dispatcher);
  ~XmaDecoder();

  memory::Memory* memory() const { return memory_; }
  runtime::FunctionDispatcher* function_dispatcher() const { return function_dispatcher_; }

  X_STATUS Setup(system::KernelState* kernel_state);
  void Shutdown();

  uint32_t context_array_ptr() const { return register_file_[XmaRegister::ContextArrayAddress]; }

  uint32_t AllocateContext();
  void ReleaseContext(uint32_t guest_ptr);
  bool BlockOnContext(uint32_t guest_ptr, bool poll);

  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

 protected:
  int GetContextId(uint32_t guest_ptr);

 private:
  void WorkerThreadMain();

  // xma_worker_decode: the decode a kick asks for runs on the decoder thread as a job, one per
  // kick, in kick order. The game's next kick, lock or clear of that context, and its release,
  // first join the job: they run it themselves if the decoder thread has not started it, or wait
  // for it to finish. So each kick still gets one decode of the context as the game left it, and
  // the game finds it done when it next works with the context, as with the inline decode.
  // job_state_ is written under job_mutex_; a join waiting for a running job polls it first.
  enum class JobState : uint8_t { kIdle, kQueued, kRunning };
  void QueueJobs(uint32_t base_context_id, uint32_t kicked_value);
  void JoinContext(uint32_t context_id);
  bool RunQueuedJobs();
  void RunJob(uint32_t context_id);
  void FinishJob(uint32_t context_id);
  void UnlinkJobLocked(uint32_t context_id);
  void LogJobStats();

  static uint32_t MMIOReadRegisterThunk(void* ppc_context, XmaDecoder* as, uint32_t addr) {
    return as->ReadRegister(addr);
  }
  static void MMIOWriteRegisterThunk(void* ppc_context, XmaDecoder* as, uint32_t addr,
                                     uint32_t value) {
    as->WriteRegister(addr, value);
  }

 protected:
  memory::Memory* memory_ = nullptr;
  runtime::FunctionDispatcher* function_dispatcher_ = nullptr;

  std::atomic<bool> worker_running_ = {false};
  system::object_ref<system::XHostThread> worker_thread_;
  std::unique_ptr<rex::thread::Event> work_event_ = nullptr;

  std::atomic<bool> paused_ = false;
  rex::thread::Fence pause_fence_;   // Signaled when worker paused.
  rex::thread::Fence resume_fence_;  // Signaled when resume requested.

  XmaRegisterFile register_file_;

  static const uint32_t kContextCount = 320;
  XmaContext contexts_[kContextCount];
  bit::BitMap context_bitmap_;

  uint32_t context_data_first_ptr_ = 0;
  uint32_t context_data_last_ptr_ = 0;

  // xma_worker_decode, read once in Setup.
  bool worker_decode_ = false;
  // The queued jobs: a list in kick order through job_next_/job_prev_ (-1 ends it), so a job the
  // game joins before the decoder thread reaches it leaves the list at once. A context has at
  // most one job: its next kick joins the last one first.
  std::mutex job_mutex_;
  std::condition_variable job_done_;
  std::array<std::atomic<JobState>, kContextCount> job_state_{};
  std::array<int16_t, kContextCount> job_next_{};
  std::array<int16_t, kContextCount> job_prev_{};
  int16_t job_head_ = -1;
  int16_t job_tail_ = -1;
  uint32_t job_waiters_ = 0;
  // Joins that found the job not done: run on the joining thread (taken) or waited for.
  uint64_t jobs_queued_ = 0;
  uint64_t jobs_taken_ = 0;
  uint64_t jobs_waited_ = 0;
  uint64_t jobs_waited_long_ = 0;  // 1 ms or more
  uint64_t job_wait_max_us_ = 0;
  uint64_t job_wait_total_us_ = 0;
  uint64_t jobs_waited_long_logged_ = 0;
  std::chrono::steady_clock::time_point long_wait_log_time_{};
  // The first summary, a minute after the first decode (the game exits without Shutdown()).
  std::chrono::steady_clock::time_point first_job_time_{};
  bool first_stats_logged_ = false;
};

}  // namespace rex::audio
