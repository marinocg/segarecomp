# Native GEMS replacement - validation (ADR 0291)

Commercial images are used locally and ephemerally (`docs/testing/commercial-games.md`): nothing below is a byte, address, trace, asset or disassembly; it is counts,
classes and pass / fail. CI never depends on them. The committed tests use synthetic data only (see section 6).

## 1. Identity and version

The four authorized local images - Cool Spot (primary), Aladdin, Earthworm Jim, Jurassic Park - upload a Z80 image with the **same activation signature**. The
signature covers the whole 8 KiB written extent, so the four games carry one and the same driver binary. Version 2.5: the address map, the interrupt / FIFO / DAC-routine
layout, the frequency tables, the sub-beat default and the position of the patch buffers match the published 2.5 driver's; the inline (not factored) delay / duration decoding
excludes 2.8, the stack initialisation and the compressed-sample hook exclude 2.0. No byte-exact original binary was available, so the evidence tier is
`address_map_and_tables` and the version is not claimed beyond it. Each game's *resources* (patch, envelope, sequence and sample tables) differ and are read from its own cartridge.

## 2. AOT-versus-replacement decision (Cool Spot) and the automatic selection

| Variant | Result |
| --- | --- |
| Original path (`--sound-driver original`) | `genesis_audio = degraded / structural_code_mismatch`, Z80 isolated at virtual frame 4, silent |
| `--z80-alternatives 1` (ADR 0259 mechanism, compiled alternatives) | Z80 isolated at frame 109: the alternatives covered the first mutation and not the next (a value the driver loads from RAM). Still degraded. |
| Static analysis of the image | 498 stores, 409 with unknown destination, 173 with unmodelled value, 11 exact candidates: not closable without RAM data-flow analysis (out of scope, ADR 0134 / 0291) |
| Default build (`--sound-driver auto`, nothing else) | Pass 1 reproduces the first row; that failure plus the verified catalog identity selects pass 2, the same program built with the replacement, which serves the image for the whole run (section 3) |

The decision is the build's, not the player's (ADR 0291 "Decision 4"). Checked on the local image: a plain `segarecomp build` of Cool Spot records `audio_implementation` = `native_driver`, `selection: automatic`,
`reason: original_aot_failure_verified_replacement`, with the original AOT's failure (`structural_code_mismatch`, frame 4) as evidence, and takes about twice the original build time (two passes; the second reuses nothing
of the first, by design). Committed: `genesis_native_gems_build_test` gives a self-modifying synthetic driver a catalog line for its signature (in a temporary copy of the runtime tree; the product catalog is untouched)
and requires the same record, and requires that a verified identity alone, without the original's failure, changes nothing.

## 3. Reference comparison

Reference: the third-party GEMSPlay C port of the 2.5 driver (offline, not vendored, never linked into the product). Three places where the port deviates from the 2.5 assembly are
corrected in a scratch copy (a sub-beat reset at every sequence start that its author marks "not in actual code", the SFX flag of envelopes, the target of the mute event); the
assembly is the authority. `tools/gems_reference_harness.c` runs it on the guest clock, driven by the commands **the game wrote** and the windows in which the 68000 held the Z80 bus (work due in a hold
waits for its end; a VBlank edge lost in a hold stays lost). `tools/gems_reference_check.py` runs every life of the native driver (a `/RESET` starts the next one) and checks, each as a failure and not a tolerance:

1. **Independent command observation.** The program, run with `SEGARECOMP_AUDIO_WRITE_LOG`, also logs the 68000's own stores into the driver's communication windows (`W` lines: the FIFO ring at `$1B40`, its
   pointers `$36` / `$37`, the mailboxes) at the bus, independent of whichever implementation reads them. The checker rebuilds the byte stream the game *committed* (ring bytes between the previous pointer and each
   new write pointer) and splits it into commands with the reference's argument counts. It compares that with the commands the native driver reports consuming: a command never consumed (a miss), one consumed that the
   game never wrote, a different content or order, a consumption before the write completed, a consumption later than two frames (time the 68000 held the Z80 bus excluded), an unknown command id and bytes the driver
   would skip are each reported. Commands written in the last two frames before the end are pending, not missed. The reference is fed the commands the **game** wrote (timed when the native driver took them: the harness models no Z80 polling).
2. **One shared horizon.** The program writes the guest time at which the observation ended (`E` line). Native and reference chip operations are compared up to that single time (the reference is run to it): exact
   operation order, port and value, and **neither side may have an extra or a missing operation inside the window** - a native driver that stops early (premature termination, a missing suffix) or plays on fails. A
   `/RESET` ends a life at the reset; the last life ends at the horizon. A log without the marker is refused unless `--horizon` is given.
3. **Time** stays a separate, reported quantity (the bounded skew per operation), never folded into identity. PCM identity is a property of the chip model given identical chip accesses and is covered by the determinism checks (section 4).

Result of the strengthened check on Cool Spot (default automatic build, about 100 s of game time, no input script): **352 commands written by the game, 352 consumed, in order, none unknown, no stray byte, consumption lag 0 ticks; 2,117,208
chip operations compared to the shared horizon, identical, skew 0; 504,454 DAC data writes; 11,675 bus holds**; the driver ended bound, no fault.

Detection power, on the real Cool Spot log (and on synthetic logs in CI, `tests/gems_reference_check_test.py`): removing the last 2 % of the native chip operations fails with "the native driver stopped early" (42,345 reference
operations missing); removing one consumed command fails at that command; removing the end marker is refused.

Earlier runs of all four games (about 4.66 million compared operations) used the first version of the checker, which fed the reference the commands the native driver *reported* and compared up to the native driver's last operation:

| Game (local image) | Commands | Sequences started | Bus holds | Chip operations compared | DAC data writes | Identical | Skew |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Cool Spot | 352 | 70 | 11,675 | 2,117,208 | 504,454 | yes | 0 |
| Aladdin | 224 | 105 | 9,763 | 589,044 | 129,862 | yes | at most 1,046 ticks (20 us) |
| Earthworm Jim | 464 | 226 | 16,146 | 1,446,677 | 336,161 | yes | 0 |
| Jurassic Park | 47 + 6 (two lives) | 21 + 2 | 13,266 + 2,593 | 371,783 + 136,405 | 86,142 + 32,879 | yes, both lives | 0 |

**The strengthened check has been run on Cool Spot only**: the scratch reference copy and the other builds were lost with the working directory before the other three could be re-run, and GEMSPlay is not vendored. Re-running
Aladdin, Earthworm Jim and Jurassic Park under it (commands from the game's own FIFO writes, shared horizon) is the open item; their results above carry the first checker's blind spots.

The compared stream is everything the replacement writes to the YM2612 and the PSG: boot, stops and clipping, FM patch loads and key on / off, frequency updates and pitch envelopes, PSG tone / noise and their envelopes, DAC set-up
and every Timer A-paced DAC byte, looped / slow / compressed sample handling, and voice allocation under contention. (Jurassic Park also streams some PCM from the 68000 itself; those accesses are not the driver's and are excluded.)
Two things are *not* claimed: exact PCM identity with another implementation, and cycle-accuracy against a real Z80.

Differences found on the way were each a defect in the replacement or in the comparison, not a tolerance: a note-to-sample mapping slip for notes below C4 (found by Earthworm Jim; fixed, with a unit test); Timer A is free-running on the chip so
the DAC pump takes the overflow time from the chip itself; at fast tempos a tick carries several sub-beats and each is a pass of its own; and four harness-model corrections (free-running timer, chip clock granularity, bus holds, tie order).

## 4. Real-game behaviour

* Genuine 68000 sound commands are delivered through the FIFO; the replacement consumes them (read pointer advances), resolves its resources from the cartridge and drives the existing YM2612 / PSG.
* PCM (headless `SOUND_SUMMARY`, Cool Spot, same instruction budget): the original-path build is silent (no sample change); the replacement build is non-silent with 2,584,123 sample changes; the digest
  is identical across repeated runs and across a different runner slice cadence (`SEGARECOMP_PERF_SLICE`). The run ends by budget, not by a stop.
* The build's own 600-frame start-up run (recorded in `status.json`) met no fault in any of the four games.

## 5. Negative and robustness cases

Committed (`tests/gems_native_test.c`, `tests/genesis_native_gems_build_test.py`, launcher tests): unverified identity (a changed signature; the fixtures' own drivers) is refused at build time with a typed failure and no
executable when the replacement is forced (the automatic policy ignores it); an unknown command id is consumed and ignored like the original; a sequence that runs off the cartridge, a resource outside it, a track count above 16, a FIFO pointer outside the ring and a
control-block number above 15 are each an explicit fault that leaves no further chip write and no fabricated acknowledgement; `/RESET` during playback restarts the driver from a clean state; a long bus hold
loses the interrupt edges inside it; sync-cadence invariance (one call vs. 777-tick slices: identical accesses); repeated builds produce an identical record; malformed options are rejected. Unexpected
structural mutation of the *original* path is the unchanged ADR 0134 fault isolation (`genesis_z80_build_pipeline_smc_test`).

## 6. Committed tests (synthetic data only)

`gems_native_tests`: boot; get-pointers / start / stop; the DAC FIFO's hand-derived sample counts (one-shot with the original's tail cut, low-note sample mapping, looped sample); FM note register map and
frequency; PSG tone note and loops; tempo including the multi-beat accumulator; mailbox store; the faults above; the save-state continuation and refusals of section 8.
`genesis_native_gems_build_test`: the build record (schema 2), the forced replacement's refusal of an unverified identity, the automatic selection on a synthetic self-modifying driver and its negative.
`genesis_native_snapshot_test`: the machine-level checks of section 8. `gems_reference_check_test`: the detection power of section 3 (missed command, invented command, wrong time, premature stop, extra operation, missing horizon) on synthetic logs.
Launcher: the record reader (schema 1 and 2, fail closed), wording and cache identity (`launcher_policy_test`), the Details page through the real window (`launcher_audio_gui_test`, run by the SDL3 jobs), which also checks that the launcher offers no sound option and passes none.

## 7. Limits

* Event-accurate, not instruction-cycle-accurate: the real driver's polling latency, its stalls while a command is half written and the 68000's DMA block flag are not modelled.
* The reference is a C port, not the original binary: where they differ the assembly decides (three corrections), and a defect shared by port and assembly would not be seen. No real-hardware capture was compared.
* The command check derives commands from the 68000's stores to the ring and its pointers; a game that bypasses that protocol is outside it. Argument counts are the reference's. Timing is reported (skew) and bounded only by the two-frame consumption limit.
* Audibility of a real play session is not claimed. One driver image / version (2.5) is verified; another is ignored by the automatic policy until it is in the catalog and validated.

## 8. Save states and rewind

**Committed, driver level (`tests/gems_native_test.c`).** For a mid-sample snapshot, a looped sample, an active FM note, a PSG loop, a pending (unconsumed) command, a BUSREQ hold, and `/RESET` followed by a fresh activation: the
uninterrupted run is the reference; the state is serialised, restored into a different running driver (and again into one that already ran past it: a rewind), the future is run with different slicing (777 or 100,003 ticks per call), and the chip
operations (kind, port, value, time), the driver's canonical state, the sound RAM with FIFO and mailboxes and the Timer A schedule must be **exactly** equal. Refused: an empty payload, every truncation, a trailing byte, another guest
clock, a chip schedule that contradicts a pacing DAC; decoding never touches the live driver for any single-byte corruption.

**Committed, machine level (`tests/genesis_native_snapshot_test.py`).** On the real runtime with the production sound adapter and a project-authored cartridge (one extra signature trusted by a test-only build define, absent from the product):
a state taken while a sample plays restores bit-identically (independent whole-state hash and the SHA-256 over every mixed sample), repeatedly, and the future across a long bus hold and a `/RESET` plus re-upload is identical; a state saved by one
process is restored by a fresh one whose replacement never started and re-captures to the identical file, and after the same future too; a restore writes nothing to a chip; a native state is refused by a program without the replacement and vice
versa; a different driver name, version, implementation version, schema or signature (valid digest) is incompatible, a damaged length a bad layout, structural damage an invalid state - with the live machine untouched; machines restored from accepted
damaged payloads keep running; a faulted driver is recorded as faulted, restores inert and writes nothing further. `genesis_snapshot_test` is unchanged and passes (programs without the replacement write the same container).

**Local demonstration, Cool Spot** (`tools/genesis_state_demo.py` with `tools/genesis_state_demo_hook.c`; not committed to CI). It builds the game with `--keep-work`, links a hook that runs to a point where the native driver is playing,
saves, runs on, restores, runs the same stretch again and a third time, and compares mixer digest, YM2612 state, driver state, guest clock and counters. Result: saved at 267 commands / 13 sequences / 44,412 DAC samples with the driver active and
healthy (148,585-byte state); restored state equal to the saved one; the following stretch (85,289 more DAC samples, 357,634 mixer frames) **identical** after the first and the second restore; non-silent.

Limits: exactness is in the machine's own deterministic terms; "restored music sounds right" is the same claim as for the uninterrupted run. The viewer's save / load / rewind uses the same service and is covered by the machine-level test, not by a GUI test of the replacement.

## 9. Reproduce locally

```
segarecomp build --rom <game>.md --output out --cc cc --runtime-dir platforms/genesis --sound-driver gems
SEGARECOMP_AUDIO_WRITE_LOG=native.log out/game --instruction-budget 40000000
# a copy of GEMSPlay with the three scratch corrections, then:
cc -std=gnu11 -w -D__cdecl= -include string.h -I<GEMSPlay copy> -o harness tools/gems_reference_harness.c <GEMSPlay copy>/GemsPlay.c
python3 tools/gems_reference_check.py --rom <game>.md --native-log native.log --harness ./harness
python3 tools/genesis_state_demo.py --rom <game>.md --work demo        # save / load while the driver plays
```

## 10. Kid Chameleon (USA, Europe): diagnosis of the silence and its resolution (ADR 0291 Decision 6)

Authorized local image, used ephemerally; aggregates and classes only. ROM SHA-256 `bc08b018...3d4d5baa` (the image of the public Kid Chameleon disassembly's reference build, identical digest).

**Failure before the change** (`segarecomp build`, default policy, no sound option): build `ok`, `genesis_audio = degraded`, `z80_audio_outcome = structural_code_mismatch`, `audio_implementation`:
`original_z80_aot / no_verified_replacement / degraded`. Two Z80 images, five epochs (a one-extent stub, an empty epoch, then the same one-extent driver three times); the sound CPU is isolated by a structural code mismatch at
frame 190 in the last epoch, i.e. in the driver image. No image is in the verified catalog (neither signature is the 2.5 line). The 68000 does submit commands (its command-FIFO write pointer advances to 29 in a 40 M-dispatch
run, 15,368 bus holds), the isolated sound CPU consumes none of them, there are 0 key-on and 0 DAC writes, 8 YM2612 and 16 PSG writes (boot only), PCM silent (`non_silent = false`), at virtual frame
3,396 the game is running. Failure class: **1 (original Z80 isolated by unsupported structural self-modification)**, with 2 (image unknown to the catalog) as the reason no replacement applies; classes 3-7 are not reached.

**Driver identity.** Compared (by structure, offline) with the published 2.0 / 2.5 / 2.8 driver sources and ports. It is not the verified 2.5 image and not byte-identical to any published build:

| Aspect | Kid Chameleon driver | GEMS 2.0 source | GEMS 2.5 (verified image) |
| --- | --- | --- | --- |
| stack / mailboxes / command FIFO / CCBs / channel cache / DAC FIFO | `$1A20` / `$1A20` / `$1A40` / `$1A80` / `$1C80` / `$1F00` (stack init and FIFO references found in the code) | same | `$1B20` / `$1B20` / `$1B40` / ... (shifted) |
| command ids dispatched | 0-7, 11-14, 16, 18, 20, 22, 23 (dispatch falls through to the main loop after 23) | the same plus 26-29 | the same plus 26-32 |
| FIFO pointers | write `$36`, read `$37` | same | same |
| driver size / code layout | 5,770 bytes, main loop and tables at their own addresses | about 5,750 bytes, other table addresses | 6,278 bytes (written extent 8 KiB) |
| conclusion | an earlier / distinct 2.x build ("unknown GEMS 2.x variant, 2.0-style RAM map"); version not established | | |

Protocol-level differences from the verified implementation therefore exist (RAM map, command set; sequence-event and sample-handling differences are suspected from the 2.0 port's diff against the 2.5 port and unmeasured). Binary-only differences cannot explain it. Per
the task's decision order this is Case C for a **native** implementation: no independent reference for this exact build exists (the 2.0 port models the dev-kit build, which this image is not), so a native variant would not be reference-checked
and is not added. The catalog is unchanged (revision 1); nothing is guessed compatible.

**Resolution without a replacement.** The original driver executes correctly once its (statically closed: 1 image, 2 bytes, 4 forms) self-modification is covered by the existing compiled structural alternatives (ADR 0259).
The default build now selects that automatically (Decision 6). Default `segarecomp build` of the same image: `genesis_audio = supported`, `audio_implementation.reason = original_with_structural_alternatives`,
`original_aot = {structural_code_mismatch, frame 190}`, `structural_alternatives = {requested, 1 image, 2 bytes, 4 entries, 4 forms}`, build time about twice (two passes).

Same run (40 M dispatches, virtual frame 3,396): sound fault none, 1,090,305 YM2612 writes (595 key-on events on five FM channels between frame 191 and 3,389, 261,765 DAC data writes), 1,258 PSG writes, PCM non-silent
(2,110,475 sample changes), two runs bit-identical (chip-access log and PCM digest). The driver consuming the game's commands is shown by that activity, not by a separate counter; **not claimed**: that a particular sound
effect was heard, a comparison with an independent reference (the original driver itself executes, so there is no second implementation to compare), nor audibility of a play session.

**Save state while playing** (`tools/genesis_state_demo.py`, 20 M dispatches before the save, 4 M compared after): state of 146,669 bytes saved with the music running, restored equal to the saved one, and the continuation
identical after the first and the second restore, non-silent.

**GEMS 2.5 games.** The first pass of Cool Spot / Aladdin / Earthworm Jim / Jurassic Park still selects the verified replacement before the new step is reachable (the code path is unchanged); the Cool Spot build record is
unchanged (see the PR for the exact result). The commercial reference rerun for the four games (352 / 352 commands, 2,117,208 operations for Cool Spot) was **not** repeated: the replacement code and catalog are untouched and the
third-party reference harness is not vendored; the committed synthetic tests are unchanged and pass.

**Failure semantics.** A failed second pass is the build's failure (typed, propagated, no executable, no success record, no third build). `genesis_native_gems_build_test` injects one failure into the second pass only
(a compiler wrapper keyed on the alternatives' emitted C: a unit compile, then the pass-program link) and requires a failed build, its own diagnostic in the log, no executable, no `genesis_audio` record and exactly one
second-pass attempt. The same test covers: closable driver -> automatic alternatives (supported); unclosable (RAM-loaded value) -> degraded `no_verified_replacement`; verified faulting image -> native replacement; verified
non-faulting image -> no replacement; working driver -> no extra pass; `--sound-driver original` -> no alternatives. Unnecessary-work review: no existing signal is complete enough to skip the pass (see ADR 0291 Decision 6); cost left as is.

**One gameplay sound effect (bounded, reproducible).** The default executable, frame-keyed pad script (`SEGARECOMP_PAD_SCRIPT`, applied with `SEGARECOMP_CAPTURE_DIR`), three Start presses at frames 1800 / 2000 / 2200 reach the first level
(frame captures show the playfield and its HUD; a further Start pauses). Control: no further input. Test: button B pressed at frame 2600 for 30 frames. Both with `SEGARECOMP_AUDIO_WRITE_LOG`, 70 M-dispatch budget:
- 68000 side: the control's command write pointer next moves at frame 2653.9; with B it moves at frame 2601.9 (the frame after the press) by three bytes (the 68000 publishes the pointer after two bytes, then after the third), i.e. a command of the
  `$FF id arg` shape; the command payload itself is not in the product's logging window for this driver's RAM map and the specific sound id was not extracted (not claimed).
- Z80 side (the game's original generated-native driver): from frame 2604.9 the PSG noise channel receives a new attenuation envelope (control: a slow fade; with B: a distinct attack/decay burst, 25 vs 23 PSG writes in the window,
  71 vs 46 PSG writes from frame 2590 to the end), the first dozen FM key-ons after the press are unchanged, and the PCM digest differs from the control (`49a759d6...` vs `33733b14...`).
- No sound fault in any run; the B run is non-silent and bit-identical across two runs (chip-access log and PCM digest). A second input (C, a jump) produced no new command and only a timing shift of already scheduled
  writes (a different PCM digest, no new PSG or key-on events): it is not claimed as an SFX.
- Not verified: that the B action is identifiable as a particular named effect in the public disassembly (its sound-call sites are conditional on the current helmet and could not be tied to the exact branch within the bounded investigation).
  The claim is therefore "a gameplay input provokes a new sound command and a corresponding new, deterministic PSG effect through the original driver", not an identified effect.

**Follow-up (not in this change).** A native variant for this driver build would need: the 2.0-style RAM map as an immutable descriptor, a disassembly-level comparison of this exact image's sequence events and sample handling,
and an independent reference for it. It is only worth doing if a game is found whose driver the structural alternatives cannot close.
