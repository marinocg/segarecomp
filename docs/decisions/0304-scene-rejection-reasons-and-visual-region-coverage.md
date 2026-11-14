# ADR 0304: scene rejection reasons, offered/accepted/presented coverage and the virtual-sprite delivery rule

- Status: accepted (combined delivery F15: SEG-140-T005, SEG-140-T006, SEG-140-T007). Builds on [ADR 0302](0302-honest-geometry-declarations-and-per-scene-coverage.md) (declarations, per-scene coverage, native-only fallback outside the range) and [ADR 0303](0303-authored-virtual-sprites-vdp-decode-priority-budget-and-layered-overlays.md) (virtual sprites, the `GenesisVirtualReason` vocabulary).
- Presentation only: nothing here reaches guest state, the native picture, VRAM or a save state.
- Not in scope: attaching virtual sprites to the live viewer, a Luau/native authoring path for them, and drawing them in the widescreen side columns (all still missing, see Limitations), auto-repairing a scene, any title-specific logic.

## Decision 1: one reason vocabulary, per frame, never a coverage counter

`SvRejectReason` (`sv_layout.h`, stable names in `sv_reject_reason_name`) says why a frame's contribution was not fully presented. A frame counts once for each reason that applies (not per component) in `reason_frames`, and the last frame's reason is `last_reason`; a clean frame sets it back to `none`. A reason never raises a coverage counter.

| Reason | Applies when |
| --- | --- |
| `unverified_state` | the surface is for another frame (stale or mismatched), or a game-state virtual-sprite set was composed without a verified, exactly current snapshot |
| `unsupported_mode` | the surface was produced above the requested widescreen level, or a virtual-sprite stage/provenance the host cannot present |
| `over_declared_geometry` | the requested aspect is outside the declared range, the declaration is unreadable, or the planner rejects the exact ratio |
| `undeclared` | content was delivered for a scene (or unknown scene) / component the package never declared |
| `virtual_sprite_rejected` | validation rejected the set; the `GenesisVirtualReason` is kept verbatim in `last_vsprite_reason` |
| `virtual_sprite_unpresented` | a set was submitted or validated but never composed and presented |
| `invalid_surface` | a surface that claims an extension has an inconsistent size, pitch or geometry |

Existing failure classes are reused (`SvExtCheck`, `SvGeomStatus`, the declaration range check, `GenesisVirtualReason`); only the grouping and the two virtual-sprite stage reasons are new. A surface is refused as `invalid_surface` or `unsupported_mode` only when the user actually requested an extension (Widescreen Mode above Original) and the surface claims produced side columns (side > 0, produced above Original). The live viewer hands over its pixel buffer on every frame and a HUD-only/Original surface is the normal case, so none of those is ever a refusal or a notice; `undeclared` is a reason only for a package that declares something (a package with no declaration claims nothing and keeps counting `undeclared` silently, as before). This was found by `mod_luau_executable_test` and is part of the evidence.

## Decision 2: offered, accepted, presented (additive report keys)

Every component of every accounted frame now has, besides covered / missing / undeclared / outside_range: `requested` (declared for this scene and inside the range), `offered` (the provider supplied it), `accepted` (the host's checks passed), `presented` (accepted and shown inside the declared range) and `rejected` (offered, not accepted). `covered` equals `presented` for a declared component; an undeclared delivery is `presented` but never `covered`; nothing outside the range is presented or covered. The report keeps schema `segarecomp.geometry-coverage/1`: keys are only added (top-level `last_reason`, `last_vsprite_reason`, `reason_frames`; per component the five counters, after `outside_range`), existing keys keep their order and meaning; `sv_coverage_test` pins the exact bytes. The viewer buffer is 36,864 bytes; the worst case (33 buckets, saturated counters) is asserted to fit.

Only the finest verified boundary is tracked: scene x component (scenery, hud, actors). The only per-region boundary the host can verify is the planner's side width; no finer region claim is made.

## Decision 3: the virtual-sprite delivery rule

`SvExtendedSurface` gains four additive fields (`actor_stage`, `actor_provenance`, `actor_state_verified`, `actor_reject`; zero = none, a surface that predates them is accounted exactly as before). Stages are `submitted`, `validated`, `composed`, `rejected`. `sv_actor_signal` credits `actors` only for `composed` with provenance verified (1, plus a verified current snapshot) or authored (2); the host then counts it presented only when it also presented the frame inside the declared range. A submitted, validated, rejected or unpresented set is `offered` but not accepted. A scenery surface, a HUD or a layered overlay never credits `actors`; a HUD or overlay never credits scenery. Layered overlays are **not** counted as `hud` (their per-layer rejection stays in `sv_host_overlay_diag`): no accounting yet establishes that a layer was presented, so this remains fail-closed.

Drawing a virtual sprite does not prove the game object exists or is active, and drawing from the verified VRAM does not prove the art is semantically right; `actors` coverage means only "a composed, verified-gate contribution was presented in a declared scene". Today nothing in the live viewer sets these fields (Limitations), so a shipped program still reports a declared `actors` as `missing`.

## Decision 4: fallback and visibility

Outside the declared range the host still shows only the native picture (ADR 0302 decision 3), under Raw or any side style, with no provider column and no HUD; this is unchanged and now covered in `sv_scene_rejection_test`. A stale surface is never shown or counted, and no sprite set, overlay or surface is cached across frames (the actor signal is per-frame data on the surface). A user sees a notice only when the reason changes (not per frame; the geometry reasons keep their own once-per-entry notices) and the Video page note ends with `Last frame: <reason>.` while a reason is current.

## Evidence

`sv_scene_rejection_test` runs 4 aspects x 3 scenes x 18 variants through the real host and compares every counter, every reason and the rendered output with a reference model written in the test from the rules above, twice (byte-identical reports). `sv_coverage_test` pins the exact JSON, the actor-signal table and the worst-case size. `sv_geometry_decl_test`, `sv_widescreen_matrix_test` and the F14 suites stay green with only the `outside_range` assertions made prefix-based.

## T006/T007 evidence (summary)

The real-game classification of the existing Sonic packages at 16:9, 21:9 and 32:9, its method and its limits are in [widescreen-aspect-matrix.md](../architecture/widescreen-aspect-matrix.md) section 5 and `docs/product/widescreen-32x9-classification.json` (checked by `widescreen_classification_test`). It found no scene class that a presentation-only change can extend with actors, left Sonic 2 REV01 NOT RUN (image unavailable) and recorded the missing live virtual-sprite consumer as the post-F15 handoff (section 7 there).

## Limitations

- Virtual sprites are not consumed by the live Genesis viewer, cannot be authored from Luau or a native provider through a supported path, and do not draw in the widescreen side columns (ADR 0303). The signal above is the accounting contract a future presenter would feed; it is exercised by synthetic fixtures only and proves nothing about gameplay.
- Layered overlays are not counted as HUD delivery.
- Scene names are still the provider's claim (ADR 0302); the host cannot verify the guest scene.
