# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Unit tests for core components
add_subdirectory(unit)

# Header-only FH1 raster policy: no game data, GPU, or runtime library needed.
add_executable(fh1_screen_raster_test fh1_screen_raster_test.cpp)
target_include_directories(fh1_screen_raster_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME fh1.screen_raster COMMAND fh1_screen_raster_test)

# PPC instruction tests (requires PPC toolchain)
add_subdirectory(ppc)
