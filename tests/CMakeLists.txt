# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Unit tests for core components
add_subdirectory(unit)
add_subdirectory(mnk-fast-taps)

# Header-only FH1 raster policy: no game data, GPU, or runtime library needed.
add_executable(fh1_screen_raster_test fh1_screen_raster_test.cpp)
target_include_directories(fh1_screen_raster_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME fh1.screen_raster COMMAND fh1_screen_raster_test)

find_package(Threads REQUIRED)
add_executable(pipeline_retirement_test pipeline_retirement_test.cpp)
target_include_directories(pipeline_retirement_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(pipeline_retirement_test PRIVATE Threads::Threads)
add_test(NAME vulkan.pipeline_retirement COMMAND pipeline_retirement_test)

# PPC instruction tests (requires PPC toolchain)
add_subdirectory(ppc)
