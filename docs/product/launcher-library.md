# Launcher library, storage and the shared in-game viewer

This is the user-facing description of what ships in the consumer launcher (`Segarecomp`) and in every generated game's window. Design rationale:
`docs/decisions/0120-shared-sdl3-viewer-foundation-and-launcher-library.md`.

## What the launcher is

A resizable window (default 1080x740, minimum about 720x520, HiDPI aware, compact mode) whose primary navigation is two destinations, **Library**
and **Settings** (four groups: **General**, **Appearance & Controls** with Input diagnostics, **Storage & Data** and **Advanced**), plus the one primary **Add game** button.
Below 900 px the sidebar becomes a rail (icon over a label). A game's page is contextual, not a sidebar entry: a card, a list row's Details button or the game's context
menu (right click, the Menu key or the gamepad's North button) opens it, and **Back** (the button, Esc or the gamepad's East button) returns to exactly the library state it came from:
search text, platform/status filters, sort, grid/list view, scroll position and the focused card. The repository's sunset artwork forms the header banner and the dimmed
scenery behind the window. Everything it shows is read from the library file, the build cache or the product: no placeholder values. Text is Inter (Regular and SemiBold) with
Silkscreen only for headings; the layout scale follows the display's scale and pixel density automatically (the review record `launcher-ux-review.md` states exactly what is and is not verified on which host).

A fresh profile (no settings, library or viewer-settings file) is welcomed with a skippable page (three steps, **Add game**, **Skip to Library**); skipping stores nothing.

* **Library** - grid or list. Every card or row carries the cover (a game without a cover shows one of four fictional placeholder paintings, picked from its digest, under the platform
  band and a "NO COVER ART" tag; a cover is only ever an image you chose), the name, the platform and region, ONE state (symbol, color and text) and ONE main action.
  States: *Ready*, *Older version* (an earlier release's verified build still plays), *Not prepared*, *Preparing*, *Queued*, *Failed*, *Game file missing*, *Needs a mapper choice*,
  *Metadata only*, *Running*. Main actions: *Play*, *Prepare & play*, *Prepare*, *Retry*, *Choose mapper*, *Find game file*, *Cancel* (while preparing), *Unqueue* (while queued). Search (Ctrl+F, Cmd+F on macOS),
  platform and status filters, sort (last played, title, last built, added, size), favorites and **Select** (multi-select for storage actions and removal). An empty library shows the
  Add game empty state.
* **Add game** - drag and drop, or *Choose files* (several files, plain ROMs, `.zip` archives or a Sega CD `.cue`; a controller opens a built-in, root-confined chooser). Nothing is added at
  once: every file is inspected first and listed in a **review** with its verdict (*Valid*, *Needs a choice*, *Already in library*, *Cannot be added*), the reason and the next step.
  Then you decide: **Add & prepare (N)** adds the selected games and queues one preparation each (one at a time, cancellable, nothing starts playing by itself) or **Add only (N)** keeps
  them in the library to prepare later; **Cancel** discards the review. ROM files are added by reference: never copied, moved, modified or uploaded; files are de-duplicated by SHA-256
  (the same game elsewhere only adds a tracked location). **Zipped ROMs**: a `.zip` holding exactly one `.md/.bin/.gen/.smd/.sms`
  (stored or deflate, CRC-checked; no zip64 or encryption) is unpacked once into `<library>/roms/<sha256>.<ext>` and that copy is what the library tracks and builds; the archive is only
  read. Decoding writes into a buffer of exactly the declared size (at most 16 MiB) and stops if the stream would exceed it; an existing copy is reused only when its own SHA-256 matches its name;
  writes go through a temporary file and a rename. **Extracted copies are kept** by every cleanup. The same ROM zipped or not is one game. Works for drag and drop, the chooser,
  *Find game file* and the headless `--import` / `--build`. A sidecar `.mapper.json` inside a zip is not read (declare the Master System mapper on the game's page).
* **Sega CD** - goes through the same **Add game** review: choose or drop the disc's **.cue**; the review shows the disc's verdict (tracks found, missing track files by
  name, or a typed unsupported-layout reason) and asks for your own BIOS image when no verified one is remembered (remembered BIOS references are measured again at every use; see
  [sega-cd-play.md](sega-cd-play.md#remembered-bios-local-profiles)). The BIOS and disc are referenced by path (never copied or bundled); only paths, sizes and SHA-256 values are stored.
  After that a Sega CD game is an ordinary library game with the same cards, states, actions, page, queue and storage actions; only the BIOS / disc rows, *Import / Replace / Verify program*
  (Game > Advanced), *Locate BIOS...* / *Locate disc...* and an *Input recording* section differ. See [sega-cd-play.md](sega-cd-play.md).
* **Game page** - a header (cover with a pencil to change it, the editable display name, favorite, platform, ONE state pill, one sentence of what happens next and ONE primary action:
  *Play*, *Prepare & play*, *Prepare*, *Retry*, *Choose mapper*, *Find game file*, *Cancel* or *Unqueue*, with *Prepare again* and **More** beside it) above four tabs:
  * **Overview** - notices, the failure card (*What happened*: the reason, what can be done, *View log*, *Game settings*), and rows that open the other tabs.
  * **Mods** (Genesis) - the per-game installed mods: *No mod (original game)* and each installed mod with a typed status; exactly one is selected; *Install mod...*, approval of native code, *Use this mod*,
    *Prepare selected mod*, *Play modded game*, remove. See [patch-packages.md](patch-packages.md) and [mod-projects.md](mod-projects.md). The tab explains itself on platforms without mods.
  * **Game settings** - *Preparation - this game* (mode Compatibility | Optimized; **Optimized is the default**, console region USA / Japan / Europe applied at launch, or for Master System the **cartridge
    mapper**: nothing preselected, never guessed; *Advanced build options*: H-interrupt discovery, cartridge hardware Automatic / Standard / Lock-on), recording, the display and audio note (shared
    settings, not per-game: see Settings) and *Library entry - this game* (favorite, cover, display name). A state line says what the executable on disk actually is and whether the selection matches it.
  * **Advanced** - *Build provenance* (ROM SHA-256, build version and toolchain, build identity, effective policy, **Audio implementation**, diagnostics), older versions, the log, the game file
    location and a measured *Storage* card with *Clean intermediates*.
* **Status** is computed from the build identity, never from a title. The words you see are the library's (*Ready*, *Not prepared*, *Metadata only*, *Out of date*, *Failed*, *Older version*, *Game file missing*, *Needs a mapper choice*);
  scripts and tests keep the stable technical labels *Ready*, *Not built*, *Metadata only*, *Needs rebuild*, *Build failed*, *Playable (older build)*. *Play* is offered for a build whose identity matches exactly, and for one more case: *Older version* (below).
* **Playing a build from an earlier release.** Updating Segarecomp changes the build identity (the product tool, release and install path are part of it). The exact identity decides whether generated output
  may be *reused* by a new build, but a finished executable can additionally be *played* when the launcher can establish from the build's own records that it still runs here: a real build
  folder inside the cache with an executable and a successful `status.json`; `metadata.json` that names this library ROM (SHA-256), the platform, this operating
  system and processor and, for Master System, the same declared mapper and profile, for Genesis the same cartridge hardware; a complete recorded policy for the *same mode* the
  game requests now; an H-interrupt discovery setting that matches; an installed SDL3 library; and the host-interface marker `runtime_contract` (recorded by every new build; bumped only when a change would stop an
  already built program from running). Builds made before the marker existed are accepted only from the explicitly listed release (`0.5.0`); a version number
  alone proves nothing. Everything else fails closed: the game shows *Out of date* with the reason. A playable earlier build shows *Older version*, keeps **Play** as the main action, and **Prepare** (again)
  is an independent action that is never queued automatically. A cancelled or failed preparation, or a moved ROM file, leaves the earlier build playable. ADR 0138 records the rules.
* **Preparing** shows the real `@stage` messages (analyze, generate C, compile, link) and the tail of the build log. There is no percentage because the build tool reports none. **Cancel** (library card, game page,
  menu) stops the running build: the build tool and every compiler process it started are shut down, the attempt's own `.partial` folder is removed, no failed state or `.failed` diagnostics are created, and an
  earlier completed build of the game stays playable. A cancel that arrives after the build has been published or has failed is refused (that outcome stands). A *queued* game is taken out with **Unqueue**; it has
  started nothing. Closing the window still waits for a running build.
* **Settings** - *General* (library view and sort, compact mode, existing builds, the preparation cleanup policy, covers and metadata consent), *Appearance & Controls* (shared viewer settings: window, picture, widescreen,
  audio, keyboard and gamepad, and **Input diagnostics**), *Storage & Data* (disk usage, **Clean up** with a truthful plan, and the data location: below) and *Advanced* (build and runtime options, about, effective paths).

## Audio implementation (Genesis, ADR 0291)

The **Audio implementation** row of Build provenance says how the executable Play will launch implements sound, from that build's own `status.json` (`audio_implementation`)
and nothing else - never from the title, the file name or the current selection:

| Row | Meaning |
| --- | --- |
| Native GEMS 2.5 (the driver's version) | The build **selected the verified native GEMS driver by itself**: the game uploads a verified GEMS driver image and the original generated-native Z80 of that game could not run it. The detail lists: automatically selected for a verified GEMS driver image (and why); independently reimplemented in native C11; verified against the original driver's command and chip-access behavior (*reference-checked*; or *build-qualified only*); the original Z80 not executed for that driver; save states and rewind supported (an older build that predates them says so); timing event-accurate rather than instruction-cycle-accurate; the coverage and implementation version; and what the build's own start-up run did (commands, sequences, DAC samples, a stop on malformed data if any). It never says that a play session sounded right. A developer override is worded as such. |
| Original Z80 (generated-native) | The game's own sound driver runs as generated native code. Compiled structural alternatives, if any, are counted. Not a replacement. If that driver could not run (the Z80 is isolated, no music support) and no verified replacement applies, the detail starts with **Degraded:** and says so separately. |
| Not recorded | No playable build of the selection, or a build that recorded nothing (an older release, an imported executable). |
| Unknown | The record was malformed or not recognised; nothing is assumed. |

There is **no option** for any of this. The build decides: it keeps the original Z80 unless a verified GEMS image is uploaded *and* the original is recorded as failing, in which case it
builds the same program with the replacement (a second, deterministic pass; ADR 0291 "Decision 4"). The launcher passes no sound argument to the build, an older catalog's
`native_gems_audio` key is ignored (and dropped on the next save), and the choice is not part of a cache identity - only what the product ships to choose from is (the
replacement's implementation version and the verified-driver catalog revision), so an updated product rebuilds rather than reuses an older program. Older records (schema 1) are
still read: they show what they recorded and nothing more (an older native record says save states are not available in that build).

## Cartridge hardware (Genesis)

The default is: import the game, build it, play it. The launcher recognizes the supported Sonic & Knuckles lock-on images from their contents (a 2 MiB Sonic & Knuckles, or the 4 MiB image of Sonic & Knuckles followed by Sonic 3), selects
Lock-on pass-through automatically, saves that with the library entry, and Game Details says `Lock-on cartridge - automatically detected`. There is no dialog. Every other Genesis game is a standard cartridge, its build command and identity are unchanged, and nothing about hardware is shown outside Advanced build options.

* **Automatic recognition** reads header fields only (ADR 0268 section 5 lists them). File names, library titles, ZIP names and patches are never consulted. An image that resembles a lock-on cartridge but cannot be identified (modified, another size, an unknown attached cartridge) stays standard, and the page says why.
* **Manual override**: Advanced build options, *Cartridge hardware*: Automatic (recommended), Standard (an ordinary cartridge), Lock-on (a pass-through cartridge). An explicit choice outranks recognition and survives reimports, restarts and rescans. Headless: `--mapper lock_on` or `--mapper standard` (`--build` does not read the library, so a choice stored there does not apply to it; its own option does).
* A library entry from an earlier release is examined when its original ROM is available; a missing ROM keeps what is stored. If that changes the hardware, the earlier build is not reused (the page says why) and a rebuild is needed. The hardware is a build option of the one game, not a separate game, and installing, selecting or removing a patch never changes it.

## Where data lives

| Data | Location | Cleaned by Storage? |
|---|---|---|
| Built games, logs | `<cache>/games/<id>[.partial|.failed]/` | yes (by plan) |
| A Sega CD game's generated build | `<library>/sega-cd/<id16>/build[.partial|.failed]/` (`program-viewer`, `status.json`, `build.log`) | yes (the same plans as a native build) |
| A Sega CD game's **save data** and **input recordings** | `<library>/sega-cd/<id16>/backup-ram.bin`, `recordings/` | **never** (protected user data, see below) |
| Shared Zig compiler cache and the Sega CD object cache | `<cache>/zig-cache/`, `<cache>/sega-cd-objects/` | yes (Clear shared compiler cache) |
| Library, covers, ROMs unpacked from zips, launcher + viewer settings, screenshots dir default | `<cache>/library/` (`library.json`, `covers/`, `roms/`, `launcher-settings.json`, `viewer-settings.cfg`) | never |
| Your ROMs | where they are | never |

`<cache>` is `SEGARECOMP_CACHE_DIR`, `%LOCALAPPDATA%\Segarecomp`, or the SDL preference path (`~/Library/Application Support/Segarecomp`, `~/.local/share/Segarecomp`).
`SEGARECOMP_LIBRARY_DIR` overrides the library location. Package install directory (read-only program files) and these per-user folders are
separate; Settings > Advanced shows both. Nothing is deleted at start-up, on import or on Play.

The data folder can be relocated from **Settings > Storage & Data > Change data location** (core contract: [ADR 0317](../decisions/0317-launcher-data-location-and-migration.md)): choose an empty folder, read the plan (exactly which
durable files are copied and how many bytes, what is not copied, the free space needed), confirm, and watch the real phases (Check, Copy, Library, Verify, Switch) with **Cancel move** until the final switch. A one-line pointer file `Segarecomp-data-location.json` beside the
default folder names the active one, absent means the defaults above, and `SEGARECOMP_CACHE_DIR` / `SEGARECOMP_LIBRARY_DIR` always win and refuse a move (the button says why). Durable data (library, settings, covers, saves,
recordings, mods) is copied, verified and switched atomically; builds and caches are not moved (those games are prepared again), nothing is removed from the previous folder
automatically, and its cleanup is a separate explicit step (*Clean up the previous location*, regenerable data by default, migrated copies unchanged since the move only when you ask). The new location is used from the next start;
a crash or restart mid-move is recovered at the next start with a notice, and the previous location stays in use until the switch.

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

Never touched: original ROMs, ROM copies extracted from ZIP archives (`library/roms/`), covers, screenshots, `library.json`, unknown folders in the cache (reported, not deleted), a running game or build.

**Protected user data (Sega CD).** A Sega CD game's save (its internal backup RAM) and its input recordings are the player's own data. The Storage report and the
game's Storage rail list them apart ("Saves & recordings"), outside every total that a cleanup can reclaim, and no cleanup action can contain them: the plans
are built from the generated build folders only, and the deletion backend refuses any other path under a game's folder, even in a forged plan. Rebuilding,
replacing the program, *Keep metadata only*, *Clean failed / abandoned builds*, the automatic keep-newest-N / soft-limit policies and *Remove from library* never delete or reset a save; a
removed game's save is found again when the same BIOS and disc are added again. The removal sheet says what is removed and what is kept. Cleanup is
refused while a build or game runs (one busy guard). Settings > General > Preparation offers an **automatic policy** after a successful build: never,
remove intermediates, keep the newest N playable games (older ones are reduced to metadata only; favorites, running games and the game being built
are excluded) or a soft limit that only reminds you. The default is *never*.

**First run.** A genuinely new profile (no settings, library or viewer-settings file yet) is asked once, in a modal that is operable by keyboard and gamepad
(focus starts on the first choice, Esc/East decides later), whether to **Keep intermediates** (policy *never*) or **Remove after builds** (policy *remove
intermediates*). The choice writes the same cleanup policy Settings > General > Preparation edits, together with a one-time `intermediates_choice_made` marker in
`launcher-settings.json`; the marker is written only when the file was saved (a failed save keeps the question open). Existing profiles, including a
settings file written before the marker existed, are never asked and keep their policy; restoring launcher defaults never re-asks. Scripted windows
(GUI scripts, screenshots, autoplay) skip the question unless `SEGARECOMP_GUI_ONBOARDING=1` (new profiles) or `=always` (test seam) is set.

## Covers and metadata provider

Covers are images (PNG or JPEG, up to 2 MiB and 2048 px, validated by content and fully decoded) copied into `library/covers/<sha256>.<ext>`.
They survive cleanup and re-import. There are three ways to get one:

* **Change cover** picks a local image.
* **Download cover (Libretro)** (a game's *More* / context menu) and **Download missing covers** (Settings > General > Metadata & covers) fetch the box art from the
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

* Resume; **Save State**, **Load State** and **Rewind** (Genesis, Master System and Sega CD: five numbered slots plus a quick slot, and a visual rewind; a session that cannot offer them, such as a recording, shows them disabled with the reason; see [save-states.md](save-states.md)); **Quick Settings** (fullscreen, scaling, aspect, display preset, VSync, FPS counter); **Video & Display** (plus scanline/mask/curvature/bloom/
  brightness sliders, a live preview that applies the current settings through the real CRT pipeline, and clickable preset buttons); **Controls** (with a **Controller status & test** page: what SDL sees, live buttons, the reason a pad does not work); **Audio** (volume/mute, gain only); **System & Diagnostics** (system,
  ROM digest when started from the launcher, FPS, renderer, picture/output size, audio queue and dropped chunks, screenshot, Reset on Master System);
  **Return to Launcher** (or Quit outside the launcher), with a confirmation.
* Scaling: *Fit* or *Integer* (whole-number multiples, falling back to fit below 1x); aspect *Original* (square pixels) or *4:3 corrected*; windowed
  or borderless fullscreen (never a display-mode change, so there is nothing to revert); HiDPI aware; the Master System's 192/224-line picture keeps its aspect.
* **Widescreen Mode** (Quick Settings and Video & Display, next to scaling and aspect; also in the launcher's Settings > Appearance & Controls): *Original* (default),
  *Extended Backgrounds (Level 2)* or *Enhanced Widescreen (Level 3)*. It is a presentation preference shared by the launcher and every viewer (Genesis, Master System
  and Sega CD) through the one `video.widescreen` key of `viewer-settings.cfg`; the pause menu applies a change to the next frame without a restart, reset or any
  change to the guest, and shows the description of the selected mode plus an honest support line. **Extended Backgrounds (Level 2) is implemented for Genesis (SEG-118), Sega CD (SEG-119; always reported as Partial, see `platforms/sega-cd/README.md`) and Master System (SEG-120; always Partial: the 256-pixel Mode 4 name table wraps, so the extra columns repeat scenery already on screen and reveal no more of the game world, see `platforms/master-system/README.md`);** a platform that does not
  report support (and any program built before this support) saves either experimental choice but reads "Not yet available for this game. Showing the original picture." and the picture
  stays Original. **Enhanced Widescreen (Level 3) is implemented for Genesis, Sega CD and Master System (SEG-121, ADR 0264):** it adds the sprites the game's own VDP state already places over the extra columns (at most a few edge-crossing pixels on the Master System), is always reported Partial, and shows as Level 2 where the sprite layer cannot be produced; it does not widen the game world. There is no widescreen control or message on the game picture itself. A program built before widescreen support keeps working in Original mode;
  it needs rebuilding to gain extended rendering (ADR 0261).
* **Side Presentation** (SEG-123, ADR 0265; same places, under Widescreen Mode): how the extra side area of a Level 2 / Level 3 picture looks. *Comfort* (default) shows a soft
  ambient fill taken from the picture's edges instead of the raw extension, *Borders* plain neutral panels, *Raw Extension* the extension exactly as produced. It is
  stored in the same file as `video.widescreen_side_style`, applies at once without a restart or reset, never changes the game picture, the game or a recording, and
  does not create new gameplay visibility. A frame that a verified game-specific native provider (a `.segapatch` schema 2 patch, see `widescreen-and-patches.md`) accepted is shown in full under Comfort and Raw; the product itself ships no game-specific provider.
* Display presets: **Pixel Perfect** (the untouched nearest-neighbour picture), **CRT Soft** (scanline gaps + glow), **CRT Classic** (adds the RGB
  aperture mask and slight curvature), **CRT Arcade** (stronger everything). Editing a slider makes the preset *Custom*.
* **Controls**: Player 1 and Player 2 (both consoles), keyboard and SDL3 gamepad bindings, gamepad hot-plug, device choice, analog dead zone (the left
  stick doubles as the D-pad), conflict handling (a duplicate binding moves from its previous owner and says so), reserved keys (Esc, F-keys, the menu
  key), a rebindable menu key (Esc always works), restore defaults. The listener waits for a *fresh* press: the press that activated it is ignored.
  Held inputs are released when the window loses focus and when the menu opens or closes.
* Menu button: **Back/Select** (Nintendo "-"), **Home/Guide** or **Esc** opens the menu. **Start** ("+") is a game button (Genesis Start / Master System Pause), so it does not open the menu. Nintendo controllers show their printed labels in the cues (South is the Nintendo B, East the A).
* Hotkeys: F5 quick save, F8 quick load and F7 rewind (Genesis, Master System and Sega CD; reserved, never bindable to a game button; the Sega CD frame step is F9), F11 / Alt+Enter fullscreen, F12 screenshot (the game picture only, PNG, in `Pictures/Segarecomp` or `SEGARECOMP_SCREENSHOT_DIR`).
* Capability matrix: Genesis is a three-button pad (A/B/C/Start; no X/Y/Z/Mode), two ports, no Reset. Master System has two buttons, the console's
  **Pause** button (an NMI edge, distinct from the host menu) and Reset in the System page. Audio device selection follows the OS default.
* Settings persist in `viewer-settings.cfg` (`SEGARECOMP_VIEWER_SETTINGS`); the launcher's Settings > Appearance & Controls edits the same file (it re-reads the file when a game exits and when Settings is opened, so a change made in the game's menu is never overwritten by a stale launcher copy).
* When a guest stops with a typed error the window shows why instead of closing silently (not for the automatic Optimized -> Compatibility restart).

Environment variables the launcher sets for the game: `SEGARECOMP_LAUNCHED`, `SEGARECOMP_VIEWER_SETTINGS`, `SEGARECOMP_ROM_SHA256` (the digest of the image the program was
built from), `SEGARECOMP_GAME_TITLE`, and (Genesis) `SEGARECOMP_GENESIS_REGION`. A Patched variant (see Patches and variants below) additionally sets
`SEGARECOMP_PROFILE_ID` (the existing explicit profile identity field, here the ORIGINAL library game, so Vanilla and Patched share one set of per-game widescreen preferences); the
window title names the patch. Unchanged: `SEGARECOMP_VIEWER_UNTHROTTLED`, `SEGARECOMP_VIEWER_SLICE`, `SEGARECOMP_VIEWER_MUTE`, the Master System
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

`Segarecomp --build <rom> [--mapper ...] [--run ...]` is unchanged, except that a Genesis image now takes the same automatic cartridge recognition as the window and `--mapper` also accepts `lock_on` or `standard` for Genesis (a mapper of the other platform is refused; the report states `cartridge=` and `cartridge_source=`). `Segarecomp --import <rom>...` adds ROMs to the library without building. Review aids
(not features): `SEGARECOMP_SHOT=<file.bmp>` with `SEGARECOMP_SHOT_PAGE` (`library|list|details:<n>|import|storage|settings<tab>`) and `SEGARECOMP_SHOT_SIZE=WxH`
(`SEGARECOMP_SHOT_SCALE` forces a UI scale, `SEGARECOMP_SHOT_LOG` expands the build log, `SEGARECOMP_SHOT_MENU` opens the first game's context menu);
the Master System/Genesis review hooks (`SEGARECOMP_SHOT_ROM`, `..._POLICY`, `..._MODEL`, `..._REGION`, `SEGARECOMP_GUI_AUTOPLAY`, `SEGARECOMP_GUI_TRACE`) behave as before.
`tools/launcher_ui_fixture.py` builds the fictional fixture library used for the screenshots.
`tools/launcher_acceptance_capture.py` builds the review profiles on the product and game doubles (ready, older version, failed, queued, mod, Sega CD, lock-on, error and data-relocation states) and captures them at the three review sizes; it needs Pillow and the built launcher and test doubles (review tooling only).
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

* `screenshots/launcher/` - 1080x740 and `launcher-small/` - 720x520 (the minimum): library grid/list/select and context menu, every game page state (`03*`: Genesis Compatibility, Genesis Optimized, mode switch awaiting a
  preparation, Master System mapper selection, metadata only + missing game file, failed preparation and its log, Master System ready) and the Mods / Game settings / Advanced tabs (`23`-`29`), Add game with real
  results (added, duplicate, refused), the empty library, the cleanup page and its confirmation sheet, the four Settings groups with their lower parts, Input diagnostics, About, and a real preparation in progress.
  They are captured from the **installed package** (an extracted release-layout tree, so the bundled fonts and assets are what is drawn), not from the source tree. `launcher-hidpi/` - the same layout at a forced 2x UI scale in
  2160x1480 output pixels (a simulation, not an OS HiDPI test). `screenshots/launcher-capture.json` records the captured binary SHA-256 and the exact file list.
* `screenshots/launcher-ux/` - small synthetic crops that back the findings of [launcher-ux-review.md](launcher-ux-review.md) (before/after text rendering, shell, review, Game page, Sega CD BIOS, mods, Settings, data
  location). Nothing there is a full capture set; those stay in the reviewer's scratch directory.
* `screenshots/parity/` - Genesis, Master System and Sega CD side by side (SEG-109 / ADR 0253): a ready game's Details page at 1080x740 and 720x520 and a running
  build at 1080x740, from project-authored fixtures (synthetic ROM headers, a synthetic BIOS/disc, the build and game doubles; placeholder art only). They come
  from `launcher_parity_gui_test.py` (`SEGARECOMP_PARITY_SHOTS=<dir>`), which also compares the three pages structurally (sections, reading order, columns, menus).
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

### Design record

The launcher UX overhaul (milestone SEG-150) replaced the earlier six-tab Settings, the single long Details page and the separate Import / Storage destinations with the structure described
above. What was checked on which host, every reference screen against its native equivalent (and where the product deliberately differs), the usability walk-throughs and the acceptance
matrix are in [launcher-ux-review.md](launcher-ux-review.md) and [launcher-ux-acceptance.md](launcher-ux-acceptance.md); the feature inventory that guarded against orphaned capabilities is
[launcher-ux-inventory.md](launcher-ux-inventory.md). There are no invented compatibility ratings, build percentages, storage figures or bundled commercial covers. Cover downloads happen
only on an explicit request (see *Covers and metadata provider*). **SDL_GPU is not implemented**; SDL_Renderer remains the presenter. **Audio device selection is not implemented**.


## Patches and variants (SEG-122, ADR 0266)

A Genesis game's page has a **Mods** tab (the Patches / Variants flow, per game; the buttons read *Install mod...*, *Use this mod*, *Use original game*, *Prepare selected mod*, *Play modded game*, *Remove*). A patch is an optional, verified `.segapatch` package (format: `docs/product/patch-packages.md`) that adds a
second, separately built version of the same library game. The game stays one library entry.

* **Install patch...** opens the file chooser (native dialog, or the built-in picker). The package is verified completely against the game's exact original image before
  anything is installed (ZIP structure, manifest, payload digest, source SHA-256, the delta, the patched image's SHA-256); the sheet shows the name, version, compatibility and
  the verification result. A package for another game or revision is refused with a plain message. Installing never selects Patched, never builds and never touches the ROM.
* **Vanilla / Patched** is a selection on the card. Vanilla is exactly the previous behaviour (same build identity, same program). Patched is a separate program built from the
  verified patched image; it has its own cache entry (the key adds the package identity and the patched image digest), its own build record and its own provenance. The Play
  button says **Play Vanilla** or **Play Patched** once a patch is installed; **Build patched version** / **Rebuild patched** act on the selected variant. A Patched selection that
  is not built is simply not playable: nothing falls back to Vanilla while a Patched label is shown.
* The patched image is materialized inside the build attempt's own `.partial` folder and removed again; the original ROM is only read. A cancelled or failed build never touches a
  published build (Vanilla's or the patch's).
* **Remove patch** removes the package and its record and selects Vanilla. The original ROM, the Vanilla build, widescreen preferences, recordings and saves are untouched; the
  patched build is deleted only when the sheet's checkbox asks for it (the same validated storage executor, restricted to that one build folder).
* The library file becomes schema 3 once a game has an installed patch (an earlier release opens it read-only instead of damaging it).
* A patch's presentation defaults (for example Level 3 + Raw Extension) apply automatically while its Patched variant runs and only then; installing, selecting or removing a patch never writes a preference file.
* Installing a BPS-only patch does not make the picture "authored widescreen": the Widescreen mode / width / side style settings apply to both variants unchanged.
* A schema 2 package (SEG-124, ADR 0267) may carry **executable native source** (`patch-packages.md`, `native-patch-sdk.md`). The install sheet then warns that the patch contains executable C code that is NOT sandboxed, names the author,
  license, package identity, source digest and the capabilities it reads, and installs only after the user approves it ("Approve and install"); cancelling installs nothing. The approval belongs to that exact package identity (a changed
  package is a new package), is stored in the library record and is re-checked before every build; a record without it is never built. A native-only package compiles the unchanged original image as its own variant; a combined package
  applies its BPS first. The build receives a reference to the package and the approved identity (not source, flags or capabilities) and re-verifies them. The cache key and the build record add the provider source digest, the ABI version and the
  capability set; a recorded build with another source, ABI or capability set is never reused. Only frames the provider validated are presented as authored (Comfort is bypassed for those frames only). Vanilla never contains provider code.
