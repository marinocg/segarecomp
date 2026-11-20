# Launcher UX overhaul: acceptance matrix (SEG-150-T010)

This is the milestone's acceptance record, filled from observed evidence only. Every row of the operator's acceptance matrix has a result, the exact evidence and, where a host or a check was not available,
the words **NOT VERIFIED** or **BLOCKED** with the reason. An untested or unavailable host is never reported as PASS. The per-screen comparison, the findings log and the usability walk-throughs are in
[launcher-ux-review.md](launcher-ux-review.md) (section "Acceptance (T010)"); the feature inventory that guarded against orphaned capabilities is [launcher-ux-inventory.md](launcher-ux-inventory.md).

- **Validated tree:** branch `task/seg-150-t001` at `18d3bd3` (T001-T009) plus the T010 working-tree changes listed in the review record (the supervisor records the final commit SHA; a changed HEAD invalidates this evidence).
- **Host:** Linux x86_64 on WSL2 (Ubuntu 24.04, glibc 2.39), SDL 3.4.16 (a local build, not the release-pinned one), Dear ImGui 1.92.9b, SDL_Renderer. Automated captures use SDL's `dummy` video driver with the software
  renderer (a layout test, not a GPU or physical-display result) at density 1.0 and UI scale 1.0. Real windows (WSLg Wayland and X11, density 1.0) were captured once at 1080x740.
- **Fixtures:** synthetic only: project-authored 512-byte Genesis headers, a generated Master System image, a 4 MiB generated lock-on-shaped image (headers only), a project-authored Sega CD fixture disc
  (`tools/sega_cd_fixture_disc.py`), BIOS doubles of a few hundred bytes, packages made by the real `segarecomp mod pack` from the repository's scenery fixture, and the product and game doubles
  (`fake_segarecomp`, `fake_game`, `fake_scd_program`). No commercial image, BIOS, cover or trace is involved anywhere.
- **No human user study was run.** Every walk-through below was executed by an agent driving the real window with the GUI script seam; nothing here is a measurement of human effort, error rate or satisfaction, and no
  timing is claimed.

## Rendering contract (from T001, unchanged by T010)

- Text is rasterized by Dear ImGui 1.92's dynamic font system from real Inter Regular and Inter SemiBold faces (`AddFontFromFileTTF`; SIL OFL 1.1, release 4.1) plus Silkscreen Bold for headings. There is no faux bold and
  no whole-window intermediate texture.
- The SDL3 backend reports `io.DisplayFramebufferScale` = the window's pixel density every frame; ImGui derives the glyph raster density from it (`FontRasterizerDensity`), so a Retina or 200 % window rasterizes glyphs at
  2x and draws them at logical size (texels map 1:1 to pixels). `FontScaleDpi` stays 1.0 on purpose: text is sized in explicit logical pixels (`ctx.u()`) and must not be scaled twice.
- After `ImGui::Render` the renderer scale is set to output size / window size, so logical units map to physical pixels; the SDL_Renderer backend then does not scale clip rectangles a second time.
- Layout scale `S` = OS display scale / pixel density (Windows fractional scaling: density 1, `S` = scale; macOS Retina: density 2, `S` = 1) times the compact factor.
- A missing font file falls back to ImGui's embedded face (regular) and to the regular face (bold): proven in T010 on an installed package copy with both Inter files removed (layout intact, thinner glyphs; `screenshots/launcher-ux/t010-font-fallback-1to1.png`).
- `SEGARECOMP_SHOT_SCALE` is a logical layout aid, not physical HiDPI evidence.

## DPI evidence

| OS / video driver / renderer | Native or simulated | Window pts | Output px | OS scale / density | UI scale | Font raster | Result |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Linux WSL2 / SDL dummy / software | simulated headless | 1080x740, 720x520, 1600x900 | same | none / 1.0 | 1.0 | Inter Regular + SemiBold, Silkscreen Bold, density 1.0 | PASS (layout and text) |
| Linux WSL2 / WSLg Wayland / SDL_Renderer | real window, re-run in T010 | 1080x740 | 1080x740 | 1.0 / 1.0 | 1.0 | same | PASS at density 1.0: crisp text at 1:1 (`screenshots/launcher-ux/t010-real-window-1to1.png`, top) |
| Linux WSL2 / WSLg X11 / SDL_Renderer | real window, re-run in T010 | 1080x740 | 1080x740 | 1.0 / 1.0 | 1.0 | same | PASS at density 1.0 (same crop, bottom) |
| Any host, `SEGARECOMP_SHOT_SCALE=2` | logical layout aid | 1080x740 | 2160x1480 | density stays 1.0 | 2.0 | density 1.0 | NOT physical HiDPI evidence |
| Windows 100 / 125 / 150 / 200 % | not available | n/a | n/a | n/a | n/a | n/a | NOT VERIFIED |
| macOS Retina (density 2) | not available | n/a | n/a | n/a | n/a | n/a | NOT VERIFIED |
| Linux native fractional / 2x display, mixed-DPI monitor switch | not available (WSLg reports 1.0) | n/a | n/a | n/a | n/a | n/a | NOT VERIFIED |

Cross-platform crispness at high density is therefore **not claimed**: the code path is the SDL3 backend's supported mechanism and is expected to hold, but it needs the Windows and macOS hosts above.

## Acceptance matrix

Commands (focused suites; counts in "Tests run" below): `ctest -R "launcher|storage_test|library_catalog|data_location|file_picker|older_build|patch|schema3|mod|cartridge|packag"` in `build/full` and `build/launcher` (counts below), the parity
suites with `SEGARECOMP_PARITY_SHOTS`, `launcher_mods_gui_test.py` with `MODS_SHOTS`, and `tools/launcher_acceptance_capture.py` for the remaining captures.

| ID | Result | Evidence (observed) |
| --- | --- | --- |
| UX01 | PASS (GUI and doubles); packaged smoke PARTIAL, see PACK01 | `launcher_import_review_gui_test` (Add only; Add & prepare then verified Play from the build it made; Prepare & play launches exactly once; failed preparation never launches), `launcher_first_run_test` (welcome, first-run question, **new** keyboard-only case), the keyboard-only fresh-profile walk-through (24 key presses from the welcome page to a verified Play and back, review record "Usability walk-throughs"), `launcher_gamepad_journey_*` (first run, import via the built-in picker, build). The real compiler path ran in the packaged smoke (PACK01) |
| UX02 | PASS | `launcher_import_review_gui_test` cases 3-5 (valid + duplicate + invalid + needs-choice batch: invalid and duplicate unselectable, needs-choice added but never prepared, existing entry untouched; decline adds nothing; cancel of a queued and of a running preparation leaves no `.partial`/`.failed`, no launch), `launcher_build_cancel_gui_test`, `launcher_build_cancel_test`, `launcher_gamepad_journey_*` build journey; native captures `06_preparing_queued_library`, `06_preparing_details`, `05_review_mixed` |
| UX03 | PASS | `launcher_older_build_test`, `older_build_test`, `launcher_gamepad_journey_*` older-build journey, `launcher_details_tabs_gui_test` (failed, missing ROM), `launcher_parity_gui_*`; native captures: Ready, Older version, Not prepared, Failed (reason, Retry), Game file missing (Find game file), Needs a mapper choice, Preparing, Queued, lock-on, BIOS changed |
| UX04 | PASS | `launcher_parity_gui_journey_*` and `_persist_*` (Genesis, Master System, Sega CD through one workflow at 1080x740 and 720x520, 238 checks at each of the sizes run in T010: 1080x740, 720x520, 1600x900), `launcher_sms_policy_test`, `launcher_lock_on_{headless,window,library,patch,gamepad}_test`, `launcher_policy_test` (refusals), native captures of the mapper choice (required, never guessed), lock-on / standard / automatic, mode tiles |
| UX05 | PASS (synthetic) | `launcher_sega_cd_review_gui_test`, `launcher_sega_cd_gui_test`, `launcher_sega_cd_flow_test`, `launcher_sega_cd_save_test`, `launcher_sega_cd_test` (complete and missing tracks, missing / mismatched / moved / changed BIOS, reuse only of a verified matching BIOS, cancel, attach, reverify, backup RAM and recording preservation); native captures `10_segaCD_review_needs_bios`, `11_bios_chosen_review`, `11_bios_disc_missing_track_review`, `12_segaReady_*`, `scd_bios_changed_*`. A real disc or BIOS was never used |
| UX06 | PASS | `launcher_patch_gui_test`, `launcher_schema3_gui_test`, `launcher_schema3_flow_test`, `launcher_mods_gui_test` (decline, wrong revision/platform, tamper, approval bound to the exact identity, selected variant needs preparation, Vanilla unchanged, uninstall, single selection, controller decline/approve), `launcher_mod_variant_test`; native captures from the mods suite (approval sheet, refusal, incompatible, switched, prepared) |
| UX07 | PASS | the inventory walk (`launcher-ux-inventory.md`): 60 control labels sampled from the inventory were located in the code (59 literally; "Load mapping" is "Reload mapping file"), every Settings group reachable (`launcher_settings_groups_gui_test`, native captures of all four groups), `launcher_ui_support_test` (settings upgrade, no reset), `launcher_input_diagnostics_test`, `launcher_default_mode_test`; no orphaned capability found |
| UX08 | PASS | `launcher_data_location_gui_test`, `launcher_data_location_test`, `data_location_test` (success, restart, old root retained, external ROM references unchanged, cache not reused unverified); native captures `21_relocate_*`, `22_migrating_*` |
| UX09 | PASS | the same suites' failure injection: destination not empty / inside the source / insufficient space, environment override, busy in both directions, cancel during copy, closing the window mid-copy, interrupted copy with recovery at restart; old root unchanged; native captures of every refusal, `22_migrating_cancelled`, `22_migrating_interrupted`, `22_migrating_recovery_notice_after_restart` |
| UX10 | PASS | `storage_test` (known-byte accounting, only eligible generated files removed; saves, originals, recordings and approvals protected), `launcher_data_location_gui_test` (previous-location cleanup: regenerable only by default, unchanged migrated copies only on request); native captures `23_cleanup*` |
| UI01 | PASS with recorded intentional deltas | the consolidated per-screen table in the review record: every reference 01-25 and every additional required state captured natively at 1080x740, 720x520 and 1600x900 and opened; no clipping of a critical control, no prototype switcher, no baked text left after the fixes of this task. References with no native equivalent are named product-truth deviations (per-game widescreen controls, mod drop page, mod combination page). Reviewed by the implementing agent; **no independent image-aware review yet** |
| UI02 | PARTIAL: PASS at density 1.0; NOT VERIFIED at 125/150/200 % and Retina | unresized native screenshots and 1:1 crops (dummy driver and two real WSLg windows); see the DPI table |
| UI03 | BLOCKED (no mixed-DPI or fractional-scale host). Resize, long names, non-ASCII, compact mode: PASS at density 1.0 | window resize across the three sizes in the captures; the long non-ASCII title (a P1 fixed in T010); hit testing follows the logical layout. Monitor switch, fractional scale: NOT VERIFIED |
| UI04 | PASS (high contrast and image-asset fallback NOT VERIFIED) | 720x520, 1080x740 and 1600x900 (the 1440x900-class size) for every state; large library (14 games, scrolled), long and non-ASCII labels, no-cover placeholders, missing fonts (installed-copy test), read-only/unwritable profile, missing installation, failure states. The launcher has no high-contrast mode (N/A); the secondary-text contrast was measured and raised (review record F4); a missing **image** asset (background, icons) was not exercised in T010 |
| UI05 | PASS (physical controller NOT VERIFIED) | `launcher_gui_navigation_test` (keyboard, mouse), `launcher_gamepad_journey_*` (virtual gamepad: first run, import via picker, library, build/cancel/queue, older build, settings, storage, hot-plug), `launcher_mods_gui_test` (controller declines a native mod), `launcher_data_location_gui_test` (gamepad Cancel move), the keyboard-only walk-through. SDL virtual joysticks only |
| UI06 | PASS | real phases and typed outcomes, no percentage where none is known: `launcher_build_cancel_*`, `launcher_import_review_gui_test` (queue, cancel), data move phases (`22_migrating_progress`: Check, Copy, Library, Verify, Switch with Cancel move), typed failures (`09_failed_*`, refusals, `error_*`), no spinner without a state; restart recovery tested |
| ARCH01 | PASS | SDL3 + Dear ImGui retained, shared `ui_kit` helpers; no new toolkit or service. Against the milestone base `44ab620` the diff touches only `apps/segarecomp-launcher`, product docs, `README.md`, the two capture tools under `tools/`, packaging install / notice / README files and one registered packaging test; `libs/`, `platforms/` and `apps/segarecomp` (the CLI) are unchanged, no Studio placeholder |
| ARCH02 | PASS for the tests; independent adversarial review pending | policy, trust and ownership paths are exercised by `launcher_policy_test`, `launcher_schema3_flow_test`, `storage_test`, `data_location_test`; T010 touches none of them (text cut, focus, one color token, a picker label, scroll flags, an asset manifest) |
| PACK01 | PARTIAL: manifest, licences, install tree, installed-run and font fallback PASS; unmodified clean-archive smoke BLOCKED on this host | see "Packaging" below |
| AUD01 | PARTIAL | named issues and resolutions with severities in the review record, walk-throughs for novice, returning, keyboard, gamepad and advanced routes (agent-executed, no human study), screenshot comparison; **independent image-aware reviewer: not yet**, planned in the validator phase |

## Packaging (PACK01)

- **Assets manifest:** `apps/segarecomp-launcher/assets/manifest.json` omitted two shipped assets (`banner.png`, `icons/Segarecomp-*.png`); fixed, and `packaging_launcher_assets_test` now asserts that every file under
  `assets/` is described, every manifest entry matches a file (counts exact), every asset the launcher code names exists, every bundled font has its licence beside it, and `packaging/install.cmake` installs the
  fonts, both OFL licences and `THIRD-PARTY-NOTICES.txt` and excludes only documentation (verified to fail on the previous manifest).
- **Real install:** `cmake --install` of a Release build with the pinned Zig 0.15.2 (a copy trimmed with `packaging/trim-zig.sh linux`) and an SDL3 prefix into a scratch prefix, archived like `release.yml` does: the tree
  holds `assets/fonts/{Inter-Regular,Inter-SemiBold,Silkscreen-Bold,Silkscreen-Regular}.ttf`, `assets/{background,banner,wordmark,icon-*}.png`, `assets/ui/` (28 icons), `assets/covers/` (4), `assets/icons/` (6) and
  `licenses/{Inter-OFL,Silkscreen-OFL,THIRD-PARTY-NOTICES,MPL-2.0,SDL3,Zig,DearImGui,Luau,Lua,zlib,ymfm-BSD-3-Clause}`; `README.md`, `manifest.json` and the OFL copies under `assets/fonts/` are not duplicated into `assets/`.
- **`packaging/dependency_check.py`** on the installed tree: `dependency check: OK` (5491 files; native dependencies are packaged or operating-system libraries; the frozen models are embedded).
- **Installed run:** the installed launcher (cwd outside the repository, `LD_LIBRARY_PATH` unset, so the packaged `sdl3/lib/libSDL3.so.0` is the one loaded through `$ORIGIN/sdl3/lib`) drew the library from `assets/`;
  the documentation screenshots were captured from it. In a scratch **copy** with both Inter files removed it started, drew the same layout with ImGui's embedded face, and exited 0 (font fallback exercised).
- **Clean-archive smoke (`packaging/smoke_test.py`), unmodified: BLOCKED on this host.** It fails twice, both times on a mismatch between this host's SDL and the release-pinned one: with the release's glibc 2.35 target the
  generated game cannot link (the local SDL 3.4.16 was built on glibc 2.39 and needs `@GLIBC_2.38` symbols), and with a 2.39 target the check that the packaged SDL declares the Switch 2 USB driver fails (the local SDL
  has no libusb support compiled in; `packaging/sdl_check.py driver` reports the literals absent). CI's `release.yml` builds the pinned SDL (`packaging/build-sdl3.sh`, Ubuntu 22.04) and runs the unmodified smoke test per OS;
  local equality with it is not claimed.
- **Clean-archive smoke, relaxed copy: PASS.** With the package built for the host's glibc (`-DSEGARECOMP_ZIG_GLIBC=2.39`) and a scratch copy of the smoke script that only relaxes the two Switch 2 driver literals,
  the extracted archive built and ran the synthetic Genesis cartridge with the bundled compiler (scrubbed `PATH`, no cc/zig/python on it), then the Master System and Optimized stages, and printed
  `package smoke test: OK`. This proves the add/prepare/run path of an installed archive without developer tools on this host; it is not the release gate.
- Windows (MSVC runtime, `SDL3.dll`, libusb) and macOS (bundle, Retina) archives: **NOT VERIFIED** locally; covered only by `release.yml` and the manual walk in `packaging/README.md`.

## Not verified (summary)

Windows fractional scale 125/150/200 %; macOS Retina; native HiDPI or mixed-DPI monitor switching; a physical gamepad (SDL virtual joysticks only); the native OS file dialogs under the dummy driver; a real GPU renderer;
Windows and macOS packages and paths; the unmodified clean-archive smoke on a release-pinned SDL; high-contrast mode (none exists); the image-asset fallback; screen readers; an independent image-aware review; any
human usability study.

## Tests run (T010, final code)

All on the final T010 code (the tree at `18d3bd3` plus the T010 working-tree changes), Linux, SDL dummy driver, `SEGARECOMP_NO_COVER_FETCH=1`:

- `build/full`: `ctest -j4 -R "launcher|storage_test|library_catalog|data_location|file_picker|older_build|patch|schema3|mod|cartridge|packag"`: **118 tests, 118 passed, 0 failed** (1215 s). It includes the new `packaging_launcher_assets_test`,
  `launcher_first_run_test` with the new keyboard-after-sheet case and `launcher_ui_support_test` with the new UTF-8 cut cases.
- `build/launcher`: the same expression: **103 tests, 103 passed, 0 failed** (891 s).
- Parity journeys with screenshots at 1080x740, 720x520 and 1600x900: 238 checks each, ok; `launcher_mods_gui_test.py` with `MODS_SHOTS` at the three sizes: ok (these ran on the code just before the last focus and color edits; the suites above re-ran them on the final code).
- Packaging: `packaging/dependency_check.py` on the installed tree: OK; relaxed clean-archive smoke: `package smoke test: OK` (twice, before and after the T010 code edits); unmodified smoke: BLOCKED (above).
- Not run locally: the fast preset, `full` / `extended` (CI-owned), Windows and macOS packaging.
- Before the fix of finding F2 the keyboard trap was observed directly (the traced focus stayed on the vanished review button while Tab was pressed 8 times); the regression case asserts the Tab order after the sheet.

