# ADR 0103: SEG-049-T004 admission authority, shared ML admission owner and model/version lifecycle

- Status: accepted (checkpoint C2 of the combined SEG-049-T003..T007 delivery). **Compatibility remains the default; `optimized` remains the SEG-047 v1 route; gen2 is
  an explicit opt-in.** No default or recommendation change (T008 owns that, and requires fresh independent source-backed evidence).
- Normative text: `docs/architecture/admission-authority-and-model-lifecycle-contract.md` (authority order, identities, v1 policy, incompatible models, fallback classes, caching).
- Predecessors: ADR 0080 (hybrid admission), ADR 0094 (structural prune), ADR 0096 (v1), ADR 0102 (native gen2 producer).

## 1. Decisions

1. **One owner.** `platforms/genesis/machine/ml_admission` runs *any* ML producer: producer -> plain `segarecomp.m68k_executable_regions.v1` proposal -> unchanged prune ->
   unchanged production validator (`apply_genesis_hybrid_admission`, applied to a scratch copy; entries are replaced only on acceptance). The previous inline v1 block of
   `main.cpp` was removed; v1 and gen2 now share exactly this code. The prune/validator owner (`hybrid_admission`) knows no producer, no launcher/runtime/emitter library includes a
   producer (enforced by `ml_producer_boundary_test`).
2. **Authority order** exact map > validated native ML proposal > broad fallback. Structural: the exact-map option and the ML options are mutually exclusive at the CLI, and
   `segarecomp build` runs no producer when `--admission-plan` is given (`effective=admission_plan`, `fallback_class=exact_map`), also when gen2 was requested.
3. **No silent repurposing.** `--aot-policy optimized` still means v1. `--aot-model gen2` (build) / `--immutable-rom-aot-ml-gen2-admission` (emit) is the only way to ask for gen2, is only
   valid together with `optimized`, and never changes a Compatibility build. v1 report identifiers (`effective=ml_region`, `model=seg046-features-v1`, report keys) and its
   cache/report identity text are frozen; the report only gained additive keys.
4. **Identities.** Seven components per producer (producer, model, representation/feature schema, CPU token contract, rank policy + plan, weight table, plus the analysis-contract
   string `segarecomp.admission_contract.v1` for the prune+validator semantics). `genesis_aot_build_identity` keeps the frozen `segarecomp.aot_policy.v1|...` text for Compatibility
   and v1 and uses `segarecomp.aot_policy.v2|...` for a gen2 request (including a gen2 fallback, so a gen2-requested build never aliases a v1 or Compatibility one).
   `genesis_aot_policy_identity` (printed by `segarecomp aot-policy-identity`) is the ROM-independent policy identity a consumer keys its cache on.
5. **Incompatible model.** A producer whose pinned digests/tables do not verify fails closed (`model_identity`, class `producer_incompatible`); there is no partial use. Tested by compiling the
   producer against eight deliberately mutated copies of the generated table (weight, BatchNorm statistic, model / token-schema / representation / rank-plan / weight-table digests, policy
   percent) and by a synthetic incompatible producer at the owner level.
6. **Fallback classification** is a stable table (`genesis_ml_fallback_class`); a fallback is `producer=broad fallback=1`, `validator=not_run|rejected`, and its emission is byte-identical to broad.
7. **Embedded models only.** Nothing is read from disk or the network; a different model is a different binary with a different identity. No model registry (two static descriptors).
8. **Cache.** No inference/admission cache is introduced (measured inference 0.3-5.3 s vs generation + compilation; see ADR 0104 for the end-to-end measurement); only correct identity/invalidation.

## 2. Tests (all hermetic, synthetic images)

- `genesis_ml_admission_lifecycle_test` (C++): five classified fallbacks leave the broad entries untouched and never claim selective execution; a whole-image region and a
  reachable-only region are accepted through the validator (unproposed code is not admitted); real v1 and gen2 producers satisfy the admit/fallback invariants; the classification table;
  identities bind every component (mutated producer copies change both identities); the Compatibility, v1 and v1-fallback identities equal their frozen SEG-047 text; no two of eight
  request/effective/ROM/admitted-set combinations alias.
- `segarecomp_aot_model_lifecycle_test` (CLI): emit line, determinism, mutual exclusion, fallback byte-identity, `aot-policy-identity`, `status.json` fields and identities for
  Compatibility / v1 / gen2 builds, usage errors, exact-map precedence over gen2 with no producer run.
- `genesis_ml_gen2_identity_mutation_test`, `ml_producer_boundary_test` as above.

## 3. Limits / honest notes

- The `validator_rejected` branch is defence in depth (the validator is applied to the plan the prune just derived); it is classified and shares the fallback code, but cannot be reached with
  consistent inputs, so no test forces it.
- `cache_reuse` is always `none` in the CLI report: `segarecomp build` never reuses an output; reuse is a consumer decision (the launcher), keyed on the product-provided identity inputs.
