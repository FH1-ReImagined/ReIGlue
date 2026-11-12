// FH1 PM4 trace (patch 0570): see fh1_pm4_trace.h. Compiled only with REXGLUE_FH1_PM4_TRACE.

#include "fh1_pm4_trace.h"

#include "fh1_pm4_trace_shm.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <rex/chrono/clock.h>
#include <rex/logging.h>

namespace rex::graphics::fh1_pm4_trace {

thread_local Context t_context;
thread_local uint32_t t_packet_start = 0;
bool g_enabled = false;

namespace {

// Packet slots: 0-127 type-3 opcodes, 128 type-0 (unused: split below), 129 type-1,
// 130 type-0 ALU constants (0x4000-0x47FF), 131 type-0 fetch constants (0x4800-0x4BFF),
// 132 other type-0 registers.
constexpr int kSlots = 136;
constexpr int kTags = 256;
enum IbClass { kIbForeign, kIbMixed, kIbWritten, kIbForeignSetConstant, kIbClasses };

struct Counts {
  uint64_t packets[kTags][kSlots];
  uint64_t dwords[kTags];
  uint64_t padding[kTags];
};

struct State {
  FILE* out = nullptr;
  uint32_t every = 300;
  uint8_t* physical = nullptr;
  fh1pm4::Shm* shm = nullptr;
  Counts inner{};  // by the innermost wrapped frame
  Counts outer{};  // by the entry point
  uint64_t ib_runs[kIbClasses]{}, ib_dwords[kIbClasses]{};
  uint64_t ib_nested = 0;
  // Foreign buffers by the tag of the INDIRECT_BUFFER packet that launched them.
  uint64_t parent_runs[2][kTags]{}, parent_dwords[2][kTags]{};
  struct Foreign {
    uint64_t times = 0;
    uint32_t depth = 0;
    uint8_t set_constant = 0, parent = 0;
  };
  std::unordered_map<uint64_t, Foreign> foreign;  // (physical << 20 | dwords)
  uint64_t frames = 0;
  uint64_t block_start = 0;
  bool unlinked = false;
  uint64_t game_prev[6][kTags]{};
  uint64_t mask_prev[kTags][6]{};
};
State* g_state = nullptr;
thread_local int t_ib_depth = 0;

void ShmName(char (&name)[64]) { std::snprintf(name, sizeof(name), "/fh1pm4-%d", int(getpid())); }

fh1pm4::Shm* OpenShm() {
  char name[64];
  ShmName(name);
  int fd = shm_open(name, O_RDWR | O_CREAT, 0600);
  if (fd < 0) return nullptr;
  if (ftruncate(fd, sizeof(fh1pm4::Shm)) != 0) {
    close(fd);
    return nullptr;
  }
  void* p = mmap(nullptr, sizeof(fh1pm4::Shm), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE,
                 fd, 0);
  close(fd);
  return p == MAP_FAILED ? nullptr : static_cast<fh1pm4::Shm*>(p);
}

// The game's segment is usable once its wrappers ran with the same layout.
bool GameReady(const fh1pm4::Shm* s) {
  return __atomic_load_n(&s->magic, __ATOMIC_ACQUIRE) == fh1pm4::kMagic &&
         s->version == fh1pm4::kVersion && s->size == sizeof(fh1pm4::Shm);
}

inline uint8_t TagAt(uint32_t physical) {
  return physical < fh1pm4::kPhys ? g_state->shm->tags[physical >> 2] : 0;
}
inline uint8_t OuterTagAt(uint32_t physical) {
  return physical < fh1pm4::kPhys ? g_state->shm->otags[physical >> 2] : 0;
}
inline uint32_t BeAt(uint32_t physical) {
  uint32_t v;
  std::memcpy(&v, g_state->physical + physical, 4);
  return __builtin_bswap32(v);
}

// The tag of an untagged dword in the current buffer: its class.
inline int ClassTag(Kind kind) {
  switch (kind) {
    case Kind::kPrimary:
      return kTagPrimary;
    case Kind::kForeign:
      return kTagForeign;
    case Kind::kForeignSetConstant:
      return kTagForeignSetConstant;
    case Kind::kWritten:
      return kTagGap;
    default:
      return kTagHost;
  }
}

inline int TagOfOffset(uint32_t offset) {
  const Context& c = t_context;
  if (c.kind == Kind::kNone) return kTagHost;
  int tag = TagAt(c.base + offset);
  return tag ? tag : ClassTag(c.kind);
}

void WriteCounts(const char* prefix, const Counts& c) {
  for (int t = 0; t < kTags; ++t) {
    uint64_t any = c.dwords[t] | c.padding[t];
    for (int k = 0; k < kSlots && !any; ++k) any |= c.packets[t][k];
    if (!any) continue;
    std::fprintf(g_state->out, "%s %d dw=%" PRIu64 " pad=%" PRIu64, prefix, t, c.dwords[t],
                 c.padding[t]);
    for (int k = 0; k < kSlots; ++k) {
      if (c.packets[t][k]) std::fprintf(g_state->out, " %d:%" PRIu64, k, c.packets[t][k]);
    }
    std::fprintf(g_state->out, "\n");
  }
}

}  // namespace

bool Enabled(uint8_t* physical_base) {
  static const bool on = [physical_base] {
    const char* path = std::getenv("FH1_PM4_STATS");
    if (!path || !*path) return false;
    auto state = std::make_unique<State>();
    state->physical = physical_base;
    state->shm = OpenShm();
    state->out = std::fopen(path, "w");
    if (const char* e = std::getenv("FH1_PM4_STATS_EVERY")) {
      state->every = std::max(1, std::atoi(e));
    }
    if (!state->shm || !state->out) {
      REXGPU_ERROR("FH1 PM4 trace: cannot open {} or the shared segment", path);
      return false;
    }
    state->block_start = rex::chrono::Clock::QueryHostTickCount();
    g_state = state.release();
    REXGPU_INFO("FH1 PM4 trace: a block every {} swaps into {}", g_state->every, path);
    return true;
  }();
  g_enabled = on;
  return on;
}

void BeginPrimary(uint32_t physical, uint32_t size) {
  t_context = {physical & 0x1FFFFFFF, size, Kind::kPrimary};
}

void BeginHost(uint32_t physical, uint32_t count) {
  t_context = {physical & 0x1FFFFFFF, count * 4, Kind::kWritten};
}

void BeginIndirect(uint32_t physical, uint32_t count) {
  State& s = *g_state;
  physical &= 0x1FFFFFFF;
  // The parent: the tag of the INDIRECT_BUFFER packet in the buffer around this one.
  const Context& outer = t_context;
  int parent = ClassTag(outer.kind);
  if (outer.kind == Kind::kWritten) {
    const uint32_t at = outer.base + t_packet_start;
    parent = OuterTagAt(at);
    if (!parent) parent = TagAt(at);
  }
  uint32_t tagged = 0;
  for (uint32_t i = 0; i < count; ++i) tagged += TagAt(physical + i * 4) != 0;
  IbClass cls = tagged == 0 ? kIbForeign : (tagged < count ? kIbMixed : kIbWritten);
  bool set_constant = false;
  if (cls == kIbForeign) {
    // Command lists built offline use SET_CONSTANT (0x2D), which this game's D3D never writes.
    for (uint32_t i = 0; i < count;) {
      uint32_t h = BeAt(physical + i * 4);
      uint32_t type = h >> 30;
      if (type == 3 && ((h >> 8) & 0x7F) == 0x2D) {
        set_constant = true;
        break;
      }
      i += type == 2 ? 1 : (type == 1 ? 3 : ((h >> 16) & 0x3FFF) + 2);
    }
    if (set_constant) cls = kIbForeignSetConstant;
    State::Foreign& f = s.foreign[(uint64_t(physical) << 20) | std::min(count, 0xFFFFFu)];
    ++f.times;
    f.depth = uint32_t(t_ib_depth + 1);
    f.set_constant = set_constant;
    f.parent = uint8_t(parent);
    s.parent_runs[set_constant][parent]++;
    s.parent_dwords[set_constant][parent] += count;
  }
  s.ib_runs[cls]++;
  s.ib_dwords[cls] += count;
  if (t_ib_depth > 0) s.ib_nested++;
  ++t_ib_depth;
  Kind kind = cls == kIbForeign ? Kind::kForeign
              : cls == kIbForeignSetConstant ? Kind::kForeignSetConstant
                                             : Kind::kWritten;
  t_context = {physical, count * 4, kind};
}

void EndIndirect() { --t_ib_depth; }

void Padding(uint32_t start_offset, uint32_t dwords) {
  int tag = TagOfOffset(start_offset);
  g_state->inner.padding[tag] += dwords;
  int otag = tag;
  if (tag >= 1 && tag <= fh1pm4::kMaxId) otag = OuterTagAt(t_context.base + start_offset);
  g_state->outer.padding[otag] += dwords;
}

void Packet(uint32_t start_offset, uint32_t end_offset, uint32_t header) {
  State& s = *g_state;
  const Context& c = t_context;
  int tag = TagOfOffset(start_offset);
  uint32_t dwords = c.capacity ? ((end_offset + c.capacity - start_offset) % c.capacity) / 4 : 0;
  int slot;
  switch (header >> 30) {
    case 3:
      slot = (header >> 8) & 0x7F;
      break;
    case 0: {
      uint32_t r = header & 0x7FFF;
      slot = (r >= 0x4000 && r < 0x4800) ? 130 : (r >= 0x4800 && r < 0x4C00) ? 131 : 132;
      break;
    }
    default:
      slot = 129;
      break;
  }
  s.inner.packets[tag][slot]++;
  s.inner.dwords[tag] += dwords;
  int otag = tag;
  if (tag >= 1 && tag <= fh1pm4::kMaxId) otag = OuterTagAt(c.base + start_offset);
  s.outer.packets[otag][slot]++;
  s.outer.dwords[otag] += dwords;
}

void OnSwap() {
  if (!g_enabled) return;
  State& s = *g_state;
  if (++s.frames < s.every) return;
  FILE* out = s.out;
  const fh1pm4::Shm& g = *s.shm;
  if (!s.unlinked) {
    // Both sides have it mapped by now (the game's wrappers ran before any of these swaps): the
    // name goes, so the 256 MB segment ends with the process.
    char name[64];
    ShmName(name);
    shm_unlink(name);
    s.unlinked = true;
  }
  const bool game = GameReady(&g);
  uint64_t now = rex::chrono::Clock::QueryHostTickCount();
  std::fprintf(out, "BLOCK frames=%" PRIu64 " ticks=%" PRIu64 " freq=%" PRIu64 " game=%s\n",
               s.frames, now - s.block_start, rex::chrono::Clock::QueryHostTickFrequency(),
               game ? "traced" : "none");
  s.block_start = now;
  WriteCounts("T", s.inner);
  WriteCounts("TO", s.outer);
  std::fprintf(out,
               "IB foreign=%" PRIu64 "/%" PRIu64 " setconst=%" PRIu64 "/%" PRIu64
               " mixed=%" PRIu64 "/%" PRIu64 " written=%" PRIu64 "/%" PRIu64 " nested=%" PRIu64
               "\n",
               s.ib_runs[kIbForeign], s.ib_dwords[kIbForeign], s.ib_runs[kIbForeignSetConstant],
               s.ib_dwords[kIbForeignSetConstant], s.ib_runs[kIbMixed], s.ib_dwords[kIbMixed],
               s.ib_runs[kIbWritten], s.ib_dwords[kIbWritten], s.ib_nested);
  for (int sc = 0; sc < 2; ++sc) {
    for (int t = 0; t < kTags; ++t) {
      if (!s.parent_runs[sc][t]) continue;
      std::fprintf(out, "P %s %d runs=%" PRIu64 " dw=%" PRIu64 "\n", sc ? "setconst" : "foreign",
                   t, s.parent_runs[sc][t], s.parent_dwords[sc][t]);
    }
  }
  std::vector<std::pair<uint64_t, uint64_t>> order;
  order.reserve(s.foreign.size());
  for (const auto& [key, f] : s.foreign) order.emplace_back(f.times * (key & 0xFFFFF), key);
  std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::fprintf(out, "FOREIGN distinct=%zu\n", s.foreign.size());
  for (size_t i = 0; i < order.size() && i < 60; ++i) {
    const State::Foreign& f = s.foreign[order[i].second];
    std::fprintf(out, "F %08X n=%u times=%" PRIu64 " depth=%u setconst=%u parent=%u\n",
                 uint32_t(order[i].second >> 20), uint32_t(order[i].second & 0xFFFFF), f.times,
                 f.depth, unsigned(f.set_constant), unsigned(f.parent));
  }
  if (game) {
    // The game's counters, as deltas since the last block.
    const uint64_t* cur[6] = {g.calls, g.outer_calls, g.bytes, g.outer_bytes, g.disc, g.mask_calls};
    for (int id = 0; id < kTags; ++id) {
      uint64_t d[6];
      bool any = false;
      for (int k = 0; k < 6; ++k) {
        uint64_t v = __atomic_load_n(&cur[k][id], __ATOMIC_RELAXED);
        d[k] = v - s.game_prev[k][id];
        s.game_prev[k][id] = v;
        any |= d[k] != 0;
      }
      if (!any) continue;
      std::fprintf(out,
                   "G %d calls=%" PRIu64 " outer=%" PRIu64 " bytes=%" PRIu64
                   " outer_bytes=%" PRIu64 " disc=%" PRIu64 " mcalls=%" PRIu64 " masks=",
                   id, d[0], d[1], d[2], d[3], d[4], d[5]);
      for (int k = 0; k < 6; ++k) {
        uint64_t v = __atomic_load_n(&g.mask_pop[id][k], __ATOMIC_RELAXED);
        std::fprintf(out, "%s%" PRIu64, k ? "," : "", v - s.mask_prev[id][k]);
        s.mask_prev[id][k] = v;
      }
      std::fprintf(out, "\n");
    }
    uint32_t ndev = std::min<uint32_t>(g.ndev, fh1pm4::kDevices);
    std::fprintf(out, "DEVS n=%u", ndev);
    for (uint32_t i = 0; i < ndev; ++i) {
      std::fprintf(out, " %08X:%" PRIu64, g.devs[i], g.dev_bytes[i]);
    }
    std::fprintf(out, "\nCURSORS");
    for (uint64_t i = 0; i < g.cursor_logged && i < 64; ++i) {
      std::fprintf(out, " %08X>%08X", uint32_t(g.cursor_log[i][0]), uint32_t(g.cursor_log[i][1]));
    }
    // Outermost calls by return address, cumulative since the start.
    std::fprintf(out, "\nLR");
    for (int i = 0; i < fh1pm4::kLr; ++i) {
      uint64_t k = g.lr[i].key;
      if (k) std::fprintf(out, " %08X/%u:%" PRIu64, uint32_t(k >> 8), uint32_t(k & 0xFF), g.lr[i].count);
    }
    std::fprintf(out, "\n");
  }
  std::fprintf(out, "END\n");
  std::fflush(out);
  s.frames = 0;
  std::memset(&s.inner, 0, sizeof(s.inner));
  std::memset(&s.outer, 0, sizeof(s.outer));
  std::memset(s.ib_runs, 0, sizeof(s.ib_runs));
  std::memset(s.ib_dwords, 0, sizeof(s.ib_dwords));
  s.ib_nested = 0;
  std::memset(s.parent_runs, 0, sizeof(s.parent_runs));
  std::memset(s.parent_dwords, 0, sizeof(s.parent_dwords));
  s.foreign.clear();
}

}  // namespace rex::graphics::fh1_pm4_trace
