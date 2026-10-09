# AOT optimization policy (Compatibility / Optimized)

`segarecomp build --aot-policy <compatibility|optimized>` selects how much of the ROM is translated into the generated native program
(ADR 0096). The user-facing concept is a policy, not a model: there is no model path, threshold or coefficient option.

| policy | behaviour | default |
| --- | --- | --- |
| `compatibility` | the broad immutable-ROM universe (every decodable instruction start is translated) | yes (through SEG-047) |
| `optimized` | Genesis M68K selective admission: a frozen, statically embedded region model proposes an executable region, structural pruning derives the admitted set, and the unchanged admission validator decides; any failure builds the broad program instead | no |

Optimized never weakens correctness: the generated program is still static native code (no interpreter, JIT or runtime decoder, no ML at
run time); a region that misses code makes the program stop fail-closed (typed stop), never execute wrongly. An explicit
`--admission-plan` (an exact source/recomp-map plan) outranks Optimized. Master System images have no Optimized producer: the request is
reported as a fallback (`platform_not_applicable`).

## Machine-readable status

Every successful build writes `status.json` with an always-present `aot_policy` member (no parsing of human text):

```json
"aot_policy": {
  "requested": "optimized",            // compatibility | optimized
  "effective": "ml_region",            // broad | ml_region | admission_plan
  "fallback": false,                   // true when Optimized fell back to broad
  "reason": "none",                    // none | exact_plan_precedence | platform_not_applicable | model_identity | rom_size |
                                       // empty_proposal | prune_rejected | validator_rejected | no_analysis | no_report
  "model": "seg046-features-v1", "schema_sha256": "d2e7c829…",     // only when effective = ml_region
  "windows": 2048, "ml_selected": 244, "seed_windows": 14,          // candidate metrics (aggregates only)
  "universe": 498276, "k0": 60459, "k": 56980, "pruned": 3479, "rounds": 77,
  "k_sha256": "…", "validator": "accepted",
  "identity": "<sha256>"               // stable cache identity: image + requested + effective + model/schema + admitted-set digest
}
```
`identity` is a pure function of the inputs: Compatibility and Optimized builds of the same image never share it, and a fallback build
never aliases a successful Optimized build. No address, byte or per-window datum is reported.
