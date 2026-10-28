# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Unit tests for core components
add_subdirectory(unit)
add_subdirectory(mnk-fast-taps)

# Header-only FH1 raster policy: no game data, GPU, or runtime library needed.
add_executable(fh1_screen_raster_test fh1_screen_raster_test.cpp)
target_include_directories(fh1_screen_raster_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME fh1.screen_raster COMMAND fh1_screen_raster_test)

# PPC instruction tests (requires PPC toolchain)
add_subdirectory(ppc)

add_executable(wheel_gear_transit_test wheel_gear_transit_test.cpp)
target_include_directories(wheel_gear_transit_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME input.wheel_gear_transit COMMAND wheel_gear_transit_test)

add_executable(wheel_rumble_pace_test wheel_rumble_pace_test.cpp)
target_include_directories(wheel_rumble_pace_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME input.wheel_rumble_pace COMMAND wheel_rumble_pace_test)
