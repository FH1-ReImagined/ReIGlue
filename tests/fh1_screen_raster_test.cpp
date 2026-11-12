#include <rex/graphics/vulkan/fh1_screen_raster.h>
#include <rex/graphics/xenos.h>

#include <cstdio>
#include <initializer_list>

namespace {
using namespace rex::graphics::vulkan::fh1;

constexpr bool CheckResolveEvidence() {
  ResolveProducerRaster producers;
  if (producers.Evidence() != ResolveEvidence::kAbsent) return false;
  producers.Add(false, false);
  if (producers.Evidence() != ResolveEvidence::kUnknown) return false;
  producers.Add(true);
  if (producers.Evidence() != ResolveEvidence::kMixed || producers.IsFractional()) return false;
  producers = {};
  producers.Add(false);
  if (producers.Evidence() != ResolveEvidence::kInteger || producers.IsFractional()) return false;
  producers = {};
  producers.Add(true);
  return producers.Evidence() == ResolveEvidence::kFractional && producers.IsFractional();
}

constexpr bool CheckResolveTrace() {
  ResolveTraceHistory history;
  if (history.Find(0x100)) return false;
  history.Record({0x100000, 0x100100, 0x1000, 7, ResolveEvidence::kFractional, true});
  const auto* trace = history.Find(0x100);
  if (!trace || !trace->MatchesBase(0x100) || !trace->final_hud) return false;
  history.Record({0x0FF000, 0x0FF000, 0x2000, 9, ResolveEvidence::kMixed, false});
  trace = history.Find(0x100);
  if (!trace || trace->MatchesBase(0x100) || trace->edram_base != 9 ||
      trace->evidence != ResolveEvidence::kMixed || history.Find(0x102))
    return false;
  for (unsigned i = 0; i < 64; ++i)
    history.Record({0x200000, 0x200000, 1, 0, ResolveEvidence::kInteger, false});
  return history.Find(0x200) && !history.Find(0x100);
}

constexpr bool CheckFullSwapNormalization() {
  constexpr uint32_t address = 0x1BDB1000;
  constexpr RasterExtent guest{1280, 720};
  constexpr RasterExtent backing{1280, 1440};
  ResolveTrace integer{address, address, 3768320, 0, ResolveEvidence::kInteger,
                       false, UINT32_MAX, guest, true};
  if (!CanResampleIntegerSwap(&integer, address, address, guest, backing, guest)) return false;
  if (!CanResampleIntegerSwap(&integer, address, address, guest, {2560, 720}, guest)) return false;
  if (CanResampleIntegerSwap(&integer, address, address, guest, {2560, 1440}, guest) ||
      CanResampleIntegerSwap(&integer, address, address, guest, backing, backing) ||
      CanResampleIntegerSwap(&integer, address, address, guest, backing, {1920, 1080}) ||
      CanResampleIntegerSwap(&integer, address, address + 4096, guest, backing, guest) ||
      CanResampleIntegerSwap(nullptr, address, address, guest, backing, guest))
    return false;
  auto other = integer;
  other.destination_base -= 4096;
  if (CanResampleIntegerSwap(&other, address, address, guest, backing, guest)) return false;
  other = integer;
  other.copy_extent = {1280, 256};
  if (CanResampleIntegerSwap(&other, address, address, guest, backing, guest)) return false;
  other = integer;
  other.color = false;
  if (CanResampleIntegerSwap(&other, address, address, guest, backing, guest)) return false;
  other = integer;
  other.written_length = 4;
  if (CanResampleIntegerSwap(&other, address, address, guest, backing, guest)) return false;
  other = integer;
  other.evidence = ResolveEvidence::kMixed;
  if (CanResampleIntegerSwap(&other, address, address, guest, backing, guest)) return false;
  other.evidence = ResolveEvidence::kFractional;
  return !CanResampleIntegerSwap(&other, address, address, guest, backing, guest);
}

static_assert(CheckResolveEvidence());
static_assert(CheckResolveTrace());
static_assert(CheckFullSwapNormalization());
}  // namespace

int main() {
  using namespace rex::graphics::vulkan::fh1;
  unsigned failures = 0;
  auto check = [&](bool result) { failures += !result; };
  // Actual cockpit producer and downsample chain, with either viewport Y sign.
  for (unsigned width : {512u, 256u, 128u, 64u}) {
    const unsigned height = width * 9 / 16;
    for (float sign : {-1.0f, 1.0f}) {
      const float x = width / 2.0f, y = sign * height / 2.0f;
      check(IsInstrumentViewport(1280, true, true, x, y, width, height));
      check(!IsNativeScreenViewport(1280, true, true, x, y, width, height));
      check(!IsInstrumentViewport(640, true, true, x, y, width, height));
      check(!IsInstrumentViewport(1280, true, true, x, y, 1280, 720));
      check(!IsInstrumentViewport(1280, false, true, x, y, width, height));
      check(!IsInstrumentViewport(1280, true, true, x + 0.5f, y, width, height));
    }
  }
  // Preserve the existing policy for display, padded bloom, square reflections,
  // software viewports, unsupported pitches and dimensions close to the family.
  for (unsigned pitch : {1280u, 640u, 320u, 160u, 80u, 512u}) {
    for (unsigned width : {1280u, 640u, 512u, 320u, 256u, 160u, 128u, 80u, 64u}) {
      for (unsigned height : {720u, 360u, 288u, 192u, 144u, 96u, 72u, 45u, 36u, width}) {
        for (bool xe : {false, true})
          for (bool ye : {false, true}) {
            const float x = width / 2.0f, y = -height / 2.0f;
            const bool supported = pitch == 1280 || pitch == 640 || pitch == 320 || pitch == 160;
            const bool prior = supported && IsDisplayViewport(xe, ye, x, y, width, height);
            const bool instrument = IsInstrumentViewport(pitch, xe, ye, x, y, width, height);
            check(IsNativeScreenViewport(pitch, xe, ye, x, y, width, height) ==
                  (prior && !instrument));
          }
      }
    }
  }
  // Square faces remain integer; padded bloom and cinematic display remain native.
  check(!IsNativeScreenViewport(1280, true, true, 256, -256, 512, 512));
  check(IsNativeScreenViewport(1280, true, true, 160, -96, 320, 192));
  check(IsNativeScreenViewport(1280, true, true, 80, -96, 160, 192));
  check(IsNativeScreenViewport(1280, true, true, 640, -232, 1280, 464));
  check(IsNativeScreenViewport(1280, false, false, 1, 1, 1280, 720));
  check(!IsNativeScreenViewport(1280, false, false, 1, 1, 512, 288));
  // Both reflection and bloom can use the same pitch. The producer viewport
  // distinguishes their coordinate systems even when they resolve to same-width textures.
  check(!IsFractionalSceneViewport(1280, true, true, 256, -256, 512, 512));
  check(!IsFractionalSceneViewport(1280, true, true, 256, -144, 512, 288));
  const bool reflection = IsFractionalSceneViewport(320, true, true, 160, -160, 320, 320);
  const bool bloom = IsFractionalSceneViewport(320, true, true, 160, -96, 320, 192);
  check(!reflection);
  check(bloom);
  ResolveProducerRaster reflection_resolve;
  reflection_resolve.Add(reflection);
  check(!reflection_resolve.IsFractional() && !reflection_resolve.IsMixed());
  ResolveProducerRaster bloom_resolve;
  bloom_resolve.Add(bloom);
  bloom_resolve.Add(bloom);
  check(bloom_resolve.IsFractional() && !bloom_resolve.IsMixed());
  bloom_resolve.Add(reflection);
  check(!bloom_resolve.IsFractional() && bloom_resolve.IsMixed());
  check(!ResolveProducerRaster{}.IsFractional());
  check(IsFractionalSceneViewport(80, true, true, 40, -22.5f, 80, 45));
  check(!IsFractionalSceneViewport(80, true, true, 40, -40, 80, 80));

  constexpr RasterExtent device_pixels{16384, 16384};
  const auto standard = GetNativeSceneRaster({1920, 1080}, {3840, 2160}, device_pixels, 7);
  check(standard.IsEnabled() && standard.backing_scale.width == 3 &&
        standard.backing_scale.height == 3 && standard.scene.width == 1920 &&
        standard.hud.width == 3840);
  const auto asymmetric = GetNativeSceneRaster({2501, 751}, {1280, 720}, device_pixels, 7);
  check(asymmetric.IsEnabled() && asymmetric.backing_scale.width == 2 &&
        asymmetric.backing_scale.height == 2 && asymmetric.scene.height == 751);
  const auto wide = GetNativeSceneRaster({7681, 719}, {1280, 720}, device_pixels, 7);
  check(wide.IsEnabled() && wide.backing_scale.width == 7 && wide.backing_scale.height == 1);
  const auto subnative = GetNativeSceneRaster({959, 539}, {1280, 720}, device_pixels, 7);
  check(subnative.IsEnabled() && subnative.backing_scale.width == 1 &&
        subnative.backing_scale.height == 2);
  const auto subnative_short_device = GetNativeSceneRaster({959, 539}, {1280, 720}, {4096, 1024}, 7);
  check(subnative_short_device.IsEnabled() && subnative_short_device.backing_scale.width == 2 &&
        subnative_short_device.backing_scale.height == 1);
  check(!GetNativeSceneRaster({0, 720}, {1280, 720}, device_pixels, 7).IsEnabled());
  check(!GetNativeSceneRaster({8961, 720}, {1280, 720}, device_pixels, 7).IsEnabled());
  check(!GetNativeSceneRaster({3001, 700}, {1280, 720}, {3200, 1024}, 7).IsEnabled());
  check(!GetNativeSceneRaster({UINT32_MAX, 720}, {1280, 720}, device_pixels, 7).IsEnabled());

  unsigned offset = 0, extent = 2560;
  ScaleRasterScissor(offset, extent, 1920, 2560);
  check(offset == 0 && extent == 1920);
  offset = 1;
  extent = 4;
  ScaleRasterScissor(offset, extent, 1920, 2560);
  check(offset == 0 && extent == 4);
  offset = 5;
  extent = 0;
  ScaleRasterScissor(offset, extent, 1920, 2560);
  check(offset == 3 && extent == 0);
  offset = 0;
  extent = 1440;
  ScaleRasterScissor(offset, extent, 751, 1440);
  check(offset == 0 && extent == 751);
  offset = 320;
  extent = 320;
  ScaleRasterScissor(offset, extent, 959, 1280);
  check(offset == 239 && extent == 241);

  check(SupportsFractionalPredicatedTiles({1920, 1080}, {2560, 1440}));
  check(SupportsFractionalPredicatedTiles({2560, 1080}, {2560, 1440}));
  check(!SupportsFractionalPredicatedTiles({2501, 751}, {2560, 1440}));
  check(!SupportsFractionalPredicatedTiles({959, 539}, {1280, 1440}));

  constexpr RasterExtent scene_backing{2560, 1440};
  constexpr RasterExtent exact_1080p{1920, 1080};
  constexpr RasterExtent odd_1080p{1919, 1079};
  check(KeepIntegerRaster(exact_1080p, scene_backing, true, false, false, true));
  check(KeepIntegerRaster({2560, 1080}, scene_backing, true, false, false, true));
  check(!KeepIntegerRaster(exact_1080p, scene_backing, true, false, false, false));
  check(KeepIntegerRaster(odd_1080p, scene_backing, true, false, false, true));
  check(!KeepIntegerRaster(exact_1080p, scene_backing, false, false, false, true));
  check(!KeepIntegerRaster(scene_backing, scene_backing, true, false, false, true));
  check(KeepIntegerRaster(exact_1080p, scene_backing, false, true, false, true));

  // The copied TU4 profile draws the 720p HUD on the seeded color target.
  // Guest draw and resolve use different packed views of the same base.
  using Color = rex::graphics::xenos::ColorRenderTargetFormat;
  constexpr RasterExtent hud720{1280, 720};
  constexpr RasterExtent asymmetric_backing{1280, 1440};
  const auto hud_target = [hud720](unsigned expected, unsigned actual, bool one_sample,
                              Color format, bool offset = false) {
    return IsNative720pHudTarget(hud720, expected, actual, 1280, one_sample, format, offset);
  };
  const bool hud_draw = hud_target(0, 0, true, Color::k_2_10_10_10_AS_10_10_10_10);
  const bool hud_copy = hud_target(0, 0, true, Color::k_2_10_10_10);
  check(hud_draw && hud_copy);
  check(!KeepIntegerRaster(hud720, asymmetric_backing, true, false, hud_draw, false));
  check(KeepIntegerRaster(hud720, asymmetric_backing, true, false, false, false));
  check(KeepIntegerRaster(hud720, asymmetric_backing, true, true, hud_draw, false));
  ResolveProducerRaster hud_producer;
  const bool final_hud_resolve = IsNative720pHudResolve(hud_copy, true, hud720, false);
  check(final_hud_resolve && !hud_producer.IsFractional());
  hud_producer.Add(true);
  check(final_hud_resolve && hud_producer.IsFractional());
  hud_producer.Add(false);
  check(final_hud_resolve && !hud_producer.IsFractional());
  check(!IsNative720pHudResolve(hud_target(UINT32_MAX, 0, true, Color::k_2_10_10_10),
                                true, hud720, false));
  check(!IsNative720pHudResolve(hud_target(0, 1, true, Color::k_2_10_10_10),
                                true, hud720, false));
  check(!IsNative720pHudResolve(hud_target(0, 0, false, Color::k_2_10_10_10),
                                true, hud720, false));
  check(!IsNative720pHudResolve(hud_target(0, 0, true, Color::k_2_10_10_10, true),
                                true, {1280, 256}, true));
  check(!IsNative720pHudResolve(hud_copy, false, hud720, false));
  check(!IsNative720pHudResolve(hud_copy, true, {1280, 256}, false));
  check(!IsNative720pHudResolve(hud_copy, true, hud720, true));
  check(!hud_target(0, 0, true, Color::k_8_8_8_8));
  check(!IsNative720pHudTarget({1920, 1080}, 0, 0, 1280, true, Color::k_2_10_10_10, false));

  // #785: the race intro's three 1280x256 tiles on the 1080p HUD target (raster 0.75).
  FractionalTileResolve tile{};
  check(GetFractionalTileResolve(0, -256, 0.75f, 0.75f, 1280, 256, tile) && tile.x == 0 &&
        tile.y == 192 && tile.width == 960 && tile.height == 192);
  check(GetFractionalTileResolve(0, -512, 0.75f, 0.75f, 1280, 208, tile) && tile.x == 0 &&
        tile.y == 384 && tile.width == 960 && tile.height == 160);
  // Columns too; the moved copy never grows past the tile.
  check(GetFractionalTileResolve(-640, 0, 0.75f, 0.75f, 640, 720, tile) && tile.x == 480 &&
        tile.y == 0 && tile.width == 480 && tile.height == 544);
  check(GetFractionalTileResolve(0, -256, 1.0f, 0.75f, 1280, 8, tile) && tile.width == 1280 &&
        tile.height == 8);
  // Nothing moves: the first tile, an untiled resolve, an integer raster (1440p, 4K), a
  // position off the 8-pixel alignment (0.75 * 8 = 6), a positive offset.
  check(!GetFractionalTileResolve(0, 0, 0.75f, 0.75f, 1280, 256, tile));
  check(!GetFractionalTileResolve(0, -256, 1.0f, 1.0f, 1280, 256, tile));
  check(!GetFractionalTileResolve(0, -8, 0.75f, 0.75f, 1280, 256, tile));
  check(!GetFractionalTileResolve(0, 256, 0.75f, 0.75f, 1280, 256, tile));
  check(!GetFractionalTileResolve(0, -256, 0.75f, 0.75f, 1280, 0, tile));
  check(!GetFractionalTileResolve(0, -256, 1.5f, 0.75f, 1280, 256, tile));
  std::printf("FH1 instrument/native raster policy: %u failures\n", failures);
  return failures ? 1 : 0;
}
