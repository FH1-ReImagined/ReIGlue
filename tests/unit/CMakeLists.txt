# Unit Tests for ReXGlue core components

add_executable(unit_tests
    test_memory.cpp
    memory/heap_allocation_test.cpp
    kernel/object_table_test.cpp
    core/cvar_test.cpp
    core/fiber_test.cpp
    core/filesystem_test.cpp
    core/vfs_overwrite_test.cpp
    core/hash_test.cpp
    core/stream_test.cpp
    core/chrono_test.cpp
    ppc/import_function_test.cpp
    ppc/stack_test.cpp
    system/guest_path_test.cpp
    system/achievement_manager_test.cpp
    system/function_dispatcher_test.cpp
    input/input_assignment_test.cpp
    codegen/codegen_writer_test.cpp
    codegen/output_partition_test.cpp
    codegen/output_stamp_test.cpp
)

target_sources(unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/rexglue/ui/ui.cpp
    ${PROJECT_SOURCE_DIR}/src/rexglue/ui/progress.cpp
)

target_include_directories(unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(unit_tests PRIVATE
    rexcodegen
    rexruntime
    Catch2::Catch2WithMain
)

# ReXApp is shipped as source for recomp projects, rather than compiled into
# rexui. Keep its SDK-consumer compilation path covered by the test build.
add_library(rexapp_compile_test OBJECT
    ${PROJECT_SOURCE_DIR}/src/ui/rex_app.cpp
)
target_link_libraries(rexapp_compile_test PRIVATE
    rexruntime
    rexui
    rexinput
    rexaudio
)
add_dependencies(unit_tests rexapp_compile_test)

# Strip Tracy/profiling instrumentation from test targets
target_compile_options(unit_tests PRIVATE
    -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING
    -UREXGLUE_ENABLE_PERF_COUNTERS -UREXGLUE_PROFILE_GUEST_FUNCTIONS
)

set_target_properties(unit_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)
if(UNIX AND NOT APPLE)
    set_target_properties(unit_tests PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON
    )
endif()

# Discover tests
include(Catch)
catch_discover_tests(unit_tests
    TEST_PREFIX "unit."
    PROPERTIES
        LABELS "unit"
        TIMEOUT 60
)

# The 0xE0000000 view at a forced 4, 16 and 64KB view offset granularity (0500, FH1 #788): one
# process per granularity, as each maps its own 4.5GB guest memory.
add_executable(physical_view_tests memory/physical_view_test.cpp)
target_include_directories(physical_view_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(physical_view_tests PRIVATE rexruntime Catch2::Catch2WithMain)
target_compile_options(physical_view_tests PRIVATE
    -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING
    -UREXGLUE_ENABLE_PERF_COUNTERS -UREXGLUE_PROFILE_GUEST_FUNCTIONS
)
set_target_properties(physical_view_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)
if(UNIX AND NOT APPLE)
    set_target_properties(physical_view_tests PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON
    )
endif()
foreach(_rex_view_case pure native 4k 16k 64k)
    add_test(NAME unit.memory.physical_view.${_rex_view_case}
             COMMAND physical_view_tests "[${_rex_view_case}]")
    set_tests_properties(unit.memory.physical_view.${_rex_view_case} PROPERTIES
        LABELS "unit" TIMEOUT 60)
endforeach()
