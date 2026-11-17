# ADR 0315: an asymmetric widescreen viewport (provider-placed picture inside one planned total)

- Status: accepted (W04). Extends [ADR 0262](0262-seg-118-m118-presentation-only-extended-surface-and-genesis-level-2.md)-era Level 2/3 geometry, [ADR 0312](0312-virtual-sprite-priority-in-extension-columns.md) and [ADR 0309](0309-abi-2-authored-scenery-window.md). Product-only; no game knowledge.

## Context

The widescreen surface was always symmetric: the planner turns the user's display aspect into `side` extra source pixels per side, the producer makes `side` columns left and right of the native picture and the host shows
them centred. A side-scroller does not need that. At the start of a level the world to the left of the camera does not exist (there is nothing to show), while the world ahead is what the player needs; the symmetric
view wastes the left margin and starves the right one.

A private experiment on a real scrolling game (aggregate result only, W04 comment 6946752668) established that the picture can sit anywhere inside the planned total without changing the guest or the native pixels: with
`left = min(camera, margin)` the far-ahead scenery cells were equal to the symmetric reference in 3,657 of 3,657 cells and the picture was bit-identical in every crop. What it needed was a reusable placement seam, not a camera engine.

## Decision

1. **One planner, one total.** The planner is unchanged: it yields `side` per side, i.e. a total of `2 * side` extra columns for the user's aspect (32:9 of 320x224: 238 + 320 + 238). Nothing about the total, the limits (`SV_GEOM_MAX_SIDE`), the checked arithmetic or the rejection of unsupported sizes changes.
2. **A surface carries a `shift`.** `GenesisExtensionResult::shift` / `SvExtendedSurface::shift` (zero-initialised = centred) says the native picture starts at surface column `side + shift`: `side + shift` produced columns lie left of it and `side - shift` right of it, `|shift| <= side`. The surface width is still `native + 2 * side`.
   Legacy producers, other platforms (Master System, Sega CD) and every caller that does not set it are bit-for-bit the previous centred behaviour.
3. **The producer makes the asymmetric surface directly.** The same loops produce `left_cols` columns on the left and `right_cols` on the right (sprites, winner classes, scenery answers and the compositor use the same columns); it never builds a larger symmetric surface and crops it. Measured on the project's synthetic scene
   (unoptimised build, 40-frame mean): asymmetric 0/476 and 200/276 cost the same as symmetric 238/238 (13.2 ms against 13.3 ms) while the crop-a-symmetric-source alternative costs 23.0 ms (+73%) and 834 KiB + 278 KiB of classes against 522 KiB + 174 KiB.
4. **A provider supplies a placement, never dimensions.** `GenesisCompatDecision::left_px` + `has_left_px` (ABI 1) and, for native ABI 2 mods, `SegaGenesisRenderApi::view_left(left_px)`, declare how many of the planned columns lie left of the picture for one frame. The host owns and validates the geometry:
   - the placement is honoured only for an accepted frame and only when `0 <= left_px <= 2 * side` of the plan the frame is produced with; anything else (missing, negative, too large, a frame the provider did not accept) is exactly the centred layout, never a clamped or reinterpreted one;
   - a provider learns the geometry it must plan for through `view_margin(&margin)` (ABI 2: the per-side margin of the most recently produced frame; ABI 1: `GenesisCompatFrame::side` of the very frame) so it can size its scenery windows from the placement it chooses, with no hard-coded aspect;
   - for ABI 2 a placement is **frame-bound** exactly like a scenery window: committed with the render callback of guest frame N, served only for the frame produced when the runtime counts N + 2, discarded by a restore, rewind, load or skipped callback, and served only if it was chosen for the margin the frame is produced with (`view_margin` at the time of the call equals `frame->side`): an aspect change is therefore never given a placement computed for another one.
5. **Presented whole or not at all.** The host shows an asymmetric surface only when the plan it is using now equals the plan the surface was produced for (`visible side == side`). Under any other aspect it shows no extension columns (the native picture only) rather than cropping the asymmetric surface into a centred one, which would reinterpret columns the producer did not make for that view. A centred surface keeps the historical visible-side rule (a narrower aspect crops symmetrically).
6. **End-to-end presentation uses visible columns per side.** The host works with `left` (the picture's texture column) and visible `vl` / `vr` columns: texture crop and destination (the destination depends only on the visible total, so an asymmetric view lands exactly where the symmetric one does), side treatments (Comfort and Borders fill each side at its own width; Raw is the surface; a zero-width side needs no buffer), the fixed HUD
   (left edge of the visible surface), positioned layers (picture-referenced layers anchor to the picture wherever it is, presentation-referenced ones to the visible surface, zero-width regions included), the virtual-sprite compositor and the HARDWARE_PIECE omission use the same two numbers.
   `sv_host_visible_columns` reports them; `sv_host_visible_side` is unchanged.
7. **Out of scope, deliberately:** a camera API, vertical placement, smoothing/easing (a provider that wants smoothing smooths its own `left_px`), any write to the guest, and spawning objects the game did not create.

## Consequences

- A package can show the world ahead of the player and nothing behind it at a level start, and move continuously to the centred view, by computing `left_px` from the game's own camera word and the learnt margin. The Castle of Illusion package does exactly this (`left = clamp(camera_x - level_start, 0, margin)`), which is 0 + 320 + 476 at the start of a 32:9 level and 238 + 320 + 238 once the camera is 238 pixels in.
- The native picture, the guest state and every non-opting game are unchanged: a game without a placement is the centred surface byte for byte.
- Tests: `genesis_native_compat_test` (the producer for nine splits against an independent crop of a larger symmetric surface, pixels and winner classes, with sprites crossing both edges; invalid/missing/unaccepted/larger-than-plan placements are the centred layout), `genesis_vsprite_wide_test` (the compositor for nine splits against the same independent crop, region accounting, whole-or-nothing visibility, invalid shifts),
  `sv_asym_viewport_test` (the pure layers; the real presenter at 32:9 for 0/476, 100/376, 238/238 and 476/0 under Raw, Comfort and Borders, HUD and layers, zero-width regions, invalid shifts, an aspect change while playing, the centred surface unchanged), `mod_view_abi2_test` (a native provider through the real viewer at 16:9, 21:9 and 32:9 against an independent golden; out-of-range, beyond-bound, once-only, windows that do not follow the placement, restore).
