// FH1 (patch 0650): the model list registry and its shadow compare
// (src/graphics/fh1_native_list.h), without a GPU or the game: on the two lists of
// tests/fh1_native_list_fixture.h (a depth pass's shape and a scene pass's with its tile blocks,
// and the templates FH1's converter made of them), run by a small PM4 interpreter that does what
// the command processor does with their packets.
// - Validate takes both templates and refuses damaged ones; the readers never read outside a
//   template it took (run under the sanitizers);
// - the live key: the same after fixups, with the loader's flags, and untiled; another after a
//   static word changed;
// - the registry: a define repeated, a bind repeated or replaced, a lookup with a wrong length,
//   an address dropped and bound again to another list;
// - the key check: a key that differs at the first check after a bind is the template's fault
//   (key-at-bind), one that differs after a check that matched is a static word the game wrote
//   (static-words-changed); a template whose slot is one dword off is the first;
// - the compare: every draw and the end agree for each tile's bin select, the bin masks a list
//   is seen to start with, and untiled; it disagrees, in the field it should, when the packets
//   write a register the template does not know, when a pool is stale, when a draw's predicate
//   goes the other way, and when the draws come out of order; and, by the registers written,
//   also when the values agree: a tile block's mask that is another tile's while the register
//   file already holds the selected tile's window, a write the template puts on another
//   register whose value both already hold.
#include "../src/graphics/fh1_native_list.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "fh1_native_list_fixture.h"

namespace fl = rex::graphics::fh1_list;
namespace fx = native_list_fixture;

#define CHECK(x)                                                                 \
  do {                                                                           \
    if (!(x)) {                                                                  \
      std::fprintf(stderr, "fh1 native list: FAIL line %d: %s\n", __LINE__, #x); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (false)

namespace {

template <size_t N>
std::vector<uint32_t> V(const uint32_t (&a)[N]) {
  return std::vector<uint32_t>(a, a + N);
}
std::vector<uint8_t> Guest(const std::vector<uint32_t>& words) {
  std::vector<uint8_t> b(words.size() * 4);
  for (size_t i = 0; i < words.size(); ++i) {
    const uint32_t g = __builtin_bswap32(words[i]);
    std::memcpy(b.data() + i * 4, &g, 4);
  }
  return b;
}

struct Case {
  std::vector<uint32_t> header, table, pm4, tmpl;
};
Case Depth() {
  return {V(fx::kDepthHeader), V(fx::kDepthPatchTable), V(fx::kDepthPm4), V(fx::kDepthTemplate)};
}
Case Scene() {
  return {V(fx::kSceneHeader), V(fx::kScenePatchTable), V(fx::kScenePm4), V(fx::kSceneTemplate)};
}

// The fixups: every slot's words given values, as the game's fixups would (a shader address keeps
// its low two bits, the type).
void FixUp(Case& c, uint32_t seed) {
  const uint32_t* t = c.tmpl.data();
  const uint32_t* slot = t + t[fl::kTSlots];
  for (uint32_t s = 0; s < t[fl::kTSlotCount]; ++s, slot += fl::kSlotWords) {
    for (uint32_t i = 0; i < slot[3]; ++i) {
      uint32_t& w = c.pm4[slot[1] + i];
      const uint32_t type = slot[0] & 0xFF, argument = slot[0] >> 8;
      if ((type == fl::kSlotVertexShader || type == fl::kSlotPixelShader) && argument == 1) {
        w = 0x00100000u + seed * 0x1000 + slot[1] * 0x40;  // a literal block in `Memory`
      } else if (type == fl::kSlotVertexShader || type == fl::kSlotPixelShader) {
        w = (0x00200000u + seed * 0x100 + slot[1] * 4) | (w & 3);
      } else {
        w = 0x12340000u + seed * 0x100 + slot[1] + i;
      }
    }
  }
}

// Guest memory for the literal blocks: 16 words at every address FixUp names.
const uint8_t* Memory(void*, uint32_t address) {
  static std::map<uint32_t, std::vector<uint8_t>> blocks;
  auto& b = blocks[address];
  if (b.empty()) {
    std::vector<uint32_t> w(fl::kLiteralWords);
    for (uint32_t i = 0; i < w.size(); ++i)
      w[i] = address ^ (0x5A5A0000u + i);
    b = Guest(w);
  }
  return b.data();
}

// What the command processor does with a model list's packets (command_processor.cpp), with a
// hook at every draw packet as patch 0650's: the register file, the bin select and mask, the
// last IM_LOAD of each type.
struct Interpreter {
  std::vector<uint32_t> regs = std::vector<uint32_t>(fl::kRegisterLimit, 0);
  uint64_t select = 0xFFFFFFFF, mask = 0xFFFFFFFF;
  fl::Shaders shaders;
  // Faults to inject.
  int extra_write_at_draw = -1;  // before draw k's hook, a register the template knows nothing of
  bool ignore_predicate = false;
  int swap_draws = -1;  // report draw k's packet as the next one's
  // Told of every register write, as the command processor's write paths tell the run.
  fl::ShadowRun* run = nullptr;

  void Set(uint32_t reg, uint32_t value) {
    regs[reg] = value;
    if (run)
      run->LiveWrites(reg, 1);
  }

  template <typename OnDraw>
  void Run(const std::vector<uint32_t>& pm4, OnDraw&& on_draw) {
    uint32_t draw = 0;
    for (uint32_t d = 0; d < pm4.size();) {
      const uint32_t h = pm4[d];
      const uint32_t type = h >> 30, count = ((h >> 16) & 0x3FFF) + 1;
      if (type == 2) {
        ++d;
        continue;
      }
      if (type == 0) {
        const uint32_t first = h & 0x7FFF;
        for (uint32_t i = 0; i < count; ++i)
          Set((h & 0x8000) ? first : first + i, pm4[d + 1 + i]);
        d += 1 + count;
        continue;
      }
      const uint32_t opcode = (h >> 8) & 0x7F;
      const uint32_t* p = &pm4[d + 1];
      if ((h & 1) && !ignore_predicate && !(select & mask)) {
        if (opcode == 0x22)
          on_draw(d, false, draw++);
        d += 1 + count;
        continue;
      }
      switch (opcode) {
        case 0x10:  // NOP
          break;
        case 0x60:
          mask = (mask & ~0xFFFFFFFFull) | p[0];
          break;
        case 0x61:
          mask = (mask & 0xFFFFFFFFull) | uint64_t(p[0]) << 32;
          break;
        case 0x2D:  // SET_CONSTANT of registers
          CHECK(((p[0] >> 16) & 0xFF) == 4);
          for (uint32_t i = 1; i < count; ++i)
            Set(0x2000 + (p[0] & 0x7FF) + i - 1, p[i]);
          break;
        case 0x27:  // IM_LOAD
          shaders.address[p[0] & 1] = p[0];
          shaders.size[p[0] & 1] = p[1];
          shaders.valid[p[0] & 1] = true;
          break;
        case 0x2F: {  // LOAD_ALU_CONSTANT
          const uint8_t* block = Memory(nullptr, p[0] & 0x3FFFFFFF);
          for (uint32_t i = 0; i < (p[2] & 0xFFF); ++i)
            Set(0x4000 + (p[1] & 0x7FF) + i, fl::Word(block, i));
          break;
        }
        case 0x22:  // DRAW_INDX: reported before it writes its three registers
          if (int(draw) == extra_write_at_draw)
            Set(0x2100, regs[0x2100] ^ 1);
          on_draw(int(draw) == swap_draws ? d + 1 : d, true, draw);
          ++draw;
          Set(0x21FC, p[1]);
          Set(0x21FA, p[2]);
          Set(0x21FB, p[3]);
          break;
        default:
          CHECK(false);
      }
      d += 1 + count;
    }
  }
};

// One run of a list beside its template; the outcomes of every draw and the end.
struct Result {
  uint32_t draws = 0, draws_agree = 0, skipped = 0, draw_fields = 0, end_fields = 0;
  bool end_agree = false;
};
// `start`, if given, changes the register file the list starts with (after the other state).
Result Compare(const Case& c, uint64_t select, uint64_t mask, Interpreter in = Interpreter(),
               const std::vector<uint32_t>* stale_pool = nullptr,
               void (*start)(const Case&, const uint32_t* pool, std::vector<uint32_t>*) = nullptr) {
  const uint32_t* t = c.tmpl.data();
  const std::vector<uint8_t> pm4 = Guest(c.pm4);
  std::vector<uint32_t> pool(t[fl::kTPoolWords]);
  fl::Resolve(t, pm4.data(), pool.data());
  if (stale_pool)
    pool = *stale_pool;
  for (uint32_t r = 0; r < fl::kRegisterLimit; ++r)
    in.regs[r] = r * 0x9E3779B1u;  // other state
  if (start)
    start(c, pool.data(), &in.regs);
  in.select = select;
  in.mask = mask;
  in.shaders.address[0] = 0x00300000;
  in.shaders.size[0] = 9;
  in.shaders.valid[0] = true;
  fl::ShadowRun run;
  run.Begin(t, pool.data(), select, mask, fl::Untiled(t, pm4.data()), in.regs.data(), in.shaders,
            Memory, nullptr);
  in.run = &run;
  Result r;
  in.Run(c.pm4, [&](uint32_t packet, bool executed, uint32_t) {
    const fl::Outcome o = run.Draw(packet, executed, in.regs.data(), in.mask, in.shaders);
    ++r.draws;
    r.skipped += !executed;
    r.draws_agree += o.agree;
    r.draw_fields |= o.fields;
  });
  const fl::Outcome o = run.End(in.regs.data(), in.mask, in.shaders);
  r.end_agree = o.agree;
  r.end_fields = o.fields;
  return r;
}
bool Agree(const Result& r, uint32_t draws) {
  return r.draws == draws && r.draws_agree == draws && r.end_agree;
}

std::vector<uint32_t> Untile(std::vector<uint32_t> pm4, const std::vector<uint32_t>& tmpl) {
  const uint32_t* t = tmpl.data();
  const uint32_t* bound = t + t[fl::kTTileBound];
  for (uint32_t i = 0; i < t[fl::kTTileBoundCount]; ++i) {
    pm4[bound[2 * i]] = fl::NopPacket(bound[2 * i + 1]);
  }
  return pm4;
}

fl::Key Key(const Case& c, uint32_t flags) {
  const auto h = Guest(c.header), p = Guest(c.table), m = Guest(c.pm4);
  return fl::LiveKey(c.tmpl.data(), h.data(), flags, p.data(), uint32_t(c.table.size()), m.data(),
                     uint32_t(c.pm4.size()));
}

void TestValidate() {
  for (const Case& c : {Depth(), Scene()}) {
    CHECK(fl::Validate(c.tmpl.data(), c.tmpl.size()));
    CHECK(!fl::Validate(c.tmpl.data(), c.tmpl.size() - 1));
    std::vector<uint32_t> t = c.tmpl;
    t[fl::kTMagic] ^= 1;
    CHECK(!fl::Validate(t.data(), t.size()));
    t = c.tmpl;
    t[fl::kTPoolWords] += 1000;  // a section past the end
    CHECK(!fl::Validate(t.data(), t.size()));
    t = c.tmpl;
    t[t[fl::kTOps] + 1] = t[t[fl::kTOps] + 3] = 0x7FFFFFFF;  // the first op's pool offset
    CHECK(!fl::Validate(t.data(), t.size()));
    t = c.tmpl;
    t[t[fl::kTSlots] + 1] = t[fl::kTListWords];  // a slot past the list
    CHECK(!fl::Validate(t.data(), t.size()));
    // Every single word changed: refused, or if taken, every reader stays inside (sanitizers).
    uint32_t taken = 0;
    for (size_t i = fl::kTHeaderWords; i < c.tmpl.size(); ++i) {
      for (uint32_t v : {0u, 1u, 0x7FFFFFFFu, 0xFFFFFFFFu, c.tmpl[i] + 1, c.tmpl[i] + 64}) {
        t = c.tmpl;
        t[i] = v;
        if (!fl::Validate(t.data(), t.size()))
          continue;
        ++taken;
        Case d = c;
        d.tmpl = t;
        if (d.pm4.size() < t[fl::kTListWords])
          continue;
        (void)Key(d, 0x86);
        for (uint64_t select : {0x80000003ull, 0xFFFFFFFFull})
          (void)Compare(d, select, 0xFFFFFFFF);
      }
    }
    CHECK(taken > 0);
  }
}

void TestKey() {
  for (Case c : {Depth(), Scene()}) {
    const fl::Key key = fl::TemplateKey(c.tmpl.data());
    const uint32_t files = c.header[fl::kHeaderFlags / 4];
    CHECK(Key(c, files) == key);
    // The loader's flags, and the texture fixups' 0x2000, in the files' form.
    const uint32_t live = (files & ~0x6u) | 0x101 | 0x2000;
    CHECK(Key(c, fl::FilesFormFlags(live)) == key);
    CHECK(Key(c, live) != key);
    FixUp(c, 3);
    CHECK(Key(c, files) == key);
    if (c.tmpl[fl::kTFlags] & fl::kTemplateTiled) {
      Case u = c;
      u.pm4 = Untile(c.pm4, c.tmpl);
      CHECK(Key(u, files) == key);
      CHECK(fl::Untiled(u.tmpl.data(), Guest(u.pm4).data()) &&
            !fl::Untiled(c.tmpl.data(), Guest(c.pm4).data()));
    }
    // A static word changed (the first register write's value), a shader's type bits, a mask, the
    // patch table.
    Case d = c;
    d.pm4[2] ^= 0x10;
    CHECK(Key(d, files) != key);
    d = c;
    d.header[fl::kHeaderMasks / 4 + 3] ^= 8;
    CHECK(Key(d, files) != key);
    d = c;
    d.table[0] ^= 0x40000000u;
    CHECK(Key(d, files) != key);
    d = c;
    const uint32_t* t = d.tmpl.data();
    const uint32_t* slot = t + t[fl::kTSlots];
    for (uint32_t s = 0; s < t[fl::kTSlotCount]; ++s, slot += fl::kSlotWords) {
      if ((slot[0] & 0xFF) == fl::kSlotPixelShader && !(slot[0] >> 8)) {
        d.pm4[slot[1]] ^= 2;
        break;
      }
    }
    CHECK(Key(d, files) != key);
  }
}

void TestRegistry() {
  fl::Registry reg;
  const Case depth = Depth(), scene = Scene();
  CHECK(reg.Define(1, depth.tmpl.data(), depth.tmpl.size()) == fl::DefineResult::kNew);
  CHECK(reg.Define(1, scene.tmpl.data(), scene.tmpl.size()) == fl::DefineResult::kRepeat);
  CHECK(reg.Define(2, scene.tmpl.data(), scene.tmpl.size() - 1) == fl::DefineResult::kInvalid);
  CHECK(!reg.FindTemplate(2));
  CHECK(reg.Define(2, scene.tmpl.data(), scene.tmpl.size()) == fl::DefineResult::kNew);
  CHECK(reg.FindTemplate(1)->words == depth.tmpl && reg.templates() == 2);

  fl::Binding b;
  b.phys = 0x17184D40;
  b.words = uint32_t(depth.pm4.size());
  b.template_id = 1;
  b.cb = 0x40001000;
  CHECK(reg.Bind(b) == fl::BindResult::kNew);
  CHECK(reg.Bind(b) == fl::BindResult::kSame);  // the packet replayed for another tile
  // Another pass group, or the retiled flag, is another bind.
  fl::Binding other = b;
  other.group = 12;
  CHECK(reg.Bind(other) == fl::BindResult::kReplaced && reg.Bind(other) == fl::BindResult::kSame);
  other.flags = 1;
  CHECK(reg.Bind(other) == fl::BindResult::kReplaced);
  CHECK(reg.Bind(b) == fl::BindResult::kReplaced && reg.bindings() == 1);
  fl::Skip skip;
  fl::Binding* found = reg.Lookup(0x17184D40, b.words, &skip);
  CHECK(found && found->template_id == 1 && skip == fl::Skip::kNone);
  found->pool_valid = true;
  // The same address run with another length: the address holds something else now.
  CHECK(!reg.Lookup(0x17184D40, b.words + 1, &skip) && skip == fl::Skip::kLength);
  CHECK(!reg.Lookup(0x17184D44, b.words, &skip) && skip == fl::Skip::kNone);
  // Bound again to another list (a reused command buffer): replaced, its pool forgotten.
  fl::Binding c = b;
  c.words = uint32_t(scene.pm4.size());
  c.template_id = 2;
  CHECK(reg.Bind(c) == fl::BindResult::kReplaced);
  found = reg.Lookup(0x17184D40, c.words, &skip);
  CHECK(found && found->template_id == 2 && !found->pool_valid);
  CHECK(!reg.Lookup(0x17184D40, b.words, &skip) && skip == fl::Skip::kLength);
  // Dropped at the unload, then the address bound to another list.
  CHECK(reg.Drop(0x17184D40) && !reg.Drop(0x17184D40));
  // A miss there is at a dropped list's address, not at a buffer that was never a list.
  CHECK(reg.WasBound(0x17184D40) && !reg.WasBound(0x17184D44));
  CHECK(!reg.Lookup(0x17184D40, c.words, &skip) && skip == fl::Skip::kNone);
  CHECK(reg.Bind(b) == fl::BindResult::kNew && reg.bindings() == 1);
  CHECK(reg.Lookup(0x17184D40, b.words, &skip)->template_id == 1);
}

// The lookup's key check: the template's slot table moved one dword on (a converter bug) shows at
// the first check after the bind; a static word the game writes after a run that matched is the
// other reason; a bind replaced starts again.
void TestKeyCheck() {
  for (Case c : {Depth(), Scene()}) {
    FixUp(c, 4);
    const uint32_t files = c.header[fl::kHeaderFlags / 4];
    const fl::Key key = fl::TemplateKey(c.tmpl.data());
    fl::Binding b;
    CHECK(fl::KeyCheck(&b, Key(c, files) != key) == fl::Skip::kNone && b.key_matched);
    // A static word the game rewrote (the second step's benchmark model): after a matching run.
    Case d = c;
    d.pm4[2] ^= 0x10;
    CHECK(fl::KeyCheck(&b, Key(d, files) != key) == fl::Skip::kStaticChanged);
    CHECK(fl::KeyCheck(&b, Key(c, files) != key) == fl::Skip::kNone);
    // A one-word slot whose list dword is one on (a word after it that is not a slot's): the key
    // differs from the first check of a fresh binding, so the reason names the template.
    Case w = c;
    uint32_t* t = w.tmpl.data();
    uint32_t* slot = t + t[fl::kTSlots];
    bool moved = false;
    for (uint32_t i = 0; i < t[fl::kTSlotCount] && !moved; ++i, slot += fl::kSlotWords) {
      const bool next_free = i + 1 == t[fl::kTSlotCount] || slot[fl::kSlotWords + 1] > slot[1] + 2;
      if (slot[3] == 1 && next_free && slot[1] + 1 < t[fl::kTListWords] &&
          w.pm4[slot[1] + 1] != 0) {
        ++slot[1];
        moved = true;
      }
    }
    CHECK(moved && fl::Validate(t, w.tmpl.size()));
    fl::Binding fresh;
    CHECK(fl::KeyCheck(&fresh, Key(w, files) != key) == fl::Skip::kKeyAtBind);
    CHECK(fl::KeyCheck(&fresh, Key(w, files) != key) == fl::Skip::kKeyAtBind);
    CHECK(!fresh.key_matched);
    // A bind replaced (a reused command buffer): the new binding has had no check yet.
    fl::Registry reg;
    CHECK(reg.Define(1, c.tmpl.data(), c.tmpl.size()) == fl::DefineResult::kNew);
    fl::Binding a;
    a.phys = 0x17184D40;
    a.words = uint32_t(c.pm4.size());
    a.template_id = 1;
    CHECK(reg.Bind(a) == fl::BindResult::kNew);
    fl::Skip skip;
    fl::Binding* found = reg.Lookup(a.phys, a.words, &skip);
    CHECK(fl::KeyCheck(found, false) == fl::Skip::kNone);
    CHECK(reg.Bind(a) == fl::BindResult::kSame && reg.Lookup(a.phys, a.words, &skip)->key_matched);
    a.cb = 0x40002000;
    CHECK(reg.Bind(a) == fl::BindResult::kReplaced);
    found = reg.Lookup(a.phys, a.words, &skip);
    CHECK(!found->key_matched && fl::KeyCheck(found, true) == fl::Skip::kKeyAtBind);
  }
}

// The tile op the scene list's draws use, and the first draw it is in.
const uint32_t* SceneTileOp(const std::vector<uint32_t>& tmpl) {
  const uint32_t* t = tmpl.data();
  for (uint32_t k = 0; k < t[fl::kTDrawCount]; ++k) {
    const uint32_t* r = fl::DrawRecord(t, k);
    if (r[fl::kDTile] != fl::kNone)
      return fl::Op(t, r[fl::kDTile]);
  }
  return nullptr;
}

// The registers written, whatever the values: faults the value compare cannot see because the
// register file the list starts with already holds what the template gets wrong.
void TestWrites() {
  Case scene = Scene();
  FixUp(scene, 5);
  const uint32_t* op = SceneTileOp(scene.tmpl);
  CHECK(op);
  const uint32_t tile_pool = op[1];
  // The register file at the start holds the first tile's window (D3D's segment set it).
  const auto window = [](const Case& c, const uint32_t* pool, std::vector<uint32_t>* regs) {
    const uint32_t* tile = pool + SceneTileOp(c.tmpl)[1];
    for (uint32_t i = 0; i < 3; ++i)
      (*regs)[fl::kRegWindowOffset + i] = tile[1 + i];
  };
  CHECK(Agree(Compare(scene, 0x80000003, 0xFFFFFFFF, Interpreter(), nullptr, window), 2));
  // The first tile's block with the second tile's mask: for the first tile's select no block of
  // the template applies, the packets write the window (the same values): only the writes differ.
  Case bad = scene;
  uint32_t* tile = bad.tmpl.data() + bad.tmpl[fl::kTPool] + tile_pool;
  tile[0] = tile[4];
  Result r = Compare(bad, 0x80000003, 0xFFFFFFFF, Interpreter(), nullptr, window);
  CHECK(!Agree(r, 2) && r.draw_fields == 1u << fl::kFieldWrites);
  // The same template is right by the writes for the second tile's select (both blocks apply,
  // the second's values last, as the packets leave them).
  CHECK(Agree(Compare(bad, 0xCull, 0xFFFFFFFF), 2));
  // A write the template puts on another register, where the register file already holds the
  // value on both: draw 0's first delta of one register moved to a register no packet writes.
  Case moved = scene;
  uint32_t* t = moved.tmpl.data();
  const uint32_t* r0 = fl::DrawRecord(t, 0);
  uint32_t* e = t + t[fl::kTDeltas] + r0[fl::kDDelta] * fl::kDeltaWords;
  uint32_t* target = nullptr;
  for (uint32_t i = 0; i < r0[fl::kDDeltaCount] && !target; ++i, e += fl::kDeltaWords) {
    const uint32_t* o = fl::Op(t, e[2]);
    if ((o[0] & 0xFF) == fl::kOpRegs && !(o[0] & fl::kOpTileBound) && e[0] >= 0x2000 &&
        e[0] < 0x2400 && !fl::PacketRegister(e[0]))
      target = e;
  }
  CHECK(target);
  static uint32_t from, to, count, value_at;
  from = target[0];
  to = 0x23F0;
  count = target[1];
  value_at = fl::Op(t, target[2])[3] + target[3];
  target[0] = to;
  CHECK(fl::Validate(t, moved.tmpl.size()));
  const auto both = [](const Case&, const uint32_t* pool, std::vector<uint32_t>* regs) {
    for (uint32_t i = 0; i < count; ++i)
      (*regs)[from + i] = (*regs)[to + i] = pool[value_at + i];
  };
  CHECK(Agree(Compare(scene, 0x80000003, 0xFFFFFFFF, Interpreter(), nullptr, both), 2));
  r = Compare(moved, 0x80000003, 0xFFFFFFFF, Interpreter(), nullptr, both);
  CHECK(r.draws_agree == 1 && r.draw_fields == 1u << fl::kFieldWrites && r.end_agree);
  // Untiled, the tile blocks are NOPs: no window write on either side.
  Case untiled = scene;
  untiled.pm4 = Untile(scene.pm4, scene.tmpl);
  CHECK(Agree(Compare(untiled, 0x3, 0xFFFFFFFF), 2));
  // The registers a draw packet writes are left out of the sets (their values are compared).
  CHECK(fl::PacketRegister(0x21F9) && fl::PacketRegister(0x21FC) && !fl::PacketRegister(0x21F8) &&
        !fl::PacketRegister(0x21FD));
}

void TestCompare() {
  Case depth = Depth(), scene = Scene();
  FixUp(depth, 1);
  FixUp(scene, 2);
  // The depth pass: not predicated, so whatever the bins.
  for (uint64_t select : {0xFFFFFFFFull, 0x80000003ull, 0x30ull}) {
    CHECK(Agree(Compare(depth, select, 0xFFFFFFFF), 1));
    CHECK(Agree(Compare(depth, select, 0x8), 1));
  }
  // The scene pass, tiled: each tile's select (the first tile's has bit 31), no tiling; the bin
  // mask the list starts with as D3D left it, and the ones seen (0x8, 0xA, 0x2).
  for (uint64_t select : {0x80000003ull, 0xCull, 0x30ull, 0xFFFFFFFFull}) {
    for (uint64_t mask : {0xFFFFFFFFull, 0x80000000ull, 0x8ull, 0xAull, 0x2ull}) {
      CHECK(Agree(Compare(scene, select, mask), 2));
    }
  }
  // A select outside every tile and the mask a tile block leaves: the windows are not written,
  // the draws are skipped, and the template says so.
  const Result outside = Compare(scene, 0x100000000ull, 0xFFFFFFFF);
  CHECK(Agree(outside, 2) && outside.skipped == 2);
  // Untiled (scene_msaa=2x|off, the 12- and 16-car races): the two-tile and one-pass selects.
  Case untiled = scene;
  untiled.pm4 = Untile(scene.pm4, scene.tmpl);
  for (uint64_t select : {0x3ull, 0xCull, 0xFFFFFFFFull}) {
    CHECK(Agree(Compare(untiled, select, 0xFFFFFFFF), 2));
  }
  // Faults: a register written the template does not know about.
  Interpreter extra;
  extra.extra_write_at_draw = 1;
  Result r = Compare(scene, 0x80000003, 0xFFFFFFFF, extra);
  CHECK(r.draws_agree == 1 && (r.draw_fields >> fl::kFieldState & 1) && !r.end_agree);
  // A stale pool: the slots read before a fixup changed them.
  Case before = scene;
  FixUp(before, 7);
  std::vector<uint32_t> stale(scene.tmpl[fl::kTPoolWords]);
  fl::Resolve(before.tmpl.data(), Guest(before.pm4).data(), stale.data());
  r = Compare(scene, 0x80000003, 0xFFFFFFFF, Interpreter(), &stale);
  CHECK(r.draws_agree == 0 && (r.draw_fields >> fl::kFieldShader & 1) &&
        (r.draw_fields >> fl::kFieldConstants & 1) && (r.draw_fields >> fl::kFieldFetch & 1));
  // A predicate that goes the other way.
  Interpreter wild;
  wild.ignore_predicate = true;
  r = Compare(scene, 0x100000000ull, 0xFFFFFFFF, wild);
  CHECK(r.draws_agree == 0 && (r.draw_fields >> fl::kFieldExecuted & 1));
  // A draw that is not the template's next one: that draw and every one after it, and the end.
  Interpreter order;
  order.swap_draws = 0;
  r = Compare(scene, 0x80000003, 0xFFFFFFFF, order);
  CHECK(r.draws_agree == 0 && (r.draw_fields >> fl::kFieldOrder & 1) &&
        (r.end_fields >> fl::kFieldOrder & 1));
  // A list run as tiled but with the template told untiled: the windows differ.
  {
    const uint32_t* t = scene.tmpl.data();
    const std::vector<uint8_t> pm4 = Guest(scene.pm4);
    std::vector<uint32_t> pool(t[fl::kTPoolWords]);
    fl::Resolve(t, pm4.data(), pool.data());
    Interpreter in;
    in.select = 0x80000003;
    fl::ShadowRun run;
    run.Begin(t, pool.data(), in.select, in.mask, true, in.regs.data(), in.shaders, Memory,
              nullptr);
    uint32_t agree = 0;
    in.Run(scene.pm4, [&](uint32_t packet, bool executed, uint32_t) {
      agree += run.Draw(packet, executed, in.regs.data(), in.mask, in.shaders).agree;
    });
    CHECK(agree == 0);
  }
  // The marks are left out of the end's compare (nothing reads them), nothing else is.
  CHECK(fl::MarkRegister(0x57E) && fl::MarkRegister(0x57F) && fl::MarkRegister(0x5002) &&
        !fl::MarkRegister(0x57D) && !fl::MarkRegister(0x2080));
}

}  // namespace

int main() {
  TestValidate();
  TestKey();
  TestRegistry();
  TestKeyCheck();
  TestCompare();
  TestWrites();
  std::printf("fh1 native list: all tests passed\n");
  return 0;
}
