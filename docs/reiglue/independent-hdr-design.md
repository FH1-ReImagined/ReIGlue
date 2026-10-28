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

This model assumes both images observe aligned pixels in the same color domain.
Actual resolve, channel order and VR paths still need witnesses. Same-color or
rounding-indistinguishable overlays cannot be recovered from these observations.
The readback checker consumes the supplied alpha classification; it verifies
mapping/protection for that classification, not completeness of overlay detection.

## Exact snapshot transfer

The separate transfer replacement uses `vkCmdCopyImage`, without filtering or
color conversion, for identical single-sample A2B10G10R10 images. Its typed plan
rejects missing/equal images, incompatible formats or samples, missing transfer
usage, zero extents, out-of-bounds regions and widened dimension overflow before
recording commands. The target is a separately allocated, caller-owned image;
source and destination allocations do not alias.

`CreateRenderTarget` supplies TRANSFER_SRC for the eligible single-sample color
format even when desktop HDR is off. Both snapshot destinations are created by
`InstallCopyTarget` with TRANSFER_DST|SAMPLED usage, one mip, one layer and sample
count one. Source dimensions come from the same key and resolution scale used
at allocation; destination dimensions come from the successful allocation record.
The color aspect, mip zero, layer zero/count one and equal rectangular extents
follow the [Vulkan image-copy requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyImage.html).

The destination owner waits for its in-flight use and schedules TRANSFER_DST.
The copy recorder ends rendering, transitions/tracks the source to TRANSFER_SRC,
flushes both pending transitions, records the copy, then schedules/tracks the
cache's next color-draw usage. This replaces the previous untracked save/restore
blit. The owner schedules destination shader-read usage and publishes the frame
only when a copy was recorded. All operations use the graphics queue; no queue
family transfer is introduced. Invalid requests retain the existing unavailable
snapshot fallback (paper-white output or the VR caller's mono frame).

Contracts cover boundary/overflow rejection without recording, the precise
subresource rectangle and read/copy/next-use ordering. The cache, deferred
recorder and command processor pass production syntax checks. Rendering
equivalence, synchronization validation and HDR-off/VR behavior still need GPU
qualification. The transfer replacement does not replace capture timing.

## Scene acquisition: evidence required before changing the live trigger

The existing two-format/same-EDRAM-base/first-overlay trigger remains inherited
and is **not replaced by the policies above**. It must not be treated as cleared.
The separately staged exact-copy replacement above also needs independent review
and rendering qualification; source checks alone do not establish either.

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

Existing clean guest-frame traces already disprove the simplest full-viewport
predicate. In the garage trace, draws 2492 and 2493 use vertex shader
`20A41D46F34D238E` and pixel shader `C786F39CB12D40FE` to composite two 1280x360
halves; UI follows at draw 2494. The earlier `64AEAE9314A0118B` pass is followed
by a resolve and these composite draws, so capturing after that earlier pass
would select the wrong stage. A future candidate must establish complete
rectangle coverage and the shader/source role, not just a full-sized viewport.
The traces are local evidence from the camera investigation, not new GPU
qualification of this branch; raw guest D records also include copy-mode commands.

The existing `FORZA_GPU_FRAME_TRACE=<path>` / `<path>.trigger` diagnostic now
adds `V` rows after actual Vulkan draw commands are queued, `T` rows for their
used texture-fetch words, and `S` rows when the current inherited policy attempts
a snapshot. V rows identify the guest draw, frame, eye, shader pair, host image,
cache, viewport, scales, normalized depth/write state and blend/color controls.
This distinguishes an issued draw from an asynchronous placeholder skipped after
its D row. It does not prove GPU completion. Added V/T rows stop after 16,384
guest draws, 65,536 added records or 16 MiB of added text, whichever comes first.
Individual records must fit 1,023 bytes; oversized records are discarded rather
than truncated. A file-write failure stops further added records for that frame.
Byte/record/reset/disabled-path contracts cover the bounds. The existing tracer
still owns the one-frame trigger and stream. With no trigger stream, the extension
only tests a null pointer and does no formatting, allocation or file access.
The trace extension makes no snapshot decision and the production syntax check
passes. Capture and visual evidence remain required before enabling a new policy.

Record draw order, shader identifiers, viewport/scissor, depth/blend/write state,
source textures, render-target identity and draw completion for title, menu,
gameplay, pause/resume, reconstruction and VR paths. Correlate those records with
scene/HUD readbacks before enabling a new trigger. A failure to find a safe
predicate must not silently disable HDR or VR separation.

## Connection request handling, 2026-10-28

The next source change replaces the inherited hot-toggle predicate with a
request/commit protocol. Its requirements come from connection lifecycle
behavior: a changed setting needs fresh capability selection; successful SDR
fallback must remain paintable; failed construction must not acknowledge a
request; a setting changed during construction needs reconsideration next paint.

`HdrConnectionRequest` records the request accepted by the last fully successful
connection. The connection function snapshots `present_hdr` once, passes it to
both the existing-surface and fresh-surface attempts, and commits after all
framebuffers and semaphores exist. Paint requests reconnection when the setting
differs from the committed request. Capability and actual format are observations,
not inputs to this decision. An unavailable HDR request produces one capability
evaluation per successful request transition, without a per-paint retry loop.
External surface reconnects still query capability; toggling off/on also retries
an unavailable request. Changes returning to the committed value before paint
need no reconstruction.

This changes behavior: toggling on an SDR-only surface requests one reconnection.
Resource retirement, UI/paint thread exclusion, reconnect failure handling and
surface events remain the existing presenter mechanisms. These are retained
dependencies, not newly independent implementations. The whole presenter
integration is not replaced. Previous code was inspected; this is not a
segregated clean-room process. Existing notices remain.

Request/format fallback and exact-copy contracts pass with Windows clang 23.1.3,
C++20, `-Wall -Wextra -Werror`; the production Vulkan presenter passes a
syntax-only check. No DLL link or graphics initialization was performed for this
change. Local evidence is in the enclosing workspace at
`build/pinyon-audit/hdr-request-source-check/results.json`. The capture trigger
remains inherited; the later trace extension remains unqualified at runtime.

The bounded next build requires a separate orchestrator grant. Serially rebuild
the presenter and trace-modified command processor in a private directory,
verify hashes before reusing other objects from the `8c97dd5` checkpoint, link
both DLLs and run the existing loader/ABI checks without creating graphics.
Allow at most 60 seconds per compile/link step; stop on failure or changed
baseline inputs. Preserve qualified baseline binaries. This cannot qualify
rendering. Capture replacement still requires correlated issued-draw, source and
readback evidence for the negative cases above; no new live trigger is enabled.

## Qualification

On 2026-10-28, checkpoint `342686ee76dd5758e691a57f84ce9e0964441ef7`
rebuilt the Vulkan command processor, presenter, render-target cache and deferred
recorder, linked both DLLs and passed loader/ABI checks in 27.063 seconds total.
The single serial job enforced 60 seconds per step and 300 seconds overall.
All 319 previously recorded input/output hashes matched before and after.
The two exact-copy objects lacked historical object hash anchors, so they were
rebuilt rather than treated as authenticated reuse; the other four reused
replacement objects matched their recorded hashes. New object and DLL hashes,
step results and ABI evidence are recorded in
`validation/2026-10-28-hdr-request.json`. Both DLLs loaded, ABI version 1 was
returned, all 135 imports resolved against 11,280 runtime exports, and invalid
requests were rejected without creating graphics. Qualified binaries and the
game SDK pin were unchanged. The CPU reservation was released after completion.
This qualifies linking/loading of the hot-toggle and trace changes, not runtime
rendering or independent provenance of the remaining integration. The concrete
next evidence plan is [capture-witness-plan.md](capture-witness-plan.md).

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

Checkpoint `8c97dd5` (including the exact-copy replacement) subsequently rebuilt
four production units and linked both DLLs privately in 29.781 seconds, serially.
Four unchanged objects were verified before reuse; qualified baseline input
hashes remained unchanged. Both DLLs loaded, ABI version 1 was returned, all 135
GPU imports resolved against 11,280 runtime exports, and incompatible ABI/null
configuration/unknown backend requests were rejected without graphics creation.
That check does not qualify rendering and does not include the later trace-only
extension. Independent source review accepted the substantive policy and transfer
replacements while retaining the capture, hot-toggle and hardware limits above.
