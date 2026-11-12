// The shader pack's file format (patch 0600, shader_pack_file.h): round trip, shared modules,
// identity checks, damage, and the clean-up of other builds' packs. No GPU or game files.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <rex/graphics/vulkan/shader_pack_file.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace rex::graphics::vulkan::shader_pack_file;

namespace {

std::vector<uint8_t> Module(uint32_t seed, size_t words) {
  // SPIR-V-like: a magic word, then mostly small numbers with some large ones.
  std::mt19937 random(seed);
  std::vector<uint8_t> bytes(words * 4);
  for (size_t i = 0; i < words; ++i) {
    uint32_t w = i == 0 ? 0x07230203u : (random() % 8 ? random() % 300 : uint32_t(random()));
    Put32(bytes.data() + i * 4, w);
  }
  return bytes;
}

Translation Make(uint64_t hash, uint64_t modification, std::vector<uint8_t> spirv,
                 std::vector<uint8_t> textures = {}, std::vector<uint16_t> samplers = {}) {
  Translation t;
  t.hash = hash;
  t.modification = modification;
  t.spirv = std::move(spirv);
  t.textures = std::move(textures);
  t.samplers = std::move(samplers);
  return t;
}

std::vector<uint8_t> ReadAll(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}
void WriteAll(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  std::ofstream(path, std::ios::binary | std::ios::trunc)
      .write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

}  // namespace

int main() {
  // LEB128 words, both ways, including the largest values.
  {
    std::vector<uint8_t> words(5 * 4), encoded, decoded;
    const uint32_t values[5] = {0, 0x7F, 0x80, 0x0FFFFFFF, 0xFFFFFFFF};
    for (size_t i = 0; i < 5; ++i) Put32(words.data() + i * 4, values[i]);
    EncodeWords(words.data(), 5, encoded);
    assert(encoded.size() == 1 + 1 + 2 + 4 + 5);
    assert(DecodeWords(encoded.data(), encoded.size(), 5, decoded) && decoded == words);
    // Too few words for the bytes, too many, a word that does not end.
    assert(!DecodeWords(encoded.data(), encoded.size(), 4, decoded));
    assert(!DecodeWords(encoded.data(), encoded.size(), 6, decoded));
    const uint8_t endless[6] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
    assert(!DecodeWords(endless, sizeof(endless), 1, decoded));
  }

  const auto dir = std::filesystem::temp_directory_path() /
                   ("rex-shader-pack-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  assert(std::filesystem::create_directory(dir));
  const std::string identity = MakeIdentity("1.2.3-dev.gabc@20260101_0000", "scale=1 vendor=amd");
  assert(BuildOf(identity) == "1.2.3-dev.gabc@20260101_0000");
  const auto path = dir / FileName(0x4D5309C9, identity);
  assert(path.filename().string().size() == 8 + 1 + 16 + kExtension.size());
  assert(path.filename().string().rfind("4D5309C9.", 0) == 0);
  assert(FileName(0x4D5309C9, identity) != FileName(0x4D5309C9, identity + "x"));

  const auto a = Module(1, 700), b = Module(2, 3000), c = Module(3, 1);
  std::vector<Translation> in;
  in.push_back(Make(0x30, 2, b, {0x21, 0xA2}, {0x1234, 0x0001}));
  in.push_back(Make(0x10, 7, a, {0x05}));
  in.push_back(Make(0x10, 1, a));  // the same module as (0x10, 7): stored once
  in.push_back(Make(0x20, 0, c, {}, {0x7FFF}));
  in.push_back(Make(0x10, 7, b));  // a second (0x10, 7): the first is kept
  const WriteResult written = Write(path, identity, 0x20261018, in);
  assert(written.written && written.pairs == 4 && written.blobs == 3);
  assert(written.spirv_bytes == a.size() * 2 + b.size() + c.size());
  assert(written.bytes < written.spirv_bytes);

  {
    Reader r;
    assert(r.Open(path, identity, 0x20261018) == Reader::Status::kOk);
    assert(r.pair_count() == 4 && r.blob_count() == 3);
    assert(!r.Find(0x10, 2) && !r.Find(0x40, 0));
    std::vector<uint8_t> spirv, scratch;
    // From the file, then from memory.
    for (int loaded = 0; loaded < 2; ++loaded) {
      if (loaded) assert(r.Load());
      const Reader::Pair* p = r.Find(0x10, 7);
      assert(p && p->textures == 1 && p->samplers == 0 && r.textures(*p)[0] == 0x05);
      assert(r.Decode(*p, spirv, scratch) && spirv == a);
      p = r.Find(0x10, 1);
      assert(p && p->blob == r.Find(0x10, 7)->blob && r.Decode(*p, spirv, scratch) && spirv == a);
      p = r.Find(0x30, 2);
      assert(p && p->textures == 2 && p->samplers == 2);
      assert(r.textures(*p)[0] == 0x21 && r.textures(*p)[1] == 0xA2);
      assert(r.sampler(*p, 0) == 0x1234 && r.sampler(*p, 1) == 0x0001);
      assert(r.Decode(*p, spirv, scratch) && spirv == b);
      p = r.Find(0x20, 0);
      assert(p && p->samplers == 1 && r.sampler(*p, 0) == 0x7FFF);
      assert(r.Decode(*p, spirv, scratch) && spirv == c);
    }
    r.Unload();
    // Another identity, modification version or format: never used.
    Reader other;
    assert(other.Open(path, identity + " ", 0x20261018) == Reader::Status::kOther);
    assert(other.Open(path, MakeIdentity("1.2.3-dev.gabc@20260101_0000", "scale=2 vendor=amd"),
                      0x20261018) == Reader::Status::kOther);
    assert(other.Open(path, identity, 0x20261019) == Reader::Status::kOther && !other.is_open());
    assert(other.Open(dir / "missing.vk.xspv", identity, 0x20261018) == Reader::Status::kNone);
    assert(ReadIdentity(path) == identity);
  }

  const std::vector<uint8_t> good = ReadAll(path);
  const auto damaged = dir / "damaged.vk.xspv";
  {
    // Every byte of the header and the metadata is checked: flip one in each region.
    const size_t meta_end = kHeaderSize + identity.size() + 4 * kPairSize + 3 * kBlobSize;
    for (size_t at : {size_t(9), size_t(17), size_t(40), size_t(60), kHeaderSize + 3,
                      kHeaderSize + identity.size() + 5, meta_end - 2}) {
      auto bytes = good;
      bytes[at] ^= 0x10;
      WriteAll(damaged, bytes);
      Reader r;
      const auto status = r.Open(damaged, identity, 0x20261018);
      assert(status == Reader::Status::kDamaged || status == Reader::Status::kOther);
      assert(!r.is_open());
    }
    // Truncated, or longer than its header says.
    auto bytes = good;
    bytes.pop_back();
    WriteAll(damaged, bytes);
    Reader r;
    assert(r.Open(damaged, identity, 0x20261018) == Reader::Status::kDamaged);
    bytes = good;
    bytes.push_back(0);
    WriteAll(damaged, bytes);
    assert(r.Open(damaged, identity, 0x20261018) == Reader::Status::kDamaged);
    WriteAll(damaged, std::vector<uint8_t>(10, 0));
    assert(r.Open(damaged, identity, 0x20261018) == Reader::Status::kDamaged);
    // A damaged module: the pack opens, that module does not decode, the others do.
    bytes = good;
    bytes[bytes.size() - 3] ^= 0x01;  // in the last module stored, (0x30, 2)'s
    WriteAll(damaged, bytes);
    assert(r.Open(damaged, identity, 0x20261018) == Reader::Status::kOk);
    std::vector<uint8_t> spirv, scratch;
    assert(!r.Decode(*r.Find(0x30, 2), spirv, scratch));
    assert(r.Decode(*r.Find(0x10, 7), spirv, scratch) && spirv == a);
    // A file replaced after Open is not read as the one opened.
    r.Unload();
    WriteAll(damaged, std::vector<uint8_t>(good.begin(), good.end() - 1));
    assert(!r.Decode(*r.Find(0x10, 7), spirv, scratch));
    std::filesystem::remove(damaged);
  }

  // Encoding is deterministic, whatever the input order.
  {
    std::vector<Translation> reversed(in.rbegin(), in.rend());
    std::swap(reversed[0], reversed[4]);  // keep the first (0x10, 7) first
    const auto again = Encode(identity, 0x20261018, reversed);
    assert(again == good);
  }

  // A file's identity: its size and checksum; another byte, another identity; none unreadable.
  {
    const auto file = dir / "plugin.so";
    WriteAll(file, a);
    const std::string id = FileId(file);
    assert(id.size() > 17 && id.substr(17) == std::to_string(a.size()));
    assert(FileId(file) == id);
    auto other = a;
    other[100] ^= 1;
    WriteAll(file, other);
    assert(FileId(file) != id);
    assert(FileId(dir / "missing.so").empty());
    std::filesystem::remove(file);
  }

  // Clean-up: other builds' packs and old temporary files go, this build's other
  // configurations stay.
  {
    const std::string same_build =
        MakeIdentity("1.2.3-dev.gabc@20260101_0000", "scale=2 vendor=amd");
    const std::string old_build = MakeIdentity("1.2.2-dev.g999@20251201_0000", "scale=1 vendor=amd");
    const auto keep_other = dir / FileName(0x4D5309C9, same_build);
    const auto old = dir / FileName(0x4D5309C9, old_build);
    const auto other_title = dir / FileName(0x12345678, old_build);
    assert(Write(keep_other, same_build, 0x20261018, {Make(1, 1, a)}).written);
    assert(Write(old, old_build, 0x20261018, {Make(1, 1, a)}).written);
    assert(Write(other_title, old_build, 0x20261018, {Make(1, 1, a)}).written);
    const auto junk = dir / ("4D5309C9.0000000000000000" + std::string(kExtension));
    WriteAll(junk, {1, 2, 3});
    const auto stale_tmp = path.string() + ".tmp-1-2-3";
    const auto fresh_tmp = path.string() + ".tmp-4-5-6";
    WriteAll(stale_tmp, {1});
    WriteAll(fresh_tmp, {1});
    std::filesystem::last_write_time(
        stale_tmp, std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));
    const auto unrelated = dir / "4D5309C9.xsh";
    WriteAll(unrelated, {1});
    assert(RemoveStale(path, identity) == 3);
    assert(std::filesystem::exists(path) && std::filesystem::exists(keep_other));
    assert(!std::filesystem::exists(old) && !std::filesystem::exists(junk));
    assert(!std::filesystem::exists(stale_tmp) && std::filesystem::exists(fresh_tmp));
    assert(std::filesystem::exists(other_title) && std::filesystem::exists(unrelated));
    // At most max_others of this build's other configurations, the newest.
    assert(RemoveStale(path, identity, 0) == 1 && !std::filesystem::exists(keep_other));
  }

  std::filesystem::remove_all(dir);
  std::puts("shader_pack_file_test: ok");
  return 0;
}
