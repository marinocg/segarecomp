# ADR 0174: SEG-077 static-first evidence audit, dispatch and dynamic-evidence spikes, and the acquisition decision

- Status: accepted (independent results review CONCUR_WITH_FINDINGS; findings F1-F6 applied)
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
   - J fails through its table-data boundary (JT: 4 wrong of 670 exposed bytes) and through one development image (d11: 8 of 8 known
     bytes wrong). J's code side is clean: 0 wrong of 29,288 known exposed predictions.
   - Every other tier fails:

     | Tier | Wrong rate (exposed set, v2 admission record) |
     |---|---|
     | CD (dynamic-call fall-through) | 4.1% |
     | G (Ghidra-corroborated roots) | 0.35% |
     | DC (sega2asm decompression) | about 25% |
     | PTD | about 3.5% |
     | P | about 1.9% |
     | IL | about 58% |
     | DREF | 32% (development: 26%) |

   - The six images that influenced v1 are now exposed validation, and v2 is a disclosed measurement correction on already-seen data.
3. **Ghidra is not limited by loading or seeding.** The existing recipe recovers 52% of true starts on the exposed images at a 1.1%
   false-start rate. Vector seeding gives 53% and 1.1%.
4. **Static negatives are trivial.** Static negative cells are almost all padding: 2,657 trivial versus 58 hard on the development set.
5. **Indirect dispatch (bounded spike, about 5 CPU-hours; corrected after the independent results review).**
   - **d07 (source-backed).** All 8 observed multi-target sites (25 targets) use the standard bounded switch idiom: load an immediate,
     compare, unsigned branch as the bounds check, scale, a PC-relative word load from a ROM offset table, then a PC-indexed jump.
     - Decoding each table over its bound plus one entry gives 125 targets. All are true instruction starts, and the decoded set covers
       every observed target.
     - The full target sets are therefore provable statically. The original analysis looked for 32-bit pointers and missed 16-bit offset
       tables; that error is corrected here.
     - This is a credible, generic static rule. The current J tier misses the variant with intervening scaling instructions.
   - **Chuck Rock.** Its 2 observed multi-target sites are register-indirect calls whose pointer comes from RAM objects.
     - No ROM table is loaded at the site, there is no bounds check, and the full target set is not provable.
     - All observed targets also occur in fixed-size ROM records. A generic record-table verifier (RT) recovers 10 roots (+4 positive
       cells). Its precision cannot be measured, because d07 has no such tables, so RT is not admissible.
   - **Execution precision (d07).** 10,027 executed PCs: 7,563 on true starts, 2,464 on reference-unknown, 0 on truth c or D.
   - **Chuck Rock's labelled code** contains no bounded-switch idiom, so a switch rule would not change its evidence.
6. **Better dynamic evidence.**
   - A public TAS input log (TASVideos 827M, Gens) replays deterministically in MAME.
   - It adds 843 ROM PCs to T001's 5,609 (+15%) for 2.4 CPU-hours (two replicas of about 1.2 h).
   - Growth shows diminishing returns: 5,705 PCs at 120 s and 6,289 at the movie's end. Desync (the TAS was made in Gens) is a hypothesis,
     not shown.
   - On Chuck Rock it adds mostly observed data: non-trivial negatives rise from 278 to 486, positive cells only from 290 to 298, and mixed
     cells from 25 to 29.
   - A random-seed diagnostic had already shown seeds reaching near-identical states, so more seeds were not run.
7. **Final Chuck Rock evidence (v2 tiers plus all execution).**
   - 298 positive cells (3.6%), 1,088 negative cells (about 486 non-trivial) and 29 mixed.
   - The code comes almost entirely from execution.
   - **None of the labelled positive cells contains an instruction trigram absent from SEG-072/075.** Held-out SEG-072 images show
     0.6-4.1%.
   - This covers only the 3.6% of cells that are labelled, which are execution-derived and biased toward common routines. It uses a
     coarse vocabulary (989 reference trigrams) and a single title.
8. **Usefulness simulation (frozen and reviewed; model A; 102 fits; source-backed images; partial labels generated without truth).**
   - Adding a partially labelled lineage changes held-out ER by D = -0.017 on average over 11 folds, with sign-flip p = 0.22:
     `NO_MEASURABLE_EFFECT` under the frozen rule.
   - **The study is underpowered, not evidence of no effect.** In 9 of 11 folds, dropping a lineage barely changes held-out ER compared with
     keeping all truth, so there is almost nothing to recover.
   - Where there is something to recover, partial labels recover most of it:

     | Held-out fold | Dropped lineage | ER: all truth / dropped / partial labels | Loss recovered |
     |---|---|---|---|
     | D01 | D03 | 0.153 / 0.743 / 0.328 | about 70% |
     | D03 | D01 | 0.054 / 0.197 / 0.039 | more than 100% |

     These are single fits per condition, so fit instability is possible.
   - Partial labels alone are a poor substitute for truth (for example, ER 0.89 against 0.15).
   - Static partial labels on commercial images are nearly error-free but cover only 1-8% of the bytes.

## Decision

**`STOP_ACQUISITION`.** Under every tested approach, new commercial titles would yield:

- **little code.** On Chuck Rock, static rules add essentially no code (V adds 2 bytes). Its dispatch is register-indirect through RAM
  objects, outside what any admissible static rule can prove, so code comes from execution, which is bounded by input reach. (Bounded
  switch tables, as on d07, are statically provable, but Chuck Rock's labelled code contains none.)
- **no new structure in the labelled cells.** Chuck Rock's labelled positive cells contain no new structure relative to SEG-072/075;
- **mostly trivial static negatives.** The useful data evidence (DMA and reads) is execution-bound;
- **an inconclusive training value.** The frozen simulation is inconclusive: recoverable signal appears in only 2 of 11 folds, and those are
  unreplicated single fits;
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
- **Adequately powered study.** The usefulness study is re-run with enough lineages to be powered, including seed-replicated fits for the
  D01/D03 folds where partial labels recovered most of the loss.
- **Bounded-switch rule.** A separately admitted bounded-switch tier (the d07 idiom) is a credible generic static improvement for titles
  that use it.

## Consequences

- SEG-077's research question (were SEG-076's results limited by data?) cannot be answered with new commercial Genesis lineages using
  credible, scalable evidence. Any final ADR will be data-limited.
- Reusable outcomes:
  - the corrected measurement and three-decoder tooling;
  - the deterministic movie/edge tracer;
  - the decompression helper (sega2asm decompressors, measured unreliable for labels);
  - the record-table verifier (unadmitted);
  - the bounded-switch observation for a future static tier.
