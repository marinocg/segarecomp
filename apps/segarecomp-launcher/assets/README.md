# Launcher assets

Original pixel-art produced for Segarecomp (not derived from Sega trademarked material):

- `background.png` -- sunset scenery background (portrait, cover-cropped to the window)
- `wordmark.png` -- SEGARECOMP pixel-art logotype (transparent)
- `icon-drop.png`, `icon-info.png`, `icon-folder.png` -- UI icons (transparent)

Fonts: [Silkscreen](https://github.com/googlefonts/silkscreen) by The Silkscreen Project Authors,
licensed under the [SIL Open Font License 1.1](fonts/Silkscreen-OFL.txt) (`fonts/Silkscreen-OFL.txt`).

All assets here are loaded at runtime (`SDL_LoadFileFromPath` + `stb_image.h` for images,
`ImGui::AddFontFromFileTTF` for fonts) via `segarecomp_launcher_asset_path`, never baked into
generated C or committed as generated/binary launcher state.

## UI textures (`ui/`)

`ui/icon-*.png` (16x16 pixel icons, tinted/upscaled with nearest-neighbour sampling) are original textures authored for the SEG-058 launcher overhaul
in the palette of the launcher's own source constants (`#08142F` panel, `#233E78` border, `#1557C9` primary, ...). They contain no commercial art.
Buttons, panels, bevels, option tiles and toggles are drawn in code with the same colors. A game without a cover shows a **title card** drawn in code
from a tinted crop of `background.png` (a different crop per game), the platform band, the title and an explicit "NO COVER ART" mark: it never
imitates box art. Text is always dynamic (Silkscreen).

`manifest.json` lists every runtime asset with its size, role and sampling, the removed first-kit placeholder, and the audit of the SEG-058 v2
kit textures (which were evaluated and why none of its backgrounds, covers or chrome textures ship). It is documentation and is not installed.

The launcher draws titles and section headings with Silkscreen Bold and body text with Dear ImGui's embedded scalable face (`AddFontDefaultVector`); no other font file ships.
