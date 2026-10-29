# ADR 0239: SEG-103-T002/T003 - verified 1M Word RAM VDP DMA source and its regression matrix

- Status: Accepted. Milestone SEG-103 (M7). Gates `WORDRAM_1M_DMA_IMPLEMENTED` and `WORDRAM_1M_DMA_DIFFERENTIAL_GO`: **met** for the admitted class.
- Related: ADR 0238 (specification and pre-registration), 0147, 0177, PR #144.

## What changed (and what did not)

`scd_vdp_dma_prepare` gained one branch: a source in `$200000-$21FFFF` (register #23 = `$10`) while the machine is in 1M mode. It
1. refuses a one-word transfer (no source word left after the delay) **before** any mutation, like the 2M path;
2. brings an in-flight CDC/graphics operation up to this instant (`scd_cd_observe`, as the 2M path);
3. reduces the programmed length by one and rewrites registers #19/#20 (the source delay, identical to 2M; the command-address rewrite in `scd_vdp_access` is the existing mechanism);
4. copies `length` words from the guest-visible main bank into the host's `stage` buffer **in source order with the VDP's 128 KiB window wrap**, using `scd_word_ram_main_bank_physical` (the same interleave and bank choice as `scd_word_ram_access`, one definition);
5. presents that buffer as the host's single `owned_regions` entry for the one synchronous transfer; `scd_vdp_dma_registers_done` (run after every armed transfer) restores the default regions.

The Genesis VDP host, the DMA control path, the slot-model timing (`scd_vdp_dma_end`), the stall, the register completion and the 2M, work-RAM and BIOS paths are untouched. No second DMA implementation, no generic memory-provider layer, no change to the runtime of generated code.

## Staging buffer justification

Maximum size: 128 KiB (`SCD_WORD_RAM_BYTES / 2`, one DMA window; `length` is at most 65536 words), a fixed array inside the already heap-allocated VDP host, allocated once. Cost: one bounded copy per transfer. Contents come from the live bank at the arming instant; the host drains the transfer synchronously, so the copy is exactly what the (atomic) transfer reads. It is data, never executable authority (it is not reachable from `scd_code_byte`, is not the overlay's view, and is only referenced by the host's DMA data read), cannot alias the other bank (every offset is derived from the main bank's interleave) and is not valid beyond the arming instruction (pointer restored). The `stage_region` bounds are `[source, source + 2 * length)`; the host's read check rejects anything outside.

## Why this and not a direct host read

The host reads sources linearly from regions or its own memory map and neither interleaves nor wraps at 128 KiB (the existing 2M path therefore reads *through* a 128 KiB boundary where GPGX and the hardware wrap; recorded as a finding, not changed here - ADR 0240). A staged, source-ordered window solves interleave and wrap with no host change; the alternative (teaching the host a bank-view callback) is the "memory provider" abstraction the milestone excludes.

## Test matrix (tests/tools/sega_cd_vdp_dma_table.c, run by sega_cd_vdp_dma_timing_test; 87 rows)

Independent oracle: bank contents are stored through the contract's interleave (2M-space word `2 i + b`), not through the implementation's function, and asserted different from the other bank and from a linear read of the array. Rows: banks 0 and 1 (RET), 16-bit and 32-bit command shapes, nonzero source and destination, skipped first word, registers (length 0, source advance, #23), stall; RET change between successive transfers; 2M -> 1M -> 2M (the 2M transfer reads the linear array again); snapshot at arming (a later write does not alter VRAM); 128 KiB window wrap to the bank start with the register wrap; length 0 (65535 words, VRAM written once round, registers); increment 4; CRAM and VSRAM destinations; timing equal to the 2M duration of the same lagged length at six phase/width shapes (H32/H40, VBlank, active line, display off, last line) plus a hand-derived value; refusals (length one, `$220000`, `$23FFF0`, `$240000`, `$3FFFF0`) with registers, VRAM, stall and armed state unchanged. The previous row asserting that 1M mode is a typed stop was replaced by the cell-image refusal.

Mutation checks (run locally, killed by the matrix): naive linear read of the 256 KiB array; no window wrap; the other bank. Existing 2M rows (PR #144 correction) pass unchanged.

## Limits kept explicit

A RET/MODE change by the sub processor during the stall is not modelled (the transfer reads the bank selected at the arming instant); hardware proof of the source delay in 1M mode is absent (the reference applies it to every `$200000-$3FFFFF` source); the cell image and mirrors, sub-owned 2M Word RAM and PRG-RAM sources remain typed stops; a one-word transfer remains a typed stop.
