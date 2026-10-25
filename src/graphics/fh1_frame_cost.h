#pragma once
// FH1 (#45): what the GPU command processor did in each frame, so that frames much longer than
// usual say why in the log. On a Steam Deck the command processor is the thread every frame
// waits for, and the frame rate collapses near other cars, after a fast camera turn and back
// from the map; a player's log only showed the 200 ms stalls (FORZA_STALL), not why 30 fps
// fell to 12.
//
// The counters of the open frame (Current()) are written on the command processor thread only
// (pipelines the background threads create are not counted, the placeholders drawn meanwhile
// are). The Vulkan command processor closes a frame at each swap (EndFrame there) and hands it
// to a Report, which collects the slow frames and makes at most one log line every 10 s.
// Free of Vulkan and the runtime, so tools/test_frame_cost.cpp can test it.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rex::graphics::fh1_frame_cost {

struct Frame {
  double ms = 0;             // since the previous frame end
  double cpu_ms = 0;         // the command processor thread's CPU time (0 where unknown)
  // cpu_ms is the frame's own (false: a coarse clock's step, which only means something summed
  // over many frames; CpuClock below).
  bool cpu_per_frame = true;
  double gpu_wait_ms = 0;    // blocked on GPU fences (frames in flight, readbacks, trims)
  double guest_wait_ms = 0;  // WAIT_REG_MEM: waiting for the game to write a value
  uint32_t draws = 0;
  uint32_t textures_created = 0;
  uint64_t texture_created_bytes = 0;  // host memory of the new textures
  double texture_create_ms = 0;
  uint32_t texture_loads = 0;  // textures (re)loaded from guest memory (untiled on the GPU)
  uint64_t texture_load_bytes = 0;  // their guest data
  double texture_load_ms = 0;       // recording the loads
  uint32_t textures_evicted = 0;    // destroyed by the texture cache's memory limits
  uint64_t texture_evicted_bytes = 0;
  uint64_t upload_bytes = 0;  // guest memory copied to the GPU's copy of it (CPU writes)
  double upload_ms = 0;
  uint32_t shaders = 0;  // guest shaders translated
  double shader_ms = 0;
  uint32_t pipelines = 0;  // pipelines created on this thread (async placeholders included)
  double pipeline_ms = 0;
  uint32_t render_targets = 0;  // render target images created
  uint64_t render_target_bytes = 0;
  double render_target_ms = 0;
  uint32_t render_target_trims = 0;
  uint32_t readbacks = 0;  // resolves copied back to guest memory (readback_resolve)
  uint64_t readback_bytes = 0;
  double readback_ms = 0;  // their buffers and copies
  double readback_wait_ms = 0;  // their waits for the GPU (also in gpu_wait_ms)
  uint32_t readbacks_skipped = 0;  // scene render targets back after a gap, not copied (0181)

  void Add(const Frame& o) {
    ms += o.ms;
    cpu_ms += o.cpu_ms;
    cpu_per_frame = cpu_per_frame && o.cpu_per_frame;
    gpu_wait_ms += o.gpu_wait_ms;
    guest_wait_ms += o.guest_wait_ms;
    draws += o.draws;
    textures_created += o.textures_created;
    texture_created_bytes += o.texture_created_bytes;
    texture_create_ms += o.texture_create_ms;
    texture_loads += o.texture_loads;
    texture_load_bytes += o.texture_load_bytes;
    texture_load_ms += o.texture_load_ms;
    textures_evicted += o.textures_evicted;
    texture_evicted_bytes += o.texture_evicted_bytes;
    upload_bytes += o.upload_bytes;
    upload_ms += o.upload_ms;
    shaders += o.shaders;
    shader_ms += o.shader_ms;
    pipelines += o.pipelines;
    pipeline_ms += o.pipeline_ms;
    render_targets += o.render_targets;
    render_target_bytes += o.render_target_bytes;
    render_target_ms += o.render_target_ms;
    render_target_trims += o.render_target_trims;
    readbacks += o.readbacks;
    readback_bytes += o.readback_bytes;
    readback_ms += o.readback_ms;
    readback_wait_ms += o.readback_wait_ms;
    readbacks_skipped += o.readbacks_skipped;
  }
};

// The open frame's counters (command processor thread).
inline Frame& Current() {
  static Frame frame;
  return frame;
}

// The host memory of all cached textures (the texture caches' own count, summed).
inline uint64_t& TextureBytes() {
  static uint64_t bytes = 0;
  return bytes;
}

// FH1 (#400): the command processor thread's CPU time per frame, from the clocks the platform
// has (CpuClock::EndFrame takes their readings at each frame end, in ms since the thread started,
// negative where a clock is missing):
// - a precise one: CLOCK_THREAD_CPUTIME_ID on Linux and macOS (Precise::kDirect); on Windows
//   the thread's cycles (QueryThreadCycleTime) at the time stamp counter's rate (CyclesPerMs),
//   only where the CPU reports an invariant time stamp counter (InvariantTsc). Microsoft does
//   not promise that these cycles run at that rate, so on Windows (Precise::kChecked) the
//   conversion times a frame only once CpuTimeCheck found it agreeing with GetThreadTimes, not
//   again after a check that disagrees, and not in a frame whose GetThreadTimes reading failed;
// - a coarse one (Windows: GetThreadTimes, 100 ns units but advancing in scheduler ticks of
//   about 15.6 ms, sampled): its steps are kept as the frame's cpu_ms with cpu_per_frame false,
//   right only summed over many frames (the minute line, the slow-frame window's average);
//   the slow-frame cause then ignores the CPU time.

// Cycles per ms from two samples of the time stamp counter and QueryPerformanceCounter; 0 when
// the samples cannot tell (no time passed, a counter went back).
inline double CyclesPerMs(uint64_t tsc_first, uint64_t tsc_last, int64_t qpc_first,
                          int64_t qpc_last, int64_t qpc_frequency) {
  if (qpc_frequency <= 0 || qpc_last <= qpc_first || tsc_last <= tsc_first) return 0;
  const double ms = double(qpc_last - qpc_first) * 1000.0 / double(qpc_frequency);
  return double(tsc_last - tsc_first) / ms;
}
inline double CyclesToMs(uint64_t cycles, double cycles_per_ms) {
  return cycles_per_ms > 0 ? double(cycles) / cycles_per_ms : -1;
}
// CPUID: the highest extended leaf (0x80000000 EAX) and leaf 0x80000007's EDX, bit 8 of which
// says the time stamp counter runs at a constant rate in every power state.
inline bool InvariantTsc(uint32_t max_extended_leaf, uint32_t edx_80000007) {
  return max_extended_leaf >= 0x80000007u && (edx_80000007 >> 8 & 1);
}
// GetThreadTimes' kernel plus user time (100 ns units) in ms.
inline double ThreadTimesToMs(uint64_t kernel_100ns, uint64_t user_100ns) {
  return double(kernel_100ns + user_100ns) / 10000.0;
}

// Checks the precise clock against the coarse one: each time the coarse one has advanced
// kCheckMs (enough scheduler ticks for its sampling to average out), the precise one must have
// advanced as much, within kTolerance.
class CpuTimeCheck {
 public:
  static constexpr double kCheckMs = 5000.0;
  static constexpr double kTolerance = 0.25;
  enum class State { kUnchecked, kAgrees, kDisagrees };

  State Update(double precise_ms, double coarse_ms) {
    if (state_ == State::kDisagrees) return state_;
    if (!started_) {
      started_ = true;
      precise_start_ = precise_ms;
      coarse_start_ = coarse_ms;
      return state_;
    }
    const double coarse = coarse_ms - coarse_start_;
    if (coarse < kCheckMs) return state_;
    ratio_ = (precise_ms - precise_start_) / coarse;
    state_ = ratio_ >= 1.0 - kTolerance && ratio_ <= 1.0 + kTolerance ? State::kAgrees
                                                                      : State::kDisagrees;
    precise_start_ = precise_ms;
    coarse_start_ = coarse_ms;
    return state_;
  }
  State state() const { return state_; }
  double ratio() const { return ratio_; }  // the last check's precise / coarse

 private:
  State state_ = State::kUnchecked;
  bool started_ = false;
  double precise_start_ = 0, coarse_start_ = 0, ratio_ = 0;
};

class CpuClock {
 public:
  // How far the precise clock is trusted: kDirect, a thread CPU-time clock of the system
  // (Linux, macOS); kChecked, a conversion (Windows' cycles) that times single frames only while
  // CpuTimeCheck agrees and the coarse reference was read in the same frame end.
  enum class Precise { kDirect, kChecked };
  explicit CpuClock(Precise precise) : precise_(precise) {}

  // A frame's end: sets its cpu_ms and cpu_per_frame. A step is taken only between two readings
  // of the same clock at consecutive frame ends; a missing reading (negative) leaves the frame
  // and the next one without that clock's step, and unknown frames get 0, not per frame.
  void EndFrame(double precise_ms, double coarse_ms, Frame& frame) {
    const bool has_precise = precise_ms >= 0, has_coarse = coarse_ms >= 0;
    bool use_precise = has_precise && had_precise_;
    if (precise_ == Precise::kChecked) {
      // Fail closed: no check, and no per-frame time, without both readings; a rejection
      // stays (CpuTimeCheck::Update).
      const CpuTimeCheck::State state =
          has_precise && has_coarse ? check_.Update(precise_ms, coarse_ms) : check_.state();
      use_precise = use_precise && has_coarse && state == CpuTimeCheck::State::kAgrees;
    }
    frame.cpu_per_frame = use_precise;
    frame.cpu_ms = 0;
    if (use_precise) {
      frame.cpu_ms = std::max(0.0, precise_ms - last_precise_ms_);
    } else if (has_coarse && had_coarse_) {
      frame.cpu_ms = std::max(0.0, coarse_ms - last_coarse_ms_);
    }
    had_precise_ = has_precise;
    had_coarse_ = has_coarse;
    if (has_precise) last_precise_ms_ = precise_ms;
    if (has_coarse) last_coarse_ms_ = coarse_ms;
  }
  const CpuTimeCheck& check() const { return check_; }

 private:
  Precise precise_;
  CpuTimeCheck check_;
  bool had_precise_ = false, had_coarse_ = false;  // the previous frame end's readings
  double last_precise_ms_ = 0, last_coarse_ms_ = 0;
};

// Adds the time of its scope to a counter (milliseconds).
class Timer {
 public:
  explicit Timer(double& ms) : ms_(ms), start_(std::chrono::steady_clock::now()) {}
  ~Timer() {
    ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_)
               .count();
  }
  Timer(const Timer&) = delete;
  Timer& operator=(const Timer&) = delete;

 private:
  double& ms_;
  std::chrono::steady_clock::time_point start_;
};

// What made the slow frames slow, as the report names it.
enum class Cause {
  kUnclear,
  kGame,  // the command processor was mostly idle: the game's frame came late
  kDraws,
  kTextures,
  kUploads,
  kShaders,
  kRenderTargets,
  kReadbacks,
  kGpuWaits,
};

inline const char* CauseName(Cause cause) {
  switch (cause) {
    case Cause::kGame:
      return "the game (the command processor waited for its commands)";
    case Cause::kDraws:
      return "more draws";
    case Cause::kTextures:
      return "textures";
    case Cause::kUploads:
      return "guest memory uploads";
    case Cause::kShaders:
      return "shaders and pipelines";
    case Cause::kRenderTargets:
      return "render targets";
    case Cause::kReadbacks:
      return "resolve readbacks";
    case Cause::kGpuWaits:
      return "waits for the GPU";
    default:
      return "unclear";
  }
}

// Collects the slow frames and decides when to log them.
class Report {
 public:
  // A frame is slow when it takes this long and kSlowFactor times the typical frame.
  static constexpr double kSlowMs = 50.0;
  static constexpr double kSlowFactor = 1.5;
  // No report in the first seconds (loading the game), then at most one line per interval;
  // after kMaxLines lines, one per kLateIntervalSeconds.
  static constexpr double kStartSeconds = 30.0;
  static constexpr double kIntervalSeconds = 10.0;
  static constexpr double kLateIntervalSeconds = 60.0;
  static constexpr uint32_t kMaxLines = 60;
  // A cause is named when it takes at least this much of a slow frame on average.
  static constexpr double kCauseMs = 4.0;

  // Every frame end; true when a line is ready (Line(), then Taken()).
  bool OnFrame(const Frame& f, double now_seconds) {
    if (!started_) {
      started_ = true;
      start_seconds_ = now_seconds;
    }
    const bool starting = now_seconds - start_seconds_ < kStartSeconds;
    const bool slow = !starting && f.ms >= SlowThresholdMs();
    // The typical frame: an average of the frames that are not slow (all of them while the game
    // starts). Slow frames move it a little, so that a new frame rate setting (20 fps) becomes
    // the typical one within half a minute.
    if (!typical_frames_) {
      typical_ms_ = f.ms;
      typical_draws_ = f.draws;
    } else {
      const double weight = slow ? kSlowTypicalWeight : kTypicalWeight;
      typical_ms_ += (f.ms - typical_ms_) * weight;
      typical_draws_ += (double(f.draws) - typical_draws_) * weight;
    }
    // The typical CPU time only from per-frame values (#400), restarted after coarse ones.
    if (!f.cpu_per_frame) {
      typical_cpu_frames_ = 0;
      typical_cpu_ms_ = 0;
    } else if (!typical_cpu_frames_++) {
      typical_cpu_ms_ = f.cpu_ms;
    } else {
      typical_cpu_ms_ += (f.cpu_ms - typical_cpu_ms_) * (slow ? kSlowTypicalWeight : kTypicalWeight);
    }
    ++typical_frames_;
    if (starting) return false;
    if (window_open_) {
      ++window_frames_;
      window_cpu_ms_ += f.cpu_ms;
    }
    if (slow) {
      if (!window_open_) {
        window_open_ = true;
        window_start_ = now_seconds;
        window_frames_ = 1;
        window_cpu_ms_ = f.cpu_ms;
        slow_ = Frame();
        slow_count_ = 0;
        worst_ms_ = 0;
      }
      slow_.Add(f);
      ++slow_count_;
      worst_ms_ = std::max(worst_ms_, f.ms);
    }
    const double interval = lines_ < kMaxLines ? kIntervalSeconds : kLateIntervalSeconds;
    return window_open_ && now_seconds - window_start_ >= interval;
  }

  void Taken() {
    window_open_ = false;
    ++lines_;
  }

  double SlowThresholdMs() const {
    return typical_frames_ ? std::max(kSlowMs, kSlowFactor * typical_ms_) : kSlowMs;
  }

  // The slow frames' costs, each averaged over them.
  Cause MainCause() const {
    if (!slow_count_) return Cause::kUnclear;
    const double n = double(slow_count_);
    const double ms = slow_.ms / n;
    const double cpu = slow_.cpu_ms / n;
    const double gpu_wait = slow_.gpu_wait_ms / n;
    // Mostly idle (neither working nor waiting for the GPU): the game was late. Only with the
    // slow frames' own CPU time (#400).
    if (slow_.cpu_per_frame && cpu > 0 && cpu + gpu_wait < 0.5 * ms) return Cause::kGame;
    double extra_draws_ms = 0;
    if (slow_.cpu_per_frame && typical_cpu_frames_ && typical_draws_ > 0 && typical_cpu_ms_ > 0) {
      const double draws = double(slow_.draws) / n;
      extra_draws_ms = std::max(0.0, draws - typical_draws_) * (typical_cpu_ms_ / typical_draws_);
    }
    const struct {
      Cause cause;
      double ms;
    } costs[] = {
        {Cause::kDraws, extra_draws_ms},
        {Cause::kTextures, (slow_.texture_create_ms + slow_.texture_load_ms) / n},
        {Cause::kUploads, slow_.upload_ms / n},
        {Cause::kShaders, (slow_.shader_ms + slow_.pipeline_ms) / n},
        {Cause::kRenderTargets, slow_.render_target_ms / n},
        {Cause::kReadbacks, (slow_.readback_ms + slow_.readback_wait_ms) / n},
        {Cause::kGpuWaits, std::max(0.0, gpu_wait - slow_.readback_wait_ms / n)},
    };
    Cause best = Cause::kUnclear;
    double best_ms = kCauseMs;
    for (const auto& c : costs) {
      if (c.ms >= best_ms) {
        best = c.cause;
        best_ms = c.ms;
      }
    }
    return best;
  }

  // The log line for the window: how many frames were slow and what each did on average.
  std::string Line(double now_seconds) const {
    const double n = slow_count_ ? double(slow_count_) : 1.0;
    // The slow frames' own CPU time, or only the window's average per frame (#400).
    char cpu[96];
    if (slow_.cpu_per_frame) {
      std::snprintf(cpu, sizeof(cpu), "%.0f ms CPU", slow_.cpu_ms / n);
    } else {
      std::snprintf(cpu, sizeof(cpu), "CPU not timed per frame (%.1f ms a frame in the window)",
                    window_frames_ ? window_cpu_ms_ / double(window_frames_) : 0.0);
    }
    char text[1400];
    std::snprintf(
        text, sizeof(text),
        "GPU slow frames: %u of %u in %.0f s over %.0f ms (worst %.0f ms, typical %.0f ms); "
        "each: command processor %s, %.0f ms GPU waits, %.0f ms game waits, %.0f draws "
        "(typical %.0f); textures %.1f new (%.1f MB, %.1f ms), %.1f loaded (%.1f MB, %.1f ms), "
        "%.1f evicted (%.1f MB); uploads %.1f MB (%.1f ms); shaders %.1f (%.1f ms), pipelines "
        "%.1f (%.1f ms); render targets %.1f new (%.1f MB, %.1f ms), %u trims; resolve readbacks "
        "%.1f (%.1f MB, %.1f ms, %.1f ms GPU waits); cause: %s",
        slow_count_, window_frames_, now_seconds - window_start_, SlowThresholdMs(), worst_ms_,
        typical_ms_, cpu, slow_.gpu_wait_ms / n, slow_.guest_wait_ms / n,
        double(slow_.draws) / n, typical_draws_, slow_.textures_created / n,
        Mb(slow_.texture_created_bytes) / n, slow_.texture_create_ms / n, slow_.texture_loads / n,
        Mb(slow_.texture_load_bytes) / n, slow_.texture_load_ms / n, slow_.textures_evicted / n,
        Mb(slow_.texture_evicted_bytes) / n, Mb(slow_.upload_bytes) / n, slow_.upload_ms / n,
        slow_.shaders / n, slow_.shader_ms / n, slow_.pipelines / n, slow_.pipeline_ms / n,
        slow_.render_targets / n, Mb(slow_.render_target_bytes) / n, slow_.render_target_ms / n,
        slow_.render_target_trims, slow_.readbacks / n, Mb(slow_.readback_bytes) / n,
        slow_.readback_ms / n, slow_.readback_wait_ms / n, CauseName(MainCause()));
    return text;
  }

  uint32_t slow_count() const { return slow_count_; }
  const Frame& slow() const { return slow_; }
  double typical_ms() const { return typical_ms_; }

 private:
  static constexpr double kTypicalWeight = 1.0 / 32.0;
  static constexpr double kSlowTypicalWeight = 1.0 / 512.0;
  static double Mb(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

  bool started_ = false;
  double start_seconds_ = 0;
  uint64_t typical_frames_ = 0;
  double typical_ms_ = 0, typical_cpu_ms_ = 0, typical_draws_ = 0;
  uint64_t typical_cpu_frames_ = 0;
  bool window_open_ = false;
  double window_cpu_ms_ = 0;
  double window_start_ = 0;
  uint32_t window_frames_ = 0;
  uint32_t slow_count_ = 0;
  double worst_ms_ = 0;
  Frame slow_;
  uint32_t lines_ = 0;
};

// FH1 (#45): what the GPU emulation holds, for a once-a-minute summary in the log. Steam Deck
// players see the frame rate fall after 10-30 minutes of play; with these sizes and the minute's
// frame costs side by side, a long session's log shows whether something grew and the frame
// cost with it. Kept by the caches themselves, on whichever thread changes them (relaxed
// atomics, changed only when an object is made or destroyed).
struct Sizes {
  std::atomic<int64_t> textures{0};      // texture cache entries
  std::atomic<int64_t> shaders{0};       // guest shaders known to the pipeline cache
  std::atomic<int64_t> pipelines{0};     // graphics pipeline descriptions (made or placeholder)
  std::atomic<int64_t> framebuffers{0};  // render target framebuffers
  std::atomic<int64_t> watch_ranges{0};  // shared memory ranges watched for CPU or GPU writes
  std::atomic<int64_t> watch_nodes{0};   // their bucket nodes
  // Watch nodes looked at by writes to guest memory (CPU write faults, resolves) and those
  // writes, since the start.
  std::atomic<uint64_t> watch_nodes_visited{0};
  std::atomic<uint64_t> watch_fires{0};
};

inline Sizes& GetSizes() {
  static Sizes sizes;
  return sizes;
}

inline void Count(std::atomic<int64_t>& counter, int64_t delta) {
  counter.fetch_add(delta, std::memory_order_relaxed);
}

// Everything else the summary reports, gathered when a line is due.
struct Snapshot {
  int64_t textures = 0;
  uint64_t texture_bytes = 0;
  uint32_t render_targets = 0;
  uint64_t render_target_bytes = 0;
  int64_t shaders = 0;
  int64_t pipelines = 0;
  int64_t framebuffers = 0;
  int64_t watch_ranges = 0;
  int64_t watch_nodes = 0;
  uint64_t watch_nodes_visited = 0;  // since the start
  uint64_t watch_fires = 0;          // since the start
  uint32_t readback_buffers = 0;
  uint64_t readback_buffer_bytes = 0;
  uint32_t readback_keys = 0;
  uint64_t guest_virtual_bytes = 0;   // reserved in the virtual heaps
  uint64_t guest_physical_bytes = 0;  // reserved physical memory
  uint64_t device_local_usage = 0;    // 0 when the driver does not tell
  uint64_t device_local_budget = 0;
  uint64_t system_usage = 0;  // the GPU's use of system memory (GTT)
  uint64_t process_resident_bytes = 0;  // 0 where unknown
  // Clocks, temperature and power (0 where unknown): a hotter, slower device and more work look
  // the same in the frame rate.
  double cpu_ghz = 0;      // the fastest CPU's clock now
  double gpu_ghz = 0;      // the GPU's shader clock now
  double gpu_celsius = 0;  // its temperature
  double gpu_watts = 0;    // its power (on an APU, the whole chip's)
};

// One line a minute (the first a minute after the first frame): the minute's frame rate, its
// frame costs on average, and the sizes now.
class Summary {
 public:
  static constexpr double kIntervalSeconds = 60.0;

  // Every frame end; true when a line is due (Line()).
  bool OnFrame(const Frame& f, double now_seconds) {
    if (!started_) {
      started_ = true;
      window_start_ = now_seconds;
    }
    ++frames_;
    sum_.Add(f);
    frame_ms_.push_back(float(f.ms));
    return now_seconds - window_start_ >= kIntervalSeconds;
  }

  // The line for the minute that ends now; starts the next one.
  std::string Line(double now_seconds, const Snapshot& s) {
    const double seconds = std::max(now_seconds - window_start_, 1e-3);
    const double n = frames_ ? double(frames_) : 1.0;
    // 1% low: the frame rate of the slowest 1% of the frames (as RECOMP_PERF's low1).
    double low1_fps = 0, worst_ms = 0;
    if (!frame_ms_.empty()) {
      const size_t slowest = std::max<size_t>(1, frame_ms_.size() / 100);
      std::nth_element(frame_ms_.begin(), frame_ms_.begin() + (slowest - 1), frame_ms_.end(),
                       [](float a, float b) { return a > b; });
      double total = 0;
      for (size_t i = 0; i < slowest; ++i) {
        total += frame_ms_[i];
        worst_ms = std::max(worst_ms, double(frame_ms_[i]));
      }
      low1_fps = total > 0 ? 1000.0 * double(slowest) / total : 0;
    }
    const uint64_t visited = s.watch_nodes_visited - watch_nodes_visited_;
    const uint64_t fires = s.watch_fires - watch_fires_;
    char memory[128] = "";
    if (s.device_local_budget) {
      std::snprintf(memory, sizeof(memory), ", GPU memory %.0f of %.0f MB device-local, %.0f MB system",
                    Mb(s.device_local_usage), Mb(s.device_local_budget), Mb(s.system_usage));
    }
    char process[64] = "";
    if (s.process_resident_bytes) {
      std::snprintf(process, sizeof(process), ", process %.0f MB resident",
                    Mb(s.process_resident_bytes));
    }
    char sensors[128] = "";
    if (s.cpu_ghz > 0 || s.gpu_ghz > 0) {
      int used = std::snprintf(sensors, sizeof(sensors), "; clocks:");
      if (s.cpu_ghz > 0 && used > 0 && size_t(used) < sizeof(sensors)) {
        used += std::snprintf(sensors + used, sizeof(sensors) - used, " CPU up to %.2f GHz",
                              s.cpu_ghz);
      }
      if (s.gpu_ghz > 0 && used > 0 && size_t(used) < sizeof(sensors)) {
        used += std::snprintf(sensors + used, sizeof(sensors) - used, "%s GPU %.2f GHz",
                              s.cpu_ghz > 0 ? "," : "", s.gpu_ghz);
        if (s.gpu_celsius > 0 && used > 0 && size_t(used) < sizeof(sensors)) {
          used += std::snprintf(sensors + used, sizeof(sensors) - used, " %.0f C", s.gpu_celsius);
        }
        if (s.gpu_watts > 0 && used > 0 && size_t(used) < sizeof(sensors)) {
          std::snprintf(sensors + used, sizeof(sensors) - used, " %.1f W", s.gpu_watts);
        }
      }
    }
    char text[1536];
    std::snprintf(
        text, sizeof(text),
        "GPU minute %u: %.1f fps (1%% low %.1f, worst %.0f ms); a frame: command processor %.1f "
        "ms CPU (%.2f us a draw), %.1f ms GPU waits, %.1f ms game waits, %.0f draws, %.2f MB "
        "uploads, %.1f "
        "resolve readbacks, %.0f memory watch nodes checked (%.1f writes); a second: %.1f "
        "textures new, %.1f loaded, %.1f evicted, %.1f pipelines new; now: textures %lld (%.0f "
        "MB), render targets %u (%.0f MB), shaders %lld, pipelines %lld, framebuffers %lld, "
        "memory watches %lld (%lld nodes), readback buffers %u (%.0f MB, %u keys), guest memory "
        "%.0f MB virtual + %.0f MB physical%s%s%s",
        ++lines_, double(frames_) / seconds, low1_fps, worst_ms, sum_.cpu_ms / n,
        sum_.draws ? 1000.0 * sum_.cpu_ms / double(sum_.draws) : 0.0, sum_.gpu_wait_ms / n, sum_.guest_wait_ms / n, double(sum_.draws) / n,
        Mb(sum_.upload_bytes) / n, double(sum_.readbacks) / n, double(visited) / n,
        double(fires) / n, double(sum_.textures_created) / seconds,
        double(sum_.texture_loads) / seconds, double(sum_.textures_evicted) / seconds,
        double(sum_.pipelines) / seconds, (long long)s.textures, Mb(s.texture_bytes),
        s.render_targets, Mb(s.render_target_bytes), (long long)s.shaders,
        (long long)s.pipelines, (long long)s.framebuffers, (long long)s.watch_ranges,
        (long long)s.watch_nodes, s.readback_buffers, Mb(s.readback_buffer_bytes),
        s.readback_keys, Mb(s.guest_virtual_bytes), Mb(s.guest_physical_bytes), memory, process,
        sensors);
    window_start_ = now_seconds;
    frames_ = 0;
    sum_ = Frame();
    frame_ms_.clear();
    watch_nodes_visited_ = s.watch_nodes_visited;
    watch_fires_ = s.watch_fires;
    return text;
  }

  // The counters since the start, at the start (so the first minute counts only its own).
  void SetStart(uint64_t watch_nodes_visited, uint64_t watch_fires) {
    watch_nodes_visited_ = watch_nodes_visited;
    watch_fires_ = watch_fires;
  }

 private:
  static double Mb(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

  bool started_ = false;
  double window_start_ = 0;
  uint32_t frames_ = 0;
  Frame sum_;
  std::vector<float> frame_ms_;
  uint64_t watch_nodes_visited_ = 0;
  uint64_t watch_fires_ = 0;
  uint32_t lines_ = 0;
};

}  // namespace rex::graphics::fh1_frame_cost
