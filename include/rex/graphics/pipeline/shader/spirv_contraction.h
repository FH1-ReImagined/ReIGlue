// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rex::graphics {
struct ColorArithmeticPolicy {
  bool requested;
  bool pixel;
  bool synthetic_depth;
  bool writes_depth;
  bool memory_export;
};

// Reconcile precision at the module boundary. The emitter always builds strict
// arithmetic; only a completed, eligible fragment module may lose the SPIR-V
// NoContraction annotation. No arithmetic operation or operand is changed.
inline bool RelaxColorContraction(std::vector<unsigned int>& words,
                                 ColorArithmeticPolicy policy) {
  if (!policy.requested || !policy.pixel || policy.synthetic_depth ||
      policy.writes_depth || policy.memory_export) return false;
  // SPIR-V's five-word header is followed by (word count, opcode) instructions.
  if (words.size() < 5 || words[0] != 0x07230203u) return false;
  bool fragment = false;
  for (size_t cursor = 5; cursor < words.size();) {
    const size_t length = words[cursor] >> 16;
    const uint32_t opcode = words[cursor] & 0xffff;
    if (!length || length > words.size() - cursor) return false;
    if (opcode == 15) {  // OpEntryPoint
      if (length < 4 || words[cursor + 1] != 4) return false;  // Fragment
      fragment = true;
    }
    if (opcode == 71 && length < 3) return false;  // OpDecorate
    cursor += length;
  }
  if (!fragment) return false;
  size_t destination = 5;
  bool changed = false;
  for (size_t source = 5; source < words.size();) {
    const size_t length = words[source] >> 16;
    const bool contraction = (words[source] & 0xffff) == 71 && length == 3 &&
                             words[source + 2] == 42;  // NoContraction
    if (contraction) {
      changed = true;
    } else {
      for (size_t i = 0; i < length; ++i) words[destination++] = words[source + i];
    }
    source += length;
  }
  words.resize(destination);
  return changed;
}
}  // namespace rex::graphics
