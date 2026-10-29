// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#include <rex/ui/vulkan/hdr_format.h>
#include <rex/ui/vulkan/hdr_request.h>
#include <rex/ui/vulkan/hdr_status.h>

#include <array>
#include <atomic>
#include <cassert>
#include <thread>

using namespace rex::ui::vulkan;

int main() {
  HdrOutputPublication status;
  const auto expect = [&status](bool offered, bool active) {
    const auto state = status.Read();
    assert(state.offered == offered && state.active == active);
  };
  expect(false, false);

  struct Format { int format, colorSpace; };
  constexpr Format sdr{1, 1}, hdr{2, 2};
  const auto enabled = ChooseHdrFormat(std::array{sdr, hdr}, sdr, true, true, hdr);
  // Discovery alone must not publish readiness. Construction may still fail.
  expect(false, false);
  status.CommitConnection(enabled.available, enabled.active);
  expect(true, true);

  // A failed reconnect or retirement must not retain either old observation.
  status.Clear();
  expect(false, false);
  status.Clear();
  expect(false, false);

  // Request acknowledgement, capability and actual output remain distinct.
  HdrConnectionRequest request;
  const auto fallback = ChooseHdrFormat(std::array{sdr}, sdr, true, true, hdr);
  status.CommitConnection(fallback.available, fallback.active);
  request.Commit(true);
  expect(false, false);
  assert(!request.NeedsConnection(true));
  status.Clear();
  const auto disabled = ChooseHdrFormat(std::array{sdr, hdr}, sdr, true, false, hdr);
  status.CommitConnection(disabled.available, disabled.active);
  expect(true, false);
  status.Clear();
  expect(false, false);

  // Bad observations fail closed; readers must never see active but unoffered.
  status.CommitConnection(false, true);
  expect(false, false);
  std::atomic<bool> start{false}, done{false};
  std::thread writer([&] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    for (int i = 0; i < 100000; ++i) {
      status.CommitConnection(true, true);
      status.Clear();
      status.CommitConnection(true, false);
      status.CommitConnection(false, false);
    }
    done.store(true, std::memory_order_release);
  });
  start.store(true, std::memory_order_release);
  do {
    const auto state = status.Read();
    assert(!state.active || state.offered);
  } while (!done.load(std::memory_order_acquire));
  writer.join();
  expect(false, false);
}
