#pragma once
// FH1 (#384): texture packs. The texture cache fingerprints a guest texture when it loads its
// data (texture_dump, texture_replace); with texture_dump, each new texture is written to
// <texture_pack_root>/dump/<fingerprint>.dds (and .png) on a worker thread; with
// texture_replace, a file <texture_pack_root>/<fingerprint>.dds or .png is read and decoded on
// another worker, and the backend (Vulkan) samples it instead of the guest texture.
//
// The fingerprint covers what defines the texture's picture, not where it is: the base level's
// visible blocks as the game wrote them (big-endian), its format, size, pitch, tiling and
// endianness. Guest addresses change between runs and are left out; so is the mip chain (games
// bind the same picture with different mip counts).
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <rex/graphics/pipeline/texture/pack_format.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::texture_pack {

// The file format a guest format is dumped as (kNone: not dumped or replaced yet).
Format GuestFormatFile(xenos::TextureFormat format);

// The fingerprinted fields of a guest texture (a TextureKey without its addresses).
struct GuestTexture {
  xenos::TextureFormat format;
  xenos::Endian endianness;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;  // the fetch constant's (texels / 32)
  uint32_t mip_levels;
  bool tiled;
  bool packed_mips;
};

// A texture cache's mailbox for decoded replacements (one per cache, kept alive by the cache;
// the workers hold weak references).
class Client {
 public:
  bool HasReady() const { return ready_count_.load(std::memory_order_acquire) != 0; }
  std::vector<std::unique_ptr<Decoded>> Take();
  void Put(std::unique_ptr<Decoded> decoded);
  // Decoded data not uploaded yet: the decoder waits while it is over a limit (preloading).
  uint64_t pending_bytes() const { return pending_bytes_.load(std::memory_order_acquire); }
  void Uploaded(uint64_t bytes) { pending_bytes_.fetch_sub(bytes, std::memory_order_acq_rel); }

 private:
  std::mutex mutex_;
  std::vector<std::unique_ptr<Decoded>> ready_;
  std::atomic<uint32_t> ready_count_{0};
  std::atomic<uint64_t> pending_bytes_{0};
};

// Why a texture was not fingerprinted (counted once per texture, kGpuWritten per load).
enum class Skip : uint32_t {
  kFormat,      // a format not dumped yet (counted per format too)
  kShape,       // cube, 3D, array, 1D, or a resolution-scaled resolve target
  kScreen,      // the size of the screen or a fraction of it: render targets read back
  kGpuWritten,  // rendered by the GPU (resolves): not art
  kDynamic,     // reloaded 3 times within a second each: written by the game as it runs
  kNoBase,      // only the mips are loaded (streaming)
  kTiny,        // under 8x8
  kCount,
};
// Whether a texture of this size is a render target's copy (1280x720 and its halves).
bool IsScreenSized(uint32_t width, uint32_t height);

class Packs {
 public:
  // nullptr when texture_dump and texture_replace are both off. Created on the first call,
  // which must come after texture_pack_root is set (the game sets it before the title runs).
  static Packs* Get();

  bool dumping() const { return dump_; }
  bool replacing() const { return replace_; }
  bool preload() const { return preload_; }
  const std::filesystem::path& root() const { return root_; }

  // Command processor thread.
  uint64_t Fingerprint(const GuestTexture& texture, const texture_util::TextureGuestLayout& layout,
                       const uint8_t* base, uint32_t base_size) const;
  // Queues the dump of a fingerprint not dumped before (copies the guest data); a full queue
  // drops it (counted, and it is tried again the next time it loads).
  void Dump(uint64_t fingerprint, const GuestTexture& texture,
            const texture_util::TextureGuestLayout& layout, const uint8_t* base,
            uint32_t base_size, const uint8_t* mips, uint32_t mips_size);
  // Whether the pack has a file for the fingerprint (the folder is listed once, at start).
  bool HasReplacement(uint64_t fingerprint) const;
  // Reads and decodes the file on a worker; the result goes to the client. width/height: the
  // guest texture's, to check the aspect ratio (0: unknown, preloading).
  void RequestDecode(uint64_t fingerprint, uint32_t width, uint32_t height,
                     const std::shared_ptr<Client>& client);
  std::vector<uint64_t> Replacements() const;

  void CountSkip(Skip skip, xenos::TextureFormat format);
  // The texture cache's time for a load (all of it), and the hashing's part of it.
  void CountLoad(uint64_t nanoseconds, uint64_t hash_nanoseconds, uint64_t bytes);
  void CountReplacementUsed(uint64_t fingerprint);
  void CountUploaded(uint64_t gpu_bytes);
  void CountUnusable() { ++unusable_; }
  // Logs the summary at most once a minute (force: now, if anything happened).
  void LogSummary(bool force);

 private:
  Packs();
  struct DumpJob;
  struct DecodeJob;
  void DumpWorker();
  void WriteDump(DumpJob& job, std::ofstream& index, uint32_t& errors_logged);
  void DecodeWorker();

  bool dump_ = false;
  bool replace_ = false;
  bool preload_ = false;
  bool dump_png_ = true;
  std::filesystem::path root_;
  std::filesystem::path dump_dir_;
  // Fingerprint -> file in root_ (read-only after construction).
  std::vector<std::pair<uint64_t, std::filesystem::path>> index_;

  std::mutex dump_mutex_;
  std::unordered_set<uint64_t> dumped_;  // on disk at start, or queued in this run
  std::deque<std::unique_ptr<DumpJob>> dump_queue_;
  uint64_t dump_queue_bytes_ = 0;
  std::vector<std::vector<uint8_t>> spare_buffers_;  // emptied dump copies, to reuse
  uint64_t spare_bytes_ = 0;
  std::condition_variable dump_cv_;

  std::mutex decode_mutex_;
  std::deque<std::unique_ptr<DecodeJob>> decode_queue_;
  std::condition_variable decode_cv_;

  // Statistics.
  struct Stats {
    uint64_t fingerprints = 0;
    uint64_t fingerprint_ns = 0;
    uint64_t fingerprint_max_ns = 0;
    uint64_t hash_ns = 0;
    uint64_t dump_copy_ns = 0;
    uint64_t fingerprint_bytes = 0;
    uint64_t dumps_queued = 0;
    uint64_t dumps_dropped = 0;
    uint64_t replacements_used = 0;
    std::array<uint64_t, size_t(Skip::kCount)> skipped{};
    std::array<uint32_t, 64> skipped_formats{};
  };
  Stats stats_;        // command processor thread
  Stats stats_logged_;  // as of the last summary
  std::unordered_set<uint64_t> used_;  // fingerprints with a replacement, seen loading
  std::atomic<uint64_t> dumps_written_{0};
  std::atomic<uint64_t> dumps_failed_{0};
  std::atomic<uint64_t> dump_bytes_written_{0};
  std::atomic<uint64_t> decodes_ok_{0};
  std::atomic<uint64_t> decodes_failed_{0};
  std::atomic<uint64_t> unusable_{0};  // read, but not usable on this GPU
  std::atomic<uint64_t> uploads_{0};
  std::atomic<uint64_t> upload_bytes_{0};
  uint64_t dumps_written_logged_ = 0;
  uint64_t last_summary_ms_ = 0;
};

// The dump's untiling and the PNG encoder (DDS: pack_format.h).
std::vector<uint8_t> UntileLevel(const GuestTexture& texture,
                                 const texture_util::TextureGuestLayout& layout,
                                 const uint8_t* base, uint32_t base_size, const uint8_t* mips,
                                 uint32_t mips_size, uint32_t level);
// RGBA8 of a level in a file format (BC decoded).
std::vector<uint8_t> ToRgba8(Format format, uint32_t width, uint32_t height, const uint8_t* data);
std::vector<uint8_t> EncodePng(uint32_t width, uint32_t height, const uint8_t* rgba);

}  // namespace rex::graphics::texture_pack
