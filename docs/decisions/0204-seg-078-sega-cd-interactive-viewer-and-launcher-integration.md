# ADR 0204: SEG-078 - the Sega CD in the shared viewer, live input and its recording, and the launcher's Sega CD game

- Status: Accepted (SEG-078-T001 and SEG-078-T002, delivered together in one product pull request).
- Scope: presenting, pacing and playing a generated-native Sega CD BIOS-image program interactively; recording and replaying the player's input;
  a Sega CD game in the launcher (inputs by reference, imported verified program, honest states). Out of scope: audio, generic CUE-to-play
  compilation (SEG-078-T003), new executable extents, other titles, any change to what the headless program computes.
- Privacy: no BIOS, disc, program, frame, trace, address or digest of commercial content appears here or in any test; the fixtures are project-authored.

## Context

The Sega CD runtime (ADR 0128 and following) has its own scheduler (`ScdMachine`, deadline slices, the vertical-blank event `vdp_frames`); the
Genesis runtime inside `scd_vdp.c` is only a device host. Until now a program only produced a headless JSON report and at most eight captured
frames. The Genesis and Master System viewers share an SDL3 host (`libs/viewer`: window, controller mapping, settings, pause menu, presenter) and
the launcher persists games as `platform + ROM SHA-256` with a build cache. Sega CD has neither a single ROM nor a build the launcher can run.

## Decisions

### A. The live driver (SEG-078-T001)

1. **One frame per host step.** `scd_live_run` (`platforms/sega-cd/runtime/scd_live.c`, strict C11, no SDL) advances the machine from the stop
   boundary before one vertical-blank onset to the boundary before the next, using the machine's own `scd_machine_run(deadline)`. It is a host
   loop over the existing scheduler, not a second runtime and not an interface forced on `GenesisRuntime`. A host (the SDL3 viewer or a test
   double) supplies `poll`, `present`, `pace`, `finished`.
2. **Input is stamped at a tick the machine has not reached.** `scd_machine_run(m, T)` executes only items that start strictly before `T`, so at the
   boundary `T` = the upcoming onset tick nothing at or after `T` has run. The driver samples the host pad there and appends the *changes* to the
   timed script (`scd_input_live_sample`) at tick `T`; the ordinary ADR 0149 machinery applies them before every other device event and instruction
   at or after `T`. The recorder refuses a tick that is not greater than the machine time (`m->now`) or than the last recorded tick (so a tick
   never touches a button twice), writes releases before presses in the grammar's button order, and emits no redundant transition. The recording is
   therefore the exact tick list the machine applied, and `--input-script <recording>` reproduces the run: report, input counters and read-log digest,
   and captured frames are equal (tested with the live run and the replay under different deadline slicing).
3. **Recorder limits are the grammar's.** 65,536 bytes, 256-byte lines, 4,096 events, port 1, no MODE/X/Y/Z. The byte limit always binds first (even
   the shortest line makes 4,096 events exceed 64 KiB). An append is all or nothing; at the limit the run stops with the typed outcome
   `recording_capacity` (program exit 25), the valid prefix is written, and the replay of that prefix reproduces the live run to the stop instant.
4. **Pause and single step touch the host loop only.** While held, `poll` is called repeatedly and nothing else is: no tick is assigned, no device
   event runs. A pad change while paused is stamped when the machine next moves. A run with held/stepped polls equals the run of the polls that
   advanced (report, recording, frames).
5. **Continuous completed-frame presentation.** `scd_vdp_present_enable` makes every frame a capture target: the existing observer instants sample
   each active line's VDP state, and the frame is rendered once at its onset with `genesis_vdp_produce_frame_raster`. It changes nothing the
   machine computes (device log and onset count are equal with and without it; measured host cost: about 1.8 ms per frame at `-O1`, in
   `sega_cd_live_table`). The first onset ends the power-on run-in, whose first lines were never sampled: that frame is counted *skipped*, never
   approximated, so presented frames = onsets - 1 and the report says so (`live.skipped`).
6. **Pacing follows the machine's frame length.** The driver passes `scd_frame_ticks()` (262 lines x 3,420 master clocks x 500,000) and
   `SCD_TICKS_PER_SECOND` to the host; the Genesis pacer gained `genesis_pacer_wait_period`, which reduces the exact fraction by its gcd and keeps the
   exact rational accumulation (59.92 Hz, not 60.0; a period that does not fit is refused, never rounded). `genesis_pacer_wait` is the same core
   with the Genesis constants and is unchanged.
7. **The viewer variant.** The unmodified generated C is compiled with `-Dscd_program_main_bios=scd_viewer_main_bios` and linked with
   `platforms/sega-cd/viewer` (the SDL3 host registered with the driver), the Genesis viewer's presenter and pacer and `libs/viewer`
   (`sega_cd_bios_build.py --viewer`; typed failure `sdl3_unavailable`, never a silent headless fallback). Every unit object is reused; the program
   identity and the headless `program` are unchanged. Keyboard and gamepad are the Genesis viewer's mapping and settings (`SV_SYSTEM_GENESIS`);
   Esc / Back opens the shared pause menu, F6 holds the machine and F7 steps one frame while held (peeked from the SDL queue). A test seam
   (`SEGARECOMP_VIEWER_KEY_SCRIPT`, like the launcher's GUI script) pushes key events so the real mapping is exercised without a person.
8. **Program surface.** `--record-input FILE` exists only in a viewer variant; `--input-script` in a viewer variant is a replay (the host pad is not
   sampled); `--describe` (all Sega CD programs) prints the program's own statement and runs nothing (decision B3). Exit statuses added: 24 host
   failure, 25 recording capacity, 26 recorder refused, 27 recording not writable; leaving by the window's close is 0. The report gains a `live`
   object only in a viewer variant; every headless report is unchanged.

### B. The launcher's Sega CD game (SEG-078-T002)

1. **A distinct platform, inputs by reference.** `platform_id = "sega-cd"`. An entry holds the BIOS path, size and SHA-256, the CUE path, size and
   SHA-256, the disc files named by the program (name, size, SHA-256), and the imported program (path, size, modification time, SHA-256, identities,
   runtime contract, viewer flag). No BIOS, disc or program byte is copied or stored; the program stays where the developer procedure put it.
2. **Entry identity (version 1).** `sha256 = SHA-256("segarecomp-sega-cd-entry-1\nbios <BIOS SHA-256>\ncue <CUE SHA-256>\n")`. A different BIOS (a
   program is built for one BIOS) or a different CUE sheet is a different game; the program is bound to the entry, not part of its identity. A record
   whose key is not the identity of its stored digests, or whose identity layout is unknown, is dropped on load - a damaged or hand-edited record is
   never half-trusted.
3. **What the product can verify.** The program is a user-supplied native executable, so nothing signs it. Verified: the BIOS file (size <= 128 KiB,
   SHA-256, measured); the CUE file (digest only; **no CUE parser in the launcher**); the program file (SHA-256, size and time at import, SHA-256 again
   before every launch); and the program's own statement (`<program> --describe`: runtime contract, viewer or headless-only, program and disc
   identity, the BIOS size and SHA-256 it was built from, the disc files by plain name, size and SHA-256). The disc files are looked up **beside the
   CUE sheet** under the names the program states and measured (digests at import and on "Verify again", sizes at every status refresh and launch;
   the program itself re-verifies the BIOS and every disc file before the first guest instruction and answers with its typed exits 20/21). "Verified"
   therefore means: unchanged since import, built for exactly these two files, speaking this launcher's contract - not that it faithfully implements the
   disc (that is the build-time evidence of the developer procedure). There is no manifest to edit; a program that cannot state itself is
   `program_unverifiable` (an older build rejects `--describe`), never trusted.
4. **Contract version.** `SCD_RUNTIME_ABI` (= 1) versions the host-facing contract (command line, exit statuses, `--describe`, report fields). The
   launcher accepts exactly the versions it knows; any other is `program_abi_unsupported` (stale).
5. **States and typed refusals.** Inputs not configured (`inputs_missing`, `bios_missing`, `cue_missing`, `disc_file_missing`); no program (the automatic
   build is `unsupported_build`: the window and `--sega-cd` say it plainly and offer "Import program..."; `request_build`/`start_build` return a typed
   refusal and never fall through); headless-only build (`program_headless_only`); stale (`program_file_missing`, `program_changed`,
   `program_abi_unsupported`, `program_unverifiable`); mismatched (`bios_changed`, `cue_changed`, `program_bios_mismatch`, `program_disc_mismatch`,
   `disc_file_size_mismatch`, `disc_file_changed`); ready to play; running; and the program's typed end (a stop class from its report, or its refusal
   exit). The same checks run at import (a failing program is refused with its code; the game itself is still added) and before launch.
6. **Catalog schema.** A library without a Sega CD game is written as schema 1, byte for byte as before (a pre-change fixture is re-saved identically);
   the first Sega CD game makes it schema 2, which an older release opens read-only (never damaged). The `sega_cd` member exists only on Sega CD records.
   Re-adding a game keeps its imported program. There is no migration of earlier records.
7. **Running.** Play reuses the launcher's process orchestration (`GameRun`, log file, exclusive-device handling, busy guard, failure banners,
   diagnostics). The command line is `--disc-dir <CUE directory> --bios <file> --instruction-budget 2^50 --backup-ram <file> --report <file>` plus
   `--record-input` or `--input-script`. The BIOS reads the console's internal backup RAM and the program stops with a typed stop without an image, so
   the launcher writes the state of a new console - 8 KiB, zero except the public 64-byte format trailer (ADR 0147; byte-identical to
   `tools/sega_cd_backup_ram.py`), no software-derived byte - into the game's run folder; the program does not write it back, so saves are not persisted yet. The "no window" replay runs the viewer variant under SDL's dummy drivers, unthrottled, and reports the result. Recordings live
   in the launcher's library folder (`sega-cd/<id>/recordings/`) and contain only button transitions.
8. **Packaging.** Nothing of a Sega CD game is bundled: the install rules name no Sega CD runtime source, program or disc, and the package verifier
   (`packaging/dependency_check.py`) refuses BIOS, CUE/disc image, generated program, `--describe` sidecar and Sega CD runtime file names.
9. **Rejected.** A launcher-side CUE parser (an untrusted-input parser for no gain: the program already names its track files); trusting a
   manifest or a build summary; copying the program into managed storage; forcing the Sega CD scheduler into `GenesisRuntime` or a universal machine
   interface; sampling the pad at arbitrary host times (a recording could then not be exact); a hard-coded 60 Hz.

## Consequences and limits

- Audio is not part of the viewer (the Z80 and the PCM/CD-DA path are unmodelled: contract section 12); the panel says so.
- A program reaches only the region its build admitted; other modules stop with a typed stop (for the checked Sonic CD program, after the first level).
- The viewer's frame is only as good as the VDP model: frames of modes the renderer does not support are counted `rejected`, not drawn.
- The F6/F7 hold and step keys are keyboard-only; a gamepad has the shared pause menu.
- Generic CUE-to-play compilation, executable-extent discovery and a supported build API are SEG-078-T003.

## Result (authorized local smoke, classes and counts only)

With an authorized local BIOS and the Sonic CD (USA) program built by the local recipe of SEG-079 with `--viewer` (the same fixed point as the headless
program: 3 rounds), run through the launcher (`--sega-cd` and the window's Play button) under SDL's dummy video/audio drivers with scripted keys at the
frame boundaries of the build's own input schedule (START, START, RIGHT): the viewer ran 2,780 frames and presented 2,778 (one power-on frame skipped,
one frame the renderer refused - an unsupported VDP state, counted `rejected`), closed cleanly (`Closed.`), and recorded exactly the six transitions of
the build's schedule. The headless replay of that recording (`--input-script`, same instruction stream, `--until-tick` at the closing boundary) produced a
report equal in every field (registers, memory and VDP digests, input counters, ticks), and eight frames captured at chosen onsets across the title,
both START presses, the level and the RIGHT hold were equal between the live run and the replay (seven pairwise distinct; the frames before and after the RIGHT
press differ). No frame, digest, address or trace is recorded. This is a local, ephemeral procedure; the durable evidence is the synthetic fixtures.

## Evidence

`sega_cd_live_test` (recorder, pacer period, presentation counts; record/replay equality incl. slicing, pause/step, quit, capacity, host failure),
`sega_cd_viewer_sdl_test` (the real SDL3 viewer under dummy drivers), `launcher_sega_cd_test` (identity, statement, verification, refusals, import,
catalog and its byte-compatibility), `launcher_sega_cd_flow_test` (launcher -> verified program -> viewer -> keys -> close/typed stop),
`launcher_sega_cd_gui_test` (the window), `packaging_commercial_content_test`, plus every earlier launcher, viewer and Sega CD suite unchanged.
