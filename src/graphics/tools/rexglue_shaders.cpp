// rexglue-shaders (patch 0580): Xenos shaders translated to SPIR-V offline, by the GPU plugin's
// own translator (the same sources, compiled into this tool), as the Vulkan pipeline cache's
// warm-up does: Shader::AnalyzeUcode, then SpirvShaderTranslator::TranslateAnalyzedShader for
// each (microcode hash, modification) pair. Built with -DREXGLUE_BUILD_SHADER_TOOLS=ON.
//
//   rexglue-shaders translate --store TITLE.xsh [inputs...] [options]
//
// Inputs (any number; the pairs are translated once each, in hash order):
//   --pairs FILE        lines "<hash> <modification>" (hexadecimal, '#' starts a comment)
//   --pipelines FILE    a pipeline store (<title>.<fbo|fsi>.vk.xpso): every pipeline's shaders
//   --defaults          every stored shader with the translator's default modification
// Options:
//   --config "WORDS"    the translator configuration a game logged ("SPIR-V translator
//                       configuration: ..."); default: a Vulkan 1.2 desktop GPU, render scale 1
//   --set KEY=VALUE     change one configuration word (several allowed)
//   --out DIR           write <hash>_<modification>.<vs|ps>.spv
//   --repeat N          translate each pair N times, keep the fastest (timing)
//   --check             translate each pair twice from scratch and compare the bytes
//   --print-config      print the configuration used and exit
//
// stdout: CSV, one line per pair: hash, modification, type, microcode dwords, analysis and
// translation microseconds, SPIR-V bytes, valid, XXH3 of the SPIR-V, identical (with --check).
// stderr: a summary. Exit 0 when every pair translated (and matched), 1 otherwise, 2 on a usage
// or input error.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline_store_file.h>
#include <rex/graphics/vulkan/pipeline_cache.h>
#include <rex/string/buffer.h>

REXCVAR_DECLARE(bool, fh1_dirt_seam_fix);

namespace {

using rex::graphics::Shader;
using rex::graphics::SpirvShaderTranslator;
using rex::graphics::vulkan::VulkanPipelineCache;
namespace xenos = rex::graphics::xenos;
namespace store = rex::graphics::pipeline_store_file;

int Usage() {
  std::fprintf(stderr,
               "usage: rexglue-shaders translate --store TITLE.xsh [--pairs FILE] [--pipelines "
               "FILE.xpso] [--defaults]\n"
               "         [--config \"WORDS\"] [--set KEY=VALUE] [--out DIR] [--repeat N] "
               "[--check] [--print-config]\n");
  return 2;
}

// What the translator is constructed with, and the cvar it reads.
struct Configuration {
  SpirvShaderTranslator::Features features{true};
  bool native_2x_msaa_with_attachments = true;
  bool native_2x_msaa_no_attachments = true;
  bool edram_fragment_shader_interlock = false;
  uint32_t draw_resolution_scale_x = 1, draw_resolution_scale_y = 1;
  float raster_scale_x = 1.0f, raster_scale_y = 1.0f;
  bool dirt_seam_fix = true;
  uint32_t modification_version = SpirvShaderTranslator::Modification::kVersion;

  Configuration() {
    // A Vulkan 1.2 desktop GPU on the framebuffer path (RADV, NVIDIA).
    features.fragment_shader_sample_interlock = false;
  }

  // One "key=value" word, as SpirvShaderTranslator::DescribeConfiguration writes them.
  bool Set(std::string_view word) {
    const size_t eq = word.find('=');
    if (eq == std::string_view::npos) return false;
    const std::string key(word.substr(0, eq)), value(word.substr(eq + 1));
    char* end = nullptr;
    const unsigned long long n = std::strtoull(value.c_str(), &end, 0);
    const bool integer = end && !*end && !value.empty();
    auto flag = [&](bool& out) {
      if (!integer || n > 1) return false;
      out = n != 0;
      return true;
    };
    SpirvShaderTranslator::Features& f = features;
    if (key == "spirv_version") return integer && (f.spirv_version = unsigned(n), true);
    if (key == "max_storage_buffer_range")
      return integer && (f.max_storage_buffer_range = uint32_t(n), true);
    if (key == "full_draw_index_uint32") return flag(f.full_draw_index_uint32);
    if (key == "vertex_pipeline_stores_and_atomics") return flag(f.vertex_pipeline_stores_and_atomics);
    if (key == "fragment_stores_and_atomics") return flag(f.fragment_stores_and_atomics);
    if (key == "clip_distance") return flag(f.clip_distance);
    if (key == "cull_distance") return flag(f.cull_distance);
    if (key == "image_view_format_swizzle") return flag(f.image_view_format_swizzle);
    if (key == "signed_zero_inf_nan_preserve_float32")
      return flag(f.signed_zero_inf_nan_preserve_float32);
    if (key == "denorm_flush_to_zero_float32") return flag(f.denorm_flush_to_zero_float32);
    if (key == "rounding_mode_rte_float32") return flag(f.rounding_mode_rte_float32);
    if (key == "fragment_shader_sample_interlock") return flag(f.fragment_shader_sample_interlock);
    if (key == "demote_to_helper_invocation") return flag(f.demote_to_helper_invocation);
    if (key == "sample_rate_shading") return flag(f.sample_rate_shading);
    if (key == "implicit_cube_lod") return flag(f.implicit_cube_lod);
    if (key == "fh1_native_viewport") return flag(f.fh1_native_viewport);
    if (key == "pixel_shader_contraction") return flag(f.pixel_shader_contraction);
    if (key == "native_2x_msaa_with_attachments") return flag(native_2x_msaa_with_attachments);
    if (key == "native_2x_msaa_no_attachments") return flag(native_2x_msaa_no_attachments);
    if (key == "edram_fragment_shader_interlock") return flag(edram_fragment_shader_interlock);
    if (key == "draw_resolution_scale_x") return integer && n && (draw_resolution_scale_x = uint32_t(n), true);
    if (key == "draw_resolution_scale_y") return integer && n && (draw_resolution_scale_y = uint32_t(n), true);
    if (key == "raster_scale_x" || key == "raster_scale_y") {
      const float v = std::strtof(value.c_str(), &end);
      if (!end || *end || value.empty() || !(v > 0.0f)) return false;
      (key == "raster_scale_x" ? raster_scale_x : raster_scale_y) = v;
      return true;
    }
    if (key == "fh1_dirt_seam_fix") return flag(dirt_seam_fix);
    // A configuration from another translator version describes other SPIR-V.
    if (key == "modification_version") return integer && (modification_version = uint32_t(n), true);
    return false;
  }

  bool SetAll(std::string_view words) {
    std::istringstream in{std::string(words)};
    std::string word;
    while (in >> word) {
      if (!Set(word)) {
        std::fprintf(stderr, "configuration: cannot use \"%s\"\n", word.c_str());
        return false;
      }
    }
    return true;
  }

  std::unique_ptr<SpirvShaderTranslator> MakeTranslator() const {
    return std::make_unique<SpirvShaderTranslator>(
        features, native_2x_msaa_with_attachments, native_2x_msaa_no_attachments,
        edram_fragment_shader_interlock, draw_resolution_scale_x, draw_resolution_scale_y,
        raster_scale_x, raster_scale_y);
  }
};

std::optional<std::vector<uint8_t>> ReadFile(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::nullopt;
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

struct Stored {
  xenos::ShaderType type;
  std::vector<uint32_t> ucode;  // guest (big-endian) order, as stored
};

// <title>.xsh, read as VulkanPipelineCache::InitializeShaderStorage reads it: up to the first
// record whose hash does not match.
bool ReadShaderStore(const std::filesystem::path& path, std::map<uint64_t, Stored>& out) {
  auto data = ReadFile(path);
  if (!data || data->size() < 8) return false;
  uint32_t magic, version;
  std::memcpy(&magic, data->data(), 4);
  std::memcpy(&version, data->data() + 4, 4);
  if (magic != 0x48534558 || std::byteswap(version) != VulkanPipelineCache::kShaderStoreVersion) {
    std::fprintf(stderr, "%s: not a shader store of this version\n", path.string().c_str());
    return false;
  }
  static_assert(VulkanPipelineCache::kShaderStoreRecordHeaderSize == 12);
  size_t o = 8;
  while (o + 12 <= data->size()) {
    uint64_t hash;
    uint32_t word;
    std::memcpy(&hash, data->data() + o, 8);
    std::memcpy(&word, data->data() + o + 8, 4);
    const uint32_t count = word & 0x7FFFFFFF;
    const size_t bytes = size_t(count) * 4;
    if (o + 12 + bytes > data->size()) break;
    const uint8_t* ucode = data->data() + o + 12;
    if (XXH3_64bits(ucode, bytes) != hash) break;
    Stored s{(word >> 31) ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex,
             std::vector<uint32_t>(count)};
    std::memcpy(s.ucode.data(), ucode, bytes);
    out.emplace(hash, std::move(s));
    o += 12 + bytes;
  }
  return true;
}

bool ReadPairs(const std::filesystem::path& path, std::set<std::pair<uint64_t, uint64_t>>& out) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line;
  size_t number = 0;
  while (std::getline(f, line)) {
    ++number;
    if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::istringstream in(line);
    std::string h, m;
    if (!(in >> h)) continue;
    if (!(in >> m)) {
      std::fprintf(stderr, "%s:%zu: want \"<hash> <modification>\"\n", path.string().c_str(),
                   number);
      return false;
    }
    out.emplace(std::stoull(h, nullptr, 16), std::stoull(m, nullptr, 16));
  }
  return true;
}

// The pairs the warm-up translates for a pipeline store: every vertex shader, and the pixel
// shader where there is one.
bool ReadPipelineStore(const std::filesystem::path& path, uint32_t api,
                       std::set<std::pair<uint64_t, uint64_t>>& out) {
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (!file) return false;
  const store::Header header =
      store::ReadHeader(file, api, VulkanPipelineCache::kPipelineStoreVersions);
  if (header == store::Header::kNone) {
    std::fclose(file);
    std::fprintf(stderr, "%s: not a pipeline store of this version and path (%s)\n",
                 path.string().c_str(), api ? "fsi" : "fbo");
    return false;
  }
  std::fseek(file, long(store::HeaderSize(header)), SEEK_SET);
  constexpr size_t kRecord = VulkanPipelineCache::kPipelineStoreRecordSize;
  std::vector<uint8_t> record(kRecord);
  while (std::fread(record.data(), kRecord, 1, file) == 1) {
    uint64_t description_hash, q[4];
    std::memcpy(&description_hash, record.data(), 8);
    if (XXH3_64bits(record.data() + 8, kRecord - 8) != description_hash) break;
    std::memcpy(q, record.data() + VulkanPipelineCache::kPipelineStoreShadersOffset, sizeof(q));
    out.emplace(q[0], q[1]);
    if (q[2]) out.emplace(q[2], q[3]);
  }
  std::fclose(file);
  return true;
}

struct Result {
  std::vector<uint8_t> binary;
  bool valid = false;
  double analyze_us = 0, translate_us = 0;
};

// A first use, as in the warm-up: a new Shader, its analysis, one translation.
Result Translate(SpirvShaderTranslator& translator, uint64_t hash, const Stored& stored,
                 uint64_t modification, rex::string::StringBuffer& disassembly) {
  Result r;
  const auto t0 = std::chrono::steady_clock::now();
  Shader shader(stored.type, hash, stored.ucode.data(), stored.ucode.size(), std::endian::big);
  shader.AnalyzeUcode(disassembly);
  const auto t1 = std::chrono::steady_clock::now();
  Shader::Translation* translation = shader.GetOrCreateTranslation(modification);
  r.valid = translator.TranslateAnalyzedShader(*translation);
  const auto t2 = std::chrono::steady_clock::now();
  r.analyze_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
  r.translate_us = std::chrono::duration<double, std::micro>(t2 - t1).count();
  r.binary = translation->translated_binary();
  return r;
}

int TranslateCommand(int argc, char** argv) {
  std::filesystem::path store_path, out_dir;
  std::vector<std::filesystem::path> pair_files, pipeline_files;
  bool defaults = false, check = false, print_config = false;
  int repeat = 1;
  Configuration config;
  for (int i = 0; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (a == "--store" && (v = next())) store_path = v;
    else if (a == "--pairs" && (v = next())) pair_files.emplace_back(v);
    else if (a == "--pipelines" && (v = next())) pipeline_files.emplace_back(v);
    else if (a == "--defaults") defaults = true;
    else if (a == "--config" && (v = next())) {
      if (!config.SetAll(v)) return 2;
    } else if (a == "--set" && (v = next())) {
      if (!config.Set(v)) {
        std::fprintf(stderr, "--set: cannot use \"%s\"\n", v);
        return 2;
      }
    } else if (a == "--out" && (v = next())) out_dir = v;
    else if (a == "--repeat" && (v = next())) repeat = std::max(1, std::atoi(v));
    else if (a == "--check") check = true;
    else if (a == "--print-config") print_config = true;
    else return Usage();
  }
  if (config.modification_version != SpirvShaderTranslator::Modification::kVersion) {
    std::fprintf(stderr,
                 "the configuration is for modification version %#x; this translator has %#x\n",
                 config.modification_version, SpirvShaderTranslator::Modification::kVersion);
    return 2;
  }
  REXCVAR_SET(fh1_dirt_seam_fix, config.dirt_seam_fix);
  std::unique_ptr<SpirvShaderTranslator> translator = config.MakeTranslator();
  if (print_config) {
    std::printf("%s\n", translator->DescribeConfiguration().c_str());
    return 0;
  }
  if (store_path.empty() || (pair_files.empty() && pipeline_files.empty() && !defaults)) {
    return Usage();
  }
  std::map<uint64_t, Stored> stored;
  if (!ReadShaderStore(store_path, stored)) return 2;
  std::set<std::pair<uint64_t, uint64_t>> pairs;
  for (const auto& p : pair_files) {
    if (!ReadPairs(p, pairs)) {
      std::fprintf(stderr, "%s: cannot read the pairs\n", p.string().c_str());
      return 2;
    }
  }
  for (const auto& p : pipeline_files) {
    if (!ReadPipelineStore(p, config.edram_fragment_shader_interlock ? 1u : 0u, pairs)) return 2;
  }
  if (defaults) {
    for (const auto& [hash, s] : stored) {
      pairs.emplace(hash, s.type == xenos::ShaderType::kPixel
                              ? translator->GetDefaultPixelShaderModification(0)
                              : translator->GetDefaultVertexShaderModification(0));
    }
  }
  if (!out_dir.empty()) std::filesystem::create_directories(out_dir);
  std::fprintf(stderr, "%zu shaders in the store, %zu pairs\n", stored.size(), pairs.size());

  rex::string::StringBuffer disassembly;
  std::printf("hash,modification,type,ucode_dwords,analyze_us,translate_us,spirv_bytes,valid,"
              "spirv_xxh3%s\n",
              check ? ",identical" : "");
  size_t ok = 0, failed = 0, missing = 0, mismatched = 0;
  double total_us = 0;
  for (const auto& [hash, modification] : pairs) {
    auto it = stored.find(hash);
    if (it == stored.end()) {
      ++missing;
      continue;
    }
    Result best;
    for (int r = 0; r < repeat; ++r) {
      Result result = Translate(*translator, hash, it->second, modification, disassembly);
      if (r == 0 || result.translate_us < best.translate_us) best = std::move(result);
    }
    int identical = -1;
    if (check) {
      // A new translator too: nothing carried over from the first translation.
      std::unique_ptr<SpirvShaderTranslator> again = config.MakeTranslator();
      Result second = Translate(*again, hash, it->second, modification, disassembly);
      identical = second.valid == best.valid && second.binary == best.binary;
      mismatched += !identical;
    }
    total_us += best.analyze_us + best.translate_us;
    const bool pixel = it->second.type == xenos::ShaderType::kPixel;
    std::printf("%016llX,%016llX,%s,%zu,%.1f,%.1f,%zu,%d,%016llX", (unsigned long long)hash,
                (unsigned long long)modification, pixel ? "ps" : "vs", it->second.ucode.size(),
                best.analyze_us, best.translate_us, best.binary.size(), int(best.valid),
                (unsigned long long)XXH3_64bits(best.binary.data(), best.binary.size()));
    if (check) std::printf(",%d", identical);
    std::printf("\n");
    if (best.valid && !best.binary.empty()) {
      ++ok;
      if (!out_dir.empty()) {
        char name[64];
        std::snprintf(name, sizeof(name), "%016llX_%016llX.%s.spv", (unsigned long long)hash,
                      (unsigned long long)modification, pixel ? "ps" : "vs");
        std::ofstream(out_dir / name, std::ios::binary)
            .write(reinterpret_cast<const char*>(best.binary.data()),
                   std::streamsize(best.binary.size()));
      }
    } else {
      ++failed;
    }
  }
  std::fprintf(stderr,
               "translated %zu, failed %zu, not in the store %zu%s, %.1f ms (analysis and "
               "translation%s)\n",
               ok, failed, missing,
               check ? (mismatched ? ", NOT DETERMINISTIC" : ", all identical twice") : "",
               total_us / 1000.0, repeat > 1 ? ", fastest of each" : "");
  if (check && mismatched) std::fprintf(stderr, "%zu pairs differed between two translations\n", mismatched);
  return failed || mismatched ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string_view(argv[1]) == "translate") {
    return TranslateCommand(argc - 2, argv + 2);
  }
  return Usage();
}
