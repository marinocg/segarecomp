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

`ui/icon-*.png` (16x16 pixel icons, tinted/upscaled with nearest-neighbour sampling) and `ui/cover-placeholder.png` (the generic cover shown when a game
has none) are original textures authored for the SEG-058 launcher overhaul in the palette of the launcher's own source constants
(`#08142F` panel, `#233E78` border, `#1557C9` primary, ...). They contain no commercial art. Buttons, panels and bevels are drawn in code with the same
colors; only icons and the placeholder are loaded from files. Text is always dynamic (Silkscreen).

The launcher draws titles and section headings with Silkscreen Bold and body text with Dear ImGui's embedded scalable face (`AddFontDefaultVector`); no other font file ships.
