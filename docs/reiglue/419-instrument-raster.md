# Cockpit instrument texture in native ultrawide

Issue [419](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/issues/419)
reports a shifted instrument cluster and blurry roof reflections at 3440x1440.
This change addresses the instrument cluster. The roof artifact remains open.

The native-screen classifier accepted every non-square viewport on a screen
pitch. The cockpit builds a 512x288 instrument image and 256x144, 128x72 and
64x36 downsample passes on pitch 1280. At 3440x1440 the native transform shrank
their drawing width to 43/48 of the integer resolve width. Consumers sample
these texture dimensions without the screen-texture UV adjustment, shifting
the gauges within the dashboard material.

Keep that observed family at integer resolve coordinates. Qualification requires
pitch 1280, both viewport scales enabled, and active scissor dimensions matching
the viewport. The allocation pitch alone cannot identify the image. Full-screen
and padded bloom viewports retain their existing behavior; square reflections
and the separate fractional reconstruction classifier are unchanged.

## Evidence and limits

A private Windows/Vulkan diagnostic based on the earlier 121-change SDK stack
used the native opening-drive Viper (live local model ID 1544), an isolated copy
of a fixture profile, no upscaler, and actual 3440x1440 internal captures. Its
control started disabled. After the stationary cockpit was inspected, a file
gate enabled only this family correction. Captures at 212.58 and 215.11 seconds
showed the cluster move into alignment in the same scene; the cyan roof artifact
persisted. The trace recorded the producer/scissor dimensions and downstream
512x288 sampling before the gate. Trace identity deduplication omitted the
control state, so it does not establish post-gate per-draw classification.

The owned process ended under an independent 230-second guard (closure at
230.67 seconds); no game remained and the source fixture hashes were unchanged.
No original player profile was opened. This diagnostic is causal evidence for
the dashboard correction, not a current-ReIGlue runtime validation or evidence
that the roof/reflection symptom is fixed.

The standalone `tests/fh1_screen_raster_test.cpp` target exercises the exact
family, Y sign, viewport/scissor disagreement, padded bloom, cinematic display,
software viewports, square reflection faces and unsupported pitches. It uses
explicit checks so release builds do not disable the regression assertions.

## Current-stack qualification

The canonical GPU plugin, runtime and code generator at `6b45bc1` built and
installed on Windows with Clang 23.1.3. The release-mode regression passed,
including 4,320 cases matching the private causal control. Game main `260ae0f`
was rebuilt against that installation: all game-authored units, SDK entry
sources, resources and generated precompiled headers were rebuilt. Only 313
verified generated guest objects were reused; 13 critical PPC ABI headers
matched exactly. The input-only capture derivative and read-only trace plugin
have separate hashes from the production binaries.

The corrected-DPI run on RTX 5060 Ti driver 617.14 confirmed an actual
3440x1440 swapchain and drawing image, with 3840x1440 padded storage. Its live
local car was model 1544. The inspected stationary cockpit capture at native
time 284.40 seconds showed the dashboard aligned and the cyan roof artifact
still present. The owned process closed at 290.47 seconds under its 290-second
guard, with zero owned job processes and an unchanged source fixture.

The material trace request came too late: no gate-open event or material/resolve
records were recorded before the guard. That run validates dashboard alignment,
but supplies no new evidence about the roof's texture consumers. Issue 419 must
remain open. A maintainer tag and a later client pin update are still required
to distribute this SDK change. Remote lint/nightly workflows were manually
disabled; these results are local validation, not green CI checks.

## Integration with the VR SDK changes

The PR integrates SDK main `7d7ad2b` (the OpenXR/VR merge). The command-processor
code merged automatically; the two documentation conflicts retained both the
instrument correction and all eight VR entries. The focused raster regression
was rebuilt with Clang 23.1.3 in release mode against the merged header and
passed. The remaining code delta from that main is the instrument classifier,
its call site, and the existing regression target.

This integration has not had a new full-SDK build or in-game qualification.
The Windows runtime evidence above is specifically for `6b45bc1`; it must not
be presented as a test of the later VR integration. The roof symptom remains
open and the private roof diagnostics are separate from this production PR.
