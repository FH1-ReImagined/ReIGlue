// FH1 (patch 0610): the native renderer's shadow mode, on the command processor's side (FH1
// docs/native-renderer.md, "M2: shadow mode"; the packet: rex/graphics/fh1_host_packet.h).
//
// The game's host device layer (FH1 src/hooks/native_device.cpp) writes a draw record behind
// each D3D flush call of one group of passes: what a native renderer would draw from, read from
// the D3D device's shadow state. Here, in stream order, each record is compared with the
// register file as the guest's own packets left it:
// - the 161 registers the device shadows;
// - the fetch constants, those the bound shaders read and the others;
// - the ALU constants as the records gave them (all in the group's first record, then the
//   chunks the device marked dirty): those the shaders read and the others;
// - the draw: one draw since the previous record for a draw call (none for a resolve, which
//   is a copy), its primitive type, count and index address;
// - the shader identity: a guest shader object is always the same microcode.
// The group's draws and copies are counted against the flush calls the game made in it. A
// summary is logged every 300 groups and at the end.
#include <rex/graphics/command_processor.h>

#include <rex/graphics/fh1_host_packet.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

namespace rex::graphics {

namespace fh = fh1_host;

namespace {

constexpr const char* kKindNames[fh::kKindCount] = {
    "none",    "DrawVertices", "DrawIndexed",      "BeginVertices", "DrawVerticesUP",
    "Resolve", "Clear",        "RunCommandBuffer", "DrawIndexedUP"};

bool IsDrawCall(uint32_t kind) {
  return (kind >= fh::kKindDrawVertices && kind <= fh::kKindDrawVerticesUp) ||
         kind == fh::kKindDrawIndexedUp;
}
bool IsUpDraw(uint32_t kind) {
  return kind == fh::kKindBeginVertices || kind == fh::kKindDrawVerticesUp ||
         kind == fh::kKindDrawIndexedUp;
}

// How a native renderer would use a shadowed register for a record of this kind.
enum Use : uint32_t {
  kUseRead,     // it draws from it: it must agree
  kUseNotRead,  // nothing the host renderer does reads it here
  kUseCallOwn,  // the call sets its own (a resolve's or clear's rectangle draw)
};
Use RegisterUse(uint32_t reg, uint32_t kind, const RegisterFile& regs, const Shader* ps) {
  const bool resolve = kind == fh::kKindResolve;
  if (kind == fh::kKindClear)
    return kUseCallOwn;
  if (resolve) {
    // The copy: its source surface and its registers.
    if (reg == XE_GPU_REG_RB_SURFACE_INFO || reg == XE_GPU_REG_RB_COLOR_INFO ||
        reg == XE_GPU_REG_RB_DEPTH_INFO ||
        (reg >= XE_GPU_REG_RB_COPY_CONTROL && reg <= XE_GPU_REG_RB_COPY_SURFACE_SLICE &&
         reg != XE_GPU_REG_RB_HIZ_CLEAR))
      return kUseRead;
    return kUseCallOwn;
  }
  if (kind == fh::kKindRunCommandBuffer)
    return kUseRead;
  // Render targets 1-3 and their blending only when the pixel shader writes them; target 0's
  // when it writes it (a depth-only draw has none).
  auto writes = [&](uint32_t n) {
    return ps && ps->is_ucode_analyzed() && ps->writes_color_target(n);
  };
  if (reg >= XE_GPU_REG_RB_COLOR1_INFO && reg <= XE_GPU_REG_RB_COLOR3_INFO)
    return writes(reg - XE_GPU_REG_RB_COLOR1_INFO + 1) ? kUseRead : kUseNotRead;
  if (reg >= XE_GPU_REG_RB_BLENDCONTROL1 && reg <= XE_GPU_REG_RB_BLENDCONTROL3)
    return writes(reg - XE_GPU_REG_RB_BLENDCONTROL1 + 1) ? kUseRead : kUseNotRead;
  if (reg == XE_GPU_REG_RB_COLOR_INFO || reg == XE_GPU_REG_RB_BLENDCONTROL0)
    return writes(0) ? kUseRead : kUseNotRead;
  if (reg == XE_GPU_REG_RB_ALPHA_REF)
    return regs.Get<reg::RB_COLORCONTROL>().alpha_test_enable ? kUseRead : kUseNotRead;
  // Coherency, hierarchical Z, the bin ID, debug and unknown registers, sample counting, the
  // copy and clear registers (a draw's), the tessellation group and reuse registers.
  if ((reg >= XE_GPU_REG_COHER_DEST_BASE_0 && reg <= XE_GPU_REG_COHER_DEST_BASE_7) ||
      reg == XE_GPU_REG_RB_HIZCONTROL || reg == XE_GPU_REG_VGT_CURRENT_BIN_ID_MIN ||
      (reg >= XE_GPU_REG_SQ_DEBUG_MISC_0 && reg <= 0x2311) || reg == 0x2313 || reg == 0x2314 ||
      reg == 0x2290 || reg == 0x2291 ||
      (reg >= XE_GPU_REG_RB_COPY_CONTROL && reg <= XE_GPU_REG_RB_SAMPLE_COUNT_ADDR) ||
      (reg >= XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL && reg <= XE_GPU_REG_VGT_GROUP_VECT_1_FMT_CNTL) ||
      reg == XE_GPU_REG_VGT_ENHANCE || reg == XE_GPU_REG_SQ_CF_PROGRAM_SIZE ||
      reg == XE_GPU_REG_VGT_VERTEX_REUSE_BLOCK_CNTL || reg == XE_GPU_REG_VGT_OUT_DEALLOC_CNTL)
    return kUseNotRead;
  return kUseRead;
}

// What a record disagrees in.
enum Field : uint32_t {
  kFieldRegister,
  kFieldRegisterUnused,
  kFieldFetchUsed,
  kFieldFetchUnused,
  kFieldAluUsed,
  kFieldAluUnused,
  kFieldDraw,
  kFieldShader,
  kFieldCount,
};
constexpr const char* kFieldNames[kFieldCount] = {"registers read",
                                                  "registers not read",
                                                  "fetch constants read",
                                                  "fetch constants not read",
                                                  "ALU constants read",
                                                  "ALU constants not read",
                                                  "draw",
                                                  "shader identity"};

struct Sample {
  uint64_t records = 0;
  uint32_t record_value = 0, cp_value = 0, kind = 0, island = 0;
  void Add(uint32_t r, uint32_t c, uint32_t k, uint32_t i) {
    if (!records++) {
      record_value = r;
      cp_value = c;
      kind = k;
      island = i;
    }
  }
};

}  // namespace

struct CommandProcessor::Fh1NativeShadow {
  // The ALU constants a native renderer would have, from the records.
  uint32_t alu[fh::kAluWords] = {};
  bool alu_valid = false;
  // The chunks a seed has sent so far, and the constants packets since the last record.
  uint64_t seed_seen[2] = {};
  bool seeding = false;
  uint32_t constant_packets = 0;
  uint64_t constants_lost = 0, literal_loads = 0;
  bool in_island = false;
  uint32_t island = 0, next_index = 0, island_records = 0;
  bool island_compare = false;
  // Draws and copies executed since the start, and at the island's start and the last record.
  uint64_t draws = 0, copies = 0, island_draws = 0, island_copies = 0, record_draws = 0,
           record_copies = 0;
  uint32_t last_initiator = 0, last_index_base = 0;
  bool last_indexed = false;

  uint64_t records = 0, agree = 0, agree_all = 0, seeds = 0, outside = 0, replays = 0,
           alu_unknown = 0, unanalyzed = 0, malformed = 0;
  uint64_t kind_records[fh::kKindCount] = {}, kind_agree[fh::kKindCount] = {};
  uint64_t field_records[kFieldCount] = {};
  uint64_t kind_field[fh::kKindCount][kFieldCount] = {};
  uint64_t kind_own[fh::kKindCount] = {};               // records whose call sets its own state
  std::map<uint32_t, Sample> registers;                 // by register, read
  std::map<uint32_t, Sample> registers_unread;          // by register, not read
  std::map<uint32_t, Sample> fetch_used, fetch_unused;  // by fetch constant register
  std::map<uint32_t, Sample> alu_used;                  // by constant (0-511)
  uint64_t alu_unused_mismatches = 0;
  // (kind, draws executed, copies executed since the previous record) -> records
  std::map<uint64_t, uint64_t> executed;
  uint64_t primitive_mismatch = 0, count_mismatch = 0, index_mismatch = 0;
  std::unordered_map<uint32_t, uint64_t> shader_hashes;
  uint64_t shader_conflicts = 0;
  // Islands: begun, ended, counts agreeing; (game draws and clears, game resolves, executed
  // draws, executed copies) -> islands.
  uint64_t islands = 0, islands_ended = 0, islands_agree = 0, islands_unwritten = 0,
           islands_missing = 0;
  std::map<uint64_t, uint64_t> island_counts;
  // 0630: records of calls the game did not make (drawn natively by the backend), in all and in
  // the current island (its draws and resolves, which the command processor never executes).
  uint64_t native_records = 0, native_islands = 0;
  uint32_t island_native_draws = 0, island_native_resolves = 0;
  uint64_t check_ns = 0, checks = 0;
  uint32_t detailed = 0;
};

void CommandProcessor::Fh1HostPacket(const uint32_t* words, uint32_t count) {
  if (count >= 5 && words[1] == fh::kOpRef) {
    // The packets of a message in guest memory, in order.
    const uint32_t address = words[2], total = words[3], sequence = words[4];
    static std::atomic<uint64_t> stale{0};
    if (total < 2 || total > fh::kRefMaxWords)
      return;
    const auto* m = memory_->TranslateVirtual<const uint32_t*>(address);
    if (!m || rex::byte_swap(m[0]) != sequence || rex::byte_swap(m[1]) != total) {
      if (stale++ < 5)
        REXGPU_WARN("FH1 host packet: message {:08X} was overwritten", address);
      return;
    }
    std::vector<uint32_t> packet;
    for (uint32_t at = 2; at < total;) {
      const uint32_t n = rex::byte_swap(m[at++]);
      if (n < 2 || n > fh::kMaxWords || at + n > total)
        break;
      packet.resize(n);
      for (uint32_t i = 0; i < n; ++i)
        packet[i] = rex::byte_swap(m[at + i]);
      at += n;
      if (packet[0] == fh::kMagic && packet[1] != fh::kOpRef)
        Fh1HostPacket(packet.data(), n);
    }
    return;
  }
  if (count >= 2 && ((words[1] >= fh::kOpIslandBegin && words[1] <= fh::kOpConstants) ||
                     words[1] == fh::kOpLiterals)) {
    Fh1NativeShadowPacket(words, count);
    return;
  }
  static std::atomic<int> warned{0};
  if (warned++ < 5)
    REXGPU_WARN("FH1 host packet: unknown op {} ({} words)", count >= 2 ? words[1] : 0, count);
}

void CommandProcessor::Fh1NativeShadowDraw(uint32_t initiator, bool indexed, uint32_t index_base) {
  auto& s = *fh1_native_shadow_;
  if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    ++s.copies;
  } else {
    ++s.draws;
  }
  s.last_initiator = initiator;
  s.last_indexed = indexed;
  s.last_index_base = index_base;
}

namespace {

void LogShadowSummary(const CommandProcessor::Fh1NativeShadow& s, const char* when);

}  // namespace

void CommandProcessor::Fh1NativeShadowPacket(const uint32_t* words, uint32_t count) {
  if (!fh1_native_shadow_) {
    fh1_native_shadow_ = new Fh1NativeShadow;
    REXGPU_INFO(
        "FH1 native shadow mode: the game's draw records are checked against the "
        "register file");
  }
  auto& s = *fh1_native_shadow_;
  const auto start = std::chrono::steady_clock::now();
  const RegisterFile& regs = *register_file_;
  const uint32_t op = words[1];

  if (op == fh::kOpIslandBegin) {
    if (s.in_island)
      ++s.islands_missing;  // the previous one never ended
    s.in_island = true;
    s.island = count > 2 ? words[2] : 0;
    s.island_compare = count > 4 && (words[4] & fh::kIslandCompareContents);
    s.next_index = 0;
    s.island_records = 0;
    s.island_draws = s.record_draws = s.draws;
    s.island_copies = s.record_copies = s.copies;
    s.alu_valid = false;
    s.seeding = false;
    s.constant_packets = 0;
    s.island_native_draws = s.island_native_resolves = 0;
    ++s.islands;
    return;
  }

  if (op == fh::kOpIslandEnd) {
    if (count < 5 + fh::kKindCount || !s.in_island || words[2] != s.island) {
      ++s.malformed;
      s.in_island = false;
      return;
    }
    s.in_island = false;
    ++s.islands_ended;
    const uint32_t* calls = words + 5;
    uint64_t game_draws = uint64_t(calls[fh::kKindDrawVertices]) +
                                calls[fh::kKindDrawIndexed] + calls[fh::kKindBeginVertices] +
                                calls[fh::kKindDrawVerticesUp] + calls[fh::kKindDrawIndexedUp] +
                                calls[fh::kKindClear];
    // The calls the game skipped (0630) were not executed: the backend drew them.
    const uint64_t game_resolves = calls[fh::kKindResolve] - s.island_native_resolves;
    game_draws -= s.island_native_draws;
    if (s.island_native_draws + s.island_native_resolves)
      ++s.native_islands;
    const uint64_t draws = s.draws - s.island_draws, copies = s.copies - s.island_copies;
    if (words[4])
      ++s.islands_unwritten;
    const bool counts_agree = draws == game_draws && copies == game_resolves &&
                              !calls[fh::kKindRunCommandBuffer] && s.island_records == words[3];
    if (counts_agree)
      ++s.islands_agree;
    auto clamp = [](uint64_t v) { return std::min<uint64_t>(v, 0xFFFF); };
    ++s.island_counts[(clamp(game_draws) << 48) | (clamp(game_resolves) << 32) |
                      (clamp(draws) << 16) | clamp(copies)];
    if (s.islands_ended == 1 || s.islands_ended % 300 == 0)
      LogShadowSummary(s, "so far");
    return;
  }

  if (op == fh::kOpConstants) {
    // ALU constants for the next record, as a native renderer would upload them.
    const uint32_t n = count > 4 ? words[4] : 0;
    if (count < fh::kConstantsHeaderWords + n * (1 + fh::kChunkWords)) {
      ++s.malformed;
      return;
    }
    if (words[3] & fh::kConstantsSeed) {
      s.seeding = true;
      s.seed_seen[0] = s.seed_seen[1] = 0;
    }
    const uint32_t* c = words + fh::kConstantsHeaderWords;
    for (uint32_t k = 0; k < n; ++k, c += 1 + fh::kChunkWords) {
      const uint32_t chunk = c[0] & 127;
      std::memcpy(s.alu + chunk * fh::kChunkWords, c + 1, fh::kChunkWords * 4);
      s.seed_seen[chunk >> 6] |= uint64_t(1) << (chunk & 63);
    }
    ++s.constant_packets;
    return;
  }

  if (op == fh::kOpLiterals) {
    const uint32_t first = count > 4 ? words[3] : 0, n = count > 4 ? words[4] : 0;
    if (count < fh::kLiteralsHeaderWords + n || first + n > fh::kAluWords) {
      ++s.malformed;
      return;
    }
    std::memcpy(s.alu + first, words + fh::kLiteralsHeaderWords, n * 4);
    ++s.literal_loads;
    ++s.constant_packets;
    return;
  }

  // A record.
  constexpr uint32_t kFixed = fh::kRecordHeaderWords + fh::kRegisterShadowWords + fh::kFetchWords;
  if (count < kFixed || words[fh::kRecKind] >= fh::kKindCount) {
    ++s.malformed;
    return;
  }
  ++s.records;
  ++s.island_records;
  const uint32_t kind = words[fh::kRecKind];
  const uint32_t island = words[fh::kRecIsland];
  const uint32_t index = words[fh::kRecIndex];
  const uint32_t flags = words[fh::kRecFlags];
  ++s.kind_records[kind];
  if (!s.in_island || island != s.island)
    ++s.outside;
  if (index < s.next_index)
    ++s.replays;  // the same words executed again (a tile replay)
  s.next_index = index + 1;

  // The record's constants: all its packets arrived, and a seed sent every chunk.
  if (s.constant_packets != words[fh::kRecConstantPackets]) {
    ++s.constants_lost;
    s.alu_valid = false;
  }
  s.constant_packets = 0;
  if (flags & fh::kRecordSeed) {
    s.alu_valid = s.seeding && s.seed_seen[0] == ~uint64_t(0) && s.seed_seen[1] == ~uint64_t(0);
    s.seeding = false;
    if (s.alu_valid)
      ++s.seeds;
  }

  // 0630: a call the game did not make: nothing executed it, so there is no state to compare;
  // the backend draws it from the record.
  if (flags & fh::kRecordNative) {
    ++s.native_records;
    --s.records;  // "records" are the ones checked
    --s.kind_records[kind];
    if (kind == fh::kKindResolve)
      ++s.island_native_resolves;
    else
      ++s.island_native_draws;
    s.record_draws = s.draws;
    s.record_copies = s.copies;
    return;
  }

  const bool draw_call = IsDrawCall(kind);
  const bool own_shaders = kind == fh::kKindResolve || kind == fh::kKindClear;
  if (own_shaders)
    ++s.kind_own[kind];
  bool bad[kFieldCount] = {};
  std::string detail;
  const bool log_detail = s.detailed < 24;
  auto note = [&](Field f, const std::string& what) {
    bad[f] = true;
    if (log_detail && detail.size() < 700)
      detail += " " + what + ";";
  };
  Shader* vs = active_vertex_shader_;
  Shader* ps = active_pixel_shader_;

  // Registers.
  const uint32_t* shadow = words + fh::kRecordHeaderWords;
  for (const fh::RegisterBlock& b : fh::kRegisterBlocks) {
    for (uint32_t i = 0; i < b.count; ++i) {
      const uint32_t reg = b.first_register + i;
      const uint32_t r = shadow[b.shadow_word + i], c = regs[reg];
      if (r == c)
        continue;
      const Use use = RegisterUse(reg, kind, regs, ps);
      if (use == kUseCallOwn)
        continue;
      const RegisterInfo* info = RegisterFile::GetRegisterInfo(reg);
      if (use == kUseRead) {
        s.registers[reg].Add(r, c, kind, island);
        note(kFieldRegister, fmt::format("{} {:08X} vs {:08X}", info ? info->name : "?", r, c));
      } else {
        s.registers_unread[reg].Add(r, c, kind, island);
        bad[kFieldRegisterUnused] = true;
      }
    }
  }

  // What the shaders read.
  uint32_t fetch_read = 0;             // texture fetch constants (whole 6 words)
  uint64_t vertex_fetch_read[2] = {};  // vertex fetch constants 0-95 (2 words each)
  bool analyzed = true;
  if (!own_shaders) {
    for (Shader* sh : {vs, ps}) {
      if (!sh)
        continue;
      if (!sh->is_ucode_analyzed()) {
        analyzed = false;
        continue;
      }
      for (const auto& t : sh->texture_bindings())
        fetch_read |= 1u << (t.fetch_constant & 31);
      for (const auto& v : sh->vertex_bindings())
        vertex_fetch_read[(v.fetch_constant >> 6) & 1] |= uint64_t(1) << (v.fetch_constant & 63);
    }
    if (!analyzed)
      ++s.unanalyzed;
  }

  // Fetch constants (an UP draw's vertices are its own: fetch constant 31, in the command buffer).
  const uint32_t* fetch = shadow + fh::kRegisterShadowWords;
  for (uint32_t w = 0; w < fh::kFetchWords && !own_shaders; ++w) {
    const uint32_t reg = fh::kFetchFirstRegister + w;
    const uint32_t r = fetch[w], c = regs[reg];
    if (r == c)
      continue;
    const uint32_t slot = w / 6, vf = w / 2;
    if (slot == 31 && IsUpDraw(kind))
      continue;
    const bool read = (fetch_read >> slot & 1) || (vertex_fetch_read[vf >> 6] >> (vf & 63) & 1);
    (read ? s.fetch_used : s.fetch_unused)[reg].Add(r, c, kind, island);
    if (read) {
      note(kFieldFetchUsed, fmt::format("fetch {}.{} {:08X} vs {:08X}", slot, w % 6, r, c));
    } else {
      bad[kFieldFetchUnused] = true;
    }
  }

  // ALU constants, as the records sent them since the group's seed.
  if (!s.alu_valid) {
    ++s.alu_unknown;
  } else if (!own_shaders) {
    const uint32_t vs_base = regs.Get<reg::SQ_VS_CONST>().base;
    const uint32_t ps_base = regs.Get<reg::SQ_PS_CONST>().base;
    for (uint32_t c = 0; c < 512; ++c) {
      if (std::memcmp(s.alu + 4 * c, &regs.values[fh::kAluFirstRegister + 4 * c], 16) == 0)
        continue;
      bool read = false;
      for (uint32_t stage = 0; stage < 2; ++stage) {
        Shader* sh = stage ? ps : vs;
        const uint32_t base_c = stage ? ps_base : vs_base;
        if (!sh || !sh->is_ucode_analyzed() || c < base_c || c >= base_c + 256)
          continue;
        const auto& map = sh->constant_register_map();
        const uint32_t i = c - base_c;
        if (map.float_dynamic_addressing || (map.float_bitmap[i >> 6] >> (i & 63) & 1))
          read = true;
      }
      if (read) {
        uint32_t w = 0;
        while (w < 3 && s.alu[4 * c + w] == regs.values[fh::kAluFirstRegister + 4 * c + w])
          ++w;
        s.alu_used[c].Add(s.alu[4 * c + w], regs.values[fh::kAluFirstRegister + 4 * c + w], kind,
                          island);
        note(kFieldAluUsed, fmt::format("c{}.{} {:08X} vs {:08X}", c, "xyzw"[w], s.alu[4 * c + w],
                                        regs.values[fh::kAluFirstRegister + 4 * c + w]));
      } else {
        ++s.alu_unused_mismatches;
        bad[kFieldAluUnused] = true;
      }
    }
  }

  // The draw.
  const uint64_t draws = s.draws - s.record_draws, copies = s.copies - s.record_copies;
  s.record_draws = s.draws;
  s.record_copies = s.copies;
  ++s.executed[(uint64_t(kind) << 32) | (std::min<uint64_t>(draws, 0xFFFF) << 16) |
               std::min<uint64_t>(copies, 0xFFFF)];
  if (draw_call) {
    reg::VGT_DRAW_INITIATOR init;
    init.value = s.last_initiator;
    if (draws != 1 || copies) {
      note(kFieldDraw, fmt::format("{} draws {} copies executed", draws, copies));
    } else {
      if (uint32_t(init.prim_type) != words[fh::kRecPrimitive]) {
        ++s.primitive_mismatch;
        note(kFieldDraw,
             fmt::format("primitive {} vs {}", words[fh::kRecPrimitive], uint32_t(init.prim_type)));
      }
      if (uint32_t(init.num_indices) != words[fh::kRecCount]) {
        ++s.count_mismatch;
        note(kFieldDraw,
             fmt::format("count {} vs {}", words[fh::kRecCount], uint32_t(init.num_indices)));
      }
      if (kind == fh::kKindDrawIndexed &&
          (!s.last_indexed || (s.last_index_base & 0x1FFFFFFFu) != words[fh::kRecIndexAddress])) {
        ++s.index_mismatch;
        note(kFieldDraw, fmt::format("indices at {:08X} vs {:08X}", words[fh::kRecIndexAddress],
                                     s.last_index_base));
      }
    }
  } else if (kind == fh::kKindResolve && copies != 1) {
    note(kFieldDraw, fmt::format("resolve: {} draws {} copies executed", draws, copies));
  } else if (kind == fh::kKindClear && draws + copies != 1) {
    note(kFieldDraw, fmt::format("clear: {} draws {} copies executed", draws, copies));
  }

  // Shader identity: a guest object is always the same microcode.
  for (uint32_t stage = 0; stage < 2 && draw_call; ++stage) {
    Shader* sh = stage ? ps : vs;
    const uint32_t object = words[stage ? fh::kRecPixelShader : fh::kRecVertexShader];
    if (!object || !sh)
      continue;
    auto [it, added] = s.shader_hashes.try_emplace(object | stage, sh->ucode_data_hash());
    if (!added && it->second != sh->ucode_data_hash()) {
      ++s.shader_conflicts;
      note(kFieldShader,
           fmt::format("{} object {:08X} was {:016X}, now {:016X}", stage ? "pixel" : "vertex",
                       object, it->second, sh->ucode_data_hash()));
      it->second = sh->ucode_data_hash();
    }
  }

  bool all = true, used = true;
  for (uint32_t f = 0; f < kFieldCount; ++f) {
    if (!bad[f])
      continue;
    ++s.field_records[f];
    ++s.kind_field[kind][f];
    all = false;
    if (f != kFieldFetchUnused && f != kFieldAluUnused && f != kFieldRegisterUnused)
      used = false;
  }
  if (used) {
    ++s.agree;
    ++s.kind_agree[kind];
  }
  if (all)
    ++s.agree_all;
  if (!used && log_detail) {
    ++s.detailed;
    REXGPU_INFO("FH1 native shadow: record {} of group {} ({}, vs {:016X} ps {:016X}) disagrees:{}",
                index, island, kKindNames[kind], vs ? vs->ucode_data_hash() : 0,
                ps ? ps->ucode_data_hash() : 0, detail);
  }
  ++s.checks;
  s.check_ns += uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
          .count());
}

namespace {

std::string TopSamples(const std::map<uint32_t, Sample>& m, bool is_register, size_t limit) {
  std::vector<std::pair<uint32_t, Sample>> v(m.begin(), m.end());
  std::sort(v.begin(), v.end(),
            [](const auto& a, const auto& b) { return a.second.records > b.second.records; });
  std::string out;
  for (size_t i = 0; i < v.size() && i < limit; ++i) {
    const auto& [key, smp] = v[i];
    std::string name;
    if (is_register) {
      const RegisterInfo* info = RegisterFile::GetRegisterInfo(key);
      name = info ? info->name : fmt::format("{:04X}", key);
    } else {
      name = fmt::format("c{}", key);
    }
    out += fmt::format(" {} x{} (first: {} record {:08X} vs {:08X});", name, smp.records,
                       kKindNames[smp.kind], smp.record_value, smp.cp_value);
  }
  return out.empty() ? " none" : out;
}

void LogShadowSummary(const CommandProcessor::Fh1NativeShadow& s, const char* when) {
  auto pct = [](uint64_t a, uint64_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
  REXGPU_INFO(
      "FH1 native shadow ({}): {} groups ({} ended, counts agree in {}, {:.2f}%; {} with records "
      "the game could not write, {} never ended), {} records: agree on what is read {} "
      "({:.3f}%), on everything {} ({:.3f}%); {} seeds, {} outside a group, {} replayed, {} "
      "without constants (before a seed or after a lost packet: {}), {} literal loads, {} with an "
      "unanalyzed shader, {} malformed; {} drawn natively (in {} groups); check {:.2f} us a "
      "record",
      when, s.islands, s.islands_ended, s.islands_agree, pct(s.islands_agree, s.islands_ended),
      s.islands_unwritten, s.islands_missing, s.records, s.agree, pct(s.agree, s.records),
      s.agree_all, pct(s.agree_all, s.records), s.seeds, s.outside, s.replays, s.alu_unknown,
      s.constants_lost, s.literal_loads, s.unanalyzed, s.malformed, s.native_records,
      s.native_islands, s.checks ? double(s.check_ns) * 1e-3 / double(s.checks) : 0.0);
  std::string kinds, fields;
  for (uint32_t k = 1; k < fh::kKindCount; ++k) {
    if (!s.kind_records[k])
      continue;
    kinds += fmt::format(" {} {}/{}{} (", kKindNames[k], s.kind_agree[k], s.kind_records[k],
                         s.kind_own[k] ? ", its own state not compared" : "");
    for (uint32_t f = 0; f < kFieldCount; ++f)
      if (s.kind_field[k][f])
        kinds += fmt::format("{} {}, ", kFieldNames[f], s.kind_field[k][f]);
    kinds += ");";
  }
  for (uint32_t f = 0; f < kFieldCount; ++f)
    fields += fmt::format(" {} {};", kFieldNames[f], s.field_records[f]);
  REXGPU_INFO("FH1 native shadow: agreeing by kind:{} records differing in:{}", kinds, fields);
  std::string executed;
  for (const auto& [key, n] : s.executed)
    executed += fmt::format(" {} d{} c{} x{};", kKindNames[key >> 32], (key >> 16) & 0xFFFF,
                            key & 0xFFFF, n);
  std::string groups;
  for (const auto& [key, n] : s.island_counts)
    groups += fmt::format(" calls {} draws {} resolves, executed {} draws {} copies: x{};",
                          key >> 48, (key >> 32) & 0xFFFF, (key >> 16) & 0xFFFF, key & 0xFFFF, n);
  REXGPU_INFO(
      "FH1 native shadow: executed since the previous record:{} draw fields: primitive {}, "
      "count {}, indices {}; shader objects {} ({} changed microcode); groups:{}",
      executed, s.primitive_mismatch, s.count_mismatch, s.index_mismatch, s.shader_hashes.size(),
      s.shader_conflicts, groups);
  REXGPU_INFO("FH1 native shadow: registers read differing:{} | not read:{}",
              TopSamples(s.registers, true, 16), TopSamples(s.registers_unread, true, 12));
  REXGPU_INFO("FH1 native shadow: fetch constants read differing:{} | not read:{}",
              TopSamples(s.fetch_used, true, 8), TopSamples(s.fetch_unused, true, 8));
  REXGPU_INFO("FH1 native shadow: ALU constants read differing:{} | not read: {} constants",
              TopSamples(s.alu_used, false, 12), s.alu_unused_mismatches);
}

}  // namespace

void CommandProcessor::Fh1NativeShadowShutdown() {
  if (!fh1_native_shadow_)
    return;
  LogShadowSummary(*fh1_native_shadow_, "at exit");
  delete fh1_native_shadow_;
  fh1_native_shadow_ = nullptr;
}

}  // namespace rex::graphics
