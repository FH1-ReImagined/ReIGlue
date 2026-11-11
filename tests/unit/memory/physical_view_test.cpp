// The 0xE0000000 view and its 0x1000 offset at 4, 16 and 64KB granularity (0500, FH1 #788).
// The view starts 4KB into physical memory. Where the view offset granularity is coarser than
// 4KB (16 and 64KB-page kernels, and every ARM64 or Android build), Memory::MapViews maps it
// rounded down and its heap adds 0x1000 (host_address_offset). These tests force the host
// granularity and check the heaps' offsets and that each guest view reaches the right physical
// byte. Each granularity is its own ctest (its own process): Memory maps 4.5GB, and the
// unit_tests binary keeps one Memory for its whole run.
#include <cstdint>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>

namespace {

using rex::memory::Memory;
namespace detail = rex::memory::detail;

void StoreU32(void* host, uint32_t value) {
  *static_cast<volatile uint32_t*>(host) = value;
}

uint32_t LoadU32(const void* host) {
  return *static_cast<const volatile uint32_t*>(host);
}

// The guest physical addresses checked through every view that maps them: the
// first 4KB page of the 0xE0000000 view, one in the middle, its last page.
constexpr uint32_t kPhysicalAddresses[] = {0x00001000,       0x00001FFC, 0x0ABCD000,
                                           0x12345678 & ~3u, 0x1FD00000, 0x1FD00FFC};

// Forces a host view offset granularity for one Memory and checks its views.
// 0 keeps the host's: then the compiled guest accesses (PhysicalHostOffset)
// must agree with the runtime too.
void CheckViews(size_t host_granularity) {
  static bool logging = [] {
    rex::InitLogging();
    return true;
  }();
  (void)logging;

  detail::SetHostViewOffsetGranularityForTesting(host_granularity);
  struct Restore {
    ~Restore() { detail::SetHostViewOffsetGranularityForTesting(0); }
  } restore;

  const size_t view_granularity = detail::ViewOffsetGranularityFor(
      host_granularity ? host_granularity : rex::memory::allocation_granularity());
  const uint32_t expected_offset = view_granularity > 0x1000 ? 0x1000u : 0u;
  INFO("host granularity 0x" << std::hex << host_granularity << ", view granularity 0x"
                             << view_granularity);

  Memory memory;
  REQUIRE(memory.Initialize());

  // Only the 0xE0000000 heap is offset.
  REQUIRE(memory.LookupHeap(0xE0000000)->host_address_offset() == expected_offset);
  REQUIRE(memory.LookupHeap(0xFFCFF000)->host_address_offset() == expected_offset);
  REQUIRE(memory.LookupHeap(0xA0000000)->host_address_offset() == 0);
  REQUIRE(memory.LookupHeap(0xC0000000)->host_address_offset() == 0);
  REQUIRE(memory.LookupHeap(0x80000000)->host_address_offset() == 0);
  REQUIRE(memory.LookupHeap(0x00010000)->host_address_offset() == 0);

  uint8_t* const base = memory.virtual_membase();
  REQUIRE(memory.TranslateVirtual(0xE0000000) == base + 0xE0000000u + expected_offset);
  REQUIRE(memory.TranslateVirtual(0xDFFFF000) == base + 0xDFFFF000u);
  REQUIRE(memory.HostToGuestVirtual(memory.TranslateVirtual(0xE0000000)) == 0xE0000000u);
  REQUIRE(memory.HostToGuestVirtual(memory.TranslateVirtual(0xFFCFF000)) == 0xFFCFF000u);

  const bool compiled_agrees = detail::ViewOffsetGranularityMatchesBuild(view_granularity);
  if (!host_granularity) {
    // The host's own granularity: Initialize refuses a build that disagrees.
    REQUIRE(compiled_agrees);
  }
  if (compiled_agrees) {
    REQUIRE(detail::PhysicalHostOffset(0xE0000000) == expected_offset);
  }

  uint32_t tag = 0x5EED0000u;
  for (uint32_t physical : kPhysicalAddresses) {
    INFO("physical 0x" << std::hex << physical);
    const uint32_t a0 = 0xA0000000u + physical;
    const uint32_t c0 = 0xC0000000u + physical;
    const uint32_t e0 = 0xE0000000u + physical - 0x1000u;

    // Written through the raw physical view, read through every alias.
    StoreU32(memory.TranslatePhysical(physical), ++tag);
    REQUIRE(LoadU32(memory.TranslateVirtual(a0)) == tag);
    REQUIRE(LoadU32(memory.TranslateVirtual(c0)) == tag);
    REQUIRE(LoadU32(memory.TranslateVirtual(e0)) == tag);
    REQUIRE(memory.GetPhysicalAddress(e0) == physical);

    // Written through the 0xE0000000 view, read through the others.
    StoreU32(memory.TranslateVirtual(e0), ++tag);
    REQUIRE(LoadU32(memory.TranslatePhysical(physical)) == tag);
    REQUIRE(LoadU32(memory.TranslateVirtual(a0)) == tag);

    // The compiled guest access (base + address + PhysicalHostOffset).
    if (compiled_agrees) {
      REQUIRE(rex::memory::GuestPtr(base, e0) == memory.TranslateVirtual(e0));
      REQUIRE(LoadU32(rex::memory::GuestPtr(base, e0)) == tag);
    }
  }

  // The 4KB below the 0xE0000000 view's start in the file are physical 0, the
  // 0xA0000000 view's first page: not what guest 0xE0000000 reads.
  StoreU32(memory.TranslatePhysical(0), 0xDEADBEEFu);
  StoreU32(memory.TranslatePhysical(0x1000), 0x0BADF00Du);
  REQUIRE(LoadU32(memory.TranslateVirtual(0xE0000000)) == 0x0BADF00Du);
}

}  // namespace

TEST_CASE("View offset granularity of this build", "[memory][physical_view][pure]") {
#if REX_PLATFORM_WIN32
  REQUIRE(detail::ViewOffsetGranularityFor(0x1000) == 0x1000);
  REQUIRE(detail::ViewOffsetGranularityFor(0x10000) == 0x10000);
#elif REX_PHYSICAL_VIEW_ROUNDED
  // The same 64KB views on 4, 16 and 64KB kernels.
  REQUIRE(detail::ViewOffsetGranularityFor(0x1000) == 0x10000);
  REQUIRE(detail::ViewOffsetGranularityFor(0x4000) == 0x10000);
  REQUIRE(detail::ViewOffsetGranularityFor(0x10000) == 0x10000);
  REQUIRE(detail::ViewOffsetGranularityMatchesBuild(detail::ViewOffsetGranularityFor(0x1000)));
  REQUIRE(detail::ViewOffsetGranularityMatchesBuild(detail::ViewOffsetGranularityFor(0x4000)));
  REQUIRE(detail::ViewOffsetGranularityMatchesBuild(detail::ViewOffsetGranularityFor(0x10000)));
#else
  // 4KB pages only: a 16 or 64KB-page host is refused, not mapped 4KB off.
  REQUIRE(detail::ViewOffsetGranularityFor(0x1000) == 0x1000);
  REQUIRE(detail::ViewOffsetGranularityMatchesBuild(detail::ViewOffsetGranularityFor(0x1000)));
  REQUIRE_FALSE(
      detail::ViewOffsetGranularityMatchesBuild(detail::ViewOffsetGranularityFor(0x4000)));
  REQUIRE_FALSE(
      detail::ViewOffsetGranularityMatchesBuild(detail::ViewOffsetGranularityFor(0x10000)));
#endif

  // Every ARM64 and Android build adds the 0x1000, whatever the page size.
#if !REX_PLATFORM_WIN32 && (defined(__aarch64__) || defined(__ANDROID__))
  STATIC_REQUIRE(REX_PHYSICAL_VIEW_ROUNDED == 1);
#elif !REX_PLATFORM_WIN32
  STATIC_REQUIRE(REX_PHYSICAL_VIEW_ROUNDED == 0);
#endif
  STATIC_REQUIRE(detail::PhysicalHostOffset(0xDFFFFFFF) == 0);
  STATIC_REQUIRE(detail::PhysicalHostOffset(0xE0000000) ==
                 (REX_PHYSICAL_VIEW_ROUNDED ? 0x1000u : 0u));
}

TEST_CASE("Guest views at the host's granularity", "[memory][physical_view][native]") {
  CheckViews(0);
}

TEST_CASE("Guest views at 4KB granularity", "[memory][physical_view][4k]") {
  CheckViews(0x1000);
}

TEST_CASE("Guest views at 16KB granularity", "[memory][physical_view][16k]") {
  CheckViews(0x4000);
}

TEST_CASE("Guest views at 64KB granularity", "[memory][physical_view][64k]") {
  CheckViews(0x10000);
}
