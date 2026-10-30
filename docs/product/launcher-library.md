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
  the new games). ROM files are added by reference: never copied, moved, modified or uploaded. **Zipped ROMs**: a `.zip` holding exactly one `.md/.bin/.gen/.smd/.sms`
  (stored or deflate, CRC-checked; no zip64 or encryption) is unpacked once into `<library>/roms/<sha256>.<ext>` and that copy is what the library
  tracks and builds; the archive is only read. Decoding writes into a buffer of exactly the declared size (at most 16 MiB) and stops if the stream would
  exceed it; an existing copy is reused only when its own SHA-256 matches its name (a damaged copy is replaced, a link or folder in its place is refused);
  writes go through a temporary file and a rename. A zip with no ROM or several ROMs is refused with the reason.
  **Extracted copies are kept**: clearing build caches, keeping metadata only and removing a game from the library do not delete them (delete them from
  the `roms` folder yourself if you want). The same ROM zipped or not is one game.
  Works for drag and drop, Browse, Locate ROM and the headless `--import` / `--build`. A sidecar `.mapper.json` inside a zip is not read (declare the
  Master System mapper on the game's page).
* **Sega CD** - a different kind of game: **Import ROMs > Add Sega CD game...** takes your BIOS image, the disc's CUE file and (optionally) a verified viewer
  program built with the developer procedure. All three are referenced by path (never copied or bundled) and only paths, sizes and SHA-256 values are stored.
  The details page offers **Build** (SEG-078-T003: compiles a viewer program from the two files on this computer, with progress and Cancel; experimental, see the
  coverage notes) next to **Import program...**, shows the verification (BIOS, disc files, program) and
  the honest state (inputs not configured, no program yet, headless-only build, stale program, mismatched, ready), and Play opens the shared viewer. See
  [sega-cd-play.md](sega-cd-play.md).
* **Details** - top to bottom, inside the first 1080x740 view: the cover (change from its corner), editable display name (the header name is kept),
  favorite, platform, build state and ROM link health, one sentence of what matters now, and the two dominant actions **Play** (only for an executable whose
  identity matches the configuration exactly) and **Build / Recompile / Rebuild / Try again** (a missing ROM makes **Locate ROM** the primary action instead).
  Rarer actions are in **More**: build log, open build folder, manage storage, change/remove cover, find cover online, copy SHA-256, locate ROM and (set
  apart) remove from library. The **Build Configuration** card follows: **Build mode** (Compatibility | Optimized; **Optimized is the default** for newly imported Genesis and Master System games: Genesis Optimized is the
  former Optimized + Experimental model request (`--aot-policy optimized --aot-model gen2`), and there is no model checkbox; Master System Optimized is its own
  product policy, offered only when the product states it is available, otherwise the game builds as Compatibility and the card says so, without rewriting the
  stored Optimized intent. An explicit Compatibility selection is kept, and a legacy Optimized/v1 Genesis record takes the new gen2 meaning and needs a rebuild)
  and, for Genesis, the **console region**
  (USA NTSC | Japan NTSC | Europe PAL, applied at launch, never a rebuild; PAL still runs on the 60 Hz schedule), or for Master System the **cartridge
  mapper** (nothing preselected, never guessed; a sidecar manifest declares it) and the one console profile. A state line says what the executable on disk
  actually is (from `status.json`: effective policy and any Optimized fallback), whether the selection matches it, and when a mode switch left the other
  mode's executable on disk ("switch back to play it"). **Advanced build options** (H-interrupt alias discovery) sit behind an expander with their consequences. Below: **Build provenance** (SHA-256 with Copy, ROM file and folder, last recompiled, last
  played and last run result, build version and toolchain, build identity with the selected options, effective policy details, diagnostics) and the build
  log. A **storage** rail shows the game's measured footprint (native executable, generated C, objects, logs, metadata, other build files) with *Clean
  intermediates* and *Manage storage*; for a metadata-only game it says the history is kept and a new build is needed to play.
* **Status** is computed from the build identity, never from a title: *Ready*, *Not built*, *Metadata only* (you removed the build on purpose; "rebuild
  required"), *Needs rebuild* (made by another release or option set, or a build mode whose executable is not built yet; never started), *Build failed*, plus a *ROM missing* badge. Play is offered for a build
  whose identity matches exactly, and for one more case: *Playable (older build)* (below).
* **Playing a build from an earlier release.** Updating Segarecomp changes the build identity (the product tool, release and install path are part of it), which
  used to force a recompile before anything could be played. The exact identity is still what decides whether generated output may be *reused* by a new build,
  but a finished executable can additionally be *played* when the launcher can establish from the build's own records that it still runs here: a real build
  folder inside the cache with an executable and a successful `status.json`; `metadata.json` that names this library ROM (SHA-256), the platform, this operating
  system and processor and, for Master System, the same declared mapper and profile; a complete recorded policy for the *same mode* (Compatibility/Optimized) the
  game requests now (an Optimized build keeps being reported with its own model, e.g. v1, never relabelled as the new gen2); an H-interrupt discovery setting that
  matches; an installed SDL3 library; and the host-interface marker `runtime_contract` (recorded by every new build; bumped only when a change would stop an
  already built program from running). Builds made before the marker existed are accepted only from the explicitly listed release (`0.5.0`); a version number
  alone proves nothing. Everything else fails closed: the game shows *Needs rebuild* with the reason. A playable earlier build shows *Playable (older build)*,
  keeps **Play** as the main action, shows what it actually is, and **Recompile** is an independent action that is never queued automatically. A cancelled or
  failed recompile, or a moved ROM file, leaves the earlier build playable. A voluntary recompile builds into the current identity with the usual atomic
  publication; afterwards the usual "keep old build artifacts" setting decides whether the earlier folder is removed. ADR 0138 records the rules.
* **Build** shows the real `@stage` messages (analyze, generate, compile, link) and the tail of the build log. There is no percentage because the
  build tool reports none. **Cancel build** (Details, and the game's menu) stops the running build: the build tool and every compiler process it started are shut down, the
  attempt's own `.partial` folder is removed, no *Build failed* state or `.failed` diagnostics are created, and an earlier completed build of the game stays playable. A cancel that
  arrives after the build has been published or has failed is refused (that outcome stands). A *queued* game is removed with **Unqueue** / *Remove from build queue*; it has
  started nothing. Closing the window still waits for a running build.

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

Never touched: original ROMs, ROM copies extracted from ZIP archives (`library/roms/`), covers, screenshots, `library.json`, unknown folders in the cache (reported, not deleted), a running game or build. Cleanup is
refused while a build or game runs (one busy guard). Settings > Build & Runtime offers an **automatic policy** after a successful build: never,
remove intermediates, keep the newest N playable games (older ones are reduced to metadata only; favorites, running games and the game being built
are excluded) or a soft limit that only reminds you. The default is *never*.

**First run.** A genuinely new profile (no settings, library or viewer-settings file yet) is asked once, in a modal that is operable by keyboard and gamepad
(focus starts on the first choice, Esc/East decides later), whether to **Keep intermediates** (policy *never*) or **Remove after builds** (policy *remove
intermediates*). The choice writes the same cleanup policy Settings > Build & Runtime edits, together with a one-time `intermediates_choice_made` marker in
`launcher-settings.json`; the marker is written only when the file was saved (a failed save keeps the question open). Existing profiles, including a
settings file written before the marker existed, are never asked and keep their policy; restoring launcher defaults never re-asks. Scripted windows
(GUI scripts, screenshots, autoplay) skip the question unless `SEGARECOMP_GUI_ONBOARDING=1` (new profiles) or `=always` (test seam) is set.

## Covers and metadata provider

Covers are images (PNG or JPEG, up to 2 MiB and 2048 px, validated by content and fully decoded) copied into `library/covers/<sha256>.<ext>`.
They survive cleanup and re-import. There are three ways to get one:

* **Change cover** picks a local image.
* **Download cover (Libretro)** (a game's *More* / context menu) and **Download missing covers** (Settings > Metadata) fetch the box art from the
  public Libretro thumbnail trees (`Sega - Mega Drive - Genesis`, `Sega - Master System - Mark III`, `Sega - Mega-CD - Sega CD`, `Named_Boxarts`). This happens **only on that
  explicit action**, on a worker thread, and the downloaded file goes through the same validation as a local image before it is used.
* **Find cover online** opens your browser at the likeliest Libretro name.

How the name is found (`cover_download.cpp`): Libretro names thumbnails like No-Intro sets (`Streets of Rage 2 (USA)`, `Sonic The Hedgehog (USA, Europe)`),
and a ROM header does not carry that name, which is why the earlier single exact guess (`<title> (<first region>)`) usually missed. The lookup now
tries, in order: the tracked ROM file names (No-Intro sets are named exactly like the thumbnails; extracted ZIP copies, named by digest, are skipped),
then the display and header titles as written and in Title Case (upper-case Genesis headers; both No-Intro capitalisations of minor words), each with
the multi-region tags Libretro uses (`(USA, Europe)`, `(World)`, `(Japan, USA)`, ...) in your preferred region order. When Libretro's directory
listing of box arts can be read (fetched at most once a week and cached in the cache folder), the best listed name whose base (case, tags, `&`/`and`
and a trailing `, The` ignored) matches is chosen, preferring released versions over betas, prototypes and unlicensed sets and then your region
order; with no listing the candidate names are probed one by one (at most 24).

Transport: the operating system's `curl` (Windows 10 and later, macOS and Linux distributions ship it; looked up at fixed system paths, never through
`PATH`), started with a fixed argument vector (no shell), `https` only also across redirects, at most 5 redirects, an 8 MiB and 45 s limit, and the
HTTP status checked (only 200 is a file, only 404/410 mean "no such cover"; anything else, such as a proxy refusal, means "unreachable").
`thumbnails.libretro.com` is asked first; while it cannot be reached the `libretro-thumbnails` mirror on `raw.githubusercontent.com` is used. Only
the thumbnail file name is requested: **ROM bytes and hashes are never sent anywhere**. Without curl nothing is downloaded and the reason is shown.
Images belong to their rights holders and the provider's terms apply; none is bundled or committed. ScreenScraper is not used (it needs developer
credentials).

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

## Operating the launcher with a gamepad only

Every ordinary journey (first-run setup, import, browse, game page, build / cancel / queue, play, Storage, Settings) can be completed with an SDL gamepad and no mouse
or keyboard: D-pad/stick move focus, **A** activates, **B** goes back, **Y** opens the focused game's menu, D-pad Right/Left hop between the sidebar and the page, and a
hint line shows these buttons while a controller is the last input device. Importing or locating a ROM and choosing a cover use a built-in, root-confined file picker
when a controller was the last input device (the native OS dialog stays for mouse and keyboard, and is the fallback's alternative when no system chooser exists).
Details, the automated journeys and a manual smoke walk are in `docs/testing/launcher-gamepad-navigation.md`; ADR 0139 records the decisions.

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
secondary by design); there are no invented compatibility ratings, build percentages, storage figures or bundled commercial covers. Cover
downloads happen only on an explicit request (see *Covers and metadata provider*). **SDL_GPU is not implemented**; SDL_Renderer remains the presenter.
**Audio device selection is not implemented**. Cancelling a build is implemented (see *Build*); closing the window still waits for a running build.
