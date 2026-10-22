# ADR 0176: SEG-071 - Word RAM executable authority, the stage's sub bank, the input-instant correction and the Checkpoint H result

- Status: Accepted (SEG-071-T001). Judged against the pre-registration of ADR 0175 (frames, schedule frame numbers and pass rule
  unchanged); corrects how the schedule's frame numbers are converted to machine ticks (decision 3), a convention inherited from ADR 0150. Decision 3 was found after generated runs; it is a fact of the reference
  implementation (its own code and pad log), and Checkpoint H is not claimed, so nothing rests on the change.
- Later (ADR 0177): the remaining difference was resolved - the sub/CD clock convention was necessary but not sufficient (the main
  processor's bus timing and the video phase were also missing) and Checkpoint H is achieved; the onset base `1197` of decision 3 became
  `1202` with the corrected video phase. The measurements below stand as recorded.
- Scope: the authentic BIOS and the Sonic CD (USA) application, generated-native, from reset through the second START, the level load,
  the first level and a 240-frame RIGHT hold, to entry + 1,600 frames; the comparison with the independent reference.
- Privacy: no BIOS or disc byte, hash, address, extent coordinate, file name, level name, frame image or digest appears here or in any
  test. Comparisons were made locally; only equality results, counts and classes are reported.

## Decisions

1. **Disc extents in the main processor's 2M Word RAM view.** A `--disc-image main:...` extent may now lie wholly inside `$200000-$23FFFF`
   (besides main work RAM); one that leaves the view, lies past it or below it, or names the sub at such a base is refused (exit 6). The
   rule is the established one (ADR 0144/0145/0147): a range of the verified disc read at build time, admitted by broad AOT, every
   instruction byte-verified against the live view before it runs. Whether main may fetch it is decided per instruction by the live view
   (2M with `RET` = 1, or the 1M bank main sees): when the sub owns Word RAM main's fetch fails closed (`code_bytes_mismatch`), as section
   15 of the machine contract already records for the sub. No RAM snapshot is an authority. Synthetic fixture:
   `tests/sega_cd_bios_wordram_extent_test.py` (a project-authored IP copies a routine into Word RAM and calls it: execution, a BIOS
   service through the mutable thunk and back, a data write into the image, the owner rule (DMNA before the call), a corrupted live byte,
   no extent / wrong disc bytes, the four refusals and the accepted end-of-view extent, determinism over eight deadline partitions).
2. **The frontier, classified.** A content-aware check (ADR 0147 decision 1) of every instruction-stream word the reference fetched from
   RAM up to entry + 1,400 finds exactly two new executable regions after the second START: the stage's main module, one whole disc file
   that the sub places in 2M Word RAM and main executes in place (Word RAM equals the file byte for byte while the level runs), and the
   stage's sound-bank module at a base in sub PRG RAM. The latter is the "source file ambiguous from fetched words alone" of ADR 0150:
   six sibling files match every fetched word but one, and exactly one matches all, so the content-aware rule selects it. With the ten
   earlier extents (re-derived by the same check: they were not recorded), the verified sub epoch and the operand templates, every fetched
   word is explained. Twelve extents (limit 16); the build fixed point with the full input schedule converged in 16 rounds (1,042 s,
   395,207 instruction functions, 399 MB of C in 97 translation units, peak compiler resident set 855 MB) and the confirming run reaches
   entry + 1,600 with no typed stop.
3. **Input instants: the reference presses at the onset that opens frame entry + N.** The private reference applies a scheduled button from
   the start of its frame `entry + N - 1`, which begins at a vertical-blank onset; its pad log confirms the first read of the button in the
   vertical-blank interval after that onset - the onset that *opens* reference frame entry + N (= closes entry + N - 1). ADR 0148
   decision 2 and ADR 0175 decision 2 describe it as the onset that *closes* entry + N, and ADR 0150 decision 1 converted it that way
   (`onset --base-onsets 1197 --press N`), so every generated press was one frame after the reference's. This is a fact of the reference
   implementation, established from its own code and pad log, independent of generated output. The generated schedule is therefore
   `onset --base-onsets 1197 --press N-1` for every registered N (START 479 / 599, RIGHT 1259, holds unchanged). The registered frame
   numbers, compared frames and pass rule are unchanged.
4. **Consequence for Checkpoint G (a post-hoc re-verification under the corrected conversion, same program; ADR 0150's result stands as recorded).** Frames entry + 510 / 540 / 600 identical, VRAM and Word
   RAM identical, work RAM 16 bytes (was 20-25) and PRG RAM 137-138 bytes (ADR 0145/0147 classes); the polls reading START are at entry +
   480, 482, 483, 485 - **exactly** the reference's (ADR 0148 decision 4), where ADR 0150 reported 481, 482, 484, 485. The per-frame
   port-1 poll counts are identical to the reference's in every frame from entry + 65 to 950 except one. ADR 0150 decision 4's
   "poll-cadence phase" was this schedule offset (with a one-frame labelling offset in its comparison), not a CD-sector phase.

## Result

**Checkpoint H: not achieved** (neither `TIME_ALIGNED` nor `EVENT_ALIGNED`, ADR 0175 decision 8). Checkpoint G remains the last passing
checkpoint (re-verified, decision 4).

| check (corrected schedule, same program; reference: two runs identical) | result |
| --- | --- |
| executable authority | every executed instruction a compiled, byte-verified entry; no typed stop to entry + 1,600 |
| level reached | yes: the level fades in, its title card, the running timer, the player |
| Checkpoint G frames entry + 510 / 540 / 600 | identical (decision 4) |
| RIGHT observation | the 240 polls reading RIGHT are in exactly the reference's frames (entry + 1,260 ... 1,499); poll sequence 1,250-1,510 identical |
| player object state (the player's object record) | **byte-identical** to the reference at every sampled frame entry + 1,260 ... 1,500 (13 frames): the player accelerates, runs and scrolls exactly as in the reference |
| canonical frames 1,150 / 1,250 / 1,300 / **1,400** / 1,500 | differ in 1,241 / 1,052 / 963 / 966 / 1,228 pixels (of 71,680); at +-1 frame the RIGHT frames differ in ~42,000, so the play is time-aligned; the differing pixels are the timer's centiseconds, the ring animation phase and animated scenery |
| determinism | one outcome (report and every capture) over 2 repeats and 8 seeded deadline partitions |
| Checkpoint F (same program, no input) | entry + 420 / 480 / 540 identical; VRAM and Word RAM identical; work / PRG RAM the ADR 0147 classes |

**Typed difference: a sub/main ordering race at the stage load, decided by the sub/CD clock convention.** After the stage's files are read
(the last CDC sector interrupt within one frame of the reference's), the sub initialises the sound bank by clearing the PCM wave RAM byte by
byte (about eight frames, on both sides) and main sends the sub a command through the communication registers and waits for its status.
In the reference the clear has begun when the command arrives: main waits from entry + 957 to 964 and then decompresses the stage. In the
generated run main reaches the stage module about one frame earlier, the command arrives before the clear begins and is answered at once:
the stage's main work starts at entry + 956, about seven frames early, and the level (fade-in, title card, timer start, ring and scenery
animation counters) runs about eleven frames ahead of the reference, while the player, who stands still until RIGHT, is unaffected. The
one-frame lead traces to the CD drive's 75 Hz clock: the reference derives the sub clock from the video line (796 sub cycles per line,
about 12.497 MHz) and quantises the drive tick to line boundaries, so its 75 Hz period is about 209.38 lines; this runtime uses exact
12.5 MHz / 75 Hz (209.33 lines, ADR 0144). The accumulated phase moves by about 44 lines between entry + 300 and + 1,000 (measured: the
reference's tick instants against the grid fitted to the generated run's level-4 interrupts). Changing only the drive period to the
reference's average (a local relink experiment) does not change the outcome and perturbs Checkpoint G state, so the difference is the
clock-domain convention as a whole (sub clock, drive tick quantisation, timer), not one constant. The real drive and sub clocks are
asynchronous to the video, so neither convention is a hardware fact; reproducing the reference's ordering is a convention decision with
consequences for every earlier Sega CD checkpoint, and is left to a successor.

Also measured and excluded as causes: VDP DMA from Word RAM (none in the level; every transfer is from work RAM), DMA fill duration (three
fills in the load window, about one frame), the VDP write FIFO (stalls total about 1.3 frames, none before the level), main instruction
rate (about 1.6 % above the reference in the decompression, about 1.5 frames over the whole stage).

## Checkpoint E

Not rebuilt. The runtime is unchanged by this task; the emitter change only admits extents that were refused before, so every earlier
program's inputs produce the identical program. The synthetic profile and the focused Sega CD suites pass.

## Consequences

- The generated-native Sonic CD reaches its first level from authentic BIOS boot, and under a held RIGHT the player's state equals the
  independent reference's at every sampled frame. The frame-level checkpoint is not claimed; its blocker is a timing-convention decision, not executable authority.
- The schedule conversion of decision 3 applies to every later timed-input checkpoint.
