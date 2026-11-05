#pragma once
// FH1 (#384): the texture pack file formats: DDS reading and writing, the pack order, and the
// graphics memory policy of the replacements (#677). Nothing else of the SDK, so
// tools/test_texture_pack_dds.cpp builds it alone.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rex::graphics::texture_pack {

// Host formats of dumped and replacement files.
enum class Format : uint8_t {
  kNone,
  kR8,
  kRG8,
  kRGBA8,
  kBGRA8,
  kBC1,
  kBC2,
  kBC3,
  kBC4,
  kBC5,
  kBC7,
};
bool IsBlockCompressed(Format format);
// Bytes of one 4x4 block (compressed formats) or one texel.
uint32_t ElementBytes(Format format);
uint64_t LevelBytes(Format format, uint32_t width, uint32_t height);
const char* FormatName(Format format);

// The largest replacement, either way.
constexpr uint32_t kMaxFileSize = 16384;

struct Level {
  uint64_t offset;
  uint64_t size;
  uint32_t width;
  uint32_t height;
};

// A replacement file, read and decoded on a worker; each level's rows packed, the levels starting
// 16-byte aligned in data.
struct Decoded {
  uint64_t fingerprint = 0;
  bool ok = false;
  Format format = Format::kNone;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<Level> levels;
  std::vector<uint8_t> data;
};

// A DDS file of the levels (rows packed, as written by EncodeDds).
std::vector<uint8_t> EncodeDds(Format format, uint32_t width, uint32_t height,
                               const std::vector<std::vector<uint8_t>>& levels);
// Reads a 2D DDS file: legacy headers (DXT1-5, ATI1/2, BC4U/BC5U, 32-bit RGB masks, luminance)
// and DX10 ones (BC1-5, BC7, RGBA8, BGRA8, R8, RG8). Uncompressed rows may be padded as the
// header's pitch says (see the code). false and a reason in error when it cannot be used.
bool DecodeDds(const uint8_t* data, size_t size, Decoded& out, std::string& error);

// texture_packs: the pack folders, in order ('|'-separated; a later one wins for the same
// fingerprint). "." is the files directly in the pack root (not its folders: the loose files
// of milestone 1's layout). A name that is not a plain folder name in the pack root ("..", a
// path, "dump" in any case) is left out and counted in refused; a name given twice keeps its
// first place; empty names are skipped ("|" alone: no packs).
std::vector<std::string> ParsePackOrder(const std::string& value, size_t& refused);

// A pack's pack.toml asks for its files to be read at start, as texture_replace_preload does
// for every pack (FH1 #659: a texture the game draws from once, as it loads, such as a car's
// decals baked into its paint job): a top-level `preload = true` (before any [table]; spaces
// and a # comment after it allowed). Anything else, `preload = "true"` included, is false.
bool ManifestPreload(std::string_view text);

// FH1 (#677): the graphics memory of the replacements. A replacement stays on the GPU while a
// texture the game has loaded has its picture, and kReleaseDelayMs after the last one went (the
// game's streaming often loads the same picture again right away); a preloaded one stays (it
// has no texture until the game loads it). They all fit in a limit; beyond it the game's own
// texture is drawn until there is room.
constexpr uint64_t kReleaseDelayMs = 5000;
// A replacement bound for a draw within this is on screen: never freed for the limit.
constexpr uint64_t kInUseMs = 2000;
// One not drawn for this long is freed too, held or not (the game keeps textures it no longer
// draws until its own cache is full): read again when it is drawn (as the game's
// texture_cache_memory_limit_soft_lifetime).
constexpr uint64_t kIdleMs = 30000;
// After the limit held one back: no new reads for this long, and the same one not before
// kRetryMs (each read is a whole file).
constexpr uint64_t kFullPauseMs = 1000;
constexpr uint64_t kRetryMs = 30000;

// The limit: limit_mb when set (texture_replace_memory_limit_mb), else 40% of the device-local
// memory (a discrete GPU's own), on an integrated GPU (its textures in system memory) a quarter
// of the system memory heap up to 2 GiB if that is more; at least 256 MiB.
uint64_t ReplacementMemoryLimit(uint32_t limit_mb, uint64_t device_local_bytes, bool integrated,
                                uint64_t system_heap_bytes);

// A replacement on the GPU, as the limit sees it.
struct ResidentReplacement {
  uint64_t fingerprint = 0;
  uint64_t bytes = 0;
  uint32_t refs = 0;          // loaded textures with its picture
  uint64_t last_used_ms = 0;  // bound for a draw
  uint64_t released_ms = 0;   // when refs reached 0
  bool pinned = false;        // preloaded
};
// What to free so that resident_bytes + incoming bytes fit under limit, freeing down to target
// (<= limit, so the next ones fit without another search): first the ones no texture has (the
// longest gone first), then the ones not bound for kInUseMs (the least recently bound first).
// Preloaded ones and those on screen are kept. fits: whether the incoming one fits after that.
std::vector<uint64_t> ChooseEvictions(std::vector<ResidentReplacement> resident,
                                      uint64_t resident_bytes, uint64_t incoming, uint64_t limit,
                                      uint64_t target, uint64_t now_ms, bool& fits);

}  // namespace rex::graphics::texture_pack
