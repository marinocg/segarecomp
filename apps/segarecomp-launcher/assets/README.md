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
