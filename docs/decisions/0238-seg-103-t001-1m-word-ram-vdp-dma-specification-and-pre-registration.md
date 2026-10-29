# ADR 0238: SEG-103-T001 - 1M Word RAM VDP DMA specification, authentic baseline and pre-registration

- Status: Accepted. Milestone SEG-103 (M7), task T001. Gate `WORDRAM_1M_DMA_SPEC_GO`: **met** for the admitted class below; the remaining questions are recorded as limitations, not guessed.
- Related: ADR 0128, 0132, 0144, 0147, 0177 (VDP host, Word RAM, DMA timing), PR #144 (2M Word RAM source delay), ADR 0233-0237 (M6).
- Privacy: classes, counts and register classes only. No commercial address, offset, data byte, hash or trace appears here. The reference emulator (Genesis Plus GX, "GPGX") and MAME are behaviour references only: their source was read, none is copied, and their licences are not the project's.

## Prerequisite

SEG-102 product PR #167 is MERGED (squash e4f4214, 2026-10-29); M6 stays `M6_PARTIAL`. M7 branches from that product main.

## The failing shape (authentic, classes only)

Main MC68000, control-port write completing a 68K->VDP DMA (CD5) to VRAM, DMA enabled, register #23 selecting the `$200000-$21FFFF` page, the machine in **1M mode**. Transfers of about 2.4 thousand words, auto-increment 2, repeated by the guest as a cycle of 24 consecutive transfers that walk through the main bank and then restart at its beginning; 132 transfers in the R0 reference run, 87 in R1's window. **None crosses the 128 KiB window** (the sources are distinct, ascending, inside the page). The same page was used earlier in the boot in 2M mode (supported since PR #144). The reference performs the same transfers at the same entry-relative frame as the product's stop (R0 entry+2170, R1 entry+15281).

## Specification: the twelve questions

Sources: **H** hardware documentation (public VDP notes: Mega Drive hardware wiki "DMA Limitations", Plutiedev "128KB boundary bug"); **G** GPGX `vdp_ctrl.c`/`scd.c` (read); **M** MAME `315_5313.cpp` (read); **A** existing Segarecomp assumption.

| # | Question | Finding |
|---|---|---|
| 1 | Source ranges that may feed DMA in 1M mode | G: the DMA reads through the ordinary main bus map. `$200000-$21FFFF` is the bank assigned to main; `$220000-$23FFFF` is the 1M cell-image view (handlers, not memory); `$240000-$3FFFFF` repeats the 2M map. **Admitted: register #23 = `$10` (the `$200000-$21FFFF` page) only.** Everything else stays the typed stop `vdp_dma_unsupported`. |
| 2 | Bank selection | G: the memory map points at word RAM bank `RET`; the DMA reads it as it is when each chunk is read. A: product contract section 6 (`RET` = 1 gives main bank 1). Chosen at the arming instant (see 10/11). |
| 3 | Same logical view as ordinary reads | G: yes, the DMA read is `m68k.memory_map[source >> 16].read16/base` - the same map. The product uses the same `scd_bank_offset` interleave as `scd_word_ram_access`. |
| 4 | One-word source delay in 1M mode | **H** (Mega Drive hardware wiki): a Mega CD Word RAM DMA transfers the first word wrongly (stale bus value) and all following words correctly; the workaround is one word less/one more length. The text does not name 2M or 1M. G applies the same rule to every source in `$200000-$3FFFFF` regardless of the RAM mode (the delay belongs to the expansion-port bus cycle). M does not model the corrupted word (it compensates). **A: the rule applies to 1M exactly as to 2M; not separately hardware-verified for 1M.** Authentic frames are compared against G. |
| 5 | Destination auto-increment and the delay | G: `addr += reg[15]` once, `dma_length--`; the first destination word is not written. Product: the existing command-address rewrite. |
| 6 | Registers after the transfer | G: length registers 0; source registers #21/#22 advanced by the transferred count (one less than programmed) as a 16-bit value; #23 unchanged. Existing product `scd_vdp_dma_registers_done`. |
| 7 | Wrapping at 128 KiB | **H**: the source counter increments only the low and middle address bytes, so the source wraps inside its 128 KiB window; G identical (`source = (reg23 << 17) \| (source & 0x1FFFF)`). **M disagrees** (linear 24-bit increment, no wrap). Product: wrap (H and G). |
| 8 | Reaching the end of the visible bank | A consequence of 7: the next word is the first word of the same window (the start of the bank). No access to the other bank or to `$220000`. |
| 9 | Timing and stall | G `vdp_dma_update`: the same slot model for every 68K-bus source (rate by H32/H40 and blanking, one slot per CRAM/VSRAM word, two per VRAM word); the 68K waits to the end; the delay only reduces the count by one. Existing product `scd_vdp_dma_end` with the reduced length. |
| 10 | Sub CPU or device modifying Word RAM during a DMA | In 1M mode the sub processor and the CDC DMA write the *other* bank; graphics-ASIC operation in 1M mode is a typed stop. Nothing else writes the main bank. G itself copies in chunks per line, so it is not atomic either. **Limitation:** the product transfers at the arming instant (as the 2M path does). |
| 11 | Mode or ownership change during a DMA | Unverified (neither H nor a hardware test). Not modelled: the product reads the bank selected at the arming instant. Recorded as a limitation; T004 measures whether any authentic RET/MODE write falls inside a transfer's stall window. |
| 12 | H32/H40 and active/blank timing for this source class | Timing identical to every other 68K source (9); the source class does not change the rate. |

Unresolved (not guessed): hardware proof of the delay in 1M mode (4), mid-transfer ownership change (10, 11), a one-word transfer (stays a typed stop, as in 2M), the `$220000-$23FFFF` and mirrored pages.

## Design decision (before implementation)

Reuse the Genesis VDP host unchanged. The host reads DMA source words through `owned_regions` linearly and does not wrap, so the Sega CD adapter supplies, for an admitted 1M transfer only, a bounded **staging region**: `length * 2` bytes (maximum 128 KiB = one DMA window) copied from the guest-visible main bank with the verified interleave and window wrap, mapped at the logical source address. It is a read-only data copy handed to the existing host for exactly one synchronous transfer; the regions pointer is restored afterwards; it is never code authority and never aliases the other bank. Validation (page, mode, ownership, length not 1) precedes every mutation.

## Pre-registered expectations (written before any code)

1. **Fixtures** (independent hand-written byte oracle, tests/tools): in 1M mode with either RET value the destination words equal the selected bank's words (a different sentinel in each bank), skipping the first destination word, ascending; a naive linear read of the 256 KiB array gives different bytes. Source registers advance by `length - 1` (16 bits); length registers 0; #23 unchanged; the stall equals the slot model for `length - 1` words; a window-crossing transfer wraps to the bank's first word; a transfer in 2M mode and every other source behave exactly as before; refused shapes leave registers, VRAM, stall and armed state untouched.
2. **R0**: the product no longer stops at the DMA; it executes the first 1M transfer at entry+2170, VRAM and the canonical frame equal the reference at entry+2172, 2200, 2300, 2400 and 2490.
3. **R1**: the product crosses the old stop (about entry+15281) and runs on; canonical frame and VRAM equal the reference at entry+15283, 15350, 15500 and 16000 (those frames the product reaches within its budget; any later checkpoint is reported with its reached/not-reached state, not dropped).
4. **R2**: the M5/M6 checkpoints (7) stay equal; executed-module accounts unchanged.
5. DMA-by-DMA: the product's sequence of 1M transfers (programmed length, destination, stall duration in master clocks) equals the reference's for the transfers both execute.
6. If a new blocker appears it is recorded with the responsible CPU and class and is not implemented.
