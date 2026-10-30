# Loaded-only library probe

This small test compiles the production platform loader with a fixture shared
library; it needs no SDK dependencies, game, RenderDoc installation or GPU.

```sh
cmake -S tests/dynlib-loaded-only -B build/dynlib-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/dynlib-tests --config Release --parallel 1
ctest --test-dir build/dynlib-tests -C Release --output-on-failure
```

The fixture is present on disk but initially absent from the process. The test
checks that probing leaves it unloaded, then explicitly loads it and checks
symbol access, retained ownership after the initial loader closes, move ownership,
failed replacement, and release of the last reference. Assertions remain enabled
in Release builds. CI runs this test on Linux, Windows and macOS.

RenderDoc uses this probe instead of loading an installed library. The POSIX
implementation uses `RTLD_NOLOAD`; Windows uses `GetModuleHandleExW` with a counted
reference. The macOS RenderDoc entry point remains disabled. The approach follows
Pinyon Shift's already-loaded RenderDoc probe; see `NOTICE` and
`LICENSE-Pinyon-Shift`. This test does not qualify interactive capture behavior.
