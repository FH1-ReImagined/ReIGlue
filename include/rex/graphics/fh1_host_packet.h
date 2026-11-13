// FH1 host packets (patch 0610): a type-3 NOP whose first payload word is kMagic ("FH1N"),
// written into the guest's command buffer by a game hook. The command processor hands it to
// CommandProcessor::Fh1HostPacket in stream order (on the recorder when the command processor is
// split), so what it carries is seen with the register file exactly as the guest's packets
// before it left it. Any other NOP is skipped as before.
//
// The FH1 native renderer's milestone M2 ("shadow mode", FH1 docs/native-renderer.md) uses them
// to send the command processor the draw records its host device layer builds from the D3D
// device's shadow state at the D3D flush points, for one group of passes at a time; the command
// processor compares every record with its own register file (fh1_native_shadow.cpp). Nothing is
// drawn from them. Header-only, shared with the game.
#pragma once

#include <cstdint>

namespace rex::graphics::fh1_host {

constexpr uint32_t kMagic = 0x4648314Eu;  // "FH1N"
// The payload words one host packet may carry (the magic included).
constexpr uint32_t kMaxWords = 4096;

enum Op : uint32_t {
  // [2] island id, [3] the island's kind (kIslandShadowMask), [4] flags (kIslandCompareContents)
  kOpIslandBegin = 0x10,
  // A draw record (below).
  kOpRecord = 0x11,
  // [2] island id, [3] records written, [4] records not written (no room in the command
  // buffer), [5 + kind] flush-point calls of each Kind made in the island.
  kOpIslandEnd = 0x12,
  // ALU constants for the next record: [2] island id, [3] flags (kConstantsSeed: a seed starts
  // here, all 128 chunks follow in this and the next packets), [4] chunk count (at most
  // kChunksPerPacket), then per chunk its index (0-63 vertex, 64-127 pixel) and 16 words.
  kOpConstants = 0x13,
  // Host packets kept in guest memory, for the ones too large for the room the D3D device
  // guarantees in its command buffer (a few dozen words past its soft limit): [2] the guest
  // (virtual) address of a message, [3] its words, [4] its sequence number. The message: its
  // sequence number, its words (both included), then packets, each its word count and its
  // payload words (the magic first), in guest byte order.
  kOpRef = 0x14,
  // ALU constants a draw's shaders load from memory (their literal constants: D3D's
  // LOAD_ALU_CONSTANT of the shader object at the draw, which the device shadow does not hold),
  // for the next record: [2] island id, [3] the first word (from register 0x4000), [4] words,
  // then the words.
  kOpLiterals = 0x15,
};
constexpr uint32_t kLiteralsHeaderWords = 5;
constexpr uint32_t kRefMaxWords = 16384;
constexpr uint32_t kConstantsSeed = 1;
constexpr uint32_t kConstantsHeaderWords = 5;
constexpr uint32_t kChunksPerPacket = 32;

enum Island : uint32_t {
  // The sun's shadow mask: its clear, the cascade passes, their resolve, the blur and its
  // resolves, up to the world pass's resolve of the mask.
  kIslandShadowMask = 1,
};
constexpr uint32_t kIslandCompareContents = 1;  // compare the group's result with a native one

// The D3D flush point a record was made at.
enum Kind : uint32_t {
  kKindNone = 0,
  kKindDrawVertices = 1,      // DrawVertices (DRAW_INDX, auto index)
  kKindDrawIndexed = 2,       // DrawIndexedVertices (an index buffer)
  kKindBeginVertices = 3,     // BeginVertices: a Draw*UP whose vertices the caller copies
  kKindDrawVerticesUp = 4,    // DrawVerticesUP (BeginVertices and the copy)
  kKindResolve = 5,           // Resolve
  kKindClear = 6,             // the clear's rectangle draw (immediate shaders)
  kKindRunCommandBuffer = 7,  // RunCommandBuffer
  kKindDrawIndexedUp = 8,     // DrawIndexedVerticesUP (indices and vertices copied in)
  kKindCount = 9,
};

// A record's words: kRecordHeaderWords, then the register shadow, then the fetch constants, then
// (kRecordVertexData) the first kVertexDataWords of an UP draw's vertex data. Its ALU constants
// come just before it in kOpConstants packets: all of them in an island's first record (the
// seed), otherwise the chunks that were dirty at the call, as a native renderer uploads them.
// Shadow sections are copied from guest memory as they are (guest byte order, like every PM4
// word); everything else is a plain value.
enum RecordWord : uint32_t {
  kRecMagic = 0,
  kRecOp = 1,
  kRecIsland = 2,
  kRecIndex = 3,  // the record's number in its island
  kRecKind = 4,
  kRecPrimitive = 5,     // the call's primitive type (xenos::PrimitiveType), when it has one
  kRecCount = 6,         // the call's vertex or index count, when it has one
  kRecIndexAddress = 7,  // indexed draws: the physical address of their first index
  kRecVertexShader = 8,  // the device's vertex shader object (guest address)
  kRecPixelShader = 9,   // the device's pixel shader object (guest address)
  kRecFlags = 10,
  kRecVertexDirtyHigh = 11,  // the vertex ALU constants' dirty mask at the call (device +0)
  kRecVertexDirtyLow = 12,
  kRecPixelDirtyHigh = 13,  // the pixel ALU constants' dirty mask at the call (device +8)
  kRecPixelDirtyLow = 14,
  kRecConstantPackets = 15,  // the kOpConstants and kOpLiterals packets sent for this record
  kRecordHeaderWords = 16,
};
constexpr uint32_t kRecordSeed = 1;        // its constants packets held all 512 constants
constexpr uint32_t kRecordVertexData = 2;  // the UP draw's vertex data follows

// The D3D device's shadow of the registers it flushes with its register-block writer: device
// +10112 .. +10864, kRegisterShadowWords words. Each block is `count` consecutive registers
// from `first_register`, at word `shadow_word` of that range (the flush calls of
// DrawIndexedVertices, TU4 sub_82446330).
constexpr uint32_t kRegisterShadowOffset = 10112;
constexpr uint32_t kRegisterShadowWords = 188;
struct RegisterBlock {
  uint32_t first_register, shadow_word, count;
};
constexpr RegisterBlock kRegisterBlocks[] = {
    {0x4900, 0, 40},    // bool and loop constants
    {0x2000, 64, 16},   // RB_SURFACE_INFO .. PA_SC_SCREEN_SCISSOR_BR
    {0x2100, 83, 21},   // VGT_MAX_VTX_INDX .. PA_CL_VPORT_ZOFFSET
    {0x2180, 104, 5},   // SQ_PROGRAM_CNTL .. SQ_WRAPPING_1
    {0x2200, 109, 12},  // RB_DEPTHCONTROL .. RB_BLENDCONTROL3
    {0x2280, 121, 21},  // PA_SU_POINT_SIZE .. VGT_ENHANCE
    {0x2300, 142, 38},  // PA_SC_LINE_CNTL .. RB_COLOR_DEST_MASK (the copy registers among them)
    {0x2380, 180, 8},   // PA_SU_POLY_OFFSET_FRONT_SCALE .. PA_CL_POINT_CULL_RAD
};
// The fetch constants: device + 1152 + 24 n, 6 words each, registers 0x4800 + 6 n.
constexpr uint32_t kFetchOffset = 1152;
constexpr uint32_t kFetchWords = 32 * 6;
constexpr uint32_t kFetchFirstRegister = 0x4800;
// The ALU constants: vertex at device +1920, pixel at +6016, 256 float4 each, registers 0x4000
// (vertex) and 0x4400 (pixel). One dirty-mask bit covers a chunk of 4 constants (16 words): bit
// 63 the first.
constexpr uint32_t kAluOffset = 1920;
constexpr uint32_t kAluWords = 2048;
constexpr uint32_t kAluFirstRegister = 0x4000;
constexpr uint32_t kChunkWords = 16;
constexpr uint32_t kVertexDataWords = 20;

}  // namespace rex::graphics::fh1_host
