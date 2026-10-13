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
choices. The host therefore draws with `SDL_Renderer` (rect fills in the launcher's palette) and `SDL_RenderDebugText` (SDL's built-in 8x8 pixel font,
integer-scaled), adding five small C files (about 1,800 lines) and no dependency. The launcher keeps Dear ImGui (it is a long-lived C++ app that
already ships it). Both use the same palette and pixel-stepped bevel geometry.

### 3. Build-list contract (not only CMake)

The five `libs/viewer/src/sv_*.c` files are part of four independent source lists, which `tests/viewer_foundation_layout_test.py` pins together:
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
complete and fast on the software renderer. It remains a possible later enhancement; nothing in the interface precludes it.

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
