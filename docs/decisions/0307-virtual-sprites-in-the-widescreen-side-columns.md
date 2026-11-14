# ADR 0307: virtual sprites in the widescreen side columns: coordinates, priority and budget

- Status: accepted (combined delivery F28: SEG-140-T009). Builds on [ADR 0301](0301-widescreen-geometry-rational-aspect-planner-and-bounded-surfaces.md) (the planner), [ADR 0303](0303-authored-virtual-sprites-vdp-decode-priority-budget-and-layered-overlays.md) and [ADR 0306](0306-virtual-sprite-authoring-transport-and-coverage.md).
- Presentation only. The guest, the VDP status, the save states and the native picture are untouched; the side columns are produced scenery, not VDP output.

## Decision 1: one additive coordinate space, per set

`GenesisVirtualSpriteSet::space` (a reserved byte; 0 is every existing set): **PICTURE** (0) draws only inside the native picture and is byte-identical to before (every F14/T008 golden holds); **PRESENTATION** (1) may also draw into the extension columns the host actually presents. Both spaces have the SAME origin: x, y are measured from the top-left pixel of the native picture, so an author needs no knowledge of the aspect. The left margin is `x < 0`, the right margin `x >= picture width`; a sprite may straddle either boundary. A per-set field was chosen over a per-sprite one because it needs one byte, one validation line and no repeated state; an unknown value is `RESERVED`.

For a 320 x 224 picture with square pixels the planner gives `side = (floor(224 * num / den) - 320) / 2`: 4:3 -> 0 (no column), 16:9 -> 39, 21:9 -> 101, 32:9 -> 238 (796 pixels wide). The columns a frame can draw in are the **visible** ones the host resolves for the current aspect (`sv_geom_resolve_visible`), a pixel outside them (beyond the outer boundary, or at 4:3 / Original) is clipped there, **never moved, clamped into the picture or counted**. A PRESENTATION clip rectangle may begin in a margin; everything else about positions and clips is the F14 validation.

## Decision 2: the ONE side priority policy

Native VDP priority classes (backdrop / low / high) exist only for native-picture pixels. The margins are the provider's authored or generic scenery, so **no class is invented for them**:

| Layer | In the native picture | In a presented margin |
| --- | --- | --- |
| `ABOVE_ALL` | above everything (unchanged) | draws over the margin scenery |
| `BETWEEN` | above low-priority, below high-priority (unchanged) | **cannot be established** |
| `BEHIND_PLANES` | visible only over the backdrop (unchanged) | **cannot be established** |

Fail closed: a `BETWEEN` or `BEHIND_PLANES` command whose drawable rectangle (after its clip, within the visible columns) reaches a margin rejects the **whole set** with `GENESIS_VSPRITE_REJ_SIDE_PRIORITY` (13, the offending index) and nothing is drawn; the same command inside the picture, or clipped back into it, is unchanged. The rule depends on the aspect of THAT frame (at 4:3 no margin exists, so nothing can be reached), deterministically. An author orders margin sprites with `ABOVE_ALL` or keeps ordered ones in the picture. A richer policy (classes derived from the producer's own plane candidates) is possible later without changing the transport; it is not built because it would assert priority equivalence the columns do not have.

## Decision 3: the original hardware budget counts margins like the picture

The per-line sprite limit and cell budget are about scanlines, not columns. A margin sprite occupies a slot and its cells on every row it touches after its clip, exactly like a picture sprite: under `LIMIT` it is dropped on rows where no slot is left (the hardware sprites come first), under the explicit `EXTENDED` opt-in it is drawn and the over-budget rows are reported. The accounting never silently exempts a margin sprite, and nothing about the emulated hardware changes.

## Decision 4: composition order and what is presented

The compositor draws onto the **produced extension surface** (the provider's or generic scenery is the base; the centre is re-derived from the native artifact) and the host presents that surface, not a temporary buffer. What the host displays decides what counts:

| Presentation | Margin sprites |
| --- | --- |
| Widescreen with `Raw` side style | displayed as composed |
| `Comfort` of generic scenery, `Borders` | the columns are REPLACED by a fill (or panels): the sprites are discarded and counted for nothing; the host says "virtual sprites not presented" |
| `Comfort` of provider-authored scenery | passed through as supplied: displayed |
| Original / 4:3 (no column) | nothing to draw in; the picture part is unaffected |
| Aspect outside the package's declaration, a planner rejection | the native picture alone is shown; nothing is drawn or counted |

Coverage counts `pixels_picture`, `pixels_left` and `pixels_right` only for regions that reached the final presented frame inside the declared range. A scenery provider never implicitly grants actor coverage; a sprite in the picture says nothing about a margin; a valid sprite wholly outside the visible geometry creates none.

## Limits

- The margins are not VDP output: nothing here says a game object exists or has art there. No Sonic or other game claim changes: the F15 scene-class feasibility values are unchanged, and the real packages submit no virtual sprites.
- The side treatments that replace the columns (Comfort of generic scenery, Borders) are not composited over: a sprite drawn on a decorative fill would not be game content.

## Verification

`genesis_vsprite_wide_tests` (the compositor on a synthetic surface: coordinates, regions, outer boundary, visible subset, the priority policy, validation, budgets, aspect changes leave nothing behind, bounded cost), `mod_vsprite_wide_test` (4:3/16:9/21:9/32:9 against the independent golden on the PRESENTED surface, side styles, Original, declaration, lifetime, and the two 32:9 demonstrations: ABI 1 and Luau), `mod_vsprite_abi2_test` (ABI 2 in the columns), `mod_vsprite_viewer_test` (every picture golden unchanged), the F14/F15 suites.
