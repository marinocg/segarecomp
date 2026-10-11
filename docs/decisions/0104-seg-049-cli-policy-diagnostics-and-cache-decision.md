# ADR 0104: SEG-049-T005 CLI policy contract, diagnostics and the cache decision

- Status: accepted (checkpoint C3). No default/recommendation change; `optimized` stays v1; gen2 stays an explicit opt-in (ADR 0103).
- Contract of record: `docs/development/aot-policy.md` (status schema, experimental opt-in) and the normative lifecycle contract of ADR 0103.

## 1. CLI contract

- Normal controls stay two: `--aot-policy compatibility|optimized`. The only model-related control is the documented experimental opt-in `--aot-model gen2` (valid only with
  `optimized`; duplicates and misuse are usage errors, identically in `build` and `aot-policy-identity`). There is no model file, threshold, rank-budget or schema control.
- `status.json` `aot_policy` is the machine-readable result consumed by the launcher (no human text is parsed): requested policy and model, effective producer, fallback + stable reason +
  class, model family, model / schema / token-contract / rank-policy / rank-plan / weight-table identities, decision-unit and universe counts, `selected_ppm` / `admitted_ppm`
  (integers), validator state, `policy_identity`, per-build `identity` and `cache_reuse`. A fallback never reports `effective` as an ML producer and carries `validator=not_run|rejected`.
- Authority and fallbacks are exactly as ADR 0103; human log lines remain sanitized one-liners (`segarecomp: m68k admission: ...`) and are not a consumer interface.

## 2. Cache decision: key-only, no new subsystem

Measured with the shipped `segarecomp build` route (gcc -O1 generated code, `--jobs 16`, one 16-core host, single samples):

| image | mode | wall | CPU (user+sys) | peak RSS | effective |
| --- | --- | --- | --- | --- | --- |
| Sonic 1 (512 KiB) | Compatibility | 62.7 s | 678.9 s | 546 MiB | broad |
| Sonic 1 | Optimized + gen2 | 34.0 s | 307.9 s | 304 MiB | `ml_region_gen2`, selected 35.0 %, admitted 35.2 % of the broad universe (K = 86,654 of 246,293) |
| Flicky (128 KiB) | Compatibility | 237.2 s | 1,554 s | 379 MiB | broad |
| Flicky | Optimized (v1) | 240.4 s | 1,524 s | 380 MiB | broad (fallback `prune_rejected`, see below) |
| Flicky | Optimized + gen2 | 256.1 s | 1,574 s | 379 MiB | broad (fallback `prune_rejected`, see below) |

The model-specific work (token decode + inference) is 0.08-1.6 s + 0.3-5.3 s for the five corpus images (ADR 0102), i.e. ~1-2 % of a Compatibility build and ~5-15 % of the
*saved* time on Sonic 1. Even re-running it once per alias-preparation round (Flicky runs 16 emission rounds) costs ~5 s of ~250 s. A separate inference/admission cache would
save at most a few percent, would add an invalidation surface (model, schema, token contract, rank policy, analysis contract, ROM) and a staleness risk for no user-visible gain, so it is **not
built**. What is implemented is the correct identity/invalidation only: `policy_identity` (ROM-independent) and `identity` (per build) bind every component, are deterministic, and are
tested for no cross-model, cross-policy or cross-contract aliasing (ADR 0103). The shipped CLI never reuses an output (`cache_reuse: none`); the optional generated-object cache of the
Python bridge (ADR 0046) is content-addressed on the generated C and is not part of the shipped CLI, so "cache enabled vs disabled" does not apply to `segarecomp build`; instead the
generated output is tested to be deterministic across builds and byte-identical to Compatibility on a fallback.

## 3. Finding (not fixed here): alias / RAM-thunk builds fall back

`segarecomp build` iterates emissions while discovering ADR 0049 copy aliases and work-RAM thunks. For Flicky the first emission (no aliases) is admitted by both v1 and gen2 but the following 15
rounds (alias + RAM thunk identities materialized from ROM source bytes) are rejected by the structural stage as `prune_rejected/outside_region`: the materialized identities lie outside the
proposed region, so the build correctly falls back to the broad program (identical to Compatibility, reported as a fallback). This is pre-existing for v1 (it never saw this because v1 also
misses Flicky code, ADR 0099) and is not an ML regression; making alias-source ranges certain-code seeds would change the proposal semantics and is therefore left to the T008 evidence/adoption
decision. Sonic 1 (no aliases) is admitted by the same route.
