# Launcher library, storage and the shared in-game viewer

This is the user-facing description of what ships in the consumer launcher (`Segarecomp`) and in every generated game's window. Design rationale:
`docs/decisions/0120-shared-sdl3-viewer-foundation-and-launcher-library.md`.

## What the launcher is

A resizable window (default 1080x740, minimum about 720x520, HiDPI aware, compact mode) whose primary navigation is exactly four areas: **Library**,
**Import ROMs**, **Storage** and **Settings** (including **Input Diagnostics**). A game's **Details** page is contextual, not a sidebar entry: a card, a
list row's Details button or the game's context menu (right click, the Menu key or the gamepad's North button) opens it, and **Back** (the button, Esc or the
gamepad's East button) returns to exactly the library state it came from: search text, platform/status filters, sort, grid/list view, scroll position and the
focused card. The repository's sunset artwork forms the header banner and the dimmed scenery behind the window. Everything it shows is read from the library
file, the build cache or the product: no placeholder values.

* **Library** - grid or list (a game without a cover shows one of four fictional placeholder paintings, picked from its digest, under the platform band and a
  "NO COVER ART" tag; a cover is only ever an image you chose), each card with its build state, platform and last activity, search, platform/status filters, sort (last played, title, last built, added, size), favorites, multi-select
  (Select mode) for storage actions and removal. An empty library shows the ROM drop zone.
* **Import** - drag and drop or Browse (several files, plain ROMs or `.zip` archives). Each file is classified by the same media classifier as before, de-duplicated by SHA-256
  (the same game elsewhere only adds a tracked location), and listed with a reason when refused. Importing never builds (an explicit checkbox builds
  the new games). ROMs are referenced, never copied, moved or uploaded. **Zipped ROMs**: a `.zip` holding exactly one `.md/.bin/.gen/.smd/.sms`
  (stored or deflate, CRC-checked; no zip64 or encryption) is unpacked once into `<library>/roms/<sha256>.<ext>` and that copy is what the library
  tracks and builds; the archive is only read. A zip with no ROM or several ROMs is refused with the reason. The same ROM zipped or not is one game.
  Works for drag and drop, Browse, Locate ROM and the headless `--import` / `--build`. A sidecar `.mapper.json` inside a zip is not read (declare the
  Master System mapper on the game's page).
* **Details** - top to bottom, inside the first 1080x740 view: the cover (change from its corner), editable display name (the header name is kept),
  favorite, platform, build state and ROM link health, one sentence of what matters now, and the two dominant actions **Play** (only for an executable whose
  identity matches the configuration exactly) and **Build / Recompile / Rebuild / Try again** (a missing ROM makes **Locate ROM** the primary action instead).
  Rarer actions are in **More**: build log, open build folder, manage storage, change/remove cover, find cover online, copy SHA-256, locate ROM and (set
  apart) remove from library. The **Build Configuration** card follows: **Build mode** (Compatibility | Optimized; for Genesis an **Experimental model** checkbox appears right under the Optimized tile while
  Optimized is selected) and, for Genesis, the **console region**
  (USA NTSC | Japan NTSC | Europe PAL, applied at launch, never a rebuild; PAL still runs on the 60 Hz schedule), or for Master System the **cartridge
  mapper** (nothing preselected, never guessed; a sidecar manifest declares it) and the one console profile. A state line says what the executable on disk
  actually is (from `status.json`: effective policy and any Optimized fallback), whether the selection matches it, and when a mode switch left the other
  mode's executable on disk ("switch back to play it"). **Advanced build options** (H-interrupt alias discovery) sit behind an expander with their consequences. Below: **Build provenance** (SHA-256 with Copy, ROM file and folder, last recompiled, last
  played and last run result, build version and toolchain, build identity with the selected options, effective policy details, diagnostics) and the build
  log. A **storage** rail shows the game's measured footprint (native executable, generated C, objects, logs, metadata, other build files) with *Clean
  intermediates* and *Manage storage*; for a metadata-only game it says the history is kept and a new build is needed to play.
* **Status** is computed from the build identity, never from a title: *Ready*, *Not built*, *Metadata only* (you removed the build on purpose; "rebuild
  required"), *Needs rebuild* (made by another release or option set, or a build mode whose executable is not built yet; never started), *Build failed*, plus a *ROM missing* badge. Play is offered only for a build
  whose identity matches exactly.
* **Build** shows the real `@stage` messages (analyze, generate, compile, link) and the tail of the build log. There is no percentage because the
  build tool reports none, and no Cancel because killing a build mid-way is not safe (closing the window waits for it).

## Where data lives

| Data | Location | Cleaned by Storage? |
|---|---|---|
| Built games, logs | `<cache>/games/<id>[.partial|.failed]/` | yes (by plan) |
| Shared Zig compiler cache | `<cache>/zig-cache/` | yes (Clear shared compiler cache) |
| Library, covers, ROMs unpacked from zips, launcher + viewer settings, screenshots dir default | `<cache>/library/` (`library.json`, `covers/`, `roms/`, `launcher-settings.json`, `viewer-settings.cfg`) | never |
| Your ROMs | where they are | never |

`<cache>` is `SEGARECOMP_CACHE_DIR`, `%LOCALAPPDATA%\Segarecomp`, or the SDL preference path (`~/Library/Application Support/Segarecomp`, `~/.local/share/Segarecomp`).
`SEGARECOMP_LIBRARY_DIR` overrides the library location. Package install directory (read-only program files) and these per-user folders are
separate; Settings > Advanced shows both. Nothing is deleted at start-up, on import or on Play.

### `library.json` (schema 1)

```json
{"schema": 1, "games": [{
  "platform": "genesis|master-system", "sha256": "<64 hex>", "detected_title": "", "display_title": "",
  "rom_paths": ["<absolute path>"], "rom_path_unknown": false,
  "mapper": "", "mapper_manifest_sha256": "", "aot_policy": "compatibility", "aot_model": "v1", "hint_alias_discovery": false, "region": "usa",
  "cover": "<sha256>.png", "favorite": false, "archived": false,
  "added_at": 0, "last_built_at": 0, "last_played_at": 0, "last_run": "clean|failed|unadmitted_pc|", "last_exit_code": 0,
  "current_build_key": "<32 hex>", "builds": [{"key": "<32 hex>", "built_at": 0, "mode": "compatibility", "version": "", "toolchain": ""}] }]}
```

Written atomically (temporary file, previous copy kept as `.bak`). A damaged file is moved to `.corrupt`, the `.bak` is tried first, and a file written
by a newer schema is shown read-only and never overwritten. Hostile values (traversal in a cover name, bad digests, oversize lists) are dropped on load.

## Storage - exact semantics

Storage measures the cache in the background (bounded, read-only, no link following) and offers these actions. Every one shows a confirmation sheet with
the exact scope, reclaimable bytes, what stays and what cannot be undone.

| Action | Deletes | Keeps |
|---|---|---|
| Remove build intermediates | `generated.c`, `generated/`, `generated-z80/`, `obj/` of the selected games' builds | executable (still playable), `status.json`, `metadata.json`, `build.log`, library data |
| Remove old builds | builds of earlier releases/options of the selected games | their current build |
| Keep metadata only | every build folder of the selected games (executable included) | the library entry, ROM reference, history, cover, settings; the game shows "Rebuild to play" |
| Clean failed / abandoned builds | verified `*.failed` / `*.partial` folders (optionally keeping `build.log` + `status.json`) | every finished build |
| Clear shared compiler cache | the contents of `zig-cache/` | all builds; the next Recompile is slower |
| Remove from library | the library entry (optionally also its build folders) | the ROM, covers |

Never touched: original ROMs, covers, screenshots, `library.json`, unknown folders in the cache (reported, not deleted), a running game or build. Cleanup is
refused while a build or game runs (one busy guard). Settings > Build & Runtime offers an **automatic policy** after a successful build: never,
remove intermediates, keep the newest N playable games (older ones are reduced to metadata only; favorites, running games and the game being built
are excluded) or a soft limit that only reminds you. The default is *never*.

## Covers and metadata provider

Covers are local images you choose (PNG or JPEG, up to 2 MiB and 2048 px, validated by content and fully decoded) copied into `library/covers/<sha256>.<ext>`.
They survive cleanup and re-import. **Segarecomp does not download covers.** The Libretro thumbnail trees (`Sega - Mega Drive - Genesis`,
`Sega - Master System - Mark III`, `Named_Boxarts` at `https://thumbnails.libretro.com/`) are public, but a trusted HTTPS client that works inside the
self-contained Windows, macOS and Linux packages is not available without adding a TLS dependency, and shelling out to `curl`, plain HTTP or embedded
keys are not acceptable. So the supported route is *Change cover*, plus *Find cover online*, which opens your browser at a **name guess**
(`Title (USA)` in your preferred region order, reserved characters replaced as Libretro does); the guess may not exist and is never presented as a
match. ROM bytes and hashes are never sent anywhere. Images belong to their rights holders; none is bundled or committed. ScreenScraper is not used (it
needs developer credentials).

## In-game viewer (Genesis and Master System)

Full-window picture, no permanent UI. **Esc** or the gamepad **Back** button opens the host pause menu (the guest does not advance):

* Resume; **Quick Settings** (fullscreen, scaling, aspect, display preset, VSync, FPS counter); **Video & Display** (plus scanline/mask/curvature/bloom/
  brightness sliders, a live preview that applies the current settings through the real CRT pipeline, and clickable preset buttons); **Controls** (with a **Controller status & test** page: what SDL sees, live buttons, the reason a pad does not work); **Audio** (volume/mute, gain only); **System & Diagnostics** (system,
  ROM digest when started from the launcher, FPS, renderer, picture/output size, audio queue and dropped chunks, screenshot, Reset on Master System);
  **Return to Launcher** (or Quit outside the launcher), with a confirmation.
* Scaling: *Fit* or *Integer* (whole-number multiples, falling back to fit below 1x); aspect *Original* (square pixels) or *4:3 corrected*; windowed
  or borderless fullscreen (never a display-mode change, so there is nothing to revert); HiDPI aware; the Master System's 192/224-line picture keeps its aspect.
* Display presets: **Pixel Perfect** (the untouched nearest-neighbour picture), **CRT Soft** (scanline gaps + glow), **CRT Classic** (adds the RGB
  aperture mask and slight curvature), **CRT Arcade** (stronger everything). Editing a slider makes the preset *Custom*.
* **Controls**: Player 1 and Player 2 (both consoles), keyboard and SDL3 gamepad bindings, gamepad hot-plug, device choice, analog dead zone (the left
  stick doubles as the D-pad), conflict handling (a duplicate binding moves from its previous owner and says so), reserved keys (Esc, F-keys, the menu
  key), a rebindable menu key (Esc always works), restore defaults. The listener waits for a *fresh* press: the press that activated it is ignored.
  Held inputs are released when the window loses focus and when the menu opens or closes.
* Menu button: **Back/Select** (Nintendo "-"), **Home/Guide** or **Esc** opens the menu. **Start** ("+") is a game button (Genesis Start / Master System Pause), so it does not open the menu. Nintendo controllers show their printed labels in the cues (South is the Nintendo B, East the A).
* Hotkeys: F11 / Alt+Enter fullscreen, F12 screenshot (the game picture only, PNG, in `Pictures/Segarecomp` or `SEGARECOMP_SCREENSHOT_DIR`).
* Capability matrix: Genesis is a three-button pad (A/B/C/Start; no X/Y/Z/Mode), two ports, no Reset. Master System has two buttons, the console's
  **Pause** button (an NMI edge, distinct from the host menu) and Reset in the System page. Audio device selection follows the OS default.
* Settings persist in `viewer-settings.cfg` (`SEGARECOMP_VIEWER_SETTINGS`); the launcher's Settings > Video & Controls edits the same file.
* When a guest stops with a typed error the window shows why instead of closing silently (not for the automatic Optimized -> Compatibility restart).

Environment variables the launcher sets for the game: `SEGARECOMP_LAUNCHED`, `SEGARECOMP_VIEWER_SETTINGS`, `SEGARECOMP_ROM_SHA256`, `SEGARECOMP_GAME_TITLE`,
and (Genesis) `SEGARECOMP_GENESIS_REGION`. Unchanged: `SEGARECOMP_VIEWER_UNTHROTTLED`, `SEGARECOMP_VIEWER_SLICE`, `SEGARECOMP_VIEWER_MUTE`, the Master System
`--viewer-*` options including `--viewer-record`.

## Controller input

See `docs/testing/input-diagnostics.md` for the A-F diagnostic states, the Switch 2 Pro Controller findings (SDL's driver is USB-only and needs libusb; Bluetooth is unsupported by SDL 3.4; real-device support is NOT VERIFIED) and the manual validation procedure.

## Headless

`Segarecomp --build <rom> [--mapper ...] [--run ...]` is unchanged. `Segarecomp --import <rom>...` adds ROMs to the library without building. Review aids
(not features): `SEGARECOMP_SHOT=<file.bmp>` with `SEGARECOMP_SHOT_PAGE` (`library|list|details:<n>|import|storage|settings<tab>`) and `SEGARECOMP_SHOT_SIZE=WxH`
(`SEGARECOMP_SHOT_SCALE` forces a UI scale, `SEGARECOMP_SHOT_LOG` expands the build log, `SEGARECOMP_SHOT_MENU` opens the first game's context menu);
the Master System/Genesis review hooks (`SEGARECOMP_SHOT_ROM`, `..._POLICY`, `..._MODEL`, `..._REGION`, `SEGARECOMP_GUI_AUTOPLAY`, `SEGARECOMP_GUI_TRACE`) behave as before.
`tools/launcher_ui_fixture.py` builds the fictional fixture library used for the screenshots.
`tools/launcher_visual_capture.py` captures an existing synthetic fixture with the **specified binary**, at both supported sizes. It requires Pillow
(review tooling only, not a runtime dependency). `--build-rom` optionally captures an actual build of a project-authored synthetic cartridge in a temporary
cache; it never runs the resulting game. `SEGARECOMP_SHOT_SCROLL=bottom` captures the lower part of a long page without saving a scroll preference.

## Screenshots (fictional fixtures)

All images are real captures of the built binaries, rendered from fictional, project-authored data: synthetic ROMs built through the packaged launcher
(`tools/launcher_ui_fixture.py`) and a synthetic picture for the viewer (`libs/viewer/test/sv_gallery.c`). No commercial ROM, cover or screenshot is involved;
the design concept boards are not part of the repository. The fixture holds real states, never forged flags: a Compatibility build, a real Optimized build,
an Optimized selection made after a Compatibility build (its Compatibility executable kept), a Master System game with and one without a declared mapper, a
metadata-only game whose ROM was moved away, and a deliberately invalid cartridge whose build really fails. Two games carry fictional user-chosen covers; the
others show the placeholder paintings.

* `screenshots/launcher/` - 1080x740 and `launcher-small/` - 720x520 (the minimum): library grid/list/select and context menu, every Details state
  (`03*`: Genesis Compatibility, Genesis Optimized, mode switch awaiting a build, Master System mapper selection, metadata only + missing ROM, failed build and
  its log, Master System ready), import with real results (added, duplicate, refused), the empty library, storage and its cleanup confirmation, all settings
  sections, lower-page views and a real build in progress. `launcher-hidpi/` - the same layout at a forced 2x UI scale in 2160x1480 output pixels (a
  simulation, not an OS HiDPI test). `screenshots/launcher-capture.json` records the captured binary SHA-256 and the exact file list.
* `screenshots/review/before/` - the principal screens of the previous binary (branch commit 034393d) captured the same way, for before/after review.
* `screenshots/viewer-*` - the shared viewer (Genesis, Master System 192 and 224 lines) at 1080x740 with `-small/` 720x520 counterparts; see
  [viewer UI review](viewer-ui.md) and `screenshots/viewer-capture.json`.
* `screenshots/input-diagnostics/` - launcher states A (no controller), B (joystick without mapping), C (open failure, injected through the test seam) and OK
  (live virtual gamepad), and the viewer's Controller status page for the same cases. **These use SDL virtual joysticks, not a physical controller.**

These captures use SDL's dummy video driver and software renderer on Linux, not a physical display or controller. To reproduce:

```sh
python3 tools/launcher_ui_fixture.py --launcher <staged-package>/Segarecomp --out <tmp>/fixture
python3 tools/launcher_visual_capture.py --launcher <staged-package>/Segarecomp --home <staged-package> --fixture <tmp>/fixture \
  --out docs/product/screenshots --build-rom <project-authored-synthetic-ROM> [--references <kit>/references/launcher_screens --comparisons <ignored dir>]
```

### Launcher design correction (SEG-058, final pass)

* **Navigation.** Exactly four primary areas; Details is contextual (card, row action, context menu). Back restores the library state; the navigation test
  asserts the four entries, keyboard and gamepad walks, and Details round trips that keep search, platform filter, view, scroll position and card focus.
* **Details hierarchy.** Identity and the two dominant actions first, the Build Configuration card next (Build mode and region or mapper as large option
  tiles, fully visible at 1080x740 for Genesis; for Master System the mapper and build mode are visible and the expander sits at the fold), provenance and
  the build log below, a measured storage rail on the right (below the configuration in narrow windows). The configuration states what the executable on
  disk is (from `status.json`), not only what is selected.
* **Art.** The repository's sunset remains the only scenery: the hero is about 163 points high at 1080x740 (114 at 720x520), cover-scaled so the whole sun
  disc, mountains and waterfalls stay visible, with readable scrims. The v2 kit's backgrounds were audited and not shipped (smooth 47k-141k-colour
  renders; see `apps/segarecomp-launcher/assets/manifest.json`) and its four covers were one image. A game without a cover shows one of the four distinct
  placeholder paintings supplied afterwards by the project owner (chosen by digest) under the platform band and a "NO COVER ART" tag.
* **Cards and screens.** Cards show the art, a build-state badge, title, platform and last activity (size and dates moved to the list, storage and
  Details). Settings groups are titled section cards. The empty library shows the import guide. Import results replace the guide when present.
* **Fixes found while reviewing captures.** Build records adopted at start-up for existing entries were not saved; the GUI trace path dangled after
  `SDL_Quit`, so its last line went to a garbage file name in the working directory.

Remaining differences from the concepts, with reasons: long pages still scroll at the minimum size and below the Details configuration (provenance is
secondary by design); there are no invented compatibility ratings, build percentages, storage figures, cover downloads or commercial covers. **Cover
downloads remain unsupported** (manual local art and an explicit browser lookup). **SDL_GPU is not implemented**; SDL_Renderer remains the presenter.
**Audio device selection is not implemented**. **Build cancellation is not implemented**; no Cancel button is offered and closing waits for the build.
