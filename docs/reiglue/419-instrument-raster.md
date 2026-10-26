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
