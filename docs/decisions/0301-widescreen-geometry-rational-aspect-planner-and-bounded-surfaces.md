# ADR 0301: rational widescreen aspect planning through 32:9, bounded surfaces, legacy settings unchanged

- Status: accepted (combined delivery F12: SEG-139-T001, SEG-139-T002, SEG-139-T003, SEG-139-T005). Evidence base: [widescreen-geometry-audit.md](../architecture/widescreen-geometry-audit.md) (citations at `ce49e73`) and [widescreen-scenery-lookup-measurement.md](../architecture/widescreen-scenery-lookup-measurement.md).
- Builds on [ADR 0262](0262-seg-118-m118-presentation-only-extended-surface-and-genesis-level-2.md), [0265](0265-seg-123-m123-widescreen-side-presentation-comfort-borders-raw-and-authored-bypass.md) and [0267](0267-seg-124-native-widescreen-compatibility-provider.md). Presentation only: nothing here reaches guest state, the native picture or a save state.
- Not in scope: per-mod / per-scene coverage declarations (SEG-139-T004/T006), Master System and Sega CD coverage (SEG-147-T009, SEG-148-T010), a menu or launcher control for the exact ratio, any rendering feature.

## Decision 1: one checked planner, rejection instead of clamping

`sv_geom_plan(native_w, native_h, pixel_aspect, target_ratio, &plan)` (in `libs/viewer/include/sv_layout.h`, pure C, no allocation) returns the side extension per side and the surface size for a requested rational display aspect, or a typed status:

- `SV_GEOM_BAD_NATIVE`: the native size is outside `1..4096`;
- `SV_GEOM_BAD_ASPECT`: the ratio is unset, a term is outside `1..4096`, or the ratio is outside `[4:3, 32:9]`;
- `SV_GEOM_TOO_WIDE`: the required side exceeds `SV_GEOM_MAX_SIDE`.

A non-OK plan has side 0 and surface size 0. The planner **never clamps**. Every consumer fails closed on a rejected plan: the Genesis producer reports `NO_PLAN` (UNAVAILABLE), the host shows only the native picture. The arithmetic is the exact integer formula the legacy 16:9 code used, generalised to `n:d` (square pixels `floor(h*n/d)`, 4:3-corrected `floor(3*w*n/(4*d))`, side `floor((total - w)/2)` when the extra is at least 2), in `int64_t` with terms bounded at 4096. It is deterministic and monotonic in the ratio (property-tested).

## Decision 2: the supported surface is 672 source pixels per side

`SV_EXT_MAX_SIDE` / `SV_GEOM_MAX_SIDE` / `GENESIS_EXT_MAX_SIDE` rise from 256 to **672**. Reason (from the audit): the widest Genesis need at 32:9 is a 256 x 448 square-pixel picture at 668 per side, and 672 is the smallest multiple of 16 above it. The maximum Genesis surface becomes 1664 x 448 RGB24 = 2,236,416 bytes (previously 774,144), allocated once (static adapter surface, one host copy, side buffers); the cost is memory only, and it is a fixed allocation, not a per-frame one. Raising the bound exposed a latent overflow that the same change fixes: the side-fill fade table had 257 entries (`SV_SIDE_MAX_FADE`) and is now sized from the bound (`SV_EXT_MAX_SIDE + 1`), verified with an exactly-sized scratch under AddressSanitizer at the maximum side.

Anything beyond (a picture so narrow that a requested ratio needs more than 672 per side, for example 200 x 448 at 32:9 which needs 696) is rejected, not shown partially.

## Decision 3: legacy settings keep their meaning bit for bit

The 4:3..16:9 `video.widescreen_width` slider, the legacy `sv_layout_extension_side` (16:9, clamped at the old 256, now named `SV_EXT_LEGACY_SIDE`) and `sv_layout_visible_side` are unchanged; `sv_geom_resolve_production` / `sv_geom_resolve_visible` select between them and the planner. A recorded fixture (352 values over 256/320 x 192/224/240/448 x both pixel policies x width 0..100, FNV-1a recorded from the unmodified functions at `ce49e73`) proves the legacy production and visible sides are identical. A side effect the planner states honestly: the legacy clamp under-supplies a 256 x 448 square-pixel picture at 16:9 (256 instead of 270); the exact planner says 270, the legacy path keeps 256 so existing frames do not change.

## Decision 4: the exact ratio is one optional global setting

`video.widescreen_ratio=N:M` (decimal terms, `4:3` to `32:9`; presets 4:3, 16:9, 21:9, 32:9, anything else in range is a custom rational). Unset is the default and is **not written**, so existing settings files stay byte-identical. An unreadable or out-of-range stored value is dropped (the legacy width then applies); it is never clamped to the nearest valid ratio. When set it replaces the legacy width for both what the producer makes and what the host shows (the visible side is also limited by what was produced, so a wider request than a platform delivered never invents columns). It is a global-file setting only: the per-game profile (`video.widescreen`, side style, width) is unchanged, because a per-game exact ratio would silently change the meaning of existing profile files.

**Previous releases.** The settings parser ignores unknown keys (an unrecognised line is skipped), so a release without this change reads a file containing `video.widescreen_ratio` as if it were absent and keeps using `video.widescreen_width`, which is preserved untouched. A test removes the single new line from a file with a ratio set and checks the remainder is exactly the legacy file.

## Decision 5: presentation modes

The host has two scale modes (fit, integer) and two pixel policies; a wider-than-window surface is letterboxed and the native rectangle is always centred and never stretched. There is no separate "stretch" or "crop" mode; "crop" exists only as the centred visible-side crop of the maximum-size texture. The matrix test verifies fit and integer placement for 4:3, 16:9, 21:9, 32:9 and a custom ratio in windows of five shapes: surface inside the window, centred, native centred on it, surface aspect equal to the plan within the side rounding, native pixels exact at an integer scale.

## Decision 6 (T005): no lookup optimization

The pre-registered criterion was not met (the 32:9 producer costs 10.57% of a 60 Hz frame, just above the 10% threshold, but `gprof` attributes only 40.4% of it to the plane lookup, below the 50% threshold; an unregistered ablation puts the share between that and 69%). No cache was added. The disagreement and the pre-registration correction (the `vdp_compose` span does not contain the extension producer) are recorded in the measurement document. A future optimization needs a profile that does not perturb ~10^8 tiny calls and a new pre-registered criterion.

## Consequences

- 21:9 and 32:9 (and custom ratios) work wherever the producer exists, with pixel-exact producer output verified against an independent reference at 101, 238, 266, 636, 668 and 672 pixels per side; a sanitizer run is clean on the planner, the producer fixtures and the host matrix.
- Memory grows only through the fixed maximum allocation; there is no per-frame allocation.
- Master System keeps its own 80-pixel producer bound and the Sega CD path inherits the Genesis producer's new bound; their coverage declarations stay with their own tasks.
- No user interface exists yet for the exact ratio; it is a settings-file value (limitation recorded in the audit).
