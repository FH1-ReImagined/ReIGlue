// The descriptor sets written in the current frame, by their contents (FH1 SDK patch 0444,
// #192). The command processor wrote a new texture descriptor set (and a new constants set)
// for every draw, about 4,000 a frame, though most draws of a frame bind the same few hundred
// combinations of image views and samplers: 20-23 % of its CPU time was descriptor setup.
//
// A set is looked up by the exact words it was written with (the image view and sampler
// handles in binding order, after a word for its layout), so a hit holds what a new write
// would hold. Entries live for one frame, while the transient sets they name are owned by
// that frame (they return to the free lists only once the frame's fence has signalled); the
// owner clears the memo at each frame, when the descriptor pools are reset, and when any
// handle a set may hold is destroyed (a new handle may reuse the value).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rex::graphics::vulkan::fh1_descriptor_memo {

inline uint64_t HashWords(const uint64_t* words, size_t count) {
  uint64_t hash = 0x9E3779B97F4A7C15ull ^ (uint64_t(count) * 0xC2B2AE3D27D4EB4Full);
  for (size_t i = 0; i < count; ++i) {
    hash ^= words[i] * 0x9E3779B97F4A7C15ull;
    hash = (hash << 31) | (hash >> 33);
    hash *= 0xBF58476D1CE4E5B9ull;
  }
  hash ^= hash >> 29;
  return hash ? hash : 1;
}

// Open addressing with linear probing; kMaxEntries keeps the load at most a half. Value is a
// handle (zero is "none"). Past kMaxEntries a frame's further sets are written as before.
template <typename Value, size_t kSlotCount = 4096, size_t kMaxEntries = kSlotCount / 2>
class DescriptorMemo {
  static_assert(kSlotCount && !(kSlotCount & (kSlotCount - 1)), "power of two");
  static_assert(kMaxEntries < kSlotCount, "an empty slot ends every probe");

 public:
  DescriptorMemo() : slots_(kSlotCount) {}

  void Clear() {
    if (!entries_) return;
    // Slots of older epochs are empty: no pass over the table.
    if (++epoch_ == 0) {
      for (Slot& slot : slots_) slot.epoch = 0;
      epoch_ = 1;
    }
    words_.clear();
    entries_ = 0;
  }

  Value Find(const uint64_t* words, uint32_t count, uint64_t hash) const {
    for (size_t i = size_t(hash) & (kSlotCount - 1);; i = (i + 1) & (kSlotCount - 1)) {
      const Slot& slot = slots_[i];
      if (slot.epoch != epoch_) return Value{};
      if (slot.hash == hash && slot.count == count &&
          !std::memcmp(words_.data() + slot.offset, words, sizeof(uint64_t) * count)) {
        return slot.value;
      }
    }
  }

  // After a Find of the same words missed.
  void Insert(const uint64_t* words, uint32_t count, uint64_t hash, Value value) {
    if (entries_ >= kMaxEntries) return;
    size_t i = size_t(hash) & (kSlotCount - 1);
    while (slots_[i].epoch == epoch_) i = (i + 1) & (kSlotCount - 1);
    Slot& slot = slots_[i];
    slot.epoch = epoch_;
    slot.hash = hash;
    slot.offset = uint32_t(words_.size());
    slot.count = count;
    slot.value = value;
    words_.insert(words_.end(), words, words + count);
    ++entries_;
  }

  size_t size() const { return entries_; }

 private:
  struct Slot {
    uint64_t epoch = 0;
    uint64_t hash = 0;
    uint32_t offset = 0;
    uint32_t count = 0;
    Value value{};
  };
  std::vector<Slot> slots_;
  std::vector<uint64_t> words_;
  uint64_t epoch_ = 1;
  size_t entries_ = 0;
};

}  // namespace rex::graphics::vulkan::fh1_descriptor_memo
