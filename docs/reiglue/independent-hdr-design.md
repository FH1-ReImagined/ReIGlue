# Independent HDR design, draft

The review checkpoint `7bb18e3` retained the previous highlight, calibration and
capture policies. Its successful builds did not establish independent design.
This document defines the subsequent replacement from behavior requirements.
All existing notices remain. Rendering qualification and provenance review are
separate gates; neither can substitute for the other.

## Luminance mapping

Required behavior: retain black and diffuse shading, provide adjustable brighter
scene highlights, preserve RGB ratios, protect overlays and never exceed the
effective display ceiling. Zero strength must be the ordinary decoded SDR signal
at the chosen white level. Invalid settings must not produce nonfinite output.

Use maximum linear RGB as the scalar intensity so the same multiplier preserves
channel ratios without a luminance/max mixture. Let `P` be paper white, `H` the
display peak clamped to at least `P`, and `s` the requested strength. The scene's
ceiling relative to paper white is `C = exp(s * log(H/P))`: strength interpolates
in exposure stops. This changes the earlier meaning of strength. At paper 200,
peak 1000 and strength 0.5, scene white is approximately 447.21 nits.

Keep `T(x)=x` through a diffuse anchor `d=0.18`. Above it use

```
u = (x-d)/(1-d)
T(x) = d + (C-d) * expm1(k*u)/expm1(k)
```

Choose `k` by solving `expm1(k)/k = (C-d)/(1-d)`. The left side is strictly
increasing for positive `k`; CPU bisection gives the unique solution. These
constraints give `T(d)=d`, `T'(d)=1`, `T(1)=C`, monotonicity and a finite ceiling.
The `C=1` case uses the identity directly. The shader applies `P*T(x)/x` to each
channel, with explicit handling for black. Any positive overlay coverage selects
ordinary paper-white mapping; coverage is not a fractional expansion control.

Contracts test the anchors, tangent, monotonicity, endpoint, hue ratio, protected
pixels, zero-strength identity, settings bounds and nonfinite inputs. They do
not test equality to the former formula. The maximum configured peak and many
intermediate strengths are included.

## Calibration

A centered four-column, three-row grid uses square cells in output pixels. Each
cell has an inset and a surround. It replaces the former horizontal strip.

- Black mode: absolute signals from zero through one nit, alternating with black
  in checker cells. Source gamma does not affect these known luminance values.
- Paper-white mode: columns at 18%, 50%, 75% and 100% paper luminance, shown against
  three different surrounds to expose adaptation and contrast effects.
- Peak mode: twelve levels approaching the declared peak in sixth-stop
  increments, each against a peak-luminance surround. No calibration sample
  exceeds the effective peak.

The grid is an output diagnostic overlay. Its placement and interaction with the
settings UI need visual qualification. CPU and GLSL compile the same scalar grid
function; tests exercise interiors, surrounds, checker cells and outside bounds.

## Overlay evidence

Treat the finished color `F` and scene color `S` as quantized observations of
`F = S*(1-a) + C*a`, where overlay color `C` lies in [0,1]. For each channel, find
the minimum opacity consistent with the source and snapshot rounding intervals.
If zero opacity is possible in every channel, there is no evidence of an overlay.
Otherwise protect the pixel. Do not dilate neighboring pixels or use an arbitrary
fixed color-distance threshold. Four floating-point ULPs bound interval-arithmetic
rounding; they are distinct from the source quantization interval.

The swap texture's documented red/blue correction flag supplies channel order.
Do not infer channel order by sorting colors. Current supported source formats
use 8- or 10-bit rounding intervals; the snapshot uses 10-bit color. Unknown
source formats currently use the conservative 8-bit interval and require review.
Tests generate black, white and translucent composites, quantize both observations,
and check that the estimated lower bound never exceeds actual opacity. A single
changed pixel must not mark its neighbors.

## Scene acquisition: evidence required before changing the live trigger

The existing two-format/same-EDRAM-base/first-overlay trigger remains inherited
and is **not replaced by the policies above**. It must not be treated as cleared.
The existing `CopyHdrScene` transfer routine also remains under review.

An alternative is to observe completed display-sized scene/postprocess draws and
snapshot the latest proven scene result after the draw. This avoids using a
format transition to predict the next UI draw. Candidate evidence includes full
display viewport and scissor, opaque color writes, no depth test/write, an actual
completed draw (not a skipped placeholder), and independently identified shader
roles. Such predicates are only a proposal until real traces establish them.

Required negative cases and counterexamples:

- Cockpit instrument targets reuse screen pitch; viewport/scissor must exclude them.
- Reflection and shadow targets may be square or screen-pitched intermediates.
- A fullscreen menu backdrop is opaque but is not the completed movie scene.
- A menu movie can be composed after entry to the UI target, with no prior 3D draw.
- Fade rectangles and opaque UI panels can cover the full viewport.
- Postprocessing can be multi-pass or use blending; opaque-only classification may
  miss the real final scene.
- Native HUD seeding, DLAA/FSR scaling and VR left-eye passes change resource ownership.
- A skipped asynchronous pipeline must not publish a new scene snapshot.

Record draw order, shader identifiers, viewport/scissor, depth/blend/write state,
source textures, render-target identity and draw completion for title, menu,
gameplay, pause/resume, reconstruction and VR paths. Correlate those records with
scene/HUD readbacks before enabling a new trigger. A failure to find a safe
predicate must not silently disable HDR or VR separation.

## Qualification

The new color/calibration/coverage policies change visible output. Prior image
captures and old reference-curve checks do not qualify them. Required witnesses
include HDR on/off, zero strength, paper/peak calibration, saturated highlights,
translucent and antialiased UI, menu video, native-resolution scaling, and VR with
desktop HDR disabled. Preserve copied-save and graphics-slot isolation during
those tests. The readback metadata identifies the new policy version explicitly.

The redesign passes all ten standalone SDK contracts with Windows clang 23.1.3,
including the two new mathematical contracts. Both regenerated shaders pass
glslang compilation and reflected ABI checks. The updated Vulkan command
processor and presenter pass production syntax checks. These are source-level
checks; the redesigned policies have not yet been linked or run on a GPU.
