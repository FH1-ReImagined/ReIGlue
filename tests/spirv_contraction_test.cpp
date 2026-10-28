#include <rex/graphics/pipeline/shader/spirv_contraction.h>
#include <cassert>
using namespace rex::graphics;
int main() {
  const ColorArithmeticPolicy color{true, true, false, false, false};
  const std::vector<unsigned int> module{
      0x07230203, 0x10000, 0, 32, 0,
      (4u << 16) | 15, 4, 1, 0,  // Fragment entry point.
      (3u << 16) | 71, 7, 42,    // NoContraction on id 7.
      (4u << 16) | 71, 8, 30, 1, // Location remains.
      (5u << 16) | 129, 2, 7, 4, 5}; // Arithmetic remains.
  auto output = module;
  assert(RelaxColorContraction(output, color));
  auto expected = module;
  expected.erase(expected.begin() + 9, expected.begin() + 12);
  assert(output == expected);
  assert(!RelaxColorContraction(output, color));
  assert(output == expected);
  for (auto policy : {ColorArithmeticPolicy{false,true,false,false,false},
                     ColorArithmeticPolicy{true,false,false,false,false},
                     ColorArithmeticPolicy{true,true,true,false,false},
                     ColorArithmeticPolicy{true,true,false,true,false},
                     ColorArithmeticPolicy{true,true,false,false,true}}) {
    output = module;
    assert(!RelaxColorContraction(output, policy));
    assert(output == module);
  }
  // Validate the whole module before making any edits, including a malformed
  // instruction after a removable decoration.
  for (unsigned int invalid : {0u, (65535u << 16) | 129}) {
    output = module;
    output.back() = invalid;
    output.push_back(invalid);
    const auto before = output;
    assert(!RelaxColorContraction(output, color));
    assert(output == before);
  }
  output = module;
  output[6] = 0; // Vertex entry point wins over a mistaken caller policy.
  const auto vertex = output;
  assert(!RelaxColorContraction(output, color));
  assert(output == vertex);
  output = {0x07230203};
  assert(!RelaxColorContraction(output, color));
}
