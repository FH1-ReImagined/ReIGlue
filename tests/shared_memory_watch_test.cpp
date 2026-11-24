// The GPU's copy of guest memory and the write watches that keep it up to date (1110, ReIGlue
// #64), against the real guest Memory and SharedMemory (no GPU: the uploads only count pages).
//
// A page the GPU copy holds as valid must be watched, or the first write to it after that must
// invalidate it some other way. Memory released while a frame still queued reads it, or held
// read-only, cannot be watched (EnablePhysicalMemoryAccessCallbacks skips pages a heap does not
// hold or the guest may not write), so the GPU copy may hold it as valid with no watch: the
// allocation that gives such pages new contents, and the protection change that makes them
// writable, must invalidate them.
//
// Its own process (ctest runs it apart from unit_tests): Memory maps 4.5GB, one per process.
// `--view-granularity 0x10000` maps the views as on a host with that view offset granularity
// (Windows; the 0xE0000000 heap then has a host address offset). No Catch2: its signal handler
// would take the access violations that guest stores to watched pages raise. `--bench` (not run
// by ctest) times the paths the fix touches. The frame cost CSV's counters (ReIGlue #75) are
// checked too: a guest write's fault is a CPU write fault, an invalidation with no fault is not.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include <rex/graphics/shared_memory.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>

#include "../src/graphics/fh1_frame_cost.h"

namespace {

int failures = 0;
const char* current_case = "";

// A failed check is reported and the test goes on, so a run shows every check that fails.
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "%s:%d: %s: check failed: %s\n", __FILE__, __LINE__, current_case, #x); \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)
#define CHECK_FALSE(x) CHECK(!(x))
// For the steps a case cannot go on without.
#define REQUIRE(x)                                                                             \
  do {                                                                                         \
    if (!(x)) {                                                                                \
      std::fprintf(stderr, "%s:%d: %s: required: %s\n", __FILE__, __LINE__, current_case, #x); \
      std::exit(1);                                                                            \
    }                                                                                          \
  } while (0)

using rex::memory::BaseHeap;
using rex::memory::Memory;

constexpr uint32_t kCommit =
    rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
constexpr uint32_t kReadWrite = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
constexpr uint32_t kBlock = 0x10000;  // 64 KB: one page of the 0xA0000000 view

Memory& TestMemory() {
  static Memory memory;
  static bool initialized = [] {
    rex::InitLogging();
    return memory.Initialize();
  }();
  REQUIRE(initialized);
  return memory;
}

// The GPU side without a GPU: an upload marks its pages valid (and watched) before it copies,
// as the Vulkan and D3D12 backends do, and counts them.
class CountingSharedMemory final : public rex::graphics::SharedMemory {
 public:
  explicit CountingSharedMemory(Memory& memory) : SharedMemory(memory) { InitializeCommon(); }

  // Whether reading [start, start + length) uploads anything.
  bool Uploads(uint32_t start, uint32_t length) {
    const uint64_t before = uploaded_pages_;
    REQUIRE(RequestRange(start, length));
    return uploaded_pages_ != before;
  }

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& page_ranges) override {
    for (const auto& [page, count] : page_ranges) {
      MakeRangeValid(page << page_size_log2(), count << page_size_log2(), false);
      uploaded_pages_ += count;
    }
    return true;
  }

 private:
  uint64_t uploaded_pages_ = 0;
};

// The 0xE0000000 view (4 KB pages) and its address of a physical address.
BaseHeap& E0() {
  return *TestMemory().LookupHeap(0xE0000000);
}
uint32_t E0Address(uint32_t physical) {
  return 0xE0000000 + physical - 0x1000;
}

// A guest store (the access violation handler sees it if the page is watched).
void GuestStore(uint32_t address, uint32_t value) {
  *TestMemory().TranslateVirtual<volatile uint32_t*>(address) = value;
}

// A host write the kernel makes for the guest (a file read): it reports the range first.
void KernelWrite(uint32_t address, uint32_t length) {
  TestMemory().TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                              address, length, true, true);
  std::memset(TestMemory().TranslateVirtual(address), 0x5A, length);
}

// Allocates a block at a physical address in the 0xE0000000 view, has the GPU read it (so it
// is watched), releases it and has a frame still queued read it once more.
void ReleaseAfterRead(CountingSharedMemory& gpu, uint32_t physical) {
  REQUIRE(E0().AllocFixed(E0Address(physical), kBlock, 0x1000, kCommit, kReadWrite));
  CHECK(gpu.Uploads(physical, kBlock));
  REQUIRE(E0().Release(E0Address(physical)));
  CHECK(gpu.Uploads(physical, kBlock));  // the late read: nothing holds the pages now
}

// A block that stays allocated: every rewrite is seen, a read again uploads nothing.
void StaysAllocated() {
  current_case = "a block that stays allocated";
  CountingSharedMemory gpu(TestMemory());
  const uint32_t physical = 0x08000000;
  const uint32_t address = E0Address(physical);
  REQUIRE(E0().AllocFixed(address, kBlock, 0x1000, kCommit, kReadWrite));

  CHECK(gpu.Uploads(physical, kBlock));
  CHECK_FALSE(gpu.Uploads(physical, kBlock));
  GuestStore(address + 0x100, 1);
  CHECK_FALSE(gpu.IsRangeValid(physical, 0x1000));
  CHECK(gpu.Uploads(physical, kBlock));
  KernelWrite(address + 0x2000, 0x1000);
  CHECK_FALSE(gpu.IsRangeValid(physical + 0x2000, 0x1000));
  CHECK(gpu.Uploads(physical, kBlock));
  CHECK_FALSE(gpu.Uploads(physical, kBlock));

  // An allocation next to it leaves it as it is.
  REQUIRE(E0().AllocFixed(address + kBlock, kBlock, 0x1000, kCommit, kReadWrite));
  CHECK(gpu.IsRangeValid(physical, kBlock));
  CHECK_FALSE(gpu.Uploads(physical, kBlock));

  REQUIRE(E0().Release(address + kBlock));
  REQUIRE(E0().Release(address));
}

// Released, read once more, allocated again (`how`): a write after it is seen.
enum class Again { kAllocFixed, kAllocRange, kOtherView };
void AllocatedAgain(Again how) {
  current_case = how == Again::kAllocFixed   ? "allocated again: AllocFixed"
                 : how == Again::kAllocRange ? "allocated again: AllocRange"
                                             : "allocated again: in the 0xA0000000 view";
  CountingSharedMemory gpu(TestMemory());
  const uint32_t physical = 0x08400000;
  ReleaseAfterRead(gpu, physical);

  uint32_t address = 0;
  BaseHeap* heap = &E0();
  switch (how) {
    case Again::kAllocFixed:
      address = E0Address(physical);
      REQUIRE(E0().AllocFixed(address, kBlock, 0x1000, kCommit, kReadWrite));
      break;
    case Again::kAllocRange:
      REQUIRE(E0().AllocRange(E0Address(physical), E0Address(physical) + kBlock - 1, kBlock, 0x1000,
                              kCommit, kReadWrite, false, &address));
      REQUIRE(address == E0Address(physical));
      break;
    case Again::kOtherView:
      heap = TestMemory().LookupHeap(0xA0000000);
      address = 0xA0000000 + physical;
      REQUIRE(heap->AllocFixed(address, kBlock, kBlock, kCommit, kReadWrite));
      break;
  }
  // The contents the allocation gives are not the GPU copy's.
  CHECK_FALSE(gpu.IsRangeValid(physical, kBlock));
  CHECK(gpu.Uploads(physical, kBlock));
  CHECK_FALSE(gpu.Uploads(physical, kBlock));
  GuestStore(address + 0x10, 2);
  CHECK_FALSE(gpu.IsRangeValid(physical, 0x1000));
  CHECK(gpu.Uploads(physical, kBlock));
  KernelWrite(address + 0x3000, 0x1000);
  CHECK_FALSE(gpu.IsRangeValid(physical + 0x3000, 0x1000));

  REQUIRE(heap->Release(address));
}

// Alloc (top-down: the same block again after its release).
void AllocAgain() {
  current_case = "allocated again: Alloc";
  CountingSharedMemory gpu(TestMemory());
  uint32_t address = 0;
  REQUIRE(E0().Alloc(kBlock, 0x1000, kCommit, kReadWrite, true, &address));
  const uint32_t physical = TestMemory().GetPhysicalAddress(address);
  CHECK(gpu.Uploads(physical, kBlock));
  REQUIRE(E0().Release(address));
  CHECK(gpu.Uploads(physical, kBlock));
  uint32_t again = 0;
  REQUIRE(E0().Alloc(kBlock, 0x1000, kCommit, kReadWrite, true, &again));
  REQUIRE(again == address);

  CHECK(gpu.Uploads(physical, kBlock));
  GuestStore(address, 3);
  CHECK_FALSE(gpu.IsRangeValid(physical, 0x1000));

  REQUIRE(E0().Release(address));
}

// Committing pages again keeps their contents, also what the GPU wrote; the pages the same
// commit adds get new ones.
void CommittedAgain() {
  current_case = "committed again";
  CountingSharedMemory gpu(TestMemory());
  const uint32_t physical = 0x08700000;
  const uint32_t address = E0Address(physical);
  REQUIRE(E0().AllocFixed(address, kBlock, 0x1000, kCommit, kReadWrite));
  gpu.RangeWrittenByGpu(physical, kBlock);
  ReleaseAfterRead(gpu, physical + kBlock);

  REQUIRE(E0().AllocFixed(address, 2 * kBlock, 0x1000, kCommit, kReadWrite));
  CHECK(gpu.IsRangeValid(physical, kBlock));
  CHECK(gpu.IsRangeGpuWritten(physical, kBlock));
  CHECK_FALSE(gpu.IsRangeValid(physical + kBlock, kBlock));

  REQUIRE(E0().Release(address));
}

// A read-only block made writable: the GPU copy is invalidated (a write after it is seen).
void ReadOnlyMadeWritable() {
  current_case = "read-only made writable";
  CountingSharedMemory gpu(TestMemory());
  const uint32_t physical = 0x08500000;
  const uint32_t address = E0Address(physical);
  REQUIRE(E0().AllocFixed(address, kBlock, 0x1000, kCommit, rex::memory::kMemoryProtectRead));

  // Read-only pages cannot be watched, but stay valid: no upload on every read.
  CHECK(gpu.Uploads(physical, kBlock));
  CHECK_FALSE(gpu.Uploads(physical, kBlock));
  REQUIRE(E0().Protect(address, kBlock, kReadWrite));
  CHECK_FALSE(gpu.IsRangeValid(physical, kBlock));
  CHECK(gpu.Uploads(physical, kBlock));
  GuestStore(address + 0x20, 4);
  CHECK_FALSE(gpu.IsRangeValid(physical, 0x1000));

  REQUIRE(E0().Release(address));
}

// What the GPU wrote: a block made writable is invalidated whether it was watched (writable
// before, made read-only) or not (read-only from the start), as before 1110 for a watched one.
// The read-only one is a trade-off, not an oversight: what the GPU wrote there while the page
// was read-only in every view (a resolve or memexport never copied back to guest memory) is
// dropped, and the next read uploads the guest's bytes over it. Before 1110 it was kept, but the
// page was then writable with no watch, so every CPU write after the protection change was
// missed (ReIGlue #64). Keeping it exactly needs a watch armed on the GPU-written pages only,
// which SharedMemory would have to tell the heap about (a change of the invalidation callback);
// the reviews of #73 counted no such page in FH1 (benchmark, free roam, photo mode).
void GpuWrittenMadeWritable() {
  current_case = "GPU-written made writable";
  CountingSharedMemory gpu(TestMemory());
  const uint32_t physical = 0x08600000;
  const uint32_t address = E0Address(physical);
  REQUIRE(E0().AllocFixed(address, kBlock, 0x1000, kCommit, kReadWrite));
  REQUIRE(
      E0().AllocFixed(address + kBlock, kBlock, 0x1000, kCommit, rex::memory::kMemoryProtectRead));
  gpu.RangeWrittenByGpu(physical, 2 * kBlock);
  REQUIRE(E0().Protect(address, kBlock, rex::memory::kMemoryProtectRead));
  CHECK(gpu.IsRangeValid(physical, 2 * kBlock));
  REQUIRE(E0().Protect(address, kBlock, kReadWrite));
  REQUIRE(E0().Protect(address + kBlock, kBlock, kReadWrite));
  CHECK_FALSE(gpu.IsRangeValid(physical, kBlock));
  CHECK_FALSE(gpu.IsRangeValid(physical + kBlock, kBlock));

  REQUIRE(E0().Release(address + kBlock));
  REQUIRE(E0().Release(address));
}

// The frame cost CSV's counters (ReIGlue #75): only a guest write's fault is a CPU write fault;
// allocations, protection changes, file reads and releases are invalidations with no fault.
void FaultCounters() {
  current_case = "fault counters";
  using rex::graphics::fh1_frame_cost::Frame;
  using rex::graphics::fh1_frame_cost::GetBackground;
  const auto take = [] {
    Frame frame;
    GetBackground().Take(frame);
    return frame;
  };
  CountingSharedMemory gpu(TestMemory());
  const uint32_t physical = 0x08800000;
  const uint32_t address = E0Address(physical);
  take();

  REQUIRE(E0().AllocFixed(address, kBlock, 0x1000, kCommit, kReadWrite));
  Frame frame = take();
  CHECK(frame.cpu_write_faults == 0);
  CHECK(frame.cpu_invalidations == 1);

  CHECK(gpu.Uploads(physical, kBlock));  // watched from here on
  take();
  GuestStore(address + 0x100, 5);
  frame = take();
  CHECK(frame.cpu_write_faults == 1);
  CHECK(frame.cpu_invalidations == 0);

  CHECK(gpu.Uploads(physical, kBlock));  // the fault unwatched more than its page
  take();
  KernelWrite(address + 0x2000, 0x1000);
  frame = take();
  CHECK(frame.cpu_write_faults == 0);
  CHECK(frame.cpu_invalidations == 1);

  CHECK(gpu.Uploads(physical, kBlock));
  REQUIRE(E0().Protect(address, kBlock, rex::memory::kMemoryProtectRead));
  REQUIRE(E0().Protect(address, kBlock, kReadWrite));
  frame = take();
  CHECK(frame.cpu_write_faults == 0);
  CHECK(frame.cpu_invalidations >= 1);

  CHECK(gpu.Uploads(physical, kBlock));
  take();
  REQUIRE(E0().Release(address));
  frame = take();
  CHECK(frame.cpu_write_faults == 0);
  CHECK(frame.cpu_invalidations == 1);
}

template <typename F>
double BestNanoseconds(int iterations, F&& f) {
  double best = 1e300;
  for (int round = 0; round < 7; ++round) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
      f();
    }
    const std::chrono::duration<double, std::nano> elapsed =
        std::chrono::steady_clock::now() - start;
    best = std::min(best, elapsed.count() / iterations);
  }
  return best;
}

// Times the paths 1110 touches.
void Bench() {
  current_case = "bench";
  CountingSharedMemory gpu(TestMemory());
  const uint32_t physical = 0x09000000;
  const uint32_t size = 0x400000;
  REQUIRE(E0().AllocFixed(E0Address(physical), size, 0x1000, kCommit, kReadWrite));
  gpu.Uploads(physical, size);  // watched from here on

  for (uint32_t pages : {1u, 16u, 256u}) {
    const uint32_t length = pages << 12;
    // The fast path: every page valid already.
    const double valid = BestNanoseconds(200000, [&] { gpu.RequestRange(physical, length); });
    // An upload of an allocated, watched range: what a CPU write leaves to the next draw
    // (MemoryInvalidationCallback is what a write's access violation calls).
    const double upload = BestNanoseconds(100000, [&] {
      gpu.MemoryInvalidationCallback(physical, length, true);
      gpu.RequestRange(physical, length);
    });
    std::printf("%4u pages: request of valid pages %7.1f ns, invalidation + upload %7.1f ns\n",
                pages, valid, upload);
  }
  REQUIRE(E0().Release(E0Address(physical)));

  for (uint32_t block : {0x1000u, kBlock, 0x400000u}) {
    const uint32_t address = E0Address(physical);
    const double alloc = BestNanoseconds(5000, [&] {
      E0().AllocFixed(address, block, 0x1000, kCommit, kReadWrite);
      E0().Release(address);
    });
    std::printf("AllocFixed + Release of %7u bytes: %8.1f ns\n", block, alloc);
  }
  // Protect read-only, then writable, of a committed block.
  REQUIRE(E0().AllocFixed(E0Address(physical), size, 0x1000, kCommit, kReadWrite));
  for (uint32_t block : {0x1000u, kBlock, 0x400000u}) {
    const uint32_t address = E0Address(physical);
    const double protect = BestNanoseconds(20000, [&] {
      E0().Protect(address, block, rex::memory::kMemoryProtectRead);
      E0().Protect(address, block, kReadWrite);
    });
    std::printf("Protect read-only + read-write of %7u bytes: %8.1f ns\n", block, protect);
  }
  REQUIRE(E0().Release(E0Address(physical)));
}

}  // namespace

int main(int argc, char** argv) {
  bool bench = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--bench") == 0) {
      bench = true;
    } else if (std::strcmp(argv[i], "--view-granularity") == 0 && i + 1 < argc) {
      rex::memory::detail::SetHostViewOffsetGranularityForTesting(
          std::strtoul(argv[++i], nullptr, 0));
    }
  }
  if (bench) {
    Bench();
    return failures ? 1 : 0;
  }
  StaysAllocated();
  AllocatedAgain(Again::kAllocFixed);
  AllocatedAgain(Again::kAllocRange);
  AllocatedAgain(Again::kOtherView);
  AllocAgain();
  CommittedAgain();
  ReadOnlyMadeWritable();
  GpuWrittenMadeWritable();
  FaultCounters();
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("shared memory watch (0xE0000000 host offset 0x%X): all checks passed\n",
              TestMemory().LookupHeap(0xE0000000)->host_address_offset());
  return 0;
}
