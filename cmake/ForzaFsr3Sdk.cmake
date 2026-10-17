# AMD FidelityFX FSR 3.1.4 upscaler GPU sources (MIT), pinned to FidelityFX SDK v1.1.4.
# The GLSL passes and their headers are fetched once, verified by SHA-256 and embedded
# into the GPU library; the renderer compiles the one permutation it needs at startup
# with its built-in glslang. No AMD binaries or prebuilt shader permutations are used.
set(_fh1_fsr3_revision "c6efa6bf7f2027b3ec94f28578bb5965eabb9e55")
set(_fh1_fsr3_base "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/${_fh1_fsr3_revision}")
if(NOT REXGLUE_FSR3_SDK)
    set(REXGLUE_FSR3_SDK "${CMAKE_BINARY_DIR}/_deps/fidelityfx-fsr3-${_fh1_fsr3_revision}")
    set(_fh1_fsr3_download ON)
endif()
set(_fh1_fsr3_files
    "LICENSE.txt" "82cf74fc23885107c7f69249ac3f1dac55898ff68320eb2c753984cae6b2d78a"
    "sdk/include/FidelityFX/gpu/ffx_common_types.h" "a19560f837c09f0c990e98b5925b731135d908e372af41c5895cbfa70cc9e5c8"
    "sdk/include/FidelityFX/gpu/ffx_core.h" "e439aa5f516b2d2ebf070ff1d24561f04fa5f00262a2e4f760764a7d07de7997"
    "sdk/include/FidelityFX/gpu/ffx_core_glsl.h" "01afe5b65549b2c0951b494ca4bf570ee2aa89a64344e4c7d98ef86e6613ab42"
    "sdk/include/FidelityFX/gpu/ffx_core_gpu_common.h" "0e3c50cb0b4592c051d87e028a1dfa54fbd097942ebb3146964db801dec095d0"
    "sdk/include/FidelityFX/gpu/ffx_core_gpu_common_half.h" "d10466cbf027c8c23858a25f8e8cbbbf521d29f373db663c9354221651e9a498"
    "sdk/include/FidelityFX/gpu/ffx_core_portability.h" "a7c3a1c213ec083bec401181036cffe7a222af2c430c0bab6bd8c14e83e8a567"
    "sdk/include/FidelityFX/gpu/fsr1/ffx_fsr1.h" "5d608d9d0645da03a3aa6c729c5ad990c6f222053eab850b8e218607c1baaa9d"
    "sdk/include/FidelityFX/gpu/spd/ffx_spd.h" "d276f32ee18948cb053c991f3c8821f9664c93f12cb2483dd2978f333bca67b2"
    "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_accumulate.h" "7212a028c6ff3ca4a76f9d62b7f64f5f5adce0f108067d8995c7bc795234afbc"
    "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h" "875c2b8d8df93716fb46271886c768a9ef40c5ccc11ade39397952b4a4e54321"
    "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_common.h" "4898aa014fef875d3644b9b3bed03f31a963349e17ad553198f87ae8af82cb3f"
    "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_luma_instability.h" "82d13d21e161732e48b267d28906f067a8df306ead5aeeed65a1ef66b3e06003"
    "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_luma_pyramid.h" "fa6eeaf8197b214ee1853d73da3c66cd0c8fc24444a65237b0023bd68f10d887"
    "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_prepare_inputs.h" "acae0499b8805c363c105895afc28fc90e549b38c8bffdcbec1cf9e2f842abb0"
    "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_prepare_reactivity.h" "ddf7a6d9cd6e357328e972e0b654da977edcb2e337c992b06d6256eeae339e03"
    "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_rcas.h" "f1d8a9152bd3ceda8a10dafe44e0c6d27109b345fc4a5704f71c6bf23d6c5cb4"
    "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_reproject.h" "d2d01ea2f59658e5494414448f11435879bdac871c3ad7c84b951248bc223702"
    "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_resources.h" "10c6f7619a6c1ff9983fdff9bac565789b1cda52f97ef60b491498223e7b7097"
    "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_sample.h" "e8de656c71b15537ddd891b7485492b7829e6aec1dea96256fbc246c9a7c7366"
    "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_shading_change.h" "13e772ff9bd32244fb990704055040f8dbb945c014cc6f0fdbf553188b300e7e"
    "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_shading_change_pyramid.h" "b1e94d7cbb1b7d6e4556e4a080577a62fde07efbc8379b52223d322ed4b75912"
    "sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_upsample.h" "c51f3af61b43fd779a6685202886dbb5dab3bfea989f8b5060c8c9ab4a93207b"
    "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_prepare_inputs_pass.glsl" "bdf3b1ca0fdce968afb79f55d667960382139926eccdfdec46e08dbae3982657"
    "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_luma_pyramid_pass.glsl" "3e637a702c2cc6b6a222df4327a4ed565f731c6190e958e0ffec5b43741f7ea4"
    "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_shading_change_pyramid_pass.glsl" "ce34582f454925499523d30d2619e19aefd8f1c091545c37d4ebd8feea91cf55"
    "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_shading_change_pass.glsl" "246d24b76e292b7c8c5ba39dfc3a4dff1486701f5fccc6482284afac5a7daaf9"
    "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_prepare_reactivity_pass.glsl" "176f6aa5f2f437487222c7fc057ce858795a6677d6ac3e937a1ce36521c6ddfc"
    "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_luma_instability_pass.glsl" "423345d36c811237af7a41ef5e6220638f5159952e90fa084aa952ddd267581b"
    "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_accumulate_pass.glsl" "73b5be6db479475559d2374957b2854aa6d7f7ddadba76ef5e4ed7f248ea9564"
    "sdk/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_rcas_pass.glsl" "110b409d5e10b16e5a237c13932a707c1c07932bf790e814f1a9d76f02569ad7"
)
set(FH1_FSR3_GENERATED "${CMAKE_BINARY_DIR}/_deps/fh1-fsr3-sources")
set(_fh1_fsr3_inc "${FH1_FSR3_GENERATED}/fh1_fsr3_sources.inc")
set(_fh1_fsr3_stamp "${_fh1_fsr3_revision}")
if(EXISTS "${_fh1_fsr3_inc}.stamp")
    file(READ "${_fh1_fsr3_inc}.stamp" _fh1_fsr3_old)
endif()
if(NOT EXISTS "${_fh1_fsr3_inc}" OR NOT _fh1_fsr3_old STREQUAL "${_fh1_fsr3_stamp}")
    set(_fh1_fsr3_table "")
    set(_fh1_fsr3_data "")
    set(_index 0)
    list(LENGTH _fh1_fsr3_files _count)
    while(_index LESS _count)
        list(GET _fh1_fsr3_files ${_index} relative)
        math(EXPR _next "${_index} + 1")
        list(GET _fh1_fsr3_files ${_next} expected_hash)
        math(EXPR _index "${_index} + 2")
        set(source "${REXGLUE_FSR3_SDK}/${relative}")
        if(EXISTS "${source}")
            file(SHA256 "${source}" actual_hash)
        else()
            set(actual_hash "")
        endif()
        if(NOT actual_hash STREQUAL expected_hash)
            if(NOT _fh1_fsr3_download)
                message(FATAL_ERROR "REXGLUE_FSR3_SDK: ${relative} does not match FidelityFX SDK v1.1.4")
            endif()
            get_filename_component(parent "${source}" DIRECTORY)
            file(MAKE_DIRECTORY "${parent}")
            message(STATUS "Fetching pinned AMD FidelityFX FSR 3 source: ${relative}")
            file(DOWNLOAD "${_fh1_fsr3_base}/${relative}" "${source}.download"
                 EXPECTED_HASH "SHA256=${expected_hash}" TLS_VERIFY ON
                 TIMEOUT 180 STATUS download_status)
            list(GET download_status 0 download_code)
            if(NOT download_code EQUAL 0)
                message(FATAL_ERROR "FidelityFX FSR 3 download failed: ${download_status}. Retry with network access, set REXGLUE_FSR3_SDK to a FidelityFX SDK v1.1.4 checkout, or disable REXGLUE_USE_FSR3.")
            endif()
            file(RENAME "${source}.download" "${source}")
        endif()
        # Shader-relative names: headers below sdk/include/FidelityFX/gpu, passes by file name.
        string(REGEX REPLACE "^sdk/include/FidelityFX/gpu/" "" name "${relative}")
        string(REGEX REPLACE "^sdk/src/backends/vk/shaders/fsr3upscaler/" "" name "${name}")
        if(name STREQUAL "LICENSE.txt")
            continue()
        endif()
        file(READ "${source}" hex HEX)
        string(LENGTH "${hex}" hex_length)
        math(EXPR size "${hex_length} / 2")
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
        string(APPEND _fh1_fsr3_data "static const unsigned char kFsr3Source${_index}[] = {${hex}};\n")
        string(APPEND _fh1_fsr3_table "  {\"${name}\", kFsr3Source${_index}, ${size}},\n")
    endwhile()
    file(MAKE_DIRECTORY "${FH1_FSR3_GENERATED}")
    file(WRITE "${_fh1_fsr3_inc}" "// Generated from FidelityFX SDK ${_fh1_fsr3_revision} (MIT).\n${_fh1_fsr3_data}static const Fh1Fsr3Source kFsr3Sources[] = {\n${_fh1_fsr3_table}};\n")
    file(WRITE "${_fh1_fsr3_inc}.stamp" "${_fh1_fsr3_stamp}")
endif()
set(FH1_FSR3_LICENSE "${REXGLUE_FSR3_SDK}/LICENSE.txt")
