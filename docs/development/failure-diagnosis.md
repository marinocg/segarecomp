# Failure diagnosis workflow (SEG-020, ADR-0042)

One developer path answers "where did generated-native execution first go wrong, and which layer owns
it?" It extends `tools/genesis_startup_bridge.py --diagnose-frontier`; it adds no new tool, trace store,
replay engine or runtime flag.

## Layers combined

| Question | Source | Where |
| --- | --- | --- |
| Stop category | sanitized bridge report (`stop_class`, `diagnostic_category`) | bridge |
| Guest provenance, access, closure metrics | `EPHEMERAL_FRONTIER` | bridge |
| Recent execution history | ephemeral channel (`--provenance-diagnostics` binaries) | bridge |
| First CPU divergence vs pinned Musashi | `tools/m68k_first_divergence.py compare` | T005 |
| CPU vs Genesis device divergence | `tools/genesis_device_divergence.py` | T006 |
| LLDB/GDB stop inspection (private only) | `tools/genesis_frontier_debug.py` | unchanged |

## Command

1. Produce a divergence report from generated and expected boundary streams (JSON on stdout):

   ```sh
   python3 tools/genesis_device_divergence.py --generated gen.txt --expected ref.txt \
       --limit 100000 --initial-pc 0x200 > divergence.json
   ```

2. Fold it into the bridge run:

   ```sh
   python3 tools/genesis_startup_bridge.py ... --diagnose-frontier --divergence-report divergence.json
   ```

   `--divergence-report` requires `--diagnose-frontier`. stderr then additionally carries:

   - `EPHEMERAL_DIAGNOSIS {...}`: stop + frontier + full divergence report. **Private session only.**
   - `DIAGNOSIS_CLASSES {...}`: the only line that may be copied into durable evidence, PRs or CI output.

A missing or malformed divergence file yields `"divergence": null`; it never fails a run. Output is
deterministic (sorted keys) for identical inputs.

## Privacy

`DIAGNOSIS_CLASSES` keeps stop class, diagnostic category, divergence result/domain/classification,
boundary ordinals and *field classes* (`d3`, `sr`, `pc`, `effect:write/w2`, `event:write@vdp/w2`,
`state:vdp_vram`). It drops addresses, values, PCs, image identity, bytes and generated/expected pairs.
Unrecognised field names collapse to `other`. Everything else stays in the ephemeral channel per
AGENTS.md and `docs/testing/commercial-games.md`. Diagnostics-off generation is byte-identical to before
(ADR-0042 section 9); nothing here affects generated code.

Evidence: `tests/genesis_failure_diagnosis_workflow_test.py`.
