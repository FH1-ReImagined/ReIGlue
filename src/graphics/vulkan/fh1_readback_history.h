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
//
// FH1 (#536) adds RepeatTracker below: which first-seen resolves may be copied back without a
// wait for the GPU.
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

// FH1 (#536): "some" copies a resolve back the first time its key is seen, after a wait for the GPU
// (the command processor, blocked, stops feeding the GPU: 16-25 ms with the GPU at its limit).
// Almost all of those waits are not pictures the CPU reads: every car that comes into view gets
// new 64x64 (and mipmap) render targets at fresh addresses, resolved again every frame and never
// read by the CPU. But a picture the CPU does read as soon as it is resolved (a photo, a car's or
// a vinyl group's thumbnail, a livery) is also a first-seen key, and its copy must be there when
// the CPU looks. Deferring every first-seen copy broke those (0410 as first drafted).
//
// So only keys that are expected to repeat are deferred: a key of a shape (format, size, byte
// order: no address) that other keys of the same shape have shown to be a per-frame render target,
// that is, resolved in kRepeatRunFrames frames in a row, and of which not mostly one-offs.
// A key resolved in such runs before (its buffer evicted after 60 frames unused) repeats too.
// The first car of a shape in a session (a shape never seen repeating) and every one-off resolve
// keep the wait.
class RepeatTracker {
 public:
  // Frames in a row (a gap of one frame allowed) that make a key a repeating one.
  static constexpr uint32_t kRepeatRunFrames = 4;
  // Repeating keys of a shape needed before its new keys are deferred.
  static constexpr uint32_t kMinRepeaters = 2;
  // A shape whose keys ended without a run more than this many times for each repeating one is a
  // shape of pictures (kMixedShape). Cars out of view for a while end runs too, so not "any".
  static constexpr uint32_t kMaxOneOffsPerRepeater = 4;
  // A shape's repeaters must be this recent (about a minute at 60 fps), or it is unlearned.
  static constexpr uint64_t kShapeFreshFrames = 3600;
  // A key resolved at least kRepeatRunFrames times with no gap longer than this (a readback buffer is
  // evicted after 60 frames unused) is a recurring render target (a shadow cascade, a mirror), though
  // it is not resolved every frame.
  static constexpr uint64_t kLooseGapFrames = 60;
  // A key not resolved for this many frames has ended.
  static constexpr uint64_t kEndedFrames = 4;
  // Keys not resolved for this many frames are forgotten (about 5 minutes at 60 fps).
  static constexpr uint64_t kForgetKeyFrames = 18000;
  static constexpr size_t kMaxKeys = 8192;

  enum class KeyState : uint8_t { kUnknown, kRepeater, kOneOff };
  struct Key {
    uint64_t shape = 0;
    uint64_t last_frame = 0;
    uint64_t gap = 0;  // frames between the last two resolves of the key (0: first seen)
    uint32_t run = 0;  // frames in the current run (a gap of one frame allowed)
    uint32_t longest_run = 0;
    uint32_t loose_run = 0;  // resolves in the current run of gaps up to kLooseGapFrames
    uint32_t longest_loose_run = 0;
    uint32_t resolves = 0;
    uint32_t misses = 0;
    KeyState state = KeyState::kUnknown;
  };
  struct Shape {
    uint32_t repeaters = 0;  // keys that reached kRepeatRunFrames in a row
    uint32_t one_offs = 0;   // keys that ended before that (halved every kShapeFreshFrames)
    uint64_t last_repeat_frame = 0;
    uint64_t decayed_frame = 0;
  };
  enum class Why : uint8_t {
    kKeyRepeated,   // this key was resolved in a long run before
    kShapeRepeats,  // keys of this shape are per-frame render targets
    kNoRepeaters,   // no key of this shape has repeated (yet)
    kFewRepeaters,  // fewer than kMinRepeaters keys of this shape have repeated
    kMixedShape,    // far more one-offs of this shape than repeaters
    kStaleShape,    // its repeaters are old
  };
  static bool Defers(Why why) { return why == Why::kKeyRepeated || why == Why::kShapeRepeats; }
  static const char* WhyName(Why why) {
    switch (why) {
      case Why::kKeyRepeated: return "key repeated before";
      case Why::kShapeRepeats: return "shape repeats";
      case Why::kNoRepeaters: return "no key of this shape repeated";
      case Why::kFewRepeaters: return "few keys of this shape repeated";
      case Why::kMixedShape: return "shape has one-offs";
      case Why::kStaleShape: return "shape's repeaters are old";
    }
    return "";
  }

  // Every resolve of `key` (of a readback mode) at frame `frame`; `shape` is its address-less
  // description. The returned entry stays valid until the next Resolved/Sweep.
  Key& Resolved(uint64_t key, uint64_t shape, uint64_t frame) {
    auto [it, inserted] = keys_.try_emplace(key);
    Key& k = it->second;
    if (inserted) {
      k.shape = shape;
      k.last_frame = frame;
      k.run = 1;
      k.loose_run = 1;
    } else if (frame != k.last_frame) {
      k.gap = frame - k.last_frame;
      k.run = k.gap <= 2 ? k.run + 1 : 1;
      k.loose_run = k.gap <= kLooseGapFrames ? k.loose_run + 1 : 1;
      k.last_frame = frame;
      if (k.state == KeyState::kOneOff) {  // it came back
        k.state = KeyState::kUnknown;
        Shape& s = shapes_[k.shape];
        if (s.one_offs) --s.one_offs;
      }
    }
    ++k.resolves;
    if (k.run > k.longest_run) k.longest_run = k.run;
    if (k.loose_run > k.longest_loose_run) k.longest_loose_run = k.loose_run;
    if (k.run >= kRepeatRunFrames) {
      Shape& s = shapes_[k.shape];
      if (k.state != KeyState::kRepeater) {
        k.state = KeyState::kRepeater;
        ++s.repeaters;
      }
      s.last_repeat_frame = frame;
    }
    return k;
  }

  // Whether a miss of `k` (the entry Resolved returned) may be copied back without a wait.
  Why Judge(const Key& k, uint64_t frame) const {
    if (k.longest_loose_run >= kRepeatRunFrames) return Why::kKeyRepeated;
    auto it = shapes_.find(k.shape);
    if (it == shapes_.end() || !it->second.repeaters) return Why::kNoRepeaters;
    const Shape& s = it->second;
    if (frame > s.last_repeat_frame && frame - s.last_repeat_frame > kShapeFreshFrames) {
      return Why::kStaleShape;
    }
    if (s.repeaters < kMinRepeaters) return Why::kFewRepeaters;
    if (s.one_offs > kMaxOneOffsPerRepeater * s.repeaters) return Why::kMixedShape;
    return Why::kShapeRepeats;
  }

  const Shape* FindShape(uint64_t shape) const {
    auto it = shapes_.find(shape);
    return it == shapes_.end() ? nullptr : &it->second;
  }

  // Now and then (about once a second): the keys that ended without a run are one-offs of their
  // shape; forget old keys and shapes whose repeaters are long gone.
  void Sweep(uint64_t frame) {
    for (auto it = keys_.begin(); it != keys_.end();) {
      Key& k = it->second;
      if (k.state == KeyState::kUnknown && frame > k.last_frame &&
          frame - k.last_frame > kEndedFrames && k.longest_loose_run < kRepeatRunFrames) {
        k.state = KeyState::kOneOff;
        ++shapes_[k.shape].one_offs;
      }
      if (frame > k.last_frame && frame - k.last_frame > kForgetKeyFrames) {
        it = keys_.erase(it);
      } else {
        ++it;
      }
    }
    if (keys_.size() > kMaxKeys) keys_.clear();
    for (auto it = shapes_.begin(); it != shapes_.end();) {
      Shape& s = it->second;
      // The one-offs of a load or a menu fade out: a shape that repeats again afterwards (a car's
      // targets) is not held back by them for the rest of the session.
      if (!s.decayed_frame) {
        s.decayed_frame = frame;
      } else if (frame - s.decayed_frame >= kShapeFreshFrames) {
        s.one_offs /= 2;
        s.decayed_frame = frame;
      }
      if (frame > s.last_repeat_frame && frame - s.last_repeat_frame > 4 * kShapeFreshFrames) {
        it = shapes_.erase(it);
      } else {
        ++it;
      }
    }
  }

  size_t key_count() const { return keys_.size(); }
  size_t shape_count() const { return shapes_.size(); }

 private:
  std::unordered_map<uint64_t, Key> keys_;
  std::unordered_map<uint64_t, Shape> shapes_;
};

}  // namespace rex::graphics::vulkan::fh1_readback_history
