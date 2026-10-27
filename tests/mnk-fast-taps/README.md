# Keyboard taps between controller polls

This test calls the real `MnkInputDriver` through its window-listener and
controller APIs. It needs no window, physical controller, input injection, or
guest memory. The stock driver loses a complete Return press/release before the
first state sample; the first assertion reproduces that failure.

Digital controller bindings retain a short tap for 34 ms after the first sample,
so a title polling several times per frame can observe it. An unsampled tap
expires after 100 ms. Repeated taps inside a retained pulse coalesce without
extending it or replaying an automatic second action. Steering directions and
analog triggers keep their existing behavior. Raw keyboard passthrough and the
keystroke queue do not receive additional events.

Coverage includes multiple polls per frame, a first sample at 99 ms, stale and
repeated taps, long holds and immediate releases, focus loss/regain without an
intervening sample, inactive overlay interaction, disable/re-enable, passthrough,
and modifier chords. The fixture is included by `REXGLUE_BUILD_TESTS` and can
also use an installed SDK:

```sh
cmake -S tests/mnk-fast-taps -B out/mnk-fast-taps -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/sdk
cmake --build out/mnk-fast-taps
ctest --test-dir out/mnk-fast-taps --output-on-failure
```

Match the installed SDK configuration. The reusable platform workflow installs
a separate Release prefix for this fixture, leaving the packaging prefix alone.

The change adds private storage to `MnkInputDriver`; rebuild its consumers,
including `input_system.cpp`, which allocates the driver. Replacing only the
driver object against an allocator compiled with its old size is unsafe.
