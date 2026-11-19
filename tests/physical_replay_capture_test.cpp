#include "../src/system/physical_replay_capture_win.h"

#include <cstdio>
#include <cstdlib>

namespace rex::memory::detail {

struct FrozenReplayPagesTestAccess {
  static bool Counts(const FrozenReplayPages& capture, size_t restores, size_t spans) {
    return capture.restore_.size() == restores && capture.sealed_.size() == spans;
  }
  static bool HeapStorage(const FrozenReplayPages& capture, bool restores, bool spans) {
    return (capture.restore_.overflow.capacity() != 0) == restores &&
           (capture.sealed_.overflow.capacity() != 0) == spans;
  }
};

}  // namespace rex::memory::detail

namespace {

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "physical replay: line %d: %s\n", __LINE__, #condition); \
      std::exit(1);                                                             \
    }                                                                           \
  } while (false)

constexpr size_t kPage = 4096;
constexpr size_t kPages = 12;

DWORD Protection(const void* address) {
  MEMORY_BASIC_INFORMATION info{};
  CHECK(VirtualQuery(address, &info, sizeof(info)));
  return info.Protect;
}

}  // namespace

int main() {
  constexpr size_t kBytes = kPages * kPage;
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       kBytes, nullptr);
  CHECK(section);
  std::array<uint8_t*, 4> aliases;
  for (auto& alias : aliases) {
    alias = static_cast<uint8_t*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, kBytes));
    CHECK(alias);
  }
  for (size_t i = 0; i < kBytes; ++i) aliases[0][i] = uint8_t(i % 251);
  const auto at = [&](size_t page) {
    auto addresses = aliases;
    for (auto& address : addresses) address += page * kPage;
    return addresses;
  };
  DWORD old = 0;
  CHECK(VirtualProtect(at(1)[1], 2 * kPage, PAGE_READONLY, &old));
  CHECK(VirtualProtect(at(8)[2], kPage, PAGE_NOACCESS, &old));

  std::array<uint8_t, 16> bytes{};
  std::array<uint8_t, 4 * kPage> expected{};
  std::memcpy(expected.data(), at(2)[3], expected.size());
  {
    using Journal = rex::memory::detail::FrozenReplayPagesTestAccess;
    rex::memory::detail::FrozenReplayPages capture;
    for (size_t page : {0u, 3u, 4u, 5u}) CHECK(capture.Copy(at(page), kPage, 0, bytes));
    CHECK(Journal::Counts(capture, 16, 16));
    CHECK(Journal::HeapStorage(capture, false, false));

    // A partially preflighted spill must truncate back to the inline boundary.
    bytes.fill(0xcd);
    CHECK(!capture.Copy(at(8), kPage, 0, bytes));
    CHECK(Journal::Counts(capture, 16, 16));
    CHECK(Journal::HeapStorage(capture, true, false));
    for (auto byte : bytes) CHECK(byte == 0xcd);
    CHECK(Protection(at(8)[0]) == PAGE_READWRITE && Protection(at(8)[1]) == PAGE_READWRITE);
    CHECK(Protection(at(8)[2]) == PAGE_NOACCESS);

    CHECK(capture.Copy(at(6), kPage, 0, bytes));
    CHECK(Journal::Counts(capture, 20, 20));
    CHECK(Journal::HeapStorage(capture, true, true));
    // Rollback must also retain already applied overflow entries.
    CHECK(!capture.Copy(at(8), kPage, 0, bytes));
    CHECK(Journal::Counts(capture, 20, 20));
    for (size_t page : {7u, 9u, 3u}) CHECK(capture.Copy(at(page), kPage, 0, bytes));
    CHECK(Journal::Counts(capture, 28, 28));
    for (size_t page : {0u, 3u, 4u, 5u, 6u, 7u, 9u})
      for (auto* address : at(page)) CHECK(Protection(address) == PAGE_READONLY);
  }
  for (size_t page : {0u, 3u, 4u, 5u, 6u, 7u, 9u})
    for (auto* address : at(page)) CHECK(Protection(address) == PAGE_READWRITE);
  {
    rex::memory::detail::FrozenReplayPages capture;
    CHECK(capture.Copy(at(0), kPage, 0, bytes));
    CHECK(capture.Copy(at(4), kPage, 0, bytes));
    // A cached RW region can include page 4, which is now sealed. The
    // spanning read must leave it out of the new restoration runs.
    CHECK(capture.Matches(at(2), 4 * kPage, 0, expected));
    for (size_t page : {0u, 2u, 3u, 4u, 5u})
      for (auto* address : at(page)) CHECK(Protection(address) == PAGE_READONLY);

    bytes.fill(0xbc);
    CHECK(!capture.Copy(at(8), kPage, 0, bytes));
    for (auto byte : bytes) CHECK(byte == 0xbc);
    CHECK(Protection(at(8)[0]) == PAGE_READWRITE);
    CHECK(Protection(at(8)[2]) == PAGE_NOACCESS);
    CHECK(capture.Copy(at(9), kPage, 0, bytes));
  }
  for (size_t a = 0; a < aliases.size(); ++a) {
    for (size_t page = 0; page < kPages; ++page) {
      DWORD protection = PAGE_READWRITE;
      if (a == 1 && (page == 1 || page == 2)) protection = PAGE_READONLY;
      if (a == 2 && page == 8) protection = PAGE_NOACCESS;
      CHECK(Protection(at(page)[a]) == protection);
    }
  }

  at(4)[0][19] ^= 1;
  {
    rex::memory::detail::FrozenReplayPages next;
    CHECK(!next.Matches(at(2), 4 * kPage, 0, expected));
  }
  {
    // The ignored word is the last word of the first page. Both pages
    // must still be sealed, and a change outside the ignored word must fail.
    constexpr size_t offset = kPage - 4;
    std::array<uint8_t, 8> header{};
    std::memcpy(header.data(), at(6)[3] + offset, header.size());
    at(6)[0][offset] ^= 1;
    rex::memory::detail::FrozenReplayPages next;
    CHECK(!next.Matches(at(6), 2 * kPage, offset, header, 1));
    CHECK(!next.Matches(at(6), 2 * kPage, offset, header, header.size()));
    CHECK(next.Matches(at(6), 2 * kPage, offset, header, 0));
    CHECK(!next.Matches(at(6), 2 * kPage, offset, header, 4));
    for (size_t page : {6u, 7u})
      for (auto* address : at(page)) CHECK(Protection(address) == PAGE_READONLY);
    // The skipped word lies on an inaccessible alias. The guard must
    // refuse the entire range even though its other bytes match.
    std::memcpy(header.data(), at(8)[3] + offset, header.size());
    CHECK(!next.Matches(at(8), 2 * kPage, offset, header, 0));
  }
  for (auto* alias : aliases) CHECK(UnmapViewOfFile(alias));
  CHECK(CloseHandle(section));
}
