# ADR 0150: SEG-070 - the Sonic CD START path, generated-native, and the Checkpoint G decision

- Status: Accepted (SEG-070-T003). Judged against the pre-registration of ADR 0148, which it does not change, using the machine input of
  ADR 0149.
- Later (SEG-071-T001, ADR 0176 decisions 3-4): the reference presses at the onset that opens frame entry + N, one frame before the instant
  decision 1 converted; with that instant the START polls equal the reference's exactly (480, 482, 483, 485) and the frames at entry + 510 /
  540 / 600 stay identical. Decision 4's poll-cadence phase was this offset, not a CD-sector phase. The result below is unchanged.
- Scope: the authentic BIOS and the Sonic CD (USA) application, generated-native, from reset through the Checkpoint F interval plus one
  START press on controller port 1 to entry + 603 frames; the comparison of the pre-registered frames and state with the independent
  reference. It does **not** claim gameplay, audio output, the Z80 sound driver's execution, the intro movie, a second START, or any
  frame other than the five registered ones (entry + 420, 480, 510, 540, 600); the frames between the press and entry + 510 are
  classified below, not claimed.
- Privacy: no BIOS or disc byte, hash, address, extent coordinate, file name, frame image or frame digest appears here or in any test.
  The comparisons were made locally; only equality results, counts and classes are reported.

## Context

ADR 0148 fixed one START press (port 1, pressed at the vertical-blank onset closing reference frame entry + 480, held 6 frames), the
target (the title screen's prompt replaced by the title menu), the frames entry + 510 / 540 (primary) / 600 and the pass rule before any
generated-native input existed. ADR 0149 added the timed standard-pad machine input. This ADR applies them.

## Decisions

1. **The schedule in machine ticks.** `tools/sega_cd_input_script.py onset --base-onsets 1197 --press 480 --hold 6` converts the
   registered frames to the two absolute ticks of the canonical time (the 1,197 onsets processed at the application entry are ADR 0145's
   measurement; onset n is at 224 lines + (n - 1) frames of 262 lines, 3,420 master clocks per line, 500,000 ticks per master clock).
   The ticks were computed from the registered frame numbers alone and never adjusted afterwards.
2. **No new executable authority and no new device behaviour were needed.** The bounded materialization fixed point, run with the
   START schedule given to every pass (`--input-script` of the build driver), converged on exactly the admission chain of Checkpoint F
   (the verified sub epoch and 13 operand templates; the ten declared disc extents; 14 rounds + the confirming run, 1,022 s cold after
   the one-pointer ABI change of ADR 0149; 178,005 instruction functions, 176 MB of C in 38 translation units; peak compiler resident
   set 922 MB), and ended with no typed stop at entry + 603 frames. The 120 main-processor instruction starts that exist only with
   START lie inside the already compiled entries, selected by their live bytes. No H-interrupt, Word RAM DMA, Z80 execution or audio
   output occurs; the Z80 makes no access to the 68000 bus window, the bank register or the VDP/PSG window in the reference up to
   entry + 600 with or without START (its accesses are to its own RAM and the YM2612), so nothing a processor can observe depends on it.
3. **START is accepted by the real application through the modelled pad.** In the generated run the first poll after the press that
   reads START is a TH-low read of data port 1 returning `$13`; four polls read START (onset intervals 481, 482, 484, 485 after the
   entry; the reference: 480, 482, 483, 485), and every data-port read of the whole run returns one of `$33`, `$7F` (released) or
   `$13`. The poll sequence (port, value, in order) of the whole run is identical to the reference's up to a phase of the application's
   poll cadence, see 4.

## Result

**Checkpoint G: `TIME_ALIGNED` (ADR 0148 decision 9).**

| check (private GPGX reference, same schedule and preconditions) | result |
| --- | --- |
| canonical frame at entry + 420 / 480 (no-input regression frames, same run) | identical, time-aligned |
| canonical frame at entry + **510 / 540 (primary) / 600** (256 x 224, digest and dimensions) | **identical at all three**, offset 0 |
| the same frames of a generated run without START (empty script) | identical to the **no-input** reference at all five frames and different from the START reference at 510 / 540 / 600 in exactly 562 pixels of one 11-row band: a run that did not react would fail |
| VDP registers 0-23, CRAM, VSRAM, VRAM (64 KiB) | identical at entry + 480 / 510 / 540 / 600 |
| Word RAM (256 KiB) | identical at all four |
| main CPU registers (d0-d7, a0-a7, pc, sr) | identical at entry + 510 / 540 / 600 (480: loop-phase differences in ten data and address registers and the pc) |
| sub CPU registers | only the pc differs (inside its polling loop), as in ADR 0147 |
| work RAM / PRG RAM | 20 / 138 bytes differ at 510 / 540 / 600 (162 / 129 at 480): the classes of ADR 0145 / 0147 (the loop's destination buffer and stack residue below the stack pointer; the BIOS status area and the sub stack); none is read by the frame |
| executed instruction starts, reset to entry + 600 | **main 3,053 = 3,053, sub 4,953 = 4,953, no start on either side only** (with START and without: 3,053 / 2,933 on the main side, both equal to their references) |
| compiled-entry and executable-image verification | every start is a compiled, byte-verified entry; no extent, template or admission was added |
| controller observations | 3,468 data-port reads against the reference's 3,458; value histogram identical in its three values (4 START reads each); the START polls are one frame late at two of the four (see decision 4) |
| determinism | one outcome (report, five canonical frames, every state dump, the read log digest and the coverage) over 2 repeats, 12 seeded deadline partitions and 3 fixed slices (17 runs); the reference: two runs identical in every frame, state dump, controller log and instruction-start set |
| Checkpoint F (no input) | the frames at the three Checkpoint F onsets of a run without START: canonical frame, VDP state, both CPUs' registers, work, PRG and Word RAM **byte-identical to the SEG-065 final run's captures** (the report differs only in its end instant) |
| Checkpoint E | see the next section |

**Classified asynchronous differences (ADR 0148 decision 8).**

4. *Poll-cadence phase.* The application polls the pads about twice in three video frames, not locked to the vertical-blank onset. In
   the generated run the polls lie in a different frame of the three-frame cycle than in the reference: before the press as well (the
   same comparison of a run without START: 167 of 974 onset intervals differ), so it is independent of the input. It is the CD-sector
   phase class ADR 0145 recorded (the application entry is not a whole number of 75 Hz periods from the reference's; the title's own
   animation is locked to the video frame and stays equal). Consequence: the first poll that sees START is one frame later in the
   generated run at two of the four polls, and the step positions of the menu text's reveal (entry + 482 ... 509, which follows the
   polls) differ: of those 28 frames 8 equal a reference frame of the captured window 481-512 (486/487 and 485; 499/500 and 498;
   505/506 and 504 at a lag of one frame; 492/493 within a dwell run) and 20 equal none. The reveal ends before entry + 510, and the
   frames from there on are identical. The pre-registered frames lie after that convergence; the intermediate reveal is therefore
   **not** claimed time-aligned, and this one-frame poll-phase class is the first timing approximation a later time-sensitive
   checkpoint will meet.
5. *Run end.* The state dumps were taken at the same onset instants; the polling-loop pc classes are those of ADR 0145 / 0147.

## Checkpoint E regression under the new runtime

(Rebuilt from the cold cache with the changed ABI header; local, authentic.)

**T004 handoff regression** (the stop-enabled program of its three original extents, rebuilt: 14 rounds + confirming run, 804 s,
151,299 functions): `application_handoff`; main and sub registers all equal, Word RAM and PCM identical, 27 work-RAM and 44 PRG-RAM
bytes - the ADR 0144 / 0147 numbers; instruction starts equal except the entry instruction that the boundary withholds. **T005
interval** (60 frames, no backup RAM, six extents, rebuilt: 14 rounds, 733 s): no typed stop; executed instruction starts identical
to the reference (main 1,724 = 1,724; sub 3,519 = 3,519); PCM identical; work RAM 27 and PRG RAM 103 bytes, Word RAM the same
one-sector lead - the ADR 0145 / 0147 values. Synthetic profile and every SEG-062/063/064/065 fixture: the focused Sega CD suites
(24) pass.

## Observation beyond the checkpoint (not claimed)

A second START (held 6 frames from entry + 600), tried only to observe, in the reference: the title fades to black (about entry +
620-650), stays black for about 420 frames (loading) and then the first level appears (about entry + 1,070). Generated-native
(same schedule): the fade starts, its frames at entry + 610 / 620 differ from the reference's (the poll-phase class above), the black
frames at entry + 630 / 640 / 700 / 900 equal the reference's, and at about entry + 957 frames the main processor enters a further module
whose entry has no compiled authority: the typed stop `entry_not_compiled` at a main-processor JSR whose target lies in 2M Word RAM
(code the sub loaded from the disc by CDC transfer and that the main processor executes in place; extent images are mapped only into
work RAM and PRG RAM today). The reference's content-aware check also shows the sub then runs code from a PRG-RAM bank window region
whose source file is ambiguous from the fetched words alone. This is the earliest remaining frontier: Word RAM-resident executable
authority for the main processor plus the stage's sub bank. The reference, with RIGHT held for 300 frames once the first level is
playable, moves the player through the level (a local observation); the generated program cannot reach that point, so no gameplay claim
or generated-native observation exists.

## Consequences

- Checkpoint G is the first independently verified interaction with the authentic generated-native Sonic CD application: press
  START, the real game reads it through the modelled pad, reacts, and reaches the title menu, matching the reference.
- Further interaction (a second START: the game start and its loading, gameplay, audio) is new work. The input script, the extent,
  backup-RAM and capture mechanisms apply. The poll-cadence phase is the earliest known timing approximation that a later
  checkpoint with a time-sensitive animation (the menu reveal, the next screen's transitions) will meet first.
