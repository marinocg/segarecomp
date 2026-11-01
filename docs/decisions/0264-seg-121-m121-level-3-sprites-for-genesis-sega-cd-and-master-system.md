# ADR 0264: SEG-121 (M121) - Enhanced Widescreen (Level 3): sprites beside the picture for Genesis, Sega CD and Master System

## Status

Accepted (SEG-121, milestone E of optional 16:9 support; builds on ADR 0261-0263). Title-specific gameplay widescreen is SEG-122.

## Context

Level 2 (ADR 0262/0263) adds real background scenery beside the unchanged native picture. Level 3 must do something different from Level 2 without becoming a promise it cannot keep: a generic
compositor can only draw sprites that the game's own sprite state already places over the extra columns, and a game that never submits objects there shows none. Level 3 therefore stays a
presentation enhancement with the same invariants as Level 2 (guest execution, the scheduler, VDP/CRAM/VSRAM/SAT contents, controller sampling, input recording, audio, the native artifact and its digest,
`SmsFrameRecord` and its SHA-256, checkpoints and serialized state are untouched).

## Decision

1. **A typed request and a typed produced mode.** `GenesisFrameExtension::request` is `GenesisExtensionMode` (0 none, 1 Level 2 background, 2 Level 3 background + sprites), sampled once per host slice
   between guest slices; `GenesisExtensionResult::mode` states what the frame actually contains, with `sprite_pixels` (visible sprite pixels in the extra columns; 0 is a valid Level 3 frame, not a failure)
   and `sprite_reason`. The shared dispatch (`genesis_frame_extension_run`, used by the Genesis live frame and the Sega CD presentation frame) refuses a result whose mode is above the request, a
   Level 3 result that claims Available, and an unknown mode, and normalizes a Level 2 producer that predates the field (mode 0) to Level 2. The Master System `SmsExtResult` carries `sprites` and
   `sprite_pixels` the same way. The viewer stamps `SvExtendedSurface::mode` from the produced result, never from the saved preference; `sv_extended_surface_check` accepts Level 2 and Level 3, and
   `sv_host_present_extended` additionally refuses (and counts) a surface produced above the currently requested level, so no sprite pixel can remain after stepping down to Level 2 or Original.
   Level 2 callers (`request == 1`) behave exactly as before. There is no second framebuffer, presenter or persistent Level 3 surface: the same adapter-owned surface holds either content.
2. **Genesis: one sprite evaluation, two spans.** `genesis_vdp_build_sprite_row` (the native sprite pass) gained an x origin and span: the native row evaluates columns `0 .. width-1`, the Level 3 extension
   evaluates `-side .. width+side-1` with the same SAT walk (bounded by the table size, link 0/out-of-range/revisit terminate), the same per-line limit (20 sprites H40 / 16 H32, the next one ends the
   walk), the same cell budget (40/32 cells in list order), flips, sizes, interlace-2 geometry, palette, priority and first-opaque-wins order. Selection is x-independent, so the extension can never show a
   sprite the native line drops and never changes which sprites the native frame shows. The 9-bit X plus 128 bias reaches columns -128 .. 383, so sprites wholly beside the picture are drawn when the
   extension covers them. The sprite layer is composed with the Plane A/B candidates of the column by the native priority rule (`genesis_vdp_select_cram_index`). The producer runs at the same
   point as Level 2, over the same VRAM journal replay, per-line registers and CRAM, so a SAT edit between lines is replayed when a journal exists. Without a journal the result carries
   `NO_VRAM_JOURNAL` (Partial) exactly as the native render and Level 2 do (the Sega CD path keeps that limitation). The native renderer's behaviour is untouched; the "sprite X = 0 masks" rule is not
   modelled natively and is not invented for the extension.
3. **Fail closed, one surface.** Before anything is written, the producer checks that every captured line decodes a SAT base and the picture's own horizontal resolution; an ambiguous state serves the whole frame
   as Level 2 (`mode = 1`, `sprite_reason = SPRITE_STATE`), never half a sprite layer. A failed producer, a missing or undersized target, a stale or mismatched sequence or an incomplete capture are UNAVAILABLE and
   the host presents the Original picture for that frame; the next valid frame recovers. The Level 2 content already in the surface is the fallback; no second buffer exists.
4. **Capability honesty.** Level 3 is never reported Available. The Genesis and Sega CD viewers report Level 3 Partial after a produced Level 3 frame, Unavailable after a frame served as Level 2 or
   failed, with the note *"Sprites appear beside the picture only where the game places them; this does not widen the game world."*. `SvWidescreenCaps` gained an optional `enhanced_note`. The saved Level 3
   preference is retained through every fallback (Level 3 -> Level 2 -> Original).
5. **Sega CD reuses the Genesis producer.** The presentation frame calls the same `genesis_frame_extension_run` with the frame's VRAM and per-line state; it stays Partial (`NO_VRAM_JOURNAL`). No Sega CD
   sprite code exists.
6. **Master System: only physically representable spill.** The X coordinate is an 8-bit field and the shift bit subtracts 8, so a sprite can leave the 256-pixel picture by at most 8 columns on the left
   (`X = 0` with the shift bit) and by 15 on the right (`X = 255`, 16 pixels wide when zoomed): there is no far-outside position, nothing is wrapped and no Genesis bias is used. The spill is taken from
   the same selected per-line hits the native pass computes (64-entry walk, the 0xD0 terminator in 192-line mode only, the 8-per-line limit, SAT order), writes only into per-line spill slots, composes
   over the WRAPPED background of the line with the native priority rule and never touches the overflow/collision flags. A frame is a Level 3 frame only if every line was a supported WRAPPED line
   (left-column mask and the vertical-scroll lock still make the whole frame UNAVAILABLE); no background is invented for the sprites, and the 240-line mode stays unsupported. The Level 3 status text is
   *"Background repeats existing scenery. Only sprites crossing the screen edges can extend; this does not expand the game world."* Level 3 on the SMS can be at best Partial.
7. **Not done.** Game-specific object activation, camera changes, larger collision bounds, created sprites, raised hardware sprite limits, larger SMS tilemaps, a universal VDP interface, Game Gear, 32X,
   Saturn, any generated-C or save-state change (SEG-122 owns optional title-specific compatibility).

## Consequences

Level 3 is visibly different from Level 2 only for games whose VDP data places objects beside the picture (typically sprites that straddle an edge or are not yet culled); a title that submits none
shows Level 2 content in a Partial Level 3 frame. The extension cost is one more sprite evaluation per output row only while Level 3 is requested.

## Verification

`tests/genesis_extension_test.cpp` (independent sprite reference over authored and random SAT scenes: edges, flips, all sizes, priorities, overlap, link termination/cycles/out-of-range, 20/16 sprite and 40/32
cell limits, interlace 2, journaled and unjournaled SAT edits, ambiguous-state fallback, mode contract of the shared dispatch, runtime Original/Level 2/Level 3 switching with native-artifact, digest, VDP and
scheduler identity, steady-state allocation), `tests/genesis_viewer_extension_sdl_test.c`, `tests/sega_cd_viewer_extension_sdl_test.c` (real Sega CD frame path with an independent sprite model, Partial
journal caveat, frame-number stamping, rejected sprite pass, stale/pending Level 3 refusal, per-line limit, Original-vs-Level-3 guest identity) and `tests/sms_extension_test.cpp` /
`tests/sms_viewer_extension_sdl_test.c` (independent SMS sprite reference: X = 0/248/252/255, shift, 8x8/8x16, zoom, maximum spill, bounded reach, priority, transparency, 8-per-line, terminator, 192/224,
240, mask/lock fallback, latching, fuzz, native invariance), `tests/sv_viewer_core_test.cpp` and `libs/viewer/test/sv_host_test.c` (surface acceptance, notes, stale-mode refusal), all run with the real SDL3
presenter reading back the output pixels.
