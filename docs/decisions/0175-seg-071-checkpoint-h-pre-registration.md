# ADR 0175: SEG-071 - Checkpoint H pre-registration: the second START to the first playable level and a timed RIGHT hold

- Status: Accepted (SEG-071-T001). Fixed from the independent reference before any generated-native execution past the SEG-070 frontier
  (`entry_not_compiled` at about entry + 957) and before any generated-native run with the schedule below. It may only be made stricter
  later; any other change invalidates Checkpoint H.
- Scope: what Checkpoint H injects, when, what the independent reference shows, what the generated program must reproduce and what
  counts as a pass. It claims nothing about the generated program.
- Privacy: no BIOS or disc byte, hash, address, file name, level name, frame image, frame digest, controller trace or disassembly
  appears here. Controller values are the hardware-defined values of a standard pad. The reference captures are local and ignored;
  durable evidence records equality results, counts and classes only.

## Context

Checkpoint G (ADR 0148-0150) reproduced the title menu after one START. ADR 0150 observed, without claiming it, that a second START in
the reference fades the title out, loads for about 420 black frames and shows the first level, and that the generated program stops
at about entry + 957 with `entry_not_compiled` at a main-processor call into 2M Word RAM. Checkpoint H asks whether the generated
program reaches that level and then responds to a held direction as the reference does.

All measurements come from the private Genesis Plus GX reference (the SEG-065/070 local harness, unchanged except that its existing
second-START and RIGHT schedules are used) under exactly the ADR 0146/0148 preconditions.

## Decisions

1. **Preconditions.** ADR 0148 decision 1 unchanged: the authorized US Model 1 class BIOS, the Sonic CD (USA) disc, empty cartridge
   slot, formatted empty 8 KiB internal backup RAM, port 1 a standard 3-button pad, port 2 a standard pad never pressed. The application
   entry and the frame length (448,020,000,000 ticks) are those of ADR 0145; the onset convention is ADR 0147 decision 6.
2. **Input schedule (port 1 only; three press/release pairs, no other button or port ever).**
   - START, pressed at the vertical-blank onset closing reference frame entry + 480, held 6 frames (ADR 0148 decision 2, unchanged).
   - START, pressed at the onset closing entry + **600**, held **6** frames (the schedule ADR 0150 used for its observation).
   - RIGHT, pressed at the onset closing entry + **1260**, held **240** frames (released at the onset closing entry + 1500).
   - Ordering and tick conversion: ADR 0148 decision 2 and ADR 0150 decision 1 (`tools/sega_cd_input_script.py onset --base-onsets
     1197`), computed from these frame numbers alone, never adjusted from generated output.
3. **What the reference shows.** After the second START the title fades to black (about entry + 620-650) and stays black while the
   level loads; the level appears from about entry + 1,070, first under its title card, and the card is gone and the level timer is
   running by entry + 1,250. Without RIGHT the player stands at the start position through entry + 1,560 (observed). With RIGHT the
   player runs, the view scrolls and rings are collected.
4. **Controller protocol in the level (reference).** In the level the application reads both pads once per video frame, inside the
   vertical-blank interval right after the onset, with the ADR 0148 decision 3 sequence (TH low, read, TH high, read). Released values
   are `$33`/`$7F`; with RIGHT held the TH-high read is `$77` (bit 3, the Right line, low) and the TH-low read is unchanged. The first
   poll reading RIGHT is in the press frame (entry + 1,260), the last in frame entry + 1,499; the poll in entry + 1,500 reads `$7F`.
   Unlike the title (ADR 0150 decision 4) these polls are locked to the video frame, so the one-frame poll-cadence phase is not expected
   here; it is not excluded and would be classified under decision 8.
5. **Selected frames (canonical form of ADR 0146 decision 5).** Level: entry + **1,150** (title card over the level), **1,250**
   (primary level frame: card gone, timer running, before RIGHT). Directional input: entry + **1,300**, **1,400** (primary RIGHT frame)
   and **1,500**. All five are H40 320 x 224, normal mode, 34-40 distinct canonical colours and 71,680 non-black pixels (non-triviality:
   at least 32 colours and 40,000 non-black pixels).
6. **Falsifiability.** The frames at entry + 1,150 and 1,250 are identical with and without RIGHT; the RIGHT frames differ from the
   no-RIGHT reference at the same index in 28,921 (1,300), 58,321 (1,400) and 62,673 (1,500) pixels. A generated run that stays on the
   loading screen, stops, shows another level, or ignores RIGHT fails; a run in which the player moves at another speed, starts a frame
   early or late, or scrolls differently fails the RIGHT frames.
7. **Compared state.** At each selected frame: canonical-frame digest and dimensions; VDP registers, CRAM, VSRAM, VRAM; both CPUs'
   registers (program-counter classes); work, PRG and Word RAM; the controller observations (per frame the port-1 read values, against
   decision 4); the executed instruction starts of both processors from reset to the onset closing entry + 1,500, classified, each a
   compiled, byte-verified entry. Asynchronous differences are classified as in ADR 0147/0148 (polling-loop pc and the data it alone
   writes, stack residue below the stack pointer, the BIOS status and sub-stack bytes, sound-driver state that no frame reads). Anything
   else is reported with its typed difference.
8. **Pass rule.** `TIME_ALIGNED` (Checkpoint H): canonical digests equal at the primary frames entry + 1,250 and 1,400, **and** at
   1,150, 1,300 and 1,500, **and** the Checkpoint G frames (entry + 510 / 540 / 600) equal in the same run, **and** RIGHT read in the
   reference's poll frames (decision 4, +-1 poll classified). `EVENT_ALIGNED` (recorded with its limitation): equality at the five
   frames with one constant whole-frame offset `|d| <= 2` whose cause is classified (for example the CD-sector phase of the load).
   Anything else is not Checkpoint H; it is reported with the typed difference and the last passing checkpoint (G) is preserved.
9. **Determinism and regressions.** The reference is repeated (two runs identical at every selected frame and in the controller log;
   measured, identical). The generated program: two repeats and eight deadline partitions with one outcome; Checkpoints F and G
   reproduced by the same program with their own schedules (no input; one START); Checkpoint E unchanged.
10. **Executable authority.** Any new executable region uses the established disc-extent mechanism (ADR 0144/0145/0147): a range of the
    verified disc read at build time and mapped at its execution base, verified per instruction against live bytes. No executable
    authority comes from a RAM snapshot. If Word RAM needs extent support, its rule and synthetic fixtures are part of the task.

## Consequences

- If Checkpoint H passes, the generated-native Sonic CD is playable to the extent that a held direction moves the player as in the
  reference over the registered interval. Audio output, other levels and full gameplay remain outside it.
