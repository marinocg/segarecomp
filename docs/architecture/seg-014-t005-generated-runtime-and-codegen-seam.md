# SEG-014-T005 generated-runtime and C11-codegen seam

## Status

Delivered by SEG-014-T005. This record captures the physical ownership move;
it does not select new Genesis device or execution behavior.

## Owners

| Responsibility | Owner |
| --- | --- |
| Persistent `GenesisRuntime` state, routed RAM/cartridge/controller/VDP access, finite dispatch drive, typed stops/provenance, and sanitized/full reports | `platforms/genesis/runtime/runtime.{h,c}` |
| Strict-C11 runtime build artifact | `segarecomp_runtime_genesis` (`C_STANDARD 11`, no host-C++ link dependency) |
| Generated bridge ABI-facing include, report-driver scaffold, and generated `main` rendering | `codegen/c11` (`libs/codegen/c11/src/genesis.cpp`) |
| Shared per-`M68kIrOperation` C11 lowering (`M68kMemoryEmissionContext`, `emit_m68k_operation_c`) | `codegen/c11` (`libs/codegen/c11/src/m68k.cpp`) |
| Genesis general-startup runtime/bridge block, program, and partial-program C11 rendering (`emit_m68k_general_startup_runtime_block_c`, `emit_m68k_general_startup_runtime_c` both overloads, `emit_m68k_general_startup_bridge_c` both overloads, `emit_m68k_frontend_c`) | `codegen/c11` (`libs/codegen/c11/src/frontend.cpp`) |
| Direct-flow C11 rendering (`emit_m68k_direct_flow_c`, `emit_m68k_structured_direct_flow_c`) | `codegen/c11` (`libs/codegen/c11/src/direct_flow.cpp`) |
| Metadata-only manifest C11 rendering (`emit_c_manifest`) | `codegen/c11` (`libs/codegen/c11/src/manifest.cpp`) |
| Discovery, partial-program promotion, C4 facts, and Genesis scenario policy | Existing CPU/recompiler/machine owners; not the runtime or codegen scaffold |
| Generate/compile/run/report orchestration | `tools/genesis_startup_bridge.py` |

`src/m68k_pipeline.cpp`, `src/m68k_pipeline_direct_flow.cpp`, and `src/c_emitter.cpp` no longer own
any C11 rendering body; the first two now hold only non-emission machine/CPU-adjacent logic still
awaiting SEG-014-T006's final `m68k_pipeline.hpp` removal, and the latter two are compatibility
translation units retained only so the existing build graph and any surviving forwarding declaration
keep resolving until T006. `platforms/genesis/machine/src/frontend.cpp` owns only Genesis
analysis/scenario/C4-fact composition and calls the `codegen/c11` entry points above; it renders no
C11 text itself. `tests/codegen_c11_emitter_ownership_test.py` is the regression that fails closed if
a real emitter definition body reappears outside `codegen/c11`.

The generated-program ABI is the C-only surface declared in
`platforms/genesis/runtime/runtime.h`: `GenesisRuntime`, `GenesisControlTransfer`,
`GenesisRuntimeStop`, `GenesisDispatchFunction`, `genesis_route_access`,
`genesis_runtime_step`, `genesis_runtime_run`, and the report writers. Generated
C includes that header and links the runtime source; the runtime receives no
frontend, recompiler, emitter, or C++ type.

> **2026-09-18 staleness note (SEG-007-T252 / ADR-0040).** The paragraph below
> describes the pre-T252 watchdog mechanism and is retained only as historical
> provenance; it no longer describes current behavior. `genesis_runtime_run`'s
> `dispatch_allowance` parameter is now a runner-owned finite dispatch-step
> allowance, not a guest no-progress window — it carries no guest-semantic
> meaning and never causes a guest `GENESIS_STOP`. `GenesisLoopProgressNote`,
> `genesis_note_loop_backedge`, and `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` no
> longer exist; exhaustion now produces the disjoint
> `GENESIS_RUNNER_RESOURCE_LIMIT` result, never
> `GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED` (which is retained only for
> wire/ABI stability and is currently unreachable). See
> `docs/decisions/0040-runner-owned-dispatch-allowance-replaces-generated-runtime-progress-watchdog.md`.
>
> SEG-007-T107 (historical): `genesis_runtime_run`'s third argument was a
> deterministic no-progress window, not a total-execution cutoff. A generated
> block that lowered a proven finite-loop taken back edge called
> `genesis_note_loop_backedge(runtime, loop_id, remaining_bound)` immediately
> after it selected the back-edge target; the drive credited only a
> strictly-monotonic decrease of one tracked loop instance's `remaining_bound`,
> bounded globally by `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` and never
> replenished, and failed closed at the (then-live)
> `GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED` /
> `GENESIS_DIAG_INSTRUCTION_BUDGET_EXHAUSTED` pair. The runtime never decoded
> DBcc/DBF or any target opcode to produce or consume the note. See
> `docs/decisions/0007-generated-runtime-loop-progress-watchdog.md`.

## Dependency direction

`codegen/c11` renders C that uses the runtime ABI. `platforms/genesis/runtime` does not
depend on codegen, host CPU/machine/recompiler implementation, target decoding,
or a runtime interpreter. It consumes only the C-compatible controller-I/O and
Genesis address-space contracts established by T004.

The former `tools/genesis_startup_bridge_runtime.c` semantic owner is removed.
The remaining tools header is a compatibility include only; new generated code
and bridge compilation use `platforms/genesis/runtime` directly.

## Preservation boundary

This was a literal behavior-preserving move. Existing work-RAM, immutable owned
cartridge reads, controller selectors, VDP state/control/DMA behavior,
fail-closed routing, finite dispatch, report schema, and sanitized/full privacy
behavior retain their existing implementations. No opcode fetch/decode,
interpreter, JIT, or new device behavior is introduced.

SEG-007-T107 later changed only the finite-dispatch termination *rule* (fixed
total cutoff -> no-progress watchdog with proven non-renewable finite-loop
credit); the report schema, stop/diagnostic enums,
`STOP_DIAGNOSTIC_PAIRS`, routing, privacy behavior, and the "no opcode
fetch/decode, interpreter, JIT" boundary are all unchanged.
