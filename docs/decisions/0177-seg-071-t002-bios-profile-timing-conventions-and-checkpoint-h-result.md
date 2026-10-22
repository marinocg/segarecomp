# ADR 0177: SEG-071-T002 - the BIOS-image profile's timing conventions (line-locked sub clock, main bus, VDP port, video phase) and the Checkpoint H result

- Status: Accepted (SEG-071-T002). Judged against the pre-registration of ADR 0175 (frames, schedule frame numbers and pass rule
  unchanged; ADR 0176 decision 3's input instants unchanged). Decision 1 (the convention for the sub/CD clock) was written down before
  any generated run with it; decisions 2-8 were added when the lockstep comparison of the two processors' executed instruction streams
  (decision 9) showed which further conventions the reference applies; no constant was fitted to a frame - each is the reference's
  own source value (cited), and decision 9 lists which of them the five Checkpoint H frames actually need.
- Scope: the BIOS-image profile only (authentic BIOS boot). The synthetic (disc-placed) profile keeps the exact 12.5 MHz sub clock,
  the exact 75 Hz drive grid, the 224-line onset phase and the flat interrupt cost, so every synthetic fixture is unchanged.
- Privacy: no BIOS or disc byte, hash, address, extent coordinate, file name, level name, frame image or digest appears here or in any
  test. Comparisons were made locally; only equality results, counts and classes are reported.

## Context

ADR 0176 measured that the generated-native Sonic CD reached the first level but ran about eleven frames ahead of the independent
reference, traced the lead to the stage load's sub/main ordering race, and attributed that to the sub/CD clock-domain convention.
That attribution was correct but incomplete: the line-locked clock removes most of the lead (it is necessary), the rest comes from
the main processor's own bus timing and from the video phase, which the profile did not model. Both processors are driven by the
reference per video line (`scd_update`), so a convention that is *not* a hardware fact (the real clocks are asynchronous) still has to
be reproduced for the orderings the application depends on to come out the same; that is what the checkpoint compares.

## Decisions

1. **Sub clock and drive tick (line-locked).** The sub receives a fixed 796 cycles per video line (`cycles_per_line`), so one sub cycle
   is 2,148,241 ticks (1,709,999,836 per 1,710,000,000-tick line, relative error 1e-7). The 75 Hz drive accumulator gains 9,552 per line
   and fires at 2,000,000; it is examined only at line ends, so tick `k` falls at the end of the first line `n` after the anchor's line
   with `acc0 + 9,552 n >= 2,000,000 k`; the anchor is power-on (`acc0 = 0`) or a peripheral reset (`acc0 = -12` x the sub cycles
   already run in that line, the reference's `cdd.cycles = (scd.cycles - s68k.cycles) * 3`). Timer, stopwatch, CDC DMA and decoder and
   graphics durations are counted in these cycles. (`scd_sub_cycle_ticks`, `scd_cdd_anchor`, `scd_cdd_grid_after`.)
2. **Main bus refresh delay.** An instruction about to execute at or after the refresh deadline loses 2 cycles (14 master clocks); the
   next deadline is 128 cycles (896 master clocks) after that instant; the first deadline is the next multiple of 896 after the reset.
   (Reference: "68K bus access refresh delay (Mega Drive / Genesis specific)", `m68kcpu.c` and `m68k.refresh_cycles`.) An instruction
   that follows its own cycles - a register-count shift or rotate (2 cycles per count) or MOVEM.L (8 per register) - whose start plus
   those cycles has reached the deadline moves the deadline on by one period (`SKIP_BUS_REFRESH`); division is in the same class in the
   reference and is not modelled (its cycles follow the operand values).
3. **Z80 window.** Each main bus access to `$A00000-$A0FFFF` takes one more cycle (7 master clocks; reference `z80_read_byte` /
   `z80_write_byte`, "Z80 bus access latency").
4. **Video phase.** The main processor leaves reset 229 lines + 1,004 master clocks into the 262-line frame whose origin is the
   vertical-blank onset (the reference's hard-reset point, measured on hardware by the HV-counter value read after power-on, GPGX
   `gen_reset`), rounded down to a multiple of 7: 784,182 master clocks. The first onset is therefore 896,040 - 784,182 = 111,858
   master clocks after power-on (the earlier profile placed it at line 224, 766,080), later onsets one frame (448,020,000,000 ticks)
   apart, and the reference's frame `f` begins at onset `f` (the old numbering was five behind). The vertical interrupt is requested
   788 (H40) / 770 (H32) master clocks after each onset (`VINT_Hxx_MCYCLE`); `F` is set with it. The entry-relative schedules of ADR 0148
   / 0150 / 0176 therefore use `--base-onsets 1202` (the entry frame) where they used 1197, and `tools/sega_cd_input_script.py` places
   onset 1 at 111,858 master clocks.
5. **Interrupt exception cycles.** An autovector interrupt exception takes 50, 59, 58, 57, 56, 55, 54, 53, 52 or 51 cycles by the
   processor's own cycle count within the frame modulo 10 (MC68000 User Manual appendix B: 44 cycles plus wait states for the alignment
   with the E clock; the reference's table `m68ki_cycle_interrupts`, 6..15 by the manual, 5..14 measured); the sub uses the same
   table (reference `s68kcpu.c` shares the file), counted in sub cycles.
6. **VDP data port and fill.** The data-port write FIFO (four entries, read-out slots per line in H32 / H40, a VRAM word two slots, a
   CRAM/VSRAM word one; emptied at each frame's start; restricted in the active display only) stalls the processor until the oldest
   entry is read out; a status read returns the FIFO full / empty bits and the DMA-busy bit; a VRAM fill has the duration of the slot
   model (rate per line 16 / 18 active, 166 / 204 blanked, plus the initial data-port write) and the busy bit lasts as long. (Reference
   `vdp_68k_data_w_m5`, `vdp_68k_ctrl_r`, `vdp_dma_update`.)
7. **Interrupt latency.** Enabling the vertical interrupt (register #1 IE0) while one is pending, from a MOVE that is not a MOVE.L,
   lets the next instruction execute before the interrupt is recognised (reference `m68k_set_irq_delay`).
8. **Not modelled, recorded.** The reference's idle-skip of a Gate Array poll loop (its clock jumps to the writer's time) and its
   processor synchronisation at line chunks; the cycles of DIVU/DIVS in the refresh-skip rule; the exact intra-instruction time of a
   VDP status read (taken as the instant 8 cycles after the instruction starts; the reference adds the opcode's own base cycles, so a
16-cycle form is 8 cycles off at FIFO and busy-bit edges); the guest-visible VBLANK, HBLANK and odd-field status bits and the HV
counter, which the shared Genesis host still derives from the old 224-line phase (the frames compared do not read them); the register
#0 IE1 latency path; aborting a fill by a register write. The instruction streams agree for the span stated in decision 9 and the table.
The sub cycle (decision 1) is an integer 2,148,241 ticks against the exact 2,148,241.206: the sub runs ahead of the line grid by about
0.2 ticks per cycle, 54 sub cycles (232 master clocks) by entry + 1,500; the checkpoint outcome shows this margin does not flip the
orderings compared, a race closer than that could.
9. **Acceptance and method.** Checkpoint H as ADR 0175 decision 8 (`TIME_ALIGNED`), Checkpoints E, F and G re-verified with the same
   program, determinism (two repeats, eight seeded deadline partitions), synthetic fixtures for every changed rule
   (`tests/sega_cd_timing_convention_test.py`). ADR 0175 decision 7's full compared state (VDP registers, CRAM, VSRAM, VRAM and RAM per
   selected frame, executed instruction starts to entry + 1,500) was not compared beyond the canonical frames, the controller log, the
   Checkpoint E state and the instruction-start lockstep to frame 129. Decision 1 was fixed before generated runs; decisions 2-8 were
   found by lockstep against the reference's source and are modelling conventions (including the round-down to a multiple of 7 of the
   reset point and the 8-cycle status read), none fitted to a frame. Method: the executed main-processor instruction starts (program counter and start time)
   of the generated run and of the reference were compared in lockstep from reset; each divergence named the next convention; the lists
   above are the divergences found, in the order they appeared.

## Result

**Checkpoint H: achieved, `TIME_ALIGNED`** (ADR 0175 decision 8), under the pre-registered schedule and frames, with no constant fitted.

| check (generated-native from authentic BIOS boot; reference: two runs identical) | result |
| --- | --- |
| canonical frames entry + 1,150 / 1,250 / 1,300 / 1,400 / 1,500 | **identical** (ADR 0176: 963-1,241 differing pixels of 71,680 each) |
| Checkpoint G frames entry + 510 / 540 / 600, same run | identical |
| Checkpoint F frames entry + 420 / 480 / 540 (no input) | identical |
| RIGHT observation | the 240 data-port reads returning `$77` are in exactly the reference's frames (entry + 1,260 ... 1,499); the multiset of port-1 reads is equal in every frame from entry + 65 to 1,510 |
| Checkpoint E: handoff and the 60-frame interval | both processors' registers identical (the earlier result left the sub two instructions apart in a poll loop); Word RAM and PCM RAM identical; executed instruction starts equal (main 1,724, sub 3,519, sets equal); work RAM 5 bytes and PRG RAM 21 bytes differ (earlier 28 and 44), the ADR 0145/0147 asynchronous classes |
| application entry | the generated entry instruction starts within 205 master clocks (30 main cycles) of the reference's, 1,202 frames after power-on |
| main-processor instruction starts, reset to frame 129 (about 680,000 instructions) | identical program counters; start times equal to a constant offset (the reference's first instruction slot) to within the 14-master-clock refresh phase; the first divergence is one instruction in a Gate Array poll loop at frame 129 (decision 8) |
| determinism | one outcome (report and every capture) over 2 repeats and 8 seeded deadline partitions |

Which conventions the frames need (each removed alone, same program and schedule, differing pixels of 71,680 at entry + 510 / 540 / 600
and 1,150 / 1,250 / 1,300 / 1,400 / 1,500): with the exact sub clock and exact drive grid 11,114 / 10,771 / 11,184 and 201 / 247 / 11 /
769 / 53; without the refresh delay 8,760 / 7,962 / 8,622 and 0 / 12 / 786 / 12 / 15; without the Z80-window latency 0 / 0 / 0 and 0 / 19 /
776 / 19 / 48; without the VDP FIFO and fill 0 / 0 / 0 and 0 / 220 / 977 / 220 / 48. The refresh-skip rule, the interrupt latency, the
vertical-interrupt delay and the interrupt exception cycles do not change these frames; they are kept because they are the reference's
own rules for the hardware behaviour they model, and the two processors' instruction streams stay in lockstep only with them (without the
exception cycles the start times differ from the first interrupt, frame 70; without the refresh-skip rule the first program-counter
divergence is at frame 128, with all of them at frame 129).

## Consequences

- The generated-native Sonic CD reaches its first level and a held RIGHT moves the player as in the reference over the registered
  interval, frame-identically at the five pre-registered frames.
- Entry-relative machine schedules use onset numbering equal to the reference's frame numbering (`--base-onsets 1202`); ADR 0176
  decision 3's conversion (press at the onset that opens the frame, `--press N-1`) is unchanged, its `1197` becomes `1202`. Earlier
  results stand as recorded: they were measured under the old phase and have been re-verified above.
- The profile now charges the main processor's bus conventions; a title whose timing depends on a main-bus effect not modelled here
  (division in a refresh-skipping position, the reference's poll-idle jumps) can differ by a refresh phase; the lockstep tool of
  decision 9 finds such a divergence directly.
