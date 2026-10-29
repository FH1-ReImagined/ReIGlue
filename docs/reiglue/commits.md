# ReIGlue commits

Every commit ReIGlue adds on top of ReXGlue `bd833a2` (nightly 0.10.0.24), in order. The number
is the patch number in Forza Horizon 1: ReImagined's former `patches/rexglue-sdk/` series (its
`docs/sdk_patches.md` has the same text); each commit message says what changes and why.

- **Forza**: only makes sense for Forza Horizon 1 (or an engine like it): it relies on the game's
  shaders (matched by hash), its scene and HUD render targets, its formats or its input layouts.
  Scene/HUD upscalers (DLSS, FSR 3, MetalFX, SMAA), HDR with HUD coverage, ultrawide, texture packs,
  shader-hash effects, car thumbnails, VR (the scene and HUD layers, true stereo). Another game
  needs its own hooks for these.
- **General**: Xenos GPU accuracy (most are Xenia fixes the SDK did not have), performance, input,
  runtime and platform fixes, diagnostics, codegen and build. Any ReXGlue game can use them; they are
  the candidates for upstream.

141 commits: 106 general (Performance 35, Xenos GPU 26, Input 16, Runtime and platform 12, Diagnostics 7, Codegen and build 7, Workarounds and defaults 3) and 35 Forza-specific (Upscaling and HUD 17, VR 8, Input 4, Game-specific fixes 3, Texture packs 2, Shader-hash effects 1).
`patches/sdl3/` holds the one patch to the SDL submodule (0147); see the README. Issue numbers
(`#45`) in the commit messages are Forza Horizon 1: ReImagined's issues; the table links them.

| # | Commit | Change | Scope | Area | Author |
| --- | --- | --- | --- | --- | --- |
| 0001 | `1be347e` | SPIR-V tfetch: exponent bias from fetch-constant word 3, not word 4 | General | Xenos GPU | Dyotson |
| 0002 | `f3f7e5c` | SPIR-V tfetch: stacked-texture layer lerp from the first layer | General | Xenos GPU | Dyotson |
| 0003 | `db146bb` | SPIR-V tfetch: clamp the stacked-texture layer index | General | Xenos GPU | Dyotson |
| 0004 | `e9a5320` | Vulkan: 2_10_10_10 render targets as A2B10G10R10 | General | Xenos GPU | Dyotson |
| 0005 | `ce28794` | Vulkan shared memory: compute writes in the next barrier's access mask | General | Xenos GPU | Dyotson |
| 0006 | `1909649` | Render target transfer: 8888 <-> GAMMA conversion direction | General | Xenos GPU | Dyotson |
| 0007 | `d831fc5` | Vulkan texture cache: barrier between uploads into the same texture | General | Xenos GPU | Dyotson |
| 0008 | `e1cce06` | Texture cache: recheck range validity before watching it | General | Xenos GPU | Dyotson |
| 0009 | `e3b85d7` | Shader translator: scalar operand Z with a three-source vector op | General | Xenos GPU | Dyotson |
| 0011 | `46ee207` | Resolve clears: 64bpp clear value halves in register order | General | Xenos GPU | Dyotson |
| 0013 | `8506349` | Draw filtering: drop kill_pix_post_hi_z draws without a VIZ query | General | Xenos GPU | Dyotson |
| 0015 | `f2e7a77` | Render target transfer: gamma bytes to the middle of their linear range | General | Xenos GPU | Dyotson |
| 0016 | `06568ab` | Vulkan tessellation: adaptive edge factors as float bits | General | Xenos GPU | Dyotson |
| 0017 | `bed0a66` | SPIR-V domain shaders: r0.w is 1 | General | Xenos GPU | Dyotson |
| 0018 | `16e779f` | Primitive processor: option to skip adaptive triangle tessellation | General | Workarounds and defaults | Dyotson |
| 0019 | `a859466` | Defaults: Xenia's values for three GPU cvars | General | Workarounds and defaults | Dyotson |
| 0020 | `d51a27a` | VIZ queries: query status no longer faked as visible | General | Xenos GPU | Dyotson |
| 0021 | `fdeed0d` | SPIR-V vertex position: W = 0 goes to infinity, as on the Xenos | General | Xenos GPU | Dyotson |
| 0022 | `cd76be8` | Vulkan occlusion queries: ZPASS_DONE reports as a running counter | General | Xenos GPU | Dyotson |
| 0023 | `199eb7d` | Resolve: an empty resolve region is a no-op | General | Xenos GPU | Dyotson |
| 0025 | `1f6d3e9` | Codegen: vmsum3fp128/vmsum4fp128 overflow gives QNaN | General | Codegen and build | Dyotson |
| 0028 | `58b6c4a` | Runtime (Linux): QueryProtect and Protect with PROCMAP_QUERY | General | Performance | Dyotson |
| 0029 | `d65a3a6` | Command processor: type-0 register writes in ranges | General | Performance | Dyotson |
| 0030 | `dbaace0` | WAIT_REG_MEM: flip waits wake on the vblank | General | Performance | Dyotson |
| 0031 | `9e7cd2c` | Shared memory: no lock for ranges that are already valid | General | Performance | Dyotson |
| 0032 | `3751b81` | Vulkan fetch constants: unchanged values keep the bindings | General | Performance | Dyotson |
| 0033 | `c9bb14c` | Threading (POSIX): WaitMultiple sleeps until signalled | General | Performance | Dyotson |
| 0034 | `218af0e` | Vulkan tessellation shaders: system constants from set 1 | General | Xenos GPU | Dyotson |
| 0035 | `660110a` | Primitive processor: tessellated triangle strips and fans as lists | General | Xenos GPU | Dyotson |
| 0036 | `33737e2` | Vulkan: shared memory uploads hoisted before the render pass | General | Performance | Dyotson |
| 0037 | `9a56da4` | Video mode: guest refresh rate up to 480 Hz | General | Runtime and platform | Dyotson |
| 0038 | `2f665a2` | Vulkan: submission on its own thread | General | Performance | Dyotson |
| 0039 | `e05a3e3` | Shared memory: clear_memory_page_state off by default, as in Xenia | General | Performance | Dyotson |
| 0040 | `6855b59` | Keyboard and mouse: steering ramp, mouse speed and modifier binds | General | Input | Dyotson |
| 0042 | `63fa74e` | Vulkan render targets: optional initial fill (debug) | General | Diagnostics | Dyotson |
| 0043 | `0e511d9` | Presenter: FSR 1 and CAS without the FidelityFX SDK | General | Runtime and platform | Dyotson |
| 0044 | `95d8d3f` | Build: native Windows SDK builds with clang-cl | General | Codegen and build | Sean O’Connor |
| 0045 | `ca36acf` | Vulkan presenter: cache the output pipeline format | General | Performance | Sean O’Connor |
| 0046 | `383c7f7` | Vulkan: native DLSS/DLAA with separate scene and UI rendering | **Forza** | Upscaling and HUD | Sean O’Connor |
| 0047 | `75e6baa` | NVIDIA: hardware implicit LOD for automatic cubemap samples | General | Xenos GPU | Sean O’Connor |
| 0048 | `d24057e` | Resolve readback: buffers keyed per source and format | General | Xenos GPU | Dyotson |
| 0049 | `468975d` | Draws: memexport readback check after the empty ranges | General | Performance | Dyotson |
| 0050 | `fa39eed` | Timer queue: blocking wait on Linux and macOS | General | Performance | Dyotson |
| 0051 | `5c7bb50` | Vulkan pipelines: sync_pipeline_creation | General | Workarounds and defaults | Dyotson |
| 0052 | `28775a1` | Textures: minimum coverage for scaled resolve textures | General | Xenos GPU | Dyotson |
| 0053 | `1f0cebc` | Vulkan: refresh reused car-thumbnail readbacks | **Forza** | Game-specific fixes | Sean O’Connor |
| 0054 | `5b61819` | GPU choice: gpu_vendor setting, discrete GPUs first | General | Runtime and platform | Dyotson |
| 0055 | `b75ba17` | Depth transfers: keep the stencil bits without stencil export | General | Xenos GPU | Sean O’Connor |
| 0057 | `dcb6756` | DLSS: scene jitter coverage across EDRAM tiles | **Forza** | Upscaling and HUD | Sean O’Connor |
| 0058 | `c49d27c` | DLSS: reprojection and tile jitter | **Forza** | Upscaling and HUD | Sean O’Connor |
| 0062 | `2099582` | Native HUD: sync 1x scene resolves into the scaled HUD cache | **Forza** | Upscaling and HUD | Sean O’Connor |
| 0063 | `a78bf49` | Native ultrawide rendering and proportional HUD | **Forza** | Upscaling and HUD | Sean O’Connor |
| 0064 | `e8aced9` | Vulkan: flush shader storage writes before waiting | General | Performance | Sean O’Connor |
| 0065 | `896f592` | Resolve readback: skip repeated readbacks in `some` mode | General | Performance | Sean O’Connor |
| 0066 | `bc83249` | Car thumbnails: average supersampled pixels | **Forza** | Game-specific fixes | Sean O’Connor |
| 0067 | `256a2e0` | Build: clock conversions with Linux libc++ | General | Codegen and build | Sean O’Connor |
| 0068 | `9aa337c` | Vulkan: scaled resolve buffer window for the readback downscale | General | Xenos GPU | Dyotson |
| 0069 | `9ea8864` | Vulkan: two frames in flight by default on NVIDIA | General | Performance | Sean O’Connor |
| 0070 | `fe4a304` | Vulkan presenter: FIFO_LATEST_READY present mode | General | Runtime and platform | Dyotson |
| 0071 | `d2156e9` | Vulkan: direct scaled texture uploads on NVIDIA | General | Performance | Sean O’Connor |
| 0072 | `40caba5` | AMD FSR 3.1 temporal upscaler | **Forza** | Upscaling and HUD | Dyotson |
| 0073 | `8ade1d4` | FSR 3 and DLSS: scene input at least the native 1280x720 | **Forza** | Upscaling and HUD | Dyotson |
| 0074 | `e756ca4` | Racing wheel input driver | General | Input | Dyotson |
| 0075 | `05df977` | Live graphics settings transaction | **Forza** | Upscaling and HUD | Sean O’Connor |
| 0076 | `8945940` | Vulkan DLSS: initialization and presentation sync | **Forza** | Upscaling and HUD | Sean O’Connor |
| 0077 | `a2a5a4f` | SMAA scene antialiasing | **Forza** | Upscaling and HUD | Sean O’Connor |
| 0078 | `3537b46` | UI: passive overlays without continuous repaints | General | Runtime and platform | Sean O’Connor |
| 0079 | `ea775a4` | Codegen: register functions from the mapping table | General | Codegen and build | Sean O’Connor |
| 0080 | `8a511c2` | SPIR-V: rectangle fallback program counter Phi | General | Xenos GPU | Sean O’Connor |
| 0081 | `e605fc9` | Vulkan: optional base-mip sampling workaround | General | Diagnostics | Sean O’Connor |
| 0082 | `2cf3857` | SPIR-V: arithmetic contraction in Apple GPU color shaders | General | Runtime and platform | Sean O’Connor |
| 0083 | `c62b2fb` | macOS: fullscreen after the window is visible | General | Runtime and platform | Sean O’Connor |
| 0084 | `623a927` | macOS: MetalFX scene reconstruction | **Forza** | Upscaling and HUD | Sean O’Connor |
| 0085 | `490b3d0` | FH1: packed cross-scale resolve copy | **Forza** | Upscaling and HUD | Sean O’Connor |
| 0086 | `734f328` | Startup pipelines: warm-up progress and driver cache | General | Performance | Sean O’Connor |
| 0090 | `9fb9b98` | Input: Xbox impulse triggers | General | Input | Sean O’Connor |
| 0091 | `d627797` | HDR presentation and HUD coverage | **Forza** | Upscaling and HUD | Sean O’Connor |
| 0095 | `7b0213e` | Codegen: multiply-add without std::fma | General | Codegen and build | Dyotson |
| 0096 | `0a0463c` | Codegen: generated headers as build outputs | General | Codegen and build | Dyotson |
| 0097 | `850a4ab` | File system: host errors without exceptions ([#211](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/211)) | General | Runtime and platform | Dyotson |
| 0098 | `c7b11d4` | HDR: title and menu backgrounds captured after the movie ([#225](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/225)) | **Forza** | Upscaling and HUD | Sean O’Connor |
| 0099 | `0ee4dd7` | Racing wheels: clutch pedal and shifter ([#125](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/125)) | **Forza** | Input | Dyotson |
| 0100 | `916e5bb` | Cross-scale scene/HUD resolve layout | **Forza** | Upscaling and HUD | Sean O’Connor |
| 0101 | `124ad40` | Racing wheels: the game's force feedback ([#219](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/219)) | General | Input | Dyotson |
| 0102 | `e26840a` | Pipeline store (Vulkan): keep the valid records | General | Performance | Dyotson |
| 0103 | `6cea633` | Audio output: fallback formats and devices | General | Runtime and platform | Dyotson |
| 0104 | `a469ca2` | Vulkan device loss and presentation stalls: diagnostics ([#221](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/221)) | General | Diagnostics | Dyotson |
| 0105 | `f8da5c5` | XMA decoder: unreadable input and locked contexts ([#228](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/228)) | General | Runtime and platform | Dyotson |
| 0112 | `2d68334` | Vulkan render target memory: trim when memory is full ([#241](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/241)) | General | Performance | Dyotson |
| 0113 | `61ce5b5` | Shader prebuild (Vulkan, [#226](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/226)) | General | Performance | Dyotson |
| 0114 | `e84f764` | Waits (Linux and macOS): a signal wakes only its waiters | General | Performance | Dyotson |
| 0115 | `794c680` | Command processor: wake waiters on written words | General | Performance | Dyotson |
| 0116 | `c3f06d5` | Controller remapping (SDL, [#279](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/279)) | General | Input | Dyotson |
| 0127 | `e395f04` | Racing wheels: d-pad bindings ([#279](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/279)) | General | Input | Dyotson |
| 0128 | `388f56c` | Racing wheels: pedals and shifter on their own USB device ([#272](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/272)) | General | Input | Dyotson |
| 0129 | `619511d` | Racing wheels: the rumble as a fine vibration ([#273](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/273)) | General | Input | Dyotson |
| 0130 | `517db01` | Racing wheels: the forces come back whole ([#304](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/304)) | General | Input | Dyotson |
| 0131 | `6d5abe1` | Racing wheels: a minimum steering force ([#303](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/303)) | General | Input | Dyotson |
| 0132 | `c492ab1` | Racing wheels: H-pattern gears on lever moves ([#303](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/303)) | **Forza** | Input | Dyotson |
| 0133 | `07d2a33` | Racing wheels: an analog handbrake ([#303](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/303)) | **Forza** | Input | Dyotson |
| 0134 | `f8828ba` | Racing wheels: which device is the wheel ([#306](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/306)) | General | Input | Dyotson |
| 0135 | `8603c57` | Racing wheels: Thrustmaster ids, axes that read 0 at start ([#308](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/308)) | General | Input | Dyotson |
| 0136 | `20f1a83` | Racing wheels: effects the wheel refuses ([#308](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/308)) | General | Input | Dyotson |
| 0137 | `59b4601` | Racing wheels: saved vJoy device names across SDL backends ([#306](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/306)) | General | Input | Sean O’Connor |
| 0138 | `704ec50` | Vulkan memory on integrated GPUs ([#268](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/268)) | General | Performance | Dyotson |
| 0140 | `f11ea04` | Command processor packet loop ([#45](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/45)) | General | Performance | Dyotson |
| 0145 | `c01c144` | Racing wheels: the game's force feedback in the log ([#304](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/304)) | General | Diagnostics | Dyotson |
| 0146 | `491081e` | Racing wheels: effects a wheel ends by itself start again ([#304](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/304)) | General | Input | Dyotson |
| 0147 | `7cb018b` | SDL: endless haptic effects stay endless on Windows and macOS ([#304](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/304)) | General | Input | Dyotson |
| 0149 | `23693a7` | Car thumbnails: scaled storage quads in the filter ([#336](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/336)) | **Forza** | Game-specific fixes | Sean O’Connor |
| 0150 | `3a35921` | Fractional display raster kept out of off-screen views ([#330](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/330)) | **Forza** | Upscaling and HUD | Sean O’Connor |
| 0180 | `d647580` | Why a frame was slow, in the log (Vulkan, [#45](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/45)) | General | Diagnostics | Dyotson |
| 0181 | `b971007` | Back from the map without copying the scene's resolves back ([#45](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/45)) | General | Performance | Dyotson |
| 0200 | `f81c526` | A minute summary of the GPU emulation in the log (Vulkan, [#45](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/45)) | General | Diagnostics | Dyotson |
| 0210 | `00e629d` | Guest thread scheduling: the core-count warning once ([#367](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/367)) | General | Runtime and platform | Dyotson |
| 0230 | `811f269` | Integrated GPUs: room in the carve-out, smaller submissions ([#268](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/268)) | General | Performance | Dyotson |
| 0270 | `ea1c4c0` | Pipelines of queued draws built ahead (Vulkan, [#378](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/378)) | General | Performance | Dyotson |
| 0280 | `76b5205` | Texture packs: dump and replace textures ([#384](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/384)) | **Forza** | Texture packs | Dyotson |
| 0290 | `4b32cca` | The picture after a minimize and restore ([#221](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/221)) | General | Runtime and platform | Dyotson |
| 0300 | `1a66a69` | The command processor's CPU time on Windows ([#400](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/400)) | General | Diagnostics | Dyotson |
| 0310 | `330ee8c` | Texture packs: the packs to use, in order ([#384](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/384)) | **Forza** | Texture packs | Dyotson |
| 0320 | `d40ee93` | Bounded Vulkan driver cache saves ([#406](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/406)) | General | Performance | Sean O’Connor |
| 0340 | `e97b6ee` | Render target setup reuse (Vulkan, [#45](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/45)) | General | Performance | Dyotson |
| 0341 | `9335bc2` | Guest memory watch walk ([#45](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/45)) | General | Performance | Dyotson |
| 0342 | `ae1b946` | Skip unchanged bounded Vulkan driver cache saves ([#443](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/443)) | General | Performance | Sean O’Connor |
| 0350 | `1af1c4b` | World ambient occlusion (Vulkan, [#380](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/380)) | **Forza** | Shader-hash effects | Dyotson |
| 0360 | `04882d9` | Command processor register ranges ([#45](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/45)) | General | Performance | Dyotson |
| 0361 | `5cdee7a` | Command processor calls into the runtime library ([#45](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/45)) | General | Performance | Dyotson |
| 0362 | `edf21bf` | Shader translation memo ([#45](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/45)) | General | Performance | Dyotson |
| 0363 | `6b45bc1` | Cockpit instrument textures retain integer coordinates in native ultrawide ([#419](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/419)) | **Forza** | Upscaling and HUD | Sean O’Connor |
| 0364 | This branch | Asynchronous pipelines are built across frame ends ([#167](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/167)) | General | Performance | Dyotson |
| 0390 | `e5f33a7` | VR: Vulkan created through the OpenXR runtime ([#333](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/333)) | **Forza** | VR | Dyotson |
| 0391 | `461ae0d` | VR: the guest output for a second consumer, with a frame tag ([#333](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/333)) | **Forza** | VR | Dyotson |
| 0392 | `b6aba71` | VR: pads without window focus ([#333](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/333)) | **Forza** | VR | Dyotson |
| 0393 | `0fc4a4b` | VR: the scene before the HUD and the HUD alone ([#333](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/333)) | **Forza** | VR | Dyotson |
| 0394 | `09122a0` | VR: both eyes from the scene and its depth ([#333](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/333)) | **Forza** | VR | Dyotson |
| 0395 | `5d9c03b` | VR: the headset's picture sharpened ([#333](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/333)) | **Forza** | VR | Dyotson |
| 0396 | `f20eb4c` | VR: true stereo, the scene executed once per eye ([#333](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/333)) | **Forza** | VR | Dyotson |
| 0397 | `4d66d72` | VR: when each guest output frame was published ([#333](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/333)) | **Forza** | VR | Dyotson |
| 0399 | This branch | Read-only active physical controller disconnect sequence ([#331](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/pull/331)) | **Forza** | Input | Sean O’Connor |
| 0400 | This branch | Build (Linux): the small code model, large on request ([#473](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/473)) | General | Codegen and build | Dyotson |
| 0406 | This branch | Vulkan: direct scaled depth texture uploads on NVIDIA ([#500](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/500)) | General | Performance | Sean O’Connor |
