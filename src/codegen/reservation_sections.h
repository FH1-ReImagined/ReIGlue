// Interrupts-off sections that are one atomic read-modify-write, run without the global lock
// (ReIGlue 0470, FH1 #506; the idea from Pinyon Shift's ShiftGlue 07b2b2a, see NOTICE).
#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rex::codegen {

/**
 * Xbox 360 code wraps each lwarx/stwcx. (ldarx/stdcx.) loop in an
 * interrupts-off section: `mtmsrd r13,1` before the load-reserve, `mtmsrd rN,1`
 * (rN from an earlier mfmsr) after the store-conditional. The generated code
 * holds the global critical region for such a section.
 *
 * A section is lock-free when, on every path from its mtmsrd r13 to a closing
 * mtmsrd, it is one load-reserve and at most one store-conditional (none on a
 * compare-exchange's failed path) of the same width to the same address
 * registers, which nothing between them writes, with only
 * register arithmetic, compares and forward branches around them: no other
 * load or store, call, return, indirect branch, trap, barrier or nested
 * section. The new value then depends only on the reserved word and on
 * registers, and the store-conditional is a host compare-exchange on that
 * word, so the section is an atomic read-modify-write without the lock: as on
 * the console, where the section keeps out only this CPU's interrupts.
 *
 * Its instructions must also be reachable only from its own entry: no branch
 * from elsewhere, jump-table entry or mid-asm hook may land inside it, and no
 * outside instruction may fall into it (a shared closing mtmsrd stays locked).
 */
struct ReservationSections {
  /// mtmsrd r13 that opens a lock-free section.
  std::unordered_set<uint32_t> enters;
  /// mtmsrd that closes one.
  std::unordered_set<uint32_t> leaves;
  /// Every instruction between (load-reserve and store-conditional included).
  std::unordered_set<uint32_t> body;
  /// Sections (mtmsrd r13) in the function, lock-free or not.
  uint32_t sections = 0;

  bool isLockFreeEnter(uint32_t address) const { return enters.contains(address); }
  bool isLockFreeLeave(uint32_t address) const { return leaves.contains(address); }
  bool inLockFreeSection(uint32_t address) const { return body.contains(address); }
};

/// One function's code, as the analysis needs it.
struct ReservationCode {
  /// Every instruction of the function: address -> big-endian-decoded word.
  std::unordered_map<uint32_t, uint32_t> words;
  /// Addresses reached other than by a direct branch: switch tables, mid-asm
  /// hook jumps, known jump tables.
  std::unordered_set<uint32_t> indirectTargets;
  /// Addresses with a mid-asm hook.
  std::unordered_set<uint32_t> hookSites;
  /// The function has a switch-like bctr whose targets are not known yet:
  /// nothing in it is converted.
  bool unknownIndirectTargets = false;
};

/// Finds the lock-free sections of one function.
ReservationSections FindReservationSections(const ReservationCode& code);

/// Totals over a codegen run (for its log line).
struct ReservationSectionTotals {
  uint64_t sections = 0;
  uint64_t lockFree = 0;
};
ReservationSectionTotals& ReservationSectionCounts();

}  // namespace rex::codegen
