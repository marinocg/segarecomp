# ADR 0249: SEG-105 M9 - graphics ASIC vertical-dot counter readback and Sonic CD special-stage continuation

- Status: Accepted (SEG-105 / M9, one combined PR; internal tasks T001-T005). Decision: see "Decision".
- Related: ADR 0147 (graphics ASIC), ADR 0240 (M7; its `M7_GO` is unchanged). M8 (PR #169) was verified merged before this work began.
- Privacy: counts, relative frame numbers and classes only; no ROM bytes, traces or soundtrack data. Full evidence is in the ignored `.cache/seg105`.

## T001 - the register (gate `GFX_VDOT_READBACK_SPEC_GO`)

Sources read (observations, no code copied): Genesis Plus GX `scd.c` (`scd_read_byte` default register path) and `gfx.c` (`gfx_update`, `gfx_start`), MAME `megacd.cpp` (`segacd_imagebuffer_vdot_size_*`, `stamp_timer_callback`: "this ends up as 0 after processing (soniccd bonus stage)"). Both treat the low byte of `$FF8064` as ONE register that is also the live counter.

| Question | Verified answer |
|---|---|
| value before an operation / after the write | the last value written (0 at power-on and after reset); a completed operation leaves 0 (GPGX and MAME agree) |
| while active; remaining vs programmed | the **remaining** lines: programmed minus `ceil((t - base)/line_ticks)` lines started, floor 0 (GPGX `gfx_update`); the programmed count is not kept separately |
| decrement point / zero | at line start (a line renders as soon as it starts); zero when the last line has started, which is also completion |
| after completion | 0, stays 0 (an operation started without reprogramming runs 0 lines) |
| halted by Word RAM ownership | frozen; resumes counting from the hand-over |
| peripheral reset | 0 (GPGX `scd_reset` clears the register file) |
| rewrite while active | replaces the remaining count (0 completes at the line boundary in progress) |
| does a read synchronize? | product: yes, to the read's own machine tick, like every graphics register access. GPGX: no, its graphics clock is advanced once per scan line, so its read can lag by up to one scan line (below) |
| byte lanes / word read / high byte | `$FF8065` is the low byte. A word read of `$FF8064` is **not** admitted: GPGX keeps the written high byte, MAME clears the whole word, hardware unknown. `$FF8064` byte read, `$FF8065` word, any `$65` write stay typed stops |
| influence on busy/interrupt | none |

Baseline (frozen M8 program, today): the R0/R1 stops reproduce (`gate_array_register_unsupported`, sub, byte read, `$FF8065`, the `TST.B` of a wait loop `while (counter != 0)`). The reference log shows the game writes `$64`=96, `$66`, then polls ~79 times per line until 0; first read right after the start returned 53 of 96 (43 lines in), values fall by 1 per line. 108,485 reads in the R0 reference up to the stop (the ADR 0240 "110 thousand").

## T002 / T003 - implementation and tests

`ScdGfx::lines_left` is the single live counter (field `v_lines` removed); a word write of `$64` sets it at any time; `scd_gfx_start` uses it; the byte read of `$65` returns it after the existing `scd_gfx_sync`. No timer, no scheduler, no interrupt effect. `vdot_reads` is an observation counter in the report. `sega_cd_subside_table` gained 50+ rows with an independent oracle: power-on, write, start, +1 tick / one tick before / at / after a line boundary, repeated reads, several lines, every sixth of a line to the end, completion and afterwards, second operation with and without reprogramming, rewrite while active (including 0), halt/resume, reset (idle and running), IEN1 on/off with polling (no early or duplicate request), negative forms (typed stops, whole `ScdGfx` unchanged), and four deadline partitions giving identical reads, state and Word RAM. ASan/UBSan: the table passes (190 rows).

## T004 - authentic Sonic CD (gate `AUTHENTIC_SPECIAL_STAGE_ADVANCED`)

One native program built once (`sega-cd build --file-first structural`, no hints; 7:02 wall, 655 MiB; the same program identity and size as M5-M8, only runtime objects changed), frozen; every route ran with an empty PATH and the program unchanged.

- **Old frontier crossed.** R0 (no input) reaches a Special Stage III attract demo at entry+14,184 frames and R1 at 27,295; both now run through the whole demo (about 3,000 frames: 3,805 graphics operations, about 2.5 M counter reads) back to the title screen. The new genuine frontier (both routes) is the main CPU enabling the VDP **H-interrupt** (register #0 IE1) at entry+18,057 (R0) / +31,168 (R1) - the next level demo - typed stop `interrupt_source_unsupported`; not graphics-ASIC, not implemented here.
- **Frames.** The independent reference (unmodified GPGX) at pre-registered frames beyond the old stop: canonical frame equal at every compared frame (R0 and R1, all pre-registered checkpoints between the old and the new stop); Word RAM bytes equal at every one; the START-in-demo route (below) 8/8 with VRAM and Word RAM.
- **Graphics operations and reads.** Operation starts 3,805 = 3,805 (reference). Counter reads: product 2,504,934; reference with the live-read variant 2,505,081 (+0.006 %); unmodified GPGX 2,516,198 (+0.45 %, the scan-line lag).
- **Quantisation.** In a private reference variant that advances the graphics clock at the register read, canonical frames, VRAM and Word RAM are byte-identical to unmodified GPGX at every checkpoint (R0 up to entry+35,000). So the exact-at-read model of this product is frame-equivalent to the reference, and a per-read equality with GPGX is neither achievable nor meaningful (its value is up to one scan line stale); the admitted class is therefore exact against the documented rule and the live variant, not against GPGX's lag.
- **Input in the special stage.** The attract demo does not react to buttons (confirmed in the reference); pressing START in it (the exact frame mapping, press onset = reference frame + 1,202) leaves the demo: 8/8 checkpoints equal in canonical frame, VRAM and Word RAM (an off-by-one press mapping shows up immediately as differing frames, so the check is sensitive). A playable (non-demo) special stage needs a long gameplay route (50 rings and a giant ring) that was not authored; this is **not** claimed.
- **Qualification (pre-existing, not graphics).** At 8 of 27 compared R0 frames (16000-16004, 18000-18004 region) (special stage and title streaming) an 8-16 byte VRAM window differs while frame and Word RAM are equal: the in-flight 68K->VDP transfer; 31,900 paired transfers are equal in source and length, 88 % equal in duration, the rest in the known classes of ADR 0240 (-15/+15/+/-1/+786 master clocks), all 2,800 1M-class transfers equal. Unchanged by this PR.
- **Determinism.** Default scheduler and two slice-size/seed settings (700 M and 3,000 M ticks, seeds 1 and 99) on R0: the three reports are byte-identical (stop, digests, counters).
- **Regression.** The M7 routes (R0, R0L, R1a/b/L, R2a/L) with CD-DA digest on, on the same frozen program: every frame comparison (canonical frame, VRAM, the three RAM images, including the known R2L late divergence at 44,300/44,500) is identical to the M8 results; only the stop moved (to the H-interrupt frontier; R2 now at 1.468 G instructions). The CD-DA PCM digest at the old stop instant (`--until-tick` of the M7 stop) equals the M8 run's. Fast suite and the 53 `sega_cd` CTest tests pass (independent reviewer, 53/53).

## Remaining unsupported graphics-ASIC behaviour

1M-mode conversion, byte writes and other byte accesses, word read of `$FF8064`, reads of all other parameter registers, the font generator and its registers, Word RAM access delays (refresh/main contention), a pixel offset that is not a multiple of 4 (cost model G).

## Independent review

PASS, no blocker or major finding. Minors fixed: dangling ADR reference (this record), contract sentence about 1M-mode/byte accesses (now states the `$58`/`$65` byte reads, and that `$65` is also readable in 1M mode, returning the stored counter). Noted: the test oracle re-derives the same ceil rule (it cannot detect a shared misreading of hardware; labelled as such).

## Decision

See the PR record (set after review and exact-head CI): `GFX_SPECIAL_STAGE_GO` requires exact-head CI, which does not run on a draft; without it, or with interactive special-stage play unreproduced, the result is `GFX_SPECIAL_STAGE_PARTIAL`. Cost: one byte compare/branch on a rare path, +8 bytes of state, no change in program size.
