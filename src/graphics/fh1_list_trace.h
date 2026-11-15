// FH1 list trace (patch 0640): where the command processor's time goes in the command lists the
// game runs with RunCommandBuffer, for the native renderer's model list work (Forza Horizon 1:
// ReImagined's docs/native-renderer.md, "M4"). Built only with -DREXGLUE_FH1_LIST_TRACE=ON
// (REX_FH1_LIST_TRACE=1); the call sites in command_processor.cpp are compiled out otherwise. A
// measurement tool, not for players.
//
// FH1_LIST_STATS=<file> turns it on. The game's FORZA_LIST_TRACE build registers every buffer a
// RunCommandBuffer call names in a shared segment (fh1_list_trace_shm.h), with its class (a
// model's list from the files, a command buffer D3D recorded, type 10, other) and the pass group
// the game was drawing. For every indirect buffer it runs, the command processor counts by class,
// pass group and depth: runs, dwords, the time spent decoding it (the time stamp counter around
// ExecuteIndirectBuffer; runs in which the decoder synchronised with the recorder are counted
// apart, since their time includes the wait), the register writes it read and the ones it handed
// to the recorder (the rest were unchanged and elided), and the bin select and mask it started
// with. Also counted: INDIRECT_BUFFER packets predicated off, by the buffer they name; packets
// that read a register (WAIT_REG_MEM, REG_RMW, COND_WRITE, REG_TO_MEM) by register; SCRATCH_UMSK
// when a list starts. Every FH1_LIST_STATS_EVERY swaps (default 600) a block of counts is
// appended to <file>, with the game's counters as deltas, and <file>.lists is rewritten with
// every registered buffer's totals. Without the game's trace build every buffer is unregistered.
#pragma once

#include <cstdint>

#if REX_FH1_LIST_TRACE

namespace rex::graphics::fh1_list_trace {

extern bool g_on;

// Reads FH1_LIST_STATS once; physical_base is the guest's physical memory on the host.
bool Enabled(uint8_t* physical_base);

// The command processor's counters a buffer's scope reads when it starts and when it ends.
struct Sources {
  const uint64_t* registers_in;   // register writes the packets made
  const uint64_t* registers_out;  // those handed to the recorder (not elided)
  const uint64_t* syncs;          // the decoder's waits for the recorder, by reason
  uint32_t sync_reasons;
  const uint64_t* bin_select;
  const uint64_t* bin_mask;
  const uint32_t* registers;  // the decoder's register file
};

// The primary ring between two read offsets.
class PrimaryScope {
 public:
  explicit PrimaryScope(uint32_t dwords);
  ~PrimaryScope();
  PrimaryScope(const PrimaryScope&) = delete;
  PrimaryScope& operator=(const PrimaryScope&) = delete;

 private:
  uint64_t start_ = 0, cpu_start_ = 0;
  bool on_ = false;
};

// One indirect buffer, while its packets run.
class IndirectScope {
 public:
  IndirectScope(uint32_t physical, uint32_t dwords, const Sources& sources);
  ~IndirectScope();
  IndirectScope(const IndirectScope&) = delete;
  IndirectScope& operator=(const IndirectScope&) = delete;

 private:
  const Sources* sources_ = nullptr;
  void* list_ = nullptr;
  uint32_t physical_ = 0, dwords_ = 0;
  uint64_t start_ = 0, children_ = 0, in_ = 0, out_ = 0, syncs_ = 0;
  IndirectScope* parent_ = nullptr;
  uint8_t cls_ = 0, group_ = 0, depth_ = 0;
};

// An INDIRECT_BUFFER packet predicated off.
void SkippedIndirect(uint32_t physical);

// Type-3 opcodes that read a register: WAIT_REG_MEM, REG_RMW, COND_WRITE, REG_TO_MEM.
inline bool ReadsRegister(uint32_t opcode) {
  return opcode == 0x3C || opcode == 0x21 || opcode == 0x45 || opcode == 0x3E;
}
// first: the packet's first payload words (at least two).
void RegisterRead(uint32_t opcode, const uint32_t* first);
// A guest CPU read of a GPU register (MMIO): the scratch registers, SCRATCH_UMSK / _ADDR and
// SHADER_CONSTANT_FLUSH_FETCH_0-2 by register, the rest together.
void CpuRegisterRead(uint32_t index);

void OnSwap();

}  // namespace rex::graphics::fh1_list_trace

#endif  // REX_FH1_LIST_TRACE
