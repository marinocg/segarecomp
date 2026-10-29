# ADR 0240: SEG-103-T004 - authentic Sonic CD evaluation of the 1M Word RAM DMA frontier (M7)

- Status: Accepted. Milestone SEG-103 (M7), task T004. Gate `AUTHENTIC_1M_DMA_FRONTIER_ADVANCED`: **met**, with the R2 late-divergence qualification below.
- Related: ADR 0238 (pre-registration), 0239, 0236 (M6 baseline).
- Privacy: classes, counts and equalities only; tools `tools/sega_cd_m5_routes.py` (unchanged) and `tools/sega_cd_dma_compare.py` print aggregates. No address, hash, byte, frame or trace is recorded here.

## Frozen program

One build (`segarecomp sega-cd build --file-first structural`; same inputs and options as M5/M6; no input script, module list, extent list or per-title option): 399 s wall, 656 MiB peak, status ok, 7 rounds, **the same program identity and generated-source tree as M5/M6** (only runtime objects changed). Made read-only; every route ran with `PATH=/nonexistent`; program and directory inventory verified unchanged after all routes (`program_unchanged_after_all_routes: true`). Baseline: the M6 runtime built from the same inputs reproduces both M6 stops exactly (R0 `vdp_dma_unsupported` at 74,488,697 retired instructions, R1 at 472,586,363; the stop is the first 1M transfer of the reference).

## Results (pre-registered checkpoints, ADR 0238)

| route | M6 stop | M7 result | reference frames equal (canonical frame and VRAM) |
|---|---|---|---|
| R0 (no input) | `vdp_dma_unsupported`, 74.5 M instructions | `gate_array_register_unsupported` (sub), 386.1 M instructions, about 14,184 frames after entry (M6: about 2,170) | 8/8 pre-registered (1500, 2100, 2170, 2172, 2200, 2300, 2400, 2490) and 6/6 late (13000 ... 14183) |
| R1 (Checkpoint H) | `vdp_dma_unsupported`, 472.6 M | same new stop, 784.2 M instructions, about 27,295 frames after entry (M6: about 15,281) | 8/8 M5, 8/8 pre-registered after the old stop (12456 ... 16000, incl. 15283, 15350, 15500, 16000), 5/5 late (20000 ... 27294) |
| R2 (RAM DATA) | budget at 900 M, no new stop | same new stop, 1,387.0 M instructions, about 50,834 frames | 7/7 M5; late 5000, 20000, 40000, 44200, 44224, 44226 equal; **44300 and 44500 differ** (below) |

Executed images as M5/M6 (R0 13 bound/13 executed, R1 14/14, R2 14/14; the unvisited-module counts grew with the longer runs, none refused: dynamic refusals `{}` everywhere); controller observations unchanged; `recheck_failures` is the by-design count of module replacements (7 in the M6 baseline, 14/15/22 over the longer runs).

## DMA effects, transfer by transfer

The product's `--event-log` (source word address, transferred words after the delay, wait in master clocks) against the reference's DMA log (arm and end of each transfer):
- **R0**: 23,692 transfers, all sources and lengths equal; **R1**: 61,465, all equal; R2: 96,493 paired, 81,960 equal before the divergence below.
- The register-#23-`$10` class (the 2M transfers of the boot and the 1M transfers): **2,800 / 2,800 (R0), 2,800 / 2,800 (R1), 2,784 / 2,784 (R2) equal in source, length and duration**; 2,182 transfers per route came from the 1M bank. The registers after a transfer follow from the equal programmed values by the one rule used for 2M since PR #144; destination and first-word-skip are verified by fixtures and by the equal VRAM at every compared frame.
- Work-RAM-class transfers differ in duration by -15..+787 master clocks in 11-13 percent of the multi-line cases (frame-boundary bookkeeping of the reference log, pre-existing, not part of M7).
- No RET/MODE change by the sub processor fell inside the wait of any of the 2,182 1M transfers (`bank_mode_changes_during_wait` = 0 on every route): the unmodelled hazard (ADR 0238 question 11) was not exercised.

## The new frontier (R0, R1 and R2 stop on it)

Class `gate_array_register_unsupported`, **sub** MC68000, byte **read** of a Gate Array register in the graphics-ASIC block (the image-buffer vertical-dot-size register, odd byte of a word register; the contract lists byte accesses and reads other than `$58` as typed stops). The same sub instruction in all three routes. The reference executes the same instruction (110 thousand times in R0) without a stop. Device class (graphics ASIC register read); not a CPU, binder or DMA issue. Not implemented here.

## R2 late divergence (recorded, not diagnosed)

After 80,517 identical transfers the reference leaves the title-animation streaming cycle at entry+44,225 frames and runs another module; the product leaves it 37 frames later (the transfer streams resynchronize with a constant 37-frame shift) and the canonical frames differ at entry+44,300 and 44,500. No typed stop; no equal-checkpoint is affected (all pre-registered frames equal). The cause is a timer-driven idle transition about 12 minutes of machine time in; candidates (unverified): CD drive/audio time (CD-DA is not modelled) or the M6 sub-timeline skew. R0 and R1 end with their DMA streams entirely equal. This is **not** DMA correctness (the DMA stream is equal to the divergence), but it is a silent divergence and is carried as a remaining barrier.

## Determinism

R1's three runs (a, b, late) retired the identical 784,157,135 instructions; R0 and R0-late 386,069,715; R2's two runs 1,386,967,124. Three slice seeds and the default scheduler gave byte-identical reports on R0 (device-log digest, frame digest, DMA count/stall, stop); the 1M transfer is synchronous at the arming instant and takes no scheduling input.
