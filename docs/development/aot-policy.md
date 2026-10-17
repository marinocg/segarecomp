# AOT optimization policy (Compatibility / Optimized)

`segarecomp build --aot-policy <compatibility|optimized> [--aot-model <v1|gen2>]` selects how much of the ROM is translated into the generated native program
(ADR 0096). The user-facing concept is a policy, not a model: there is no model path, threshold or coefficient option.

| policy | behaviour | default |
| --- | --- | --- |
| `compatibility` | the broad immutable-ROM universe (every decodable instruction start is translated) | yes (through SEG-047) |
| `optimized` | Genesis M68K selective admission: a frozen, statically embedded region model proposes an executable region, structural pruning derives the admitted set, and the unchanged admission validator decides; any failure builds the broad program instead | no |

Optimized never weakens correctness: the generated program is still static native code (no interpreter, JIT or runtime decoder, no ML at
run time); a region that misses code makes the program stop fail-closed (typed stop), never execute wrongly. An explicit
`--admission-plan` (an exact source/recomp-map plan) outranks Optimized. Master System images have no Optimized producer: the request is
reported as a fallback (`platform_not_applicable`) **until SEG-057-T002, which adds the experimental Master System policy described below**.

### Experimental model opt-in (SEG-049)

`optimized` means the SEG-047 **v1** producer. A second-generation producer (`gen2`: frozen M3 CNN + B35 rank-budget selection, ADR 0100-0102) exists as an
**explicit, experimental opt-in**: `--aot-model gen2` (only valid together with `--aot-policy optimized`; a usage error otherwise). It is never selected implicitly, is
not recommended or the default (SEG-049-T008 decided OPT-IN RETAINED, ADR 0109; a later promotion needs deeper unseen runtime evidence and, for a default, fresh independent source-backed evidence), and Compatibility is unaffected. The
authority order is exact map > validated ML proposal > broad fallback and the admission validator is unchanged
(`docs/architecture/admission-authority-and-model-lifecycle-contract.md`, ADR 0103). `segarecomp aot-policy-identity --aot-policy <p> [--aot-model <m>]` prints the
ROM-independent policy identity a consumer should key its cache on.

### Launcher default (SEG-069-T002)

The packaged launcher (not the `segarecomp build` CLI, whose default stays `compatibility`) selects Optimized for newly imported Genesis games, passing
`--aot-policy optimized --aot-model gen2` (the former Optimized + Experimental model request, with the same cache identity), and Optimized for newly
imported Master System games, passing `--aot-policy optimized` only when `aot-policy-identity --platform master-system` reports it available. See ADR 0137.

## Machine-readable status

Every successful build writes `status.json` with an always-present `aot_policy` member (no parsing of human text):

```json
"aot_policy": {
  "requested": "optimized",            // compatibility | optimized
  "requested_model": "gen2",           // none | v1 | gen2
  "effective": "ml_region_gen2",       // broad | ml_region (v1) | ml_region_gen2 | admission_plan  (the producer that actually ran)
  "fallback": false,                   // true when Optimized fell back to broad
  "reason": "none",                    // none | exact_plan_precedence | platform_not_applicable | model_identity | rom_size | token_range |
                                       // non_finite_score | empty_proposal | prune_rejected | validator_rejected | no_analysis | no_report | plan_alias_set_mismatch
  "fallback_class": "none",            // none | producer_incompatible | producer_input | proposal_rejected | validator_rejected | not_applicable | exact_map
  "model_family": "gen2",              // none | v1 | gen2
  "analysis_contract": "segarecomp.admission_contract.v1",
  // only when an ML producer ran and was accepted:
  "model": "11258708…98e1", "schema_sha256": "cf43e758…",          // v1: "seg046-features-v1" / feature-schema digest
  "token_contract_sha256": "9a1f4f4f…", "rank_policy": "B35", "rank_plan_sha256": "8c4d98b9…", "weight_table_sha256": "72699dbc…",   // gen2 only
  "cells": 128, "ml_selected": 45, "seed_cells": 1,                  // v1: windows / ml_selected / seed_windows (aggregates only)
  "universe": 8187, "k0": 2880, "k": 453, "pruned": 2427, "rounds": 576,
  "selected_ppm": 351562, "admitted_ppm": 55331,                     // selected fraction of decision units, admitted fraction of the broad universe
  "k_sha256": "…",
  "validator": "accepted",             // accepted | rejected | not_run
  "policy_identity": "<sha256>",       // ROM-independent: contract + requested policy/model + producer components (key consumers on this too)
  "identity": "<sha256>",              // per build: image + requested + effective + model/schema(/tokens/rank/weights) + admitted-set digest
  "cache_reuse": "none"                // the CLI never reuses an output; reuse is the consumer's decision
}
```
`identity` is a pure function of the inputs: Compatibility, v1 and gen2 builds of the same image never share it, and a fallback build never aliases a successful
Optimized build. The Compatibility and v1 `identity` values are frozen at their SEG-047 text. No address, byte or per-unit datum is reported.

## Master System Optimized (EXPERIMENTAL, SEG-057-T002, ADR 0119)

`segarecomp build --platform master-system --aot-policy optimized` runs the native frozen D512HP ensemble (SEG-056) at **build time**, proposes the
Z80 instruction starts to compile (32-byte cells, top-two logit mean, structural root cells, 65% budget) and hands the proposal to the SEG-057
admission contract, which validates its identity and integrity. It does **not** prove that omitted starts are never executed: the generated program
stops with the typed `Z80_ERROR_UNADMITTED_PC` the moment it reaches one. Compatibility (the broad immutable-image AOT) remains the default.

- `--aot-model` is a Genesis option: it is refused for Master System (`MODEL_NOT_APPLICABLE`); there is exactly one frozen Z80 model, embedded in the
  binary (no model file). Genesis `v1`/`gen2` are never Z80 model names.
- Any build-time failure (`model_identity`, `producer_input`, `inference`, `admission_rejected`) emits the broad program byte for byte and reports the
  reason and class (`producer_incompatible`, `producer_input`, `producer_failed`, `proposal_rejected`) in `status.json`; the launcher publishes a cache
  entry only after a fully successful build.
- `segarecomp aot-policy-identity --platform master-system --aot-policy <p>` prints the ROM-independent identity (model, rank policy, schema and
  admission contract bound in); exit status 3 means the producer is unavailable, so a launcher must not offer it.
- `status.json` `aot_policy` for an applied Optimized build: `effective` `ml_region_z80`, `model_family` `sms_z80_d512hp`, `model` (ensemble digest),
  `artifact_sha256`, `architecture`, `rank_policy`, `schema`, `cells`, `ml_selected`, `image_offsets`, `admitted_offsets`, `selected_ppm`,
  `admitted_ppm`, `inference_ms`, `plan_sha256` (identity of the admitted set), `validator`, `policy_identity`, `identity`. No address or byte.
- Runtime: a generated Master System program that stops on the omitted start prints `z80_outcome unadmitted_pc pc <hex4> identity <n>` and exits with
  the **dedicated status 7** (`SMS_EXIT_Z80_UNADMITTED_PC`; every other stop keeps its status: 0 ok, 2 cycle budget, 3 other Z80 outcome, 4 SMS
  error, 5 viewer host error, 64 usage). The launcher recognizes exactly this status (no log scanning).
- Launcher: the Mode control offers Optimized for Master System only when the product states it is available. An Optimized run that exits with
  status 7 shows a notice, switches to Compatibility, reuses a matching Compatibility build or builds one, and restarts the program **from reset**,
  once per Play (a failing Compatibility build is reported, not retried). No machine state is carried over.
  The Running label and the headless `run_mode` report the **effective** program (an Optimized request that fell back at build time runs and is labelled
  Compatibility; `run_requested_mode` keeps the request); the historical `run=exited code=N` line is unchanged.

## Alias and RAM-thunk builds (ADR 0111)

Games that copy cartridge code into work RAM add ADR 0049 alias identities (and possibly observed RAM `JMP` thunks) during the build-time preparation. Under
Optimized, a copy-alias identity is admitted when the immutable cartridge source it is byte-identical to is inside the proposed executable region (no region
is widened; every alias identity remains a mandatory root of the unchanged validator). A build that also needs RAM thunks still falls back to the broad program
(`prune_rejected`). Compatibility, the default policy and the experimental status of `--aot-model gen2` are unchanged. ML selection inside a materialized image
is not supported (ADR 0113: the validator admits no proper subset of an alias image).
