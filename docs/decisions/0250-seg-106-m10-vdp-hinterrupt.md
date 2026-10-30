# ADR 0250: SEG-106 M10 - Sega CD VDP horizontal interrupt (level 4)

- Status: Accepted contract + implementation (SEG-106 / M10, one combined PR; internal tasks T001-T005). Decision: see "Decision" (filled at T005).
- Related: ADR 0249 (M9; its `GFX_SPECIAL_STAGE_GO` is unchanged, it named this frontier), ADR 0177 (VDP frame phase and the vertical interrupt, `main_irq_delay`), ADR 0147 (frame capture). M9 (PR #170) was verified merged before this work began.
- Privacy: counts, relative frame numbers and classes only; no ROM bytes, traces, framebuffers. Full evidence is in the ignored `.cache/seg106`.

## T001 - the reference contract (gate `HINT_CONTRACT_GO`)

Sources read (observations, no code copied): Genesis Plus GX `core/system.c` (`system_frame_scd`), `core/vdp_ctrl.c` (`vdp_reg_w` case 0, `vdp_68k_irq_ack`) and `core/m68k/m68kcpu.c` (`m68k_update_irq`, `m68k_set_irq_delay`); MAME `315_5313.cpp` (`vdp_reg_w`/`vdp_handle_scanline_callback`, `m_irq4counter`, `m_irq4_pending`). The project's Genesis runtime (SEG-055) is an architectural precedent, not an oracle.

| Question | Verified answer (source) |
|---|---|
| counter step | once per scan line, independent of register #0 IE1 and of display enable. Active lines 0-223: counter == 0 -> reload from register #10 and latch the request, else decrement (GPGX `system_frame_scd`; MAME decrements and latches at -1 on the same lines, one line later in its arbitrary-timing model - the period `reg10 + 1` and the line count per frame agree) |
| first vertical-blank line (224) | latch if the counter is 0; no decrement, no reload (GPGX). MAME's step at its line 224 is the same check |
| other blank lines | nothing; the last line (261) reloads the counter from register #10 (GPGX, MAME "else reload") |
| reset | counter `$FF`, nothing pending, register #10 = 0 (GPGX `vdp_reset`) |
| latch vs IE1 | the latch is set whether or not IE1 is set (GPGX `hint_pending = 0x10` outside the `reg[0] & 0x10` test; MAME `m_irq4_pending = 1` likewise) |
| IE1 enabled with a latch pending | the request rises; GPGX delays it by one instruction (`m68k_set_irq_delay`, MOVE.W-class writes only, not MOVE.L, and not while an enabled vertical interrupt is pending); MAME delays 16 clocks. Product: `main_irq_delay = 1`, the existing ADR 0177 mechanism |
| IE1 cleared with a latch pending | the request drops, the latch stays; enabling again requests again (GPGX `vdp_reg_w` case 0, MAME) |
| priority | level 6 over level 4 (GPGX `vdp_68k_irq_ack`: the vertical interrupt is acknowledged first when `reg[1] & vint_pending`; level 4 is re-asserted after it while the latch is set) |
| acknowledge | clears the latch of the interrupt taken (level 4 -> hint latch); level 6 -> the vertical flag; the other source is untouched. A latch set between request and acknowledge merges into the one acknowledged |
| masked request | stays pending (level-triggered), taken when the SR mask falls below 4; STOP wakes on it |
| vector | level 4 autovector (vector 28) = `$FFFF:$A12006` in the BIOS profile (already implemented, ADR 0142) |
| frame origin | GPGX's frame begins at the first vertical-blank line (its `system_frame_scd` order: blank lines, then active 0-223); the product's frame origin is the vertical-blank onset (`vdp_onset - SCD_FRAME_TICKS`), so lines 224...261, 0...223 start at multiples of 3,420 master clocks from the first onset |

Discrepancy kept explicit: MAME's counter phase is one line later than GPGX's relative to the line-0 render; GPGX is the reference the authentic comparison uses and its order is what is implemented. Not modelled and NOT guarded by a new typed stop (the 262-line / line-224 geometry is fixed, as for the existing vertical interrupt; a BIOS that selects V30, Mode 4 or interlace would get a wrong H-INT phase silently - a documented limitation, to be stopped when a target uses it): V30, Mode 4, interlace, HV-counter reads, the Z80 H-INT line. The counter syncs at the instruction's start tick (`m->now`), like the vertical interrupt; a line starting inside the writing instruction is stepped with the old register values (instruction-granularity approximation, review minor).

Unresolved/limitation: the exact cycle of acceptance inside a line follows the existing boundary-sampled interrupt model (instruction granularity, ADR 0177), as for the vertical interrupt; GPGX's 1-instruction IRQ latency is reproduced only for the case above.

Frontier reproduction (frozen M9 executable, unchanged input schedule): R0 `interrupt_source_unsupported` at the main MC68000 control-port write of register #0 IE1 at entry+18,057; R1 at entry+31,168 (see T004).

## T002 - counter and event source (scd_vdp.c)

One 8-bit counter, one latch, one cursor (`hint_next`, the next un-stepped line on the first-onset grid) and one scheduled tick (`hint_at`, the next latching line, only while IE1 is set and no request is pending) in `ScdMachine`. Lines are a pure function of the deterministic tick. The counter is brought up to date lazily - before every control-port write (so register #10 and IE1 changes apply from the right line), at every level-4 acknowledge, and by the single `hint_at` event - and a run of whole frames from the counter's reload state is skipped in one step. No second scheduler, no per-tick work, no dependence on sampling instants (the eight deadline partitions and the fixed slicing of the generated-native fixture give identical reports). The typed stop at the IE1 write is removed; the classifier's other result (a `101x xxxx` word outside a command) and every other `vdp_access_unsupported` stay.

## T003 - level-4 delivery (scd_vdp.c, scd_machine.c unchanged)

`scd_main_request_level` returns 6 for the vertical request, else 4 for latch AND IE1; `scd_main_wake_level` 6, else 4 while IE1 is set; `scd_main_interrupt_acknowledge(4)` brings the counter up to date and clears the latch. Acceptance, exception entry, stack/vector validation, generated-native dispatch of the handler (through the RAM thunk the BIOS vector names) and RTE are the unchanged CPU-owned path. The report gains `hint_latched`; `main_interrupts_taken[4]` counts level-4 acceptances.

Tests (`sega_cd_bios_hardware_test`, fixture `tools/sega_cd_bios_hw_fixture.py`, generated-native through the full build chain): latch without delivery while IE1 is clear (three level-6 frames, register #10 = 0); level-4 handlers between level-6 handlers of two whole frames for register #10 = 9 (44 = 2 x 22) and 0 (450 = 2 x 225, the active lines and line 224), the expectation derived by the fixture's own statement of the counter rules; level 6 taken before a pending level 4; a mask of 4 holds level 4 and the fall to 3 delivers it once; three STOPs each woken by one level-4 interrupt; repeats, eight random deadline partitions and a fixed slicing identical. The old negatives `main:d7=4` and `main:d7=8` (IE1 stops) are removed; `main:d7=9` (a `101x` control word) remains.

## T004 - authentic Sonic CD (gate `HINT_FRONTIER_CROSSED`, equality partial)

One native program built once from the T003 product state (`sega-cd build --file-first structural`, no hints, 6:54 wall, 655 MiB; program identity `7caf4565...`), every route run with an empty PATH and the program unchanged. The frozen M9 program (identity `e5c0d954...`) reproduces the old stop first: `interrupt_source_unsupported`, main pc in Word RAM, `$C00004` word write, vertical-blank onset 19,260 (entry+18,058). Reference: unmodified Genesis Plus GX (frames/RAM dumps from the project's local oracle; its two pad scripts were validated against the M9 references before use). Checkpoints were registered before the first run (`prereg.txt`, entry-relative frames).

- **Old frontier crossed.** R0, R1 and R2 no longer stop at the IE1 write. No typed stop occurs: each route runs the whole 2.1 G instruction budget (about 82-86 thousand frames, 2,311 to 6,933 level-4 and 81-86 thousand level-6 acceptances, `hint_latched` = level-4 acceptances + 1, no refusals, 17-18 images bound and executed against 15 before, executed main instruction starts 17,533 against 10,772). R0 passes the old stop through the intro sequence; R1 through the Stardust-Speedway-class level attract demo (the first levels use IE1 with register #10 = 223) back to a fade; neither route is gameplay under player control.
- **Frames.** Pre-registered checkpoints equal in canonical frame: R0 17 of 18 (26000 differs), R1 14 of 16 (31300 and 32300 differ), R2 7 of 8 (38000 differs, below); START-exit (M9 `st_-1` mapping) 8/8 equal in frame, VRAM and Word RAM. Diagnostic extra frames (31000-43000 R0, neighbours): two more isolated differences (R0 35500, 38000). The R0 and R1 differences are isolated one-frame transients (R0 26000: 8 rows, one sprite tile, VRAM static during the display, IE1 clear; R1 31300: 4 rows, 32300: 11 rows, with a non-zero in-display VRAM-change counter): the frames on either side (+-1 to +-10) are equal and VRAM at the onset is equal. They are the pre-existing in-flight VRAM transient class; not attributed to H-INT (IE1 is clear at R0 26000, and the R1 differences are at rows 25-35 while the H-INT line is 223). This attribution is by those observations, not by a counterfactual run.
- **Word RAM.** Equal in 2M mode (every frame <= 31000, 37000+). During the intro (about 32000-36000, 1M mode) the reference's dumped `word_ram_2M` is stale (a reference quantisation limit), so the plain comparison shows ~228 KB of difference; against the reference's live 1M banks (a local oracle variant dumping `word_ram[0/1]`) the product bank is byte-identical in the stable bank (0 bytes at 35100 and 36000) and differs by 1.7-2.0 KB in the bank being written (in-flight), at 32000/34000/35500.
- **Determinism.** Three scheduler configurations (default, `--slice-seed 7`, `--slice-ticks 777777`) of R1 over 1.2 G instructions give byte-identical reports. CD-DA digests were collected (`--cdda-digest`) but NOT compared with the M8/M9 digests in this milestone: those routes end before the first IE1 frame, so the regression claim for CD-DA rests on the unchanged CD-DA code and the M8 tests in the full suite.
- **Unresolved divergence (R2).** R2 (title, RIGHT, START) is equal at 20000-36900 and 40000, but between 36900 and 37000 the product falls 3 frames behind the reference (product frame f = reference frame f-3, for example 38001 = 37998 exactly) for at least 37000-38050 and is equal again at 40000. IE1 is clear throughout; the H-INT counter only latches there. Root cause not identified; it is the pre-registered checkpoint R2 38000 and keeps this milestone from `GO`.

Playable non-demo route: not attempted (no independently reproducible schedule); no claim is made that M10 proves gameplay.

## T005 - independent review and decision

Adversarial review (a separate reviewer with its own falsification experiments; replica of the counter with and without the whole-frame skip over 20,000 random schedules; hand re-derivation of the 22/225 per-frame counts from GPGX order): PASS, no blocker or major. Minors: documented the unmodelled display modes (above); the fixture's mask-4 result is capped (does not detect a double delivery; the sub-sampled counts of the windows do) - left; the `r1 == registers[1]` clause on the IRQ delay is redundant and harmless; sync at instruction start accepted as instruction-granularity (above).

### Decision: `M10_HINT_PARTIAL`

Generic H-INT is implemented, tested at three levels (generated-native fixture with analytic counts, eight random plus fixed scheduler partitions, authentic routes) and delivered through the existing CPU-owned path; the old frontier is crossed on three routes and the frames match the reference at every pre-registered checkpoint of the H-INT-active R1 level demo apart from two isolated transients. It is not `GO` because (a) the R2 route develops an unexplained 3-frame lag at 36900-37000 (pre-registered R2 38000 differs) and (b) no player-controlled route was validated. No typed stop is the new frontier: the next limit lies beyond what these routes exercise.

Recommendation for the next milestone: root-cause the R2 lag with the reference's access/instruction logs (not H-INT: IE1 is clear), then author a reproducible input route into player-controlled level play to find the next real typed stop; Sonic CD PCM sound effects stay out of scope until then.
