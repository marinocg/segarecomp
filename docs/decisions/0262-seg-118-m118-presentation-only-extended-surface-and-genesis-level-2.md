# ADR 0262: SEG-118 (M118) - the presentation-only extended surface and Genesis Level 2 (Extended Backgrounds)

## Status

Accepted (SEG-118, milestone B of optional 16:9 support; builds on ADR 0261). Sega CD follows in SEG-119, Master System in SEG-120, Level 3 (sprites) in SEG-121.

## Context

Level 2 shows real Plane A/B scenery to the left and right of the unchanged native picture on a 16:9 display. The presenter is handed only a finished `GenesisFrameArtifact`, which cannot
reconstruct off-screen columns (it holds the visible pixels and their palettes). The scenery therefore has to be produced while the authentic per-line VDP state and the VRAM journal of the frame
are still available, and it must never influence what the console does or what is recorded about it.

## Decision

1. **Invariant.** Extending the picture changes nothing about the virtual console: guest execution, the scheduler and virtual time, the VDP state, `GenesisFrameArtifact` (bytes, digest), checkpoint
   evidence, input recording/playback and audio are untouched. `GenesisFrameArtifact` and `checkpoint_evidence.h` gain no field. The extension is write-only to its own surface.
2. **Where it is produced.** Inside the existing live-frame publication (`genesis_publish_live_frame`), right after the native frame rendered successfully and before the VRAM journal closes, by a
   second call into the same plane sampling code (`genesis_vdp_produce_extension`, `vdp_render.c`). It reads the raster state, the VRAM journal and the finished native artifact and writes only an
   adapter-owned RGB24 target. The runtime reaches it through a function pointer (`GenesisFrameExtension::producer`), so `runtime.c` gains no link dependency on `vdp_render.c`. With no target, or
   `request == 0`, no extension code runs (the result is only stamped NONE). There is no second renderer and no snapshot is copied to the presenter.
3. **How it reaches the viewer.** The adapter owns one preallocated `GenesisFrameExtension` (`frame_extension.h`): a fixed RGB24 surface sized for the widest geometry (`GENESIS_EXT_SURFACE_BYTES`,
   from `sv_layout_extension_max`), a `request` flag, a `plan` callback and a result record `{sequence, status, reason, flags, native_w, native_h, side, pitch}`. It is attached through one optional
   pointer on `GenesisRasterCapture` (`extension`; NULL = today's behaviour; never serialized). The runtime stamps the result with the sequence published by the same call. The adapter's existing
   `present` callback keeps its signature, builds an `SvExtendedSurface` from the result and calls `sv_host_present_extended(..., sequence, &surface)`. `genesis_viewer_run`, `GenesisViewerHost`, the
   artifact and the exports are unchanged.
4. **Geometry (`sv_layout`, pure).** `sv_layout_extension_side(w, h, aspect)` gives the source pixels per side so the surface displays at 16:9 (symmetric, never a constant: 320x224 -> 39 px
   (398 wide), 256x224 -> 71 px (398), interlace 320x448 -> 238 px; with `SV_ASPECT_4_3` the extension has the 4:3 pixel shape: 320x224 -> 53 px (426 wide), 256x224 -> 42 px (340); clamped to
   `SV_EXT_MAX_SIDE` = 256, so no surface exceeds `max_w + 512` by `max_h`). It depends only on the native size and the aspect policy, not on the window, so a resize never changes what must be
   produced. `sv_layout_extended_dest` places the surface in the window with the established fit / whole-number scaling (fallback to fit below 1x) and returns the native rectangle with the very same
   scale, centred. The window only letterboxes: native 4:3 content is never stretched to 16:9. Invalid sizes return empty rectangles; all inputs are bounded before any arithmetic.
5. **Host (`sv_host_present_extended`).** The surface is borrowed for one call, never retained, never written. `sv_extended_surface_check` (pure) accepts it only if the status is
   Partial/Available, the produced mode is Level 2, the stamped sequence equals the frame being presented, the geometry is exactly `native + 2 * side` with `0 < side <= SV_EXT_MAX_SIDE`, the pitch
   covers a row and the readable size covers every row the host will read (overflow-safe). Otherwise the original picture is presented for that frame, a saturating counter increments, and the next
   frame is judged afresh (no carried state). The host re-uploads the native pixels from the native picture over the surface centre, so the centre is the original picture by construction. The frame
   texture follows the geometry (recreated only when the extended width changes, counted); CRT presets, scanlines, mask, curvature, brightness, fit/integer scaling, fullscreen and resize use the extended
   picture exactly like the original; menu thumbnails, previews and screenshots show the native picture.
6. **What is sampled, and what is honestly Partial.** Off-screen columns use the same code as the visible ones: Plane A and B nametables, plane size and wrap, horizontal scroll in all three modes
   (per-line and per-8-line scroll are real per-row data), vertical scroll (11 bits in interlace mode 2), tile flips, palettes, priority, the backdrop colour, per-line CRAM/register changes and the
   journalled VRAM replay. Not extended: sprites (Level 3), shadow/highlight (not modelled by the native renderer either). Caveats are typed, never guessed: `WINDOW_EDGE` (the window plane owns a native
   edge on some row; it does not exist off-screen, so Plane A is omitted there and Plane B/backdrop show), `COLUMN_VSCROLL` (per-column scroll with differing values: the nearest edge column's value is
   used), `NO_VRAM_JOURNAL` (mid-frame VRAM changes cannot be replayed, the same limit as the native render). A caveat makes the result PARTIAL; none makes it AVAILABLE. UNAVAILABLE (with a reason) for no
   plan/zero extension, undersized surface (nothing is written), a native/raster mismatch or invalid arguments. No tile or asset is invented or repeated.
7. **Requested, capability, effective (ADR 0261, unchanged).** The adapter reads the saved mode once per host slice in its `window_closed` poll (menu edits happen inside `sv_host_service`, between guest
   slices) and sets `request` (any mode above Original asks for Level 2 scenery); a frame is produced wholly under one value, and presentation uses the mode stamped with that frame's result, never the live
   preference, so a switch takes effect on the next completed frame and never mixes modes. Capability: Level 2 is Available/Partial/Unavailable from the latest requested frame's outcome (Available until a
   frame says otherwise), Level 3 stays Unavailable and falls back to Level 2 through `sv_widescreen_effective`. A failed frame is Original for that frame and recovers on the next. Nothing in guest
   execution, scheduling, input or audio reads requested or effective mode. A viewer that never attaches the target (Sega CD, Master System, tools) reports Level 2 Unavailable and stays Original.
8. **Reuse.** The target, plan callback, result record, host entry point and geometry are platform-neutral. Sega CD reuses `genesis_vdp_produce_extension` and the adapter code but owns its
   own frame boundary and attachment (SEG-119); Master System supplies its own producer (SEG-120). No universal VDP abstraction was introduced.

## Consequences

Level 2 costs one extra pass over the background planes only for frames produced while the mode is requested. The native frame, the digest and every reference checkpoint are byte-identical with and
without it. Game-specific or gameplay-aware widening (sprites, object logic) is out of scope here.

## Evidence

`tests/sv_viewer_core_test.cpp` (exact extension sides and placements for H32/H40/interlace/4:3/integer/fit/tiny windows, surface validation), `libs/viewer/test/sv_host_test.c` (synthetic extended
surfaces: scenery on both sides, exact centre, rejection of stale/short/mismatched surfaces, recovery, no texture churn, CRT presets, 4:3, resize), `tests/genesis_extension_test.cpp` (independent
reference sampler over synthetic scenes: wrap, per-line/per-tile scroll, column vscroll, window, priority, palettes, H32/H40, interlace, mid-frame CRAM/register/VRAM changes, missing journal,
undersized buffers, register fuzzing; runtime stamping, mode switching, native artifact/VDP/scheduler identity with and without the target, steady-state heap), and
`tests/genesis_viewer_extension_sdl_test.c` (the real SDL3 adapter and host: rendered output read back, live Original/Level 2/Level 3, H32<->H40, CRT, forced failures with recovery, guest invariance).
