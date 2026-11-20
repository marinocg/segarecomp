# Launcher UX inventory (baseline for the UX overhaul)

Purpose: every action, persistent setting, status, modal and input path of the launcher as of this commit, mapped to the target screen of the UX overhaul, so that no capability is orphaned by a later redesign task. It is derived from the sources under `apps/segarecomp-launcher/` and from real captures of the native launcher against a synthetic fixture (`tools/launcher_ui_fixture.py`); it is not a compatibility statement. When a later task moves a control, it updates the "Target" column here instead of dropping the row.

Target names: **Library**, **Add game**, **Game > Overview / Mods / Game settings / Advanced**, **Settings > General / Appearance & Controls / Storage & Data / Advanced**. A target may only be changed by the task that owns it (see the milestone's task list).

## Pages today

| Page (`Page` enum) | Contents | Target |
| --- | --- | --- |
| `library` | cover grid or list, search (Ctrl+F, Cmd+F on macOS), platform/status filter, sort, favorites, Select mode with bulk actions, context menu, Add game button (the sidebar's primary Add game opens the Add game page), free-space and build-slot footer. T003: cards and rows carry cover, name, platform, ONE state (symbol + color + text) and ONE main action (Play, Prepare & play, Prepare, Retry, Choose mapper, Find game file, Cancel while preparing, Unqueue while queued, Details); an empty library shows the Add game empty state, a fresh profile the skippable welcome | Library |
| `details` | T004: identity header (cover, title edit, favorite, platform, ONE state in the library's words, a sentence of what happens next, ONE primary action per state, More) above four tabs (`DetailsTab`, ordinary view state, stable focus ids `tab_overview` / `tab_mods` / `tab_settings` / `tab_advanced`): **Overview** (notices, the failure card with reason, next step, View log; rows into the other tabs, "Advanced details"), **Mods** (T006: No mod (original game) and each installed mod as a row with typed status, install / select / prepare / play / remove; an honest unavailable card for non-Genesis games), **Game settings** (preparation options, input recording, display and audio pointers, library entry: favorite, cover, remove), **Advanced** (build provenance and identity, older versions, build log, game file row, storage card, diagnostics) | Game (done in T004) |
| `import_page` | (T002: renamed Add game in the UI; opened by the sidebar's primary Add game button) file picker, drag and drop, accepted file types (T005: a Sega CD disc's .cue is one of them and the separate Sega CD form is gone: the disc verdict and the BIOS step are rows of the review). T003: inspected files wait in a review (valid / needs a choice / already in library / cannot be added) and nothing is added until Add & prepare or Add only; the former "Also build each newly added game" checkbox is replaced by that explicit choice (it was never persisted) | Add game |
| `storage` | per-game and total build storage by category, cleanup plans with confirmation | Settings > Storage & Data (T002: no longer a sidebar entry; T007: opened by "Review usage and clean up" in the Storage & Data group, titled STORAGE & DATA, the old header Storage button and the Build & Runtime "Manage Storage" are gone; Back / Escape / B return to the Storage & Data group with focus on that button, or to the Library when a library action opened it) |
| `settings` | T007: four groups (`settings_tab` 0-3, ordinary view state, focus ids `tab#N`, a drop-down `settings_section` below 760 px): **General**, **Appearance & Controls**, **Storage & Data**, **Advanced** (the former six tabs: Library; Metadata & Covers; Build & Runtime; Video & Controls; Input Diagnostics; Advanced / About; every control is mapped in "Settings groups" below) | Settings (done in T007) |

## Actions

| Area | Action | Source | Target |
| --- | --- | --- | --- |
| Library | open game, toggle favorite, select several, bulk build / remove / clean | `app_screens.cpp` | Library |
| Library | list/grid view, sort (Last played, Title, Last built, Added, Size), compact mode | `app_screens.cpp`, `launcher_settings.hpp` | Library, Settings > General |
| Add | pick files (the extensions the code accepts: md, bin, gen, smd, sms, zip), drop files, controller file picker | `app.cpp`, `app_picker.cpp`, `file_picker.*` | Add game |
| Add | review, then Add & prepare (queues the new, fully determined games; no launch) or Add only; Cancel discards the review; a duplicate is never overwritten and a Master System game without a declared mapper is added but never prepared | `app_pages.cpp`, `app.cpp` | Add game (done in T003) |
| Library | main action per game: Prepare & play (explicit; launches once from the revalidated build it made, never after a failed, cancelled or unqueued preparation), Cancel / Unqueue while preparing or queued | `app_screens.cpp`, `app.cpp` | Library (done in T003) |
| Add | Sega CD: CUE + tracks (inspected in the Add game review: tracks found / missing by name, typed unsupported layouts, cancellable), user BIOS (own file; remembered as a local profile `sega-cd-bios.json` by verified SHA-256 + region + location, revalidated at every use, Locate / Choose another / explicit choice when several), Add & prepare / Add only | `app_sega_cd.cpp`, `sega_cd.*`, `app_pages.cpp` | Add game (done in T005; the former Add Sega CD game... form and its three buttons are removed) |
| Game | Sega CD: attach a prebuilt program (Import / Replace), Verify again, cancel a running check | `app_sega_cd.cpp`, `app_details.cpp` | Game > Advanced (Program rows: Import / Replace / Verify; also the More menu and the Game settings expander) (done in T005) |
| Game | Play (verified build only), Prepare & play / Prepare / Prepare again / Retry / Find game file / Choose mapper / Cancel / Unqueue: one primary action per state (`details_primary_label`), the library's words | `app_details.cpp`, `launcher_core.*` | Game > Overview (done in T004) |
| Game | Mode (Compatibility / Optimized / Sega CD modes where declared), console region, SMS mapper, cartridge hardware (Automatic / Standard / Lock-on), H-int discovery and the other "Advanced build options" (an expander) | `app_details.cpp` | Game > Game settings (done in T004: the controls and their capability gating are unchanged; the card is titled "Preparation - this game") |
| Game | older playable builds, build identity, provenance, build log panel (View build log, live while preparing, also from the failure card) | `app_details.cpp` | Game > Advanced (done in T004) |
| Game | patches and variants (schema 1/2/3 install, approve, select, prepare, play, uninstall): choose package -> verified preview sheet (identity, version, author, target, kind, capabilities, exact identity) before consent -> identity-bound approval where code is carried (Cancel focused, not sandboxed, host-process permissions) -> installed; ONE selected variant (No mod (original game) always one click; composition is not implemented); typed row status Installed / Selected - Needs preparation / Selected - Prepared / Needs approval / Incompatible / preparation failed; refusals keep their typed message plus a next step; Mods for a non-Genesis game says plainly that mods are not available | `app_variants.cpp`, `app_details.cpp`, `patch_library.*` | Game > Mods (done in T006) |
| Game | input recording and replay options (the Record switch is launcher-wide and not saved; the recordings listed are the game's) | `app_details.cpp` | Game > Game settings (done in T004) |
| Game | change cover (user-chosen image) and remove cover: the cover's corner button, the More menu and a labeled button on Game settings; automatic cover fetching is not packaged (the provider opt-in toggle is shown disabled); "Find cover online" only opens the browser | `covers.*`, `cover_download.*` | Game > Overview header, Game > Game settings (done in T004) |
| Game | Sega CD: Locate BIOS... and Locate disc... (relink moved BIOS/CUE with the same SHA-256, context menu, the Overview's primary button and the Advanced file rows; T005 also offers Locate for a changed BIOS / CUE, so the original file elsewhere can be pointed to) | `app_details.cpp`, `app_picker.cpp` | Game > Overview / Advanced; Add game review (Locate... on a remembered BIOS) (done in T005) |
| Game | Remove from build queue (queued game: Unqueue), Cancel / Stopping (running game); also in the details menu | `app_details.cpp` | Game > Overview (done in T004) |
| Game | context menu: Find cover online (opens the browser, nothing uploaded), Copy ROM SHA-256 / Copy game identity | `app_details.cpp` | Game > Overview |
| Game | remove from library (Game settings, More menu), clean intermediates (Advanced storage card); a game whose ROM moved is shown as "Game file missing" and offers Find game file (relink a moved file with the same SHA-256; context menu, the one primary button when no playable version exists, a banner button when one does, and the Locate button on the Advanced "ROM file" row) | `app_details.cpp`, `app_screens.cpp`, `storage.*` | Game > Overview / Game settings / Advanced (done in T004) |
| Storage | per-game and bulk cleanup plans, free-space, cache folder | `storage.*`, `app_pages.cpp` | Settings > Storage & Data (done in T007: usage summary and effective paths in the group, plans on the cleanup page it opens) |

## Persistent settings (`launcher_settings.hpp`)

view (grid/list), sort, compact, keep build logs on cleanup, keep old build artifacts, adopt cache builds at start, preferred regions, cover provider opt-in (reserved, shown disabled), first-run / intermediates-choice flag, start fullscreen, FPS counter, default video/control values shared with the viewer. The UI scale is derived from the display scale and pixel density (plus compact mode), not stored. Environment overrides: `SEGARECOMP_CACHE_DIR`, `SEGARECOMP_LIBRARY_DIR`, `SEGARECOMP_HOME`. T007: the settings file format is unchanged (no field added; an old file loads with its exact values and is not rewritten by looking at Settings; `launcher_ui_support_test`, `launcher_settings_groups_gui_test`). No automatic-preparation setting was added: Add & prepare / Add only stays an explicit choice in every Add game review.

## Settings groups (T007)

Every control of the former six tabs, where it is now. Nothing was removed; only real persisted or real actions are shown (the cover provider opt-in is reserved and has no control).

| Former tab / control | Settings group > section |
| --- | --- |
| Library: view (Grid / List), default sort, compact mode, adopt cache builds at start, ROM links count and Re-check | General > Library (compact mode is also offered under Appearance & Controls > Interface: one setting, two places) |
| Build & Runtime: Keep old build artifacts, After a successful build (Never / Remove intermediates / Keep last N / Soft limit and ask, N and limit steppers, policy sentence) | General > Preparation (with the sentence that Add & prepare / Add only is chosen per import) |
| Metadata & Covers: online lookup, Download missing covers, provider text, preferred regions (Rotate order), offline note, Open covers folder | General > Metadata & Covers |
| Library: Restore launcher defaults, Clear library (their confirmations) | General > Reset |
| Video & Controls: start fullscreen, scaling, aspect, widescreen mode / width / aspect / side presentation, display preset, VSync, FPS counter | Appearance & Controls > Window & Picture |
| Video & Controls: volume, mute, Restore video and audio defaults | Appearance & Controls > Audio |
| Video & Controls: keyboard and gamepad notes (player 2 keys, Pause key) | Appearance & Controls > Audio note and Keyboard & Gamepad |
| Input Diagnostics (status, Rescan, Copy report, menu test, SDL, Switch 2, controllers, OS HID scan, mapping file) | Appearance & Controls > Input diagnostics (last section; every control keeps its id) |
| Build & Runtime: toolchain text, Master System Optimized availability | Advanced > Build & Runtime |
| About: version, toolchain, SDL, host; Advanced: installation, viewer settings file | Advanced > About |
| About: library schema, notice, Adopt existing builds now, error / network note | Advanced > Diagnostics (logs: where they are) |
| About: Where things live (installation, cache, library) and Open cache / library folder | Storage & Data > Where things live: effective paths, read-only, with the variable that governs each (SEGARECOMP_CACHE_DIR, SEGARECOMP_LIBRARY_DIR, SEGARECOMP_HOME); relocation (T008/T009) attaches here and has no control yet |
| Storage page summary tiles | Storage & Data > Disk usage (bytes only once measured) |
| Storage page plans, selection, confirmation sheet | Storage & Data > Clean up > Review usage and clean up (the sheet lists the protected data) |

## Statuses (must stay distinct)

Catalog `GameStatus` values: not built, ready, metadata only (ROM moved), stale, failed, playable older build. T004: the library and the Game page show the same user words (`badge_for`: Preparing, Queued, Not prepared, Ready, Out of date, Failed, Older version, Game file missing, Needs a mapper choice, Metadata only, Running); the technical labels of `status_label` (Not built, Needs rebuild, Build failed, Playable (older build)) stay what the script key `expect status=` reports. Derived runtime states shown on top of them: queued, building (running), stopping, cancelled, missing tool/package, unsupported selection (for example an undeclared SMS mapper), and the Sega CD refusals (BIOS or disc missing).

## Modals and overlays

first-run "Keep build intermediates?" (T003: raised when the user first prepares a game, not at start-up; Esc cancels that preparation request and records nothing), cleanup confirmation, delete confirmation, context menu, patch approval sheet (host-process permissions), toast, controller picker, build log panel. Each keeps Esc / B as cancel.

## Input paths

mouse; keyboard (Tab/arrows, Enter, Esc, Ctrl+F or Cmd+F, scripted via `SEGARECOMP_GUI_SCRIPT`); gamepad zones (`app_input.cpp`: D-pad/stick navigation, A confirm, B back, West/Y context). Existing journey suites: `launcher_gamepad_journey_*`, `launcher_gui_navigation_test`, `launcher_parity_gui_*`.
