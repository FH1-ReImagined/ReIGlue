// FH1 list trace (patch 0640): see fh1_list_trace.h. Compiled only with REXGLUE_FH1_LIST_TRACE.

#include "fh1_list_trace.h"

#include "fh1_list_trace_shm.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

#include <rex/logging.h>

namespace rex::graphics::fh1_list_trace {

bool g_on = false;

namespace {

inline uint64_t Ticks() {
#if defined(__x86_64__)
  return __rdtsc();
#else
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}
// The decoder thread's CPU time (a system call: only around the primary ring, a few a frame).
inline uint64_t ThreadCpuNs() {
  timespec t;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
}
// A counter of the command processor that LogRecordSplit sets back to 0 once a minute.
inline uint64_t Since(uint64_t start, uint64_t now) {
  return now >= start ? now - start : now;
}
inline uint64_t SteadyNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

constexpr uint32_t kDepths = 4;  // 1, 2, 3, deeper
constexpr uint32_t kGroups = 256;
// SCRATCH_REG0-7, SCRATCH_UMSK, SCRATCH_ADDR, SHADER_CONSTANT_FLUSH_FETCH_0-2.
constexpr uint32_t kRegScratch0 = 0x578, kRegScratchUmsk = 0x1DC;  // register_table.inc

struct ClassCounts {
  uint64_t runs = 0, dwords = 0, ticks = 0, self_ticks = 0;
  uint64_t sync_runs = 0, sync_ticks = 0;
  uint64_t registers_in = 0, registers_out = 0;
  uint64_t skipped = 0;
};

struct Unregistered {
  uint64_t runs = 0, dwords = 0, ticks = 0;
  uint32_t depth = 0;
};

struct State {
  FILE* out = nullptr;
  std::string lists_path;
  uint32_t every = 600;
  uint8_t* physical = nullptr;
  fh1list::Shm* shm = nullptr;
  bool unlinked = false;
  uint64_t frames = 0, block_ticks = 0, block_ns = 0;
  // The primary ring.
  uint64_t primary_calls = 0, primary_dwords = 0, primary_ticks = 0, primary_cpu_ns = 0;
  uint32_t primary_depth = 0;
  // Indirect buffers by class, pass group and depth.
  ClassCounts counts[fh1list::kClassCount][kGroups][kDepths];
  std::unordered_map<uint64_t, Unregistered> unregistered;  // depth >= 2: (physical << 20 | dwords)
  // Bin select and mask when a buffer at depth 2 or deeper starts.
  std::map<std::tuple<uint8_t, uint8_t, uint64_t, uint64_t>, uint64_t> bins;
  uint64_t umsk[fh1list::kClassCount][256] = {};
  // Register reads by packets: (opcode, register, memory poll) -> count.
  std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint64_t> reads;
  uint64_t counter_prev[fh1list::kCounters] = {};
};
State* g_state = nullptr;
thread_local IndirectScope* t_top = nullptr;
thread_local uint32_t t_depth = 0;
std::atomic<uint64_t> g_cpu_reads[16];  // the registers above by CPU (MMIO) reads; 15 = others

void ShmName(char (&name)[64]) {
  std::snprintf(name, sizeof(name), "/fh1list-%d", int(getpid()));
}

fh1list::Shm* OpenShm() {
  char name[64];
  ShmName(name);
  int fd = shm_open(name, O_RDWR | O_CREAT, 0600);
  if (fd < 0)
    return nullptr;
  if (ftruncate(fd, sizeof(fh1list::Shm)) != 0) {
    close(fd);
    return nullptr;
  }
  void* p = mmap(nullptr, sizeof(fh1list::Shm), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE,
                 fd, 0);
  close(fd);
  return p == MAP_FAILED ? nullptr : static_cast<fh1list::Shm*>(p);
}

bool GameReady(const fh1list::Shm* s) {
  return __atomic_load_n(&s->magic, __ATOMIC_ACQUIRE) == fh1list::kMagic &&
         s->version == fh1list::kVersion && s->size == sizeof(fh1list::Shm);
}

inline uint32_t DepthSlot(uint32_t depth) {
  return std::min(depth, kDepths) - 1;
}

uint64_t HashWords(const uint8_t* p, uint32_t dwords) {
  uint64_t h = 0xCBF29CE484222325ull ^ dwords;
  for (uint32_t i = 0; i < dwords; ++i) {
    uint32_t v;
    std::memcpy(&v, p + i * 4, 4);
    h = (h ^ v) * 0x100000001B3ull;
  }
  return h | 1;
}

uint64_t SyncTotal(const Sources& s) {
  uint64_t n = 0;
  for (uint32_t i = 0; i < s.sync_reasons; ++i)
    n += s.syncs[i];
  return n;
}

}  // namespace

bool Enabled(uint8_t* physical_base) {
  static const bool on = [physical_base] {
    const char* path = std::getenv("FH1_LIST_STATS");
    if (!path || !*path)
      return false;
    auto state = std::make_unique<State>();
    state->physical = physical_base;
    state->shm = OpenShm();
    state->out = std::fopen(path, "w");
    state->lists_path = std::string(path) + ".lists";
    if (const char* e = std::getenv("FH1_LIST_STATS_EVERY")) {
      state->every = std::max(1, std::atoi(e));
    }
    if (!state->shm || !state->out) {
      REXGPU_ERROR("FH1 list trace: cannot open {} or the shared segment", path);
      return false;
    }
    state->block_ticks = Ticks();
    state->block_ns = SteadyNs();
    g_state = state.release();
    REXGPU_INFO("FH1 list trace: a block every {} swaps into {}", g_state->every, path);
    return true;
  }();
  g_on = on;
  return on;
}

PrimaryScope::PrimaryScope(uint32_t dwords) {
  if (!g_on)
    return;
  State& s = *g_state;
  on_ = s.primary_depth++ == 0;
  if (!on_)
    return;
  ++s.primary_calls;
  s.primary_dwords += dwords;
  cpu_start_ = ThreadCpuNs();
  start_ = Ticks();
}

PrimaryScope::~PrimaryScope() {
  if (!g_on)
    return;
  State& s = *g_state;
  --s.primary_depth;
  if (on_) {
    s.primary_ticks += Ticks() - start_;
    s.primary_cpu_ns += ThreadCpuNs() - cpu_start_;
  }
}

IndirectScope::IndirectScope(uint32_t physical, uint32_t dwords, const Sources& sources) {
  if (!g_on)
    return;
  State& s = *g_state;
  physical &= 0x1FFFFFFF;
  sources_ = &sources;
  physical_ = physical;
  dwords_ = dwords;
  parent_ = t_top;
  t_top = this;
  depth_ = uint8_t(std::min<uint32_t>(++t_depth, 255));
  fh1list::List* e = fh1list::Find(s.shm, physical);
  list_ = e;
  cls_ = e ? e->cls : fh1list::kClassNone;
  group_ = e ? e->group : fh1list::kGroupNone;
  if (cls_ >= fh1list::kClassCount)
    cls_ = fh1list::kClassOther;
  if (e) {
    // A command buffer recorded by D3D: are its words the same as the last time (sampled: the
    // first run and every 64th, outside the timed part)?
    if (cls_ == fh1list::kClassRecorded && (e->runs & 63) == 0 &&
        physical + uint64_t(dwords) * 4 <= (512ull << 20)) {
      const uint64_t h = HashWords(s.physical + physical, dwords);
      if (e->hash && e->hash != h)
        ++e->changes;
      e->hash = h;
    }
  }
  if (depth_ >= 2) {
    const uint32_t umsk = sources.registers[kRegScratchUmsk];
    ++s.umsk[cls_][umsk & 0xFF];
    ++s.bins[{cls_, group_, *sources.bin_select, *sources.bin_mask}];
  }
  in_ = *sources.registers_in;
  out_ = *sources.registers_out;
  syncs_ = SyncTotal(sources);
  start_ = Ticks();
}

IndirectScope::~IndirectScope() {
  if (!sources_)
    return;
  const uint64_t ticks = Ticks() - start_;
  State& s = *g_state;
  t_top = parent_;
  --t_depth;
  if (parent_)
    parent_->children_ += ticks;
  const bool synced = SyncTotal(*sources_) != syncs_;
  ClassCounts& c = s.counts[cls_][group_][DepthSlot(depth_)];
  ++c.runs;
  c.dwords += dwords_;
  c.registers_in += Since(in_, *sources_->registers_in);
  c.registers_out += Since(out_, *sources_->registers_out);
  if (synced) {
    ++c.sync_runs;
    c.sync_ticks += ticks;
  } else {
    c.ticks += ticks;
    c.self_ticks += ticks - std::min(ticks, children_);
  }
  if (auto* e = static_cast<fh1list::List*>(list_)) {
    __atomic_fetch_add(&e->runs, 1, __ATOMIC_RELAXED);
    if (!synced)
      __atomic_fetch_add(&e->ticks, ticks, __ATOMIC_RELAXED);
  } else if (depth_ >= 2) {
    Unregistered& u = s.unregistered[(uint64_t(physical_) << 20) | std::min(dwords_, 0xFFFFFu)];
    ++u.runs;
    u.dwords += dwords_;
    u.ticks += ticks;
    u.depth = depth_;
  }
}

void SkippedIndirect(uint32_t physical) {
  if (!g_on)
    return;
  State& s = *g_state;
  fh1list::List* e = fh1list::Find(s.shm, physical & 0x1FFFFFFF);
  uint8_t cls = e ? e->cls : fh1list::kClassNone;
  if (cls >= fh1list::kClassCount)
    cls = fh1list::kClassOther;
  const uint8_t group = e ? e->group : fh1list::kGroupNone;
  ++s.counts[cls][group][DepthSlot(t_depth + 1)].skipped;
  if (e)
    __atomic_fetch_add(&e->skipped, 1, __ATOMIC_RELAXED);
}

void RegisterRead(uint32_t opcode, const uint32_t* first) {
  State& s = *g_state;
  uint32_t reg = 0, memory = 0;
  switch (opcode) {
    case 0x3C:  // WAIT_REG_MEM: wait info, then the register (or the address with bit 4)
    case 0x45:  // COND_WRITE
      memory = (first[0] >> 4) & 1;
      reg = memory ? 0 : first[1];
      break;
    case 0x21:  // REG_RMW
      reg = first[0] & 0x1FFF;
      break;
    case 0x3E:  // REG_TO_MEM
      reg = first[0];
      break;
    default:
      return;
  }
  ++s.reads[{opcode, reg, memory}];
}

void CpuRegisterRead(uint32_t index) {
  uint32_t slot = 15;
  if (index >= kRegScratch0 && index < kRegScratch0 + 8)
    slot = index - kRegScratch0;
  else if (index >= 0x5000 && index <= 0x5002)
    slot = 8 + (index - 0x5000);
  else if (index == kRegScratchUmsk)
    slot = 11;
  else if (index == kRegScratchUmsk + 1)
    slot = 12;  // SCRATCH_ADDR
  g_cpu_reads[slot].fetch_add(1, std::memory_order_relaxed);
}

void OnSwap() {
  if (!g_on)
    return;
  State& s = *g_state;
  if (++s.frames < s.every)
    return;
  FILE* out = s.out;
  const fh1list::Shm& g = *s.shm;
  const bool game = GameReady(&g);
  // The name goes once the game has mapped the segment too (its hooks run later than the
  // first swaps), so the segment ends with the process.
  if (!s.unlinked && game) {
    char name[64];
    ShmName(name);
    shm_unlink(name);
    s.unlinked = true;
  }
  const uint64_t now_ticks = Ticks(), now_ns = SteadyNs();
  const double wall =
      std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
  std::fprintf(out, "BLOCK frames=%" PRIu64 " ticks=%" PRIu64 " ns=%" PRIu64 " wall=%.3f game=%s\n",
               s.frames, now_ticks - s.block_ticks, now_ns - s.block_ns, wall,
               game ? "traced" : "none");
  s.block_ticks = now_ticks;
  s.block_ns = now_ns;
  std::fprintf(out,
               "PRIMARY calls=%" PRIu64 " dw=%" PRIu64 " ticks=%" PRIu64 " cpu_ns=%" PRIu64 "\n",
               s.primary_calls, s.primary_dwords, s.primary_ticks, s.primary_cpu_ns);
  for (uint32_t cls = 0; cls < fh1list::kClassCount; ++cls) {
    for (uint32_t group = 0; group < kGroups; ++group) {
      for (uint32_t d = 0; d < kDepths; ++d) {
        const ClassCounts& c = s.counts[cls][group][d];
        if (!c.runs && !c.skipped)
          continue;
        std::fprintf(out,
                     "C %u %u %u runs=%" PRIu64 " dw=%" PRIu64 " ticks=%" PRIu64 " self=%" PRIu64
                     " sync_runs=%" PRIu64 " sync_ticks=%" PRIu64 " rin=%" PRIu64 " rout=%" PRIu64
                     " skipped=%" PRIu64 "\n",
                     cls, group, d + 1, c.runs, c.dwords, c.ticks, c.self_ticks, c.sync_runs,
                     c.sync_ticks, c.registers_in, c.registers_out, c.skipped);
      }
    }
  }
  for (const auto& [key, n] : s.bins) {
    std::fprintf(out, "BIN %u %u sel=%016" PRIX64 " mask=%016" PRIX64 " runs=%" PRIu64 "\n",
                 unsigned(std::get<0>(key)), unsigned(std::get<1>(key)), std::get<2>(key),
                 std::get<3>(key), n);
  }
  for (uint32_t cls = 0; cls < fh1list::kClassCount; ++cls) {
    for (uint32_t v = 0; v < 256; ++v) {
      if (s.umsk[cls][v]) {
        std::fprintf(out, "UMSK %u %02X runs=%" PRIu64 "\n", cls, v, s.umsk[cls][v]);
      }
    }
  }
  for (const auto& [key, n] : s.reads) {
    std::fprintf(out, "READ op=%02X reg=%04X mem=%u n=%" PRIu64 "\n", std::get<0>(key),
                 std::get<1>(key), std::get<2>(key), n);
  }
  std::fprintf(out, "CPUREAD");
  for (int i = 0; i < 16; ++i) {
    std::fprintf(out, " %" PRIu64, g_cpu_reads[i].exchange(0, std::memory_order_relaxed));
  }
  std::fprintf(out, "\n");
  for (const auto& [key, u] : s.unregistered) {
    std::fprintf(out, "U %08X n=%u depth=%u runs=%" PRIu64 " dw=%" PRIu64 " ticks=%" PRIu64 "\n",
                 uint32_t(key >> 20), uint32_t(key & 0xFFFFF), u.depth, u.runs, u.dwords, u.ticks);
  }
  if (game) {
    for (uint32_t i = 0; i < fh1list::kCounters; ++i) {
      const uint64_t key = __atomic_load_n(&g.counters[i].key, __ATOMIC_ACQUIRE);
      if (!key)
        continue;
      const uint64_t v = __atomic_load_n(&g.counters[i].count, __ATOMIC_RELAXED);
      if (v != s.counter_prev[i]) {
        std::fprintf(out, "K %016" PRIX64 " %" PRIu64 "\n", key, v - s.counter_prev[i]);
        s.counter_prev[i] = v;
      }
    }
    // Every registered buffer's totals since the start, rewritten each block.
    const std::string tmp = s.lists_path + ".tmp";
    if (FILE* l = std::fopen(tmp.c_str(), "w")) {
      for (uint32_t i = 0; i < fh1list::kLists; ++i) {
        const fh1list::List& e = g.lists[i];
        if (!__atomic_load_n(&e.ready, __ATOMIC_ACQUIRE))
          continue;
        std::fprintf(l,
                     "L %08X n=%u cb=%08X flags=%X cls=%u group=%u ctx=%u binds=%u calls=%" PRIu64
                     " runs=%" PRIu64 " skipped=%" PRIu64 " ticks=%" PRIu64 " changes=%" PRIu64
                     "\n",
                     e.phys, e.words, e.cb, e.flags, unsigned(e.cls), unsigned(e.group),
                     unsigned(e.context), e.binds, e.calls, e.runs, e.skipped, e.ticks, e.changes);
      }
      std::fclose(l);
      std::rename(tmp.c_str(), s.lists_path.c_str());
    }
  }
  std::fprintf(out, "END\n");
  std::fflush(out);
  s.frames = 0;
  s.primary_calls = s.primary_dwords = s.primary_ticks = s.primary_cpu_ns = 0;
  for (auto& by_class : s.counts) {
    for (auto& by_group : by_class) {
      for (ClassCounts& c : by_group)
        c = ClassCounts{};
    }
  }
  s.unregistered.clear();
  s.bins.clear();
  std::memset(s.umsk, 0, sizeof(s.umsk));
  s.reads.clear();
}

}  // namespace rex::graphics::fh1_list_trace
