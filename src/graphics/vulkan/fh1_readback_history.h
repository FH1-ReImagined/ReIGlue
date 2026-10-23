#pragma once
// FH1 (#45): which resolves "readback_resolve = some" need not copy back again after a gap.
//
// "some" copies a resolve back to guest memory (a wait for the GPU, then a copy) the first time
// its key (address, length, source, format) is seen, and again whenever its readback buffer was
// evicted after 60 frames unused: car pictures and photos, made now and then, must reach the
// CPU each time. But the scene's own render targets are resolved every frame and not read by
// the CPU (after their first copy "some" leaves them stale on purpose); after the map or a long
// menu (over 60 frames without the world) every one of them was a first-seen key again: ten
// copies, 24 MB, 25-30 ms of the command processor and a wait for the GPU in the first frame
// back, more on a Steam Deck.
//
// A key resolved in kHotRunFrames frames in a row is such a render target: when it comes back
// after its buffer was evicted it is treated as copied. One-off resolves (pictures, photos)
// never run that long and keep their copy. Free of Vulkan, so tools/test_readback_history.cpp
// can test it.
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace rex::graphics::vulkan::fh1_readback_history {

// Frames in a row (a gap of one frame allowed) that make a key a per-frame render target.
constexpr uint32_t kHotRunFrames = 150;
// Keys not resolved for this many frames are forgotten (about 30 minutes at 30 fps), and at
// most kMaxKeys are kept (the oldest go first).
constexpr uint64_t kForgetFrames = 54000;
constexpr size_t kMaxKeys = 2048;

class History {
 public:
  struct Entry {
    uint64_t last_frame = 0;
    uint32_t run = 0;          // frames in the current run
    uint32_t longest_run = 0;  // the longest run so far
  };

  // Every resolve of `key` (readback_resolve "some") at frame `frame`.
  void Resolved(uint64_t key, uint64_t frame) {
    auto [it, inserted] = entries_.try_emplace(key);
    Entry& e = it->second;
    if (inserted) {
      e.last_frame = frame;
      e.run = 1;
    } else if (frame != e.last_frame) {
      e.run = frame - e.last_frame <= 2 ? e.run + 1 : 1;
      e.last_frame = frame;
    }
    if (e.run > e.longest_run) e.longest_run = e.run;
  }

  // A key whose readback buffer is new (first seen, or evicted): true when it is a per-frame
  // render target already copied back once, so the copy can be skipped.
  bool IsHot(uint64_t key) const {
    auto it = entries_.find(key);
    return it != entries_.end() && it->second.longest_run >= kHotRunFrames;
  }

  // Now and then: drop keys not seen for kForgetFrames, then the oldest past kMaxKeys.
  void Forget(uint64_t frame) {
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (frame > it->second.last_frame && frame - it->second.last_frame > kForgetFrames) {
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
    while (entries_.size() > kMaxKeys) {
      auto oldest = entries_.begin();
      for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->second.last_frame < oldest->second.last_frame) oldest = it;
      }
      entries_.erase(oldest);
    }
  }

  size_t size() const { return entries_.size(); }

 private:
  std::unordered_map<uint64_t, Entry> entries_;
};

}  // namespace rex::graphics::vulkan::fh1_readback_history
