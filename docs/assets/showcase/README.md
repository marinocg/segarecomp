# Widescreen showcase screenshots

Six individually reviewed, reduced-size captures of **generated-native** gameplay (four Genesis 16:9 captures at 1200x675 with widescreen patches, two Sega CD 4:3 captures at 900x675 without any), published under the narrow exception in
[`docs/testing/commercial-games.md`](../../testing/commercial-games.md#promotional-screenshot-exception-seg-129). They are frames from the real viewer output (the composed 16:9 window, including the
provider's scenery and relocated HUD) saved by the viewer's existing automation capture seam (`SEGARECOMP_VIEWER_CAPTURE_DIR`), **not** the F12 screenshot, which saves only the source game pixels. They were then uniformly resized and palette-quantized for file size. Nothing was painted, extended or composited afterwards.
The Sonic 2 frame also shows a partial actor at the far left edge and the Sonic 3 & Knuckles background shows the wrapped-plane texture the caption mentions; the Streets of Rage 2 right edge repeats generic wrapped scenery.

| File | Game and image | Patch (maintained outside this repository, not distributed) | Setting | What the frame shows |
| --- | --- | --- | --- | --- |
| `sonic-1-green-hill-patched-16x9.png` | Sonic the Hedgehog (Genesis, REV00), Green Hill Zone 1 | native provider + HUD-relocation BPS (schema 2) | Enhanced Widescreen, width 100 (16:9), Raw Extension | game-authored scenery on both sides, original HUD relocated to the left edge, not duplicated |
| `sonic-2-emerald-hill-patched-16x9.png` | Sonic the Hedgehog 2 (Genesis, World REV00), Emerald Hill Zone 1 | native provider + HUD and actor-coverage BPS (schema 2, standard package; **changes gameplay slightly**) | same | authored scenery, relocated HUD, and a Buzzer badnik drawn in the extended right margin |
| `sonic-3-knuckles-angel-island-patched-16x9.png` | Sonic 3 & Knuckles (Genesis, lock-on composite), Angel Island Zone 1 | native provider + HUD and actor-coverage BPS (schema 2, standard package; **changes gameplay slightly**) | same | authored foreground scenery (background is partly the VDP's own wrapped plane on this act), relocated HUD |
| `streets-of-rage-2-stage-1-patched-16x9.png` | Streets of Rage 2 (Genesis, USA), stage 1 | data-only BPS (schema 1) that widens the game's sprite submission window | same | enemies drawn beside the native picture by generic Level 3. **No authored scenery or HUD**: the side scenery is generic, the HUD stays in the original place |
| `sonic-cd-palmtree-panic-4x3.png` | Sonic CD (Sega CD, USA), Palmtree Panic Zone 1 | none (no widescreen patch exists for Sega CD) | original 4:3 (aspect-corrected), built from the user's own BIOS (Sega-CD Model 1 V1.10 U) and disc with `--file-first structural` | plain gameplay frame; no widescreen content |
| `earthworm-jim-special-edition-new-junk-city-4x3.png` | Earthworm Jim Special Edition (Sega CD, USA), New Junk City | none | same | plain gameplay frame; no widescreen content |

The two Sega CD frames are saved from the same viewer capture seam, cropped to the picture (no resizing beyond the uniform 900x675 scale) with scripted START/RIGHT/jump input.

Provenance: built with the then-current `main` (v0.7.0 plus SEG-117 to SEG-128), scripted pad route, software renderer, bounded instruction budget and wall-clock timeout. Captures were taken in the first act / stage 1 of each game; the maintainer states that extended scenery and widescreen were validated in all levels, but these images do not show them. The packages are **not** bundled with any release; native packages are executable code and must come from
sources you trust. Each frame was checked by eye for stale columns, duplicate HUD and corruption; per-frame provider acceptance is not individually logged.

SEGA, Sonic the Hedgehog, Streets of Rage, Genesis and Mega Drive are trademarks of SEGA Corporation. The games and their artwork belong to their owners; segarecomp is not affiliated with or endorsed by them.
The Sega CD images were made from the maintainer's own BIOS and discs, which are not distributed. The maintainer published these screenshots on his own responsibility.
