# ADR 0309: ABI 2 authored scenery as a per-frame name-table window

- Status: accepted (combined delivery F29: SEG-140-T011). Builds on [ADR 0305](0305-live-scheduled-mods-and-native-abi2-providers-in-built-programs.md) (the scheduled provider and its typed render feature) and the ABI 1 extension producer (`GenesisNativeCompat::scenery_word`, unchanged).
- Presentation only: nothing here reaches guest state, VRAM or a save state. No game-specific behavior.

## Context

A native ABI 2 provider already delivers HUD overlays and virtual sprites from its `render` callback, but the scheduled-mod adapter answered no scenery, so offscreen columns could only be the generic wrapped plane. The producer asks `scenery_word(plane, row, x, hscroll, vscroll)` for every offscreen cell of BOTH planes, inside a per-pixel loop that must not call guest-influenced code.

## Decision

1. `SegaGenesisRenderApi::scenery_window(plane, cell_x0, cell_y0, cols, rows, words)` (second reserved slot; table size unchanged) submits, during `render`, a bounded name-table window (<= 128 x 64 cells, <= 4096 words) per plane in unwrapped plane-cell coordinates. It requires the existing `scenery.words` capability (`SEGA_MOD_E_CAPABILITY` otherwise); malformed, oversized or duplicate submissions are range faults of the callback. `scenery.words` is now granted to the scheduled native path and publishes the render feature.
2. The host answers `scenery_word` from the committed window only (cell = floor((x - hscroll) / 8), floor((line + vscroll) / 8)). A cell outside the window fails the frame's authored claim honestly; the provider is never called from the producer and the host never invents a tile. The host's own VDP code decodes patterns, palettes and flips, so art must be resident in VRAM.
3. Frame validity: windows are cleared when a render callback starts, committed only if it did not fault, and served only for the one produced frame they describe (render of guest frame N feeds the frame produced when the runtime counts N + 2, the same one-onset lag as every route). A rewind, load or skipped callback therefore never reuses a stale window. A frame is served only when BOTH planes have a window (an empty plane submits blank cells), because the producer asks both.
4. The played-program build passes the `scenery.words` producer capability (`SEGARECOMP_NATIVE_PROVIDER_CAPS` 12U) only when the package was granted it; scenery, hud and actors coverage stay independent.

## Consequences

- No second renderer, scheduler or ABI version; ABI 1, Luau and Vanilla paths are unchanged (the Luau route never carries scenery).
- The provider derives world coordinates from guest state it may read under its declared ranges; keeping its window content a function of the world cell (not of the camera) makes the one-onset lag harmless apart from the window's extent, which must leave a margin for the next frame's scroll.
