# Launcher UX inventory (baseline for the UX overhaul)

Purpose: every action, persistent setting, status, modal and input path of the launcher as of this commit, mapped to the target screen of the UX overhaul, so that no capability is orphaned by a later redesign task. It is derived from the sources under `apps/segarecomp-launcher/` and from real captures of the native launcher against a synthetic fixture (`tools/launcher_ui_fixture.py`); it is not a compatibility statement. When a later task moves a control, it updates the "Target" column here instead of dropping the row.

Target names: **Library**, **Add game**, **Game > Overview / Mods / Game settings / Advanced**, **Settings > General / Appearance & Controls / Storage & Data / Advanced**. A target may only be changed by the task that owns it (see the milestone's task list).

## Pages today

| Page (`Page` enum) | Contents | Target |
| --- | --- | --- |
| `library` | cover grid or list, search (Ctrl+F, Cmd+F on macOS), platform/status filter, sort, favorites, Select mode with bulk actions, context menu, Add game button (the sidebar's primary Add game opens the Add game page), free-space and build-slot footer. T003: cards and rows carry cover, name, platform, ONE state (symbol + color + text) and ONE main action (Play, Prepare & play, Prepare, Retry, Choose mapper, Find game file, Cancel while preparing, Unqueue while queued, Details); an empty library shows the Add game empty state, a fresh profile the skippable welcome | Library |
| `details` | cover and metadata editing, favorite, Play/Build/More, status line, Build configuration, Patches / Variants, Build provenance, Build log, Input recording, per-game storage card | Game |
| `import_page` | (T002: renamed Add game in the UI; opened by the sidebar's primary Add game button) file picker, drag and drop, accepted file types, Sega CD form (CUE, BIOS, prebuilt program). T003: inspected files wait in a review (valid / needs a choice / already in library / cannot be added) and nothing is added until Add & prepare or Add only; the former "Also build each newly added game" checkbox is replaced by that explicit choice (it was never persisted) | Add game |
| `storage` | per-game and total build storage by category, cleanup plans with confirmation | Settings > Storage & Data (T002: no longer a sidebar entry; the Storage button in the Settings header and "Manage Storage" in Build & Runtime open it; Back / Escape / B return to Settings, or to the Library when a library action opened it) |
| `settings` | six tabs: Library; Metadata & Covers; Build & Runtime; Video & Controls; Input Diagnostics; Advanced / About | Settings (four groups) |

## Actions

| Area | Action | Source | Target |
| --- | --- | --- | --- |
| Library | open game, toggle favorite, select several, bulk build / remove / clean | `app_screens.cpp` | Library |
| Library | list/grid view, sort (Last played, Title, Last built, Added, Size), compact mode | `app_screens.cpp`, `launcher_settings.hpp` | Library, Settings > General |
| Add | pick files (the extensions the code accepts: md, bin, gen, smd, sms, zip), drop files, controller file picker | `app.cpp`, `app_picker.cpp`, `file_picker.*` | Add game |
| Add | review, then Add & prepare (queues the new, fully determined games; no launch) or Add only; Cancel discards the review; a duplicate is never overwritten and a Master System game without a declared mapper is added but never prepared | `app_pages.cpp`, `app.cpp` | Add game (done in T003) |
| Library | main action per game: Prepare & play (explicit; launches once from the revalidated build it made, never after a failed, cancelled or unqueued preparation), Cancel / Unqueue while preparing or queued | `app_screens.cpp`, `app.cpp` | Library (done in T003) |
| Add | Sega CD: CUE + tracks, user BIOS, attach prebuilt program | `app_sega_cd.cpp` | Add game, Game > Advanced |
| Game | Play (verified build only), Build / Rebuild / Cancel build | `app_details.cpp`, `launcher_core.*` | Game > Overview |
| Game | Build mode (Compatibility / Optimized / Sega CD modes where declared), model, console region, SMS mapper, lock-on, H-int discovery and other advanced build options | `app_details.cpp` | Game > Game settings / Advanced |
| Game | older playable builds, build identity, provenance, build log panel | `app_details.cpp` | Game > Advanced |
| Game | patches and variants (schema 1/2/3 install, approve, select, uninstall) | `app_variants.cpp` | Game > Mods |
| Game | input recording and replay options | `app_details.cpp` | Game > Game settings |
| Game | change cover (user-chosen image); automatic cover fetching is not packaged (the provider opt-in toggle is shown disabled); "Find cover online" only opens the browser | `covers.*`, `cover_download.*` | Game > Overview |
| Game | Sega CD: Locate BIOS... and Locate disc... (relink moved BIOS/CUE with the same SHA-256, context menu and details file rows) | `app_details.cpp`, `app_picker.cpp` | Game > Advanced, Add game |
| Game | Remove from build queue (queued game), Cancel build / Stopping build (running game); also in the details menu | `app_details.cpp` | Game > Overview |
| Game | context menu: Find cover online (opens the browser, nothing uploaded), Copy ROM SHA-256 / Copy game identity | `app_details.cpp` | Game > Overview |
| Game | remove from library, clean intermediates; a game whose ROM moved is shown as metadata only and offers Locate ROM... (relink a moved file with the same SHA-256; context menu, primary button when the ROM is missing, and the Locate button on the details "ROM file" row) | `app_details.cpp`, `app_screens.cpp`, `storage.*` | Game > Game settings / Advanced |
| Storage | per-game and bulk cleanup plans, free-space, cache folder | `storage.*`, `app_pages.cpp` | Settings > Storage & Data |

## Persistent settings (`launcher_settings.hpp`)

view (grid/list), sort, compact, keep build logs on cleanup, keep old build artifacts, adopt cache builds at start, preferred regions, cover provider opt-in (reserved, shown disabled), first-run / intermediates-choice flag, start fullscreen, FPS counter, default video/control values shared with the viewer. The UI scale is derived from the display scale and pixel density (plus compact mode), not stored. Environment overrides: `SEGARECOMP_CACHE_DIR`, `SEGARECOMP_LIBRARY_DIR`, `SEGARECOMP_HOME`. Target: Settings groups; the settings file format and migration are not changed by this task.

## Statuses (must stay distinct)

Catalog `GameStatus` values: not built, ready, metadata only (ROM moved), stale, failed, playable older build. Derived runtime states shown on top of them: queued, building (running), stopping, cancelled, missing tool/package, unsupported selection (for example an undeclared SMS mapper), and the Sega CD refusals (BIOS or disc missing).

## Modals and overlays

first-run "Keep build intermediates?" (T003: raised when the user first prepares a game, not at start-up; Esc cancels that preparation request and records nothing), cleanup confirmation, delete confirmation, context menu, patch approval sheet (host-process permissions), toast, controller picker, build log panel. Each keeps Esc / B as cancel.

## Input paths

mouse; keyboard (Tab/arrows, Enter, Esc, Ctrl+F or Cmd+F, scripted via `SEGARECOMP_GUI_SCRIPT`); gamepad zones (`app_input.cpp`: D-pad/stick navigation, A confirm, B back, West/Y context). Existing journey suites: `launcher_gamepad_journey_*`, `launcher_gui_navigation_test`, `launcher_parity_gui_*`.
