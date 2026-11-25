// FH1 (0670): the pass ledger's pure parts (src/graphics/fh1_pass_ledger.h) and the register
// groups (rex/graphics/register_groups.h): every register in exactly one group, the fetch
// constants' texture and sampler bits, the change-mask comparison, the time accrual at marks
// (time lands in the class in progress at a mark, the recorder's waits are left out), the
// sequence numbers of marks read again, the labels and the CSV lines.
#include "../src/graphics/fh1_pass_ledger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace fh = rex::graphics::fh1_host;
namespace pl = rex::graphics::fh1_pass_ledger;
namespace rg = rex::graphics::register_groups;

#define CHECK(x)                                                             \
  do {                                                                       \
    if (!(x)) {                                                              \
      std::fprintf(stderr, "pass ledger: FAIL line %d: %s\n", __LINE__, #x); \
      std::exit(1);                                                          \
    }                                                                        \
  } while (false)

namespace {

void Ranges() {
  // In order, without gaps or overlaps, from 0 to the end of the register file.
  uint32_t next = 0;
  for (const rg::Range& r : rg::kRanges) {
    CHECK(r.first == next);
    CHECK(r.last >= r.first);
    CHECK(r.group < rg::kRangeGroupCount);
    next = r.last + 1;
  }
  CHECK(next == rg::kRegisterEnd);
  // Each register in exactly one range.
  std::vector<int> hits(rg::kRegisterEnd, 0);
  for (const rg::Range& r : rg::kRanges) {
    for (uint32_t i = r.first; i <= r.last; ++i)
      ++hits[i];
  }
  for (int h : hits)
    CHECK(h == 1);
  // A few the counter depends on.
  CHECK(rg::GroupOf(0x2000) == rg::kRenderTargets);  // RB_SURFACE_INFO
  CHECK(rg::GroupOf(0x2208) == rg::kRenderTargets);  // RB_MODECONTROL
  CHECK(rg::GroupOf(0x2200) == rg::kPipelineState);  // RB_DEPTHCONTROL
  CHECK(rg::GroupOf(0x2110) == rg::kDynamicState);   // PA_CL_VPORT_XOFFSET
  CHECK(rg::GroupOf(0x21FC) == rg::kDrawSetup);      // VGT_DRAW_INITIATOR
  CHECK(rg::GroupOf(0x2319) == rg::kResolve);        // RB_COPY_DEST_BASE
  CHECK(rg::GroupOf(0x4000) == rg::kVertexConstants);
  CHECK(rg::GroupOf(0x47FF) == rg::kPixelConstants);
  CHECK(rg::GroupOf(0x4800) == rg::kFetchConstants);
  CHECK(rg::GroupOf(0x4927) == rg::kBoolLoopConstants);
  CHECK(rg::GroupOf(0x0100) == rg::kSystem);
  for (uint32_t g = 0; g < rg::kGroupCount; ++g)
    CHECK(std::strcmp(rg::GroupName(g), "?") != 0);
}

void Fetch() {
  uint32_t a[6] = {0x2 | (1u << 22), 0x1000u, 0x00FF00FFu, 0x0u, 0x0u, 0x0u};  // a texture
  uint32_t b[6];
  std::memcpy(b, a, sizeof(a));
  CHECK(rg::FetchSlotChange(a, b) == 0);
  b[3] |= 1u << 19;  // mag filter: a sampler bit
  CHECK(rg::FetchSlotChange(a, b) == 1u << rg::kSamplers);
  b[1] = 0x2000u;  // base address: the texture's
  CHECK(rg::FetchSlotChange(a, b) == (1u << rg::kSamplers | 1u << rg::kTextures));
  std::memcpy(b, a, sizeof(a));
  b[0] |= 1u << 10;  // clamp x
  CHECK(rg::FetchSlotChange(a, b) == 1u << rg::kSamplers);
  b[0] ^= 1u << 10;
  b[3] |= 1u << 13;  // exp adjust: the texture's
  CHECK(rg::FetchSlotChange(a, b) == 1u << rg::kTextures);
  // A vertex fetch slot (type 3).
  uint32_t v0[6] = {0x3 | 0x100u, 0x40u, 0x3, 0x0, 0x3, 0x0};
  uint32_t v1[6];
  std::memcpy(v1, v0, sizeof(v0));
  v1[2] = 0x3 | 0x200u;
  CHECK(rg::FetchSlotChange(v0, v1) == 1u << rg::kVertexFetch);
}

void Groups() {
  std::vector<uint32_t> now(rg::kRegisterEnd, 0), then(rg::kRegisterEnd, 0);
  uint64_t unchanged[rg::kGroupCount] = {};
  pl::CompareGroups(now.data(), then.data(), unchanged);
  CHECK(unchanged[rg::kRenderTargets] == 1 && unchanged[rg::kVertexConstants] == 1 &&
        unchanged[rg::kTextures] == 1 && unchanged[rg::kSystem] == 0 &&
        unchanged[rg::kFetchConstants] == 0);
  now[0x2110] = 7;      // the viewport
  now[0x4401] = 1;      // a pixel constant
  now[0x2000] = 0x500;  // the surface
  now[0x0100] = 9;      // a system register: not compared
  pl::CompareGroups(now.data(), then.data(), unchanged);
  CHECK(unchanged[rg::kDynamicState] == 1);
  CHECK(unchanged[rg::kPixelConstants] == 1);
  CHECK(unchanged[rg::kRenderTargets] == 1);
  CHECK(unchanged[rg::kVertexConstants] == 2);
  CHECK(unchanged[rg::kPipelineState] == 2);
  // The copy follows: the same values again are unchanged.
  pl::CompareGroups(now.data(), then.data(), unchanged);
  CHECK(unchanged[rg::kDynamicState] == 2 && unchanged[rg::kPixelConstants] == 2 &&
        unchanged[rg::kRenderTargets] == 2);
  // A texture slot's sampler bit.
  now[rg::kFetchFirst + 6 * 3] = 2;
  pl::CompareGroups(now.data(), then.data(), unchanged);  // type change: a texture now
  now[rg::kFetchFirst + 6 * 3 + 4] = 5;                   // word 4: the sampler's
  pl::CompareGroups(now.data(), then.data(), unchanged);
  CHECK(unchanged[rg::kTextures] == 4 && unchanged[rg::kSamplers] == 4);
}

void Slots() {
  CHECK(pl::SlotOf(fh::kPassClassTrack | 2 << 4) == fh::kPassClassTrack + 2 * fh::kPassClassCount);
  CHECK(pl::SlotClass(pl::SlotOf(fh::kPassClassTrack | 2 << 4)) == fh::kPassClassTrack);
  CHECK(pl::SlotLayer(pl::SlotOf(fh::kPassClassTrack | 2 << 4)) == 2);
  CHECK(pl::SlotOf(15) == fh::kPassClassNone);  // an unknown class
  CHECK(pl::SlotOf(fh::kPassClassD3D) < pl::kSlots);
}

void Accrual() {
  uint64_t ns[pl::kSlots] = {};
  pl::Accrual a;
  a.Start(1000, 1, true);
  a.Switch(1500, 3, ns);          // 500 to slot 1
  a.Mark(1700, ns);               // 200 to slot 3 (a segment end: the class goes on)
  a.Pause(1800, ns);              // 100 more to 3, then waiting
  CHECK(a.Resume(2800) == 1000);  // the wait, not counted
  a.Switch(2900, 5, ns);          // 100 to 3
  a.Pause(3000, ns);              // 100 to 5
  // A mark while paused adds nothing, and the wait still counts from the Pause.
  a.Mark(3500, ns);
  a.SetSlot(6);
  CHECK(a.Resume(4000) == 1000);
  a.Pause(4100, ns);  // 100 to 6
  CHECK(ns[1] == 500 && ns[3] == 400 && ns[5] == 100 && ns[6] == 100);
  uint64_t total = 0;
  for (uint64_t v : ns)
    total += v;
  CHECK(total == 1100);  // the busy time exactly: 4100 - 1000 less the waits (1000 + 1000)
  // A clock that goes back adds nothing.
  a.Resume(5000);
  a.Mark(4000, ns);
  CHECK(ns[6] == 100);
}

void Split() {
  uint64_t ticks[pl::kSlots] = {}, waits[pl::kSlots] = {}, out[pl::kSlots] = {};
  // 1000 ns of CPU over three classes: 300 ticks, 600 ticks of which 100 a wait, 100 ticks.
  ticks[1] = 300;
  ticks[3] = 600;
  waits[3] = 100;
  ticks[5] = 100;
  pl::SplitByTicks(1000, ticks, waits, 1, out);
  CHECK(out[1] == 333 && out[3] == 556 && out[5] == 111);  // 333 + 555 + 111 + 1 rounding
  CHECK(out[1] + out[3] + out[5] == 1000);                 // the segment's total is exact
  for (uint32_t s = 0; s < pl::kSlots; ++s)
    CHECK(ticks[s] == 0 && waits[s] == 0);
  // No ticks (a segment of waits only): all to the class in progress.
  uint64_t out2[pl::kSlots] = {};
  ticks[2] = 50;
  waits[2] = 80;
  pl::SplitByTicks(400, ticks, waits, 6, out2);
  CHECK(out2[6] == 400 && out2[2] == 0);
}

void Sequence() {
  pl::Sequence s;
  CHECK(s.New(1));
  CHECK(s.New(2));
  CHECK(!s.New(2));  // the same mark read again (another tile)
  CHECK(!s.New(1));
  CHECK(s.New(3));
  pl::Sequence w;
  CHECK(w.New(0xFFFFFFFEu));
  CHECK(w.New(0xFFFFFFFFu));
  CHECK(w.New(0));  // wraps
  CHECK(!w.New(0xFFFFFFFFu));
}

void Labels() {
  CHECK(pl::LabelName(fh::PassLabel(fh::kPassKindPresentation, 4, 0, 0)) == "p4 #0");
  CHECK(pl::LabelName(fh::PassLabel(fh::kPassKindPresentation, 6, 2, 0)) == "p6 #2");
  CHECK(pl::LabelName(fh::PassLabel(fh::kPassKindPresentation, 4, 0, 3)) == "p4 #0 +3");
  CHECK(pl::LabelName(fh::PassLabel(fh::kPassKindShadowMask, 0, 1, 0)) == "mask #1");
  CHECK(pl::LabelName(fh::PassLabel(fh::kPassKindStart, 0, 0, 2)) == "start +2");
  CHECK(pl::LabelName(fh::PassLabel(fh::kPassKindTail, 0, 0, 0)) == "tail");
  CHECK(pl::LabelName(fh::PassLabel(fh::kPassKindCars, 2, 1, 0)) == "c2 #1");
  CHECK(pl::LabelName(fh::PassLabel(fh::kPassKindSwap, 0, 0, 0)) == "swap");
  CHECK(pl::LabelName(0xFF000000u) == "unlabelled");
}

int Columns(const std::string& line) {
  int n = 1;
  for (char c : line)
    n += c == ',';
  return n;
}

void Csv() {
  pl::Rows rows;
  pl::Row& scene = rows.At(fh::PassLabel(fh::kPassKindPresentation, 4, 0, 0));
  scene.segments = 3;
  scene.rdr_cpu_ns = 1500000;
  scene.rt_known = true;
  scene.rt[0] = 0x0000A500;
  pl::Cell& track = scene.cells[pl::SlotOf(fh::kPassClassTrack)];
  track.dec_ns = 2000000;
  track.rec_draws = 10;
  track.rec_issue_ns = 7000;
  CHECK(&rows.At(fh::PassLabel(fh::kPassKindPresentation, 4, 0, 0)) == &scene);
  pl::Rows window;
  window.Add(rows);
  window.Add(rows);
  CHECK(window.rows.size() == 1 && window.rows[0].segments == 6 &&
        window.rows[0].cells[pl::SlotOf(fh::kPassClassTrack)].rec_draws == 20);
  pl::Totals totals;
  totals.frames = 2;
  totals.frame_draws = 20;
  const std::string header = pl::CsvHeader();
  const std::string lines = pl::CsvLines(5, 1.5, true, window, totals);
  // A segment row, the track row, the frame row: each with the header's columns.
  int count = 0;
  size_t at = 0;
  while (at < lines.size()) {
    const size_t end = lines.find('\n', at);
    CHECK(end != std::string::npos);
    CHECK(Columns(lines.substr(at, end - at)) == Columns(header.substr(0, header.size() - 1)));
    at = end + 1;
    ++count;
  }
  CHECK(count == 3);
  CHECK(lines.find(",p4 #0,segment,0,6,0.00,3.0000,") != std::string::npos);
  CHECK(lines.find(",p4 #0,track,0,0,0.00,0.0000,0,0.0000,0,0,0,4.0000,0,0,0.0000,20,0.0140,") !=
        std::string::npos);
  CHECK(lines.find(",frame,frame,0,0,0.00,0.0000,0,") != std::string::npos);
  CHECK(lines.find("0000A500") != std::string::npos);
}

// The deletion switch (0680): the kinds FORZA_TEST_SKIP_PASS names, and the recorder's decisions
// on a frame with the FH1 track's labels.
void Skip() {
  const auto P = [](uint32_t pass, uint32_t occurrence, uint32_t after) {
    return fh::PassLabel(fh::kPassKindPresentation, pass, occurrence, after);
  };
  const uint32_t mask0 = fh::PassLabel(fh::kPassKindShadowMask, 0, 0, 0);
  const uint32_t mask_after = fh::PassLabel(fh::kPassKindShadowMask, 0, 1, 2);
  const uint32_t cars = fh::PassLabel(fh::kPassKindCars, 3, 1, 0);
  const uint32_t start = fh::PassLabel(fh::kPassKindStart, 0, 0, 1);
  std::string bad;
  pl::SkipSpec spec;
  CHECK(spec.Parse("shadow", &bad) && !spec.copies() && !spec.empty());
  CHECK(spec.Matches(P(6, 0, 0), false) && spec.Matches(P(6, 3, 0), true));
  CHECK(!spec.Matches(P(6, 0, 1), false) && !spec.Matches(P(0, 0, 0), false));
  CHECK(spec.Parse(" prepass , cube,copies", &bad) && spec.copies());
  CHECK(spec.Matches(P(0, 0, 0), false) && spec.Matches(P(1, 5, 0), false));
  CHECK(!spec.Matches(P(4, 0, 0), false) && !spec.Matches(P(6, 0, 0), false));
  CHECK(spec.Parse("scene", &bad));
  for (uint32_t after = 0; after <= 3; ++after)
    CHECK(spec.Matches(P(4, 0, after), false) && spec.Matches(P(4, 0, after), true));
  CHECK(!spec.Matches(P(4, 0, 4), false));
  CHECK(spec.Parse("tiles", &bad));
  CHECK(!spec.Matches(P(4, 0, 0), false) && spec.Matches(P(4, 0, 0), true) &&
        spec.Matches(P(4, 0, 3), true) && !spec.Matches(P(4, 0, 4), true));
  CHECK(spec.Parse("post", &bad));
  CHECK(spec.Matches(P(4, 0, 4), false) && spec.Matches(P(4, 0, 30), false) &&
        !spec.Matches(P(4, 0, 3), false));
  CHECK(spec.Parse("mask,cars,start", &bad));
  CHECK(spec.Matches(mask0, false) && spec.Matches(mask_after, false) && spec.Matches(cars, false) &&
        spec.Matches(start, false) && !spec.Matches(P(4, 0, 0), false));
  CHECK(spec.Parse("p4+5-6,c2,p7+1", &bad));
  CHECK(spec.Matches(P(4, 0, 5), false) && spec.Matches(P(4, 1, 6), false) &&
        !spec.Matches(P(4, 0, 7), false) && !spec.Matches(P(4, 0, 0), false));
  CHECK(spec.Matches(fh::PassLabel(fh::kPassKindCars, 2, 0, 0), false) &&
        !spec.Matches(fh::PassLabel(fh::kPassKindCars, 3, 0, 0), false));
  CHECK(spec.Matches(P(7, 0, 1), false) && !spec.Matches(P(7, 0, 0), false));
  // Labels only the command processor makes (the tail, the swap) are never a kind.
  CHECK(!spec.Matches(fh::PassLabel(fh::kPassKindTail, 0, 0, 0), false));
  for (const char* wrong : {"shadows", "p", "p4+", "p4+3-1", "p4-3", "p256", "q1", "p4+1-2x"}) {
    bad.clear();
    CHECK(!spec.Parse(std::string("cube,") + wrong, &bad) && bad == wrong && spec.empty());
  }
  CHECK(spec.Parse("copies", &bad) && spec.empty());  // nothing to skip: the ledger refuses it
  CHECK(spec.Parse("none", &bad) && spec.empty() && spec.none());  // the baseline
  CHECK(!spec.Matches(P(6, 0, 0), false));
  CHECK(spec.Parse("shadow", &bad) && !spec.none());

  // The recorder: a frame of the shadow maps (with the frame's start), the pre-pass, the scene in
  // three tiles (the second and third read again) and its resolves, then post-processing.
  struct Segment {
    uint32_t pass_id, label;  // the game's pass id at the class mark, the label at the end
    bool repeat;
    uint32_t draws, resolves;
  };
  const std::vector<Segment> frame = {
      {P(6, 0, 0), P(6, 0, 0), false, 156, 1}, {P(0, 0, 0), P(0, 0, 0), false, 554, 1},
      {P(4, 0, 0), P(4, 0, 0), false, 900, 0}, {P(4, 0, 1), P(4, 0, 1), false, 0, 1},
      {P(4, 0, 0), P(4, 0, 0), true, 800, 0},  {P(4, 0, 1), P(4, 0, 1), true, 0, 1},
      {P(4, 0, 0), P(4, 0, 0), true, 700, 0},  {P(4, 0, 1), P(4, 0, 1), true, 0, 1},
      {P(4, 0, 4), P(4, 0, 4), false, 40, 1},
  };
  const auto run = [&](const char* text, const std::vector<Segment>& segments, uint64_t* draws,
                       uint64_t* resolves, uint64_t* wrong) {
    pl::SkipSpec s;
    CHECK(s.Parse(text, nullptr));
    pl::SkipTracker t(s);
    *draws = *resolves = 0;
    for (const Segment& g : segments) {
      t.ClassMark(g.pass_id, g.repeat);
      for (uint32_t i = 0; i < g.draws; ++i)
        *draws += t.Draw(false);
      for (uint32_t i = 0; i < g.resolves; ++i)
        *resolves += t.Draw(true);
      t.SegmentEnd(g.label, g.repeat);
    }
    uint64_t ws = 0, wk = 0;
    t.TakeWrong(&ws, &wk);
    *wrong = ws + wk;
    CHECK(t.skipped_draws() == *draws && t.skipped_copies() == *resolves);
  };
  uint64_t d, r, w;
  run("shadow", frame, &d, &r, &w);
  CHECK(d == 156 && r == 0 && w == 0);  // resolves stay without `copies`
  run("shadow,copies", frame, &d, &r, &w);
  CHECK(d == 156 && r == 1 && w == 0);
  run("tiles,copies", frame, &d, &r, &w);
  CHECK(d == 1500 && r == 2 && w == 0);
  run("scene", frame, &d, &r, &w);
  CHECK(d == 2400 && r == 0 && w == 0);
  run("post,prepass", frame, &d, &r, &w);
  CHECK(d == 594 && r == 0 && w == 0);
  // A prediction that missed: the game said pre-pass, the segment ended as a second shadow map
  // (its draws were skipped wrongly), and a segment predicted as nothing ended as the pre-pass
  // (its draws kept wrongly).
  std::vector<Segment> changed = frame;
  changed[1].label = P(6, 1, 0);
  changed[2].pass_id = 0;
  changed[2].label = P(0, 0, 0);
  run("prepass", changed, &d, &r, &w);
  CHECK(d == 554 && w == 554 + 900);
  // After a segment end nothing is skipped until the next class mark (and a draw there whose
  // segment ends as a kind is counted).
  {
    pl::SkipSpec s;
    CHECK(s.Parse("prepass", nullptr));
    pl::SkipTracker t(s);
    t.ClassMark(P(0, 0, 0), false);
    CHECK(t.Draw(false));
    t.SegmentEnd(P(0, 0, 0), false);
    CHECK(!t.Draw(false) && !t.Draw(true));  // a resolve: not counted without `copies`
    t.SegmentEnd(P(0, 0, 0), false);
    uint64_t ws = 0, wk = 0;
    t.TakeWrong(&ws, &wk);
    CHECK(ws == 0 && wk == 1);
  }
  // The command processor's own ends (tail, swap) keep the pass id the game sent last.
  {
    pl::SkipSpec s;
    CHECK(s.Parse("shadow", nullptr));
    pl::SkipTracker t(s);
    t.ClassMark(P(6, 0, 0), false);  // after the game's swap end: the next frame's first
    t.SegmentEnd(fh::PassLabel(fh::kPassKindTail, 0, 0, 0), false, true);
    t.SegmentEnd(fh::PassLabel(fh::kPassKindSwap, 0, 0, 0), false, true);
    CHECK(t.Draw(false));
    t.SegmentEnd(P(6, 0, 0), false);
    uint64_t ws = 0, wk = 0;
    t.TakeWrong(&ws, &wk);
    CHECK(ws == 0 && wk == 0);
  }
  // The 0670 game sends no pass id: nothing is skipped, every draw of the kind counted wrong.
  std::vector<Segment> old = frame;
  for (Segment& g : old)
    g.pass_id = 0;
  run("prepass", old, &d, &r, &w);
  CHECK(d == 0 && w == 554);
}

}  // namespace

int main() {
  Ranges();
  Fetch();
  Groups();
  Slots();
  Accrual();
  Split();
  Sequence();
  Labels();
  Csv();
  Skip();
  std::printf("pass ledger: all checks passed\n");
  return 0;
}
