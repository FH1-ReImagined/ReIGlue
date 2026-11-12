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
    TextureKey key;
    key.is_valid = true;
    key.dimension = xenos::DataDimension::k2DOrStacked;
    key.base_page = page;
    key.width_minus_1 = key.height_minus_1 = 31;
    key.pitch = 1;
    key.tiled = true;
    key.format = xenos::TextureFormat::k_8_8_8_8;
    const auto* texture = FindOrCreateTexture(key);
    return texture ? std::optional<bool>(texture->key().scaled_resolve != 0) : std::nullopt;
  }

 private:
  class TestTexture final : public Texture {
   public:
    TestTexture(TextureCache& cache, const TextureKey& key) : Texture(cache, key) {}
  };
  bool IsScaledResolveSupportedForFormat(TextureKey) const override { return true; }
  uint32_t GetHostFormatSwizzle(TextureKey) const override { return 0; }
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension) const override { return 8192; }
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension) const override { return 1; }
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override {
    return std::make_unique<TestTexture>(*this, key);
  }
  bool LoadTextureDataFromResidentMemoryImpl(Texture&, bool, bool) override { return false; }
};

bool Check() {
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
  return provenance.Find(kBase >> 12).has_value() &&
         textures.Fh1GetResolveRaster(kBase >> 12) == Raster::kScene;
}
}  // namespace

int main() {
  const bool passed = Check();
  std::puts(passed ? "FH1 resolve provenance cache fixture passed" : "FH1 resolve provenance cache fixture failed");
  return passed ? 0 : 1;
}
