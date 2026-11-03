# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Unit tests for core components
add_subdirectory(unit)
add_subdirectory(mnk-fast-taps)
add_subdirectory(dynlib-loaded-only)

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

# linux_thread_cores: core lists, name matching and the auto layout, header only.
add_executable(thread_cores_test thread_cores_test.cpp)
target_include_directories(thread_cores_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME thread.cores COMMAND thread_cores_test)

add_executable(wheel_gear_transit_test wheel_gear_transit_test.cpp)
target_include_directories(wheel_gear_transit_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME input.wheel_gear_transit COMMAND wheel_gear_transit_test)

add_executable(wheel_rumble_pace_test wheel_rumble_pace_test.cpp)
target_include_directories(wheel_rumble_pace_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME input.wheel_rumble_pace COMMAND wheel_rumble_pace_test)

# The presenter's publish deferrer (#476, 0419): a fake presenter against the runtime, no GPU.
add_executable(presenter_publish_deferrer_test presenter_publish_deferrer_test.cpp)
target_include_directories(presenter_publish_deferrer_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(presenter_publish_deferrer_test PRIVATE rexruntime)
# Next to the runtime library, which Windows loads from the program's folder.
set_target_properties(presenter_publish_deferrer_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>)
add_test(NAME ui.presenter_publish_deferrer COMMAND presenter_publish_deferrer_test)

# The log file's size limit (0421, FH1 #609): rolls over to <stem>.1<ext>, one roll kept.
add_executable(log_file_sink_test log_file_sink_test.cpp)
target_include_directories(log_file_sink_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(log_file_sink_test PRIVATE rexruntime)
set_target_properties(log_file_sink_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>)
add_test(NAME core.log_file_sink COMMAND log_file_sink_test)
