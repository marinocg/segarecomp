# ADR 0314: a provider may leave a scenery plane to the VDP-derived extension

- Status: accepted (W04). Extends [ADR 0309](0309-abi-2-authored-scenery-window.md) and [ADR 0313](0313-hardware-piece-commands-independent-provider-contributions-and-two-scenery-windows.md). No game-specific behavior; additive (a provider that submits both planes behaves exactly as before).

## Context

Authored scenery exists to replace what the generic extension gets wrong: the VDP plane wraps over a few streamed columns, so beyond them it repeats stale cells. That is true of a world-map plane. It is **not** true of a tiling background whose map period equals the VDP plane's own period (for example a 32-block, 64-cell background): the VRAM plane already holds one whole period, so the wrapped plane is exact, and an authored window can only reproduce it.

Measured on a real scrolling game (aggregates only; 5 scenes x 8 frames, 40 frames, about 90,000 off-picture cells per plane and frame): the provider's name-table words for plane B were **identical to the VDP's own words in 100% of the cells it answered**, in every frame, in the playfield rows, the rows scrolled to 0 and the status rows (so the VDP-derived extension was already exact there), while plane A's words differed from the wrapped VRAM exactly where the world map continues beyond what the game had streamed (the case authoring exists for). The only failures left after ADR 0313 in the two persistent stages were plane B cells: in one stage the record camera of plane B was 0 while the VDP scrolled the playfield rows by -482 (cells 30..129, 60 cells away), because that plane's playfield scroll does not come from the record the provider reads (it comes from a per-line table that the game mirrors in work RAM at an address that was not stable across stages and frames). Authoring those cells brought no content, only failures.

The contract could not express "serve plane A, leave plane B to the VDP": a frame was served only with a window for each plane, and an unanswered cell was a failure.

## Decision

1. A frame is served when **at least one** plane has a window. A plane that has no window is **left to the VDP-derived generic extension**: its cells are produced exactly as without a provider and this is **not a failure** (a new `scenery_word` result `GENESIS_COMPAT_WORD_GENERIC`, returned by the adapter for a plane without windows). A frame with no window at all is still not served.
2. Every other rule is unchanged: a cell of a plane that has a window and falls outside all of its windows is an unanswered cell (generic fallback, a failed authored-scenery claim, ADR 0313); any result other than a word or `GENERIC` is an unanswered cell; the generic cells still never invent a tile.
3. The producer still asks the provider for every off-picture cell of both planes (a provider that wants the generic extension on one plane answers `GENERIC`/has no window), so the generic frame and the partly authored frame differ only where the provider authored.

## Provenance (what earns authored credit)

"Authored" is earned only by words the provider actually supplied; the absence of failures alone is not evidence. The producer counts the cells answered with a word (`answer == 1`):

- **Whole frame** (`GenesisExtensionResult::authored`): at least one answered word, no unanswered cell, and none of the existing degradations (window edge, per-column vertical scroll, no VRAM journal). A frame in which every plane is left to the generic extension earns nothing (it is exactly the generic frame), and neither does a frame whose cells are all unanswered (that is a failure).
- **Row level** (`authored_rows`, ADR 0310): a row is marked when its off-picture cells include at least one answered word and none unanswered, outside window-edge rows. A row whose cells are all generic by declaration is not marked; a row that mixes an authored plane with a generic one is. The credit therefore measures where the provider's own data is on screen, not where nothing went wrong.
- A mixed-plane frame (one plane authored, the other generic) stays valid and is credited, because its authored plane is the provider's data.

## Consequences

- A package whose plane is provably what the generic extension already shows submits nothing for it: no failures from scroll sources it cannot read, no authoring claim beyond what it verified.
- Tests: `genesis_native_compat_test` (a plane answering `GENERIC` reproduces the generic frame byte for byte and is no failure; the provenance matrix: both planes authored, one authored and one generic in either order, both generic earns nothing, every cell unanswered or an undefined value is a failure with no credit, and a row-level mixed frame marks exactly the answered rows), `mod_scenery_abi2_test` (plane B without a window: plane A authored equal to the golden, the frame equal to the two-plane frame, scenery credited, no fault; unanswered cells still keep the HUD).
