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

add_executable(spirv_contraction_test spirv_contraction_test.cpp)
target_include_directories(spirv_contraction_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME graphics.spirv_contraction COMMAND spirv_contraction_test)

add_executable(presentation_transform_test presentation_transform_test.cpp)
target_include_directories(presentation_transform_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME graphics.presentation_transform COMMAND presentation_transform_test)

add_executable(hdr_color_test hdr_color_test.cpp)
target_include_directories(hdr_color_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME graphics.hdr_color COMMAND hdr_color_test)

add_executable(hdr_parameters_test hdr_parameters_test.cpp)
target_include_directories(hdr_parameters_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME graphics.hdr_parameters COMMAND hdr_parameters_test)
