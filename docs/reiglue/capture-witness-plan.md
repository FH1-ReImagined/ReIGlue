# Scene capture witness plan

This is an evidence collection plan, not a replacement trigger. The live
two-format/same-base/first-overlay policy remains inherited. No game/GPU run is
authorized by this document. Required notices and the game's SDK pin remain.

## Questions each witness must answer

1. Which issued draw finished the scene, including reconstruction and menu video?
2. Which image contains that result, and how does it reach the swap source?
3. Which subsequent draw first adds an overlay, and on which image?
4. Do scene and final readbacks share geometry, channel order and color domain?
5. Does a proposed decision reject the negative examples below without losing
   scene separation in the positive examples?

Do not infer a shader's role solely from its hash or from absence of a depth
test. Full viewport and opaque writes are insufficient: existing garage traces
show two half-height composite draws after the earlier full-screen pass.

## Collection matrix after a separate resource grant

| Witness | Positive evidence | Counterexample to preserve |
| --- | --- | --- |
| Title/menu movie | Decoded movie included before logo/text | No preceding 3D scene; opaque menu panel is not a movie |
| Garage, exterior and cockpit | Complete final scene rectangle, including both composite halves | Instrument/reflection targets can reuse screen pitch |
| Gameplay with HUD and fades | Scene before translucent/antialiased HUD and fade | Full-screen fade must not become the scene |
| Pause/resume and menu transitions | Correct current frame after both transitions | Stale snapshot from an earlier mode/frame |
| Native-resolution and reconstruction modes | Actual final output dimensions and source ownership | Earlier postprocess input is not the final scene |
| Async pipeline miss | D row without V row cannot publish a snapshot | Guest command observed but host draw skipped |
| VR with desktop HDR off | Eye/cache identity and left-eye scene/HUD separation | Desktop HDR state must not suppress VR acquisition |
| HDR enable/disable, zero strength and SDR-only fallback | Successful connection and correct output mode | Reconnection loops or stale image dimensions |

Use copied saves and the orchestrator's exclusive graphics slot. Record exact
game/SDK source heads, DLL hashes, title update, settings, resolution, scene and
frame trigger with every artifact. Avoid overwriting qualified binaries or saves.
Repeated observations across adjacent frames are required before promoting a
candidate; one frame is a counterexample finder, not sufficient qualification.

## Current instrumentation and missing evidence

The opt-in `FORZA_GPU_FRAME_TRACE` trigger emits guest D records and bounded V/T/S
extensions. V is recorded after a host draw command is queued; it does not prove
submission completion. Join records within one trace using frame/draw plus eye,
cache and image identity. T supplies raw texture-fetch words for used slots; S
reports whether the inherited trigger recorded a copy. Image handles and cache
pointers are meaningful only within that process, not across independent runs.

V currently records viewport data and draw/raster scales, normalized depth/write
and blend controls, shader hashes and target identity. It does **not** provide an
explicit final Vulkan scissor rectangle, resolved host source-image identities,
image generation/lifetime, or GPU completion/readback evidence. Raw T fetch words
must be decoded and correlated with resolve/texture-cache ownership before they
can establish a source relationship. Do not treat the present record as already
covering those gaps or treat viewport bounds as actual draw coverage.

Before choosing a trigger, capture final applied scissor/viewport, resolve and
source-resource lineage, and fence-completed scene/final readbacks for each
candidate. Any further diagnostic instrumentation must remain opt-in and obey
the existing record/byte bounds; it must be source-checked before another private
build. Correlate readbacks to issued draws and final swap source, including
reconstruction, native HUD seeding and VR cache changes. If resource lineage or
coverage is ambiguous, record the candidate as unresolved instead of enabling
it or silently removing HDR/VR scene separation.

## Review artifact

For each candidate, retain the trace excerpt, decoded source/target lineage,
readback hashes, scene/final comparison, and a list of every positive and negative
case tested. Explain the algorithm from requirements and observed resource flow;
compare it with the inherited policy to identify retained dependencies. Passing
image tests and low textual similarity do not establish independent derivation.
The gate is both supported behavior and explicit provenance review, with remaining
uncertainty recorded. No new acquisition predicate is enabled by the current PR.
