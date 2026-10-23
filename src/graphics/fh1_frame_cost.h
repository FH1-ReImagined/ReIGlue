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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rex::graphics::fh1_frame_cost {

struct Frame {
  double ms = 0;             // since the previous frame end
  double cpu_ms = 0;         // the command processor thread's CPU time (0 where unknown)
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
      typical_cpu_ms_ = f.cpu_ms;
      typical_draws_ = f.draws;
    } else {
      const double weight = slow ? kSlowTypicalWeight : kTypicalWeight;
      typical_ms_ += (f.ms - typical_ms_) * weight;
      typical_cpu_ms_ += (f.cpu_ms - typical_cpu_ms_) * weight;
      typical_draws_ += (double(f.draws) - typical_draws_) * weight;
    }
    ++typical_frames_;
    if (starting) return false;
    if (window_open_) {
      ++window_frames_;
    }
    if (slow) {
      if (!window_open_) {
        window_open_ = true;
        window_start_ = now_seconds;
        window_frames_ = 1;
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
    // Mostly idle (neither working nor waiting for the GPU): the game was late.
    if (cpu > 0 && cpu + gpu_wait < 0.5 * ms) return Cause::kGame;
    double extra_draws_ms = 0;
    if (typical_draws_ > 0 && typical_cpu_ms_ > 0) {
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
    char text[1280];
    std::snprintf(
        text, sizeof(text),
        "GPU slow frames: %u of %u in %.0f s over %.0f ms (worst %.0f ms, typical %.0f ms); "
        "each: command processor %.0f ms CPU, %.0f ms GPU waits, %.0f ms game waits, %.0f draws "
        "(typical %.0f); textures %.1f new (%.1f MB, %.1f ms), %.1f loaded (%.1f MB, %.1f ms), "
        "%.1f evicted (%.1f MB); uploads %.1f MB (%.1f ms); shaders %.1f (%.1f ms), pipelines "
        "%.1f (%.1f ms); render targets %.1f new (%.1f MB, %.1f ms), %u trims; resolve readbacks "
        "%.1f (%.1f MB, %.1f ms, %.1f ms GPU waits); cause: %s",
        slow_count_, window_frames_, now_seconds - window_start_, SlowThresholdMs(), worst_ms_,
        typical_ms_, slow_.cpu_ms / n, slow_.gpu_wait_ms / n, slow_.guest_wait_ms / n,
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
  bool window_open_ = false;
  double window_start_ = 0;
  uint32_t window_frames_ = 0;
  uint32_t slow_count_ = 0;
  double worst_ms_ = 0;
  Frame slow_;
  uint32_t lines_ = 0;
};

}  // namespace rex::graphics::fh1_frame_cost
