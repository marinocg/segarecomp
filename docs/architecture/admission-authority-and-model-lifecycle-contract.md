# Admission authority and ML model lifecycle contract (normative)

- Status: normative for Genesis M68K selective admission as of SEG-049-T004 (ADR 0103). Owner: `platforms/genesis/machine` (`ml_admission`).
- Scope: which source decides the admitted instruction set of a build, how an ML region proposal is identified, versioned, cached and retired, and how every
  non-success is classified and reported. It does not change the structural prune, the admission validator, the CPU semantics or any generated runtime.

## 1. Authority order (highest first)

1. **Exact authoritative map.** An explicit `--immutable-rom-aot-admission <plan>` (CLI) / `--admission-plan <plan>` (build) is an exact map produced by an exact-map plan builder.
   When present it decides admission and **no ML producer runs** (the options are mutually exclusive; `segarecomp build` reports `effective=admission_plan` or, when the plan's
   ADR 0049 alias set differs from the emission's, `effective=broad` with `reason=plan_alias_set_mismatch`). A requested ML model never overrides it and never fills its gaps.
2. **Eligible validated native ML proposal.** Only when the policy is `optimized` on an applicable platform (Genesis M68K), no exact map is present and the requested producer is
   compatible (section 3). The producer proposes a region `R`; the unchanged structural prune derives `K`; the unchanged production validator (`apply_genesis_hybrid_admission`)
   accepts or rejects the resulting ordinary hybrid plan. Only a validator-accepted plan changes the emission.
3. **Broad compatibility fallback.** Everything else, always available, always correct. A fallback is never reported as selective execution.

The validator is the sole correctness authority and is not modified by any ML producer. An ML producer cannot read or write validator state; its output is a plain
`segarecomp.m68k_executable_regions.v1` proposal (ranges + ROM digest). No ML type, score or identity reaches the validator, the emitter or the generated runtime.
ML does not claim to prove that unselected code is never executed; only the broad program is complete by construction.

## 2. Producers and user-facing policy

| user control | meaning in this release |
| --- | --- |
| `compatibility` (default) | broad AOT; no producer runs |
| `optimized` | ML selective admission with the **v1** producer (`ml_region`, SEG-047). Unchanged by SEG-049-T003..T007 |
| `optimized` + `--aot-model gen2` (build) / `--immutable-rom-aot-ml-gen2-admission` (emit) | **experimental opt-in** gen2 producer (`ml_region_gen2`: M3 + B35). Never selected implicitly |

`optimized` is not repurposed: its producer, report identifiers and cache identities are the v1 ones. Whether gen2 becomes recommended or the default is decided only by SEG-049-T008
(which requires fresh independent source-backed evidence; ADR 0102 section "Limits"). There is no user-visible model file, threshold, schema or rank-budget control.

## 3. Identities (all embedded in the binary; nothing is read from disk or the network)

A producer is identified by seven components. The **compatibility identity** of a build is the SHA-256 of their canonical text together with the requested policy/model, the
effective producer and the admitted-set digest (`k_sha256`); it is what `status.json` reports as `aot_policy.identity` and what a consumer may use as a stable cache/report key.

| component | v1 (`ml_region`) | gen2 (`ml_region_gen2`) |
| --- | --- | --- |
| producer id | `ml_region` | `ml_region_gen2` |
| model id | `seg046-features-v1` (artifact digest pinned in `ml_region_producer.cpp`) | model SHA-256 `11258708...98e1` |
| representation / feature schema | feature-schema SHA `d2e7c829...f570` | representation SHA `cf43e758...0c67` (`seg049-gen2-repr-v1`) |
| CPU token contract | not used | token-schema SHA `9a1f4f4f...1d75` (`segarecomp.m68k_decode_tokens.v1`) |
| rank / selection policy | frozen logit threshold | `B35` (rank plan SHA `8c4d98b9...1e29`) |
| analysis contract | `segarecomp.admission_contract.v1` (the prune + validator semantics both producers feed) | same |
| weight table | folded-weight digest pinned | binary32 weight-table SHA `72699dbc...7d98` |

Any change to any component is a **new** identity: it must change the pinned constants, the generated table and the golden digests together, and the cache identity changes with it.
The analysis-contract string is bumped by any change to the structural prune or the validator that can alter an admitted set.

## 4. Compatibility, deprecation and incompatible models

- v1 stays supported and selectable (`optimized`). Its report/cache identities for existing builds are frozen (regression-tested). It may be superseded only by a later,
  evidence-gated decision (T008) and then only through an explicit, documented transition; this release performs none.
- A producer whose embedded identity does not verify against the pinned constants (model, tables, token schema, representation, rank plan, policy) is **incompatible**: it fails
  closed with `reason=model_identity` and the build falls back to broad. There is no partial use, no best-effort loading and no auto-upgrade.
- There is exactly one model per generation embedded in a binary; there is no model registry, no discovery of model files and no user-managed model. A different model ships only as a
  new binary with a new identity.
- A cached build is valid only for the identity that produced it. A consumer never reuses an entry whose policy, model family or identity differs from the current request.

## 5. Fallback classification and reporting

Every `optimized` build reports, machine-readably (`status.json` `aot_policy`), at least: `requested`, `requested_model`, `effective`, `fallback`, `reason`, `fallback_class`,
`model_family`, the identity components above, `selected_ppm`, `admitted_ppm`, `validator`, `policy_identity`, `identity` and `cache_reuse` (`effective` names the producer that actually ran). `fallback_class` is one of:

| class | reasons (stable codes) |
| --- | --- |
| `none` | `none` |
| `producer_incompatible` | `model_identity` |
| `producer_input` | `rom_size`, `token_range`, `non_finite_score`, `empty_proposal`, `no_analysis`, `no_report` |
| `proposal_rejected` | `prune_rejected` (+ sanitized detail class) |
| `validator_rejected` | `validator_rejected` |
| `not_applicable` | `platform_not_applicable` |
| `exact_map` | `exact_plan_precedence` (not a fallback), `plan_alias_set_mismatch` (fallback to broad) |

A fallback sets `fallback=true`, `effective=broad` and `validator` to `not_run`/`rejected`; its generated code is byte-identical to the Compatibility build. A consumer (the launcher) must
read these fields and never parse human-readable log text.

## 6. Caching

There is no separate inference/admission cache: the measured inference cost (ADR 0102) is small next to generation and compilation, and an admission result depends on the ROM,
the producer identity and the analysis contract, all of which are in the compatibility identity. Reuse of a previously built program is the consumer's decision and must be keyed on the
requested policy, requested model family and the product-provided identity inputs (`segarecomp aot-policy-identity`), never on launcher-private model knowledge. A consumer must key on `policy_identity` as well as `identity`: the frozen v1/Compatibility `identity` text predates the analysis-contract string, so a contract bump changes `policy_identity` (which includes it) but not those `identity` values.
