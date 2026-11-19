#pragma once

#include <cstdlib>
#include <cstring>

namespace rex::graphics::fh1_list {

inline bool SustainedReplayRequested() {
  static const bool requested = [] {
    const char* audit = std::getenv("FH1_M4_BINDING_AUDIT");
    const char* reuse = std::getenv("FH1_M4_REUSE_SNAPSHOT");
    const char* sustained = std::getenv("FH1_M4_SUSTAINED_REPLAY");
    const char* parity = std::getenv("FH1_M4_REPLAY_PIXEL_PARITY");
    return audit && std::strcmp(audit, "timed-replay") == 0 && reuse &&
           std::strcmp(reuse, "1") == 0 && sustained && std::strcmp(sustained, "1") == 0 &&
           !(parity && (std::strcmp(parity, "1") == 0 || std::strcmp(parity, "texture") == 0));
  }();
  return requested;
}

}  // namespace rex::graphics::fh1_list
