# Changelog

Releases are published on the [Releases](../../releases) page. This file records what changed between them. Per-game and per-feature status is in the
[compatibility ledger](docs/product/compatibility-ledger.md); the operator checklist for cutting the next release is [docs/product/release-readiness.md](docs/product/release-readiness.md).

## Unreleased (on `main` since v0.7.0, DRAFT release notes)

> Draft for the next release. The version number and the publication are the operator's decision; nothing here has been tagged or published.

### Widescreen through 32:9: rational aspect planning, bounded surfaces (ADR 0301, Genesis)

* New optional setting `video.widescreen_ratio=N:M` (4:3 through 32:9; presets 4:3, 16:9, 21:9, 32:9, or any custom ratio in range) makes the Genesis Level 2/3 extension produce and show exactly that aspect. A request that does not fit the supported surface is **rejected** (nothing extra is shown), never silently reduced. Unset (the default) leaves every existing setting, saved file and 16:9 picture unchanged; a previous release simply ignores the new line.
* The maximum extension per side rises from 256 to 672 source pixels (1664 x 448 surface at most); a latent scratch-size overflow in the side treatment that the larger bound would have exposed is fixed.
* Measured, not optimized: the 32:9 producer costs about 10.6% of a 60 Hz frame on the reference host; the pre-registered lookup-optimization criterion was not met, so no cache was added ([measurement](docs/architecture/widescreen-scenery-lookup-measurement.md)). The exact ratio is selectable under Widescreen Aspect (ADR 0302).

### Honest geometry declarations, per-scene coverage, and the aspect matrix (ADR 0302)

* A schema 3 package (and `mod.toml`) can declare the aspect range it supports (`aspect_min`/`aspect_max`, inside 4:3 to 32:9) and per-scene components (`scenery`, `hud`, `actors`). The declaration is a claim, never a grant: it is part of the package identity, not the content digest; packages without one are byte-identical to before.
* The viewer compares declared with delivered per scene and component and reports it as stable JSON (`SEGARECOMP_COVERAGE_REPORT`). A provider that declares 16:9 and is shown at 32:9 reports `outside declared range`: only the native picture is shown (no provider scenery, no relocated HUD) with a visible notice. Background coverage is never reported as gameplay support, and a declared `actors` component stays `missing` until a provider can signal actor delivery explicitly. A verified `--luau-package` build compiles the declaration into its program (recorded in `status.json`; the environment cannot loosen it). The Video page and the launcher gain a **Widescreen Aspect** choice (Slider, 4:3, 16:9, 21:9, 32:9) and the Video page states the package's range and what was delivered. A provider or Luau mod names its scene (`GenesisCompatDecision.scene`, `sega.scene(id)`) so coverage is counted per declared scene; a custom aspect can be chosen (Custom Aspect), and an ignored saved aspect or a planner rejection is now shown instead of silent (ADR 0302).
* A synthetic 4:3 / 16:9 / 21:9 / 32:9 matrix runs in CI (golden frame hashes, declared-range coverage, coarse time and memory budgets, a producer-cost guard); the table, the cost against 16:9 and what 32:9 does and does not cover are in [the matrix document](docs/architecture/widescreen-aspect-matrix.md).

### Gameplay mods in a built program, `mod diff-run` / `mod test`, a synthetic gameplay example (ADR 0300, Genesis)

* A Luau mod can now run all of its phases (including bounded work-RAM reads/writes, input reads and its own saved state) inside a **built, generated-native program**: `segarecomp build --luau-package` without a viewer links the one mod scheduler. The memory a mod may touch is declared in the manifest (`[[memory]]`, per revision, part of the package identity) and is the only memory it can touch; a build with a memory capability but no range for the exact image is refused. The played viewer window still runs only the HUD seam, and function hooks / native providers are not linked yet.
* New `segarecomp mod diff-run`: runs Vanilla and Modded with the same scripted pad and reports the first frame at which the picture or a watched RAM region differs, the changed bytes, and the mod's summary (`--aggregate` drops byte values). New `segarecomp mod test`: a project's bounded `tests.toml` scenarios (hand-derivable per-frame RAM, frame, state and PCM expectations, an optional save/restore checkpoint); a broken expectation fails with its first divergence. Every run needs an explicit instruction budget and timeout.
* New redistributable example `platforms/genesis/sdk/examples/gameplay` (fabricated fixture program, no commercial input): holding C adds a score bonus through typed RAM access and a HUD counter shows the grants. CI builds, tests, packs and installs it and checks replay and save/restore identity ([page](docs/product/gameplay-example.md)).

### Mod state in save states and rewind, replay regression suite, platform conformance suite (ADR 0299, Genesis)

* A mod's own state now travels with Genesis save states and rewind history: the new `MODSTATE` section holds the mod's identity, state schema, disabled/fault state, input latch, replay witness, any hook call in flight and the mod's bounded blob (native `save_state`/`load_state`, or the Luau `save()`/`load()` value). A state saved with other mods, another version/schema/capability set, or no mod is refused (`E_STATE_MOD_MISMATCH`) and the game is untouched; a mod that refuses its saved state refuses the whole restore (`E_STATE_MOD_REJECTED`); a mod that changes the game without declaring `state.mod` makes saving unavailable (`E_STATE_UNAVAILABLE`) instead of saving something wrong.
* New regression suites: recorded-input replay produces identical state hashes (native and Luau), sandbox failures during replay (range, Luau budget, script error) are deterministic and replay identically across a restore, and incompatible or corrupted states are rejected without partial application. One table-driven conformance suite runs the same lifecycle, capability, state and failure scenarios for every registered platform and asserts each platform's declared support or its exact fail-closed diagnostic (Master System and Sega CD assert `unsupported` until their milestones flip them).
* Still not in a built program (SEG-138-T009); Master System / Sega CD containers are later milestones.

### Static function hooks, deterministic callback ordering and transactional failure (ADR 0298, Genesis)

* A mod can now declare hooks on verified symbols (`[[hook]]`: `before`, `after`, `before-after`, `replace` with a declared register/flag/cost contract). The build resolves them against the exact ROM digest and classifies every site against the compiled program; anything it cannot prove fails the build with a stable `E_HOOK_*` class (mid-instruction, not a function entry, tail jumps / RTE / computed returns, computed targets, work-RAM code, interrupt context, overlap, bad contract, unsupported CPU).
* The feasibility spike returned **GO** for the Genesis M68K: a no-op hook leaves the guest bit-identical to the unhooked program over 600 frames (including nested calls), ten active hooks with trivial callbacks cost within about 5 % of the frame time, and a program without hooks is unchanged byte for byte. Hooks are not yet reachable from `segarecomp build` (SEG-138-T009), so the user-facing support level is unchanged.
* One mod's callbacks run in a defined order (`frame-begin`, `input`, `post-input`, hook entries in call order and exits in reverse, `frame-end`, `render`), see the deterministic cycle counter, and fail transactionally: a faulting callback's writes are discarded, a faulting replacement runs the original function, the mod is disabled and the guest continues unmodified.

### Mod phase scheduler, input interception and bounded work-RAM access (ADR 0297, Genesis)

* The Genesis runtime can now run mod callbacks at the documented phases (`reset`, `frame-begin`, `input`, `post-input`, `frame-end`, `render`) through one deterministic scheduler used by native ABI 2 providers and by Luau. Callbacks run only between dispatches, take zero guest time and `frame-end` runs before the frame is rendered; `render` is read-only. The `input` phase can replace the pad the game reads; the physical pad stays what recordings store.
* Mods can read and write declared work-RAM ranges (1/2/4 bytes, big-endian, even-aligned for words) in the legal phases only; writes are staged and committed atomically, every committed write is logged. Any violation (undeclared capability, illegal phase, out-of-range, odd word access, budget) faults only that mod with a stable `E_MOD_FAULT cause=...` diagnostic and the game continues unmodified. Luau gained `frame_begin/input/post_input/frame_end` callbacks, `sega.mem.read/write` and `sega.input.get/set` with results identical to a native provider.
* Not yet in a built program: linking the scheduler into a generated-native executable, function hooks and mod state in save states are later steps. Without a scheduler the runtime is unchanged.

### Luau for mods: read-only API, static check, error diagnostics, a Luau-only HUD example (ADR 0295)

* A deliberately small, **read-only** Luau API: `sega.log`, `sega.frame.value` (values the host publishes for a frame, copied in), `sega.overlay.rect/text` (requests drawn into the fixed 128x256 HUD overlay) and the optional `reset()` / `render(frame)` callbacks. Every binding is gated by the capability the package declares (`host.log`, `frame.work_ram`, `overlay.rgba`); arguments are validated, geometry is clamped, rates are limited, and a script that misbehaves **faults and disables only itself** (stable `E_MOD_FAULT cause=...` diagnostic, nothing it staged is kept). There is no memory access, input, clock, file or network.
* `segarecomp mod check/pack/build` reject an undeclared capability, an unknown API name and visible argument errors as `<file>:<line>: <message>`. `segarecomp mod api` prints the machine-readable API list and the generated `.d.luau`; `mod init --template luau` adds the declarations and VS Code (luau-lsp) settings.
* A Luau-only package now runs **in the played game** (ADR 0296): `segarecomp build --luau-package` (and the launcher's executable cache, which keys the program on the package identity, the effective image and the compiler pin) links the verified compiled scripts into the generated-native Genesis program, which calls `render(frame)` every presented frame and draws the requested HUD over the picture. The live path grants `host.log` and `overlay.rgba`; anything else is refused at build. A script fault disables only the mod. The committed example `platforms/genesis/sdk/examples/luau-hud` (with a tiny runnable fixture ROM written for the repository) is built, installed (consent sheet, no native approval) and run end to end by CI on Linux, with the overlay checked in a genuinely rendered frame.
* `segarecomp mod session` remains as an isolated **diagnostic** (synthetic picture; not the Genesis machine).
* A bounded, canonical state seam (`save`/`load`) and an adversarial test corpus (memory/CPU bombs, metatable and global escapes, coroutine and loader abuse, malformed state, determinism, isolation). Restricted and limited, **not** "safe".

### Luau for mods: pinned build, restricted per-mod VM, trusted bytecode compile (ADR 0294)

* Luau 0.746 is vendored unmodified (VM, compiler, AST; **no native code generation / JIT** is in the repository or the binary) with its license texts shipped in the package. Mod scripts are still **source only**: `segarecomp mod build` and `mod variant` now compile them with the pinned compiler, report a syntax error as `<file>:<line>: <message>` (`E_LUAU_SYNTAX`) and print the bytecode digest, which is part of the variant identity. A package that carries bytecode is rejected, and compiled output that is not what the build produced is never loaded (it is rebuilt from source).
* A restricted VM per mod (library whitelist, read-only globals, deterministic `math.random`, a hard memory quota, a deterministic instruction budget, a call-depth limit, a hang watchdog, one disabled mod never affecting another) exists in the host library. It is "restricted", not "safe".

### Guest code injection, header repair and BPS construction for mods (ADR 0292, ADR 0293)

* A mod can now carry `guest-asm`: lower-case M68K or Z80 assembly assembled by a small built-in assembler (no external tool, no includes) and injected into declared, asserted-free ranges of the exact original image. The emitted code is validated first - privileged instructions, traps, port I/O, interrupt control, raw numeric branch targets and stores outside RAM are refused with stable `E_GUEST_*` classes - and overlapping blocks, free ranges and edits are refused.
* `segarecomp mod apply` builds the verified target image (edits, injected code, Genesis checksum repair per an explicit `[build] header_repair` declaration); `mod bps` writes a deterministic BPS proved by applying it back; `mod verify-bps` and `mod pack --rom` verify a BPS or every declared image.

### Native GEMS audio replacement (new, automatic, Genesis; ADR 0291)

* Games whose Z80 sound driver rewrites its own code are silent on the original path. For one verified driver image - **GEMS 2.5**, uploaded by Aladdin, Earthworm Jim, Jurassic Park and Cool Spot - `segarecomp build` now **selects by itself** (no option, nothing to tick) a program where a **native C11 re-implementation** of the driver plays the game's music and effects - when the build's own verification run shows that the original generated-native Z80 cannot run that driver and the uploaded image is a verified GEMS image. Every other game is built exactly as before. It is not a Z80 emulator and never executes the original driver's code.
* **The driver's whole command and sequence-event set** (FM, PSG, noise and digital patches, envelopes, tempo, loops, pitch bend, looped / slow / compressed samples), compared operation by operation with an independent reference implementation over long runs of all four games. Only malformed data stops it (explicit fault, silent afterwards); an unverified driver is never replaced. **Save states and rewind work while it plays**, with an exact continuation of the music and samples.
* Build metadata (`status.json` `audio_implementation`, schema 2) records the requested and actual sound implementation, that the selection was automatic and why, the replacement's implementation version and catalog revision and that save states are supported; the launcher's Build provenance has an **Audio implementation** row built from that record only (*Native GEMS 2.5* with its details, or *Original Z80 (generated-native)*, with a degraded original disclosed separately).
* Developer overrides `--sound-driver original|gems` exist for comparison; the launcher offers none.

### Widescreen (new)

* A shared, presentation-only widescreen setting for Genesis, Master System and Sega CD: **Original**, **Extended Backgrounds (Level 2)** and **Enhanced Widescreen (Level 3)**, with a **Widescreen Width** slider from 4:3 to 16:9,
  **Comfort / Borders / Raw Extension** side treatments and per-game saved settings, all changeable live from the pause menu and the launcher settings.
* Honest capability reporting: Level 2/3 are often **Partial**. Master System's extra columns repeat scenery already on screen; Level 3 draws only sprites the game already submits and does not make a game create objects outside its original window.
* A program built with an earlier release keeps playing in Original and needs a rebuild to gain widescreen.

### Game patches and the native patch SDK (new, Genesis)

* `.segapatch` packages: **schema 1** (verified data-only BPS) and **schema 2** (a **trusted native C11 provider**, optionally with BPS), installed from a game's **Patches / Variants** card as a separate **Patched** variant beside **Vanilla**.
* A small public **provider SDK** (`segarecomp_genesis_patch.h`, ABI 1) lets a package reconstruct scenery from the game's own data and redraw its original HUD. The SDK header, its documents and a synthetic sample ship in the release package under `sdk/`.
* Packages are bound to the exact game image and the exact approved package identity. Native patches are **executable code, not sandboxed**; hashes prove integrity, not authorship or safety.
* Game-specific packages (Sonic 1, Sonic 2, Sonic 3 & Knuckles, Streets of Rage 2) are maintained outside this repository, are **not included in this release**, and are described in the ledger; the maintainer states their extended scenery and widescreen were independently validated in all levels.

### Save states and rewind for Master System and Sega CD (new; ADR 0279)

* The Genesis workflow below now also works on **Master System** and **Sega CD**: five numbered slots plus a quick slot (**F5** / **F8**), the same pause-menu rows, and the visual rewind on **F7** with the same defaults (a moment every 500 ms of game time, 96 MiB, oldest dropped first).
* Each console owns its complete state format (Master System: CPU, mapper, VDP, renderer, PSG; Sega CD: both 68000s, RAMs, Gate Array, CD controller and drive, PCM, CD-DA, mixer, graphics ASIC, backup RAM in memory, VDP, Z80, YM2612, PSG). A Sega CD state is bound to the disc, the BIOS and the program's image set; a Master System state to the cartridge and the declared mapper. Refused states never touch the running game.
* Master System cartridge RAM is in every state but is **not** saved between sessions (the board contract cannot identify battery backup). Sega CD backup RAM: a restore changes only the copy in memory; the file is still written only when a session ends cleanly (ADR 0252, unchanged).
* A Sega CD moment is about 1.1 MiB, so the default 96 MiB keeps about **43 seconds** (not 60) of rewind; the rewind screen reports the real figure.
* **Changed key:** the Sega CD viewer's single-frame step moved from **F7** to **F9** (F6 still holds the machine); F7 is rewind everywhere.

### Save states, game saves and rewind (new, Genesis; ADR 0278)

* **Automatic game saves:** a Genesis cartridge's SRAM or X24C01 EEPROM is kept between sessions, written when you end the session (Return to Launcher / Quit / close the window) and never after a crash, a typed stop or an instruction limit. Saves live in the library (`saves/genesis/<image sha256>/vanilla|patches/<id>/`), separate for a patched version, and survive rebuilds, cache cleanup and removing the game from the library.
* **Save states:** *Save State* / *Load State* in the pause menu (five numbered slots with a picture and play time each, confirmation before overwriting or loading), **F5** / **F8** for a quick slot. A state is the whole machine, including the sound chips; loading continues sample for sample. A failed save never damages the previous state; a failed load never changes the running game; a state from another game, version, region or build is refused with the reason.
* **Rewind (F7 or the pause menu):** browse the last minute of play as pictures on a timeline (a moment every half second, about 120 in all, in memory only, at most 96 MiB), pick a moment and continue from it; cancelling changes nothing. Gamepad (both triggers held, shoulders, south / east buttons), keyboard and mouse all work.
* (Master System and Sega CD gained the same states and rewind in ADR 0279, above.) The Sega CD backup RAM behaves as before. See [docs/product/save-states.md](docs/product/save-states.md) for the guarantees and limits.

### Genesis

* Automatic recognition of the Sonic & Knuckles lock-on cartridge images (exact header contents only; manual override in Advanced build options; `--mapper lock_on` on the command line).
* Opt-in `--z80-static 1` for `segarecomp build`: reconstructs provable Genesis Z80 images before the first Z80 compilation (ADR 0277). Off by default, not exposed in the launcher, not a guarantee for any title.
* Optimized AOT evaluation for cartridges that run code from RAM thunks, and static RAM `JMP` thunks (ADR 0270, 0272). The changed interrupt-delivery behavior of the latter has no independent Genesis-level reference.

### Performance and diagnostics

* **F3** performance overlay (Off / Player / Developer), profiling reports and benchmark tools (ADR 0273).
* Throughput work (ADR 0275, 0276): measured on one development host, the software-rendered viewer ran about 20% faster unthrottled and builds were about 10-14% faster. Guest behavior and frames are unchanged. These are not guarantees on other machines.

### Documentation

* The README, platform pages and Sega CD notes now describe what is actually implemented (generated-native Genesis Z80, Master System, Sega CD audio and saves, key remapping) and how strong the evidence is.
  Human-reported completions of Sonic CD and Earthworm Jim Special Edition are recorded as **maintainer-reported**, not as independently validated runs.

### Known limitations

* Compatibility is not universal; unsupported behavior stops with a typed diagnostic.
* Sega CD: complete CUE-to-play compilation is not claimed; FM/PSG output is not reference-checked.
* Genesis is a three-button pad; PAL runs at 60 Hz.
* The full patch-to-executable build test runs on Linux CI only; Windows and macOS validate compilation of the SDK sample, packaging and the launcher.
* Packages are not signed or notarized. macOS needs right-click > Open on first launch; Linux needs glibc 2.35 or newer.

## v0.7.0 (2026-11-01)

Sega CD becomes an interactive platform in the launcher: authentic BIOS execution, Sonic CD playable, shared viewer with input recording and replay, self-service and File-first builds, CD-DA, RF5C164 PCM,
main-side Z80 with FM/PSG, persistent internal backup RAM and unified launcher integration. See the [release page](../../releases/tag/v0.7.0).
