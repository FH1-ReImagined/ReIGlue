# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Unit tests for core components
add_subdirectory(unit)
add_subdirectory(mnk-fast-taps)

# PPC instruction tests (requires PPC toolchain)
add_subdirectory(ppc)
