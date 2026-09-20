# MC68000 pipeline migration contract (SEG-012-T001)

## Purpose and inheritance

This contract is the migration baseline for SEG-012.  It permits a later
implementation to put the already delivered, bounded MC68000 paths behind one
reusable frontend, decode, lift, discovery, and structured-C emission path. It
does **not** select additional behavior.  The implementation task is
SEG-012-T002; this document supplies no implementation or fixture change.

The following accepted contracts are inherited, not redefined:

| Inherited contract | Relevant rule retained by this migration |
| --- | --- |
| [`docs/architecture/pipeline.md`](pipeline.md) | Typed addresses, separately retained image offsets and byte lengths, deterministic ordering, and static (not runtime-decoded) execution are pipeline contracts. |
| [`docs/decisions/0001-vertical-slices-and-explicit-ir.md`](../decisions/0001-vertical-slices-and-explicit-ir.md) and [`0002-static-translation-and-fail-closed-execution.md`](../decisions/0002-static-translation-and-fail-closed-execution.md) | The typed IR boundary is explicit; reached unsupported behavior fails closed with source provenance; generated code must not fetch or decode target opcodes. |
| [`docs/references/moveq-contract.md`](../references/moveq-contract.md) (SEG-002) | `MOVEQ` decode precedence, `M68kProgramAddress`, image-offset and byte provenance, `IrMoveq32`, one-instruction state/CCR behavior, and its synthetic vectors remain authoritative. |
| [`docs/references/direct-flow-contract.md`](../references/direct-flow-contract.md) (SEG-003) | The selected register-only direct-flow subset, block/edge provenance, deterministic FIFO discovery and block names, block budget, diagnostics, synthetic loop vectors, and boundary oracle remain authoritative. |

“Shared” below means one implementation route for these inherited contracts;
it does not mean that their presently separate public commands, reports, or
fixture schemas become one interface.

## Evidence inventory and compatibility boundary

The migration must account for each row without changing its scope.  SEG-002
and SEG-003 validation records provide the completed behavioral evidence:
the SEG-002-T003 backlog record
the SEG-003-T003 backlog record.

| Area | Currently accepted behavior | Excluded behavior that must remain excluded | Evidence and migration disposition |
| --- | --- | --- | --- |
| `MOVEQ` | SEG-002 accepts the documented two-byte `MOVEQ #s8,Dn` form, preserves typed source/address/offset/bytes/length, lifts it to `IrMoveq32`, and observes full D-register, PC, SR, and budget-stop state. | Odd/truncated input, `ILLEGAL`, and other primary words reject in the inherited precedence. Memory, devices, exceptions, timing, code writes, and runtime decoding are not selected. | SEG-002 contract §§ “Project decode, provenance, and IR policy”, “Strict result categories and precedence”, and “Independent execution oracle”; SEG-002-T003 records eight project-authored differential vectors and strict-C11/determinism evidence. Migrate this accepted behavior only. |
| Direct flow | SEG-003 accepts only its loop recipe: inherited `MOVEQ #s8,D0`, `SUBQ.L #1,D0`, nonzero-displacement `BNE.S`, and nonzero-displacement `BRA.S`; it observes ordered blocks, direct edges, full boundary state, and a completed-block budget. | Other `MOVEQ` destinations in this direct-flow slice; other `SUBQ` forms; other conditions; word branch displacements; calls, returns, indirect transfers, memory operands, exceptions, timing, peripherals, mappers, and code writes remain fail-closed or outside the input contract. | SEG-003 contract §§ “Selected hardware subset”, “Support matrix and exclusions”, “Typed discovery and block-boundary contract”, and “Fail-closed diagnostics”; SEG-003-T003 records both taken and fallthrough `BNE` evidence. Migrate these four selected forms only. |
| Startup and mapping | The repository separately exposes bounded Genesis reset-image validation through `analyze` and `analyze_genesis_reset_image`; its accepted outcome is a finite, identity, non-mirrored raw cartridge mapping and an even, in-image reset PC. | Startup mapping is **not** an accepted SEG-002 or SEG-003 frontend/direct-flow behavior. SEG-003 expressly excludes reset-vector mapping, console mapping, and mapper behavior. No startup instruction execution, stack behavior, call/return path, or console boot sequence is imported into this migration. | [`docs/references/genesis-reset-image-contract.md`](../references/genesis-reset-image-contract.md) is the separate SEG-001 source for the bounded validator. For SEG-012-T002, retain this validated startup ingress and its existing `analyze` contract; only after that contract has produced its existing accepted result may an adapter feed its already verified typed region, entry address/image offset, and source provenance into the common typed program/provenance pipeline. That handoff neither selects a decode/execution form nor makes the startup result a direct-flow entry. A separately cited mapping and entry-integration contract with evidence is required before either claim. |

In particular, a typed `reset_seed` in the SEG-003 discovery contract is an
ingestion-supplied seed. It is not evidence for Genesis reset-vector loading or
startup mapping.

The startup handoff is therefore an ingress-preservation boundary, not reset
emulation: preserve the existing validator's input checks, acceptance/rejection
class, and report before constructing common typed program/provenance records.
It must not reinterpret `reset_seed` as a raw reset mapping or use the accepted
startup entry to begin direct-flow discovery without the separately evidenced
mapping and entry integration above.

## Current types versus planned unified route

The following repository interfaces exist now and are evidence to preserve,
not evidence that a unified frontend already exists.

| Status | Current path and types | Contractual migration requirement |
| --- | --- | --- |
| Existing shared domain/provenance types | [`libs/legacy_compat/include/segarecomp/moveq.hpp`](../../libs/legacy_compat/include/segarecomp/moveq.hpp): `TargetAddressSpace`, `M68kProgramAddress`, `MoveqImageOffset`, `ByteLength`, `CpuVariant`, `DecodeSource`, and `InstructionProvenance`. [`include/segarecomp/direct_flow.hpp`](../../include/segarecomp/direct_flow.hpp) reuses these types. | Preserve values independently: an address is `{m68k_program, value}`, and image offset is never reconstructed from an address or host pointer. Retain raw bytes and verified length on decoded instructions, IR operations, blocks, and edges as required by the inherited contracts. |
| Existing, slice-specific decode/lift/emission | `moveq.hpp` defines `MoveqInstruction`, `IrMoveq32`, `decode_moveq`, `lift_moveq`, and `emit_moveq_c`. `direct_flow.hpp` separately defines `DirectInstruction`, `DirectFlowIrOperation`, `discover_direct_flow`, and `emit_direct_flow_c`. | These names and separate representations show the starting state. They must not be treated as a second semantic path after consolidation. The unified route must represent the same selected instruction, state, provenance, block, edge, and rejection facts before any structured-C emission. |
| Existing direct-flow analysis records | `direct_flow.hpp` defines `MappingClaim`, `BlockId`, `BlockProvenance`, `DirectEdge`, `BlockBoundary`, `DirectFlowProgram`, and `RejectedDirectFlow`. | Preserve SEG-003 ordering and source provenance, including mapping-claim ingestion order. No inferred mapping, guessed target, or empty successful block is permitted. |
| Existing public entry points | [`apps/segarecomp/main.cpp`](../../apps/segarecomp/main.cpp) currently supplies `emit-moveq-c`, `emit-direct-flow-c`, and the separate `analyze` command. | Preserve each command's established input validation, success/rejection class, and report schema. Their output formats are not a proposed common serialization. After preserving `analyze`'s existing contract, its accepted startup ingress may feed common typed program/provenance components, but that feed does not add Genesis mapping, boot, decode, or direct-flow-entry support. |
| Planned, not present | One reusable MC68000 frontend accepts a typed program/mapping and selected entry; one decode/lift route produces typed selected operations or a rejection; one discovery route produces blocks/edges; one structured-C backend consumes lifted/static analysis records. | SEG-012-T002 may introduce the smallest interfaces needed to make that single route real. The preserved startup ingress may supply common typed program/provenance records, but its accepted reset result is not a selected frontend or direct-flow entry without separately evidenced mapping and entry integration. The route must retain CPU-specific operation semantics until reuse is proved, per ADR 0002, and must not introduce a parallel compatibility path, runtime target-byte fetch/decode, or a generic framework. |

The current `emit_c_manifest` declaration in
[`libs/codegen/c11/include/segarecomp/c_emitter.hpp`](../../libs/codegen/c11/include/segarecomp/c_emitter.hpp) is a
header-manifest prototype, as `pipeline.md` says. It is not evidence that the
manifest command is an instruction emitter or that it belongs in this
migration's semantic equivalence set.

## Observable deterministic equivalence

For a given inherited fixture and identical command/options, the consolidated
implementation is equivalent when all applicable observables below match the
fixture/contract expectation and repeated runs are byte-identical in their
respective artifact category.

| Observable | Required comparison |
| --- | --- |
| Accepted decode/lift facts | Selected outcome/form, typed CPU/address/offset, raw bytes, verified length, immediate/operand, and lifted operation preserve the inherited provenance. |
| Direct discovery | Ordered blocks, stable `m68k_block_%08X` identities, ordered instruction provenance, direct edge kind/condition/target, reset-seed record, and block-boundary ordering match SEG-003. |
| Generated behavior | Generated C is deterministic for repeated translation of the same accepted vector, contains no source image or runtime opcode decoder, compiles with `-std=c11 -Wall -Wextra -Werror -pedantic`, and produces the specified full state and `instruction_budget_exhausted` result. |
| Rejections | Exit class, stable category, all available source/edge/mapping provenance, null/unavailable fields, and no generated/compiled successful artifact match the relevant inherited negative case. |
| Oracle state | Where an adapter is available, generated MOVEQ final state or generated direct-flow post-block state matches the pinned Musashi result described below. |

This deliberately does **not** require equality between CLI outputs. For
example, `emit-moveq-c` reports a one-instruction state while
`emit-direct-flow-c` reports block boundaries; `analyze` reports reset-image
validation. They have different contracts, argument domains, and JSON shapes.
Only repeated output from the same interface and fixture, or comparison to that
interface's defined fixture/oracle observables, is required.

## Fixture and pinned-oracle reuse

Reuse only the project-authored, hash-checked fixtures already in the
repository; do not add commercial images or alter their bytes during migration.

| Behavior | Reused path | Required use |
| --- | --- | --- |
| MOVEQ positives/negatives | [`tests/fixtures/moveq-fixtures.json`](../../tests/fixtures/moveq-fixtures.json) and [`tests/fixtures/moveq-oracle-vectors.json`](../../tests/fixtures/moveq-oracle-vectors.json) | Preserve the listed positive state/provenance cases and the listed odd, truncated, illegal, and unsupported rejection cases. The oracle manifest covers all D0--D7 destinations with zero, positive, and negative immediates. |
| Direct-flow positives | [`tests/fixtures/direct-flow-oracle-vectors.json`](../../tests/fixtures/direct-flow-oracle-vectors.json) | Reuse `synthetic/SEG-003/direct-loop-v1`, its exact SHA-256, both loop vectors, expected blocks/edges/boundaries, and both taken/fallthrough `BNE` outcomes. |
| Direct-flow negatives | [`tests/fixtures/direct-flow-fixtures.json`](../../tests/fixtures/direct-flow-fixtures.json) and [`tests/fixtures/direct-flow-subq-boundary-fixtures.json`](../../tests/fixtures/direct-flow-subq-boundary-fixtures.json) | Retain the contract-defined decode, target, mapping, structural-overlap, and unresolved-edge negative coverage; additions must be migration regressions, not new instruction support. |

The pinned independent oracle is Musashi commit
`313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`, configured for the original
MC68000. It is local and ignored; neither Musashi, its adapter, generated
opcode files, nor adapter input/output artifacts may be committed. The exact
adapter invocations used by the current focused harnesses are:

```sh
"$SEGARECOMP_MOVEQ_MUSASHI_ORACLE" \
  --input "$input" --output "$output" --instructions 1

"$SEGARECOMP_DIRECT_FLOW_MUSASHI_ORACLE" \
  --input "$input" --output "$output" --blocks "$blocks"
```

Prerequisites are: an executable path in each corresponding environment
variable; for direct flow, a `musashi` checkout adjacent to the adapter whose
`git -C "$(dirname "$SEGARECOMP_DIRECT_FLOW_MUSASHI_ORACLE")/musashi" rev-parse
HEAD` is exactly the pinned revision; and a C11 compiler passed to the focused
harness. The direct-flow harness enforces the checkout revision; the MOVEQ
harness validates the adapter's reported `musashi@<revision>` identity. The
harnesses create schema-specific temporary JSON inputs themselves, so no
adapter path or temporary filename is a repository contract.

## Migration-specific fail-closed regressions

In addition to every negative already required by SEG-002 and SEG-003, the
implementation/validation work must reject these consolidation failures with a
stable, source-provenanced diagnostic and no generated successful execution:

| Regression to detect | Required failure property |
| --- | --- |
| A previously rejected word becomes a selected operation through a common decoder, including `ILLEGAL`, unsupported primary words, unsupported direct-flow forms, or zero-displacement branch forms. | Preserve the inherited diagnostic precedence and rejection category; do not turn it into a no-op or select it because another slice can decode a related word. |
| Consolidation reads before typed-space/variant, odd-address, mapping, or bounds validation; drops offset/bytes/length; or derives provenance from a host buffer. | Reject before the prohibited read or inference and retain the earliest applicable typed source provenance. |
| A direct target is guessed, dynamically decoded, ambiguously mapped, unmapped, odd, inside a completed instruction, or persisted unresolved. | Preserve the SEG-003 target-validation order and diagnostic provenance; produce neither an implicit block nor runtime dispatch. |
| A unified route changes the startup validator's established `analyze` contract, or accepts its result as a direct-flow entry without separately evidenced mapping and entry integration. | Preserve the startup ingress's existing validation/report behavior; leave direct-flow entry outside the frontend input contract. Do not fabricate a mapping claim or treat a `reset_seed` as console startup proof. |
| Legacy and new routes coexist so one fixture bypasses shared decode/lift/discovery/emission, or generated C embeds/fetches source bytes or decodes opcodes. | Fail architecture inspection: exactly one shared route serves migrated fixtures, and generated/runtime code has no target-byte fetch/decode or non-emitted dispatch target. |
| Reordering caused by containers or different serialization paths changes blocks, edges, diagnostics, C source, or execution records across identical runs. | The same interface/fixture/options must be deterministic; preserve mapping ingestion order and SEG-003 block/edge ordering. |

## Reproducible verification plan

This research change creates no code or tests. Its successor should run the
narrow focused checks while iterating, with the pinned-oracle environment
variables set when local adapters are available:

```sh
cmake --preset dev
cmake --build --preset dev
python3 tests/moveq_static_slice_test.py build/dev/segarecomp "$(xcrun --find cc)"
python3 tests/direct_flow_static_slice_test.py build/dev/segarecomp "$(xcrun --find cc)"
ctest --preset dev --output-on-failure
```

For oracle-backed runs, set `SEGARECOMP_MOVEQ_MUSASHI_ORACLE` and
`SEGARECOMP_DIRECT_FLOW_MUSASHI_ORACLE` to the local executable adapters before
the two Python commands. The harnesses still run static fixture, rejection,
determinism, and C11 checks when an adapter is unavailable; they report that
the independent comparison was unavailable. This is not a substitute for the
pinned differential evidence required when closing an implementation task.

## Explicit non-claims

This contract makes no new opcode, operand form, addressing mode, CPU variant,
hardware, device, mapper, memory, timing, exception, stack, call/return,
indirect-flow, reset-sequencing, or startup-mapping claim. It does not claim
that SEG-003's synthetic `reset_seed` boots a Genesis image, and it does not
extend SEG-001's bounded reset-image validation into instruction execution.
Any such behavior needs a new, independently evidenced contract before it can
enter the unified route.
