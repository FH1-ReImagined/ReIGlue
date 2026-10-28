// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

namespace rex::ui::vulkan {
// A successful connection consumes the request it evaluated, including an SDR
// fallback. Actual output and advertised capability are separate observations.
// Failed construction must not commit; surface changes may retry independently.
class HdrConnectionRequest {
 public:
  constexpr bool NeedsConnection(bool requested) const {
    return !connected_ || requested != accepted_request_;
  }
  constexpr void Commit(bool requested) {
    accepted_request_ = requested;
    connected_ = true;
  }

 private:
  bool connected_ = false;
  bool accepted_request_ = false;
};
}  // namespace rex::ui::vulkan
