#ifdef NDEBUG
#undef NDEBUG
#endif
#include <rex/graphics/util/draw_trace_budget.h>
#include <cassert>
#include <string>
int main() {
#ifdef _WIN32
  FILE* file = nullptr;
  assert(tmpfile_s(&file) == 0);
#else
  FILE* file = std::tmpfile();
#endif
  assert(file);
  rex::graphics::DrawTraceBudget bytes(100, 7);
  assert(!bytes.Write(nullptr, 1, "disabled"));
  assert(std::ftell(file) == 0);
  assert(bytes.Write(file, 1, "%s", "abc"));
  assert(!bytes.Write(file, 1, "%s", "12345"));
  assert(bytes.Write(file, 1, "%s", "1234"));
  assert(!bytes.Write(file, 1, "%s", "x"));
  assert(std::ftell(file) == 7);
  assert(bytes.Write(file, 2, "%s", "abcdefg"));
  assert(std::ftell(file) == 14);
  rex::graphics::DrawTraceBudget records(2, 1024 * 1024);
  assert(records.Write(file, 2, "%s", "a"));
  assert(records.Write(file, 2, "%s", "b"));
  assert(!records.Write(file, 2, "%s", "c"));
  assert(std::ftell(file) == 16);
  const std::string oversize(1024, 'x');
  assert(!records.Write(file, 3, "%s", oversize.c_str()));
  assert(std::ftell(file) == 16);  // A truncated record must never be published.
  assert(records.Write(file, 3, "draw=%u\n", 42u));
  assert(std::ftell(file) == 24);
  std::fclose(file);
}
