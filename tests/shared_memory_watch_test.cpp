// The GPU's copy of guest memory and the write watches that keep it up to date (1110, ReIGlue
// #64), against the real guest Memory and SharedMemory (no GPU: the uploads only count pages).
//
// A page the GPU copy holds as valid must be watched, or the first write to it after that must
// invalidate it some other way. Memory released while a frame still queued reads it, or held
// read-only, cannot be watched (EnablePhysicalMemoryAccessCallbacks skips pages a heap does not
// hold or the guest may not write): such a page must not be held as valid, or the allocation
// or protection change that makes it writable again must invalidate it.
//
// Its own process (ctest runs it apart from unit_tests): Memory maps 4.5GB, one per process. No
// Catch2: its signal handler would take the access violations that guest stores to watched
// pages raise. `--bench` (not run by ctest) times the paths the fix touches.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include <rex/graphics/shared_memory.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>

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

  REQUIRE(E0().Release(address));
}

// Memory no view holds is uploaded but not held as valid.
void NotHeldNotValid() {
  {
    current_case = "not held: released while a frame still reads it";
    CountingSharedMemory gpu(TestMemory());
    const uint32_t physical = 0x08100000;
    ReleaseAfterRead(gpu, physical);
    CHECK_FALSE(gpu.IsRangeValid(physical, kBlock));
    CHECK(gpu.Uploads(physical, kBlock));
  }
  {
    current_case = "not held: never allocated";
    CountingSharedMemory gpu(TestMemory());
    const uint32_t physical = 0x08200000;
    CHECK(gpu.Uploads(physical, kBlock));
    CHECK_FALSE(gpu.IsRangeValid(physical, kBlock));
  }
  {
    current_case = "not held: only part of the range is held";
    CountingSharedMemory gpu(TestMemory());
    const uint32_t physical = 0x08300000;
    REQUIRE(E0().AllocFixed(E0Address(physical), 0x1000, 0x1000, kCommit, kReadWrite));
    CHECK(gpu.Uploads(physical, 0x3000));
    CHECK(gpu.IsRangeValid(physical, 0x1000));
    CHECK_FALSE(gpu.IsRangeValid(physical + 0x1000, 0x1000));
    CHECK_FALSE(gpu.IsRangeValid(physical + 0x2000, 0x1000));
    REQUIRE(E0().Release(E0Address(physical)));
  }
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
  // The contents the allocation gives are not the GPU copy's either.
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

// A read-only block made writable: a write after it is seen.
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
  GuestStore(address + 0x20, 4);
  CHECK_FALSE(gpu.IsRangeValid(physical, 0x1000));
  CHECK(gpu.Uploads(physical, kBlock));
  GuestStore(address + 0x20, 5);
  CHECK_FALSE(gpu.IsRangeValid(physical, 0x1000));

  REQUIRE(E0().Release(address));
}

// What the GPU wrote where no view holds it stays valid until an allocation.
void GpuWrittenNotHeld() {
  current_case = "GPU-written, not held";
  CountingSharedMemory gpu(TestMemory());
  const uint32_t physical = 0x08600000;
  gpu.RangeWrittenByGpu(physical, kBlock);
  CHECK(gpu.IsRangeValid(physical, kBlock));
  CHECK_FALSE(gpu.Uploads(physical, kBlock));

  REQUIRE(E0().AllocFixed(E0Address(physical), kBlock, 0x1000, kCommit, kReadWrite));
  CHECK_FALSE(gpu.IsRangeValid(physical, kBlock));
  REQUIRE(E0().Release(E0Address(physical)));
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
    const double invalidate_only =
        BestNanoseconds(100000, [&] { gpu.MemoryInvalidationCallback(physical, length, true); });
    std::printf(
        "%4u pages: request of valid pages %7.1f ns, invalidation + upload %7.1f ns "
        "(the invalidation alone %7.1f ns)\n",
        pages, valid, upload, invalidate_only);
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
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) {
    Bench();
    return failures ? 1 : 0;
  }
  StaysAllocated();
  NotHeldNotValid();
  AllocatedAgain(Again::kAllocFixed);
  AllocatedAgain(Again::kAllocRange);
  AllocatedAgain(Again::kOtherView);
  AllocAgain();
  ReadOnlyMadeWritable();
  GpuWrittenNotHeld();
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("shared memory watch: all checks passed\n");
  return 0;
}
