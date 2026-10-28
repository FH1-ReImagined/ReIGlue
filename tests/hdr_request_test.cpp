// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#include <rex/ui/vulkan/hdr_format.h>
#include <rex/ui/vulkan/hdr_request.h>
#include <array>
#include <cassert>

using namespace rex::ui::vulkan;
int main() {
  struct Format { int format, colorSpace; };
  constexpr Format sdr{1, 1}, hdr{2, 2};
  HdrConnectionRequest connection;
  assert(connection.NeedsConnection(false));
  connection.Commit(false);
  assert(!connection.NeedsConnection(false));
  assert(connection.NeedsConnection(true));

  // A failed construction leaves the last successful connection's request.
  auto fallback = ChooseHdrFormat(std::array{sdr}, sdr, true, true, hdr);
  assert(!fallback.active && !fallback.available);
  assert(connection.NeedsConnection(true));
  // Successful SDR fallback consumes an HDR request without a repaint loop.
  connection.Commit(true);
  for (int frame = 0; frame < 1000; ++frame)
    assert(!connection.NeedsConnection(true));
  // Turning off and on retries even when previous capability was unavailable.
  assert(connection.NeedsConnection(false));
  connection.Commit(false);
  assert(connection.NeedsConnection(true));
  auto enabled = ChooseHdrFormat(std::array{sdr, hdr}, sdr, true, true, hdr);
  assert(enabled.active && enabled.available);
  connection.Commit(true);
  assert(!connection.NeedsConnection(true));
  // A request changed during construction must be handled on the next paint.
  assert(connection.NeedsConnection(false));
  connection.Commit(false);
  assert(!connection.NeedsConnection(false));
  // An external reconnect re-evaluates capability even for the same request.
  connection.Commit(false);
  assert(!connection.NeedsConnection(false));
}
