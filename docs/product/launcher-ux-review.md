# Launcher UX review record

Running record of the native review for the launcher UX overhaul (REVIEW_TEMPLATE style). All captures use the synthetic fixture (`tools/launcher_ui_fixture.py`, `--no-build`: metadata-only states), `SEGARECOMP_NO_COVER_FETCH=1`, isolated home and cache directories. No commercial image, BIOS, trace or artwork is involved. Images were opened and inspected; they are not pixel-hash comparisons. Only small synthetic crops are committed; full capture sets stay in the reviewer's local scratch directory.

## T001: text rendering (SEG-150-T001)

### Audit result (pinned SDL 3.4.16, Dear ImGui 1.92.9b, SDL_Renderer backend)

- The SDL3 backend reports `io.DisplayFramebufferScale` = window pixel density each frame; Dear ImGui 1.92 derives the glyph raster density from it automatically (`g.FontRasterizerDensity`). `FontScaleDpi` stays 1.0 on purpose: the launcher sizes text in explicit logical pixels (`ctx.u()`), so it must not be multiplied a second time.
- `SDL_SetRenderScale(output / window size)` after `ImGui::Render` makes logical units map to physical pixels. The SDL_Renderer backend then uses a clip scale of 1 when the renderer scale is not 1, so clipping is not scaled twice. The glyph atlas is baked at the framebuffer density and drawn at the same scale: texels map 1:1 to pixels. There is no whole-window intermediate texture.
- Layout scale `ctx_.S` = OS display scale / pixel density (Windows fractional scaling: density 1, S = scale; macOS Retina: density 2, S = 1) times the compact factor. This remains the contract; it was not changed.
- Measured defects (fixed): the body face was Dear ImGui's embedded thin vector font, and "bold" was that same face drawn twice with a 0.6 px offset (smeared stems, wider measured text than the drawn text). Now: Inter Regular and Inter SemiBold (real faces, SIL OFL 1.1, unmodified static TTFs of release 4.1) via `AddFontFromFileTTF`; the double draw and its 0.6 px measurement fudge are gone. Silkscreen Bold stays for headings only. A missing font file falls back to the embedded face (regular) and to the regular face (bold).
- Not changed: filtering (covers filtered, pixel art nearest), FreeType (not evaluated; the SDL_Renderer/stb_truetype result is already clean at 1:1), atlas handling.

### Before / after (1:1, unresized, 1080x740 logical = 1080x740 physical, density 1)

`screenshots/launcher-ux/t001-text-before-after-1to1.png`: top half before (main 44ab620), bottom half after. Before: monospaced-looking thin glyphs, doubled-stroke bold, descriptions cut off ("may fall back to ..."). After: proportional Inter with distinct regular and semibold weights; the same descriptions now fit. Observed in the full 1080x740 captures of Library and Game details: no clipping introduced, counts and statuses legible. Reviewer: the implementing agent, by opening the images; no independent image-aware review yet (task validator).

### DPI evidence

| OS / video driver / renderer | Native or simulated | Window pts | Output px | OS scale / density | Logical UI scale | Font | Result |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Linux WSL2 / SDL dummy / software | simulated headless | 1080x740 | 1080x740 | none / 1.0 | 1.0 | Inter Regular + SemiBold, Silkscreen Bold, raster density 1.0 | PASS (layout and text) |
| Linux WSL2 / WSLg Wayland / SDL_Renderer (SDL 3.4.16) | real window | 1080x740 | 1080x740 | 1.0 / 1.0 | 1.0 | same | PASS at density 1: `screenshots/launcher-ux/t001-wslg-wayland-crop-1to1.png` |
| Linux WSL2 / WSLg X11 | real window | 1080x740 | 1080x740 | 1.0 / 1.0 | 1.0 | same | capture produced, same size; not separately inspected |
| Any host, `SEGARECOMP_SHOT_SCALE=2` | logical layout aid only | 1080x740 | 2160x1480 capture | density stays 1.0 | 2.0 | raster density 1.0 | NOT physical HiDPI evidence |
| Windows fractional scale (125/150/200%) | not available | n/a | n/a | n/a | n/a | n/a | NOT VERIFIED |
| macOS Retina | not available | n/a | n/a | n/a | n/a | n/a | NOT VERIFIED |
| Linux native fractional/2x display | not available (WSLg reports 1.0) | n/a | n/a | n/a | n/a | n/a | NOT VERIFIED |

Cross-platform crispness at high density is therefore not claimed; the code path is the backend-supported mechanism and is expected to hold, but needs the Windows/macOS hosts above (planned in the milestone acceptance task) or CI/manual evidence.

### Known limits

- Inter covers Latin, Greek and Cyrillic; CJK and other scripts are not in the bundled faces (the embedded vector face had the same limit). Non-Latin file names in those scripts show fallback glyphs. A merged fallback face is a later decision, not added here to avoid a large font.
- Glyph-boundary ellipsis uses the same measurement as drawing (`CalcTextSizeA`), so truncated labels end on whole glyphs.

## T002: tokens, shell and responsive navigation (SEG-150-T002)

Native launcher, SDL dummy video driver with the software renderer (layout test, not a GPU or physical-display result), synthetic fixture, density 1.0, UI scale 1.0. Reference screens are the operator's simulated design pack (visual direction only; product behavior outranks them). Reviewer: the implementing agent, by opening each capture; no independent image-aware review yet. Committed crops: `screenshots/launcher-ux/t002-shell-720x520-library-settings.png`, `t002-shell-1080x740-library-settings.png`, `t002-shell-1600x900-library.png`, `t002-storage-back-720x520.png` (the Storage Back button was captured on a direct page jump, so it reads "Library"; from Settings it reads "Settings").

Implemented: sidebar = Library, Settings and one filled primary Add game button (the library toolbar's own Add game is now secondary, so the screen has one filled primary); Storage left the sidebar and is opened by a Storage button in the Settings header (and the existing Manage Storage button), with Back / Escape / B returning to Settings; the open destination is a navy tile with an accent bar, the filled blue belongs to the primary action; compact navigation (below 900 px) stacks icon over label (labels, plus tooltips); content padding 12-14 (narrow) / 20-22 px (normal); banner height `0.20 * height`, 92-150 px; status marks pair color with a symbol (`ui::tone_mark`: check, warning, info; neutral states keep a dot) on cards, rows, the Details pill and toasts; spacing/radius/type constants and `tone_icon` live in `ui_kit.hpp`.

| Reference | Native capture | Viewport | Scale | Deviations | Severity | Result |
| --- | --- | --- | --- | --- | --- | --- |
| 03_library | Library | 1080x740 | 1.0 | Reference has Add game in the page header and a taller card grid with a title/description block; native keeps Library Home heading, search + filters toolbar and a secondary Add game next to search. Sidebar labeled, banner 148 px (spec 110-145) | P2 | Accepted; content redesign is T003 |
| 03_library | Library | 720x520 | 1.0 | Compact rail 72 px with icon over label (Library, Settings, Add game); list cards; no clipping of the rail labels | none observed | PASS |
| 03_library | Library | 1600x900 | 1.0 | Banner capped at 150 px, six cards per row, empty space below content is plain water; no enlarged decoration | none observed | PASS |
| 18_settings | Settings | 1080x740 | 1.0 | Reference groups settings differently (T007); native keeps six tabs. Storage button sits in the header right; sidebar highlights Settings | P2 | Accepted |
| 18_settings | Settings | 720x520 | 1.0 | Section drop-down plus Storage button fit on one row; body scrolls | none observed | PASS |
| 18_settings | Settings | 1600x900 | 1.0 | Tabs left, body wide, Storage at far right (long eye travel) | P2 | Accepted |
| (storage) | Storage | 1080x740, 720x520 | 1.0 | Back button added left of the sort drop-down; Settings stays highlighted in the sidebar | none observed | PASS |
| (modal) | Remove-build-intermediates and first-run Keep intermediates | 1080x740, 720x520 | 1.0 | Sheet unchanged; sidebar labels visible through the scrim, no overlap | none observed | PASS |
| 02_empty | First start (empty library opens the first-run sheet over the Add game page) | 1080x740, 720x520 | 1.0 | The reference's dedicated empty Library state is not implemented here (T003); Add game page content unchanged (its Sega CD ghost button overlaps the drop zone's dashed edge at 720x520, pre-existing) | P1 (pre-existing, T003 owns the page) | Open |
| any | Library with the software renderer at very large sizes (for example 2000x1000, or 1600x900 with a taller banner) | >= 2000x1000 | 1.0 | The banner quad renders as a diagonal wedge under the dummy/software SDL renderer; not seen at <= 1600x900 after the 150 px cap; GPU renderer not checked | P2 | Observed software-renderer artifact, open |

Not verified: HiDPI/fractional scale, real GPU renderer, keyboard/controller focus ring visuals beyond the journey suites, large UI size (compact factor only).
