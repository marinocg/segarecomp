# ADR 0263: SEG-120 (M120) - Master System Level 2: a presentation-only, honestly "wrapped" background extension

## Status

Accepted (SEG-120, milestone D of optional 16:9 support; builds on ADR 0261 settings/UI and ADR 0262 shared presentation). Sprites follow in SEG-121.

## Context

Level 2 shows scenery to the left and right of the unchanged native picture on a 16:9 display. The Master System is different from the Genesis: Mode 4 has a single background layer whose name table is
**32 columns = 256 pixels wide and wraps horizontally**, i.e. exactly the width of the native viewport. A horizontal sample outside the viewport is `(x - R8) & 255`, the very tile pixel that the viewport
shows at `x & 255`. Extending the sampling range therefore *repeats scenery that is already on screen*; it can never reveal more of the game world (a game that scrolls a larger map does so by rewriting the
name table column by column, and those columns are not in VRAM yet). The extension is real VDP data, but it is not "more picture".

## Decision

1. **Invariant.** Nothing the console does or records changes: Z80 execution, scanline scheduling, VDP registers/VRAM/CRAM, sprite overflow/collision flags, input recording, PSG audio, `pixels`/`last`,
   `SmsFrameRecord`, its SHA-256 and the machine/VDP digests are identical with the extension on or off. The extension is write-only to its own arrays and is never serialized.
2. **Production point.** `sms_render_line` (the VDP line hook, one call per active line with that line's authentic state) additionally writes, when `SmsRenderer::ext` is attached and a side was requested,
   `side` columns left of x = 0 and right of x = 255 into preallocated `SmsExtension` arrays (6-bit CRAM values, like `pixels`). It reuses the line's own background samples (the left side is the last `side`
   source samples, the right side the first `side`, which is exactly `(x - hscroll) & 255` for x outside the viewport); nothing is rebuilt from the finished framebuffer and no state snapshot is stored.
   It is placed before the sprite pass and touches neither the sprite arrays nor the status flags. Per-line state (R0 lock bit, R7, R8, CRAM, VRAM) is therefore honoured line by line, and frame-latched R9 is
   the same value the native line used. The requested side (per frame height) is latched at line 0, so a request change applies to the next frame and no frame mixes Original and Level 2.
3. **Publication and lifetime.** At frame completion (next to the native `last` copy) the work arrays are copied to the published arrays together with `SmsExtResult {sequence, height, side, class, reason}`;
   `sequence` is `SmsRenderer::frames_completed`, the identity of the native frame it extends. The target is owned by the SDL3 adapter (`SmsSdl3Viewer`), attached to the renderer by
   `sms_viewer_rig_set_extension` (it survives `sms_viewer_rig_reset`, which re-initialises the renderer), and sized by the constants `SMS_EXT_MAX_SIDE` (80; the widest need is 71 for 224 lines with square pixels)
   times `SMS_FB_MAX_HEIGHT`. No allocation happens per frame. `SmsViewerHost::present` keeps its signature: the adapter reads the target through its own pointers.
4. **Classes.**
   - `WRAPPED` - every line of the frame was produced. Mode 4 has only this case: the produced samples are, by the arithmetic above, the native samples at `x & 255`. Reported **Partial**, with the Video
     menu sentence *"Background repeats existing scenery; this does not reveal more of the game world."* (`SvWidescreenCaps::partial_note`). Never Available and never described as full widescreen.
   - `DISTINCT` - reserved in the vocabulary for scenery with provenance outside the native viewport. It would need a name table wider than the viewport, which Mode 4 does not have; the renderer never
     produces it and nothing upgrades `WRAPPED`. Property tests assert that every produced sample equals the native-sample reference, which is the proof that no DISTINCT case exists in this hardware model.
   - `UNAVAILABLE` (typed reason) - a line that cannot be extended soundly. Behaviour outside the physical screen is undefined for the **left-column mask** (R0 bit 5) and the **right-hand vertical-scroll lock**
     (R0 bit 7: columns 24-31 are fixed by physical screen position); a requested side above the buffer bound is a **geometry** failure. One unavailable line makes the frame unavailable; the host then presents
     the Original picture and the next valid frame recovers. The **horizontal-scroll lock** of the top two tile rows is a function of the screen row, not column, so it is honoured per line. A disabled display
     yields backdrop on the sides (the border is the backdrop colour). The fine-scroll gap (`x & 255 < R8 & 7`) is the backdrop, periodically, so the picture seams stay consistent. Mid-frame register, CRAM and
     VRAM changes are real per-line data, not a reason to refuse.
5. **Supported modes.** 192- and 224-line Mode 4 only. The 240-line mode (and every other unsupported shape) keeps its existing typed unsupported behaviour: the renderer renders nothing, the machine stops
   with the existing error, and the extension adds no acceptance of any mode. Native display-mode support is not widened for widescreen.
6. **Host.** The adapter (`sms_viewer_sdl3.c`) builds an `SvExtendedSurface` per presented frame (6-bit to RGB888 with the native conversion; the centre is the native picture) and calls
   `sv_host_present_extended(..., sequence = frames_completed, surface)`; CRT, fit/integer scaling, fullscreen and resize apply as for Genesis. A stale result is offered without pixels so the shared check
   rejects and counts it; an unavailable or mismatching result is the Original picture. The requested mode is read once per guest frame in the adapter's poll (between frames), sets the per-height side from
   `sv_layout_extension_side`, and the result stamped with the frame decides the presented mode. Capability: initially Partial (the best Mode 4 can do), then Partial after a WRAPPED frame, Unavailable after an
   UNAVAILABLE frame, Available only for a (non-existent) DISTINCT frame; Level 3 is Unavailable and falls back to Level 2. A frame is produced under the preference of the previous poll, because line 0 of a
   frame is retired at the end of the preceding guest iteration, so a change shows one frame later; whole frames only.
7. **Shared change.** `SvWidescreenCaps` gains an optional static `partial_note`; `sv_widescreen_status` returns it for a Partial effective level, and the Video page appends it when Level 3 falls back to a Partial Level 2.
   Platforms that set nothing keep the generic *"Partly available for this game"* wording.
8. **Not done.** Sprites (SEG-121), Game Gear, any 240-line support, tilemap or scroll-logic changes to obtain a better class, game-specific behaviour.

## Consequences

Level 2 on the Master System is honest scenery repetition, labelled as such; for a game with a 256-pixel-wide static screen it looks like a seamless tiling, for a scrolling game it shows the wrapped column
the game is about to rewrite. Games that need the left mask or the vertical lock (many scrolling games) fall back to Original, because their off-screen behaviour is undefined. A future provenance-bearing source of
wider scenery (e.g. a game-specific map) would be a new decision; this one must not be read as a promise of extra gameplay view.

## Verification

`tests/sms_extension_test.cpp` (renderer: independent reference sampler, scroll/lock/mask/mode/mid-frame/latching/geometry/fuzz native invariance), `tests/sms_viewer_extension_sdl_test.c` (real SDL3 adapter and
host: pixels read back, Original/Level 2/Level 3, 192/224, unavailable fallback, failure injection, no texture churn, guest invariance against a control run, 240-line stop, unlinked-extension viewer),
`tests/sv_viewer_core_test.cpp` (status wording), and the existing SMS renderer, VDP, viewer and equivalence suites unchanged.
