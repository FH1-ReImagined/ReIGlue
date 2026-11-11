// The microcode disassembler and analysis with values that have no name (FH1 #808): a texture
// fetch whose opcode the GPU does not define, an anisotropic filter of 6, a vertex format with no
// name and an exec past the end of the microcode. Each disassembled as a number (a null name was
// appended before, and the game stopped in strlen), the fetch parsed as one that does nothing.
// Built from the plugin's parser and disassembler sources, no GPU.
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/string/buffer.h>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define CHECK(x)                                                                 \
  do {                                                                           \
    if (!(x)) {                                                                  \
      std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #x); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

using namespace rex::graphics;

namespace {

bool Defined(uint32_t opcode) {
  return opcode == 1 || (opcode >= 16 && opcode <= 19) || (opcode >= 24 && opcode <= 26);
}

template <typename T>
T FromDwords(const uint32_t (&dwords)[3]) {
  T op;
  static_assert(sizeof(T) == sizeof(dwords));
  std::memcpy(&op, dwords, sizeof(op));
  return op;
}

// tfetch2D r1, r0.xy, tf3 with the given opcode and aniso filter (7: the fetch constant's).
void TextureFetchDwords(uint32_t opcode, uint32_t aniso, uint32_t (&dwords)[3]) {
  dwords[0] = opcode | (0u << 5) | (1u << 12) | (3u << 20) | (0x24u << 26);  // src .xyz
  // dst swizzle xyzw, the other filters the fetch constant's (3), computed LOD.
  dwords[1] = 0x688u | (3u << 12) | (3u << 14) | (3u << 16) | (aniso << 18) | (3u << 24) |
              (3u << 26) | (1u << 28);
  dwords[2] = 1u << 14;  // 2D
}

std::string DisassembleTextureFetch(uint32_t opcode, uint32_t aniso, bool* known) {
  uint32_t dwords[3];
  TextureFetchDwords(opcode, aniso, dwords);
  ParsedTextureFetchInstruction instr;
  *known = ParseTextureFetchInstruction(FromDwords<ucode::TextureFetchInstruction>(dwords), instr);
  rex::string::StringBuffer out;
  instr.Disassemble(&out);
  return out.to_string();
}

// Two control flow instructions in the first 3 dwords: exec_end at address 1 with count
// instructions (sequence: all fetches), then a nop.
void ExecEnd(uint32_t count, uint32_t* dwords) {
  uint32_t sequence = 0;
  for (uint32_t i = 0; i < count; ++i) {
    sequence |= 1u << (2 * i);
  }
  dwords[0] = 1u | (count << 12) | (sequence << 16);
  dwords[1] = uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12;
  dwords[2] = 0;
}

}  // namespace

int main() {
  // Every opcode other than the vertex fetch: the defined ones parse with their names, the others
  // as tfetch_opcode_N with no result and no operands.
  for (uint32_t opcode = 1; opcode < 32; ++opcode) {
    bool known = false;
    const std::string text = DisassembleTextureFetch(opcode, 7, &known);
    CHECK(known == Defined(opcode));
    if (!known) {
      CHECK(text.find("tfetch_opcode_" + std::to_string(opcode) + "\n") != std::string::npos);
    } else {
      CHECK(text.find("tfetch_opcode_") == std::string::npos);
    }
  }
  {
    bool known = false;
    const std::string text = DisassembleTextureFetch(1, 7, &known);
    CHECK(known && text.find("tfetch2D r1") != std::string::npos &&
          text.find(", tf3") != std::string::npos);
    CHECK(text.find("AnisoFilter") == std::string::npos);
  }
  // Anisotropic filters: 0-5 named, 6 has no name.
  {
    bool known = false;
    CHECK(DisassembleTextureFetch(1, 5, &known).find(", AnisoFilter=max16to1") != std::string::npos);
    CHECK(DisassembleTextureFetch(1, 6, &known).find(", AnisoFilter=6") != std::string::npos);
  }

  // A full vertex fetch with each data format: the named ones by name, the rest as numbers.
  for (uint32_t format = 1; format < 64; ++format) {
    uint32_t dwords[3];
    dwords[0] = 0u | (0u << 5) | (1u << 12) | (1u << 19) | (2u << 20);
    dwords[1] = 0x688u | (format << 16);
    dwords[2] = 4;  // stride
    const auto op = FromDwords<ucode::VertexFetchInstruction>(dwords);
    ucode::VertexFetchInstruction previous = op;
    ParsedVertexFetchInstruction instr;
    ParseVertexFetchInstruction(op, previous, instr);
    rex::string::StringBuffer out;
    instr.Disassemble(&out);
    const std::string text = out.to_string();
    if (format == 6) {
      CHECK(text.find(", DataFormat=FMT_8_8_8_8") != std::string::npos);
    } else if (format == 1 || format == 63) {
      CHECK(text.find(", DataFormat=" + std::to_string(format) + ",") != std::string::npos);
    }
  }

  // A pixel shader whose only fetch has opcode 27: analysed without a texture binding.
  {
    uint32_t ucode[6];
    ExecEnd(1, ucode);
    uint32_t fetch[3];
    TextureFetchDwords(27, 7, fetch);
    std::memcpy(ucode + 3, fetch, sizeof(fetch));
    Shader shader(xenos::ShaderType::kPixel, 0x808, ucode, 6, std::endian::native);
    rex::string::StringBuffer buffer;
    shader.AnalyzeUcode(buffer);
    CHECK(shader.is_ucode_analyzed());
    CHECK(shader.texture_bindings().empty());
    CHECK(shader.ucode_disassembly().find("tfetch_opcode_27") != std::string::npos);
  }
  // An exec whose second instruction is past the end of the microcode: the first is analysed, the
  // second is not read.
  {
    uint32_t ucode[6];
    ExecEnd(2, ucode);
    uint32_t fetch[3];
    TextureFetchDwords(1, 7, fetch);
    std::memcpy(ucode + 3, fetch, sizeof(fetch));
    Shader shader(xenos::ShaderType::kPixel, 0x809, ucode, 6, std::endian::native);
    rex::string::StringBuffer buffer;
    shader.AnalyzeUcode(buffer);
    CHECK(shader.texture_bindings().size() == 1);
    CHECK(shader.texture_bindings()[0].fetch_constant == 3);
    CHECK(shader.ucode_disassembly().find("past the end of the microcode") != std::string::npos);
  }

  std::puts("shader disassembly: ok");
  return 0;
}
