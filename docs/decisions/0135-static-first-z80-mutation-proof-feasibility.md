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
