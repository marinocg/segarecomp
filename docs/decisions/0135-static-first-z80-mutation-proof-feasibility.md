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

Roots: the hardware vectors $0000 (reset state), $0038 and $0066 (unknown state). No root, address, value or form is supplied from any
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
