# Independent presentation implementations

This branch replaces several Pinyon Shift adaptations. It does **not** yet remove
the Pinyon dependency or its notice. The remaining HDR presenter and snapshot
integration, including any reuse in newer VR helpers, need a complete provenance
review and replacement. Historical source in the game repository is separate.

## File record

| Files | Change |
| --- | --- |
| `include/rex/graphics/util/presentation_transform.h` | New affine HUD transform shared by both backends; neutral fallback for invalid settings. |
| `src/graphics/d3d12/command_processor.cpp`, `src/graphics/vulkan/command_processor.cpp` | Replace duplicated HUD transform implementation with the shared type. Existing draw classification, native raster correction and shader-family research remain. |
| `src/graphics/flags.cpp` | Stable configuration names remain. The adapted transform they controlled has been replaced. |
| `include/rex/graphics/pipeline/shader/spirv_contraction.h` | New completed-module precision pass. Structural validation precedes mutation; only eligible fragment shaders lose NoContraction. |
| `include/rex/graphics/pipeline/shader/spirv_builder.h`, `src/graphics/pipeline/shader/spirv_builder.cpp` | Remove the mutable builder contraction policy; restore unconditional strict decoration. |
| `src/graphics/pipeline/shader/spirv_translator.cpp` | Apply the completed-module pass. Shader key, Apple selection and existing multiply/NaN semantics remain. |
| `include/rex/ui/hdr_color.h` | New scalar HDR transfer/gain contract compiled by both C++ tests and GLSL. |
| `include/rex/ui/hdr_parameters.h` | New HDR setting validation and push-constant packing; nonfinite settings use defaults. |
| `src/ui/shaders/hdr_output.ps.glsl` | New output/calibration implementation against the existing push-constant ABI. |
| `src/ui/shaders/hdr_ui_mask.ps.glsl` | New alpha-only coverage sampling shader. |
| `src/graphics/shaders/hdr_hud_mask.cs.glsl` | New shared-memory tile implementation: each workgroup loads an 18x10 image tile, then performs 3x3 dilation from shared memory. Channel permutations are compared without sorting. |
| `src/ui/shaders/vulkan_spirv/hdr_output_ps.h`, `hdr_ui_mask_ps.h`, `src/graphics/shaders/vulkan_spirv/hdr_hud_mask_cs.h` | Regenerated from the replacement shader sources; no retained binary from the former shaders. |
| `include/rex/ui/vulkan/hdr_format.h`, `hdr_output_pass.h` | New exact format/color-space selection, output render-pass construction, shader module setup and command recording. |
| `include/rex/ui/vulkan/hdr_surface.h` | New move-only owner for per-image HDR resources. Partial construction cleans itself up. |
| `include/rex/ui/vulkan/presenter.h`, `src/ui/vulkan/vulkan_presenter.cpp` | Replace per-image allocation/cleanup with the resource owner and batch descriptor allocation; publish frame resources only after all allocations succeed. Replace HDR settings packing. Other HDR presenter portions remain adapted. |

The author inspected previous implementations during the audit. This is not a
segregated clean-room process or a legal clearance statement. New algorithms and
resource ownership were written against the established interfaces and behavior;
the source licenses and notices remain intact for everything still retained.

## Validation

Windows clang 23.1.3 standalone tests, C++20 with `-Wall -Wextra -Werror`:

- `presentation_transform_test`: output rectangle endpoints, depth preservation,
  inverse/tiled viewport identity and invalid settings.
- `spirv_contraction_test`: annotation removal with operations preserved,
  idempotence, excluded stages/depth/exports, malformed input without mutation.
- `hdr_color_test`: every 8-bit sRGB round trip, gamma conversion, bounded
  monotonic highlight output, paper-white HUD and disabled expansion.
- `hdr_shader_contract_test`: reflects the three generated SPIR-V binaries to
  check shader stages, workgroup dimensions, descriptor bindings, push-constant
  member offsets and the C++ parameter size.
- `hdr_format_test`: HDR available/requested/disabled, extension absence, wrong
  format/color-space pair and empty capability list.
- `hdr_parameters_test`: normal settings, below-paper peak, range clamps,
  nonfinite defaults and comparison/calibration settings.

The changed SPIR-V builder, translator, Vulkan and D3D12 command processors and
Vulkan presenter pass production Windows syntax checks against SDK headers.
The three GLSL shaders compile with glslangValidator. At `5d8a02e`, six changed
production translation units were rebuilt without a PCH and both runtime/GPU
DLLs linked in a private directory in 27 seconds, serially. Baseline object and
library hashes stayed unchanged. Windows loaded both DLLs; all 135 GPU imports
resolved. The ABI version returned 1 and the factory rejected incompatible ABI,
null configuration and unknown backend without constructing a graphics system.
The output-pass/format-selection changes at `ab7752b` also passed a private
presenter rebuild and both DLL links in 5.375 seconds. Five unchanged objects
were reused from the first validation; baseline input hashes stayed unchanged.
Both DLLs loaded and passed the same ABI functions, with all 135 imports
resolved against 11,280 runtime exports. No HDR display readback,
Vulkan validation run, VR run or macOS visual test has qualified the branch. Do not treat this branch as ready to remove notices or ship.

Shader regeneration, from the repository root:

```sh
glslangValidator -V -S frag --vn hdr_output_ps src/ui/shaders/hdr_output.ps.glsl -o src/ui/shaders/vulkan_spirv/hdr_output_ps.h
glslangValidator -V -S frag --vn hdr_ui_mask_ps src/ui/shaders/hdr_ui_mask.ps.glsl -o src/ui/shaders/vulkan_spirv/hdr_ui_mask_ps.h
glslangValidator -V -S comp --vn hdr_hud_mask_cs src/graphics/shaders/hdr_hud_mask.cs.glsl -o src/graphics/shaders/vulkan_spirv/hdr_hud_mask_cs.h
```
