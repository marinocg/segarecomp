# Sonic the Hedgehog 1 native widescreen (capability `genesis.sonic1.native-widescreen.v1`)

Optional, opt-in and portable (a schema 2 `.segapatch`, see `patch-packages.md`; ADR 0267). The capability is a statically compiled, title-specific component of the product
(`platforms/genesis/native_compat/sonic1_widescreen.c`) behind a generic interface (`platforms/genesis/runtime/native_compat.h`). It exists only in a program built from a verified package for the exact
original image (Genesis, USA/Europe REV00, SHA-256 `46160baa...f2a6`); Vanilla, BPS-only and other games contain none of it. It reads a read-only work-RAM snapshot taken at the previous VBlank onset
(the state the game used to prepare the displayed frame), never writes guest state, and falls back to the generic presentation (with an accurate status) for anything it does not verify.

## Components

* **scenery** (Green Hill Zone acts 1 and 2 while the level runs; act 1 validated start to finish, act 2 over the part explored in validation; every frame is identity-checked): the extra side columns come from the level layout, the 256x256 chunk table and the 16x16 block table the game itself decompressed
  into work RAM, looked up at the world coordinates of the game's own per-line scroll (not from the wrapped plane nametable). The generic VDP code decodes patterns, palettes, flips,
  priority and sprites exactly as for visible columns. Outside the level layout a cell is blank (the backdrop shows); nothing is mirrored, repeated or invented.
* **hud**: the game's own HUD, relocated. The patch stops the HUD object queueing its sprites (two `jmp DisplaySprite` become `rts`; every counter and update routine keeps running) and the product draws the very
  same sprite pieces from the displayed frame's own VRAM tiles and CRAM palettes: identical font, size, colours, digits and layout (SCORE / TIME / RINGS on top, the Sonic icon with the lives at the bottom), no
  background, the same rows. Only the horizontal position differs: the overlay's left edge is the left edge of the visible surface (it covers the left side and the first picture columns it needs), or, with no visible
  side (4:3, a zero Widescreen Width, Original), the original's own place inside the picture. The label flash (RINGS with no rings, TIME at nine minutes) follows the object's own rule. Because the digits are the game's own
  tiles, the numbers are always exactly what the original would show for that frame.

## Validated scenery scope

Per zone and act: the scenery component is enabled only where the provider's cells were measured against the game's own picture in view (level select jump, then the game's debug fly mode crossing the level
right, down, left, down, right, up, left; guard-declined frames excluded). Numbers are mismatching cells / compared cells over the traversal (foreground, background inside the game-streamed window).

| zone / act | frames | camera x max | foreground | background |
|---|---|---|---|---|| 0/0 (Green Hill acts 1-3) | 2415 | 8640 | 0/2700320 | 2663/2353674 |
| 0/1 (Green Hill acts 1-3) | 2415 | 7871 | 0/2703680 | 17572/2327748 |
| 0/2 (Green Hill acts 1-3) | 2416 | 8592 | 0/2691360 | 2588/2354879 || 2/0 (Marble acts 1-3) | 2422 | 6079 | 0/2712640 | 7/2712640 |
| 2/1 (Marble acts 1-3) | 2422 | 6079 | 0/2712640 | 0/2712640 |
| 2/2 (Marble acts 1-3) | 2422 | 6144 | 0/2712640 | 0/2712640 || 1/0 (Labyrinth acts 1-3 and the fourth-slot act (Scrap Brain 3)) | 2435 | 6591 | 0/2727200 | 0/2727200 |
| 1/1 (Labyrinth acts 1-3 and the fourth-slot act (Scrap Brain 3)) | 2434 | 4271 | 3/2726080 | 0/2726080 |
| 1/2 (Labyrinth acts 1-3 and the fourth-slot act (Scrap Brain 3)) | 2435 | 8239 | 672/2721600 | 0/2721600 |
| 1/3 (Labyrinth acts 1-3 and the fourth-slot act (Scrap Brain 3)) | 2434 | 8383 | 0/2720480 | 0/2720480 || 3/0 (Star Light acts 1-3) | 2431 | 8127 | 0/2722720 | 0/2406873 |
| 3/1 (Star Light acts 1-3) | 2431 | 8127 | 0/2722720 | 0/2415766 |
| 3/2 (Star Light acts 1-3) | 2431 | 8192 | 0/2722720 | 0/1940332 || 4/0 (Spring Yard acts 1-3) | 2421 | 8624 | 0/2711520 | 0/2711520 |
| 4/1 (Spring Yard acts 1-3) | 2421 | 8624 | 246/2711520 | 0/2711520 |
| 4/2 (Spring Yard acts 1-3) | 2421 | 8624 | 0/2711520 | 0/2711520 |

## Frame validation and fallback

A frame is served only if (1) the game mode is a running level, (2) the camera, layout and chunk indices are in range, (3) the VDP is in the verified configuration and the scroll it applies equals the game's
own per-line scroll in work RAM (so RAM and VDP describe the same frame), and (4) the provider's own cells for the part of the world in view agree with what the VDP nametables hold there, sampled every 8 pixels, tolerating up to 10% foreground and 14% background disagreement for the
streaming lag the game itself has (a gross disagreement, e.g. a fade, a level transition or another level, declines the frame). Anything else (another zone, the level start sequence, title, special stage, a different revision) is served by the generic
extension with its accurate status, or not at all; Vanilla and BPS-only builds are never affected. A background band's world position is only defined modulo the 512-pixel plane: the provider uses the alias nearest the
camera of the region the game streams it from (the upper region from the first background camera, the region below the game's split from the second one); cells left of world x 0 are blank (the backdrop shows).

## Known limits (stated, not hidden)

* At the very start of an act the camera sits at x 0, so the strip left of the level (and right of its end) is the backdrop colour: there is no world there and nothing is invented.
* Sprites beside the picture exist only where the game submits them (Level 3 is Partial); the game does not activate objects beyond its own window.
* The level start sequence (title card) is not extended. Zones other than Green Hill Zone acts 1 and 2, special stages, bonus screens and the ending are not extended (the HUD still replaces the game's own in every running level).
* The patched program runs the game with the HUD sprites not queued (slightly different guest timing from Vanilla by design); between presentation modes of the same variant guest state and audio are identical (tested).
* The 16:9 side is 39 pixels wide while the original HUD is 96 pixels wide, so the relocated HUD still covers the first picture columns (about 57 of the 96, against the 16 + 96 the original used).

## Provenance

A frame is authored only when the provider served the scenery of that very frame and nothing degraded it; a declined, HUD-only or unsupported frame keeps the generic extension and Comfort.
