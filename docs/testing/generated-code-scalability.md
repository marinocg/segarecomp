# Generated-code scalability measurement (SEG-022-T001)

`tools/generated_code_scalability_report.py` measures size/time cost of the generated-native
startup-bridge build for one route (external hints, or no external hints) and attributes every byte
of the generated C to a category. It is instrumentation only: it runs the unchanged
`emit-general-startup-bridge-c ... --immutable-rom-aot` command and post-processes the output, so
generated guest semantics and address sets are untouched. Reported figures are baselines, never
correctness or test gates.

```sh
python3 tools/generated_code_scalability_report.py measure \
  --segarecomp build/dev/apps/segarecomp/segarecomp --rom games/<rom>.md \
  [--external-hints .tools/analysis-hints/<sha>.json] \   # omit => no-external-hints route
  --out-dir .cache/<unique-run> --report .cache/<unique-run>.json
python3 tools/generated_code_scalability_report.py attribute <generated.c>
```

`measure` reports: generation wall time and peak RSS; source bytes/lines; the emitter's aggregate
inventory counts (aligned candidates, admitted AOT, emitted blocks, final compiled addresses);
function counts; compile wall time and peak RSS (generated and runtime translation units); link
time; object and executable bytes; toolchain and machine class.

Attribution partitions each line by (owning region, line class). Categories: ordinary block
bodies/boilerplate, immutable-ROM AOT bodies, AOT boilerplate (entry/retirement glue), dispatch
structures, compiled-entry tables, target-membership checks, provenance, mapping metadata,
frontier code, owned/resolved ROM literals, runtime/prelude glue. Every byte lands in exactly one
category, so the documented residual is `unattributed_residual` (expected 0). Classification is
line-pattern based and follows the emitter's current output shape; update the patterns if the
emitter format changes (the unit test fails if the partition stops being exact).

Output contains only aggregate counts/bytes/times; raw generated C stays in the ignored `--out-dir`.

Site-local `m68k_indirect_targets_*[]` arrays (repeated computed-control membership data, single- or
multi-line, in any owner) are attributed to `target_membership_structures`, never to instruction
bodies; the report also gives array count, total elements, bytes and the largest array.

Exact-set baselines are stored as count + SHA-256 of the sorted canonical `%08x\n` address list
(addresses themselves are never kept): the admitted immutable-ROM AOT set (from the opt-in,
measurement-only `emit-general-startup-bridge-c --immutable-aot-address-report <path>` sink, deleted
by the tool after hashing), and the final compiled-entry address set / its AOT- and block-owned parts
(from the generated compact `genesis_compiled_entry_addresses[]` / `genesis_compiled_entry_owner_ids[]` / `genesis_compiled_owners[]`
tables, the exact tables the generated dispatcher looks up). Later SEG-022 tasks must reproduce these digests.
