# ADR 0265: SEG-123 (M123) - Side presentation of the widescreen extension: Comfort / Borders / Raw, and the authored-widescreen bypass

## Status

Accepted (SEG-123). Builds on ADR 0261-0264. Presentation only. Title-specific widescreen remains SEG-122.

## Context

The Level 2 / Level 3 producers (ADR 0262-0264) return real, hardware-derived data beside the native picture. Because the game was not written for a wider viewport that data can look distracting
(repeated or discontinuous scenery, sprites without surroundings, the Master System's wrapped 256-pixel name table). Whether the data is *pleasant* is a presentation concern, not a producer
concern, and a future verified per-title widescreen implementation must be able to show genuinely wider content untouched.

## Decision

1. **One new presentation setting.** `video.widescreen_side_style` = `comfort` (default) | `borders` | `raw`, stored in the shared settings file next to `video.widescreen`. Zero is Comfort, so an
   unconfigured or zero-initialised struct, an older file without the key and any invalid value are Comfort; the Level 2/3 preference is untouched. The style never reaches the producers, the guest, frame
   timing, digests, recording or input.
2. **Provenance on the extended-surface contract.** `SvExtendedSurface::source` (`SvExtSource`, last member): `SV_EXTSRC_GENERIC` (0, the default of every zero-initialised surface: hardware-derived Level 2/3
   data) or `SV_EXTSRC_AUTHORED` (extended content supplied by a verified, opt-in widescreen compatibility implementation). It is declared only by that implementation and is never inferred from the mode,
   the Available/Partial status, pixel similarity, a title or filename, or a width. An unknown value is generic. It controls **visual treatment only**: `sv_extended_surface_check` (identity, geometry,
   capacity, mode, status) runs before and independent of it, so declaring a surface authored cannot bypass any validation, grants no memory access and cannot change guest state.
3. **Decision table (`sv_side_treatment`, pure).** Raw: pass-through for both sources. Comfort: generic -> Comfort treatment, authored -> pass-through (unmodified). Borders: neutral panels for both (the
   user explicitly asked, so the preference overrides either source). A genuinely widened native picture presented without the extension path has no side area and is never treated.
4. **Treatments (`sv_side_fill`, pure, deterministic, no allocation).** The native picture is never written. Comfort derives an ambient colour per row from the 8 outermost native columns of each edge,
   smooths it vertically (radius 12 rows), and fades it horizontally with a quadratic falloff from 60% of the edge colour to a dark neutral at the outer boundary: no tile, mirror, duplicated sprite or seam
   is reproduced, and a change of the edge strip changes the field by at most that change (no amplified flashing). Borders are a flat neutral panel. It is decorative, not game content. Complex image
   analysis was deliberately not used; the optional narrow strip of real extension pixels was not needed by the visual fixtures and is not included.
5. **Host ownership.** `sv_host.c` retains the native picture and a private copy of the accepted raw extension in buffers allocated once at open (no per-frame heap allocation, no new texture, backend or
   dependency). `upload_picture` builds the frame texture from them: native centre always from the native picture; sides from the retained raw extension (Raw / authored under Comfort) or from the
   treatment. The same function re-applies the style at once when it changes in the paused menu (`sv_host_restyle`, also called by `sv_host_apply_settings`), without a new frame, guest reset, producer
   request or recording event. A retained surface produced above the currently requested level is never re-exposed, so a style/mode change cannot show stale sprite pixels. CRT presets, fit/integer,
   aspect, fullscreen and resizing apply to the composed texture exactly as before; the native screenshot (F12) still uses the native pixels only. The Genesis, Sega CD and Master System adapters are
   unchanged (they zero-initialise the surface, hence generic).
6. **UI.** Video & Display gets `Side Presentation` (Comfort / Borders / Raw Extension) next to Widescreen Mode; the launcher exposes the same setting. Content capability (Widescreen Mode status, Partial
   notes) and presentation treatment are reported separately: a Comfort frame is described as a decorative fill that "does not create new gameplay visibility", and an authored surface under Comfort
   says its full content is preserved.
7. **Automation seams (read-only, environment-gated).** `SEGARECOMP_VIEWER_CAPTURE_DIR` + `SEGARECOMP_VIEWER_CAPTURE_FRAMES` save the composed window output of chosen presented frames as BMP, and
   `SEGARECOMP_VIEWER_CAPTURE_ALL_STYLES` additionally saves the same frame in every style from the retained data. They never modify the guest, the picture or a setting and exist to compare the styles on real
   programs independent of timing.

## Consequences

- Raw is bit-identical to the pre-SEG-123 extension presentation; existing SEG-117..121 suites that verify extension pixels select Raw explicitly. The default Comfort changes only what generic extensions
  look like, never the native picture, the producers or the guest.
- A future per-title implementation sets `source = SV_EXTSRC_AUTHORED` and must prove that claim; this milestone adds no patch engine, registry, hook or CPU change.

## Verification

`sv_side_style_test` (SDL readback on H32/H40/interlace/SMS 192/224, square and 4:3, fit and integer, 720p and 1080p, CRT, fullscreen, resize; hostile synthetic scenes; Raw/authored bit-identical;
Comfort smooth and subdued; Borders flat; stale-sprite and invalid-surface cases; no texture churn), `sv_viewer_core_test` (setting, decision table, pure fill), the style probe inside the real
Genesis, Sega CD and Master System adapter suites, and the launcher widescreen GUI test. Local real-program visual comparison is summarised in the task record (observations only).
