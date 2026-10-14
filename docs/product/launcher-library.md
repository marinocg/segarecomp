# Launcher library, storage and the shared in-game viewer

This is the user-facing description of what ships in the consumer launcher (`Segarecomp`) and in every generated game's window. Design rationale:
`docs/decisions/0120-shared-sdl3-viewer-foundation-and-launcher-library.md`.

## What the launcher is

A resizable window (default 1080x740, minimum about 720x520, HiDPI aware, compact mode) with five areas: **Library**, **Import ROMs**, **Storage**,
**Settings** (including **Input Diagnostics**) and a per-game **Details** page. The repository's sunset artwork forms the header banner and the dimmed scenery behind the window. Everything it shows is read from the library file, the build cache or the product: no placeholder values.

* **Library** - grid or list (a game without a cover shows a neutral cartridge with its initials; a cover is only ever an image you chose), search, platform/status filters, sort (last played, title, last built, added, size), favorites, multi-select
  (Select mode) for storage actions and removal. An empty library shows the ROM drop zone.
* **Import** - drag and drop or Browse (several files). Each file is classified by the same media classifier as before, de-duplicated by SHA-256
  (the same game elsewhere only adds a tracked location), and listed with a reason when refused. Importing never builds (an explicit checkbox builds
  the new games). ROMs are referenced, never copied, moved or uploaded.
* **Details** - cover (change/remove; the generic placeholder otherwise), editable display name (the header name is kept), favorite, platform,
  region/mapper, ROM digest and link health, first added / last built / last played / last run result, toolchain and product version of the build,
  output size by kind (executable, generated C, objects, logs, metadata), and the actions Play, Build/Recompile/Rebuild, Locate ROM, Open build folder,
  View logs, Storage cleanup, Remove from library. Build options keep the previous capabilities: Genesis region, H-interrupt alias discovery,
  Compatibility/Optimized (and the experimental Genesis model), the Master System mapper declaration (never defaulted) and the experimental Master
  System Optimized mode when the product offers it, with the one-shot visible Compatibility restart.
* **Status** is computed from the build identity, never from a title: *Built*, *Not built*, *Metadata only* (you removed the build on purpose; "rebuild
  required"), *Stale build* (made by another release or option set; never started), *Build failed*, plus a *ROM missing* badge. Play is offered only for a build
  whose identity matches exactly.
* **Build** shows the real `@stage` messages (analyze, generate, compile, link) and the tail of the build log. There is no percentage because the
  build tool reports none, and no Cancel because killing a build mid-way is not safe (closing the window waits for it).

## Where data lives

| Data | Location | Cleaned by Storage? |
|---|---|---|
| Built games, logs | `<cache>/games/<id>[.partial|.failed]/` | yes (by plan) |
| Shared Zig compiler cache | `<cache>/zig-cache/` | yes (Clear shared compiler cache) |
| Library, covers, launcher + viewer settings, screenshots dir default | `<cache>/library/` (`library.json`, `covers/`, `launcher-settings.json`, `viewer-settings.cfg`) | never |
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
(not features): `SEGARECOMP_SHOT=<file.bmp>` with `SEGARECOMP_SHOT_PAGE` (`library|list|details:<n>|import|storage|settings<tab>`) and `SEGARECOMP_SHOT_SIZE=WxH`;
the Master System/Genesis review hooks (`SEGARECOMP_SHOT_ROM`, `..._POLICY`, `..._MODEL`, `..._REGION`, `SEGARECOMP_GUI_AUTOPLAY`, `SEGARECOMP_GUI_TRACE`) behave as before.
`tools/launcher_ui_fixture.py` builds the fictional fixture library used for the screenshots.
`tools/launcher_visual_capture.py` captures an existing synthetic fixture with the **specified binary**, at both supported sizes. It requires Pillow
(review tooling only, not a runtime dependency). `--build-rom` optionally captures an actual build of a project-authored synthetic cartridge in a temporary
cache; it never runs the resulting game. `SEGARECOMP_SHOT_SCROLL=bottom` captures the lower part of a long page without saving a scroll preference.

## Screenshots (fictional fixtures)

All images are real captures of the built binaries, rendered from fictional, project-authored data: synthetic ROMs built through the packaged launcher
(`tools/launcher_ui_fixture.py`, procedurally drawn covers for some games and none for others) and a synthetic picture for the viewer
(`libs/viewer/test/sv_gallery.c`). No commercial ROM, cover or screenshot is involved; the design concept boards are not part of the repository.

* `screenshots/launcher/` - 1080x740; `launcher-small/` - 720x520 (the minimum size). These are **42 refreshed launcher captures**: library grid/list/select,
  details with chosen art and without art, import, storage, the real intermediate-cleanup confirmation, all six settings sections, lower-page views and a
  real build in progress. `screenshots/launcher-capture.json` records the captured binary SHA-256 and exact file list. These captures use SDL's dummy video
  driver and software renderer on Linux, not a physical display or controller. The existing fixture builds correctly appear **stale** when their recorded
  build identity does not match; no screenshot readiness flag is forged. The two build-progress captures use fresh temporary caches and real producer stages.
* `launcher-states/` is **historical evidence from the earlier binary**, not refreshed by the launcher visual correction: ready, metadata-only, stale,
  ROM missing, failed (with log), not built, multiple imports and build progress. Do not use it as proof of the current layout.
* `screenshots/viewer-genesis/`, `viewer-sms/` (192 lines), `viewer-sms-224/` (224-line mode) now contain new-binary **1080x740** captures, with
  matching `-small/` directories at **720x520**. See [viewer UI review](viewer-ui.md) and `screenshots/viewer-capture.json` for all pages, input cases,
  hashes, scale simulations and honest validation limits.
* `screenshots/input-diagnostics/` - launcher states A (no controller), B (joystick without mapping), C (open failure), OK (live virtual gamepad) and the viewer's
  Controller status page for the same cases. **These use SDL virtual joysticks, not a physical Switch 2 Pro Controller.**

### Launcher visual correction review

The original repository sunset and wordmark remain the runtime art. The aspect-preserving hero is now 180–240 logical pixels high (about 215 at
1080x740, 180 at 720x520), with no content-panel overlap. No new background, cover landscape, font binary or kit concept artwork was added. A missing
cover uses a neutral code-drawn cartridge with title initials. The fictional chosen covers in the fixture are explicit local-art examples, not fallbacks.
Short windows use horizontal compact grid cards that keep title, platform and build status visible; the normal grid returns when height permits.

Settings use bounded, independently scrollable content, labelled rows and wrapping option groups; at narrow widths the section selector becomes a dropdown.
Details wrap secondary actions and put build progress before metadata. Storage prioritizes build status and total size in narrow tables, retaining measured
breakdowns in the selection panel. Long paths/digests wrap at character boundaries. Confirmation scope lists scroll while Cancel and the destructive action
remain fixed inside the window. Sidebar arrows/D-pad stay within the navigation group regardless of hero height.

Local comparison sheets were made for the seven individual launcher design crops at **both sizes**. They are review-only, ignored build output, never runtime
assets or release contents. To reproduce with a locally supplied kit (keep comparison output ignored):

```sh
python3 tools/launcher_visual_capture.py --launcher <new-built-Segarecomp> --home <staged-package> \
  --fixture <synthetic-fixture> --out docs/product/screenshots \
  --references <kit>/references/launcher_screens --comparisons build/visual-review/comparisons \
  --build-rom <project-authored-synthetic-ROM>
```

Validation: the seven focused launcher/library/storage CTest tests passed. After the final layout adjustment, the navigation test passed again with
9 keyboard/gamepad assertions and 24 real-window responsive render smoke cases. This is not pixel-perfect visual automation, HiDPI hardware validation,
physical-gamepad validation, a three-OS release test, or a final fast gate. The validator owns the latter. No viewer source changed in this phase, and the
  viewer correction is now documented separately in [viewer UI review](viewer-ui.md); this paragraph's validation counts describe only the launcher visual phase.

Remaining intentional differences from the concepts: long pages require scrolling, particularly at minimum size; there are no invented compatibility
ratings, build percentages, storage figures or commercial covers. **Cover downloads remain unsupported** (manual local art and an explicit browser lookup,
labelled `Web lookup`, are available). **SDL_GPU is not implemented**; SDL_Renderer remains the presenter. **Audio device selection is not implemented**;
the default output device is used. **Build cancellation is not implemented**; no Cancel button is offered and closing waits for the build. These are capability
  limits, not capabilities supplied by this visual correction. The subsequent viewer reconciliation audits overlay hierarchy, video preview/CRT presentation,
  controls/remap focus, audio limitations and confirmation layouts against the individual viewer crops; launcher captures do not validate those behaviors.
