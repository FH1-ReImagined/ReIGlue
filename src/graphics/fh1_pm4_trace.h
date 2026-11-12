// FH1 PM4 trace (patch 0570): what the command processor executes, counted by the guest code
// that wrote it. Built only with -DREXGLUE_FH1_PM4_TRACE=ON (REX_FH1_PM4_TRACE=1); the call sites
// in command_processor.cpp are compiled out otherwise. A study tool for the native renderer
// (Forza Horizon 1: ReImagined's docs/native-renderer.md, "Tools"), not for players.
//
// Every packet the command processor reads (before the predicate skip, so packets predicated
// off count too) is counted by opcode against the tag of its first dword: the id of the game's
// D3D function that wrote it, from the shared segment the game's FORZA_D3D_TRACE build fills
// (fh1_pm4_trace_shm.h). Indirect buffers are classified by their dwords' tags: written by D3D
// (all tagged), mixed, or foreign (no tagged dword: command lists D3D did not write, such as the
// ones in model data); a foreign buffer with SET_CONSTANT (an opcode the game's D3D never
// writes) is its own class. Every foreign buffer is also counted against its parent, the tag of
// the INDIRECT_BUFFER packet that launched it.
//
// FH1_PM4_STATS=<file> turns it on; every FH1_PM4_STATS_EVERY swaps (default 300) a block of
// counts is appended to <file> and the counters restart (the return-address table is
// cumulative). Without the game's trace build every packet counts as untagged.
//
// Tags: 1-247 the game's ids, 0 an untagged dword inside a buffer that has tagged ones (a gap),
// 249 a foreign buffer with SET_CONSTANT, 250 another foreign buffer, 251 the primary ring,
// 252 a packet with no guest address.
#pragma once

#include <cstdint>

#if REX_FH1_PM4_TRACE

namespace rex::graphics::fh1_pm4_trace {

constexpr int kTagGap = 0, kTagForeignSetConstant = 249, kTagForeign = 250, kTagPrimary = 251,
              kTagHost = 252;

// Where the packets being read are: a physical base and the reader's capacity in bytes.
enum class Kind : uint8_t { kNone, kPrimary, kForeign, kWritten, kForeignSetConstant };
struct Context {
  uint32_t base = 0;
  uint32_t capacity = 0;
  Kind kind = Kind::kNone;
};
extern thread_local Context t_context;
extern thread_local uint32_t t_packet_start;  // reader offset of the packet being executed
extern bool g_enabled;

// Reads FH1_PM4_STATS once; physical_base is the guest's physical memory on the host.
bool Enabled(uint8_t* physical_base);

// The context of a buffer while its packets run; restores the outer one when it ends.
class Scope {
 public:
  Scope() : saved_(t_context) {}
  ~Scope() { t_context = saved_; }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Context saved_;
};
void BeginPrimary(uint32_t physical, uint32_t size);
void BeginIndirect(uint32_t physical, uint32_t count);  // classifies the buffer
void EndIndirect();
void BeginHost(uint32_t physical, uint32_t count);

inline bool Active(uint32_t reader_capacity) {
  return g_enabled && t_context.kind != Kind::kNone && reader_capacity == t_context.capacity;
}
void Padding(uint32_t start_offset, uint32_t dwords);
void Packet(uint32_t start_offset, uint32_t end_offset, uint32_t header);
void OnSwap();

}  // namespace rex::graphics::fh1_pm4_trace

#endif  // REX_FH1_PM4_TRACE
