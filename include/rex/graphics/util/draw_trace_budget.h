// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace rex::graphics {
// Additional backend evidence is bounded independently of the existing raw
// packet tracer. The caller owns the FILE and its one-frame lifetime.
class DrawTraceBudget {
 public:
  explicit DrawTraceBudget(size_t max_records = 65536, size_t max_bytes = 16 * 1024 * 1024)
      : max_records_(max_records), max_bytes_(max_bytes) {}
  template <typename... Args>
  bool Write(FILE* stream, uint64_t frame, const char* format, Args... args) {
    if (!stream) return false;
    if (stream != stream_ || frame != frame_) {
      stream_ = stream;
      frame_ = frame;
      records_ = bytes_ = 0;
      failed_ = false;
    }
    if (failed_ || records_ >= max_records_ || bytes_ >= max_bytes_) return false;
    char line[1024];
    const int length = [&] {
      if constexpr (sizeof...(args) == 0) return std::snprintf(line, sizeof(line), "%s", format);
      else return std::snprintf(line, sizeof(line), format, args...);
    }();
    if (length <= 0 || size_t(length) >= sizeof(line) || size_t(length) > max_bytes_ - bytes_)
      return false;
    if (std::fwrite(line, 1, size_t(length), stream) != size_t(length)) {
      failed_ = true;
      return false;
    }
    ++records_;
    bytes_ += size_t(length);
    return true;
  }
 private:
  const size_t max_records_, max_bytes_;
  FILE* stream_ = nullptr;
  uint64_t frame_ = 0;
  size_t records_ = 0, bytes_ = 0;
  bool failed_ = false;
};
inline DrawTraceBudget& ThreadDrawTraceBudget() {
  static thread_local DrawTraceBudget budget;
  return budget;
}
}  // namespace rex::graphics
