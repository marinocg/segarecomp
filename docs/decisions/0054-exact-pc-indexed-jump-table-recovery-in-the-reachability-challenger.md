# ADR 0054: Exact PC-Indexed Jump-Table Recovery in the Reachability Challenger (Experiment)

- Status: Accepted (decision: REFINE; next experiment: provenance of mutable-state dispatch indices, measured with
  STOP as an expected possible outcome; broad Gen-2 immutable-ROM AOT stays the production representation)
- Date: 2026-09-29
- Task: SEG-026-T002
- Related, unchanged: ADR 0053 (the challenger and the coverage oracle this experiment extends), ADR 0051
  (operand-width region expansion snowballs; not reintroduced), ADR 0009 (typed `(d8,PC,Xn)` EA), ADR 0002
  (static translation, fail closed).

## Question

Can challenger-reachable `JMP/JSR (d8,PC,Xn)` sites be resolved from immutable-image jump tables using only locally
proven static facts? If so, how much observed Sonic attract execution does that unlock, and does the challenger
stay dramatically smaller than broad AOT?

Everything below is report-only. Admission, generated C, emission and runtime execution authority are unchanged.

## Mechanism (retained, off by default: `--pc-index-recovery`)

```text
reachable (d8,PC,Xn) site
  -> backward, demand-driven proof of the index register's exact finite domain
  -> exact entries read from uniquely owned immutable cartridge bytes (when the slice loads a table)
  -> exact target identities -> ordinary challenger discovery -> deterministic fixed point
```

- **Value domain** (`libs/cpu/m68k` `finite_register_values`). A value is Unknown, or an exact finite set of
  register values modulo 2^16 (`.W` index) or 2^32 (`.L` index). The limit is 4,096 values; this is a resource
  bound (the site stays unresolved), not a table extent. Supported transfers:
  - MOVEQ, CLR and immediate MOVE;
  - MOVE Dm,Dn;
  - a byte from mutable memory (0..255);
  - word/long entries from immutable image bytes (`(d8,PC,Xn)`, `d16(PC)`, absolute);
  - AND/OR/EOR/ADD/SUB with immediate, quick or register sources;
  - LSL/ASL/LSR/ASR by an immediate count;
  - EXT.W/EXT.L (taken before the effect whitelist, which does not list them).
  A known mask applied to an unknown register gives the exact submask set. Control transfers and NOP write no
  data register. An AND whose register source is width-derived stays width-derived. MOVEQ uses its decoded destination: the effect owner deliberately keeps a legacy D0-only MOVEQ
  footprint. Every other writer yields Unknown. There is no loop reasoning and no memory model.
- **Guards.** A CMPI/CMP #imm/TST + Bcc edge filters the value set exactly, using the existing
  `m68k_evaluate_subtraction` and `M68kConditionSpecification`. The filter applies only when the branch's sole
  predecessor is the physically preceding flag setter.
- **Predecessors.** The proof walks only the challenger's own discovered graph: fixed-flow edges, direct call edges
  into a callee, and already-recovered PC-indexed edges. Machine roots and every stacked call/exception
  continuation are opaque (Unknown). A cycle is Unknown.
- **Strict extent policy.** A domain whose upper extent is only the width of a byte loaded from mutable memory
  (`width_derived`: 0..255 carried through copies, table loads, additions and left shifts, never cut by a mask,
  right shift or guard) is `width_only_domain` and stays unresolved. It is sound, but it is an operand-width bound,
  not an explicit table-extent proof. `--pc-index-width-domains` admits such domains as a labelled measurement
  variant only.
- **Fail closed.** The whole site stays unresolved (never a partial target set) when:
  - an entry read leaves the uniquely owned immutable image;
  - a target is not a mapped image PC.
  Odd targets and misaligned word/long entry reads raise an address error, so they are excluded and counted.
- **Fixed point.** Recovery runs after the ordinary closure and repeats until no site's target set changes. A proof
  is relative to the predecessors the challenger knows. If a later edge makes a resolved site lose its proof or any
  target, discovery restarts from scratch with that site pinned `invalidated`. The pinned set only grows, so this
  terminates.
- **Never used:** runtime coverage, broad-AOT identities, hints, external disassembler targets, operand-width
  regions, plausibility scans, table-size guesses or title-specific addresses.

Runtime coverage only falsifies: `tools/reachability_coverage_compare.py` counts every observed first entry whose
witness predecessor is a resolved site but which lies outside that site's proven set (a recovery escape).
Classification-only labels (`--classify-pcs`) apply the same strict proof to observed PC-indexed sites. That proof
runs over a predecessor graph built only from the classified instructions' fixed successors, with machine roots and
stacked continuations opaque, to measure the remaining PC-indexed graph. A block also entered by an unclassified
dynamic transfer can still be labelled optimistically. Those labels never enter D.

The architecture stays one-shot and static: ROM → static challenger → finite proven targets → D. Runtime
execution never modifies D. There is no runtime decode, JIT, interpreter, runtime target learning or
regenerate-after-observation loop.

## Workload

- The unchanged SEG-026-T001 oracle: pinned authorized Sonic 1 image, no input, 23,200 no-render frames.
- Re-run at this task's head. It reproduced T001 exactly:
  - `frames_reached`, 224,352,923 dispatches, 292,926,087 retirements;
  - O = 10,512;
  - coverage digest `b41bbfcc…578c` and final-state digest `01eab827…d947`, identical to T001.
- The challenger outputs are byte-identical across runs.

## Results (Sonic 1; sanitized aggregates)

| measure | T001 baseline | T002 strict | T002 width-domain variant |
| --- | --- | --- | --- |
| `U` | 246,293 | 246,293 | 246,293 |
| `D` | 1,276 | 6,765 | 3,070 |
| `D/U` | 0.52% | 2.75% | 1.25% |
| `O ∩ D` | 1,023 | 4,493 | 2,250 |
| `O − D` | 9,489 | 6,019 | 8,262 |
| `D − O` | 253 | 2,272 | 820 |
| observed recall | 9.73% | **42.74%** | 21.40% |
| recall after the first demo cycle (frame 3,100) | 14.3% | 52.7% | 29.8% |
| overlapping instruction starts | 0 | 70 | 63 |
| decoder-rejected targets | 0 | 1 | 0 |
| exception-raising decodes (ILLEGAL/line A/F) | 0 | 31 | 13 |
| recovery escapes (observed first entries outside proven sets) | — | 0 | 0 |

**PC-indexed sites (strict).**
- 14 encountered: the 4 original first-gate sites (round 0) and 10 exposed by recovered code (round 1).
- 4 resolved (2 JMP, 2 JSR); 96 exact targets; 2 recovery rounds; 0 restarts.
- 10 unresolved:
  - 9 `width_only_domain`;
  - 1 `index_unknown`, whose index is entered through a return continuation.
- None failed on out-of-image entries or targets, and none hit the resource limit.

**The four original gates.** Three resolved:

| site | targets | proof |
| --- | --- | --- |
| 1 | 8 | mask selecting an inline BRA.W table |
| 2 | 25 | mask, then a word-offset table |
| 3 | 5 | zero-extended byte, CMPI/Bcc lower and upper guards, SUBI, LSL #2 |

The fourth is a lower-guarded byte whose upper bound is only the byte width. It is `width_only_domain`: the proof
reaches it across a direct call edge, correctly, and stays unresolved.

**Proof mechanisms of resolved sites.**
- mask 3, zero-extended byte 4, immutable entry load 2, add/sub 2, guard 1, shift 1, logical 1;
- one resolved site depends on a recovered edge.

Targets observed per resolved site: 3/5, 4/8, 11/25, 9/58. Attract mode does not exercise every real target, so an
unobserved target is not thereby accidental.

**Graph-growth and accidental-decode indicators.**
- Recovery added 5,489 instructions. 3,470 of them were observed.
- Site 2's mask proves 32 entries, but the table is shorter. 29 of its 32 entries overlap discovered code (48 of
  117 entries read overall).
- One over-read entry lands mid-sequence at another dispatch. That dispatch's only known predecessor is then this
  accidental edge, which yields the 58-target round-1 resolution. The runtime never escaped that set, but the
  proof is weak: its real entry path is not in D.
- Overlapping starts (0 → 70) and exception-raising decodes (0 → 31) show a moderate, bounded accidental-decode
  tail. There is no snowball.

**Width-domain variant.**
- Admitting width-only domains resolves more sites early.
- Their over-read entries then add edges that invalidate four proofs, including an original first-gate site, which
  is pinned. The result: D = 3,070, recall 21.4%, lower than strict.
- A width bound is therefore not merely imprecise: it destabilizes the exact proofs. The strict policy stands.

**Next gates (strict, structural attribution over the unchanged oracle).**

| first gate | missing PCs | sites |
| --- | --- | --- |
| `JSR (An)` | 5,367 (89.2%) | 2 |
| `JMP (d8,PC,Xn)` (width-only / unknown) | 616 | 7 |
| `JSR (d8,PC,Xn)` | 25 | 1 |
| `JMP (An)` | 11 | 1 |

- Nearest mechanism: PC-indexed for 5,622 missing PCs (93.4%), `(An)` for 397.
- 3 `(An)` sites are reachable after recovery (2 `JSR (An)`, 1 `JMP (An)`), all executed.

**Is the remaining PC-indexed graph manageable?** The classification-only strict labels over the observed
fixed-flow graph:
- 84 observed PC-indexed sites: 13 explicitly bounded, **70 width-only**, 1 opaque (return continuation);
- of the 72 outside D: 10 explicitly bounded, 62 width-only;
- missing PCs by the domain of their nearest PC-indexed site: width-only 3,722, explicitly bounded 1,870,
  unknown 30.

Across every dynamic step on each missing PC's structural chain back to D:
- **5,681 of 6,019 missing PCs (94.4%) lie behind at least one width-only PC-indexed dispatch;**
- only 308 (5.1%) lie behind nothing but `(An)` and explicitly bounded PC-indexed sites. `(An)` provenance plus
  this mechanism could therefore add at most about 2.9 recall points.

**Supplementary (static only; Sonic 2 is not an attract oracle).**
- D grows from 335 to 6,660 (U = 496,387, D/U 1.34%).
- 1 of its 2 first-gate JSR sites resolves (16 targets).
- 24 sites encountered: 9 width-only, 13 unknown, 1 target outside the image.

## Decision

**REFINE.**
- Exact recovery of explicitly bounded tables is a major, cheap gain: recall rises 9.7% → 42.7% (4.4×) while D
  stays 36× smaller than U, with zero escapes among observed first entries.
- The four original gates were not the whole problem. They exposed a larger PC-indexed graph, and most of it is not
  a graph of explicitly bounded tables: 70 of the 84 observed PC-indexed sites select entries with a mutable RAM
  byte whose only bound is its width.
- The remaining misses concentrate in exactly one generic family: 94.4% of `O − D` is behind width-only
  PC-indexed dispatch (mutable-state dispatch indices).
- `(An)`, the expected T001 candidate, is the structural first gate, but it would unlock at most about 5% of
  `O − D` on its own. It is deliberately not selected.

**Next experiment (exactly one).** Measure whether the index domains of width-only PC-indexed dispatch can be
proven from static stores to their mutable-state source. Examples of stores: constants and bounded updates written
to a state byte.
- Classify the index sources by generic kind (absolute RAM byte vs `(An)`-relative field).
- Prove exact store sets only where their completeness is itself provable; that is, no unanalysed store may alias
  the location.
- Measure recall, D/U and falsification with the same oracle.

If completeness needs general pointer/alias analysis or value-set analysis, record STOP for the reachability-first
direction.

## Limits

- **Relative proofs.** A proof is relative to the challenger's known predecessors. A block also entered through an
  unresolved dynamic site, or through an undiscovered real path, can be mis-proven. The invalidation restart covers
  only edges the challenger later discovers. Runtime escapes are checked (0 observed), and they are the falsifier
  for this limitation.
- **Interrupts.** Interrupted code is assumed to see its data registers preserved by handlers. The exception model
  is unchanged.
- **Mask over-reads.** A mask is sound but may exceed the real table, so over-read entries become targets (see
  indicators). No plausibility filter is applied, by design.
- **Recovery and escape counts.** They cover the no-input attract workload only; unobserved targets are not
  classified as accidental. The escape check sees first-entry witnesses only (14 first entries from resolved
  sites; 27 proven targets observed): a later transfer to an already-observed PC is not checked.
- **Mask bounds.** Any mask that cuts the maximum counts as explicit, even a weak one (for example `#$FE` on a
  byte, 128 values).

## Revisit when

- A later experiment proves mutable-state index domains without general alias analysis. Then combine it with this
  mechanism and `(An)` provenance and re-measure.
- A production admission proposal reconsiders selective discovery; nothing here authorizes one.
