// Interrupts-off sections that are one atomic read-modify-write (ReIGlue 0470, FH1 #506).
// See reservation_sections.h.
#include "reservation_sections.h"

#include <set>

namespace rex::codegen {

namespace {

// A path from the section's entry to a closing mtmsrd longer than this, or more
// paths than this, keeps the lock (FH1's longest is 4 instructions, 2 paths).
constexpr uint32_t kMaxPathInstructions = 32;
constexpr uint32_t kMaxPaths = 8;

uint32_t Op(uint32_t w) {
  return w >> 26;
}
uint32_t Xo10(uint32_t w) {
  return (w >> 1) & 0x3FF;
}
uint32_t Xo9(uint32_t w) {
  return (w >> 1) & 0x1FF;
}
uint32_t RT(uint32_t w) {
  return (w >> 21) & 31;
}
uint32_t RA(uint32_t w) {
  return (w >> 16) & 31;
}
uint32_t RB(uint32_t w) {
  return (w >> 11) & 31;
}

bool IsMtmsrd(uint32_t w) {
  return Op(w) == 31 && Xo10(w) == 178;
}

bool IsLoadReserve(uint32_t w, uint32_t& width) {
  if (Op(w) != 31)
    return false;
  if (Xo10(w) == 20)  // lwarx
    width = 4;
  else if (Xo10(w) == 84)  // ldarx
    width = 8;
  else
    return false;
  return true;
}

bool IsStoreConditional(uint32_t w, uint32_t& width) {
  if (Op(w) != 31 || !(w & 1))
    return false;
  if (Xo10(w) == 150)  // stwcx.
    width = 4;
  else if (Xo10(w) == 214)  // stdcx.
    width = 8;
  else
    return false;
  return true;
}

// An integer instruction that only reads and writes registers (GPRs, CR, XER):
// sets dest to the GPR it writes, or -1. False for anything else.
bool IsRegisterOnly(uint32_t w, int& dest) {
  switch (Op(w)) {
    case 7:   // mulli
    case 8:   // subfic
    case 12:  // addic
    case 13:  // addic.
    case 14:  // addi, li
    case 15:  // addis, lis
      dest = int(RT(w));
      return true;
    case 10:  // cmpli
    case 11:  // cmpi
      dest = -1;
      return true;
    case 20:  // rlwimi
    case 21:  // rlwinm
    case 23:  // rlwnm
    case 24:  // ori, nop
    case 25:  // oris
    case 26:  // xori
    case 27:  // xoris
    case 28:  // andi.
    case 29:  // andis.
    case 30:  // rldicl, rldicr, rldic, rldimi, rldcl, rldcr
      dest = int(RA(w));
      return true;
    case 31:
      switch (Xo10(w)) {
        case 0:   // cmp
        case 32:  // cmpl
          dest = -1;
          return true;
        case 24:   // slw
        case 26:   // cntlzw
        case 27:   // sld
        case 28:   // and
        case 58:   // cntlzd
        case 60:   // andc
        case 124:  // nor
        case 284:  // eqv
        case 316:  // xor
        case 412:  // orc
        case 444:  // or, mr
        case 476:  // nand
        case 536:  // srw
        case 539:  // srd
        case 792:  // sraw
        case 794:  // srad
        case 824:  // srawi
        case 922:  // extsh
        case 954:  // extsb
        case 986:  // extsw
          dest = int(RA(w));
          return true;
        default:
          break;
      }
      if (((w >> 2) & 0x1FF) == 413) {  // sradi
        dest = int(RA(w));
        return true;
      }
      // XO forms (the OE bit is the top bit of the 10-bit field).
      switch (Xo9(w)) {
        case 8:    // subfc
        case 9:    // mulhdu
        case 10:   // addc
        case 11:   // mulhwu
        case 40:   // subf
        case 73:   // mulhd
        case 75:   // mulhw
        case 104:  // neg
        case 136:  // subfe
        case 138:  // adde
        case 200:  // subfze
        case 202:  // addze
        case 232:  // subfme
        case 233:  // mulld
        case 234:  // addme
        case 235:  // mullw
        case 266:  // add
        case 457:  // divdu
        case 459:  // divwu
        case 489:  // divd
        case 491:  // divw
          dest = int(RT(w));
          return true;
        default:
          return false;
      }
    default:
      return false;
  }
}

int32_t BranchOffset(uint32_t w) {
  if (Op(w) == 18) {
    int32_t li = int32_t(w & 0x03FFFFFC);
    return (li & 0x02000000) ? li - 0x04000000 : li;
  }
  int32_t bd = int32_t(w & 0xFFFC);
  return (bd & 0x8000) ? bd - 0x10000 : bd;
}

// Whether execution can continue at the next address.
bool FallsThrough(uint32_t w) {
  const bool link = w & 1;
  if (Op(w) == 18)
    return link;  // b never, bl returns
  if (Op(w) == 16)
    return link || (RT(w) & 0x14) != 0x14;               // conditional, or bcl
  if (Op(w) == 19 && (Xo10(w) == 16 || Xo10(w) == 528))  // bclr, bcctr
    return link || (RT(w) & 0x14) != 0x14;
  return true;
}

struct PathState {
  uint32_t pc;
  uint32_t steps = 0;
  bool reserved = false;
  bool conditional = false;
  uint32_t width = 0;
  uint32_t ra = 0;
  uint32_t rb = 0;
  uint32_t written = 0;  // GPRs written since the load-reserve
};

// Every path from the mtmsrd r13 at `enter`: true if each is one load-reserve and
// at most one matching store-conditional between register-only instructions,
// ending at a closing mtmsrd. Fills the instructions visited and the closing mtmsrds.
bool WalkSection(const ReservationCode& code, uint32_t enter, std::set<uint32_t>& visited,
                 std::set<uint32_t>& leaves) {
  std::vector<PathState> work{PathState{enter + 4}};
  uint32_t paths = 1;
  while (!work.empty()) {
    PathState s = work.back();
    work.pop_back();
    for (;;) {
      if (++s.steps > kMaxPathInstructions)
        return false;
      auto it = code.words.find(s.pc);
      if (it == code.words.end())
        return false;  // leaves the function
      const uint32_t w = it->second;
      visited.insert(s.pc);

      uint32_t width = 0;
      if (IsMtmsrd(w)) {
        // A path may close without its store-conditional (a compare-exchange
        // whose compare failed): it only loaded the word.
        if (RT(w) == 13 || !s.reserved)
          return false;  // nested, or no load-reserve
        leaves.insert(s.pc);
        break;
      }
      if (IsLoadReserve(w, width)) {
        if (s.reserved)
          return false;
        s.reserved = true;
        s.width = width;
        s.ra = RA(w);
        s.rb = RB(w);
        s.written = 1u << RT(w);
        s.pc += 4;
        continue;
      }
      if (IsStoreConditional(w, width)) {
        if (!s.reserved || s.conditional || width != s.width || RA(w) != s.ra || RB(w) != s.rb)
          return false;
        const uint32_t address_regs = (s.ra ? 1u << s.ra : 0u) | (1u << s.rb);
        if (s.written & address_regs)
          return false;
        s.conditional = true;
        s.pc += 4;
        continue;
      }
      if (Op(w) == 18 || Op(w) == 16) {
        if (w & 3)
          return false;  // calls and absolute branches
        if (Op(w) == 16 && !(RT(w) & 0x04))
          return false;  // decrements CTR
        const uint32_t target = s.pc + uint32_t(BranchOffset(w));
        if (target <= s.pc)
          return false;  // a loop inside the section
        if (Op(w) == 18 || (RT(w) & 0x14) == 0x14) {
          s.pc = target;
          continue;
        }
        if (++paths > kMaxPaths)
          return false;
        PathState taken = s;
        taken.pc = target;
        work.push_back(taken);
        s.pc += 4;
        continue;
      }
      int dest = -1;
      if (!IsRegisterOnly(w, dest))
        return false;  // memory access, barrier, SPR, trap, indirect branch, ...
      if (dest >= 0 && s.reserved)
        s.written |= 1u << dest;
      s.pc += 4;
    }
  }
  return !leaves.empty();
}

}  // namespace

ReservationSections FindReservationSections(const ReservationCode& code) {
  ReservationSections result;

  // Direct branch sources of each target in the function.
  std::unordered_map<uint32_t, std::vector<uint32_t>> sources;
  std::vector<uint32_t> enters;
  for (const auto& [address, w] : code.words) {
    if (Op(w) == 18 || Op(w) == 16) {
      if (!(w & 2))
        sources[address + uint32_t(BranchOffset(w))].push_back(address);
    } else if (IsMtmsrd(w) && RT(w) == 13) {
      enters.push_back(address);
    }
  }
  result.sections = uint32_t(enters.size());
  if (code.unknownIndirectTargets)
    return result;

  for (uint32_t enter : enters) {
    if (code.hookSites.contains(enter))
      continue;
    std::set<uint32_t> visited, leaves;
    if (!WalkSection(code, enter, visited, leaves))
      continue;
    bool closed = true;
    for (uint32_t address : visited) {
      if (code.indirectTargets.contains(address) || code.hookSites.contains(address)) {
        closed = false;
        break;
      }
      auto from = sources.find(address);
      if (from != sources.end()) {
        for (uint32_t source : from->second) {
          if (!visited.contains(source)) {
            closed = false;
            break;
          }
        }
      }
      const uint32_t previous = address - 4;
      if (previous != enter && !visited.contains(previous)) {
        auto it = code.words.find(previous);
        if (it != code.words.end() && FallsThrough(it->second))
          closed = false;
      }
      if (!closed)
        break;
    }
    if (!closed)
      continue;
    result.enters.insert(enter);
    for (uint32_t address : visited) {
      if (leaves.contains(address))
        result.leaves.insert(address);
      else
        result.body.insert(address);
    }
  }
  return result;
}

ReservationSectionTotals& ReservationSectionCounts() {
  static ReservationSectionTotals totals;
  return totals;
}

}  // namespace rex::codegen
