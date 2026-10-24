// FH1 (#378): where the command processor is in the commands it executes, for the pipeline
// lookahead (vulkan/fh1_pipeline_lookahead.cpp): the ring buffer and the indirect buffers being
// executed, outermost first, and the ring position in dwords since the start, which only grows.
// When a draw runs, each of these readers is at the end of its current packet (the draw, or the
// INDIRECT_BUFFER that called the next one). One command processor per process; its thread alone
// changes this.
#pragma once

#include <cstdint>

#include <rex/memory/ring_buffer.h>

namespace rex::graphics::fh1_command_cursor {

constexpr uint32_t kMaxBuffers = 8;

struct Cursor {
  const memory::RingBuffer* buffers[kMaxBuffers] = {};
  uint32_t depth = 0;
  // The ring's reader while ExecutePrimaryBuffer runs (buffers[0] then), else nullptr.
  const memory::RingBuffer* ring = nullptr;
  // Ring dwords executed by the finished ExecutePrimaryBuffer calls, and the ring index the
  // current one started at.
  uint64_t ring_done = 0;
  uint32_t ring_start = 0;
};

inline Cursor cursor;

// For the time a buffer is executed.
class Scope {
 public:
  explicit Scope(const memory::RingBuffer* buffer) {
    if (cursor.depth < kMaxBuffers) cursor.buffers[cursor.depth] = buffer;
    ++cursor.depth;
  }
  ~Scope() { --cursor.depth; }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

}  // namespace rex::graphics::fh1_command_cursor
