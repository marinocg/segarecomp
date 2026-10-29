# ADR 0241: SEG-103-T005 - generality, cost and remaining barriers

- Status: Accepted. Gate `DMA_GENERALITY_AND_COST_RECORDED`: met.

## Generality

- The change is a **Sega CD machine-view adapter** (`scd_vdp_dma_prepare` plus `scd_word_ram_main_bank_physical`): it supplies the unchanged Genesis VDP host with the guest-visible 1M main bank. It is not applicable to other Genesis devices (no host change) and contains no title, address sequence or register sequence of any game; any Sega CD title that DMAs from the main bank in 1M mode benefits (the selector is register #23 = `$10` plus 1M mode). No other title was run in M7; reuse is a claim from the rule, not a measurement.
- Not generalised: the cell image `$220000-$23FFFF`, the mirror pages, sub-owned 2M Word RAM, PRG-RAM sources, a one-word transfer.
- Finding (not changed): the 2M path reads linearly across a 128 KiB boundary inside the 256 KiB array, where the hardware and the reference wrap in the 128 KiB window; the existing 2M fixture row crossing `$220000` documents the linear read. The authentic 2M transfers never cross; 1M wraps correctly.

## Cost (same inputs, same interval: R0 to the M6 stop tick, 3 alternating runs each)

| | M6 runtime | M7 runtime |
|---|---|---|
| wall | 16.8 / 16.6 / 16.5 s | 18.5 / 18.6 / 18.6 s (about +11 %, cause not isolated; the interval contains no 1M transfer, so it is code layout or the larger host struct, not the new path) |
| peak RSS | 24.2-24.4 MiB | 23.9-24.1 MiB (the 128 KiB stage is untouched until the first 1M transfer) |
| executable | 119,625,872 B | 119,625,928 B (+56) |
| build | 311 s (M6 runtime, partly cached) | 399 s (its passes run further: more frames before their stops; same 656 MiB peak) |

DMA work in the authentic runs: 2,182 1M transfers per route (R0/R1; 2,181 R2) of about 2.4 thousand words (about 10.5 MB copied per route into the stage, an order of 10 ms in total); the whole copy is a bounded `memcpy`-class loop. Maximum extra memory 128 KiB. No optimisation was made or is warranted.

## Remaining independent barriers (not 1M-DMA correctness)

1. The new frontier: sub graphics-ASIC register byte read (`gate_array_register_unsupported`), reached by all three routes (ADR 0240).
2. R2's late 37-frame idle-transition divergence (entry+44,225): CD-DA/drive time or sub timing.
3. Sound output, CD-DA, PCM beyond the pointer reads (ADR 0233-0237), Z80.
4. Unverified DMA sources and the unmodelled RET/MODE change during a DMA wait; hardware proof of the 1M delay.
5. Earthworm Jim SE's transformed-code provenance frontier; incomplete executable source inventory (`inventory.partial`).
6. The M6 sub-CPU timing skew (M6_PARTIAL), unchanged.
