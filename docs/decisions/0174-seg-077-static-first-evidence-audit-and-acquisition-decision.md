# ADR 0174: SEG-077 static-first evidence audit, dispatch and dynamic-evidence spikes, and the acquisition decision

- Status: proposed (pending independent review of the results; SEG-077-T009 is not complete until then)
- Task: SEG-077-T009, combined SEG-077 PR #135
- Records (aggregate-only):
  - `tools/segarecomp_seg077_t009.results.json`
  - `tools/segarecomp_genesis_static_v2.t009.admission.json` (plan `c51293b6...`)
  - `tools/segarecomp_seg077_partial.results.json` (plan `d24a1a1e...`)
  - `tools/segarecomp_genesis_static_v2.audit.json` (superseded audit, kept as the diagnostic record)
- Preserved unchanged:
  - ADR 0171/0172 and T001/T002's `STOP`;
  - the v1 static plan `5a1cd094...`, its admission and its frozen result;
  - the superseded 54/113-job T006 farm (plan `fa844809...`).

## Context

The operator directed that the Chuck Rock step be redone with the requested tool combination (ADR 0173 had followed the trace-bounded
T001 protocol). The static-first v1 labeller (Ghidra, sega2asm decompressors, two decoders plus Capstone, tables and traces) was frozen
and admitted tiers on six held-out commercial images. The operator then required:

- an audit of that measurement;
- a diagnosis rather than threshold relaxation;
- a bounded indirect-dispatch spike;
- better dynamic evidence;
- a frozen, reviewed usefulness study;
- one recommendation.

## Findings

1. **v1's measurement was biased, and one admission was wrong.**
   - v1 counted predictions over reference-unknown bytes in the error-rate denominator only. A synthetic test reproduces this.
   - The exposed commercial images have complete truth, so v1's exposed rates were unaffected, but its development rates were understated.
   - v1 also counted Capstone's `dc.w` pseudo-op as a decode.
   - v1 admitted callback-pointer roots (IP) on 1,532 exposed bytes. Under corrected measurement IP is **96% wrong** on the
     development set: 88 wrong roots produced 73K inherited wrong instructions.
   - The IP-containing Chuck Rock sample is superseded.
2. **Corrected admission (v2, versioned, two review rounds).** Only V (vector-root descent), PT (code-pointer tables) and FILL (padding)
   are admitted.
   - J fails, through its table-data boundary and a development per-image rate.
   - Every other tier fails:

     | Tier | Wrong rate |
     |---|---|
     | CD (dynamic-call fall-through) | 3.8% |
     | G (Ghidra-corroborated roots) | 0.28% |
     | DC (sega2asm decompression) | 25% |
     | PTD | 3.5% |
     | P | 1.9% |
     | IL | 58% |
     | DREF | 28% |

   - The six images that influenced v1 are now exposed validation, and v2 is a disclosed measurement correction on already-seen data.
3. **Ghidra is not limited by loading or seeding.** The existing recipe recovers 52% of true starts on the exposed images at a 1.1%
   false-start rate. Vector seeding gives 53% and 1.1%.
4. **Static negatives are trivial.** Static negative cells are almost all padding: 2,657 trivial versus 58 hard on the development set.
5. **Indirect dispatch (bounded spike, about 5 CPU-hours).**
   - The observed multi-target sites are 2 on Chuck Rock (`JSR (A1)` from RAM objects) and 5 on d07 (PC-indexed `JMP` without stored
     pointers).
   - None has a ROM-loaded table at the site, a bounds check, or a provable full target set.
   - Runtime targets are observable but cover only the executed ones, which execution evidence already labels (d07: 27,428 known bytes,
     0 wrong).
   - A generic record-table verifier (RT) found Chuck Rock's 24-byte object records (10 roots, +4 positive cells). Nothing on d07 lets its
     precision be measured, so it is not admissible.
6. **Better dynamic evidence.**
   - A public TAS input log (TASVideos 827M, Gens) replays deterministically in MAME.
   - It adds 843 ROM PCs to T001's 5,609 (+15%) for 2.4 CPU-hours, then plateaus, most likely through desync.
   - On Chuck Rock it adds mostly observed data: non-trivial negatives rise from 278 to 486, positive cells only from 290 to 298, and mixed
     cells from 25 to 29.
   - A random-seed diagnostic had already shown seeds reaching near-identical states, so more seeds were not run.
7. **Final Chuck Rock evidence (v2 tiers plus all execution).**
   - 298 positive cells (3.6%), 1,088 negative cells (about 486 non-trivial) and 29 mixed.
   - The code comes almost entirely from execution.
   - **0 positive cells carry an instruction trigram absent from SEG-072/075.** Held-out SEG-072 images show 0.6-4.1%.
8. **Usefulness simulation (frozen and reviewed; model A; 102 fits; source-backed images; partial labels generated without truth).**
   - Adding a partially labelled lineage changes held-out ER by D = -0.017 on average over 11 folds, with sign-flip p = 0.22:
     `NO_MEASURABLE_EFFECT`.
   - The effect is concentrated in two folds and two added lineages (Sonic, Shining Force).
   - Partial labels alone are a poor substitute for truth (for example, ER 0.89 against 0.15).
   - Static partial labels on commercial images are nearly error-free but cover only 1-8% of the bytes.

## Decision

**`STOP_ACQUISITION`.** Under every tested approach, new commercial titles would yield:

- **little code.** Static recovery of commercial dispatch is not credible at the admission error bar, so code comes from execution, which
  is bounded by input reach;
- **no new structure.** Chuck Rock adds no structure relative to SEG-072/075;
- **mostly trivial static negatives.** The useful data evidence (DMA and reads) is execution-bound;
- **no measurable training effect.** The frozen simulation finds no measurable effect from adding partially labelled lineages;
- **a cost that does not scale.** About 7-12 CPU-hours per title, plus a per-title deterministic input recording, which does not scale to
  diverse engines.

The qualified corpus is preserved:
- G077-01 under v2 tiers plus execution (classes digest `a7a601ed...`);
- T001/T002's records;
- the superseded farm.

The milestone continues only with scientifically qualified data. T010 (the 16-title acquisition) is not started, and no blind-cohort
input is touched.

**Reopening criteria (a new protocol, not an amendment).**
- **Input recordings.** Operator-recorded deterministic MAME input recordings (`-record`), or verified-sync movies, for at least two
  structurally different engines.
- **Cheap novelty check first.** A cheap novelty check is run on each before acquisition: novel-trigram positive cells must be at least
  the SEG-072 hold-one-out floor (0.6%).
- **Adequately powered study.** The usefulness study is re-run with enough lineages to be powered.

## Consequences

- SEG-077's research question (were SEG-076's results limited by data?) cannot be answered with new commercial Genesis lineages using
  credible, scalable evidence. Any final ADR will be data-limited.
- Reusable outcomes:
  - the corrected measurement and three-decoder tooling;
  - the deterministic movie/edge tracer;
  - the decompression helper (sega2asm decompressors, measured unreliable for labels);
  - the record-table verifier (unadmitted).
