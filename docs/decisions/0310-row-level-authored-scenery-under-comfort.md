# ADR 0310: row-level authored-scenery provenance under Comfort

- Status: accepted (W04 renderer gap A). Builds on ADR 0265 (side presentation), ADR 0267 (native providers) and ADR 0309 (authored scenery window). Presentation only; no guest state; no game-specific behavior.

## Context

The extension producer declares a frame `authored` only when every off-picture cell came from the provider AND nothing degraded the frame, and Comfort bypasses its decorative fill only for an authored frame. A game whose status bar is the VDP window plane
(the picture's lower rows) always sets `GENESIS_EXT_FLAG_WINDOW_EDGE` (plane A is omitted beside those rows), so such a frame can never be authored: Comfort, the default, replaced BOTH margins on EVERY row, including the rows the provider had served completely
and correctly. Evidence: the synthetic fixture of `tests/genesis_native_compat_test.cpp` (window plane from cell row 24, every provider cell answered) is `Partial`, `authored == 0`, `compat_reason == 0`, while every row above the window was provider-served.

## Decision

1. The strict whole-frame verdict is unchanged: `authored`, `SvExtendedSurface::source`, the coverage counters and `sv_side_treatment` keep their meaning. A frame with some authored rows is still not an authored frame and earns no `scenery` credit.
2. The runtime additionally reports ROW provenance (`GenesisExtensionResult::authored_rows`, one bit per output row, and `authored_row_count`): a row is set only when the scenery component was accepted, no off-picture cell of that row was unanswered, and the window plane owns neither picture edge in that row; the whole mask is empty when the frame has per-column vertical scroll or no VRAM journal. A declined, stale, faulting or provider-less frame has none.
3. The surface carries it as an optional `authored_rows`/`authored_row_count` pair (zero = none, ignored for an AUTHORED surface). The host validates it whole (the count equals the set bits, no bit beyond the picture) and ignores it otherwise.
4. Comfort leaves the provider's pixels on the marked rows (both sides) and fills the other rows exactly as before. Borders replaces both margins entirely (an explicit user choice), Raw is the extension as produced, and the native picture is never touched. `sv_host_authored_rows` reports how many rows a Comfort frame kept.

## Consequences

- No second renderer and no new provider API: the existing per-cell answers already decide each row. Nothing changes for a frame without row provenance (every existing test is byte-identical).
- Coverage still reports a window-plane frame as not authored (honest: partially supported); a later change may add a row-level counter if a consumer needs it.
