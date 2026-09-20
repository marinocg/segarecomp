# Genesis startup shared-route ownership contract (SEG-005-T004)

## Purpose and boundary

This is a migration contract, not an implementation, fixture, hardware, or
oracle result.  It consumes the bounded startup policy in
[SEG-005-T001](../references/genesis-rom-startup-contract.md), the retained
SEG-003 policy in [the direct-flow contract](../references/direct-flow-contract.md),
and the shared-route boundary in [the MC68000 pipeline migration
contract](m68k-pipeline-migration-contract.md).  The existing Genesis ingress
continues to validate and report exactly as it does now.  Its accepted reset
result is only a verified ingress handoff; it is neither a SEG-003
`reset_seed`, a direct-flow entry, nor proof of boot, mapping, or execution.

The one shared owner after ingress is the **shared MC68000 route** represented
by `m68k_pipeline.hpp`: one typed program/provenance analysis artifact feeds
one static discovery artifact and one structured-C lowering input.  The names
below describe ownership, not required new public C++ names.

Compatibility is cumulative: the shared route must preserve every inherited
SEG-002 fixture/vector, every SEG-003 fixture/vector, and every SEG-005-T001
accepted and negative recipe.  Their public command interfaces and report
schemas remain distinct; shared ownership does not merge, replace, or
reinterpret those interfaces.

| Fact | Exactly one shared owner | Retained requirement |
| --- | --- | --- |
| Verified 2-/6-byte spans and provenance | decoded-operation record in the shared program artifact | Address, image offset, raw verified bytes, CPU, and length remain independent; primary/extension truncation retains only the provenance safely verified.  This retains SEG-003 source-before-read and earliest-provenance diagnostics and SEG-005-T001 decode order. |
| Typed startup operations | lifted-operation record in that artifact | Only T001's selected forms are represented; rejected primaries/forms never become operations.  This retains SEG-003 fail-closed selection and T001 primary classification/extension order. |
| Direct call/return identity | shared discovery call record and its direct-call/return edges | `CallId` is caller provenance, verified continuation, and callee; an RTS matches that identity, never a numeric stack value.  Edges retain terminating provenance and deterministic target validation/order. |
| A7, RAM, and ordered bus observations | shared lowered-execution record | Boundaries, changed RAM ordering, and monotonically ordered bus records are lowered from the shared operations and edges.  T001's operand/stack validation order and no-mutation failures remain authoritative. |
| Structured-C lowering | shared structured-C backend input | It consumes only the shared lifted/discovery records, emits deterministic C11, and has no source image or target decoder.  SEG-003 block/edge/symbol order is retained. |

`M68kProgramAddress`, image offset, byte length, and raw bytes are never
reconstructed from a host pointer or from one another.  Mapping-claim ingestion
order, FIFO successor order, completed-block budget semantics, block identities,
and null/unavailable diagnostic fields remain those of SEG-003.  T001 adds its
own scoped A7/RAM/bus facts without changing those direct-flow facts.

Selected startup A7/RAM behavior is production lowering semantics.  Ordered bus
and boundary records are deterministic startup validation/report instrumentation;
where those observations are unrequested, they do not require always-on tracing
in a future Mega Drive runtime ABI.  This clarification does not alter startup
reporting.

## Ordered seams

| Task | Input → output seam | Must not do |
| --- | --- | --- |
| T005 | verified ingress/program mapping → shared verified spans and typed startup operations | recover calls, execute A7/RAM/bus state, lower C, or migrate ingress |
| T006 | T005 operations → shared `CallId`, direct-call/return edges, and static frames | infer a return from a stack number, recover general calls, or execute stack state |
| T007 | T005/T006 records → shared lowered A7/RAM/bus/boundary records and structured C | change ingress ownership or dynamically fetch/decode target bytes |
| T008 | preserved ingress plus T005--T007 output → the sole startup route | retain a startup-only projection, executor, emitter, or compatibility bypass |

The exact removal condition is T008 completion only when architecture inspection
and regressions demonstrate that every accepted startup translation traverses,
after the unchanged ingress, the one shared decode/lift, discovery, and
structured-C lowering route.  Every startup failure must reach its applicable
shared validation, decode, discovery, or lowering stage, or produce a
source-provenanced shared fail-closed rejection record; it must preserve the
no-successful-artifact property and never use a startup-only bypass.  At that
point the startup-only projection, executor, emitter, and compatibility route
are deleted.  After T008 none may produce a successful artifact, and generated
or runtime code must not embed, fetch, or decode target instruction bytes.
[ADR 0003](../decisions/0003-genesis-startup-shared-route-boundary.md) records the precise
meaning of "no startup-only projection, executor, or emitter" applied to reach this condition.

**2026-08-08 (SEG-005-T008):** An initial inspection-only pass concluded the removal condition was
already met because the startup command reached the shared entry points
(`analyze_m68k_frontend`/`execute_m68k_frontend_startup`/`emit_m68k_frontend_c`) and no second,
legacy file existed. Independent adversarial review correctly rejected that conclusion: reaching a
shared *entry point* is not the same as sharing *ownership* of what each selected operation means,
and `execute_m68k_frontend_startup`/the `genesis_rom_startup` branch of `emit_m68k_frontend_c` were
still self-contained interpreters/emitters that independently implemented register/CCR effects,
absolute-memory addressing, and call/return push/pop semantics, rather than consuming them from a
shared definition. T008 then implemented the missing shared ownership (see ADR 0003 for the full
before/after):

- Deleted `M68kStartupOperation` (a struct that duplicated `decoded`/`ir`/`raw_bytes`/`extension`
  into a second per-operation record); `analyze_startup_profile`, `execute_m68k_frontend_startup`,
  and `emit_m68k_frontend_c` now consume `FrontendAnalysis::decoded`/`ir` directly by index.
- Renamed `StartupCall`/`StartupEdge`/`StartupEdgeKind`/`StartupBlock`/`StartupStaticFrame` to
  `M68kStaticCall`/`M68kStaticEdge`/`M68kStaticEdgeKind`/`M68kStaticBlock`/`M68kStaticFrame` and
  the `FrontendAnalysis` fields that hold them from `startup_*` to `static_*`: this is the
  profile-neutral shared static-program representation T004 named, not a startup-specific type.
- `execute_m68k_frontend_startup`'s JSR handling now reads the call/continuation identity directly
  from `analysis.static_frames.front().call` (T006's single discovered fact) instead of
  recomputing `source_address + length` independently; `emit_m68k_frontend_c` sources the same
  field for its generated continuation literal.
- Added `m68k_startup_absolute_operand_alignment`/`m68k_startup_ram_operand_in_range`/
  `m68k_startup_ram_offset` as the sole shared implementation of the MOVE.L Abs.L operand
  24-bit/even/in-range policy and RAM-offset projection, used by both the static decode-time
  validation and the defensive runtime re-validation, replacing two independently written copies.
- Extended `emit_m68k_operation_c` (an optional `M68kMemoryEmissionContext` parameter) to own the
  full C lowering of `write_d0_absolute_long`/`read_absolute_long_d1`/`call_absolute_long`/
  `return_from_subroutine`, not only their CCR update; `emit_m68k_frontend_c`'s startup branch is
  now a thin loop over `analysis.ir` calling this one function, plus RAM/frame-array harness
  declarations and the fixed report footer.

What remains profile-specific — `analyze_startup_profile`'s fixed graph traversal,
`execute_m68k_frontend_startup`'s host-buffer RAM/stack simulation, and the
`StartupExecution`/`StartupFailure`/`StartupBusRecord`/`StartupBoundary` report schemas — has no
SEG-003 equivalent to share and is reporting/instrumentation, not semantic ownership, per ADR 0003.
A repository-wide search (`grep -rln "startup" -i` over `src/`, `include/`, `tests/`, `tools/`,
excluding `build/`) still returns only the shared pipeline files, their tests, and the existing
SEG-005-T001/T002 test driver/fixture — no second, legacy, or parallel file exists.
`tests/m68k_pipeline_test.cpp` asserts: (1) startup discovery never populates
`FrontendAnalysis::direct_flow`/`units` (no parallel direct-flow CFG discovery runs); and (2) the
executed call record and the emitted continuation literal both equal the one identity
`discover_m68k_static_call_return` established, proving execution and C lowering share that single
owner rather than each recomputing it.

**2026-08-09 (SEG-005-T008, round 4):** The repository owner reviewed the 2026-08-08 state again and
found the removal condition still not fully met: `execute_m68k_frontend_startup` still directly
decided each selected operation's *execution-semantic* meaning inline (MOVEQ sign-extension/D0
write, absolute-long load/store address+register, JSR/RTS stack push/pop), including one concrete
cross-file duplication with SEG-003's `execute_op` for MOVEQ's register write, and
`emit_m68k_frontend_c` still special-cased MOVEQ's PC-advance outside `emit_m68k_operation_c`. This
round closed both gaps (see ADR 0003's 2026-08-09 revision for the complete before/after):

- Added `M68kOperationEffect`/`m68k_operation_effect` as the sole decision of what every selected
  `M68kIrKind` means: MOVEQ's register write, the absolute-long forms' memory register/address, the
  call/return control-transfer kind, and each non-control form's PC advance. `execute_op` (SEG-003)
  and `execute_m68k_frontend_startup` (SEG-005) both consume it instead of independently redeciding
  it; address-policy validation, backing storage, CCR application, and call/return stack values
  sourced from discovered call identity remain the caller's job.
- Folded MOVEQ's `"pc += 2U;\n"` into `emit_m68k_operation_c`'s shared `write_moveq` case (gated on
  the existing optional `M68kMemoryEmissionContext`, like the other four kinds), removing
  `emit_m68k_frontend_c`'s separate MOVEQ special case.
- `execute_m68k_frontend_startup` now fails closed (`category = "invalid_startup_analysis"`) before
  any bus/RAM/A7/PC/frame mutation when `FrontendAnalysis::decoded`/`::ir` disagree in length or
  per-element identity (typed source address, image offset, verified length, decoded-kind-to-lifted-
  kind mapping, and operand/extension).

An independent adversarial-validator pass re-derived all of this from source (not from this
document's prose), confirmed each claim against the actual diff, confirmed the RTS pop/validate/
dispatch sequence correctly remains in `execute_m68k_frontend_startup` (it depends on runtime
A7/RAM/frame state the effect layer cannot know), confirmed SUBQ and the two branch forms were
correctly left untouched, and reran the full gate from a clean rebuild. **Verdict: PASS**, with one
narrow hardening gap found and then closed in the same round (the decoded/lifted consistency check
initially did not compare `operand`/`extension`, which a follow-up commit and regression fixed).

**2026-08-09 (SEG-005-T008, round 5):** The repository owner reviewed the round-4 state again and
found round 4's `M68kOperationEffect` reduced JSR/RTS to a bare `control` marker (`call`/`ret`) with
no stack or PC-transition facts beyond JSR's target address, so `execute_m68k_frontend_startup` still
independently defined "JSR pushes 4 bytes and jumps to the direct target" and "RTS pops 4 bytes and
jumps to the observed stack value" inline — the RTS branch even computed its shared effect and
discarded it with `[[maybe_unused]]`, so the effect materially controlled nothing there. This round
closed that gap (see ADR 0003's second 2026-08-09 revision for the complete before/after):

- Replaced `M68kControlEffectKind`/`control`/`call_target` with typed `M68kStackEffectKind`
  (`push_static_continuation`/`pop_static_return`) plus `stack_width`, and `M68kPcEffectKind`
  (`advance`/`direct_target`/`observed_stack_return`) plus `direct_target`, on
  `M68kOperationEffect`. `m68k_operation_effect` is now the sole decision of every selected
  operation's stack push/pop *intent and width* and PC-transition *kind*, not only its
  register/memory facts. The pushed continuation *value* stays sourced exclusively from
  `discover_m68k_static_call_return` (T006, via `analysis.static_frames`); the observed return
  *value* stays a real host-RAM read the adapter performs and validates; whether/how a requested
  effect succeeds against RAM/A7 bounds stays the adapter's job — the effect layer still has no
  memory, RAM-window, or call-identity model of its own.
- `execute_m68k_frontend_startup`'s JSR/RTS branches now apply `effect.stack`/`effect.stack_width`/
  `effect.pc`/`effect.direct_target` instead of hardcoded `4U` literals or a discarded effect, with a
  structural fail-closed guard on each branch (not reachable via any current fixture) so a future
  inconsistency between `ir.kind` and `m68k_operation_effect` cannot silently execute against the
  wrong shape. Every existing alignment/range/overflow check, failure category, failure field, bus
  record, and mutation order is unchanged; every existing JSR/RTS negative regression (A7=0/1/2
  alignment/range precedence, overflow, `return_context_missing`, `return_target_mismatch`,
  successful return) produces the exact same result as before.
- Added a direct `m68k_operation_effect` regression for JSR and RTS asserting the exact stack/pc/
  direct_target fields and that no continuation or return value is guessed by the effect.

## Falsifiable preservation matrix

All rows use only existing project-authored, hash-checked fixture materials;
no commercial or generated fixture material is added.  “Same” means the named
category, ordered records, retained provenance, and no-successful-artifact
property remain observable under the owning seam.

| Source fixture class | Regression check | Required result |
| --- | --- | --- |
| SEG-002 accepted records in `tests/fixtures/moveq-fixtures.json` and every D0--D7 vector in `tests/fixtures/moveq-oracle-vectors.json` | Run every accepted fixture/vector through the shared decode/lift/backend ownership, retaining the standalone `MOVEQ` command/report schema. | Every D0--D7 destination and positive, zero, and negative immediate retain signed 8-to-32 extension, selected-destination replacement, unaffected D registers, exact CCR/SR and PC, typed CPU/address/image-offset/raw-byte/length provenance, deterministic generated C, existing budget/stop behavior, and byte-identical repeated output.  This is the complete standalone SEG-002 profile, not the SEG-003 `MOVEQ D0` subset. |
| SEG-002 negative records `odd`, `truncated`, `illegal`, and `unsupported` in `tests/fixtures/moveq-fixtures.json` | Run each established rejection through shared ownership without changing the standalone command/report schema. | Preserve precedence and exact stable category, available byte count, requested length where applicable, instruction-length nullability, source address, image offset, and raw verified provenance where available; do not invent bytes or provenance and produce no successful artifact.  `illegal` remains the source-defined `ILLEGAL` category, not generic unsupported. |
| SEG-003 `direct-loop-v1`: taken/fallthrough BNE vectors | Run both existing vectors through the shared direct-flow ownership. | Same ordered blocks (`m68k_block_%08X`), edges, boundaries, full state, and budget stop. |
| SEG-003 `truncated-bcc-word` | Supply `6600` with no extension. | `truncated_instruction`, primary bytes/provenance, requested length 4; no guessed operation. |
| SEG-003 `odd-target`, `unmapped-target` | Discover each direct BRA target. | Same target diagnostic order and edge provenance; no target decode/block. |
| SEG-003 `conflicting-map` | Preserve the two claims in supplied order. | `conflicting_address_mapping` with both ordered claims before target decode. |
| SEG-003 `mid-instruction-target` | Target supplied completed structural interval. | `mid_instruction_direct_target` with structural provenance; no new BRA.W support. |
| SEG-003 `unsupported-nop`, `reached-unresolved-edge` | Reach each retained negative. | Same fail-closed category/provenance; no no-op, dispatch, or dynamic decode. |
| SEG-003 `subq-l-d0-zero` | Execute its one-block `SUBQ.L #1,D0` fallthrough boundary. | Same accepted result: D0 `FFFFFFFF`, PC `00000102`, SR `A5F9`, and fallthrough edge. |
| SEG-003 `subq-l-d0-one` | Execute its one-block `SUBQ.L #1,D0` fallthrough boundary. | Same accepted result: D0 `00000000`, PC `00000102`, SR `A5E4`, and fallthrough edge. |
| SEG-003 `subq-l-d0-signed-minimum` | Execute its one-block `SUBQ.L #1,D0` fallthrough boundary. | Same accepted result: D0 `7FFFFFFF`, PC `00000102`, SR `A5E2`, and fallthrough edge. |
| SEG-003 `subq-l-d0-signed-minimum-plus-one` | Execute its one-block `SUBQ.L #1,D0` fallthrough boundary. | Same accepted result: D0 `80000000`, PC `00000102`, SR `A5E8`, and fallthrough edge. |
| T001 accepted `recognized-reset-jsr-rts-v2` | Use the existing hash-checked recipe after preserved ingress. | Same 2-/6-byte provenance, `CallId`, A7/RAM/bus/boundary order, C11 artifact, and budget stop. |
| T001 `primary-truncated`; `extension-truncated-store`; `extension-truncated-jsr` | Cut the listed primary or extension bytes. | Same `truncated_instruction` precedence; primary-only provenance only for extension truncation; no unavailable read or invented bytes. |
| T001 `illegal-primary`; `unsupported-primary`; `unsupported-form` | Substitute the listed complete primary. | Same `illegal_instruction`, `valid_but_unsupported_instruction`, or `unsupported_instruction_form`; no extension/target guess or bus record. |
| T001 `rom-write` | Store to the listed ROM address. | `rom_write_prohibited` after instruction record only; no data write. |
| T001 `odd-data`; `unmapped-data` | Use the listed data effective address. | Same `odd_effective_address` or `unmapped_data_access`; no data/device access. |
| T001 `invalid-stack-alignment`; `invalid-stack-range` | Use each listed initial A7. | Same stack diagnostic after instruction record only; no stack bus record, frame, state mutation, or dispatch. |
| T001 `return-context-missing`; `return-target-mismatch` | Supply the listed isolated RTS context. | Valid stack read first, then the distinct return diagnostic; no PC/A7/frame mutation, return edge, or dynamic target dispatch. |

The ignored pinned Musashi adapter may later supply bounded CPU/RAM comparison
only; it supplies no provenance, bus, CFG, Genesis, or ownership oracle claim
for this task.  This documentation delivery records the successor gate only:

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev --output-on-failure
```

It makes no implementation or oracle-evidence claim.
