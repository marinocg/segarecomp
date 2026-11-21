# ADR 0318: Native Krisalis sound-driver replacement for Genesis games whose Z80 driver the generated-native path cannot run

- Status: Accepted (SEG-151-T001, one combined delivery).
- Builds on: ADR 0072/0073 (Genesis Z80 platform integration, RAM-backed images), 0134 (structural variants NO-GO), 0259 (structural alternatives), 0291 (the native GEMS replacement: the
  machine seam, the automatic selection, the save-state contract). Changes none of their conclusions; it adds a second replacement family behind the same seam and generalises the build policy from one family to an ordered list.
- Architecture and contract: `docs/architecture/genesis-native-krisalis.md`. Evidence: `docs/testing/genesis-native-krisalis-validation.md`.

## Context

Three local images froze at start-up (The Lion King (UEJ): black screen; Mickey Mania: black screen; Chuck Rock: frozen on its title screen) and two further images of the same family (Sensible Soccer, Boogerman) were found to share the failure
class. All five end with `genesis_audio = degraded / structural_code_mismatch`: the generated-native Z80 isolates itself at start-up (the driver copies its own helper routines into the vector area and executes them, and the later revisions patch operands of their own code), the isolated sound CPU never acknowledges the 68000's mailbox, and the 68000
waits for that acknowledge forever (it is stuck in its sound-command wait loop: a bus-request / release pulse train, millions of times).

## Facts established (aggregates only; no byte, address, disassembly or title data is recorded)

1. **Not a shared GEMS problem and not a shared binary.** The five images carry the public Krisalis driver (Shaun Hollingworth; the 68000 is the tracker sequencer, the Z80 a player). The driver code each image uploads is byte-identical, over its whole real length, with one of three published revisions
   (offline comparison against the public reverse-engineering project, kept private): **revision 1** (Chuck Rock), **revision 2** (both local Sensible Soccer images) and **revision 3** (Mickey Mania, The Lion King, Boogerman: one and the same code image). The Lion King's revision is therefore established exactly (3), not "nearest".
2. **Three Z80 images per game, one of them the driver.** Every image epoch uploads (a) a small program shared by every game (the same signature in all five), (b) a 27-byte boot stub (revision 1 selects ROM bank 2, revisions 2/3 bank 0), (c) the driver. Only (c) faults the original path and only (c) is replaced. Its activation signature covers the reset area and the whole 5 KiB block the game copies (the code and whatever followed it in the game's ROM), so each game's driver upload has its own signature even where the code image is the same.
3. **The 68000 stops at the acknowledge**, not at a VDP or CPU failure: the sound-command wait (`Z80_WaitIdle` of the published 68000 module) spins on the mailbox command byte; a replacement that clears it exactly when the original does lets all three games proceed.
4. **A second, unrelated frontier then appears for revision-3 module layouts that dispatch through a jump table** (The Lion King, Boogerman): the 68000 sound module's entry (`lsl.w #2,d7 / jmp table(pc,d7.w)` over a run of `bra.w` slots) is an indexed computed jump whose index is a caller constant; the static analysis does not prove its target set and the program stops with `unresolved_indirect_target`. This is a 68000
   discovery gap, independent of sound; it is recorded as a follow-up (SEG-151-T002) and not masked.

## Decision 1 - one shared Krisalis core with revision descriptors, not three drivers and not a framework

`krisalis_native.c` implements the observable behaviour once. A revision is a small static layout descriptor (RAM addresses of the mailbox, the channel state and the note table), a timing row and a few explicit branches (acknowledge timing, DAC player, slides, cell commands, volume, banks). Nothing is added for a
revision that no supplied image needs. The module reads and writes the same sound RAM the 68000 does (mailbox, voice bank, channel state), so a re-upload of the driver block resets the mailbox and the channel state exactly as it resets the original, and a machine snapshot carries all of it; only the effect queue and the DAC player are private state.

## Decision 2 - the 68000 is not changed and nothing is fabricated

The replacement consumes the mailbox command, turns the six cells of a row (or the tick) into YM2612 writes through the existing device model, plays the DAC from the cartridge, and clears the command byte at the point the original does (revision 1 when the work is done, revision 3 when the command is read).
It never writes a ready flag the original would not write. Unsupported or malformed operations fault the driver (`unmapped_resource`, `invalid_state`, `unsupported_revision`); it then stays silent and the machine records the fault. No fallback driver, interpreter or JIT exists.

## Decision 3 - timing: event level, with one property that is not optional

The original's wait loop stands still in the middle of an iteration while the 68000 holds the bus, and the 68000 holds it again and again for a few cycles while it waits for the acknowledge. A model that restarts the loop at every hold never reaches the command read, and the game hangs. The driver therefore keeps the loop's *phase* across holds
(measured period of the original's wait loop per revision) and delays pending work by a hold. Spacing of writes is a fitted per-revision table; order is exact. Cycle accuracy is not claimed.

## Decision 4 - identity and the automatic policy

Only the uploaded driver image is identified, by its activation signature, in `krisalis_driver_catalog.inc` (an independent catalog and revision counter; the format and the verification tiers mirror ADR 0291's). The build's automatic policy becomes: original first; when it isolates the sound CPU by a structural code mismatch and the faulting image is in the GEMS catalog, GEMS (unchanged
precedence); else when it is in the Krisalis catalog, Krisalis; else the structural-alternatives pass of ADR 0291 Decision 6 when its emitter reports admissible forms; else degraded. Exactly one replacement is compiled into a program (`-DSEGARECOMP_NATIVE_SOUND_<FAMILY>`); an image both catalogs name belongs to GEMS. `--sound-driver krisalis` is a developer override
with the same typed refusal (`sound_driver_unverified`). The record (`audio_implementation`) names `krisalis`, the revision, `version_evidence = published_code_identity`, the identity prefix, the tier, the implementation version and the catalog revision; the launcher cache identity includes both of the Krisalis replacement's.

## Decision 5 - save states and rewind

Unchanged contract (ADR 0291 Decision 5): canonical versioned payload, staged restore against the guest clock, atomic commit, no chip write on restore, refusal by a program without this replacement or with another driver / version / signature. The bound of the payload (`GENESIS_Z80_NATIVE_STATE_MAX`) is raised from 4 KiB to 6 KiB (the longest effect queue of a revision-3 row); the GEMS payload is unchanged.

## Decision 6 - revision 2 is analysed, not claimed

Revision 2's code is a close sibling of revision 3 (acknowledge at the end of the work as in revision 1, DAC polled only in the wait loop, no loaded-voice cache). It is not implemented here: independent verification would need a gameplay-driven reference run for it and the supplied games do not exercise it. No Sensible Soccer image is in the catalog; the automatic policy leaves those builds degraded exactly as before.

## Consequences

Chuck Rock and Mickey Mania run past their freezes (gameplay, input, music and samples) on the default build path; The Lion King shows its logo and starts its music and then meets the discovery frontier of fact 4. Builds of the three games take one extra pass (two in total), with the first pass's evidence as the selection record.

## Rejected

A Z80 interpreter or JIT (forbidden by the project's scope); one driver per game (the three images share one code image); a GEMS/Krisalis common engine (no common musical semantics); a plugin registry or virtual-CPU abstraction (two families do not justify one); fabricating the acknowledge (a hidden failure); treating the activation signature as a revision key (it covers the bytes that follow the code in each game's ROM, hence one catalog line per exact upload).
