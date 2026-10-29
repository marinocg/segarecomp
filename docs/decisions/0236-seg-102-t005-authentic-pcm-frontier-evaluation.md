# ADR 0236: SEG-102-T005 - authentic Sonic CD evaluation of the PCM frontiers (M6)

- Status: Accepted. Milestone SEG-102 (M6), task T005. Gate `AUTHENTIC_PCM_FRONTIER_ADVANCED`: **met**, with the value-level qualification below.
- Related: ADR 0233 (specification, evaluation rules and their history), 0234, 0235, 0227-0232 (M5).
- Privacy: classes, counts, equalities and register offsets only. No address, hash, pointer value, trace or frame of the commercial image appears here. Tool: `tools/sega_cd_pcm_compare.py` (aggregate output) and `tools/sega_cd_m5_routes.py` (unchanged).

## Frozen program

One build, `segarecomp sega-cd build ... --file-first structural` (same inputs and options as M5's B1, no input script, no module list, no extent list, no per-title option) from the T001-T004 runtime plus the review fix: 317 s wall, 656 MiB peak, status ok, 7 rounds, **the same generated-source identity as B1** (3 boot-system extents, 0 game-module extents): the corrected runtime changed only the runtime objects, hence the program hash. Its own execution-derived passes now continue past F0 and end at the new frontier below. The program was made read-only and **not rebuilt, relinked or re-emitted** for any route; each route ran with `PATH=/nonexistent`, and the program file and its directory inventory were verified identical after all routes (`program_unchanged_after_all_routes: true`).

References: variant F of ADR 0233 (the chip code with the per-frame phase restart removed), with the M5 inputs, pad schedules and canonical-frame capture. Its frames at the M5 checkpoints were verified byte-identical (frame, VDP, Work/PRG/Word RAM) to the M5 reference captures: R1 8/8, R2 7/7 (the M5 evidence is not touched by the PCM change).

## Results per route

| | R0 (input-free) | R1 (Checkpoint H script) | R2 (RAM DATA route) |
|---|---|---|---|
| old stop | F0, `pcm_unsupported`, sub, byte read, channel 0 pointer | F1, `pcm_unsupported`, sub, long read of channel 6's pointer | none (budget) |
| new result | `vdp_dma_unsupported` (main), 74.49 M instructions (+25 k), 18.5 s | `vdp_dma_unsupported` (main), 472.6 M instructions (+66 M: about 2,825 reference frames beyond F1), 101 s | `budget_exhausted` at 900 M instructions, 192 s |
| reference frames equal (canonical frame and VRAM) | 4/4 (entry+2100, 2168, 2169, 2170: the last two at and after the first pointer read) | 8/8 M5 (510 ... 1500) + 8/8 after F1 (12450, 12456, 12500, 13000, 15000, 15270, 15279, 15280) | 7/7 M5 |
| executed images | 9 bound, 9 executed, 0 previously unvisited | 11 / 11 / **2** unvisited (as M5) | 8 / 8 / **1** unvisited (as M5) |
| dynamic refusals / compiled-code mismatches | 0 / 0 | 0 / 0 | 0 / 0 |
| PCM accesses | 4,737 pointer reads served | 4,820 served (88 of them the F1 channel-6 reads, 40 frames) | 59,852 writes, no pointer read (as the reference) |

No instruction-byte verification, mapping rule or admission rule was touched; no interpreter, JIT or runtime compilation exists. Two process runs, eight slice seeds and one slice size give byte-identical reports **and** PCM traces on R0.

## Pointer values against the reference

1. **Operation streams.** The executed PCM operation sequences (kind, chip offset and, for writes, the written value) of the product and of the reference are identical operation for operation from the last chip reset: 130,163 operations on R0 and 224,277 on R1, **0 mismatches**.
2. **Model against the reference's own authentic streams** (ADR 0235): 2,792,481 / 2,792,481 R0 and 88 / 88 R1 pointer reads equal when the reference's operations and instants are replayed through the product model. The chip model is exact; what remains is the two machines' CPU timelines.
3. **Generated-native reads, F0**: the first read and 4,734 of the 4,737 reads of the first polled frame equal the reference's; the 3 others are one-event differences at a pointer-page transition (the reference is ahead). Sample-event difference (reference minus product): 0 in 4,201 reads, 1 in 534, 16 in 2.
4. **Generated-native reads, F1**: the channel-6 low-byte register differs from the reference by exactly **one sample event** (one frequency step, about half a byte) in 86 of the 88 reads (equal in 2); the high byte equals the reference's in the same window. The later phase of R1: 4,793 of 4,820 reads equal; histogram of the event-count difference {-1: 1,055, 0: 3,673, +1: 86, 16: 5, 17: 1}.
5. **Cause.** The two machines' sub-processor timelines are not in lockstep to the sub cycle: measured relative to the chip reset, the product-versus-reference skew of the PCM operations spans about +-6,000 sub cycles (it changes in bursts of thousands of cycles at a periodic group of writes, consistent with interrupt-handler placement differing by several video lines; the mechanism was not investigated further) and stood at +313 sub cycles at channel 6's start and +311 at its first read. A channel started 56 sub cycles before the reference's next sample event, with a skew of that size, is counted one event differently for every later read of a 512-event poll interval - exactly the observed constant difference. The skew is a property of the CPU timing of ADR 0145/0177, not of the PCM model; equality of the F1 values would require sub-cycle lockstep of the sub processor, which is not a PCM property.
6. **Consumption.** The guest consumes the values: R0's 4,737 reads are a busy poll of a channel pointer, F1's a once-per-frame long read of one channel's two pointer bytes, and the machine continues for 2,825 frames after F1 with all 8 post-F1 reference frames (canonical frame and VRAM) equal, including the last frame before the new stop. Work RAM differs by 1-3 bytes and PRG RAM by 24-215 bytes in the post-F frames, the same classes and magnitudes as the M5 frames (polling-loop data, stack residue below the stack pointer). The Word RAM comparison of the tool is not meaningful once the machine runs Word RAM in 1M mode (the reference dump is of the 2M array; 114,670 bytes differ in the last frames of R0/R1 and none before): reported, not used.

## New authentic frontier (both R0 and R1 stop on it)

Class `vdp_dma_unsupported`, **main** MC68000, hardware: the first of a per-frame series of VDP DMA transfers to VRAM (a few thousand words) whose source is the main-owned half of **1M-mode Word RAM**. The reference performs the same transfer at the same point (its DMA log shows the transfer, from the same source register values, in the frame in which the product stops, and again every following frame). Source class: device (VDP DMA from the 1M Word RAM bank; the contract lists it as not modelled, section 17 "1M/sub-owned Word RAM VDP DMA"). It is not a transformed-code, source-admission or runtime-mapping problem, and not part of the pointer-state contract: **not implemented here**.

## Gate

`AUTHENTIC_PCM_FRONTIER_ADVANCED`: both original stops are crossed on authentic input with the same executed PCM operations as the reference, the guest consumes the returned values, generated-native execution continues (about one frame past F0, about 2,825 frames past F1) and 16 reference frames beyond the old stops are equal; the next frontier is a different, independently verified hardware stop. The value-level qualification is item 4 above.
