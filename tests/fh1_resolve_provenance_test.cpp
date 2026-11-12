#include <rex/graphics/register_file.h>
#include <rex/graphics/vulkan/fh1_resolve_provenance.h>
#include <rex/memory.h>

#include <cstdio>

namespace {
using namespace rex::graphics;
using namespace rex::graphics::vulkan::fh1;

class TestSharedMemory final : public SharedMemory {
 public:
  explicit TestSharedMemory(rex::memory::Memory& memory) : SharedMemory(memory) { InitializeCommon(); }

 private:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>&) override { return false; }
};

class TestTextureCache final : public TextureCache {
 public:
  TestTextureCache(const RegisterFile& registers, SharedMemory& memory)
      : TextureCache(registers, memory, 2, 2) {}

  std::optional<bool> FetchPage(uint32_t page) {
    const auto* texture = FindOrCreateTexture(ViewKey(page, 32));
    return texture ? std::optional<bool>(texture->key().scaled_resolve != 0) : std::nullopt;
  }

  std::optional<Fh1ResolveRaster> FetchRaster(uint32_t page, uint32_t height,
                                             uint32_t mip_page = 0, uint32_t pitch = 1) {
    return FetchRaster(ViewKey(page, height, mip_page, pitch));
  }

  std::optional<Fh1ResolveRaster> FetchRaster(TextureKey key) {
    const auto* texture = FindOrCreateTexture(key);
    // Keep the stock scaled-buffer admission behavior independently visible.
    return texture && texture->key().scaled_resolve
               ? std::optional(GetFh1ResolveRasterForTexture(*texture)) : std::nullopt;
  }

  static TextureKey ViewKey(uint32_t page, uint32_t height, uint32_t mip_page = 0,
                            uint32_t pitch = 1) {
    TextureKey key;
    key.is_valid = true;
    key.dimension = xenos::DataDimension::k2DOrStacked;
    key.base_page = page;
    key.width_minus_1 = 31;
    key.height_minus_1 = height - 1;
    key.pitch = pitch;
    key.mip_page = mip_page;
    key.mip_max_level = mip_page ? 1 : 0;
    key.tiled = true;
    key.format = xenos::TextureFormat::k_8_8_8_8;
    return key;
  }

 private:
  class TestTexture final : public Texture {
   public:
    TestTexture(TextureCache& cache, const TextureKey& key) : Texture(cache, key) {}
  };
  bool IsScaledResolveSupportedForFormat(TextureKey) const override { return true; }
  uint32_t GetHostFormatSwizzle(TextureKey) const override { return 0; }
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension) const override { return 8192; }
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension) const override { return 6; }
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override {
    return std::make_unique<TestTexture>(*this, key);
  }
  bool LoadTextureDataFromResidentMemoryImpl(Texture&, bool, bool) override { return false; }
};

bool CheckScaledLoadFootprint() {
  constexpr uint32_t kWidth = 33, kHeight = 33, kPitch = 96;
  constexpr uint32_t kPage = 4096;
  for (uint32_t sx : {1u, 2u, 3u, 7u}) {
    for (uint32_t sy : {1u, 2u, 3u, 7u}) {
      // Direct image upload dispatches 4-wide and the generic buffer loader
      // dispatches 8-wide, reading a second quad even on a partial edge.
      for (uint32_t group : {4u, 8u}) {
        for (uint32_t y = 0; y < kHeight * sy; ++y) {
          for (uint32_t x = 0; x < kWidth * sx; x += group) {
            const uint32_t guest_y = y / sy;
            const uint32_t expected_page = (guest_y / 32) * (kPitch / 32) + x / (32 * sx);
            for (uint32_t lane = 0; lane < group; lane += 4) {
              const uint32_t sample_x = x + lane;
              const uint32_t guest_x = (sample_x / 4 / sx) * 4;
              const uint32_t offset = uint32_t(texture_util::GetTiledOffset2D(guest_x, guest_y, kPitch, 2));
              const uint32_t scaled_page = kPage * sx * sy;
              const uint32_t address = offset * sx * sy + ((sample_x / 4 % sx) * sy + y % sy) * 16;
              if (address / scaled_page != expected_page ||
                  (address + 15) / scaled_page != expected_page) return false;
            }
          }
        }
      }
    }
  }
  return true;
}

bool Check() {
  if (!CheckScaledLoadFootprint()) return false;
  constexpr uint32_t kBase = 0x100000;
  const uint32_t page_size = uint32_t(rex::memory::page_size());
  const uint32_t gap = kBase + page_size;
  const uint32_t after_gap = gap + page_size;
  using Raster = TextureCache::Fh1ResolveRaster;

  rex::memory::Memory memory;
  if (!memory.Initialize()) return false;
  constexpr uint32_t kVirtualBase = 0xE0000000 + kBase - 0x1000;
  auto* heap = memory.LookupHeap(kVirtualBase);
  if (!heap || !heap->AllocFixed(kVirtualBase, 3 * page_size, 0x1000,
          rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
          rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) return false;
  RegisterFile registers;
  TestSharedMemory shared(memory);
  TestTextureCache textures(registers, shared);
  ResolveProvenance provenance(textures, shared);

  auto publish = [&](uint32_t address, uint32_t length, Raster raster, ResolveEvidence evidence) {
    provenance.Publish(raster, {address, address, length, 1, evidence});
  };
  publish(kBase, 3 * page_size, Raster::kScene, ResolveEvidence::kFractional);
  const auto old_snapshot = provenance.Find(kBase >> 12);
  if (!old_snapshot || textures.Fh1GetResolveRaster(gap >> 12) != Raster::kScene ||
      textures.FetchPage(gap >> 12) != std::optional<bool>(true) ||
      !shared.IsRangeAllGpuWritten(kBase, 3 * page_size)) return false;

  // Notify through the same physical memory callback as a guest write fault,
  // then write a new CPU value through its virtual alias.
  const uint32_t virtual_gap = kVirtualBase + page_size;
  if (!memory.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                             virtual_gap, 4, true, true)) return false;
  *memory.TranslateVirtual<uint32_t*>(virtual_gap) = 0x12345678;
  if (shared.IsRangeValid(gap, page_size) || textures.HasScaledResolveRange(gap, page_size) ||
      textures.Fh1GetResolveRaster(gap >> 12) != Raster::kFull ||
      provenance.Find(kBase >> 12) || provenance.Find(gap >> 12)) return false;

  // Two independently resolved rows surround the CPU-written alias. The
  // actual cache must not promote the gap back to GPU-valid or scaled.
  publish(kBase, page_size, Raster::kHud, ResolveEvidence::kFractional);
  provenance.Publish(Raster::kFull, {gap, after_gap, page_size, 1, ResolveEvidence::kInteger});
  const auto after = provenance.Find(after_gap >> 12);
  if (!shared.IsRangeAllGpuWritten(kBase, page_size) || !textures.HasScaledResolveRange(after_gap, page_size) ||
      shared.IsRangeValid(gap, page_size) || textures.HasScaledResolveRange(gap, page_size) ||
      textures.FetchPage(gap >> 12) != std::optional<bool>(false) ||
      textures.FetchPage(after_gap >> 12) != std::optional<bool>(true) ||
      textures.Fh1GetResolveRaster(gap >> 12) != Raster::kFull || provenance.Find(gap >> 12) ||
      textures.Fh1GetResolveRaster(kBase >> 12) != Raster::kHud || !after ||
      after->evidence != ResolveEvidence::kInteger || old_snapshot->evidence != ResolveEvidence::kFractional)
    return false;

  // An unclassified GPU write stays GPU-valid, while its old resolve tag and
  // history expire. A later real resolve may publish new evidence normally.
  shared.RangeWrittenByGpu(kBase + 4, 4);
  if (!shared.IsRangeAllGpuWritten(kBase, page_size) || provenance.Find(kBase >> 12) ||
      textures.Fh1GetResolveRaster(kBase >> 12) != Raster::kFull || !provenance.Find(after_gap >> 12))
    return false;
  publish(kBase, page_size, Raster::kScene, ResolveEvidence::kFractional);
  if (!provenance.Find(kBase >> 12) || textures.Fh1GetResolveRaster(kBase >> 12) != Raster::kScene)
    return false;

  // A two-macrotile alias still meets the stock 50% scaled-buffer heuristic.
  // Its unknown second page must disqualify the first page's scene transform.
  const uint32_t view_base = kBase >> 12;
  constexpr uint32_t kGuestPage = 0x1000;
  if (page_size != kGuestPage) return true;
  // With 32 logical texels and 64-pixel pitch, physical pages 0 and 2
  // cover the entire view. Page 1 still contains the independent CPU alias.
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 32; ++x) {
      if ((texture_util::GetTiledOffset2D(x, y, 64, 2) >> 12) != (y >> 5) * 2)
        return false;
    }
  }
  publish(after_gap, page_size, Raster::kScene, ResolveEvidence::kFractional);
  if (shared.IsRangeValid(gap, page_size) ||
      textures.FetchRaster(view_base, 64, 0, 2) != std::optional(Raster::kScene) ||
      textures.FetchRaster(view_base, 33, 0, 2) != std::optional(Raster::kScene)) return false;
  auto padded = TestTextureCache::ViewKey(view_base, 64, 0, 2);
  padded.format = xenos::TextureFormat::k_2_10_10_10;
  if (textures.FetchRaster(padded) != std::optional(Raster::kScene)) return false;
  padded.packed_mips = true;
  if (textures.FetchRaster(padded) != std::optional(Raster::kFull) ||
      textures.FetchRaster(view_base, 64, after_gap >> 12, 2) != std::optional(Raster::kFull))
    return false;
  padded.packed_mips = false;
  padded.format = xenos::TextureFormat::k_16_16;
  if (textures.FetchRaster(padded) != std::optional(Raster::kFull)) return false;

  TestTextureCache stock_textures(registers, shared);
  stock_textures.MarkRangeAsResolved(kBase, page_size);
  if (stock_textures.FetchRaster(view_base, 32) != std::optional(Raster::kFull)) return false;
  publish(kBase, page_size, Raster::kScene, ResolveEvidence::kFractional);
  publish(after_gap, page_size, Raster::kHud, ResolveEvidence::kFractional);
  if (textures.FetchRaster(view_base, 64, 0, 2) != std::optional(Raster::kFull)) return false;
  publish(after_gap, page_size, Raster::kFull, ResolveEvidence::kInteger);

  if (textures.FetchRaster(view_base, 64) != std::optional(Raster::kFull)) return false;
  publish(gap, page_size, Raster::kScene, ResolveEvidence::kFractional);
  if (textures.FetchRaster(view_base, 64) != std::optional(Raster::kScene)) return false;
  publish(gap, page_size, Raster::kHud, ResolveEvidence::kFractional);
  if (textures.FetchRaster(view_base, 64) != std::optional(Raster::kFull)) return false;

  // A mip shares the view's single UV transform even when the base itself is
  // fully covered. An integer mip rejects the scene tag until it also agrees.
  if (textures.FetchRaster(view_base, 32, after_gap >> 12) != std::optional(Raster::kFull))
    return false;
  publish(after_gap, page_size, Raster::kScene, ResolveEvidence::kFractional);
  if (textures.FetchRaster(view_base, 32, after_gap >> 12) != std::optional(Raster::kScene))
    return false;

  auto cube = TestTextureCache::ViewKey(view_base, 32, 0, 2);
  cube.dimension = xenos::DataDimension::kCube;
  cube.depth_or_array_size_minus_1 = 5;
  const uint32_t cube_bytes = cube.GetGuestLayout().base.level_data_extent_bytes;
  if (cube_bytes <= 2 * page_size) return false;
  shared.RangeWrittenByGpu(gap, page_size);  // Remove the tag in one cube storage page.
  publish(after_gap, cube_bytes - 2 * page_size, Raster::kScene, ResolveEvidence::kFractional);
  if (textures.FetchRaster(cube) != std::optional(Raster::kFull)) return false;

  // At the physical aperture end, a one-macrotile view fits exactly. A
  // padded second row must fail without wrapping the address into low memory.
  const uint32_t last_page = SharedMemory::kBufferSize - page_size;
  publish(last_page, page_size, Raster::kScene, ResolveEvidence::kFractional);
  const uint32_t last_padded_view = last_page - 2 * page_size;
  publish(last_padded_view, page_size, Raster::kScene, ResolveEvidence::kFractional);
  return textures.FetchRaster(last_page >> 12, 32) == std::optional(Raster::kScene) &&
         textures.FetchRaster(last_padded_view >> 12, 33, 0, 2) == std::optional(Raster::kScene) &&
         textures.FetchRaster(last_page >> 12, 33, 0, 2) == std::optional(Raster::kFull);
}
}  // namespace

int main() {
  const bool passed = Check();
  std::puts(passed ? "FH1 resolve provenance cache fixture passed" : "FH1 resolve provenance cache fixture failed");
  return passed ? 0 : 1;
}
