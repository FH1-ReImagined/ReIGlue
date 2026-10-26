# rexgpu-xenos: Xenos GPU emulation plugin (ported from Xenia)
# Namespace: rex::graphics
#
# Runtime-loaded via rex::system::LoadGpuPlugin; consumers never link this
# target. The Xenos format layer is compiled into rexcore.

set(REXGPU_XENOS_SOURCES
    plugin_main.cpp

    # Core GPU types
    flags.cpp
    register_file.cpp
    registers.cpp
    pipeline/shader/shader.cpp
    format/ucode.cpp
    sampler_info.cpp

    # GPU system
    graphics_system.cpp
    command_processor.cpp
    util/draw_extent_estimator.cpp
    util/draw.cpp
    packet_disassembler.cpp
    primitive_processor.cpp
    pipeline/render_target/cache.cpp
    shared_memory.cpp
    pipeline/texture/cache.cpp
    pipeline/texture/pack.cpp
    pipeline/texture/pack_format.cpp
    pipeline/shader/interpreter.cpp
    pipeline/shader/translator.cpp
    pipeline/shader/translator_disasm.cpp
)

# SPIR-V shader translation
set(REXGPU_XENOS_SPIRV_SOURCES
    pipeline/shader/spirv_builder.cpp
    pipeline/shader/spirv.cpp
    pipeline/shader/spirv_translator.cpp
    pipeline/shader/spirv_translator_alu.cpp
    pipeline/shader/spirv_translator_fetch.cpp
    pipeline/shader/spirv_translator_memexport.cpp
    pipeline/shader/spirv_translator_rb.cpp
)

# Vulkan backend
set(REXGPU_XENOS_VULKAN_SOURCES
    vulkan/graphics_system.cpp
    vulkan/deferred_command_buffer.cpp
    vulkan/fh1_device_memory.cpp
    vulkan/fh1_pipeline_lookahead.cpp
    vulkan/fh1_dlss.cpp
    vulkan/fh1_fsr3.cpp
    vulkan/fh1_smaa.cpp
    vulkan/fh1_world_ao.cpp
    vulkan/command_processor.cpp
    vulkan/pipeline_cache.cpp
    vulkan/primitive_processor.cpp
    vulkan/render_target_cache.cpp
    vulkan/shader.cpp
    vulkan/shared_memory.cpp
    vulkan/texture_cache.cpp
)

# D3D12 backend
if(REXGLUE_USE_D3D12)
    # DXBC third-party dependency
    set(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES
        ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    )

    # DXBC shader translation (required for D3D12)
    set(REXGPU_XENOS_DXBC_SOURCES
        pipeline/shader/dxbc.cpp
        pipeline/shader/dxbc_translator.cpp
        pipeline/shader/dxbc_translator_alu.cpp
        pipeline/shader/dxbc_translator_fetch.cpp
        pipeline/shader/dxbc_translator_memexport.cpp
        pipeline/shader/dxbc_translator_om.cpp
    )

    # D3D12 GPU backend
    set(REXGPU_XENOS_D3D12_SOURCES
        d3d12/graphics_system.cpp
        d3d12/command_processor.cpp
        d3d12/primitive_processor.cpp
        d3d12/render_target_cache.cpp
        d3d12/shader.cpp
        d3d12/shared_memory.cpp
        d3d12/texture_cache.cpp
        d3d12/deferred_command_list.cpp
        d3d12/pipeline_cache.cpp
    )
endif()

if(REXGLUE_USE_VULKAN)
    list(APPEND REXGPU_XENOS_SOURCES ${REXGPU_XENOS_SPIRV_SOURCES} ${REXGPU_XENOS_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXGPU_XENOS_SOURCES
        ${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        ${REXGPU_XENOS_DXBC_SOURCES}
        ${REXGPU_XENOS_D3D12_SOURCES})
endif()

add_library(rexgpu-xenos SHARED ${REXGPU_XENOS_SOURCES})
add_library(rex::gpu-xenos ALIAS rexgpu-xenos)

if(REXGLUE_USE_VULKAN)
    include(${REXGLUE_ROOT}/cmake/ForzaSmaa.cmake)
endif()

if(APPLE AND REXGLUE_USE_VULKAN)
    target_sources(rexgpu-xenos PRIVATE vulkan/fh1_metalfx.mm)
    # Vulkan-Hpp uses constexpr Metal pointer members that are incompatible with ARC.
    set_source_files_properties(vulkan/fh1_metalfx.mm PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "-fno-objc-arc")
    target_link_libraries(rexgpu-xenos PRIVATE "-framework Metal" "-framework MetalFX" "-framework Foundation")
endif()

option(REXGLUE_USE_FSR3 "Build the FH1 AMD FidelityFX FSR 3 upscaler (fetches pinned MIT sources)" ON)
set(REXGLUE_FSR3_SDK "" CACHE PATH "Optional local FidelityFX SDK v1.1.4; otherwise fetch the pinned FSR 3 sources")
if(REXGLUE_USE_VULKAN AND REXGLUE_USE_FSR3)
    include(${REXGLUE_ROOT}/cmake/ForzaFsr3Sdk.cmake)
    target_compile_definitions(rexgpu-xenos PRIVATE REX_FH1_FSR3=1)
    target_include_directories(rexgpu-xenos PRIVATE "${FH1_FSR3_GENERATED}")
    add_custom_command(TARGET rexgpu-xenos POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FH1_FSR3_LICENSE}"
            "$<TARGET_FILE_DIR:rexgpu-xenos>/AMD-FidelityFX-LICENSE.txt")
    install(FILES "${FH1_FSR3_LICENSE}" DESTINATION share/licenses/amd-fidelityfx-fsr3)
endif()

option(REXGLUE_USE_DLSS "Build native FH1 DLSS support (NVIDIA SDK terms apply)" ${WIN32})
set(REXGLUE_DLSS_SDK "" CACHE PATH "Optional local NVIDIA DLSS SDK; otherwise fetch pinned SDK files")
if(WIN32 AND REXGLUE_USE_VULKAN AND REXGLUE_USE_DLSS)
    include(${REXGLUE_ROOT}/cmake/ForzaDlssSdk.cmake)
    target_compile_definitions(rexgpu-xenos PRIVATE REX_FH1_NGX=1)
    target_include_directories(rexgpu-xenos SYSTEM PRIVATE "${REXGLUE_DLSS_SDK}/include")
    target_link_libraries(rexgpu-xenos PRIVATE
        "${REXGLUE_DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    add_custom_command(TARGET rexgpu-xenos POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${REXGLUE_DLSS_SDK}/LICENSE.txt"
            "$<TARGET_FILE_DIR:rexgpu-xenos>/NVIDIA-DLSS-LICENSE.txt"
        COMMAND ${CMAKE_COMMAND}
            "-DFH1_GPU_DLL=$<TARGET_FILE:rexgpu-xenos>"
            "-DFH1_CAPABILITIES=$<TARGET_FILE_DIR:rexgpu-xenos>/fh1-native-dlss.json"
            -P "${REXGLUE_ROOT}/cmake/Fh1DlssCapabilities.cmake")
    install(FILES "$<TARGET_FILE_DIR:rexgpu-xenos>/fh1-native-dlss.json" DESTINATION bin)
    install(FILES "${REXGLUE_DLSS_SDK}/LICENSE.txt"
        DESTINATION share/licenses/nvidia-dlss)
endif()

set_target_properties(rexgpu-xenos PROPERTIES
    EXPORT_NAME gpu-xenos
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
rexglue_add_version_resource(rexgpu-xenos DESCRIPTION "ReXGlue Xenos GPU emulation")

target_include_directories(rexgpu-xenos PRIVATE
    ${REXGLUE_ROOT}/thirdparty/renderdoc
    ${REXGLUE_ROOT}/thirdparty/stb  # stb_image.h: texture pack PNG files (pack.cpp)
    ${REXGLUE_ROOT}  # For thirdparty/dxbc/DXBCChecksum.h include
)

target_compile_options(rexgpu-xenos PRIVATE -Wno-unused-variable)
# FH1 (#45, patch 0361): rex::bit_scan_forward, tzcnt and lzcnt inline in the plugin
# (include/rex/math.h) instead of calls into the runtime library.
target_compile_definitions(rexgpu-xenos PRIVATE REX_INLINE_BIT_SCANS=1)

rex_target_pch(rexgpu-xenos
    <mutex>
    <unordered_map>
    <rex/math.h>
    <rex/memory.h>
    <rex/thread/mutex.h>
    <rex/system/xmemory.h>
    <rex/graphics/xenos.h>
    <rex/graphics/registers.h>
    <rex/graphics/format/ucode.h>
    <rex/graphics/pipeline/shader/shader.h>
)

if(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES)
    set_source_files_properties(${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

# Everything PRIVATE: the plugin has no consumers. Never link the OBJECT
# libraries (rexcore/rexui) here; rexruntime already contains those objects
# and relinking them would duplicate global state.
target_link_libraries(rexgpu-xenos PRIVATE
    rexruntime
    xxHash::xxhash
)

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexgpu-xenos PRIVATE
        Vulkan::Headers
        glslang::SPIRV
        GPUOpen::VulkanMemoryAllocator
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler
    )
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexgpu-xenos PRIVATE
        d3d12
        dxgi
        dxguid
    )
endif()
