# ADR 0237: SEG-102-T006 - cost, independent review and the M6 decision

- Status: Accepted. Milestone SEG-102 (M6), task T006. **Decision: `M6_PARTIAL`.**
- Related: ADR 0233-0236.

## Cost of the PCM state

Same program inputs, same R1 interval (to just before F1, `--until-tick`), M5's frozen B1 (no PCM state) against the M6 program, three alternating runs each, no other load: B1 84.8 / 91.1 / 84.9 s, M6 82.9 / 83.5 / 83.4 s (no measurable cost; the differences are noise); peak memory 26.7 MiB against 27.9 MiB (the 8 channel addresses and the counters; the 64 KiB wave RAM already existed). PCM work over that interval: 138,122 synchronizations, 7,379,269 sample events applied, 151,474 channel steps (events are cheap because few channels run: the loop is skipped when none does), the largest single catch-up 760,618 events (an interval of about 23 s of machine time between two PCM accesses; it costs milliseconds). Build: 317 s, 656 MiB, unchanged from M5. No optimisation was made or is warranted.

## Independent adversarial review

A separate falsification reviewer attacked boundary ordering, tick conversion and overflow, drift and determinism, reset phase, ON/OFF polarity and reload rules, wraparound, lanes and the 32-bit read split, wave RAM changes and markers, refused-access soundness, output responsibilities, regressions, test strength and licence. Verdict **PASS, no blocker or major finding.** It re-ran the differential against fresh seeds (16 more seeds x 3,000 operations under both profiles: every read equal), regenerated the committed vectors (identical), ran 19 mutants of the model (17 killed; one equivalent; one survivor = finding 1), the sanitizer build and the sega_cd / genesis CTest groups (pass). Findings, all minor, all handled: (1) the read-side chip-offset mask was not pinned by a test (wave RAM reads at `$1010-$1011` would have been served as pointers if the mask were narrowed): the refusal classes now include them; (2) the "refused read does not synchronize" claim was not asserted: it now is; (3) a dangling ADR number: resolved by ADR 0236; (4) no committed vector contains a chip reset: the reset phase is covered by the closed-form row, the generated-native fixture (RES0) and the authentic replays (two resets each), and it is a recorded convention (ADR 0233 assumption (a)), not a reference-derived fact. What the reviewer could not falsify: that the reference-derived evidence proves equivalence to Genesis Plus GX, not to hardware.

## Decision against the M6 conditions

| # | condition | result |
|---|---|---|
| 1 | clock and synchronization model independently justified | **met** - two public implementations agree on clock/384 and pointer split; conventions listed |
| 2 | running pointers correct in focused differential tests | **met** - ADR 0235 |
| 3 | byte and word reads match the reference | **met** at chip level (822 synthetic + 2,792,569 authentic-stream reads, all equal); byte/word/lane and mirror rows |
| 4 | F0 crossed with the expected value | **met with 3 exceptions** - first read and 4,734/4,737 reads equal; 3 one-event differences at a page transition |
| 5 | F1 crossed with the expected value | **not met at value level** - crossed, consumed and followed by 2,825 equal frames, but the channel-6 low byte differs from the reference by one sample event in 86/88 reads (CPU-timeline skew, ADR 0236) |
| 6 | generated-native execution continues beyond the old frontiers | **met** - one frame past F0, about 2,825 frames past F1 |
| 7 | a valid observation beyond each frontier | **met** - 4 R0 and 8 post-F1 reference frames equal (canonical frame and VRAM) |
| 8 | M5 checkpoints still match | **met** - R1 8/8, R2 7/7, executed-module accounts as M5 |
| 9 | no instruction-byte verification or mapping rule weakened | **met** |
| 10 | no interpreter, JIT or runtime compilation | **met** |
| 11 | determinism, regression and CI gates | local gates pass (below); exact-head CI is recorded on the PR |

## Why `M6_PARTIAL` and the precise missing requirements

The PCM chip model is correct and cheap, both authentic frontiers are crossed and the game continues on the reference's frames. `M6_GO` is withheld because two required statements are not demonstrated as stated:

1. **Exact CPU-visible values at F1 (and three reads at F0)** equal the reference's only up to one sample event. The model reproduces the reference exactly when it is given the reference's access instants; the instants differ by the two machines' sub-processor timeline skew (hundreds of sub cycles at the channel start, thousands in bursts). Closing it needs sub-cycle lockstep of the sub processor with the reference (interrupt placement and bus-wait conventions), a CPU-timing task outside the PCM contract.
2. **Hardware-unverified conventions**, each observable or potentially observable: the loop-marker step (GPGX and MAME disagree; the choice changes 44,212 of the 2.79 M authentic R0 reads), the zero upper lane of a word read, the grid anchor / equal-instant ordering, the open-bus value of an even-address byte read (stays a typed stop). All "reference" evidence is Genesis Plus GX behaviour; none of it is hardware ground truth.

Not `M6_NO_GO`: no constant, approximation or unexplained timing constant was used to obtain progress, and nothing was tuned to the reference's output.

## Remaining gaps and the next frontier

- Next authentic frontier (not implemented, ADR 0236): main-CPU VDP DMA to VRAM from the main-owned half of 1M-mode Word RAM (`vdp_dma_unsupported`).
- CDC DMA into PCM RAM (destination 4), PCM wave RAM reads, other register reads, CD-DA and any audio output remain out of scope and typed stops.
- Sub-processor timeline lockstep (item 1) and hardware ground truth for the loop step and lanes (item 2).

## Validation recorded for this PR

Focused: `sega_cd_pcm_test`, `sega_cd_pcm_native_test`, `sega_cd_subside_test`, the fixture manifest (42 images) and the whole `sega_cd` CTest group (48 tests) passed locally; the reviewer additionally ran the `sega_cd|genesis` group (no failures; 2 local-image tests skipped) and the sanitizer build of the PCM test. Authentic: ADR 0236. The exact-head GitHub CI result is appended to the PR (not repeated here).
