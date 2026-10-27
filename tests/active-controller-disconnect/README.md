# Active controller disconnect sequence (0399)

`ForzaInputDisconnectSequence(mask)` is an optional read-only C export for the FH1 client
auto-pause feature. Each selected guest user's counter advances when the physical device
last used by that user disappears during the SDK's normal enumeration. Keyboard/NOP
emulation may still leave the merged connected-user mask set. Idle spare devices,
synthetic-device removal and reconnecting alone do not advance the counter.

The export performs atomic reads only; it does not enumerate devices or change class
layout/vtables. The client discovers it dynamically and preserves its older-SDK fallback.
Unsigned counter wrap remains an event edge.

After building/installing this branch's runtime:

```sh
cmake -S tests/active-controller-disconnect -B out/active-disconnect -DCMAKE_PREFIX_PATH=/path/to/sdk
cmake --build out/active-disconnect --parallel 1
ctest --test-dir out/active-disconnect --output-on-failure
```

The fixture uses the actual SDK `InputSystem` enumeration, assignment and active-device
tracking with a deterministic driver. Its completion marker prevents early exit from
counting as success. It is not a physical USB/Bluetooth, wheel, focus or multiplayer test.
Those remain acceptance gates in FH1 PR #331. No production revision tag is created here.
