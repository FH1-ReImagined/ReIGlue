#pragma once
#include <chrono>
#include <string_view>
#include <rex/logging.h>

namespace rex::graphics {
// Startup runs before the guest resumes. Keep progress in the existing log so
// frontends can display it without an additional ABI or a public network API.
class ShaderWarmupLog {
 public:
  explicit ShaderWarmupLog(std::string_view backend) : backend_(backend) {
    REXGPU_INFO("SHADER_WARMUP: {} begin", backend_);
  }
  ~ShaderWarmupLog() {
    REXGPU_INFO("SHADER_WARMUP: {} complete in {} ms", backend_,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_).count());
  }
  void Progress(size_t done, size_t total) const {
    REXGPU_INFO("SHADER_WARMUP: {} progress {}/{}", backend_, done, total);
  }
 private:
  std::string_view backend_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};
}  // namespace rex::graphics
