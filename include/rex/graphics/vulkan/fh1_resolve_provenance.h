#pragma once

#include <optional>

#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/vulkan/fh1_screen_raster.h>

namespace rex::graphics::vulkan::fh1 {

// Resolve provenance describes the latest known writer, not just the latest
// resolve we happened to record. Both CPU and unclassified GPU writes expire it.
class ResolveProvenance {
 public:
  ResolveProvenance(TextureCache& textures, SharedMemory& memory)
      : textures_(textures), memory_(memory) {
    textures_.Fh1TrackFractionalResolves();
    watch_ = memory_.RegisterGlobalWatch(OnWrite, this);
  }

  ResolveProvenance(const ResolveProvenance&) = delete;
  ResolveProvenance& operator=(const ResolveProvenance&) = delete;
  ~ResolveProvenance() { memory_.UnregisterGlobalWatch(watch_); }

  void Publish(TextureCache::Fh1ResolveRaster raster, const ResolveTrace& trace) {
    auto lock = thread::global_critical_region::AcquireDirect();
    // MarkRangeAsResolved notifies all overlapping watches before publishing
    // the replacement evidence. Keep the transaction ordered against CPU writes.
    textures_.MarkRangeAsResolved(trace.written_address, trace.written_length);
    textures_.Fh1MarkResolveRaster(trace.written_address, trace.written_length, raster);
    history_.Record(trace);
  }

  std::optional<ResolveTrace> Find(uint32_t page) const {
    auto lock = thread::global_critical_region::AcquireDirect();
    const auto* trace = history_.Find(page);
    return trace && memory_.IsRangeAllGpuWritten(trace->written_address, trace->written_length)
               ? std::optional<ResolveTrace>(*trace) : std::nullopt;
  }

  void InvalidateAll() {
    auto lock = thread::global_critical_region::AcquireDirect();
    OnWrite(lock, this, 0, SharedMemory::kBufferSize - 1, false);
  }

 private:
  static void OnWrite(const std::unique_lock<std::recursive_mutex>&, void* context,
                      uint32_t first, uint32_t last, bool) {
    auto& state = *static_cast<ResolveProvenance*>(context);
    state.textures_.Fh1MarkResolveRaster(first, last - first + 1, TextureCache::Fh1ResolveRaster::kFull);
    state.history_.Invalidate(first >> 12, last >> 12);
  }

  TextureCache& textures_;
  SharedMemory& memory_;
  SharedMemory::GlobalWatchHandle watch_;
  ResolveTraceHistory history_;
};

}  // namespace rex::graphics::vulkan::fh1
