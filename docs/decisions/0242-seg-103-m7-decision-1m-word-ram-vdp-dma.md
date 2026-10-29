# ADR 0242: SEG-103-T006 - independent review and the M7 decision

- Status: Accepted. **Decision: `M7_GO`** (with the qualifications below).

## Independent review

A separate falsification reviewer (no write access to the worktree) attacked bank selection, interleave, wrap, delay, registers, refusals, stage lifetime, timing, 2M regressions, buffer bounds, code authority, privacy and test strength. Verdict **PASS, no blocker or major defect.** It ran the sega_cd_vdp_dma/word_ram/dma_compare tests and the whole `sega_cd` group (50/50) and 13 mutants of the change: 12 killed, the survivors being equivalent (each of the two pointer-restore sites is covered by the other) or one gap: nothing asserted the restore or `scd_cd_observe` on a 1M transfer. The restore is now asserted (a row after the 1M transfer); `scd_cd_observe` ordering is now covered by a fixture with a CDC transfer in flight into the bank main sees (writer latched, sub flips RET, 1M DMA armed mid-transfer; the mutant that drops the call is killed). Post-review follow-up: `tools/sega_cd_dma_compare.py` is now strict (an empty or truncated comparison fails, any duration mismatch of the admitted class fails, `--prefix N` evaluates an accepted prefix such as R2's 80,517 pre-divergence transfers, `--allow-product-suffix` declares an unverifiable product tail), with negative tests. Minor privacy remark (a fixture length resembling the ADR's class): aggregate only, kept.

## Conditions

| # | condition | result |
|---|---|---|
| 1 | M6 merged and verified | **met** - PR #167 squash e4f4214; M6_PARTIAL unchanged |
| 2 | semantics independently established | **met** for the admitted class (ADR 0238: hardware docs for delay and 128 KiB wrap, GPGX; MAME disagrees on wrap; 1M delay not hardware-verified) |
| 3 | selected-bank mapping | **met** - fixtures with a separate oracle, mutation-checked |
| 4 | bytes, registers, timing | **met** - 91-row table; authentic: all 2,800 / 2,800 / 2,784 page-class transfers equal in source, length, duration |
| 5 | 2M preserved | **met** - existing rows unchanged, `sega_cd` group passes |
| 6, 7 | R0 and R1 cross | **met** |
| 8 | further native execution | **met** - R0 +about 12,000 frames, R1 +about 12,000 frames (R2 also) |
| 9 | reference frames beyond | **met** - R0 14/14, R1 21/21 beyond M5's, all pre-registered |
| 10 | R2 and M5/M6 checkpoints | **met** at every pre-registered checkpoint (R2 7/7, R1 M5 8/8); R2 late frames at entry+44,300 and 44,500 differ (ADR 0240), outside the pre-registration |
| 11 | no interpreter/JIT/runtime compile | **met** |
| 12 | no weakened admission/guard | **met** - same program identity; binder and byte guards untouched |
| 13 | determinism, review, CI | local gates and review pass; exact-head CI is recorded on the PR |

`M7_GO` does not claim gameplay, audio, or the R2 late divergence; the next frontier (sub graphics-ASIC byte register read) is recorded and not implemented.
