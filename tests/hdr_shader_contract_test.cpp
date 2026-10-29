#include <cassert>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include "../src/graphics/shaders/vulkan_spirv/hdr_hud_mask_cs.h"
#include "../src/ui/shaders/vulkan_spirv/hdr_output_ps.h"
#include "../src/ui/shaders/vulkan_spirv/hdr_ui_mask_ps.h"
#include <rex/ui/hdr_parameters.h>

// Inspect the actual shipped SPIR-V, independently of its GLSL declarations.
// This catches stale generated binaries and CPU/shader layout disagreement.
struct Contract {
  std::set<unsigned> bindings;
  std::vector<unsigned> offsets;
  unsigned push_end = 0, stage = ~0u, x = 0, y = 0, z = 0;
};
template <size_t Count>
Contract Inspect(const uint32_t (&words)[Count]) {
  assert(Count >= 5 && words[0] == 0x07230203);
  std::map<unsigned, unsigned> binding, set, pointer, sizes;
  std::map<unsigned, std::vector<unsigned>> members;
  std::map<std::pair<unsigned, unsigned>, unsigned> offset;
  unsigned push_pointer = 0;
  Contract result;
  for (size_t at = 5; at < Count;) {
    const auto length = words[at] >> 16, opcode = words[at] & 0xffff;
    assert(length && length <= Count - at);
    const auto* operand = words + at + 1;
    switch (opcode) {
      case 15: result.stage = operand[0]; break;  // OpEntryPoint
      case 16:  // OpExecutionMode, LocalSize
        if (operand[1] == 17) { result.x=operand[2]; result.y=operand[3]; result.z=operand[4]; }
        break;
      case 20: sizes[operand[0]] = 4; break;  // Logical bool vectors are not push constants.
      case 21: case 22: sizes[operand[0]] = operand[1] / 8; break;
      case 23: sizes[operand[0]] = sizes.at(operand[1]) * operand[2]; break;
      case 30: members[operand[0]] = {operand + 1, operand + length - 1}; break;
      case 32: pointer[operand[0]] = operand[2]; break;
      case 59: if (operand[2] == 9) push_pointer = operand[0]; break;
      case 71:
        if (operand[1] == 33) binding[operand[0]] = operand[2];
        if (operand[1] == 34) set[operand[0]] = operand[2];
        break;
      case 72:
        if (operand[2] == 35) offset[{operand[0],operand[1]}] = operand[3];
        break;
    }
    at += length;
  }
  for (auto [id, number] : binding) { assert(set.at(id) == 0); result.bindings.insert(number); }
  const auto structure = pointer.at(push_pointer);
  const auto& fields = members.at(structure);
  for (unsigned i=0; i<fields.size(); ++i) {
    const auto start = offset.at({structure,i});
    result.offsets.push_back(start);
    result.push_end = start + sizes.at(fields[i]);
  }
  return result;
}
int main() {
  auto mask = Inspect(hdr_hud_mask_cs);
  assert(mask.stage == 5 && mask.x == 16 && mask.y == 8 && mask.z == 1);
  assert(mask.bindings == (std::set<unsigned>{0,1,2}));
  assert(mask.offsets == (std::vector<unsigned>{0,8,12}) && mask.push_end == 16);
  auto output = Inspect(hdr_output_ps);
  assert(output.stage == 4 && output.bindings == (std::set<unsigned>{0}));
  assert(output.offsets == (std::vector<unsigned>{16,20,24,28,32,36,40,44,48,52,56}));
  assert(output.push_end == 16 + sizeof(rex::ui::HdrParameters));
  auto coverage = Inspect(hdr_ui_mask_ps);
  assert(coverage.stage == 4 && coverage.bindings == (std::set<unsigned>{0,1}));
  assert(coverage.offsets == (std::vector<unsigned>{16,24}) && coverage.push_end == 32);
}
