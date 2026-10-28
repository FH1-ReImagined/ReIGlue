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

add_executable(hdr_format_test hdr_format_test.cpp)
target_include_directories(hdr_format_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME graphics.hdr_format COMMAND hdr_format_test)

add_executable(hdr_shader_contract_test hdr_shader_contract_test.cpp)
target_include_directories(hdr_shader_contract_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME graphics.hdr_shader_contract COMMAND hdr_shader_contract_test)
foreach(contract presentation_transform_test spirv_contraction_test hdr_color_test hdr_parameters_test hdr_format_test hdr_shader_contract_test)
  if(MSVC)
    target_compile_options(${contract} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${contract} PRIVATE -UNDEBUG)
  endif()
endforeach()

if(REXGLUE_USE_VULKAN)
  add_executable(color_snapshot_test color_snapshot_test.cpp)
  target_include_directories(color_snapshot_test PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/thirdparty/vulkan-headers/include)
  add_test(NAME graphics.color_snapshot COMMAND color_snapshot_test)
  add_executable(image_compute_test image_compute_test.cpp)
  target_include_directories(image_compute_test PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/thirdparty/vulkan-headers/include)
  if(MSVC)
    target_compile_options(image_compute_test PRIVATE /UNDEBUG)
  else()
    target_compile_options(image_compute_test PRIVATE -UNDEBUG)
  endif()
  add_test(NAME graphics.image_compute COMMAND image_compute_test)
endif()

add_executable(scene_capture_test scene_capture_test.cpp)
target_include_directories(scene_capture_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
if(MSVC)
  target_compile_options(scene_capture_test PRIVATE /UNDEBUG)
else()
  target_compile_options(scene_capture_test PRIVATE -UNDEBUG)
endif()
add_test(NAME graphics.scene_capture COMMAND scene_capture_test)

foreach(name hdr_calibration hdr_coverage draw_trace_budget)
  add_executable(${name}_test ${name}_test.cpp)
  target_include_directories(${name}_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
  if(MSVC)
    target_compile_options(${name}_test PRIVATE /UNDEBUG)
  else()
    target_compile_options(${name}_test PRIVATE -UNDEBUG)
  endif()
  add_test(NAME graphics.${name} COMMAND ${name}_test)
endforeach()
