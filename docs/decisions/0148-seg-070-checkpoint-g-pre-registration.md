# ADR 0148: SEG-070 - Checkpoint G pre-registration: one START press on the Sonic CD title screen, its target transition and comparison rule

- Status: Accepted (SEG-070-T001). Fixed before any runtime input work and before any generated-native execution with a controller
  input. It may only be made stricter later; any other change invalidates Checkpoint G.
- Scope: what Checkpoint G injects, when, what the independent reference shows in response, what the generated program must reproduce
  and what counts as a pass. It claims nothing about the generated program.
- Privacy: no BIOS or disc byte, hash, address, frame image, frame digest, controller trace or disassembly appears here. Controller
  register values quoted below are the hardware-defined values of a standard pad, not commercial data. The reference captures are local
  and ignored; durable evidence records equality results, counts and classes only.

## Context

Checkpoint F (ADR 0146/0147) reproduced the Sonic CD (USA) title screen time-aligned with the independent reference with no controller
input. Checkpoint G asks what the next interaction does: one START press, supplied as a timed machine input, observed by the real
application through modelled controller hardware, and followed to the first independently identifiable next state.

All measurements below come from the private Genesis Plus GX reference (local harness, ignored; additions for this task: an
entry-relative START schedule, a log of main-processor accesses to the I/O register block, nothing else) under exactly the ADR 0146
preconditions. They were made before any generated-native input work existed.

## Decisions

1. **Preconditions.** ADR 0146 decisions 1 and 2 unchanged: the authorized US Model 1 class BIOS, the Sonic CD (USA) disc, empty
   cartridge slot, formatted empty 8 KiB internal backup RAM. Controller port 1 is a standard 3-button pad, port 2 a standard pad with
   no button pressed for the whole run. The application entry and the frame length (448,020,000,000 ticks) are those of ADR 0145.
2. **Input schedule.** Exactly one event pair on port 1: START pressed, then released; no other button, no other port, ever.
   - Press: the vertical-blank onset that closes reference frame *entry + 480* (the 480th onset after the application entry, the onset
     convention of ADR 0147 decision 6). It is also the Checkpoint F primary frame, whose own content is therefore not affected.
   - Release: the onset that closes reference frame *entry + 486*. The button is held **6 NTSC frames** (6 x 448,020,000,000 ticks).
   - Ordering at an instant: the pad state changes before any processor access or interrupt acceptance at or after that instant (the
     reference refreshes its inputs at the vertical-blank onset, before the vertical interrupt and the rest of that line). The reference
     input is frame-granular; a machine-tick schedule reproduces it by placing both transitions on the onset instants.
   - The schedule is locked here, before generated-runtime tuning, and is not retuned from generated output.
3. **Controller protocol visible to the game (reference).** The application reads both data ports with one standard sequence: write
   data `$00` (TH low), read the data byte, write `$40` (TH high), read the data byte, for port 1 and then port 2, about two polls in
   three video frames (the first poll after the entry is in the frame entry + 65; 283 port-1 and 236 port-2 polls from the entry through entry + 649). The
   control registers are set to `$40` (TH output) three times (once before the entry by the BIOS, twice after); data register 1
   receives `$C0` twice. Released values are `$33` (TH low) and `$7F` (TH high); with START held the TH-low read is `$13` (bit 5, the
   Start line, low) and the TH-high read is unchanged. No other value was ever read from either port; no 6-button, multitap or other
   protocol access occurs. This is the existing Genesis pad model (TH multiplexing, direction registers, 0 = pressed).
4. **First application-visible input read.** In the reference, the first poll after the press returns `$13` in the same video frame as
   the press (the poll follows the onset within the vertical-blank interval of frame entry + 480). Four polls observe START: in the
   frames entry + 480, 482, 483 and 485; the polls after the release read `$33`/`$7F` again. The application reads no pad before the entry
   + 65 poll, so no earlier pad observation exists to compare.
5. **Resulting transition and target state.** START does not begin gameplay: the
   title screen's "press start" prompt is replaced by the title **menu** (a first menu entry with a blinking selection marker), and
   the title animation continues. The menu text is present from entry + 510 and stays unchanged (apart from the marker blink) through the
   measured window (reviewed through entry + 640). Relative to the no-input run the main processor executes 120 further instruction starts
   in the window up to entry + 600, the sub processor none. The first
   meaningful stable post-START state is therefore "title menu displayed". Starting the game (a second START) and everything after
   it are later work and not claimed.
6. **Target and selected frames.** The frames completed at application entry + **510, 540 (primary) and 600** NTSC frames, canonical
   form of ADR 0146 decision 5. All three are H32 256 x 224, normal mode only, 38 distinct canonical colours and between 53,824 and
   53,827 non-black pixels (non-triviality: at least 32 colours and 40,000 non-black pixels). Each differs from the no-input reference
   frame at the same index in exactly 559 to 562 pixels, all in one band of 11 text rows (the prompt/menu text); the animated background
   is otherwise identical to the no-input run at these three frames, but not at every frame (the START run's animation phase differs
   from the no-input run at some intermediate frames, a timing effect of the reaction that the schedule fixes). The frames at entry +
   420 and 480 equal the no-input frames exactly.
7. **Acceptance of the target (falsifiable).** A generated run reproducing the no-input title (Checkpoint F) fails: it differs from the
   reference at all three selected frames. A generated run that shows the menu but a different animation phase fails the primary frame.
   The four conditions of the milestone are: START observed by the real application (>= 1 pad read returning the pressed value, at the
   reference frames of decision 4), the title idle behaviour left, the menu reached, the canonical frame equal at the selected frames.
8. **Compared state.** At each selected frame: canonical-frame digest and dimensions; VDP registers, VRAM, CRAM, VSRAM; both CPUs'
   registers (program-counter classes); work, PRG and Word RAM; the device events that explain the frame (VDP DMA, CRAM/VSRAM writes,
   graphics-ASIC operations, Word RAM ownership changes, interrupt events); the controller observations (per video frame the count of
   port-1/port-2 data-register reads and their value set, against decision 3/4); the set of executed instruction starts of both
   processors from reset to the onset closing entry + 600, classified (BIOS-derived against application; compiled, guard-verified entry
   for each). Asynchronous differences are classified as in ADR 0147: the program counter inside a short polling loop and the data it
   alone writes, stack residue below the stack pointer, the BIOS status and sub-stack bytes, at most one poll across a frame boundary.
   Anything else is reported with its typed difference.
9. **Pass rule.** `TIME_ALIGNED` (Checkpoint G): canonical digests equal at the primary frame entry + 540, **and** equal at entry + 510
   and 600, **and** the no-input regression frames (entry + 420, 480) equal in the same run, **and** the controller observations of
   decision 4 reproduced (pressed polls at the reference frames, +-1 poll classified). `EVENT_ALIGNED` (recorded with its limitation):
   equality at all three frames with one constant whole-frame offset `|d| <= 2` whose cause is classified. Anything else is not
   Checkpoint G; it is reported with the typed difference and the last passing checkpoint (F) is preserved.
10. **Determinism and regressions.** The reference capture is repeated: two runs are identical at every selected frame, in the
    controller log and in the instruction-start sets (measured, identical). The generated program: two repeats, eight deadline
    partitions with one outcome; the original no-input title checkpoint (Checkpoint F) and the Checkpoint E interval unchanged under the
    new runtime.

## Consequences

- The runtime needs one machine input the SEG-065 program lacks: a timed standard-pad state, applied on the existing Genesis pad
  contract. Its design and fixtures are SEG-070-T002 (ADR 0149), independent of this schedule.
- If the generated program needs further executable regions for the 120 main instruction starts or any other frontier, the
  established extent mechanism applies; no executable authority comes from a RAM snapshot.
