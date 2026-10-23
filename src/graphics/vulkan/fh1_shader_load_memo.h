// Which shader the command processor last loaded from a guest address (FH1 SDK
// patch 0140). The game loads a vertex and a pixel shader for nearly every draw
// (PM4_IM_LOAD: about 150,000 a second in a race), almost always the same few
// hundred, each time hashing the microcode and looking the hash up. A load whose
// address, length and words match the last load from that address gets that
// load's shader: the same one the hash would find, since the shader map only
// ever adds entries (until the cache shuts down: a new generation).
//
// On a miss the guest words are copied into the slot first, and the hash, the
// shader's own microcode and the memo's key are all taken from that one copy: a
// guest write while the shader is looked up or made cannot pair the new words
// with the old shader.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rex::graphics::vulkan::fh1_shader_load_memo {

template <typename ShaderT, size_t kSlotCount = 256>
class ShaderLoadMemo {
  static_assert(kSlotCount && !(kSlotCount & (kSlotCount - 1)), "power of two");

 public:
  // The shader stored for these words at this address, or nullptr.
  ShaderT* Find(const void* owner, uint64_t generation, const uint32_t* words,
                uint32_t dword_count) const {
    const Slot& slot = slots_[Index(words)];
    if (slot.shader && slot.owner == owner && slot.generation == generation &&
        slot.address == words && slot.words.size() == dword_count &&
        (!dword_count || !std::memcmp(slot.words.data(), words, dword_count * sizeof(uint32_t)))) {
      return slot.shader;
    }
    return nullptr;
  }

  // The stored shader for the words at `words`, else load(snapshot), where snapshot
  // is this memo's copy of the words (stable while load runs), and remembers that.
  template <typename Load>
  ShaderT* FindOrLoad(const void* owner, uint64_t generation, const uint32_t* words,
                      uint32_t dword_count, Load&& load) {
    if (ShaderT* shader = Find(owner, generation, words, dword_count)) {
      return shader;
    }
    Slot& slot = slots_[Index(words)];
    slot.shader = nullptr;
    slot.words.assign(words, words + dword_count);
    const uint32_t* snapshot = dword_count ? slot.words.data() : words;
    ShaderT* shader = load(snapshot);
    slot.owner = owner;
    slot.generation = generation;
    slot.address = words;
    slot.shader = shader;
    return shader;
  }

 private:
  struct Slot {
    const void* owner = nullptr;
    uint64_t generation = 0;
    const uint32_t* address = nullptr;
    std::vector<uint32_t> words;
    ShaderT* shader = nullptr;
  };
  static size_t Index(const uint32_t* address) {
    uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(address) >> 2) * UINT64_C(0x9E3779B97F4A7C15);
    return size_t(key >> 40) & (kSlotCount - 1);
  }
  std::array<Slot, kSlotCount> slots_;
};

}  // namespace rex::graphics::vulkan::fh1_shader_load_memo
