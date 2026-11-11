#ifdef NDEBUG
#undef NDEBUG
#endif
#include <rex/graphics/vulkan/driver_cache_file.h>

#include <cassert>

using namespace rex::graphics::vulkan::driver_cache_file;

int main() {
  const Identity id{0x10de, 0x2d04, 1, 2, {}};
  auto data = std::vector<uint8_t>(96, 42);
  Put32(data.data(), 32);
  Put32(data.data() + 4, 1);
  Put32(data.data() + 8, id.vendor);
  Put32(data.data() + 12, id.device);
  std::copy(id.uuid.begin(), id.uuid.end(), data.begin() + 16);
  auto first = MakeEntry(1, data, data.size());
  auto second = MakeEntry(2, data, data.size() + 1);
  data.back() ^= 1;
  auto collision = MakeEntry(3, data, data.size());
  collision.checksum = first.checksum;

  const auto dir = std::filesystem::temp_directory_path() /
      ("rex-driver-cache-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  assert(std::filesystem::create_directory(dir));
  const auto path = dir / "driver.vkpc";
  const uint64_t budget = kHeaderSize + 2 * kEntryHeaderSize + first.size();
  const auto saved = Write(path, {first, second, collision}, id, budget);
  assert(saved.written && saved.entries == 2 && saved.skipped == 1 && saved.bytes == budget);
  const auto loaded = Read(path, id);
  assert(!loaded.rejected && loaded.entries.size() == 2);
  assert(loaded.entries[0].key == first.key && loaded.entries[1].key == second.key);
  assert(loaded.entries[1].fresh_size == second.fresh_size);
  assert(loaded.entries[0].data == loaded.entries[1].data && *loaded.entries[0].data == *first.data);

  // Even a checksummed reference cannot point to itself.
  {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    const auto offset = std::streamoff(budget - kEntryHeaderSize);
    std::array<uint8_t, kEntryHeaderSize> record{};
    file.seekg(offset);
    assert(file.read(reinterpret_cast<char*>(record.data()), record.size()));
    Put32(record.data() + 12, kPayloadReference | 1);
    Put64(record.data() + 16, Checksum(record.data(), 16, first.checksum));
    file.seekp(offset);
    assert(file.write(reinterpret_cast<const char*>(record.data()), record.size()));
  }
  const auto damaged = Read(path, id);
  assert(damaged.rejected && damaged.entries.size() == 1);

  // A single inline record has the same layout in both versions.
  assert(Write(path, {first}, id).written);
  auto header = EncodeHeader(id, 1);
  Put32(header.data() + 8, kPreviousFormatVersion);
  Put64(header.data() + 56, Checksum(header.data(), 56));
  {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    assert(file.write(reinterpret_cast<const char*>(header.data()), header.size()));
  }
  const auto previous = Read(path, id);
  assert(!previous.rejected && previous.entries.size() == 1);
  assert(previous.entries[0].key == first.key && *previous.entries[0].data == *first.data);

  RetainedPayloads retained;
  retained.Add(first);
  retained.Add(second);
  retained.Add(collision);
  assert(first.data == second.data && first.data != collision.data);
  assert(retained.bytes() == 3 * kEntryHeaderSize + first.size() + collision.size());
  retained.Remove(first);
  assert(retained.bytes() == 2 * kEntryHeaderSize + second.size() + collision.size());
  retained.Remove(collision);
  assert(retained.bytes() == kEntryHeaderSize + second.size());
  retained.Remove(second);
  assert(retained.bytes() == 0);
  assert(std::filesystem::remove(path));
  assert(std::filesystem::remove(dir));
}
