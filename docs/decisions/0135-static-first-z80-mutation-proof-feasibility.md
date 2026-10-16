# ADR 0135: Static-first Z80 code-mutation proof - feasibility spike and ML comparison (SEG-068-T008)

- Status: Research record (feasibility spike; no production behaviour). Written in two parts: the PRE-REGISTRATION below was committed
  before any real (commercial-derived) input was analysed and before any evaluation result was read; the RESULTS part is appended later.
- Related: ADR 0134 (observed-pass variant enumeration NO-GO), ADR 0073, ADR 0077, ADR 0088, SEG-041-T003, SEG-054 (separate M68K scope).

## Clarification of ADR 0134

ADR 0134's NO-GO applies to the **observed-pass variant enumeration** proposal: discovering structural variants by executing the
generated-native guest and enumerating the forms it faults on. Independently **proved static variant sets** are a possible future
route; observational closure is not their required proof method, and nothing in this spike changes ADR 0134's decision for the
observation-driven design.

## Question

Can possible Z80 structural code mutations be proved from the instruction stream and its code-writing effects alone, without executing
any guest instruction? Separately: can the Z80 image bytes be established statically from the ROM? (Reported separately.)

## Pre-registration (frozen before real inputs)

D0 = deterministic baseline: existing CPU-owned decode (`decode_at`/`classify_all`), effect projection (`project_effect`) and a small
bounded finite-value propagation. Nothing else.

Limits (generic, not derived from any title; exceeding any yields UNKNOWN / incomplete, never a guessed set):

| Limit | Value | Meaning |
| --- | --- | --- |
| `max_values` K | 16 | finite-value cardinality per register / store address / store value |
| `max_starts` | 16,384 | distinct explored instruction starts (the whole 16 KiB logical code window) |
| `max_revisits` | 4 | times a start's joined state may change before it is widened to unknown |
| `max_steps` | 262,144 | total worklist transfer steps |

Soundness rules: unknown or unsupported never becomes a guessed value; a result reports `reachability_complete`, `effects_complete` and
`store_addresses_complete` separately and `candidate_set_complete` is their conjunction. After a CALL the continuation state is unknown
(no interprocedural summary). Unsupported effect forms havoc all registers. Memory loads are unknown. Hardware interrupt pushes are
modelled as stores at SP-2 when SP is finite and mark the result incomplete when it is not. Standing assumption (reported with every
result, never hidden): interrupt handlers are analysed as separate roots with unknown entry state, and their register effects on the
interrupted flow are NOT joined into it; therefore `candidate_set_complete` is reported false whenever an EI is reachable.

Roots: the reset vector $0000 (reset state); $0038 (unknown state) is added when an EI is reachable; $0066 is NOT a root because the Genesis NMI is not connected (audio contract section 8) - a deliberate narrowing of the first draft's three-root list, recorded here. No root, address, value or form is supplied from any
title; the previously observed JR->RET mutation (ADR 0134) is a post-hoc comparison target only.

Synthetic fixtures (project-authored): A constant replacement, B finite alternatives (two branches, and a join), C unknown address/value
(including a pointer incremented in a loop), D overlapping / variable length (shorter, longer, prefix, operand-only), E unsupported
semantics, plus adversarial cases (callee write, stack write, self-overwrite, budget exhaustion).

Verdict classes (reported separately for mutation proof and for the image producer): STATIC_MUTATION_PROOF_FEASIBLE, SYNTHETIC_ONLY,
STATIC_PROOF_TOO_COMPLEX, STATIC_PROOF_UNSOUND, IMAGE_PRODUCER_BLOCKED.

## ML arm pre-registration (committed before any ML-related measurement or evaluation result was read)

Arms: D0 (above); M1 = learned ranker of Z80 code-writer / mutable-site candidates whose proposals go to exactly the D0 verifier; M2 =
learned ranking of M68K loader / copy / decompress / upload routines versus a simple deterministic search D0'. D0' = a byte scan of the
ROM for 32-bit immediates in the Z80 RAM window ($A00000-$A01FFF) and the bus/reset registers ($A11100 / $A11200), reported per title.

Inference inputs (if a model were run): immutable bytes, CPU-owned decode metadata, effect projections, local control-flow context. Never
execution traces, never labels. Labels (training / scoring only): offline observation, source-derived truth or independent examples;
never proof authority. Model output is advisory: only the D0 verifier establishes a variant or an image; no model produces bytes.

Lineage rule (fixed now): two images belong to one lineage when they are byte-identical or share >= 25% of their 16-byte windows; a whole
lineage is entirely in training or entirely in evaluation.

M0 corpus-adequacy gate (a trained comparison requires ALL of): >= 6 independent lineages with independently established labels, of which
>= 3 contain a positive (a structural code writer for M1; a labelled loader routine for M2) and >= 2 positive lineages are held out for
evaluation; labels not derived from the evaluation images themselves. Otherwise: CORPUS_BLOCKER, a data-acquisition recommendation, and
stop. Zero-shot or pre-existing models may be run only as exploratory research and never reported as evidence of generalization.
The SMS D512HP executable-region CNN is not assumed suitable.

Adoption criteria (all required for any production ML): (1) at least one additional case certified by the UNCHANGED D0 verifier that D0
alone does not certify on a held-out lineage, or a >= 50% verifier-cost reduction for the same certified set; (2) zero unsound
certificates under adversarial review; (3) deterministic inference; (4) no new ML framework, decoder, registry or symbolic engine.
Prediction accuracy alone never qualifies. If ML finds a valid candidate the verifier cannot certify, the exact missing proof capability is
recorded and the verifier is not weakened.
Budgets: D0 limits unchanged; no inference budget is spent unless M0 passes.

---

# RESULTS (appended after the pre-registration above; limits and rules unchanged)

Two refinements were made before any real input was analysed and are part of the frozen D0: (a) the result also reports
`closed_under_mutation` (first-order scope: a write that changes a reachable start can expose new code with further stores the analysis
never saw); (b) reset state defines only A (= $FF) and SP (= $FFFF), every other register is unknown (initially all registers were
seeded; corrected after review of soundness, real-image results identical). Cause attribution of unknown values was added for
reporting only and changes no decision.

## Phase A - existing CPU-owned capability (libs/cpu/z80)

`decode_at` / `classify_all` give an exact per-start classification with logical length and prefix accounting; applying a candidate byte
to a copy of the image and re-classifying yields, with no new decoder, the original and modified forms of every overlapping start. `project_effect`
gives: register writes (constant, copy, +constant, load, opaque), at most one memory store with an absolute or register-relative
address and a value expression, and control successors (jump, conditional, call, return, computed). Not described (reported unsupported,
never guessed): EX/EXX, PUSH/POP, every ED and CB space form (block transfers, shifts/rotates, BIT/RES/SET, 16-bit ADC/SBC), 16-bit ADD,
memory read-modify-write, CALL cc, RST, RETI/RETN, EI/DI/IM, HALT, undocumented index-half forms; ALU results are opaque; memory loads are
unknown; flags are not modelled. Smallest missing capability, from the attribution below: a **CPU-owned register/memory write
footprint for the currently unsupported forms** (which registers a form may write, and its store address expression, even when the value
is opaque).

## Phase B - synthetic proof (tests/z80_static_mutation_tests, 36 checks, all pass, incl. the two review regressions S1/S2)

A constant replacement: exact destination and value, original jr (2 B) vs modified ret (1 B) distinguished at a reachable start, both forms
decode through the existing decoder; B two-branch RET/EXX and a join -> finite sets {C9, D9}, nothing guessed; C unknown value, unknown
(memory-loaded) address and a loop-incremented pointer -> UNKNOWN, incomplete; D shorter (3->1), longer via a written prefix (1->4) and
operand-only (payload_only; unreachable overlapping starts may differ and are reported as such); E PUSH/EXX/CALL cc unsupported -> UNKNOWN,
incomplete; adversarial: callee write with caller constants, stack push into code as a width-2 candidate, self-overwrite, step budget,
EI with unknown SP (hardware push destination UNKNOWN), unresolved and resolved computed jumps, mirror aliasing, undefined reset
registers, determinism, wrong image size. A first-order complete set is explicitly NOT closed under mutation when a candidate changes a
reachable start (fixture A).

## Phase C - known images (label: known_image_from_prior_execution; analyzer given only the 8 KiB image and the reset root)

| Image (lineage) | reachable starts | instruction stores (addr exact / unknown) | candidates (structural / payload-only) | observed mutation recovered | complete |
| --- | --- | --- | --- | --- | --- |
| Sega-family driver image shared byte-identically by Aladdin, Earthworm Jim, Jurassic Park | 2,512 | 498 (89 / 409) | 11 (9 / 2) | **yes**: exact destination with exact values for both observed forms (return and exchange-register-set) | no (RET targets unproven) |
| Krusty's Super Fun House (same family, different build, 34% window overlap) | 2,432 | 485 (85 / 400) | 11 (9 / 2) | **yes** (same two forms at its own offset) | no |
| The Lion King (unrelated) | 36 | 9 | 0 | no (reachability stops at CALL cc / RST / block copy) | no |
| Sonic 1 epoch (control, no mutation observed) | 104 | 19 | 0 | n/a | no |

The previously observed mutation was a post-hoc comparison target only. The analysis recovered it blind, from the image and the reset
vector. It also lists additional exact write sites the observation pass never reached (a write that turns the jump's operand into
exchange-AF, and one more exact code-byte site); whether they execute is not claimed (flags are not modelled, both branch edges are taken).
Cost per image: 0.07 s wall / 0.07 s CPU, 14 MB peak RSS, byte-identical output on repeat runs (6/6 images).

Why `candidate_set_complete` is false everywhere (cause attribution of the ~400 unknown store addresses): 36% a call-return continuation
(no interprocedural summary), ~62% an unsupported-effect form that havocs the state (BIT, 16-bit ADD, PUSH, POP, LD ED/CB forms, shifts,
NEG/CPL, EXX, LDIR, ...), the rest memory loads and one widened join. Plus EI reachable, so interrupt handler register effects are
unverified. Consequently the proven fact is **existence of exact-valued code writes**, not the **complete** mutation set; a complete set
(needed before any compiled-variant selection) is not established for any real image.

## Phase D - image extraction, separate from mutation proof

Uploaded images were compared with the ROM (aggregates only). The 38-byte boot stub is a verbatim ROM extent in every title. The 8 KiB driver
image of the Sega-family titles is reassembled entirely from 2-10 exact ROM byte segments with zero uncovered bytes (Lion King: 4 segments, 16
bytes uncovered); Sonic 1's driver is compressed (107 of 7,095 windows match). So for the Sega-family images a **static M68K copy
certification** (exact ROM-to-RAM copies) is a plausible static_proof route (ADR 0077) - that is SEG-054's M68K scope, not done here - while
Sonic 1 would need a decompression proof. SEG-041-T003 recovered Sonic 1's image by executing the decompressor concretely; that is not a
no-execution proof. The deterministic D0' scan of the ROM for Z80-window immediates (M2 baseline) yields 34-345 sites per title (noisy: ROM data
bytes match) and lies within 256 bytes of the source-pointer immediate in the Sega-family titles that have a unique pointer; Lion King's
loader has no such pointer and Sonic 1's source is compressed.

## ML arm (M0 gate evaluated against the pre-registration)

Lineages available with any label: 3 (the Sega-family image [4 titles], The Lion King, Sonic 1). Lineages with a positive recovered by D0: 1.
Required: >= 6 independent lineages, >= 3 positive, >= 2 positive held out. M0 FAILS -> **CORPUS_BLOCKER**; no model was trained and none was run
(no zero-shot run either: no existing model targets this task, and the SMS D512HP CNN is not assumed suitable). Nevertheless the information
argument is decisive for M1: D0's remaining unknowns are unknown addresses/values caused by missing CPU footprints and missing
interprocedural summaries; a ranker can reorder candidates but cannot supply an address or value, and the verifier must not accept one.
Additional certified cases from M1 = 0 by construction; D0 already costs 0.07 s per image, so there is no cost to reduce. For M2, D0' (simple
deterministic search) already localises the loader neighbourhood where a pointer exists, and the labels needed to train a loader-routine ranker
(a handful of lineages, each with one loader) cannot support a held-out comparison. Recommendation: retain D0, reject production ML, and if
ML is revisited, first acquire >= 6 independent lineages with source-derived mutation / loader truth (public sound-driver disassemblies:
Sega standard, GEMS, SMPS variants, Z80 loaders in M68K sources).

## Verdicts

- Mutation proof: **STATIC_MUTATION_PROOF_FEASIBLE** for *existence* of exact-valued code writes (blind recovery of the observed mutation on
  the two real driver images, deterministic, 0.07 s, existing CPU-owned decode/effect only); **NOT** feasible as a *complete/closed* proof
  today: completeness is blocked by the unsupported-form footprint and by interprocedural continuation (STATIC_PROOF_TOO_COMPLEX is not claimed:
  the missing piece is small and CPU-owned for 62% of the unknowns). The independent adversarial review found TWO unsound-certificate
  cases in the first revision (S1: a supported RET has no followed successor yet reachability was reported complete; S2: closure ignored
  payload-only changes although an operand write can retarget control flow). Both were fixed in this PR (every reachable RET/RET cc/RETI/RETN is
  `return_target_unproven` -> reachability incomplete; closure requires that no candidate changes ANY reachable start) and are regression
  tests S1/S2. No real image was ever certified complete.
- Image producer: **IMAGE_PRODUCER_BLOCKED** for guest-free construction in general (Sonic 1 requires decompression; the Lion King loader is
  not analysed); a static exact-copy route is plausible for the Sega-family images and is SEG-054 territory.
- ML: **CORPUS_BLOCKER; reject production ML**; D0 suffices for everything it can certify.
- Compiled-variant selection still needs, beyond ADR 0134: a complete closed candidate set, one decode per address / selection before
  effects, interrupt-handler register-effect proof, and second-order closure (mutated code re-analysed).

## Single minimal next step

Add a CPU-owned write footprint to `project_effect` for the currently unsupported forms (registers possibly written; store address
expression even with an opaque value), validated against the pinned independent Z80 oracle, then re-run D0 once with the unchanged limits to
see how far `store_addresses_complete` moves. (Not done here: it is a CPU-semantic extension that this spike must not make merely to improve a
real workload.)
