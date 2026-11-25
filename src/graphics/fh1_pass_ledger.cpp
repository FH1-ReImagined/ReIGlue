// FH1 (patch 0670): the pass ledger on the command processor's threads (fh1_pass_ledger.h says
// what it measures; the marks: rex/graphics/fh1_host_packet.h kOpPassClass, kOpPassEnd).
//
// Decoder: at each segment end its thread's CPU time since the previous one goes to the segment's
// label (a thread CPU clock read: about half a microsecond with today's kernels, so only there),
// split among the draw classes by the time stamp counter's time in each between the class marks,
// counted only while the decoder runs the ring (not while it waits for the game's next commands,
// nor in WAIT_REG_MEM). The game's own numbers (render thread) come with each mark; a mark read
// again (the scene's commands run once per EDRAM tile) keeps its time but not the game's numbers.
// At the swap the frame goes to the recorder in a recorded call.
// Recorder: the same marks in stream order (a recorded call each), its busy time inside batches by
// the time stamp counter, each IssueDraw timed; at the frame's call the two frames are added to
// the window, and the window is written every fh1_pass_cost_every frames.
#include <rex/graphics/command_processor.h>

#include "fh1_frame_cost.h"
#include "fh1_pass_ledger.h"

#include <rex/cvar.h>
#include <rex/graphics/fh1_host_packet.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if REX_PLATFORM_WIN32
#include <windows.h>
#if REX_ARCH_AMD64
#include <intrin.h>
#endif
#else
#include <time.h>
#if REX_ARCH_AMD64
#include <x86intrin.h>
#endif
#endif

REXCVAR_DEFINE_STRING(
    fh1_pass_cost_csv, "", "GPU",
    "Testing (FH1 pass ledger): write each pass's cost on the render thread, "
    "the GPU command threads and the GPU (with fh1_gpu_profile) to this CSV file; "
    "the game marks its passes only while it is set");
REXCVAR_DEFINE_INT32(fh1_pass_cost_every, 60, "GPU",
                     "Testing (FH1 pass ledger): frames summed in each window of the CSV");
REXCVAR_DEFINE_BOOL(fh1_pass_delta_stats, false, "GPU",
                    "Testing (FH1 pass ledger): also count, per draw, the render state groups and "
                    "recorder stages unchanged since the previous draw (costs time: not for timed "
                    "runs; FORZA_VK_DELTA_STATS=1 too)");

namespace rex::graphics {

namespace fh = fh1_host;
namespace pl = fh1_pass_ledger;

namespace {

uint64_t SteadyNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// The calling thread's CPU time in ns (0 where the platform has no precise clock).
uint64_t ThreadCpuNs() {
#if REX_PLATFORM_WIN32
#if REX_ARCH_AMD64
  // The thread's cycles at the time stamp counter's rate, measured once against
  // QueryPerformanceCounter (as the frame cost's clock, fh1_frame_cost.h; unchecked here).
  static const double ns_per_cycle = [] {
    int regs[4] = {};
    __cpuid(regs, int(0x80000000u));
    const uint32_t max_extended_leaf = uint32_t(regs[0]);
    uint32_t edx = 0;
    if (max_extended_leaf >= 0x80000007u) {
      __cpuid(regs, int(0x80000007u));
      edx = uint32_t(regs[3]);
    }
    if (!fh1_frame_cost::InvariantTsc(max_extended_leaf, edx))
      return 0.0;
    LARGE_INTEGER frequency, first, last;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&first))
      return 0.0;
    const uint64_t tsc_first = __rdtsc();
    do {
      QueryPerformanceCounter(&last);
    } while (last.QuadPart - first.QuadPart < frequency.QuadPart / 500);
    const uint64_t tsc_last = __rdtsc();
    const double per_ms = fh1_frame_cost::CyclesPerMs(tsc_first, tsc_last, first.QuadPart,
                                                      last.QuadPart, frequency.QuadPart);
    return per_ms > 0 ? 1e6 / per_ms : 0.0;
  }();
  ULONG64 cycles = 0;
  if (ns_per_cycle <= 0 || !QueryThreadCycleTime(GetCurrentThread(), &cycles))
    return 0;
  return uint64_t(double(cycles) * ns_per_cycle);
#else
  return 0;
#endif
#else
  timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
    return 0;
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
}

// A fast clock for the time between marks and around each draw: the time stamp counter on
// x86-64 (a few ns to read; invariant on every CPU the game runs on), the steady clock elsewhere.
// Ticks become ns at the rate measured when the ledger starts.
uint64_t Ticks() {
#if REX_ARCH_AMD64
  return __rdtsc();
#else
  return SteadyNs();
#endif
}
double MeasureNsPerTick() {
#if REX_ARCH_AMD64
  const uint64_t s0 = SteadyNs(), t0 = Ticks();
  uint64_t s1;
  do {
    s1 = SteadyNs();
  } while (s1 - s0 < 2000000);  // 2 ms, once
  const uint64_t t1 = Ticks();
  return t1 > t0 ? double(s1 - s0) / double(t1 - t0) : 1.0;
#else
  return 1.0;
#endif
}

constexpr uint32_t kLabelRing = 8192;
const uint32_t kTailLabel = fh::PassLabel(fh::kPassKindTail, 0, 0, 0);

// FNV-1a over 64-bit words (the recorder stages' identities).
uint64_t Mix(uint64_t h, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (8 * i)) & 0xFF;
    h *= 0x100000001B3ull;
  }
  return h;
}

}  // namespace

struct CommandProcessor::Fh1PassLedger {
  // What the decoder hands the recorder at the swap.
  struct DecoderFrame {
    pl::Rows rows;
    pl::Totals totals;
  };

  std::FILE* csv = nullptr;
  uint32_t every = 60;
  bool delta = false;
  bool split = false;

  double ns_per_tick = 1.0;
  uint64_t Ns(uint64_t ticks) const { return uint64_t(double(ticks) * ns_per_tick); }

  // Decoder.
  pl::Sequence sequence;
  pl::Accrual dec_clock;  // ticks, paused while the decoder waits
  pl::Cell dec_pending[pl::kSlots];
  uint64_t dec_ticks[pl::kSlots] = {};
  uint64_t dec_wait_ns[pl::kSlots] = {};  // WAIT_REG_MEM inside the ticks
  uint64_t dec_segment_cpu = 0;           // the thread CPU clock at the previous segment end
  std::unique_ptr<DecoderFrame> dec_frame = std::make_unique<DecoderFrame>();
  uint64_t dec_frame_cpu = 0;
  std::vector<uint32_t> dec_previous;  // the register file at the previous draw (delta)

  // Recorder.
  pl::Accrual rec_clock;  // ticks, paused between batches (split)
  bool rec_started = false;
  uint32_t rec_slot = fh::kPassClassNone;
  pl::Cell rec_pending[pl::kSlots];
  uint64_t rec_ticks[pl::kSlots] = {};
  bool rec_rt_known = false;
  uint32_t rec_rt[3] = {};
  uint64_t rec_idle_ns = 0, rec_frame_cpu = 0;
  pl::Rows rec_frame;
  uint32_t serial = 0;
  struct LabelEntry {
    uint32_t serial = 0, label = 0;
    bool valid = false;
  };
  std::vector<LabelEntry> labels = std::vector<LabelEntry>(kLabelRing);
  uint64_t previous_stages[pl::kStageCount] = {};
  bool previous_stages_valid = false;

  // The window (recorder).
  pl::Rows window_rows;
  pl::Totals window_totals;
  uint64_t window = 0;

  // The segment's CPU time (`cpu` the thread clock now) goes to its classes in proportion to their
  // ticks (all of it to the class in progress if none were counted).
  void FoldDecoder(uint32_t label, uint64_t cpu) {
    pl::Row& row = dec_frame->rows.At(label);
    const uint64_t cpu_ns = cpu > dec_segment_cpu ? cpu - dec_segment_cpu : 0;
    dec_segment_cpu = cpu;
    uint64_t total = 0;
    for (uint32_t s = 0; s < pl::kSlots; ++s) {
      const uint64_t wait = uint64_t(double(dec_wait_ns[s]) / ns_per_tick);
      dec_ticks[s] = dec_ticks[s] > wait ? dec_ticks[s] - wait : 0;
      dec_wait_ns[s] = 0;
      total += dec_ticks[s];
    }
    for (uint32_t s = 0; s < pl::kSlots; ++s) {
      dec_pending[s].dec_ns += total
                                   ? uint64_t(double(cpu_ns) * double(dec_ticks[s]) / double(total))
                                   : (s == dec_clock.slot() ? cpu_ns : 0);
      dec_ticks[s] = 0;
      if (!dec_pending[s].Empty())
        row.cells[s].Add(dec_pending[s]);
      dec_pending[s] = pl::Cell();
    }
  }
  void FoldRecorder(uint32_t label) {
    pl::Row& row = rec_frame.At(label);
    for (uint32_t s = 0; s < pl::kSlots; ++s) {
      rec_pending[s].rec_ns += Ns(rec_ticks[s]);
      rec_ticks[s] = 0;
      if (!rec_pending[s].Empty())
        row.cells[s].Add(rec_pending[s]);
      rec_pending[s] = pl::Cell();
    }
    if (rec_rt_known && !row.rt_known) {
      std::memcpy(row.rt, rec_rt, sizeof(row.rt));
      row.rt_known = true;
    }
    rec_rt_known = false;
  }
  // The game's numbers of a class interval: [0] the class, [1] flush ns, [2] calls, [3] draws,
  // [4] runs.
  void AddGame(const uint32_t* w) {
    pl::Cell& c = dec_pending[pl::SlotOf(w[0])];
    c.rdr_flush_ns += w[1];
    c.rdr_flush_calls += w[2];
    c.submitted += w[3];
    c.runs += w[4];
  }
  void WriteWindow() {
    if (csv) {
      const double epoch =
          std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
              .count();
      const std::string lines = pl::CsvLines(window, epoch, split, window_rows, window_totals);
      std::fwrite(lines.data(), 1, lines.size(), csv);
      std::fflush(csv);
    }
    ++window;
    window_rows.Clear();
    window_totals = pl::Totals();
  }
};

void CommandProcessor::Fh1PassPacket(const uint32_t* words, uint32_t count) {
  const uint32_t op = words[1];
  if ((op == fh::kOpPassClass && count < fh::kPassClassWords) ||
      (op == fh::kOpPassEnd && count < fh::kPassEndWords)) {
    return;
  }
  if (!fh1_ledger_dec_) {
    const std::string path = REXCVAR_GET(fh1_pass_cost_csv);
    if (path.empty())
      return;  // marks without the setting: nothing measured
    auto* ledger = new Fh1PassLedger;
    ledger->every = uint32_t(std::max(1, REXCVAR_GET(fh1_pass_cost_every)));
    const char* env = std::getenv("FORZA_VK_DELTA_STATS");
    ledger->delta = REXCVAR_GET(fh1_pass_delta_stats) || (env && std::strcmp(env, "1") == 0);
    ledger->split = record_split_;
    ledger->csv = std::fopen(path.c_str(), "w");
    if (ledger->csv) {
      const std::string header = pl::CsvHeader();
      std::fwrite(header.data(), 1, header.size(), ledger->csv);
    }
    const uint32_t* regs = (record_split_ ? decode_register_file_.get() : register_file_)->values;
    if (ledger->delta)
      ledger->dec_previous.assign(regs, regs + RegisterFile::kRegisterCount);
    ledger->ns_per_tick = MeasureNsPerTick();
    const uint64_t cpu = ThreadCpuNs();
    ledger->dec_clock.Start(Ticks(), fh::kPassClassNone, true);
    ledger->dec_frame_cpu = ledger->dec_segment_cpu = cpu;
    fh1_ledger_dec_ = ledger;
    REXGPU_INFO("FH1 pass ledger: on ({}), to {}{}, a window every {} frames{}",
                ledger->split ? "split: decoder and recorder" : "one command processor thread",
                path, ledger->csv ? "" : " (could not open it)", ledger->every,
                ledger->delta ? "; change-mask counter on (not for timing)" : "");
    RecordCall([this, ledger]() {
      fh1_ledger_rec_ = ledger;
      // Started inside a batch on the recorder (or at once on one thread).
      ledger->rec_clock.Start(Ticks(), fh::kPassClassNone, true);
      ledger->rec_started = true;
      ledger->rec_frame_cpu = ThreadCpuNs();
      Fh1PassLedgerStarted();
    });
  }
  Fh1PassLedger& l = *fh1_ledger_dec_;
  const uint64_t now = Ticks();
  const bool fresh = l.sequence.New(words[2]);
  ++l.dec_frame->totals.marks;
  if (!fresh)
    ++l.dec_frame->totals.stale_marks;
  uint32_t value;
  if (op == fh::kOpPassClass) {
    if (fresh)
      l.AddGame(words + 4);
    value = words[3];
    l.dec_clock.Switch(now, pl::SlotOf(value), l.dec_ticks);
  } else {
    l.dec_clock.Mark(now, l.dec_ticks);
    value = words[3];
    if (fresh)
      l.AddGame(words + 5);
    pl::Row& row = l.dec_frame->rows.At(value);
    ++row.segments;
    if (fresh) {
      row.rdr_cpu_ns += words[4];
      row.other_device += words[10];
    }
    l.FoldDecoder(value, ThreadCpuNs());
  }
  RecordCall([this, op, value]() { Fh1LedgerRecorderMark(op, value); });
}

void CommandProcessor::Fh1LedgerRecorderMark(uint32_t op, uint32_t value) {
  Fh1PassLedger* ledger = fh1_ledger_rec_;
  if (!ledger)
    return;
  Fh1PassLedger& l = *ledger;
  if (l.split)
    l.rec_clock.Mark(Ticks(), l.rec_ticks);
  if (op == fh::kOpPassClass) {
    l.rec_slot = pl::SlotOf(value);
    l.rec_clock.SetSlot(l.rec_slot);  // accrued up to now above
  } else {
    l.FoldRecorder(value);
    ++l.serial;
    Fh1PassLedger::LabelEntry& e = l.labels[l.serial % kLabelRing];
    e.serial = l.serial;
    e.label = value;
    e.valid = true;
  }
  Fh1PassLedgerTag((l.serial + 1) << 5 | l.rec_slot);
}

void CommandProcessor::Fh1LedgerDecodedDraw(bool executed) {
  Fh1PassLedger& l = *fh1_ledger_dec_;
  pl::Cell& c = l.dec_pending[l.dec_clock.slot()];
  if (!executed) {
    ++c.dec_skipped;
    return;
  }
  ++c.dec_draws;
  if (l.delta && !l.dec_previous.empty()) {
    const uint32_t* regs = (record_split_ ? decode_register_file_.get() : register_file_)->values;
    pl::CompareGroups(regs, l.dec_previous.data(), c.unchanged);
    ++c.delta_draws;
  }
}

void CommandProcessor::Fh1LedgerDecoderSwap() {
  Fh1PassLedger& l = *fh1_ledger_dec_;
  l.dec_clock.Mark(Ticks(), l.dec_ticks);
  const uint64_t cpu = ThreadCpuNs();
  l.FoldDecoder(kTailLabel, cpu);
  l.dec_frame->totals.dec_thread_ns = cpu > l.dec_frame_cpu ? cpu - l.dec_frame_cpu : 0;
  l.dec_frame_cpu = cpu;
  Fh1PassLedger::DecoderFrame* frame = l.dec_frame.release();
  l.dec_frame = std::make_unique<Fh1PassLedger::DecoderFrame>();
  // Before the swap's own call: the frame cost's draws are still the frame's there.
  RecordCall([this, frame]() {
    std::unique_ptr<Fh1PassLedger::DecoderFrame> decoded(frame);
    Fh1PassLedger* ledger = fh1_ledger_rec_;
    if (!ledger)
      return;
    Fh1PassLedger& r = *ledger;
    if (r.split)
      r.rec_clock.Mark(Ticks(), r.rec_ticks);
    r.FoldRecorder(kTailLabel);
    pl::Totals totals = decoded->totals;
    totals.frames = 1;
    totals.frame_draws = fh1_frame_cost::Current().draws;
    if (r.split) {
      const uint64_t cpu = ThreadCpuNs();
      totals.rec_thread_ns = cpu > r.rec_frame_cpu ? cpu - r.rec_frame_cpu : 0;
      r.rec_frame_cpu = cpu;
      totals.rec_idle_ns = r.rec_idle_ns;
      r.rec_idle_ns = 0;
    }
    // The labels in stream order (the recorder's rows come in it).
    for (size_t i = 0; i < r.rec_frame.rows.size(); ++i) {
      r.rec_frame.rows[i].order_sum = i;
      r.rec_frame.rows[i].order_frames = 1;
    }
    r.window_rows.Add(r.rec_frame);
    r.window_rows.Add(decoded->rows);
    r.window_totals.Add(totals);
    r.rec_frame.Clear();
    if (r.window_totals.frames >= r.every)
      r.WriteWindow();
  });
}

bool CommandProcessor::Fh1LedgerIssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                          IndexBufferInfo* index_buffer_info,
                                          bool major_mode_explicit) {
  Fh1PassLedger& l = *fh1_ledger_rec_;
  const RegisterFile& regs = *register_file_;
  const bool copy = regs.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy;
  const uint64_t work = Fh1PassLedgerWork();
  const uint64_t t0 = Ticks();
  const bool result = IssueDraw(prim_type, index_count, index_buffer_info, major_mode_explicit);
  const uint64_t t1 = Ticks();
  pl::Cell& c = l.rec_pending[l.rec_slot];
  if (copy) {
    ++c.rec_copies;
    c.rec_copy_ns += l.Ns(t1 - t0);
    return result;
  }
  ++c.rec_draws;
  c.rec_issue_ns += l.Ns(t1 - t0);
  if (Fh1PassLedgerWork() == work) {
    ++c.rec_empty;
    return result;
  }
  if (!l.rec_rt_known) {
    l.rec_rt[0] = regs[XE_GPU_REG_RB_SURFACE_INFO];
    l.rec_rt[1] = regs[XE_GPU_REG_RB_COLOR_INFO];
    l.rec_rt[2] = regs[XE_GPU_REG_RB_DEPTH_INFO];
    l.rec_rt_known = true;
  }
  if (l.delta) {
    uint64_t stages[pl::kStageCount] = {};
    if (Fh1PassLedgerStages(stages)) {
      stages[pl::kStageShaders] =
          Mix(Mix(0xCBF29CE484222325ull, uint64_t(uintptr_t(active_vertex_shader_))),
              uint64_t(uintptr_t(active_pixel_shader_)));
      if (l.previous_stages_valid) {
        ++c.stage_draws;
        for (uint32_t s = 0; s < pl::kStageCount; ++s) {
          if (stages[s] == l.previous_stages[s])
            ++c.stage_same[s];
        }
      }
      std::memcpy(l.previous_stages, stages, sizeof(stages));
      l.previous_stages_valid = true;
    }
  }
  return result;
}

void CommandProcessor::Fh1LedgerBatch(bool begin) {
  Fh1PassLedger& l = *fh1_ledger_rec_;
  if (!l.rec_started)
    return;
  const uint64_t now = Ticks();
  if (begin) {
    l.rec_idle_ns += l.Ns(l.rec_clock.Resume(now));
  } else {
    l.rec_clock.Pause(now, l.rec_ticks);
  }
}

void CommandProcessor::Fh1LedgerDecoderWaited(std::chrono::steady_clock::time_point start) {
  Fh1PassLedger& l = *fh1_ledger_dec_;
  l.dec_wait_ns[l.dec_clock.slot()] += uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
          .count());
}

void CommandProcessor::Fh1LedgerDecoderBusy(bool begin) {
  Fh1PassLedger& l = *fh1_ledger_dec_;
  const uint64_t now = Ticks();
  if (begin) {
    l.dec_clock.Resume(now);
  } else {
    l.dec_clock.Pause(now, l.dec_ticks);
  }
}

uint64_t CommandProcessor::Fh1PassLedgerTagKey(uint32_t tag) const {
  const Fh1PassLedger* l = fh1_ledger_rec_;
  const uint32_t slot = tag & 31, segment = tag >> 5;
  if (!l)
    return ~uint64_t(0);
  const Fh1PassLedger::LabelEntry& e = l->labels[segment % kLabelRing];
  if (!e.valid || e.serial != segment)
    return ~uint64_t(0);
  return uint64_t(e.label) << 8 | slot;
}

std::string CommandProcessor::Fh1PassLedgerKeyName(uint64_t key) {
  if (key == ~uint64_t(0))
    return "?";
  const uint32_t slot = uint32_t(key & 0xFF);
  std::string name = pl::LabelName(uint32_t(key >> 8));
  name += '|';
  name += pl::ClassName(pl::SlotClass(slot));
  name += '|';
  name += std::to_string(pl::SlotLayer(slot));
  return name;
}

void CommandProcessor::Fh1LedgerShutdown() {
  Fh1PassLedger* l = fh1_ledger_dec_;
  if (!l)
    return;
  if (l->window_totals.frames)
    l->WriteWindow();
  if (l->csv)
    std::fclose(l->csv);
  l->csv = nullptr;
  fh1_ledger_dec_ = fh1_ledger_rec_ = nullptr;
  delete l;
}

}  // namespace rex::graphics
