# ADR 0300: scheduled Luau mods in built programs, declared memory ranges, `mod diff-run` / `mod test`, and the synthetic gameplay example

- Status: accepted (combined delivery F11: SEG-136-T007, SEG-138-T008, SEG-138-T009). Implements the "linking" half that [ADR 0297](0297-genesis-mod-phase-scheduler-and-work-ram-services.md) and [ADR 0299](0299-mod-state-in-save-states-replay-suite-and-platform-conformance.md) deferred to SEG-138-T009, and the Vanilla-versus-Modded workflow of SEG-136-T007.
- Not in scope: linking function hooks or a native ABI 2 provider into a built program (a `hook.function` or native package is still refused at build); the played (SDL3 viewer) program, which keeps the HUD-only ABI 1 seam of [ADR 0296](0296-luau-only-mod-executable-integration.md); several mods in one program (SEG-141); Master System and Sega CD.

## Decision 1: memory ranges are declared in the manifest, per revision

The scheduler denies every access outside a declared range ([ADR 0297](0297-genesis-mod-phase-scheduler-and-work-ram-services.md)); until now a range existed only in a hand-built `GenesisModConfig`. Schema 3 gains an optional `memory` array (project: `[[memory]]` tables): `{revision (optional: every declared revision), space, offset, length, access = read | write | read-write}`. It lives in the package manifest, so it is part of the **package identity** and the **content digest** and a changed range is a different variant; a package without `memory` keeps its identity. The reader and `mod check` apply the same declaration check (`check_memory`): the space is `m68k.work_ram` (Genesis; other spaces are named, never accessible), the range is non-empty and inside the 64 KiB, the revision is a declared label, no range is declared twice, the entries are in canonical order (revision, space, offset, length, access), at most 16, and each access is backed by the declared `memory.read.<space>` / `memory.write.<space>` capability. Stable classes: `E_MEMORY_COUNT`, `E_MEMORY_SPACE`, `E_MEMORY_ACCESS`, `E_MEMORY_RANGE`, `E_MEMORY_REVISION`, `E_MEMORY_DUPLICATE`, `E_CAPABILITY_INCONSISTENT`.

The ranges that reach the scheduler are derived **only** from this declaration for the **exact source image digest being built** (`memory_for_revision`): the build selects the revision by the image's SHA-256, never by a title or a guess. A memory capability with no range of that access for this revision (including a range declared only for another revision) fails the build closed with `E_RANGE_DECLARATION` before any program exists. This closes the "declared-range provenance" requirement ADR 0297 left for T009. No heuristic, no inferred range.

## Decision 2: the headless scheduled path of `segarecomp build --luau-package`

`--luau-package` previously required the SDL3 viewer and linked the ABI 1 HUD seam. Without a viewer (`--sdl3-include` absent) it now links the **scheduled-mod path** into the same headless program the frame capture drives:

- `luau_artifact.c` (generated, constants only) now also carries the effective image digest, the mod's state schema and the declared ranges of the exact revision;
- `platforms/genesis/viewer/mod_capture.cpp` creates the Luau phase adapter from the verified stored artifact (identity and bytecode digest re-checked, exactly as the viewer path does), attaches the ONE scheduler (`mod_scheduler.c`) to the program's `GenesisRuntime` before the first dispatch and prints one `MOD_SUMMARY` after the run (fault state, callbacks, committed writes, replay witness, HUD overlay hash). A mod the scheduler refuses (`E_RANGE_DECLARATION`, ...) stops the program with exit 5 before the guest runs: a program never runs with part of its mod missing;
- the capture (`frame_capture.c`, compiled with `-DSEGARECOMP_CAPTURE_MOD=1` only in such a program) calls `begin`/`checkpoint`/`end` around its own run loop. Without the macro the file is byte-for-byte what it was.

The path grants exactly `host.log`, `overlay.rgba`, `memory.read/write.m68k.work_ram`, `input.read`, `input.intercept` and `state.mod` (`luau::scheduled_support`); everything else is refused with the existing `E_LIVE_*` classes (`hook.function`, a native ABI 2 provider, `frame.work_ram`, unsupported payloads). The package must be Luau-only; its effective image (typed edits, guest code, BPS) is what is compiled.

No interpreter and no JIT are added: the guest is the generated-native program; the mod is the pinned, restricted Luau VM that ADR 0294 already ships, run only between dispatches.

## Decision 3: observation reuses the existing capture; nothing is stored

Two additions to the headless capture, both opt-in by environment variable and absent from every other run:

- `SEGARECOMP_CAPTURE_RAM="<offset>:<length>[,...]"` records the bytes of up to eight work-RAM regions (at most 1024 bytes) with every captured frame, read when the frame is published (after the `frame-end` phase of the frame), and adds `ram_regions`, `ram_frames` and `final_work_ram_sha256` to `CAPTURE_SUMMARY`. Existing summaries are unchanged without it.
- `SEGARECOMP_MOD_CHECKPOINT=<frame>` performs, at the first slice boundary at or after that guest frame, a save-state round trip through the SEG-131 codec with the `MODSTATE` section (capture, restore into the same machine, capture again, compare), `SEGARECOMP_MOD_SAVE=<file>` also writes that container and `SEGARECOMP_MOD_LOAD=<file>` restores a container into a **fresh** program before it runs (a corrupted or foreign-identity state is refused with `E_STATE_RESTORE`, exit 5). The snapshot identity is the effective image digest and the package identity.

## Decision 4: `mod diff-run` and `mod test`

Both commands build ordinary programs with the unmodified product (a child `segarecomp build` per variant: Vanilla from the original image; Modded with `--luau-package`, or from the effective image of a data-only package), run them headless under the capture with the SAME scripted pad, and compare frame digests, RAM region samples and the PCM digest. Every bound is explicit: `--budget` (1..2,000,000,000 instructions), `--timeout` (1..3600 s per run), `--frames` (1..64) are required and refused with `E_BUDGET` / `E_TIMEOUT` / `E_FRAMES` before anything is built; the build has a fixed 900 s timeout; a child that exceeds a timeout is killed (`E_RUN_TIMEOUT`, `E_BUILD_TIMEOUT`); an exhausted budget or an early guest stop is `E_RUN_INCOMPLETE`, never a partial success. The capture applies a keyed pad entry at slice resolution; the commands fix the slice at 16 dispatches (reported as `slice_dispatches`).

`diff-run` reports the first frame (1-based publication ordinal) whose frame digest or RAM sample differs, the changed bytes (offset, Vanilla value, Modded value; at most 32 per region), the number of diverged frames, whether the final state or PCM digest differ, the mod's own summary and, for `--code <offset:length>`, the changed bytes of the effective image. `--aggregate` removes every byte value (counts and digests only): the form real-game use reports. `mod test <project>` reads the project's `tests.toml` (never packed, never in the identity): bounded `[[scenario]]` tables with `frames`, `budget`, `timeout`, `input`, `ram`, `checkpoint` and expectations (`expect_ram_frames` per frame, `expect_frames`, `expect_frame`, `expect_state`, `expect_pcm`); a scenario fails with the first divergence (`field`, `frame`, `expected`, `actual`) and the command exits 1 with `E_TEST_FAILED`. The platform-neutral CLI takes the compiler and the SDK directory like `mod build`; the adapter-specific part is the program the product builds.

**Fail-closed execution (final review).** The guest child runs with every inherited `SEGARECOMP_*` variable withheld unless the command sets it (`run_process(..., scrub_prefix)`; the caller's environment is untouched, and on Windows the overlay is restored afterwards), so the observation depends on the flags alone. The expected exit status of the capture seam is 1 after a completed window or an exhausted allowance and 0 after a typed guest stop or completion; any other status (3 malformed option, 4 I/O, 5 mod refused, a signal) is `E_RUN_FAILED` even if a report was printed. The report must hold the requested frame digests, RAM samples of the right size and PCM digest, and a program built with a package must report `MOD_LOADED` / `MOD_SUMMARY` (a Vanilla program must not). A `mod test` scenario with `checkpoint` passes only with an executed round trip (`status ok` and the carried state digest); on a data-only package it is refused with `E_TEST_CHECKPOINT_UNSUPPORTED`. `diff-run` is never `identical` when the mod faulted (`mod_faulted`).

**Symbol regions.** `--code @<name>[+0xN]:<length>` names an image region by a **verified** symbol of the exact revision the `--rom` image is (`E_SYMBOL_UNKNOWN`, `E_SYMBOL_NOT_VERIFIED`, `E_SYMBOL_REVISION`, `E_REGION` for a malformed or empty region); numeric regions are unchanged. `--ram` is always numeric: a symbol is an image address and is refused there (`E_SYMBOL_SPACE`); work-RAM symbols are not inferred.

## Decision 5: the example

`platforms/genesis/sdk/examples/gameplay` is the source of the public Genesis example: a fabricated, runnable fixture program (`fixture_rom.py`, 90 one-frame turns: B raises a score word in work RAM, Right raises a position word) and a Luau mod that, while C is held, adds 10 to the score (typed, declared-range RAM access) and draws a HUD counter, saving and restoring its own counter. Its tests (`tests/mod_gameplay_example_test.py`) derive the per-frame RAM and the final state independently of the program.

## Consequences and limits (stated, not hidden)

- The mod runs in the **headless** program (capture, `diff-run`, `mod test`). The played SDL3 viewer program still links the HUD-only seam: a package that needs memory or input capabilities is refused there (`E_LIVE_CAPABILITY`) and the launcher's executable cache does not select it. Playing a gameplay mod in the viewer is a continuation (viewer-side scheduler wiring and presenting `overlay()` while the scheduler reports no fault).
- The overlay is hashed (`MOD_SUMMARY.overlay_sha256`) but is not drawn into the captured VDP frames; frame digests therefore do not differ for a mod that only changes state and HUD.
- PCM: `expect_pcm` / `pcm_sha256` is the mixer digest the existing sound summary reports for the whole run; a Genesis program without sound hardware activity reports the silent digest.
- Hooks and native ABI 2 providers are not linked (refused with `E_LIVE_CAPABILITY` / `E_LIVE_ABI`). `memory` ranges exist for the Genesis work RAM only.
- The Windows CI host does not build the Luau-linked program tests (skipped like the other generated-native build tests); Linux and macOS do.
- The support matrix is **not** raised: it counts the played program and `memory-access`, `input-modifications` and `luau-scripting` stay at their levels until the viewer wiring above lands.

## Rejected alternatives

A second Luau runner for tests (the point is the scheduler everyone shares); running the program in-process from the CLI (a guest must be a killable child); defaulting the budget or timeout (a test that can hang is not a test); recording raw traces or RAM dumps (only digests and a bounded changed-byte list are reported, and `--aggregate` drops the values); deriving ranges from symbols or titles (a range is a declaration the author signs for one revision).

## Verification

See the final handoff of the combined PR (`F11`): `mod_diff_run_test`, `mod_gameplay_example_test`, the extended `mod_package_test`, `mod_project_test`, `mod_workflow_test`, `luau_*`/`genesis_mod_*` suites and the fast gate. Real-game validation (SEG-138-T008) is local and aggregate only; its classification is in the harness task record, not in this repository.
