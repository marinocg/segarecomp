# Sonic the Hedgehog 1 native widescreen (capability `genesis.sonic1.native-widescreen.v1`)

Optional, opt-in and portable (a schema 2 `.segapatch`, see `patch-packages.md`; ADR 0267). The capability is a statically compiled, title-specific component of the product
(`platforms/genesis/native_compat/sonic1_widescreen.c`) behind a generic interface (`platforms/genesis/runtime/native_compat.h`). It exists only in a program built from a verified package for the exact
original image (Genesis, USA/Europe REV00, SHA-256 `46160baa...f2a6`); Vanilla, BPS-only and other games contain none of it. It reads a read-only work-RAM snapshot taken at the previous VBlank onset
(the state the game used to prepare the displayed frame), never writes guest state, and falls back to the generic presentation (with an accurate status) for anything it does not verify.

## Components

* **scenery** (Green Hill Zone act 1 while the level runs): the extra side columns come from the level layout, the 256x256 chunk table and the 16x16 block table the game itself decompressed
  into work RAM, looked up at the world coordinates of the game's own per-line scroll (not from the wrapped plane nametable). The generic VDP code decodes patterns, palettes, flips,
  priority and sprites exactly as for visible columns. Outside the level layout a cell is blank (the backdrop shows); nothing is mirrored, repeated or invented.
* **hud**: a replacement for the game's own HUD. The patch removes the original HUD sprites (two `jmp DisplaySprite` become `rts`; every counter and update routine keeps running) and the product draws
  SCORE / TIME / RINGS / SONIC x lives in the original's colours in the left side space, or at the replaced HUD's place inside the picture when the visible side is too narrow (4:3, a low Widescreen Width,
  Original). The numbers are the displayed frame's own counters; the original on-screen digits lag the counters by one frame at a change (the game updates its HUD lazily).

## Provenance

A frame is authored only when the provider served the scenery of that very frame and nothing degraded it; a declined, HUD-only or unsupported frame keeps the generic extension and Comfort.
