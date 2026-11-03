// The log file sink (0421, FH1 #609): it appends to an existing file, and with a size limit the
// file rolls over to <stem>.1<ext> and a new one starts, one roll kept, so the two stay under twice
// the limit however much is logged. 0 is the old unlimited file. Against the runtime, no game.
#include <rex/logging.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <spdlog/logger.h>

#define CHECK(x)                                                                 \
  do {                                                                           \
    if (!(x)) {                                                                  \
      std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #x); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

namespace fs = std::filesystem;

namespace {

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// `lines` lines of about 100 bytes through a logger on the sink, then the sink is closed.
void Log(const fs::path& path, uint64_t max_bytes, int lines) {
  auto logger = std::make_shared<spdlog::logger>("test", rex::MakeFileSink(path, max_bytes));
  logger->set_pattern("%v");
  for (int i = 0; i < lines; ++i) {
    logger->info("line {:06d} {}", i, std::string(88, 'x'));
  }
  logger->flush();
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "rex_log_file_sink_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path log = dir / "forza.log";
  const fs::path rolled = dir / "forza.1.log";

  // The default: 50 MB.
  CHECK(rex::LogFileMaxBytes() == 50ull * 1024 * 1024);

  // An existing file is appended to, not truncated (an old game may still be writing it).
  { std::ofstream(log, std::ios::binary) << "kept\n"; }
  Log(log, 64 * 1024, 10);
  CHECK(Read(log).rfind("kept\n", 0) == 0);
  CHECK(!fs::exists(rolled));

  // Past the limit the file rolls once per limit; only the newest roll is kept.
  Log(log, 64 * 1024, 5000);  // ~500 KB through a 64 KB limit
  CHECK(fs::exists(rolled));
  CHECK(fs::file_size(log) <= 64 * 1024);
  CHECK(fs::file_size(rolled) <= 64 * 1024);
  CHECK(!fs::exists(dir / "forza.2.log"));
  const std::string last = Read(log);
  CHECK(last.find("line 004999") != std::string::npos);  // the newest lines are in the file
  CHECK(Read(rolled).find("line 000000") == std::string::npos);
  // The roll holds the lines just before the file's first one.
  const std::string first_kept = last.substr(5, 6);
  const int first = std::stoi(first_kept);
  char before[16];
  std::snprintf(before, sizeof(before), "line %06d", first - 1);
  CHECK(Read(rolled).find(before) != std::string::npos);

  // 0: no limit, the old basic file.
  fs::remove(log);
  fs::remove(rolled);
  Log(log, 0, 5000);
  CHECK(fs::file_size(log) > 400 * 1024);
  CHECK(!fs::exists(rolled));

  fs::remove_all(dir);
  std::puts("log_file_sink_test: ok");
  return 0;
}
