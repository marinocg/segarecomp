# ADR 0120: Shared SDL3 viewer foundation and the persistent launcher library

Status: accepted (SEG-058). Supersedes the "keep the two viewers separate" conclusion of ADR 0070 only for the host-side UI; it does not touch the
console execution loops.

## Context

ADR 0070 kept the Genesis and Master System viewers separate because the code they could share (a pacer and ~tens of lines of SDL window code) was
smaller than the cost of an abstraction: runtime execution, CRAM/VDP conversion, audio and input differ. The product now needs host features that
are identical for both consoles and large: a pause overlay, quick settings, resizable/fullscreen/integer-scaled presentation, CRT presets, key and
gamepad remapping with hotplug, volume/mute, diagnostics and screenshots. Duplicating ~1,500 lines of menu, input and renderer code in two viewers
would be the larger cost. The launcher, separately, only knew "one ROM at a time" and re-derived everything from the (disposable) build cache.

## Decision

### 1. `libs/viewer`: a small host foundation, not a machine runner

* `sv_settings`, `sv_layout`, `sv_menu` are **SDL-free strict C11** (persistent settings, bindings and conflict rules, destination-rectangle/CRT math,
  the pure menu model). They have no SDL include and are covered by `sv_viewer_core_test` with synthetic values.
* `sv_host` / `sv_pages` are the SDL3 host: window, event pump, gamepads, presenter, pause overlay, screenshots, playback gain. They are strict C11 too.
* The boundary is a narrow one: an adapter calls `sv_host_service()` at its own deterministic sampling point (Genesis: once per host slice from
  `window_closed`; Master System: once per frame in `poll`, before the frame's input is recorded) and `sv_host_present()` with a finished RGB24 frame
  `{data, width, height, pitch}`. The host returns generic action masks; each adapter maps them to its own pad mask. **The guest loops, pacing,
  frame-observer, input recording and replay, audio production, Pause NMI and reset stay exactly where they were.** There is no universal machine runner.
* Host pause is a blocking call inside `sv_host_service()`: the guest simply is not advanced while the overlay is open, and nothing host-side
  (menu state, scaling, CRT, volume) reaches guest state, frame artifacts, PCM or the recorded input. The pacer resynchronizes after the pause through
  its existing bounded late-stall rule. The SMS hardware Pause button (default `P`, rebindable) is a console button; the host menu is Esc / gamepad Back.
* Headless targets never link `libs/viewer`'s SDL host (`segarecomp_viewer_host` exists only under `SEGARECOMP_ENABLE_SDL3_VIEWER`); the SMS
  equivalence test still proves the headless/runtime/core sources contain no SDL.

### 2. Why plain SDL3 drawing and no Dear ImGui in the game

The packaged route compiles the viewer **per game** with the bundled Zig in strict C11. Linking Dear ImGui into every game would add a C++ compile
(many seconds cold per game on the pinned toolchain), a C/C++ boundary and font/asset packaging to a program whose menu needs a list, toggles, sliders and
choices. The host therefore draws with `SDL_Renderer` (rect fills in the launcher's palette) and, since the corrective delivery (addendum below), a
generated bitmap font atlas (the first version used SDL's built-in 8x8 debug typeface, which was too small and ungainly for a product UI), adding a
handful of small C files and no dependency. The launcher keeps Dear ImGui (it is a long-lived C++ app that
already ships it). Both use the same palette and pixel-stepped bevel geometry.

### 3. Build-list contract (not only CMake)

The `libs/viewer/src/sv_*.c` files (the list lives in `kViewerFoundationSources` of `build_command.cpp`) are part of four independent source lists, which `tests/viewer_foundation_layout_test.py` pins together:
`apps/segarecomp/build_command.cpp` (Genesis and Master System recipes), `tools/genesis_startup_bridge.py` (`--viewer`), `packaging/install.cmake`
(shipped under `runtime/libs/viewer`) and the CMake `segarecomp_viewer_*` targets. `launcher_core` refuses a package that lacks `sv_host.c`. The cache
identity (`entry_dir`) is unchanged: the launcher version, product CLI identity, toolchain and host already key every build, and the viewer sources are
part of the release.

### 4. Video: renderer factory

`SDL_Renderer` is the only renderer. Pixel Perfect is a single nearest-neighbour copy and is byte-identical to the source picture at an integer
scale (tested by reading the output back). The CRT presets are real passes in a window-sized render target: box-filtered scanline gaps, an RGB aperture
mask (`SDL_BLENDMODE_MOD`), four-tap additive bloom, an additive brightness lift, and a barrel-warped mesh with corner falloff
(`SDL_RenderGeometry`). Four presets plus Custom are parameter sets over these passes, so no label is backed by the same effect. An `SDL_GPU` shader
path is **not shipped**: it needs offline-compiled SPIR-V/DXIL/MSL binaries from a vetted toolchain per platform, and the CPU/renderer path is already
   implemented on the software renderer. This is not evidence of real-display/GPU performance on every host. It remains a possible later enhancement; nothing in the interface precludes it.

### 5. Persistent library (launcher)

* `library.json` (per user, `<cache root>/library/`, schema 1, atomic write with `.bak`, damaged files moved aside as `.corrupt`, newer schema opened
  read-only) keyed by `platform + ROM SHA-256`. Fields: titles (detected/user), tracked ROM paths (references), per-game build options, cover file
  name, favorite, `archived`, timestamps, last run result and the **list of build identities** (the `entry_dir` names) with their provenance.
* `entry_dir()` / `entry_ready_for()` are untouched and remain the only authority for "this build is playable". A catalog record never makes a build
  playable and a metadata-only game is never treated as `entry_ready`. A build left by an earlier release is shown as *Stale* and never started.
* Existing valid cache records (`metadata.json` of a finished build) are adopted once, read-only, without inventing a ROM path.
* The library lives beside, never inside, the two folders cache maintenance may touch (`games/`, `zig-cache/`).

### 6. Storage maintenance is plan-then-execute and fail-closed

`storage.cpp` builds an explicit `Plan` (exact items, bytes, what is kept, irreversible effects), the UI shows it, and `execute_plan` re-validates every
item against the canonical cache root immediately before deleting: strict descendant check, allowed shapes only (`games/<32 hex>[.partial|.failed]`,
one level below, or `zig-cache[/x]`), no symlink components or items, expected file type, no busy entry (one `BusyGuard` shared with builds and runs,
and a maintenance lock that excludes them), no following of links inside trees (a link is reported and left). Original ROMs, covers, the catalog and
screenshots are not reachable by any plan. Tests: `storage_test` (fake trees, symlink escapes, forged plans, busy entries).

## Consequences

* The two viewers share ~1,800 lines instead of duplicating them; the console adapters shrink slightly.
* A per-game compile compiles five more small C files (a few seconds on the bundled Zig, cached by the Zig cache).
* Known limits: no `SDL_GPU` shaders; audio output device selection is not offered (SDL follows the OS default; gain/mute are); Genesis has no reset
  seam so Reset is Master System only; the launcher cannot download covers (see `docs/product/launcher-library.md`).

## Addendum: corrective delivery (hardware input, readable UI, art-led launcher)

Review of the first delivery found that a physical Nintendo Switch 2 Pro Controller did nothing, that the viewer menu used SDL's 8x8 debug font, that the launcher
was almost flat navy with a 92 px art band and algorithmic placeholder covers, and that `ci_shard_guard_test` failed when SDL3 was enabled. Decisions:

1. **One input detection engine, not two.** `sv_input_diag` (SDL3, no window) is linked by the launcher and by every game; `sv_input_state` (SDL free) holds the
   classification. A controller is classified A (no joystick) / B (joystick without a gamepad mapping) / C (open failed, SDL's error kept) / D (opened, silent) /
   E (live but game bindings broken) / F (SDL live, launcher navigation not receiving) / OK, and is never reported working from enumeration alone. An
   `SDL_INIT_GAMEPAD` or `SDL_OpenGamepad` failure is logged and shown instead of silently falling back. Raw joysticks are never reinterpreted as gameplay input; a
   user-supplied `gamecontrollerdb.txt` is the explicit route. The viewer's pad slots borrow the gamepads the engine opens (one owner of the SDL handles).
2. **Root cause of the physical failure** (details and evidence in `docs/testing/input-diagnostics.md`): SDL 3.4's Switch 2 driver is USB-only and exists only in an
   SDL built with libusb; neither the official Windows DLL nor our own Linux/macOS builds contained it. The release workflow now installs the libusb development
   package and `build-sdl3.sh` verifies (and with `SDL_REQUIRE_LIBUSB=1` enforces) that the driver is compiled in; libusb stays a run-time system library (no LGPL
   bundling). Bluetooth is unsupported upstream, and Windows keeps the official DLL (no driver binding is installed by us). Physical support remains **NOT VERIFIED**.
3. **Viewer text is a generated bitmap font** (`tools/gen_viewer_font.py`, Pillow, run offline, output committed as `sv_font_data.c`, 1.8 KB of 1-bit atlas): ProggyClean
   (MIT) for body text and Silkscreen Bold (OFL, already in the repository) for headings. Text widths and wrapping come from glyph advances, the UI scale is a whole
   number from the output height (13 px text at 1x up to 4x at 4K), nothing is loaded from disk and no font library or Dear ImGui enters the per-game C11 build.
   `SDL_RenderDebugText` is banned from `libs/viewer/src` by `viewer_foundation_layout_test.py`. Pages use stepped (pixel-chamfered) frames in the repository palette and
   the real wordmark when the assets folder is present (a Silkscreen title otherwise).
4. **Video page**: the live preview renders a centre crop of the current picture through the same CRT pipeline as the game (`SvCrtState` is now instantiated twice), so
   scanlines, mask, bloom and curvature in the preview are the real passes at a resolvable scale; the four preset buttons are real controls (mouse and menu).
5. **Launcher art**: the existing `background.png` is composed responsively (hero strip from the horizon rows, dimmed water, foliage frame) behind the whole window; a game
   without a cover shows a neutral hand-drawn cartridge with the title's initials. The design kit's `cover_placeholder.png` and the 9-slice PNGs were evaluated and **not**
   shipped: the placeholder depicts a recognisable platform-game scene (not neutral), and the kit's own README recommends code-drawn controls in the same palette; the repository's
   icons, Silkscreen and wordmark are reused as the kit requires.
6. **CI shard guard**: `tests/ci_shards.cmake` scanned only `tests/`; the SDL3 viewer/launcher suites (registered in `libs/viewer` and `apps/`) were never sharded, so
    `ci_shard_guard_test` failed whenever SDL3 was enabled. It now labels the tests of every directory (CMake >= 3.28 for the cross-directory property form).

### Viewer reconciliation

This is the UI ADR with number 0120; the separate SMS invariant-window operand ADR is unrelated.

* All wordmark/icon file reads happen during host setup. Drawing uses cached textures, including cached misses; textures are destroyed before their owning renderer.
* The libusb availability probe is cached at diagnostic-engine creation. Diagnostic drawing and hotplug summaries do not repeatedly load/unload a shared library.
  OS HID enumeration is explicit and asynchronous in both hosts. Workers return isolated result buffers; the UI thread matches them against its current SDL device
  records. Workers finish before SDL shutdown. Closing may therefore wait for an outstanding OS scan; this is not a cancellation feature.
* Start/Plus never opens or closes the host menu; Back/Select and Guide/Home are the menu buttons. SMS Pause remains a latched guest input. Audio defaults restore
  only playback gain/mute, without regenerating PCM. Destructive confirmations default to No and share the keyboard/gamepad footer.
* Wide two-column pages reduce their integer UI scale when needed to keep the rows inside the available height. Small windows scroll instead of reserving four
  rows that cannot fit. The output-size simulations cover 1x/2x/3x text and doubled logical review sizes; they are not physical HiDPI validation.
* Gallery open-failure injection is real through the diagnostic test seam, and virtual input is pressed only after SDL enumerates/opens it. Requested states are
  checked before capture. Video rows are rebuilt after applying the preview preset, so the labels and picture agree. Captures and comparison sheets use only the
  fictional RGB fixture; reference sheets stay ignored. The manifest records binary/image hashes and bounded reproduction commands.
* The atlas remains strict C11 in the CLI's two consumer recipes, the development bridge and installed runtime sources. The package ships the complete ProggyClean
  MIT notice and the existing Silkscreen OFL notice; review/test executables and test C sources are not installed as game runtime sources.

Renderer limits remain explicit: no SDL_GPU path or offline shader binaries; curvature is a mesh approximation, bloom is four additive taps rather than a
thresholded HDR/multipass shader, and the pause background is dimmed, not blurred. Audio follows the OS default; no device chooser or buffer-tuning facade.
There is no packaged HTTPS cover client and no build cancellation. See `docs/product/viewer-ui.md` for current captures and validation limits.

### Final design correction (contextual Details)

* **Navigation model.** The launcher's primary navigation is exactly Library, Import ROMs, Storage and Settings. Game Details is a contextual destination
  (card, list-row action, context menu via right click / Menu key / gamepad North) and Back restores the library's search, filters, sort, view, scroll and
  focused card. A page that is not an application area no longer occupies, or disables itself in, the sidebar.
* **Build configuration is first-class on Details.** Build mode and the Genesis region (or the Master System mapper) are large option tiles inside the first
  1080x740 viewport. The card distinguishes the selection from the executable on disk (`status.json`'s effective policy and fallback) and from a build of
  the other mode still on disk (computed by the status worker with the same `entry_ready_for` check, so a switch is shown as non-destructive only when it
  really is). Region stays a run-time choice outside the cache identity; mode, model, H-interrupt discovery and mapper remain identity inputs.
* **Art and covers.** The repository's own artwork remains the only scenery. The v2 kit's fallback backgrounds and cover placeholders were audited and not
  shipped (`apps/segarecomp-launcher/assets/manifest.json`). A game without a cover shows a code-drawn title card (tinted crop of the repository art, the
  platform, the title, "NO COVER ART"), which cannot be mistaken for identified box art.
