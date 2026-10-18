# ADR 0145: SEG-064-T005 - the authentic BIOS and application together past the handoff, and the Checkpoint E decision (G-A)

- Status: Accepted (SEG-064-T005). Builds on ADR 0142 (admission, G-M), ADR 0143 (BIOS-scale emission, main-side boot hardware) and ADR
  0144 (sub-side boot hardware, disc-extent images, the application handoff); ADR 0141's historical `NO_GO` is unchanged.
- Scope: the BIOS-image profile continues past the application entry for a pre-registered interval of the application's own execution,
  through generated-native code only, and the result is compared with the independent reference. It does **not** claim gameplay, a title
  screen, audio, the Z80 sound driver, or anything after the interval.
- Privacy: no BIOS or disc byte, hash, address, extent coordinate, routine inventory, trace or disassembly appears here or in any test.
  The comparisons with the independent reference were made locally; only aggregate identity results, capability classes and counts are
  reported. The regression tests use project-authored fixtures only.

## Context

ADR 0144 ended, by design, at the first main-processor instruction of the application IP (`application_handoff`, before it executes).
T005 asks the remaining G-A question: does the generated-native program keep running the authentic BIOS and the real application
together, every BIOS-derived instruction a compiled, byte-verified entry, and do the two agree with the reference over the same interval?

## Decisions

1. **The boundary is removed, not replaced.** The program is built without `--application-entry`: the entry instruction runs through the
   normal generated dispatch. No new execution mode exists; the stop-enabled program of ADR 0144 is unchanged and remains the handoff
   regression (decision 9). The entry is part of the program identity, so the post-handoff program is another program: its whole
   admission chain (the verified sub epoch, the operand templates) was rebuilt under the new identity by the bounded fixed point (14
   rounds + the confirming run); no admission of the stop-enabled program is reused.
2. **Pre-registered interval.** 60 NTSC video frames of machine time after the application entry (448,020,000,000 ticks each), no
   controller input, fixed before any post-handoff run (the default of the task statement). It was not changed afterwards.
3. **Disc-extent images, generically.** The limit of declared extents was four (ADR 0144 decision 8); the application needs more
   executable regions than that, so it is now sixteen (`kMaxDiscImages`; seventeen is refused; every extent keeps its bounds, parsing and
   byte-identity rules, and the digest of its bytes enters the program identity). Three families of executable regions appear in the
   interval, each with independent evidence of where its bytes come from, and each declared as an extent of the verified disc:
   - **a RAM-resident routine copied by the IP's own code.** The IP copies a short routine from its image to a work-RAM address and
     jumps to it. The evidence is the copy loop itself (disc code; source, destination and count are immediates of the loop) and the
     reference's own fetches: the words it fetched at the destination equal the IP bytes at the source. It is therefore the IP's bytes
     mapped at a second base (an extent of the same disc bytes); the live bytes select it, per instruction, exactly like every variant.
   - **two disc-loaded main-processor modules.** The application's loader takes a header-described module from Word RAM (which the sub
     filled from the disc by CDC transfer) into work RAM and enters it. The evidence is the header (destination, length and entry fields),
     the reference's fetched words (equal to the disc file's payload at the declared destination), and the CDC host-data word streams of
     both sides (identical). Each payload is a disc extent mapped at its destination.
   - **a module loaded by the SP into sub PRG RAM**, found the same way (the reference's fetches and the disc), mapped at its load base.
   The extents are build inputs found by local analysis of the executed frontier; their coordinates are not recorded. No extent is
   inferred from a live RAM snapshot: each is a range of the verified disc whose bytes the reference executed at that place.
4. **Machine-time end (`--until-tick`).** The program driver gained `--until-tick <ticks>`: the run ends at that machine-time instant with
   the result `deadline` (exit 11, no typed stop). It is the existing deadline mechanism with a final deadline, so it is resumable and
   identical under any deadline partition (tests: eight seeds and a fixed slice). A zero is a usage error. The build driver passes it to
   every pass when asked (`--until-tick`), and can preset registers of a pass (`--pass-set`) so that project-authored fixtures select a case.
5. **Diagnostics for the comparison** (local, never durable evidence, no effect on the machine): `--coverage` also writes the distinct
   control-flow edges (a processor's consecutive instruction starts, the relation the reference records) to `<file>.edges`; the
   report's `bios_profile` carries `vblank_onsets`. The edge sets are what the BIOS-service call and return classes below are computed
   from.
6. **No new device behaviour was required.** The interval executes with the devices of ADR 0143/0144 only (graphics ASIC, CDD, CDC host
   and Word RAM transfers, the Gate Array, PCM writes, the main-side Genesis devices). Z80 execution, the H-interrupt line counter and
   VDP DMA duration were not reached as behaviour that the interval's checkpoints depend on (decision 7). Every device in an unsupported
   shape remains a typed stop.
7. **The Z80 is still not executed, and the reference shows why that is sound here.** The application loads a sound driver into Z80 RAM
   and releases the Z80 from reset inside the interval. The model stores the RAM, honours BUSREQ/RESET and, once the Z80 has been
   runnable, stops a main read of the Z80 area (`main_device_unsupported`) instead of answering. To establish that nothing the main or
   sub processor can observe depends on the running Z80 in the interval, the reference was instrumented (locally) to count the Z80's bus
   activity: between the handoff and the end of the interval the Z80 made accesses only to its own RAM and to the YM2612 and **none to
   the 68000 bus window, the bank register or the VDP/PSG window**. The Z80 and the sound are therefore not part of the claim; a later
   interval in which the application talks to the driver needs a Z80 integration (a separate, generated-native design), not a shim.

## Result (SEG-064-T005)

**Authentic build** (authorized US Model 1 class BIOS and Sonic CD (USA) disc, local only, official driver, cold, gcc -O1 strict C11,
12 jobs): the epoch + 12 operand templates (2 main, 10 sub), 14 rounds + the confirming run, 630 s; 156,684 instruction functions
(5,385 more than the stop-enabled program of the three T004 extents, 151,299), 150 MB of C in 37 translation units; peak compiler resident set 579 MB, emission 467 MB.
Images (compiled starts): BIOS 61,869; sub epoch 64,628; disc boot area 16,337; SP 5,627; SP-loaded sub module 9,179; RAM-resident
routine 39; main modules 2,012 and 3,922.

**Accepted interval: 60 frames, no input, completed with no typed stop** (result `deadline`; 549,829 main and 977,042 sub instructions
retired in it; `vblank_onsets` 1,197 at the entry, 1,257 at the end).

| check (private GPGX reference, same input, no controller) | result |
| --- | --- |
| T004 stop-enabled regression (the original three extents + the entry boundary, rebuilt: 14 rounds, 727 s) | **PASS**: `application_handoff`, main 19 registers + SR identical, sub identical except the pc (two instructions apart in a polling loop), Word RAM and PCM identical, 28 work-RAM and 44 PRG-RAM bytes differ - exactly the T004 result |
| executed instruction starts after the handoff (whole run, per processor) | main 1,724 vs 1,724 and sub 3,519 vs 3,519: **identical sets** (192 new main and 40 new sub starts in the reference's interval; none missing, none extra) |
| compiled authority of the interval's starts | every executed start is a compiled entry of an admitted image whose whole instruction was compared with the live bytes (any other start is a typed stop before the instruction); main: 196 application starts (no main BIOS ROM instruction executes after the handoff - the reference executes none either, in 600 frames); sub: 1,158 BIOS-epoch, 169 SP and 42 module starts |
| register files at 11 time-aligned checkpoints (entry + 0, 1, 2, 5, 10, 20, 30, 40, 50, 59, 60 frames, reference frame ends) | main: all 19 registers and SR equal at 3 of 11 (at the others only the pc and registers that a handler or the loader loop is changing at that instant differ); sub: equal at 9 of 11 (the pc inside a polling loop otherwise) |
| the same files at the semantic event "the 70,794th CDC host-data word consumed" (+ a quarter frame) | **main and sub: every register and SR equal** |
| Gate Array command, status, flag, mode, control, timer registers | equal at all 11 checkpoints; the interrupt-enable byte differs at one checkpoint (the CDC enable bit, toggled around a transfer: phase) |
| Word RAM | equal at 9 of 11 checkpoints; at the last two a single 2 KiB block differs - ours is one CD sector (about a frame) ahead in the same transfer: ours equals the reference's state 0.6-1.0 frame earlier, and **at the semantic event above all 262,144 bytes are equal** |
| PCM chip RAM and registers | identical (65,536 bytes) |
| sub-side CDC host-data word streams (70,794 words) | **identical word for word** (ours reads 1,026 more by the end of the interval - the same 0.6 frame) |
| work RAM / PRG RAM | 27-28 bytes (all in the main stack frame area) / 28-100 bytes (BIOS CD-status and message data, current-position fields sampled at different instants, and the sub stack): classified below |
| Z80 bus activity in the interval (reference) | RAM 12,688 reads / 9,858 writes; YM2612 70,691 reads / 158 writes; **no** 68000-window, bank-register or VDP/PSG access |
| determinism | one report/device-log/two-trace-digest outcome over 2 repeats, 12 seeded deadline partitions and 2 fixed slices (16 runs) |

**BIOS-service classes in the interval.** Main: no BIOS instruction executes after the handoff, in the interval or in the reference's next
600 frames - the application owns the main processor (its vertical-blank vector is a work-RAM thunk to application code, 59 executions in
the interval). The six work-RAM-to-BIOS control-flow edges seen over the whole run were all executed before the handoff.
Sub: 12 BIOS entry points are reachable from SP/module code over the whole run; 6 are invoked in the interval, 370 invocations
(entry-instruction executions, an upper bound), through the mutable sub jump table (10 operand templates) and the BIOS interrupt
handlers (3,837 SP and 80,712 module instruction executions, 892,334 BIOS-epoch ones).

**Classification of the differences** (none unexamined): (a) exact equality - the executed sets, the main register file at the event,
the sub register file at the event, the Gate Array command/status registers, the CDC host-data streams, the PCM chip, the Word RAM at the
event; (b) independently justified asynchronous differences - the pc inside a tight polling loop (the reference idle-skips a processor
that polls a Gate Array register after three reads), the instants at which interrupts are taken (all 274 differing control-flow edges
are interrupt entries and `RTE` returns, plus 4 conditional-branch directions of polling loops), the position of an in-flight CD sector
(one sector, the data stream identical), CD current-position fields sampled at different instants, stack residue below the stack
pointers, the CDD exchange cadence (ADR 0144); (c) material differences requiring correction - **none found**; (d) evidence
unavailable - the Z80's and the sound's internal state (not modelled; no 68000-observable effect in the interval, above).

**Latest runtime-selected frontier (outside the accepted interval).** 3.3 frames after the end of the interval the main processor enters
a third module the loader copied into work RAM: `code_bytes_mismatch` at its entry, because no extent of the disc declares that module -
ordinary missing compiled executable authority of the same family as decision 3, not an architecture contradiction.

## Checkpoint E decision (gate G-A)

**CHECKPOINT_E_PASS, with the claims kept distinct.**

- *Fixture claim* (project-authored, in CI): the BIOS-image profile executes an application past the handoff through generated-native
  dispatch, a RAM-resident routine admitted as a disc extent, a BIOS service reached through a mutable work-RAM thunk and its return, the
  typed failures (uncompiled thunk target, corrupted live byte, missing/wrong extent), `--until-tick` and the deterministic partitions
  (`sega_cd_bios_posthandoff_test`).
- *Authentic BIOS boot claim* (local only): the authentic main and sub BIOS run from reset to the application entry (ADR 0144, repeated and
  unchanged in this task), and then, together with the authentic application, through a 60-frame interval with every BIOS-derived
  instruction start a compiled, byte-guard-verified entry of an admitted image, the executed sets and the register files at the semantic
  event equal to the independent reference's, and the classified asynchronous differences above.
- *Commercial-compatibility claim*: **none** beyond that interval - no Z80/sound, no gameplay, no title screen, nothing after the interval,
  and no claim about main-side BIOS services (the application does not call any in the reference's first 600 frames after the handoff).

The criteria of the task statement are met: the boot and handoff remain independently verified; the agreed interval executes through
generated-native code; every executed BIOS-derived start has verified compiled authority; the observed sub-side BIOS-service interactions
are correct (executed sets and the state at the event equal); no unresolved image-authority, indirect-control or device frontier lies on
the accepted path; the checkpoint states agree with the reference with every asynchronous difference examined; determinism holds.

## Consequences

- G-A is satisfied; SEG-064's acceptance (a supported user BIOS and disc boot path reaching a validated handoff, every BIOS service
  statically compiled from the user's BIOS) is met by ADR 0144 plus this decision.
- Continuing the application further (the third and later modules, the Z80 driver, audio) is new work under a later milestone
  (SEG-065 graphics and title screen); the extent mechanism and the comparison method here apply unchanged.
- The synthetic post-BIOS profile (SEG-062/063) and the SEG-064-T002/T003/T004 proofs are unchanged; the bootarea test's expectation of a
  "too many images" refusal moved from 5 to 17 declarations.
