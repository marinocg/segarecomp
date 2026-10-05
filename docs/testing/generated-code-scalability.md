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

## Multi-translation-unit output (SEG-022-T010)

Since SEG-022-T003/T008 the emitter writes a set of translation units (`bridge_generated.units`
manifest) plus `bridge_generated.h`, with non-static functions and grouped `genesis_aot_owner_*`
functions (per-owner `switch (runtime->pc)` entry dispatch and `genesis_aot_entry_*:` labels).
`measure` now attributes every unit plus the header as one ordered stream (the header counts as
`runtime_prelude_glue`; the route record/mapping tables count as `provenance` / `mapping_metadata`).
`aot_function` counts grouped owners and `aot_entry_label` counts entries; the per-owner entry switch
is the new `owner_entry_dispatch` category (in T001 terms it was part of AOT boilerplate). The
partition stays exact (residual 0). `--jobs N` compiles independent TUs concurrently.

## Z80 / Master System (SEG-033)

The Z80 AOT route has its own production-build measurements and gates: see `z80-build-performance.md` and ADR 0071.

## Hybrid admission candidate (SEG-031, ADR 0080)

Broad immutable-ROM AOT stays the production default. The explicit candidate filters the broad identities to a hybrid admission
proven by the report-only planner (`segarecomp-genesis-analysis-report --hybrid-plan <plan>`), consumed by
`emit-general-startup-bridge-c --immutable-rom-aot-admission <plan>`, `segarecomp build --admission-plan <plan>` or
`genesis_startup_bridge.py --admission-plan <plan>`. A plan holds exact addresses: keep it in an ignored location for a commercial input.

```sh
python3 tools/genesis_hybrid_admission_compare.py --segarecomp <cli> --driver <analysis-report> --rom games/<rom> \
  --work build/<ignored>/cmp --reference-segarecomp <pre-change cli> --coverage-dir <private complete oracle> --run-frames 3000
```

It reports, as sanitized JSON: the plan aggregate (`broad_u`, `precise_d`, fallback islands and admitted identities, hybrid total,
ratio, whole-image triggers by generic reason) and its cost; generated C bytes/files broad vs planned and whether the trees are
identical (and identical to the reference CLI's broad tree); production build wall/CPU time, peak child RSS and executable size, and
whether the executables are identical; fixed-frame runtime and whether final-state, frame-stream and coverage digests agree; and the
oracle falsification (observed broad identities outside the admission, which must be 0).

## Entry-representation attribution (SEG-036-T001)

`attribute` / `measure` also report `source.entry_representation`: an exact split of the AOT per-entry
generated text. Helper-backed entries (`genesis_aot_entry_<PC>: { return genesis_aot_shared_N(runtime[, <provenance literal>][, <own PC>]); }`)
are counted as `helper_exact` / `helper_own_pc` and their bytes split into `label`, `call_syntax`,
`provenance_argument` and `own_pc_argument` (the split must sum exactly to the entry-line bytes).
Entries with an inline body are `inline_helper_eligible` or `inline_helper_ineligible` (contains
`case`/`static`/`switch (`). `case_dispatch_lines` counts the owner `switch` rows.
`routing_only_bytes` = case rows + labels + call syntax + own-PC arguments, i.e. the C that only routes a known PC to an already
selected helper; `routing_plus_provenance_argument_bytes` adds the repeated provenance compound literals.
Only counts and byte totals are reported. The existing category partition is unchanged.
