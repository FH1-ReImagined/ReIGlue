// FH1 (#378): building the pipelines of queued draws before the command processor reaches them.
//
// When a draw needs a pipeline that does not exist yet, the command processor walks the commands
// the game has already queued behind it (the rest of the indirect buffers being executed and the
// ring up to the write pointer, and the indirect buffers those call) on a copy of the context
// registers, works out each draw's pipeline the way the draw itself will, and hands every one not
// built yet to the pipeline creation threads together, instead of meeting them one draw at a time.
// The idea is ReGPU's (github.com/zer0ducksgiven/ReGPU, MIT; renderer.cpp GpuLookahead).
//
// This header holds the parts without SDK types, for tools/test_pipeline_lookahead.cpp: the PM4
// walk over guest command words (Walker) and the ring position arithmetic. The register copy, the
// draws' pipelines and the creation are in fh1_pipeline_lookahead.cpp and pipeline_cache.cpp.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace rex::graphics::vulkan::fh1_pipeline_lookahead {

// Why a walk stopped.
enum class End : uint32_t {
  kQueueEnd,   // everything queued was walked
  kPackets,    // the packet limit
  kPipelines,  // the limit of pipelines started by one walk
  kDraws,      // the limit of draws looked at
  kTime,       // the time limit
  kWait,       // a WAIT_REG_MEM on guest memory not satisfied yet: what follows may not be ready
  kPartition,  // FH1's split rendering switches to the other render partition (its own caches)
  kBadPacket,  // a packet longer than its buffer
  kCount,
};

inline const char* EndName(End end) {
  switch (end) {
    case End::kQueueEnd: return "queue end";
    case End::kPackets: return "packets";
    case End::kPipelines: return "pipelines";
    case End::kDraws: return "draws";
    case End::kTime: return "time";
    case End::kWait: return "wait";
    case End::kPartition: return "partition";
    case End::kBadPacket: return "bad packet";
    default: return "?";
  }
}

// What the log's pipeline summary counts (since the start; one command processor per process).
struct Stats {
  // Any thread (the creation threads too).
  std::atomic<uint64_t> created{0};        // pipelines built (placeholders not counted)
  std::atomic<uint64_t> created_ahead{0};  // of those, started by a lookahead
  std::atomic<uint64_t> failed_ahead{0};   // started by a lookahead, creation failed
  std::atomic<uint64_t> placeholders{0};   // placeholders built on the command processor thread
  // The command processor thread.
  uint64_t walks = 0, walk_packets = 0, walk_draws = 0, queued_ahead = 0, drawn_ahead = 0;
  uint64_t walk_unmet_waits = 0;
  uint64_t ends[uint32_t(End::kCount)] = {};
  double walk_ms = 0, wait_ms = 0;
  uint64_t waits = 0;
  uint64_t frames = 0, skipped_frames = 0, skipped_draws = 0;
  // #167: frames shown with draws skipped (after the hold, or with the skip setting off); frames
  // held past the hold for a draw that must not be missing; CPU readbacks with draws skipped.
  uint64_t incomplete_frames_shown = 0, held_for_stale_contents = 0, incomplete_readbacks = 0;
  // #167: skipped draws that hold their frame, by kind (Fh1SkipMustHoldFrame).
  uint64_t skipped_stereo = 0, skipped_depth = 0, skipped_screen_passes = 0,
           skipped_front_buffer = 0;
};
inline Stats stats;

// Advanced when a pipeline cache deletes its shaders: a walk's saved state is stale then.
inline std::atomic<uint64_t> shader_epoch{0};

// The log's pipeline summary, at most every 10 s while pipelines are being built (the swaps).
void LogSummary();

// Ring dwords from index `from` to index `to` (exclusive), wrapping at `size` dwords.
inline uint32_t RingDistance(uint32_t from, uint32_t to, uint32_t size) {
  return size ? (to + size - from) % size : 0;
}

// The registers a walk keeps: the context registers (render state, VGT, SQ, RB, PA).
// Constants (0x4000 and up) do not change pipelines; the ones below are the command processor's.
constexpr uint32_t kFirstRegister = 0x2000;
constexpr uint32_t kRegisterCount = 0x2000;
constexpr uint32_t kRegisterDrawInitiator = 0x21FC;  // VGT_DRAW_INITIATOR
constexpr uint32_t kRegisterDmaBase = 0x21FA;        // VGT_DMA_BASE
constexpr uint32_t kRegisterDmaSize = 0x21FB;        // VGT_DMA_SIZE
constexpr uint32_t kMaxDepth = 4;                    // indirect buffers in indirect buffers

// PM4 type-3 opcodes the walk follows (xenos.h).
namespace pm4 {
constexpr uint32_t kDrawIndx = 0x22, kDrawIndx2 = 0x36, kImLoad = 0x27, kImLoadImmediate = 0x2B,
                   kSetConstant = 0x2D, kSetConstant2 = 0x55, kSetShaderConstants = 0x56,
                   kLoadAluConstant = 0x2F, kIndirectBuffer = 0x3F, kIndirectBufferPfd = 0x37,
                   kWaitRegMem = 0x3C, kXeSwap = 0x64, kSetBinMaskLo = 0x60,
                   kSetBinMaskHi = 0x61, kSetBinSelectLo = 0x62, kSetBinSelectHi = 0x63,
                   kSetBinMask = 0x50, kSetBinSelect = 0x51;
}  // namespace pm4

inline uint32_t Swap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

// Walks PM4 packets as the command processor executes them, without executing anything. Hooks:
//   void Register(uint32_t index, uint32_t value)  a context register write (index in
//                                                  [kFirstRegister, +kRegisterCount))
//   const uint32_t* Translate(uint32_t physical, uint32_t bytes)  guest memory (big-endian
//                                                  words) or nullptr when out of range
//   void LoadShader(uint32_t type, const uint32_t* words, uint32_t dwords)  IM_LOAD(_IMMEDIATE):
//                                                  guest-endian words, contiguous
//   End Draw(uint32_t initiator, bool dma, uint32_t dma_base, uint32_t dma_size)
//                                                  a draw (kQueueEnd to go on, else why to stop)
//   End Swap()                                     the end of a frame
//   bool Out()                                     the time limit was reached (asked every
//                                                  kOutInterval words, padding included)
constexpr uint32_t kOutInterval = 64;

template <typename Hooks>
class Walker {
 public:
  Walker(Hooks& hooks, uint32_t max_packets, uint64_t bin_mask, uint64_t bin_select)
      : hooks_(hooks), max_packets_(max_packets), bin_mask_(bin_mask), bin_select_(bin_select) {}

  // Walks `count` dwords of `base` from dword `start`, wrapping at `wrap` dwords (0: no wrap).
  // At depth 0, stopped() is then the dword (from `start`) where the walk stopped: the packet
  // that stopped it, or `count`. Every packet counts towards the packet limit, padding words
  // included, and the time limit is checked every kOutInterval of them whatever their type.
  End Walk(const uint32_t* base, uint32_t start, uint32_t count, uint32_t wrap,
           uint32_t depth = 0) {
    Buffer b{base, start, count, wrap};
    uint32_t i = 0;
    End end = End::kQueueEnd;
    while (i < count) {
      if (++packets_ > max_packets_) {
        end = End::kPackets;
        break;
      }
      if (!(packets_ % kOutInterval) && hooks_.Out()) {
        end = End::kTime;
        break;
      }
      uint32_t header = b.Word(i);
      if (header == 0 || (header >> 30) == 2) {
        ++i;  // padding (zero words and type-2 packets)
        continue;
      }
      uint32_t type = header >> 30;
      if (type == 1) {
        // Two registers with 11-bit indices: none is a context register.
        if (count - i < 3) {
          end = End::kBadPacket;
          break;
        }
        i += 3;
        continue;
      }
      uint32_t n = ((header >> 16) & 0x3FFF) + 1;
      if (n > count - i - 1) {
        end = End::kBadPacket;
        break;
      }
      if (type == 0) {
        uint32_t index = header & 0x7FFF;
        bool one = (header >> 15) & 1;
        for (uint32_t k = 0; k < n; ++k) Set(one ? index : index + k, b.Word(i + 1 + k));
        i += 1 + n;
        continue;
      }
      end = Packet3(b, header, i + 1, n, depth);
      if (end != End::kQueueEnd) break;
      i += 1 + n;
    }
    if (!depth) stopped_ = end == End::kQueueEnd ? count : i;
    return end;
  }

  // Whether a wait on guest memory not satisfied yet ends the walk (else it is only counted).
  void set_stop_at_unmet_waits(bool stop) { stop_at_unmet_waits_ = stop; }
  uint32_t unmet_waits() const { return unmet_waits_; }
  uint32_t stopped() const { return stopped_; }
  uint64_t bin_mask() const { return bin_mask_; }
  uint64_t bin_select() const { return bin_select_; }
  uint32_t packets() const { return packets_; }

 private:
  struct Buffer {
    const uint32_t* base;
    uint32_t start, count, wrap;
    uint32_t Word(uint32_t i) const {
      uint32_t at = start + i;
      if (wrap) at %= wrap;
      return Swap32(base[at]);
    }
  };

  void Set(uint32_t index, uint32_t value) {
    if (index >= kFirstRegister && index < kFirstRegister + kRegisterCount) {
      hooks_.Register(index, value);
    }
  }

  End Packet3(const Buffer& b, uint32_t header, uint32_t d, uint32_t n, uint32_t depth) {
    uint32_t opcode = (header >> 8) & 0x7F;
    // Predicated packets run only for the selected bins; predicated swaps never.
    if ((header & 1) && (!(bin_select_ & bin_mask_) || opcode == pm4::kXeSwap)) {
      return End::kQueueEnd;
    }
    auto arg = [&](uint32_t k) { return b.Word(d + k); };
    switch (opcode) {
      case pm4::kIndirectBuffer:
      case pm4::kIndirectBufferPfd: {
        if (n < 2 || depth >= kMaxDepth) break;
        uint32_t address = arg(0) & 0x1FFFFFFF;
        uint32_t dwords = arg(1) & 0xFFFFF;
        const uint32_t* words = dwords ? hooks_.Translate(address, dwords * 4) : nullptr;
        if (!words) break;
        End end = Walk(words, 0, dwords, 0, depth + 1);
        // A packet cut by its indirect buffer's end ends only that buffer, as on the GPU.
        if (end != End::kQueueEnd && end != End::kBadPacket) return end;
      } break;
      case pm4::kSetConstant: {
        if (n < 1) break;
        uint32_t offset_type = arg(0);
        // Type 4 is registers; 0-3 are ALU, fetch, bool and loop constants.
        if (((offset_type >> 16) & 0xFF) == 4) {
          uint32_t index = kFirstRegister + (offset_type & 0x7FF);
          for (uint32_t k = 1; k < n; ++k) Set(index + k - 1, arg(k));
        }
      } break;
      case pm4::kSetConstant2:
      case pm4::kSetShaderConstants: {
        if (n < 1) break;
        uint32_t index = arg(0) & 0xFFFF;
        for (uint32_t k = 1; k < n; ++k) Set(index + k - 1, arg(k));
      } break;
      case pm4::kLoadAluConstant: {
        if (n < 3) break;
        uint32_t address = arg(0) & 0x3FFFFFFF;
        uint32_t offset_type = arg(1);
        uint32_t size = arg(2) & 0xFFF;
        if (((offset_type >> 16) & 0xFF) != 4 || !size) break;
        const uint32_t* words = hooks_.Translate(address & 0x1FFFFFFF, size * 4);
        if (!words) break;
        uint32_t index = kFirstRegister + (offset_type & 0x7FF);
        for (uint32_t k = 0; k < size; ++k) Set(index + k, Swap32(words[k]));
      } break;
      case pm4::kImLoad: {
        if (n < 2) break;
        uint32_t address_type = arg(0);
        uint32_t dwords = arg(1) & 0xFFFF;
        const uint32_t* words =
            dwords ? hooks_.Translate((address_type & ~3u) & 0x1FFFFFFF, dwords * 4) : nullptr;
        if (words) hooks_.LoadShader(address_type & 3, words, dwords);
      } break;
      case pm4::kImLoadImmediate: {
        if (n < 2) break;
        uint32_t dwords = arg(1) & 0xFFFF;
        if (!dwords || dwords > n - 2) break;
        // In the ring the microcode may wrap: copy it out (as it is in memory, guest endian).
        immediate_.resize(dwords);
        for (uint32_t k = 0; k < dwords; ++k) immediate_[k] = Swap32(arg(2 + k));
        hooks_.LoadShader(arg(0) & 3, immediate_.data(), dwords);
      } break;
      case pm4::kDrawIndx:
      case pm4::kDrawIndx2: {
        // DRAW_INDX has the viz query token first.
        uint32_t k = opcode == pm4::kDrawIndx ? 1 : 0;
        if (n < k + 1) break;
        uint32_t initiator = arg(k);
        // Source select (bits 6-7): 0 is DMA (an index buffer: base and size follow).
        bool dma = ((initiator >> 6) & 3) == 0;
        uint32_t dma_base = 0, dma_size = 0;
        if (dma) {
          if (n < k + 3) break;
          dma_base = arg(k + 1);
          dma_size = arg(k + 2);
        }
        End end = hooks_.Draw(initiator, dma, dma_base, dma_size);
        if (end != End::kQueueEnd) return end;
      } break;
      case pm4::kWaitRegMem: {
        if (n < 5) break;
        uint32_t info = arg(0);
        // A wait on guest memory that is not satisfied yet: the game may still be writing what
        // follows (the indirect buffers, the shaders). Register waits are the GPU's own.
        if (!(info & 0x10)) break;
        uint32_t address = arg(1);
        const uint32_t* word = hooks_.Translate((address & ~3u) & 0x1FFFFFFF, 4);
        if (!word) break;
        // As the command processor reads it: the word as loaded, then the GPU endian swap.
        uint32_t value = *word;
        switch (address & 3) {
          case 1: value = ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu); break;
          case 2: value = Swap32(value); break;
          case 3: value = (value << 16) | (value >> 16); break;
          default: break;
        }
        if (!WaitMatched(info & 7, value & arg(3), arg(2))) {
          ++unmet_waits_;
          if (stop_at_unmet_waits_) return End::kWait;
        }
      } break;
      case pm4::kXeSwap: {
        End end = hooks_.Swap();
        if (end != End::kQueueEnd) return end;
      } break;
      case pm4::kSetBinMaskLo:
        if (n >= 1) bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | arg(0);
        break;
      case pm4::kSetBinMaskHi:
        if (n >= 1) bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(arg(0)) << 32);
        break;
      case pm4::kSetBinSelectLo:
        if (n >= 1) bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | arg(0);
        break;
      case pm4::kSetBinSelectHi:
        if (n >= 1) bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(arg(0)) << 32);
        break;
      case pm4::kSetBinMask:
        if (n >= 2) bin_mask_ = (uint64_t(arg(0)) << 32) | arg(1);
        break;
      case pm4::kSetBinSelect:
        if (n >= 2) bin_select_ = (uint64_t(arg(0)) << 32) | arg(1);
        break;
      default:
        break;
    }
    return End::kQueueEnd;
  }

  static bool WaitMatched(uint32_t function, uint32_t value, uint32_t ref) {
    switch (function) {
      case 1: return value < ref;
      case 2: return value <= ref;
      case 3: return value == ref;
      case 4: return value != ref;
      case 5: return value >= ref;
      case 6: return value > ref;
      case 7: return true;
      default: return false;
    }
  }

  Hooks& hooks_;
  uint32_t max_packets_;
  uint32_t packets_ = 0;
  uint32_t stopped_ = 0;
  uint32_t unmet_waits_ = 0;
  bool stop_at_unmet_waits_ = true;
  uint64_t bin_mask_, bin_select_;
  std::vector<uint32_t> immediate_;
};

}  // namespace rex::graphics::vulkan::fh1_pipeline_lookahead
