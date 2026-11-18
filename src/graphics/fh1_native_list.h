// FH1 (patch 0650): the native renderer's model lists on the command processor's side (FH1
// docs/native-renderer.md, "M4, third step"; the host packets: rex/graphics/fh1_host_packet.h).
//
// A track model carries one precompiled D3D command buffer per pass group, and the game
// converts each into a template (FH1 src/platform/native_list.h): what the list's packets do,
// with every word a fixup writes as a slot that is read from the live list. The game registers
// templates and the lists that use them with host packets in stream order (kOpListDefine,
// kOpListBind, kOpListDrop), and the command processor finds a list by the physical address of
// the indirect buffer it runs.
//
// This header is the pure part, for tests/fh1_native_list_test.cpp:
// - the template's layout and its readers, a copy of the game's (keep them the same: the
//   fixture in the test is a template the game's converter made, and the game's test checks
//   that its converter still makes it);
// - Validate: a template from guest memory is only used when every offset in it stays inside
//   it and inside its list;
// - the registry: templates by id, lists by physical address, the lookup's checks;
// - the shadow compare: the state a list's template says each draw starts with and the list
//   leaves, over the register file the list started with, against the register file the
//   emulated packets produced.
#pragma once

#include <algorithm>
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rex::graphics::fh1_list {

// ---- The template (FH1 src/platform/native_list.h) ----------------------------------------------

constexpr uint32_t kMagic = 0x4648314Cu;  // "FH1L"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kNone = 0xFFFFFFFFu;

constexpr uint32_t kHeaderBytes = 0xC8;
constexpr uint32_t kHeaderMasks = 0x18;
constexpr uint32_t kMaskWords = 20;
constexpr uint32_t kHeaderFlags = 0x6C;
constexpr uint32_t kFlagFixedUp = 0x2000;
// The game's loader sets flags 0x101 and clears 0x6 of the files' 0x86 / 0x886 (FH1 M4, second
// step): templates are converted and keyed with the flags in the files' form.
constexpr uint32_t kLoaderFlags = 0x101, kFileOnlyFlags = 0x6;
constexpr uint32_t FilesFormFlags(uint32_t flags) {
  return (flags & ~(kLoaderFlags | kFlagFixedUp)) | kFileOnlyFlags;
}

constexpr uint32_t kRegDmaBase = 0x21FA;
constexpr uint32_t kRegWindowOffset = 0x2080;
constexpr uint32_t kTiles = 3;
constexpr uint64_t kBinMaskAfterTiles = 0xFFFFFFFFull;
constexpr uint32_t kPm4Nop = 0x10;
constexpr uint32_t NopPacket(uint32_t header) {
  return 0xC0000000u | (header & 0x3FFF0000u) | (kPm4Nop << 8);
}

enum : uint32_t {
  kTMagic,
  kTVersion,
  kTWords,
  kTKey,
  kTListWords = kTKey + 4,
  kTCbFlags,
  kTFlags,
  kTOps,
  kTOpCount,
  kTDraws,
  kTDrawCount,
  kTDeltas,
  kTDeltaCount,
  kTSlots,
  kTSlotCount,
  kTPool,
  kTPoolWords,
  kTTileBound,
  kTTileBoundCount,
  kTMasks,
  kTHeaderWords,
};
constexpr uint32_t kTemplateTiled = 1;

constexpr uint32_t kOpWords = 4;
enum : uint32_t { kOpRegs = 1, kOpTile = 2, kOpShader = 3, kOpLiterals = 4, kOpDraw = 5 };
constexpr uint32_t kOpOneRegister = 0x100, kOpTileBound = 0x200, kOpPixel = 0x400;

enum : uint32_t {
  kDFlags,
  kDInitiator,
  kDIndexPool,
  kDSizeWord,
  kDPacket,
  kDFirstOp,
  kDOpCount,
  kDDelta,
  kDDeltaCount,
  kDVertexShader,
  kDVertexShaderSize,
  kDPixelShader,
  kDPixelShaderSize,
  kDVertexFetch,
  kDTexture,
  kDTile = kDTexture + 8,
  kDrawWords,
};
constexpr uint32_t kDrawPredicated = 1, kDrawTail = 2;
constexpr uint32_t kDeltaWords = 4, kSlotWords = 4;
constexpr uint32_t kSlotVertexShader = 9, kSlotPixelShader = 10;
constexpr uint32_t kTileWords = 12;          // a tile block's pool words: per tile mask, 3 values
constexpr uint32_t kLiteralWords = 16;       // a literal block
constexpr uint32_t kRegisterLimit = 0x5003;  // RegisterFile::kRegisterCount

struct Key {
  uint32_t w[4] = {};
  bool operator==(const Key& o) const { return std::memcmp(w, o.w, sizeof(w)) == 0; }
  bool operator!=(const Key& o) const { return !(*this == o); }
};

// MurmurHash3 x64's block mixing, four words a block (as the game's Hasher).
class Hasher {
 public:
  void Add(uint32_t v) {
    buffer_[fill_++] = v;
    ++count_;
    if (fill_ == 4)
      Block();
  }
  Key Finish() {
    if (fill_) {
      while (fill_ < 4)
        buffer_[fill_++] = 0;
      Block();
    }
    h1_ ^= count_;
    h2_ ^= count_;
    h1_ += h2_;
    h2_ += h1_;
    h1_ = Mix(h1_);
    h2_ = Mix(h2_);
    h1_ += h2_;
    h2_ += h1_;
    Key k;
    k.w[0] = uint32_t(h1_);
    k.w[1] = uint32_t(h1_ >> 32);
    k.w[2] = uint32_t(h2_);
    k.w[3] = uint32_t(h2_ >> 32);
    return k;
  }

 private:
  static uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
  static uint64_t Mix(uint64_t k) {
    k ^= k >> 33;
    k *= 0xFF51AFD7ED558CCDull;
    k ^= k >> 33;
    k *= 0xC4CEB9FE1A85EC53ull;
    k ^= k >> 33;
    return k;
  }
  void Block() {
    constexpr uint64_t c1 = 0x87C37B91114253D5ull, c2 = 0x4CF5AD432745937Full;
    uint64_t k1 = buffer_[0] | uint64_t(buffer_[1]) << 32;
    uint64_t k2 = buffer_[2] | uint64_t(buffer_[3]) << 32;
    k1 *= c1;
    k1 = Rotl(k1, 31);
    k1 *= c2;
    h1_ ^= k1;
    h1_ = Rotl(h1_, 27);
    h1_ += h2_;
    h1_ = h1_ * 5 + 0x52DCE729;
    k2 *= c2;
    k2 = Rotl(k2, 33);
    k2 *= c1;
    h2_ ^= k2;
    h2_ = Rotl(h2_, 31);
    h2_ += h1_;
    h2_ = h2_ * 5 + 0x38495AB5;
    fill_ = 0;
  }
  uint64_t h1_ = 0, h2_ = 0, count_ = 0;
  uint32_t buffer_[4] = {};
  uint32_t fill_ = 0;
};

// A guest (big endian) word.
inline uint32_t Word(const uint8_t* guest, size_t index) {
  uint32_t v;
  std::memcpy(&v, guest + index * 4, 4);
  return __builtin_bswap32(v);
}

constexpr uint32_t kDefineAckAlignment = 64;
static_assert(std::atomic_ref<uint32_t>::is_always_lock_free);
static_assert(kDefineAckAlignment % std::atomic_ref<uint32_t>::required_alignment == 0);

// The producer's ring holds fewer than 2^31 outstanding sequences, including at wrap.
inline bool IsNewDefineSequence(uint32_t sequence, uint32_t last_ack) {
  const uint32_t distance = sequence - last_ack;
  return distance != 0 && distance < (uint32_t{1} << 31);
}

// Once the decoder has finished reading a template, let the game reuse its storage.
inline void PublishDefineAck(uint32_t& word, uint32_t sequence) {
  std::atomic_ref<uint32_t>(word).store(__builtin_bswap32(sequence), std::memory_order_release);
}

inline Key TemplateKey(const uint32_t* t) {
  Key k;
  std::memcpy(k.w, t + kTKey, sizeof(k.w));
  return k;
}
inline const uint32_t* Op(const uint32_t* t, uint32_t op) {
  return t + t[kTOps] + op * kOpWords;
}
inline const uint32_t* DrawRecord(const uint32_t* t, uint32_t k) {
  return t + t[kTDraws] + k * kDrawWords;
}

// Every offset of template t (`words` words, for a list of t[kTListWords] dwords) inside the
// template and its list, so the readers below cannot read outside them.
inline bool Validate(const uint32_t* t, size_t words) {
  if (words < kTHeaderWords || t[kTMagic] != kMagic || t[kTVersion] != kVersion ||
      t[kTWords] != words || !t[kTListWords] || t[kTListWords] > (1u << 20)) {
    return false;
  }
  const auto section = [&](uint32_t at, uint64_t each, uint64_t extra = 0) {
    return t[at] >= kTHeaderWords && uint64_t(t[at]) + (t[at + 1] + extra) * each <= words;
  };
  if (!section(kTOps, kOpWords) || !section(kTDraws, kDrawWords, 1) ||
      !section(kTDeltas, kDeltaWords) || !section(kTSlots, kSlotWords) || !section(kTPool, 1) ||
      !section(kTTileBound, 2) || t[kTMasks] < kTHeaderWords ||
      uint64_t(t[kTMasks]) + kMaskWords > words) {
    return false;
  }
  const uint32_t ops = t[kTOpCount], draws = t[kTDrawCount], pool = t[kTPoolWords],
                 list = t[kTListWords];
  const auto in_pool = [&](uint64_t at, uint64_t n) { return at + n <= pool; };
  uint32_t draw_ops = 0;
  for (uint32_t i = 0; i < ops; ++i) {
    const uint32_t* o = Op(t, i);
    switch (o[0] & 0xFF) {
      case kOpRegs:
        if (!o[2] || uint64_t(o[1]) + ((o[0] & kOpOneRegister) ? 1 : o[2]) > kRegisterLimit ||
            !in_pool(o[3], o[2])) {
          return false;
        }
        break;
      case kOpTile:
        if (!in_pool(o[1], kTileWords))
          return false;
        break;
      case kOpShader:
        if (!in_pool(o[1], 1))
          return false;
        break;
      case kOpLiterals:
        if (o[2] != kLiteralWords || uint64_t(o[1]) + o[2] > kRegisterLimit || !in_pool(o[3], 1))
          return false;
        break;
      case kOpDraw:
        if (o[1] >= draws)
          return false;
        ++draw_ops;
        break;
      default:
        return false;
    }
  }
  if (draw_ops != draws)
    return false;
  for (uint32_t k = 0; k <= draws; ++k) {
    const uint32_t* r = DrawRecord(t, k);
    if (uint64_t(r[kDFirstOp]) + r[kDOpCount] > ops ||
        uint64_t(r[kDDelta]) + r[kDDeltaCount] > t[kTDeltaCount]) {
      return false;
    }
    if (k < draws && (!in_pool(r[kDIndexPool], 1) || r[kDPacket] + 5 > list))
      return false;
    for (uint32_t s : {kDVertexShader, kDPixelShader}) {
      if (r[s] != kNone && !in_pool(r[s], 1))
        return false;
    }
    if (r[kDVertexFetch] != kNone && !in_pool(r[kDVertexFetch], 6))
      return false;
    for (uint32_t i = 0; i < 8; ++i) {
      if (r[kDTexture + i] != kNone && !in_pool(r[kDTexture + i], 6))
        return false;
    }
    if (r[kDTile] != kNone && (r[kDTile] >= ops || (Op(t, r[kDTile])[0] & 0xFF) != kOpTile))
      return false;
  }
  const uint32_t* deltas = t + t[kTDeltas];
  for (uint32_t i = 0; i < t[kTDeltaCount]; ++i) {
    const uint32_t* e = deltas + i * kDeltaWords;
    if (e[2] >= ops || !e[1] || uint64_t(e[0]) + e[1] > kRegisterLimit)
      return false;
    const uint32_t* o = Op(t, e[2]);
    const uint32_t kind = o[0] & 0xFF;
    if (kind == kOpRegs) {
      if (!in_pool(uint64_t(o[3]) + e[3], e[1]))
        return false;
    } else if (kind == kOpLiterals) {
      if (uint64_t(e[3]) + e[1] > kLiteralWords)
        return false;
    } else {
      return false;
    }
  }
  const uint32_t* slot = t + t[kTSlots];
  uint32_t last = 0;
  for (uint32_t i = 0; i < t[kTSlotCount]; ++i, slot += kSlotWords) {
    if (slot[1] < last || uint64_t(slot[1]) + slot[3] > list || !in_pool(slot[2], slot[3]))
      return false;
    last = slot[1] + slot[3];
  }
  const uint32_t* bound = t + t[kTTileBound];
  for (uint32_t i = 0; i < t[kTTileBoundCount]; ++i) {
    if (bound[2 * i] >= list || (i && bound[2 * i] <= bound[2 * i - 2]))
      return false;
  }
  return true;
}

// The key of a list as it is now (the game's LiveKey): the header's flags (in the files' form,
// without kFlagFixedUp) and ten masks, the patch table, area C (not in memory: none), and the
// PM4 with every slot as zero (a microcode address's low two bits kept) and every tile-bound
// header as recorded, whether the live word is the header or its NOP.
inline Key LiveKey(const uint32_t* t, const uint8_t* header, uint32_t flags,
                   const uint8_t* patch_table, uint32_t patch_words, const uint8_t* pm4,
                   uint32_t pm4_words) {
  Hasher h;
  h.Add(kVersion);
  h.Add(flags & ~kFlagFixedUp);
  for (uint32_t i = 0; i < kMaskWords; ++i)
    h.Add(Word(header, kHeaderMasks / 4 + i));
  h.Add(patch_words);
  for (uint32_t i = 0; i < patch_words; ++i)
    h.Add(Word(patch_table, i));
  h.Add(0);  // area C
  h.Add(pm4_words);
  const uint32_t* slot = t + t[kTSlots];
  const uint32_t* slots_end = slot + t[kTSlotCount] * kSlotWords;
  const uint32_t* bound = t + t[kTTileBound];
  const uint32_t* bound_end = bound + t[kTTileBoundCount] * 2;
  for (uint32_t d = 0; d < pm4_words;) {
    if (slot != slots_end && slot[1] == d) {
      const uint32_t type = slot[0] & 0xFF, argument = slot[0] >> 8;
      const bool microcode =
          (type == kSlotVertexShader || type == kSlotPixelShader) && argument == 0;
      for (uint32_t i = 0; i < slot[3] && d < pm4_words; ++i, ++d) {
        h.Add(microcode ? Word(pm4, d) & 3 : 0);
      }
      slot += kSlotWords;
      continue;
    }
    uint32_t w = Word(pm4, d);
    if (bound != bound_end && bound[0] == d) {
      if (w == NopPacket(bound[1]))
        w = bound[1];
      bound += 2;
    }
    h.Add(w);
    ++d;
  }
  return h.Finish();
}

// Word 4 of a list recorded for the tiles is a NOP exactly while scene_msaa has it untiled.
inline bool Untiled(const uint32_t* t, const uint8_t* pm4) {
  if (!(t[kTFlags] & kTemplateTiled) || t[kTListWords] <= 4)
    return false;
  const uint32_t w = Word(pm4, 4);
  return w >> 30 == 3 && ((w >> 8) & 0x7F) == kPm4Nop;
}

// The pool with every slot read from the live list.
inline void Resolve(const uint32_t* t, const uint8_t* pm4, uint32_t* pool) {
  if (t[kTPoolWords])
    std::memcpy(pool, t + t[kTPool], t[kTPoolWords] * sizeof(uint32_t));
  const uint32_t* slot = t + t[kTSlots];
  for (uint32_t s = 0; s < t[kTSlotCount]; ++s, slot += kSlotWords) {
    for (uint32_t i = 0; i < slot[3]; ++i)
      pool[slot[2] + i] = Word(pm4, slot[1] + i);
  }
}

// ---- The registry -------------------------------------------------------------------------------

// Why a run at a registered address is not compared (the buffer is decoded as today, which in
// shadow mode it is anyway).
enum class Skip : uint32_t {
  kNone,
  kRefused,        // the game's converter refused the list (its reason in the binding)
  kNoTemplate,     // bound to a template id the plugin never got (or could not read)
  kLength,         // the indirect buffer's length is not the registered one: the address reused
  kRebound,        // the command buffer no longer names this buffer: the object reused
  kStaticChanged,  // a static word of the list changed: its key, the template's at an earlier
                   // run since the bind, is not the template's now
  kNested,         // a list run inside a list being compared
  kKeyAtBind,      // the key is not the template's at the first key check after the bind: the
                   // template's slot table or the converter is wrong (not the game), never 0
                   // in a correct build
  kFixupInProgress,  // the slot snapshot was rejected by the generation check
  kCount,
};
constexpr const char* SkipName(Skip s) {
  constexpr const char* names[] = {"none",    "refused",     "no-template",
                                   "length",  "rebound",     "static-words-changed",
                                   "nested",  "key-at-bind", "fixup-in-progress"};
  static_assert(sizeof(names) / sizeof(names[0]) == size_t(Skip::kCount));
  return s < Skip::kCount ? names[size_t(s)] : "?";
}

struct Template {
  std::vector<uint32_t> words;
};

struct Binding {
  uint32_t phys = 0, words = 0, template_id = kNone, refusal = 0;
  uint32_t state_word = 0, cb = 0, patch = 0, patch_words = 0, group = 0xFF, flags = 0;
  // Decoder-local incarnation. A recycled guest address may have the same
  // header, template and generation as an earlier allocation.
  uint64_t incarnation = 0;
  // The pool resolved at generation pool_generation (even), when pool_valid.
  std::vector<uint32_t> pool;
  bool pool_valid = false;
  uint32_t pool_generation = 0;
  // A key check since the bind found the template's key (KeyCheck).
  bool key_matched = false;
};

// The lookup's key check of a run of binding b: kNone when the live key is the template's;
// otherwise kStaticChanged when an earlier run since the bind had the template's key (the game
// wrote a static word after it), and kKeyAtBind when none had (the template does not describe
// the list it was bound for: a slot in the wrong place, a word the converter got wrong).
inline Skip KeyCheck(Binding* b, bool differs) {
  if (!differs) {
    b->key_matched = true;
    return Skip::kNone;
  }
  return b->key_matched ? Skip::kStaticChanged : Skip::kKeyAtBind;
}

enum class DefineResult { kNew, kRepeat, kInvalid };
enum class BindResult { kNew, kSame, kReplaced };

class Registry {
 public:
  // A template the game sent (host order); a repeat of an id (a tile replay of the packet) is
  // ignored.
  DefineResult Define(uint32_t id, const uint32_t* words, size_t count) {
    if (templates_.count(id))
      return DefineResult::kRepeat;
    if (!Validate(words, count))
      return DefineResult::kInvalid;
    templates_[id].words.assign(words, words + count);
    return DefineResult::kNew;
  }
  const Template* FindTemplate(uint32_t id) const {
    auto it = templates_.find(id);
    return it == templates_.end() ? nullptr : &it->second;
  }
  BindResult Bind(const Binding& b) {
    ever_bound_.insert(b.phys);
    auto [it, fresh] = bindings_.try_emplace(b.phys, b);
    if (fresh) {
      it->second.incarnation = NextIncarnation();
      return BindResult::kNew;
    }
    Binding& e = it->second;
    if (e.words == b.words && e.template_id == b.template_id && e.refusal == b.refusal &&
        e.state_word == b.state_word && e.cb == b.cb && e.patch == b.patch &&
        e.patch_words == b.patch_words && e.group == b.group && e.flags == b.flags) {
      return BindResult::kSame;
    }
    e = b;
    e.incarnation = NextIncarnation();
    return BindResult::kReplaced;
  }
  bool Drop(uint32_t phys) { return bindings_.erase(phys) != 0; }
  // Whether a list was ever bound at phys (a lookup miss there is not a buffer that was never a
  // list: a list dropped before its run was decoded, or the memory reused for another buffer).
  bool WasBound(uint32_t phys) const { return ever_bound_.count(phys) != 0; }
  // The list registered at physical address phys for an indirect buffer of `count` dwords, or
  // null (and `*skip` kLength when the address is registered with another length).
  Binding* Lookup(uint32_t phys, uint32_t count, Skip* skip) {
    *skip = Skip::kNone;
    auto it = bindings_.find(phys);
    if (it == bindings_.end())
      return nullptr;
    if (it->second.words != count) {
      *skip = Skip::kLength;
      return nullptr;
    }
    return &it->second;
  }
  size_t templates() const { return templates_.size(); }
  size_t bindings() const { return bindings_.size(); }

 private:
  uint64_t NextIncarnation() {
    // Leave zero reserved for bindings without replay authority. Shadow
    // comparison continues even after the counter is exhausted.
    return next_incarnation_ ? next_incarnation_++ : 0;
  }
  uint64_t next_incarnation_ = 1;
  std::unordered_map<uint32_t, Template> templates_;
  std::unordered_map<uint32_t, Binding> bindings_;
  std::unordered_set<uint32_t> ever_bound_;
};

// ---- The shadow compare -------------------------------------------------------------------------

// The register file ranges the compare tells apart.
enum Field : uint32_t {
  kFieldOrder,      // a draw packet that is not the template's next draw, or draws missing
  kFieldExecuted,   // a draw ran where the template says it is predicated off, or the reverse
  kFieldBinMask,    // the bin mask
  kFieldShader,     // the vertex or pixel shader loaded (IM_LOAD's two words)
  kFieldState,      // registers 0x2000-0x23FF
  kFieldConstants,  // ALU constants 0x4000-0x47FF
  kFieldFetch,      // fetch constants 0x4800-0x48BF
  kFieldBoolLoop,   // bool and loop constants 0x4900-0x4927
  kFieldOther,      // (end of a list) every other register
  kFieldWrites,     // the registers written: one the packets write and the template does not,
                    // or the reverse, before a draw or after the last (whatever the values)
  kFieldCount,
};
constexpr const char* kFieldNames[kFieldCount] = {
    "draw order",      "executed",         "bin mask",        "shader",
    "state registers", "ALU constants",    "fetch constants", "bool/loop constants",
    "other registers", "registers written"};
inline Field FieldOf(uint32_t reg) {
  if (reg >= 0x2000 && reg < 0x2400)
    return kFieldState;
  if (reg >= 0x4000 && reg < 0x4800)
    return kFieldConstants;
  if (reg >= 0x4800 && reg < 0x48C0)
    return kFieldFetch;
  if (reg >= 0x4900 && reg < 0x4928)
    return kFieldBoolLoop;
  return kFieldOther;
}
// D3D's marks (SCRATCH_REG6/7, SHADER_CONSTANT_FLUSH_FETCH_0-2): no packet, CPU read or memory
// write-back reads them (FH1 M4, second step), so the end-of-list compare leaves them out.
constexpr bool MarkRegister(uint32_t reg) {
  return reg == 0x57E || reg == 0x57F || (reg >= 0x5000 && reg <= 0x5002);
}
// Registers a packet writes as part of what it does (EVENT_WRITE*, VIZ_QUERY: the event
// initiator; DRAW_INDX: the DMA base and size and the draw initiator), after the draw hook: the
// write sets leave them out; their values are compared (the next draw's view, the list's end).
constexpr bool PacketRegister(uint32_t reg) {
  return reg >= 0x21F9 && reg <= 0x21FC;
}

// A set of registers, cleared in the time its size takes.
class RegisterSet {
 public:
  void Add(uint32_t first, uint32_t count) {
    const uint32_t end = uint32_t(std::min<uint64_t>(uint64_t(first) + count, kRegisterLimit));
    for (uint32_t reg = first; reg < end; ++reg) {
      uint64_t& word = bits_[reg >> 6];
      const uint64_t bit = uint64_t(1) << (reg & 63);
      if (!(word & bit) && !MarkRegister(reg) && !PacketRegister(reg)) {
        word |= bit;
        list_.push_back(uint16_t(reg));
      }
    }
  }
  bool Has(uint32_t reg) const {
    return reg < kRegisterLimit && (bits_[reg >> 6] >> (reg & 63)) & 1;
  }
  size_t size() const { return list_.size(); }
  const std::vector<uint16_t>& registers() const { return list_; }
  void Clear() {
    for (uint16_t reg : list_)
      bits_[reg >> 6] = 0;
    list_.clear();
  }

 private:
  uint64_t bits_[(kRegisterLimit + 63) / 64] = {};
  std::vector<uint16_t> list_;
};

// What a draw's shaders and fixed function read, compared at every draw.
constexpr uint32_t kDrawRanges[][2] = {{0x2000, 0x2400}, {0x4000, 0x4928}};

// IM_LOAD's two words as the command processor last decoded them, per shader type (0 vertex,
// 1 pixel); `valid` false before any.
struct Shaders {
  uint32_t address[2] = {}, size[2] = {};
  bool valid[2] = {};
};

struct Outcome {
  bool agree = true;
  uint32_t fields = 0;  // bits by Field
  // The first register that differs: its expected and its live value.
  uint32_t reg = kNone, expected = 0, live = 0;
  void Note(Field f, uint32_t r = kNone, uint32_t e = 0, uint32_t l = 0) {
    if (reg == kNone && r != kNone) {
      reg = r;
      expected = e;
      live = l;
    }
    agree = false;
    fields |= 1u << f;
  }
};

// One run of a template's list, beside the emulated packets. `memory` translates a physical
// address to guest memory (the literal blocks a list loads). Begin before the indirect buffer's
// first packet, Draw at each draw packet (before it runs, and also for one its predicate skips),
// End after its last.
class ShadowRun {
 public:
  using PhysicalMemory = const uint8_t* (*)(void* context, uint32_t address);

  void Begin(const uint32_t* t, const uint32_t* pool, uint64_t bin_select, uint64_t bin_mask,
             bool untiled, const uint32_t* regs, const Shaders& shaders, PhysicalMemory memory,
             void* memory_context) {
    t_ = t;
    pool_ = pool;
    select_ = bin_select;
    mask_ = bin_mask;
    untiled_ = untiled;
    memory_ = memory;
    memory_context_ = memory_context;
    shaders_ = shaders;
    expected_.assign(regs, regs + kRegisterLimit);
    next_ = 0;
    order_broken_ = false;
    template_writes_.Clear();
    live_writes_.Clear();
  }
  // Registers the command processor wrote for a packet of the list (any register write it
  // makes while the run is open; the ones PacketRegister names and the marks are left out).
  void LiveWrites(uint32_t first, uint32_t count) { live_writes_.Add(first, count); }
  uint32_t draw_count() const { return t_[kTDrawCount]; }
  uint32_t next_draw() const { return next_; }

  // A draw packet at list dword `packet`, which the command processor runs (`executed`) or
  // skips by its predicate.
  Outcome Draw(uint32_t packet, bool executed, const uint32_t* regs, uint64_t bin_mask,
               const Shaders& shaders) {
    Outcome o;
    if (order_broken_ || next_ >= t_[kTDrawCount] || DrawRecord(t_, next_)[kDPacket] != packet) {
      order_broken_ = true;
      o.Note(kFieldOrder);
      return o;
    }
    const uint32_t* r = DrawRecord(t_, next_);
    const uint64_t mask = Fold(r);
    const bool predicated = (r[kDFlags] & kDrawPredicated) != 0;
    const bool expect_run = !predicated || (select_ & mask) != 0;
    if (expect_run != executed)
      o.Note(kFieldExecuted);
    if (mask != bin_mask)
      o.Note(kFieldBinMask);
    CompareShaders(r, shaders, &o);
    for (const auto& range : kDrawRanges)
      Compare(regs, range[0], range[1], &o);
    CompareWrites(&o);
    if (expect_run) {
      expected_[kRegDmaBase] = pool_[r[kDIndexPool]];
      expected_[kRegDmaBase + 1] = r[kDSizeWord];
      expected_[kRegDmaBase + 2] = r[kDInitiator];
    }
    ++next_;
    return o;
  }

  // After the list's last packet: the whole register file but the marks.
  Outcome End(const uint32_t* regs, uint64_t bin_mask, const Shaders& shaders) {
    Outcome o;
    if (order_broken_ || next_ != t_[kTDrawCount]) {
      o.Note(kFieldOrder);
      return o;
    }
    const uint32_t* r = DrawRecord(t_, next_);
    const uint64_t mask = Fold(r);
    if (mask != bin_mask)
      o.Note(kFieldBinMask);
    CompareShaders(r, shaders, &o);
    CompareWrites(&o);
    // The marks (MarkRegister) left out.
    Compare(regs, 0, 0x57E, &o);
    Compare(regs, 0x580, 0x5000, &o);
    static_assert(kRegisterLimit == 0x5003);
    return o;
  }

 private:
  // The record's delta over the expected register file, then the tile block in effect: the
  // view of StateAtDraw (FH1 native_list.h), one draw at a time. Returns the bin mask. Every
  // register it writes goes into the template's write set, so a write the template leaves out,
  // puts on another register or applies for the wrong tile is seen even when its value is
  // already in the register file (the window a tile block writes, which D3D's segment set for
  // the selected tile before the list).
  uint64_t Fold(const uint32_t* r) {
    const uint32_t* deltas = t_ + t_[kTDeltas];
    const uint32_t* e = deltas + r[kDDelta] * kDeltaWords;
    for (uint32_t i = 0; i < r[kDDeltaCount]; ++i, e += kDeltaWords) {
      const uint32_t* op = Op(t_, e[2]);
      if ((op[0] & 0xFF) == kOpLiterals) {
        const uint8_t* block = memory_(memory_context_, pool_[op[3]] & 0x3FFFFFFFu);
        for (uint32_t k = 0; k < e[1]; ++k) {
          expected_[e[0] + k] = block ? Word(block, e[3] + k) : 0xDEADBEEFu;
        }
        template_writes_.Add(e[0], e[1]);
      } else if (!(untiled_ && (op[0] & kOpTileBound))) {
        std::memcpy(&expected_[e[0]], pool_ + op[3] + e[3], e[1] * sizeof(uint32_t));
        template_writes_.Add(e[0], e[1]);
      }
    }
    if (r[kDTile] == kNone)
      return mask_;
    if (!untiled_) {
      const uint32_t* tile = pool_ + Op(t_, r[kDTile])[1];
      // The block in effect is applied at every draw after it; its packets are among this
      // draw's ops only once.
      const bool here = r[kDTile] >= r[kDFirstOp] && r[kDTile] - r[kDFirstOp] < r[kDOpCount];
      for (uint32_t k = 0; k < kTiles; ++k) {
        if (select_ & tile[k * 4]) {
          std::memcpy(&expected_[kRegWindowOffset], tile + k * 4 + 1, 3 * sizeof(uint32_t));
          if (here)
            template_writes_.Add(kRegWindowOffset, 3);
        }
      }
    }
    return kBinMaskAfterTiles;
  }
  void CompareShaders(const uint32_t* r, const Shaders& live, Outcome* o) const {
    for (uint32_t stage = 0; stage < 2; ++stage) {
      const uint32_t at = r[stage ? kDPixelShader : kDVertexShader];
      uint32_t address = shaders_.address[stage], size = shaders_.size[stage];
      bool valid = shaders_.valid[stage];
      if (at != kNone) {
        address = pool_[at];
        size = r[stage ? kDPixelShaderSize : kDVertexShaderSize];
        valid = true;
      }
      if (valid != live.valid[stage] ||
          (valid && (address != live.address[stage] || size != live.size[stage]))) {
        o->Note(kFieldShader);
      }
    }
  }
  // The registers the packets wrote since the last draw (or the run's start) against the ones
  // the template wrote; both sets start again. A register in one set only is noted with
  // expected / live 1 for the side that wrote it.
  void CompareWrites(Outcome* o) {
    if (template_writes_.size() != live_writes_.size() ||
        !std::all_of(live_writes_.registers().begin(), live_writes_.registers().end(),
                     [&](uint16_t reg) { return template_writes_.Has(reg); })) {
      uint32_t first = kNone;
      bool by_template = false;
      for (uint16_t reg : live_writes_.registers())
        if (!template_writes_.Has(reg) && reg < first)
          first = reg;
      for (uint16_t reg : template_writes_.registers())
        if (!live_writes_.Has(reg) && reg < first) {
          first = reg;
          by_template = true;
        }
      o->Note(kFieldWrites, first, by_template ? 1 : 0, by_template ? 0 : 1);
    }
    template_writes_.Clear();
    live_writes_.Clear();
  }
  void Compare(const uint32_t* regs, uint32_t from, uint32_t to, Outcome* o) const {
    if (from >= to)
      return;
    if (std::memcmp(&expected_[from], regs + from, (to - from) * sizeof(uint32_t)) == 0)
      return;
    for (uint32_t reg = from; reg < to; ++reg) {
      if (expected_[reg] != regs[reg])
        o->Note(FieldOf(reg), reg, expected_[reg], regs[reg]);
    }
  }

  const uint32_t* t_ = nullptr;
  const uint32_t* pool_ = nullptr;
  uint64_t select_ = 0, mask_ = 0;
  bool untiled_ = false, order_broken_ = false;
  PhysicalMemory memory_ = nullptr;
  void* memory_context_ = nullptr;
  Shaders shaders_;
  std::vector<uint32_t> expected_;
  uint32_t next_ = 0;
  RegisterSet template_writes_, live_writes_;
};

}  // namespace rex::graphics::fh1_list
