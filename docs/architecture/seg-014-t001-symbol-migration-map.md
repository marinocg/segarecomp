# SEG-014-T001 symbol/file-to-module migration map

**Status:** Planning artifact only. No `src/`/`include/` production code moved by this task.
**Consumer:** SEG-014-T002 and later structural-move tasks under
`docs/architecture/post-seg007-architecture-refactor-contract.md` (section 33's Phase 2 onward).
**Snapshot basis:** re-scan of the tree performed for this task, after SEG-007-T084/T091 merged.

This document lists, per target module named in the contract's sections 4/7/32, the concrete
current file(s) that own the relevant code today and the concrete current symbol/type/function
names that belong in that target module, with a short ownership rationale tied to the contract's
own rules (section 8, "Module responsibilities"; section 36, "RULE 1..10"). It does not move any
code. It also explicitly separates **permanent target modules** from **temporary SEG-002/SEG-003
compatibility surfaces** (`moveq.hpp`/`.cpp`, `direct_flow.hpp`/`.cpp`) that must not be read as
architecture destinations in their own right. A final section records which ownership questions
this correction pass resolves outright and which are deliberately deferred to a specific later
SEG-014 child task (never left as a vague "T002 must decide").

**2026-08-26 correction pass.** This document was corrected after independent review found several
ownership misclassifications that could have let SEG-014-T002 inherit incorrect assumptions:
MOVEQ/direct-flow were being treated as candidate permanent modules instead of temporary
compatibility facades; direct-flow C emission was misattributed to `cpu/m68k` instead of
`codegen/c11`; several Genesis address-space *routing* functions were misattributed to
`device/sega/genesis` instead of `machine/genesis`; the frontier-split and tooling-ownership
questions were left open when the underlying scope already resolves or bounds them. Every section
below reflects the corrected classification; no production code changed as part of this
correction (see this task's own Evidence for verification commands).

Current physical layout (all production C++ is one target today, contract section 6/28's own
named problem):

- `include/segarecomp/m68k_pipeline.hpp` (1741 lines) — the god-header the contract names by name.
- `src/m68k_pipeline.cpp` (4980 lines), `src/m68k_pipeline_frontend.cpp` (4430 lines),
  `src/m68k_pipeline_direct_flow.cpp` (691 lines).
- `include/segarecomp/{moveq,direct_flow,rom,c_emitter}.hpp` + matching `src/*.cpp`. `moveq.*` and
  `direct_flow.*` are **temporary SEG-002/SEG-003 compatibility surfaces, not permanent modules**
  — see the dedicated section immediately after `core/` below.
- `apps/segarecomp/main.cpp` (427 lines, CLI dispatcher).
- `tools/genesis_startup_bridge_runtime.{h,c}` (350 + 775 lines, generated-program runtime ABI).
- `tools/genesis_startup_bridge.py`, `tools/sonic_startup_inventory.py`, `tools/ghidra.py`
  (the private workflow/backlog tooling that once sat beside them has since moved out of this repository).
- Root `CMakeLists.txt`: one `segarecomp_core` library (`src/c_emitter.cpp`, `src/direct_flow.cpp`,
  `src/m68k_pipeline.cpp`, `src/m68k_pipeline_direct_flow.cpp`, `src/m68k_pipeline_frontend.cpp`,
  `src/moveq.cpp`, `libs/media/src/rom.cpp`), one `segarecomp` CLI executable, and per-test executables/CTest
  entries under `tests/`. No `segarecomp_base`/`segarecomp_m68k`/`segarecomp_genesis_*` target
  boundaries exist yet (contract section 28).
- `tests/` (59 files after this task's own addition; see "Testing shape" below).

---

## `core/`

Contract rule: "own only genuinely cross-target contracts"; must not know MC68000, Genesis,
startup scenarios, controller I/O, or generated-C details (section 8.1).

**Current owner:** `include/segarecomp/moveq.hpp` (the only header with no MC68000/Genesis-specific
content beyond the address-space enum's single `m68k_program` value).

**Symbols that belong in `core/`:**
- `TargetAddressSpace` (currently a one-value enum; stays cross-target-shaped even though only
  `m68k_program` exists today — adding `z80_program`/`sh2_program` values later is a `core/` change,
  not a new type).
- `M68kProgramAddress` — despite its name, this is the project's *only* typed target-address wrapper
  (`{space, value}`); every CPU/device/machine/runtime module already depends on it. Contract
  section 8.1 lists "target address types" and "provenance" as canonical `core/` content.
- `MoveqImageOffset` — the project's only typed image-offset wrapper. Same argument: despite the
  MOVEQ-era name, every stage (decode, direct-flow, general-startup, bridge runtime) already reuses
  this one type for "how far into the source image this byte came from". **Resolved direction**
  (see the renamed final section): T002 relocates it into `core/` as a fixed-width (`uint64_t`)
  `ImageOffset`, converging toward the single type `rom.hpp`'s independently-defined `ImageOffset`
  should also reuse once T004 relocates the reset-image validator; this is the one piece of
  `moveq.hpp` content that is *not* part of the temporary compatibility surface described below —
  it is a genuinely reusable core type that merely lives in the wrong file today.
- `ByteLength` — generic verified-length wrapper, reused by every instruction/IR/provenance record.
- `DecodeSource` (`{cpu_variant, address, image_offset}`) and `InstructionProvenance`
  (`{source, bytes, length}`) — the shared typed-provenance contract itself (contract section 3.2 and
  8.1's "provenance").
- `CpuVariant` — currently one-value (`mc68000`); same "genuinely cross-target, grows later" shape as
  `TargetAddressSpace`.

**Rationale:** these five/six types are the only ones in the current codebase with zero MC68000
opcode/Genesis-device content and are already consumed identically by every later stage. They are
misplaced today only because `moveq.hpp` is named after SEG-002's slice, not because their content
is CPU- or Genesis-specific.

**Explicitly NOT `core/`:** `DataRegister` (MC68000 register file — `cpu/m68k/`, not core, per
section 8.1's "must not know MC68000"), `DecodeOutcome` (mixes core-shaped rejections like
`odd_instruction_address`/`truncated_instruction` with the MOVEQ-specific `decoded_moveq` —
**resolved**: `DecodeOutcome` must NOT be proposed as a permanent `core/` type; it stays attached to
the temporary MOVEQ compatibility surface below until that surface is deleted, and the architectural
decode-result API `core`/`cpu/m68k` code should actually use is the shared
`M68kDecodeResult`/`RejectedM68kDecode`/`M68kDecodedInstruction` family, which already carries no
MOVEQ-specific value), and every `rom.hpp` type (image/header classification is `media/` per
section 7, not `core/`).

---

## Temporary SEG-002/SEG-003 compatibility surfaces (not permanent modules)

**This section exists because independent review found the original version of this document
implicitly treated `moveq.hpp`/`.cpp` and `direct_flow.hpp`/`.cpp` as candidate permanent module
content. They are not.** Both are historical SEG-002/SEG-003 slice surfaces that predate the shared
MC68000 decode/lift/IR/C4/codegen route SEG-012 established. Today both already delegate their real
work to that shared route; what remains in each file is a thin, named-after-a-historical-slice
compatibility wrapper, not architecture.

**`include/segarecomp/moveq.hpp` + `src/moveq.cpp`.**
- `decode_moveq()` already calls `decode_m68k_instruction(image, source,
  M68kDecodeProfile::moveq)` directly (`src/moveq.cpp`) and only re-wraps the shared
  `M68kDecodedInstruction` result into the MOVEQ-named `MoveqInstruction`/`DecodedMoveq` types.
  `lift_moveq()` likewise builds a shared `M68kDecodedInstruction` and calls
  `lift_m68k_instruction()` directly, then re-wraps the shared `M68kIrOperation` result into
  `IrMoveq32`. `emit_moveq()`'s actual per-operation C lowering is the one shared
  `emit_m68k_operation_c(..., M68kIrKind::write_moveq, ...)` call (`codegen/c11`, see below); only
  the surrounding standalone-test-harness scaffolding (`main()`, the fixed printf report format,
  the literal `#include`s) is MOVEQ-specific and predates the general-purpose bridge/startup C
  emission entry points.
- **Intended end state:** the reusable provenance/address/core types this file happens to also
  define (`MoveqImageOffset`, i.e. `core/`'s future `ImageOffset` — see the `core/` section above)
  migrate to `core/`; every M68K register/instruction fact (`DataRegister`, and MOVEQ's own
  identity as `M68kInstructionKind::moveq`/`M68kIrKind::write_moveq`) already lives in, and stays
  in, `cpu/m68k/`; MOVEQ itself becomes exactly what it already semantically is — one ordinary M68K
  instruction with no dedicated module — and the standalone `moveq.hpp`/`moveq.cpp` compatibility
  module (`MoveqInstruction`, `IrMoveq32`, `DecodedMoveq`, `RejectedMoveq`, `MoveqDecodeResult`,
  `MoveqCpuState`, `EmittedMoveq`, `decode_moveq`, `lift_moveq`, `emit_moveq`, `emit_moveq_c`,
  `decode_outcome_name`, `format_moveq_rejection`, and `DecodeOutcome` itself) should be **deleted**
  once legacy CLI/report compatibility (the `emit-moveq-c` command and its exact wire format) is
  routed through the shared architecture instead. **SEG-014-T006 is the task that removes it**
  (T006's own scope: "Remove obsolete temporary forwarding facades and dead parallel routes
  introduced only to stage T002-T005").
- **T002 must not be blocked on this deletion.** T002 may retain `moveq.hpp`/`.cpp` unchanged, or
  as a thin forwarder onto the newly-extracted `cpu/m68k`/`core` types (mirroring the pattern
  `direct_flow.cpp` already uses below), as long as it has **no semantic ownership** of its own —
  every fact it reports must trace to a shared `cpu/m68k`/`core` computation, never a
  MOVEQ-private decision.

**`include/segarecomp/direct_flow.hpp` + `src/direct_flow.cpp`.**
- `src/direct_flow.cpp` is *already* an explicit compatibility forwarder today — its own comment
  reads "Compatibility surface only. The selected MC68000 route is implemented by
  `m68k_pipeline_direct_flow.cpp` alongside the shared decode/lift frontend" — and every one of its
  five functions (`direct_flow_diagnostic_name`, `discover_direct_flow`, `execute_direct_flow`,
  `emit_direct_flow_c`, `format_direct_flow_rejection`) is a one-line forwarder onto the
  `m68k_pipeline`-owned `m68k_direct_flow_diagnostic_name`/`discover_m68k_direct_flow`/
  `execute_m68k_direct_flow`/`emit_m68k_direct_flow_c`/`format_m68k_direct_flow_rejection`.
- **Intended end state:** this file/forwarder pair should not retain semantic ownership long term
  either. Direct-flow *analysis/discovery/execution* (`DirectFlowProgram`, `DirectFlowAnalysis`,
  `discover_m68k_direct_flow`, `execute_m68k_direct_flow`, and the block/edge/state types alongside
  them) belongs to `cpu/m68k`/`recompiler` as appropriate — see the `cpu/m68k/` section below for
  the current best split and the renamed final section for what remains genuinely open. Direct-flow
  **C emission** (`emit_m68k_direct_flow_c`, `emit_m68k_structured_direct_flow_c`) belongs to
  `codegen/c11`, **not** `cpu/m68k` — CPU modules decide instruction meaning, they do not render C
  text (RULE 5); the original version of this document incorrectly listed these two emission
  functions under `cpu/m68k/`, corrected below. Once `main.cpp`'s `emit-direct-flow-c` command is
  routed through whatever `codegen/c11`/scenario-emission entry point T005/T006 establish, the
  `direct_flow.hpp`/`.cpp` forwarder itself should be deleted — again **SEG-014-T006's** "remove
  obsolete temporary forwarding facades" scope, not T002's.

---

## `cpu/m68k/`

Contract rule: "own MC68000 architecture semantics only" — registers/CCR, instruction
representation, EA decoding, legal EA sets, decode, CPU-specific IR, instruction effects,
CPU-level control flow, call/return semantics, CPU-level static program representation, CPU
discovery (section 8.2). Forbidden dependency: `cpu/m68k -> machine/genesis` or
`cpu/m68k -> device/sega/genesis`.

**Current owner:** almost all of `include/segarecomp/m68k_pipeline.hpp` plus most of
`src/m68k_pipeline.cpp`/`src/m68k_pipeline_frontend.cpp`/`src/m68k_pipeline_direct_flow.cpp`,
`include/segarecomp/moveq.hpp`'s decode/lift functions, and `include/segarecomp/direct_flow.hpp`.

**Symbols that belong in `cpu/m68k/` (pure ISA semantics, no Genesis policy):**
- Decode/EA/instruction representation: `M68kDecodeProfile`, `M68kEaMode`, `M68kEffectiveAddress`,
  `M68kEaLegalMask` and every `m68k_ea_*` mask constant, `M68kMemoryAccessWidth`,
  `M68kMemoryAccessDirection`, `AddressRegister`, `M68kInstructionKind`, `M68kMovemDirection`,
  `M68kShiftRotateKind`, `M68kIrKind`, `M68kCondition`, `M68kDecodedInstruction`, `M68kIrOperation`,
  `decode_m68k_instruction`, `lift_m68k_instruction`, `m68k_ir_is_transfer`,
  `RejectedM68kDecode`/`M68kDecodeResult`.
- Legacy SEG-002/SEG-003 direct-flow *analysis/discovery/execution* facts that are still pure ISA
  facts (not Genesis policy) and not C emission (see "Temporary SEG-002/SEG-003 compatibility
  surfaces" above for the emission functions this document originally, incorrectly, listed here):
  `DirectFlowKind`, `DirectFlowIrKind`, `DirectEdgeKind`, `DirectCondition`, `DirectFlowDiagnostic`,
  `DirectInstruction`, `DirectFlowIrOperation`, `BlockId`, `BlockProvenance`, `DirectBlock`,
  `DirectEdge`, `DirectFlowState`, `BlockBoundary`, `DirectFlowProgram`, `DirectFlowAnalysis`,
  `RejectedDirectFlow`, `DirectFlowExecution`, `DirectFlowAnalysisResult`,
  `DirectFlowExecutionResult`, `discover_m68k_direct_flow`, `discover_m68k_direct_flow_entries`,
  `execute_m68k_direct_flow`, `format_m68k_direct_flow_rejection`, `m68k_direct_flow_diagnostic_name`.
  `DataRegister` (the M68K D0-D7 register enum) is also `cpu/m68k/` content despite physically
  living in `moveq.hpp` today — it is reused far beyond MOVEQ (e.g. `M68kRegisterWrite`) and is not
  part of the temporary MOVEQ compatibility surface described above.
- **Corrected: `emit_m68k_direct_flow_c` and `emit_m68k_structured_direct_flow_c` are NOT
  `cpu/m68k` content.** They are C-emission functions and belong to `codegen/c11` (see that section
  below) under RULE 5 ("codegen only renders"); the original version of this document misattributed
  them here. The compatibility wrappers `discover_direct_flow`/`execute_direct_flow`/
  `emit_direct_flow_c`/`format_direct_flow_rejection`/`direct_flow_diagnostic_name` in
  `direct_flow.hpp`/`direct_flow.cpp` are covered by the "Temporary SEG-002/SEG-003 compatibility
  surfaces" section above, not this one — they are forwarders, not `cpu/m68k` ownership.
- MOVEQ itself (`decode_moveq`/`lift_moveq`/`emit_moveq`/`emit_moveq_c` and the MOVEQ-named
  wrapper types) is covered entirely by the "Temporary SEG-002/SEG-003 compatibility surfaces"
  section above, not here: it is not a `cpu/m68k`-owned module, it is a temporary facade around
  `cpu/m68k`'s already-generic `M68kInstructionKind::moveq`/`M68kIrKind::write_moveq` handling.
- CPU-level static program/discovery (profile-neutral per the header's own comment at
  `M68kStaticCall`'s declaration, "currently have only one populating profile ... not because they
  are startup-specific types"): `M68kStaticCall`, `M68kStaticEdgeKind`, `M68kStaticBlock`,
  `M68kStaticFrame`, `M68kStaticEdge`, `StaticCallReturnDiscovery`, `StaticCallReturnResult`,
  `discover_m68k_static_call_return`, `discover_m68k_general_startup`,
  `classify_m68k_cpu_frontier`/`M68kCpuFrontierKind`.
- Semantic-operation helpers ("one shared X owner" comments throughout): `M68kSubtractionResult`,
  `M68kAdditionResult`, `M68kLogicalResult`, `M68kExtendFlagPolicy`, `m68k_evaluate_subtraction`,
  `m68k_evaluate_addition`, `m68k_addition_ccr`, `m68k_evaluate_logical`, `m68k_logical_ccr`,
  `M68kBitOperationKind`, `M68kBitOperationResult`, `m68k_evaluate_bit_operation`,
  `m68k_bit_test_ccr`, `m68k_evaluate_condition`, `m68k_condition_c_expr`, `m68k_branch_target`,
  `M68kDbccDecrementResult`, `m68k_evaluate_dbcc_decrement`, `M68kMovemTransferOrder`,
  `m68k_movem_transfer_order`, `M68kShiftRotateResult`, `m68k_evaluate_shift_rotate`,
  `m68k_shift_rotate_ccr`, `m68k_subtraction_ccr`, `m68k_compare_ccr`,
  `m68k_is_statically_foldable_control_ea`, `M68kRegisterWrite`, `M68kMemoryEffectKind`,
  `M68kStackEffectKind`, `M68kPcEffectKind`, `M68kOperationEffect`, `m68k_operation_effect`,
  `m68k_move_result_ccr`.
- Address-policy helpers that are pure MC68000 addressing-mode mechanics, not Genesis memory-map
  policy: `m68k_startup_absolute_operand_alignment`, `m68k_genesis_canonical_ea_address`.
  **Resolved** (was listed as an open question in the original version of this document): its
  actual body is exactly `ea.mode == absolute_word ? ea.absolute_address & 0x00FFFFFF :
  ea.absolute_address` — the generic MC68000 rule that an absolute-word encoding retains its
  decoded sign-extended 32-bit value while its logical bus address is only the low 24 bits. This
  computation contains no Genesis memory-map/device/mapping decision whatsoever (it does not touch
  ROM/RAM/controller-I/O/VDP classification); it is purely "what 24-bit bus address does this
  already-decoded EA correspond to", i.e. `cpu/m68k` CPU/bus mechanics, with the *result* of that
  computation then handed to `machine/genesis`'s separate address-routing step (see
  `machine/genesis/` below) to classify. Only its *name* is Genesis-specific. T002 should rename it
  to something like `m68k_canonical_bus_address` when it moves this function, so a future
  Sega-CD/32X/SMS/Game-Gear MC68000-hosting machine does not have to call a Genesis-named function
  for ordinary CPU addressing-mode mechanics. Do not let this name force generic M68K semantics into
  `device/sega/genesis`/`machine/genesis`.

**Rationale:** every symbol above answers "what does this MC68000 instruction/EA/condition/decode
form mean", independent of which machine hosts the CPU, matching RULE 1 (section 36) exactly.

---

## `device/sega/genesis/`

Contract rule: "devices implement hardware behavior that is reusable across machine
configurations" (section 8.5); "must not know Genesis controller behavior" boundary belongs to
devices, never `cpu/m68k` (RULE 2).

**Corrected boundary (was wrong in the original version of this document): device semantics vs.
machine routing.** Independent review correctly identified that this document's first version put
Genesis *address-space routing* — deciding whether a given address is ROM, synthetic work-RAM,
controller-I/O, or unmapped — under `device/sega/genesis/`. That is `machine/genesis/`'s job per
contract section 9 ("which devices exist, memory maps, **address routing**") and section 13.3's own
named `MachineAddressRouter` concept ("MC68000 request -> MachineAddressRouter -> add-on overlay
claims address? ... otherwise -> GenesisBase bus"). `device/sega/genesis/` owns only *what a given
device does once an access has already been routed to it* — its own register layout and protocol
behavior. This correction is not cosmetic: leaving routing inside `device/sega/genesis/` would let
it grow into exactly the kind of second god-module RULE 10/section 35.2 warns against. **T004
executes this reclassification** (its own scope: "Move current Genesis memory-map, work-RAM/
cartridge routing, controller I/O, VDP/register/DMA state and relevant machine-policy code into
explicit device/machine owners"); T002 does not move any Genesis-specific code (T002's own scope is
CPU-only and explicitly makes the build reject an MC68000-to-Genesis dependency).

**Current owner:** the controller-I/O and (nascent, runtime-only-so-far) VDP sections of
`m68k_pipeline.hpp`/`m68k_pipeline.cpp`/`m68k_pipeline_frontend.cpp`, plus the device-state parts of
`tools/genesis_startup_bridge_runtime.{h,c}`.

**SEG-014-T004 delivered header seam.** `device/sega/genesis/controller_io.hpp` now owns the
C++ controller request/result/classification declarations and imports only the narrow address/request
contracts it needs, never `m68k_pipeline.hpp`. `controller_io_contract.h` owns the C/C++ descriptor
table for all three currently-supported selectors; both the host device policy and strict-C11 runtime
iterate that table. `m68k_pipeline.hpp` temporarily reexports the owning device/machine headers for
compatibility only.

**Symbols that belong in `device/sega/genesis/` (host/static side — controller-I/O *device
protocol*, not routing):**
- `ControllerIoAccessShapeMismatch`, `ControllerIoTargetRegisterClass`, `ControllerIoAccessShape`,
  `M68kControllerIoPolicyProvenance`, `M68kControllerIoWordObservation`, `M68kControllerIoResult`,
  `M68kControllerIoFailure`, `M68kControllerIoAccessResult`, `m68k_controller_io_access`,
  `m68k_classify_controller_io_access_shape`, `m68k_classify_controller_io_target_register_class`,
  and every `m68k_controller_io_*_register_address`/`m68k_controller_io_selector_*`/
  `m68k_controller_io_region_*` constant (SEG-007-T020/T021/T032/T034/T035/T038/T079 policy) — this
  is genuinely "what does reading/writing *this specific already-identified* controller register
  do", the device's own register map and behavior, not a decision about which addresses belong to
  it.

**Symbols reclassified OUT of `device/sega/genesis/` into `machine/genesis/` by this correction**
(see that section below for the full listing and rationale): `M68kGenesisDeviceRoutingResult`,
`m68k_route_genesis_device_access`, `M68kAbsoluteOperandRegion`, `M68kAbsoluteTestOperand`,
`M68kAbsoluteTestOperandResolution`, `m68k_resolve_absolute_test_operand` (both overloads), and the
synthetic-work-RAM window policy (`m68k_startup_ram_begin`/`m68k_startup_ram_end`/
`m68k_startup_ram_range_in_range`/`m68k_startup_ram_operand_in_range`/`m68k_startup_ram_offset`) —
all of these decide *which region an address belongs to* (ROM vs. RAM vs. controller-I/O vs.
unmapped), which is routing, not device behavior.

**Symbols that belong in `device/sega/genesis/` (generated-runtime/native side — see also
`platforms/genesis/runtime/` below for the physical destination once contract section 23/Phase 7 applies):**
- Every `GenesisVdp*`/`GenesisOwnedCartridgeRegion`/`GENESIS_VDP_*` type and constant in
  `tools/genesis_startup_bridge_runtime.h`, and the matching VDP-register/DMA logic in
  `tools/genesis_startup_bridge_runtime.c` (`genesis_vdp_access` and friends) — this is Genesis VDP
  *hardware* semantics, reusable by a future Sega CD/32X machine composition unchanged, exactly the
  device/runtime split contract sections 23-25 describe.

**Note on `genesis_route_access` (runtime side, `tools/genesis_startup_bridge_runtime.c`):** the
same host-side routing-vs-device correction applies here. `genesis_route_access` plays the
generated-runtime's own `MachineAddressRouter` role (it decides ROM vs. work-RAM vs. controller-I/O
vs. VDP vs. unmapped before dispatching), structurally mirroring `bus.c` in the contract's own
future `runtime/components/sega/genesis/{bus.c,ram.c,io.c,vdp.c}` shape (section 25) — it is not, by
itself, device protocol behavior in the way `genesis_vdp_access`/the controller-I/O dispatch it
calls into are. This does not change this correction's `platforms/genesis/runtime/` placement below (contract
section 32 explicitly says not to build the finer `runtime/components/...` split during the first
migration), but T004/T005 should keep this routing/device distinction in mind — and reconcile it
with the host-side `m68k_route_genesis_device_access`/`m68k_resolve_absolute_test_operand` split
below — when they do their own work, exactly as T004's own scope already anticipates ("Reconcile
translation-time fold/resolution and runtime routing so they consume one authoritative device
policy/specification rather than independently reimplementing values/selectors") and T005's scope
restates ("Preserve static/runtime Genesis device policy consistency established by T004").

**Rationale:** controller I/O and VDP *device protocol* are Genesis hardware, not MC68000 ISA or
generic machine wiring; RULE 2 places their semantics here, and RULE 7 (section 36) requires the
static/runtime halves to share one specification even though they currently live as two
independently-typed C++/C implementations without a shared descriptor. This module must stay
narrowly scoped to actual device behavior — never address-space routing — so it does not become a
second god-module in place of the one the refactor is removing.

---

## `machine/genesis/`

Contract rule: "which CPU instances exist, which devices exist, memory maps, address routing,
interrupt wiring, bus ownership, scheduling, shared memory, machine boot/reset composition"
(section 9); "should not redefine CPU instruction semantics or duplicate device implementations."

**Current owner:** the startup-profile/ingress/mapping-claim machinery spread across
`m68k_pipeline.hpp` and `m68k_pipeline_frontend.cpp`, plus `rom.hpp`/`rom.cpp`'s reset-image
validator, plus the startup-state-construction half of `apps/segarecomp/main.cpp`.

**SEG-014-T004 delivered header seam.** `machine/genesis/address_space.hpp` now owns the synthetic
work-RAM range/offset API, controller-region routing result, and absolute-operand-resolution types and
declarations. Its implementation remains `platforms/genesis/machine/src/address_space.cpp`; the pipeline facade
reexports it temporarily. The target graph now has concrete `segarecomp_device_genesis` and
`segarecomp_machine_genesis` libraries, with the machine linking the device policy after its own
address-space classification rather than treating either as an interface-only facade.

**Symbols reclassified INTO `machine/genesis/` by this correction (moved out of
`device/sega/genesis/`; see that section above for why):** `M68kGenesisDeviceRoutingResult`,
`m68k_route_genesis_device_access`, `M68kAbsoluteOperandRegion`, `M68kAbsoluteTestOperand`,
`M68kAbsoluteTestOperandResolution`, `m68k_resolve_absolute_test_operand` (both overloads), and the
synthetic-work-RAM window policy (`m68k_startup_ram_begin`/`m68k_startup_ram_end`/
`m68k_startup_ram_range_in_range`/`m68k_startup_ram_operand_in_range`/`m68k_startup_ram_offset`).
These are exactly the contract's own `MachineAddressRouter` concept (section 13.3): given an
address, decide whether it resolves to ROM, RAM, controller-I/O (delegating the actual register
behavior to `device/sega/genesis/` once routed there), or is unmapped/unsupported. Note that
`m68k_route_genesis_device_access` (used by controller-I/O/VDP-shaped dispatch call sites) and
`m68k_resolve_absolute_test_operand` (used by absolute-operand-resolution call sites) currently
re-implement overlapping pieces of this same ROM/RAM/device routing decision independently — T004's
own scope already targets exactly this kind of duplication ("one authoritative device
policy/specification rather than independently reimplementing values/selectors"), so T004 should
treat unifying these two into one router, not just relocating both unchanged, as in scope.

**Symbols that belong in `machine/genesis/`:**
- `M68kFrontendProfile` (`direct_flow`/`genesis_rom_startup`/`general_startup`). **Correction:** the
  original version of this document classified this enum wholesale as `machine/genesis/`. It should
  not be, without qualification: `genesis_rom_startup`/`general_startup` are genuinely Genesis
  *workflow/scenario* selections per contract section 21 ("profiles and startup scenarios ...
  represent workflows/scenarios, not MC68000 ISA semantics"), but the enum's own three-value mix
  (including the CPU-route-selecting `direct_flow` value, and gating `M68kDecodeProfile` — a
  decode-capability concept — alongside it) is itself the clearest evidence in the codebase of
  exactly the kind of mixed CPU/Genesis ownership this refactor exists to separate. This document
  does not resolve that split (see the renamed final section: **SEG-014-T003** begins it per its own
  scope, "move startup/scenario profile composition upward where it represents a Genesis/
  application workflow rather than ISA semantics"; **SEG-014-T004** completes the Genesis-scenario
  half's landing in `machine/genesis/`). For T002's own purposes the enum and its three
  profile-specific discovery/execution/report/emission entry points below may move as-is
  (compatibility-preserving), without pre-deciding the eventual split:
  `analyze_m68k_frontend`, `discover_m68k_general_startup` (dual-owned with `cpu/m68k/`'s static
  discovery machinery — see the renamed final section), `format_m68k_general_startup_result`,
  `execute_m68k_frontend_startup`, `format_genesis_rom_startup_result`, `format_m68k_frontend_result`.
- `MappingClaim`, `FrontendImage`, `M68kStartupIngress`, `FrontendProgram` (including its nested
  `CompletionContract`), `FrontendAnalysis` (including its nested `CompletionRecord`),
  `StartupBusKind`, `StartupBusRecord`, `FrontendRejected`, `M68kMemoryAccessRequest`,
  `GenesisFrontierClass`, `UnresolvedFrontier`, `FrontendPartialProgram`, `FrontendResult` — the
  Genesis-specific ingress/mapping/frontier composition the frontend builds around the CPU's own
  decode/discovery facts. (`FrontendPartialProgram`/`UnresolvedFrontier`/`GenesisFrontierClass` are
  also flagged below: contract section 20 wants this promoted to `recompiler/`, not left in
  `machine/genesis/` or `cpu/m68k/` — **deferred to SEG-014-T003**, see the renamed final section.)
- `StartupInstructionKind`, `StartupInstruction`, `StartupState`, `StartupBoundary`, `StartupReturn`,
  `StartupExecution`, `StartupFailure`, `StartupExecutionTestContext`, `StartupResult` — the fixed
  five-operation `genesis_rom_startup` execution/report vocabulary (Genesis-scenario-shaped, not
  general MC68000 execution).
- `FrontendOracleVector`, `FrontendVectorAccepted`, `FrontendVectorRejected`, `FrontendVectorResult`,
  `validate_and_execute_m68k_frontend_vector` — Genesis-scenario oracle/vector validation glue.
- `analyze_genesis_reset_image`/`GenesisResetImageReport`/`ResetOutcome`/`ResetDiagnostic`/
  `ResetRangeStatus`/`ResetCheckStatus`/`ResetRange`/`ResetVectorProvenance`/`M68kAddress24`/
  `VectorWord32`/`ImageOffset` (all in `rom.hpp`/`rom.cpp`) — Genesis reset-vector/mapping
  validation, i.e. "machine boot/reset composition" (section 9), not `media/`'s generic
  header-classification job (see the `media/` note below) and not `core/`'s generic image offset.
  `ImageOffset` here is a second, independently-defined type with the same conceptual meaning as
  `core/`'s (`MoveqImageOffset`-derived) `ImageOffset` — **resolved direction** (see the renamed
  final section): T004 should reuse the single `core/`-owned `ImageOffset` T002 establishes rather
  than keep this second definition; this is structural cleanup only, no reset-validation behavior
  changes.
- The startup-state-construction logic in `apps/segarecomp/main.cpp`'s `genesis-rom-startup`/
  `emit-genesis-rom-startup-c`/`genesis-general-startup` command bodies (initial register values,
  SSP-from-reset-image wiring, mapping-claim construction) — this is machine boot composition
  currently living in the CLI, which section 30 says the CLI must not own.

**Rationale:** every symbol above is a Genesis-specific *scenario/composition* fact (which
addresses are ROM, what the reset vectors mean, what the fixed startup graph looks like), never an
MC68000 ISA fact and never raw device hardware behavior — exactly `machine/`'s job per section 9.

**Explicitly NOT `machine/genesis/`:** `M68kC4*` preflight types (see `recompiler/`/`codegen/c11`
split below) and `M68kMemoryEmissionContext`/`emit_m68k_operation_c` (see `codegen/c11` below).

---

## `media/`

Contract section 7 lists `media/cartridge.hpp`/`cd_image.hpp`/`cue_bin.hpp` as the target shape for
"image/media ingestion" (section 4 item 5), separate from `machine/`'s reset/boot composition.

**Current owner:** `libs/media/include/segarecomp/rom.hpp`/`libs/media/src/rom.cpp`'s header-classification half only.

**Symbols that belong in `media/`:**
- `Platform`, `ClassificationOutcome`, `Diagnostic`, `ByteRange`, `HeaderCandidate`, `RomInfo`,
  `image_size_limit`, `read_binary`, `inspect_rom`, `platform_name`, `outcome_name`,
  `diagnostic_name`, `format_inspection` — generic multi-platform (Genesis/SMS/GG) container/header
  classification with no Genesis-specific reset/mapping semantics.

**Rationale:** this is literally "validates container/header data" (`docs/architecture/pipeline.md`
stage 1), reusable unchanged by SMS/GG ingestion; the *reset-image* validator in the same file
(listed under `machine/genesis/` above) is a distinct, Genesis-only boot-composition concern the
contract's own section 7 tree separates from generic media ingestion.

---

## `recompiler/`

Contract rule (section 19-20): "partial programs, multiple unresolved frontiers, machine
compilation plans, mapping of CPU programs into machine programs, runtime-selected frontier exits,
compilation result composition" — higher-level than MC68000 semantics, promoted above `cpu/m68k/`.

**Current owner:** the frontier/partial-program types already listed once above under
`machine/genesis/` (`FrontendPartialProgram`, `UnresolvedFrontier`, `GenesisFrontierClass`,
`M68kMemoryAccessRequest`), plus the C4-preflight-gap machinery in `m68k_pipeline.hpp`/
`m68k_pipeline_frontend.cpp`.

**Symbols that belong in `recompiler/` (promoted out of `machine/genesis/`, per contract section
20's explicit instruction that promotion into a machine-level partial program "belongs above"
the CPU module):**
- `FrontendPartialProgram`, `UnresolvedFrontier`, `GenesisFrontierClass`,
  `m68k_discovery_max_frontier_exits` — this is the multi-exit/frontier-promotion concept section 20
  names by its own conceptual model (`RecompiledProgram { cpuProgram; machinePlan;
  unresolvedFrontiers[]; provenance }`). Today it is Genesis-specific only because no other machine
  exists yet, not because the concept is intrinsically Genesis-shaped — the exact `cpu/m68k` vs
  `recompiler/` split this requires is **deferred to SEG-014-T003** (see the renamed final section),
  matching T003's own scope ("Promote partial-program/multi-exit/frontier and compilation-plan
  concerns that are not intrinsically MC68000 into an explicit `recompiler` owner").
- `M68kC4OperandRole`, `M68kC4AutoUpdateClass`, `M68kC4GapClass`, `M68kC4PreflightRow`,
  `M68kC4Preflight`, `preflight_m68k_general_startup_c4` — a compilation-plan-readiness/gap-tracking
  artifact over an already-discovered program, matching section 19's "compilation result
  composition", not raw CPU or device semantics.
- `M68kStaticMemoryFactRole`, `M68kStaticMemoryFact`, `M68kMovemAdjacentLeaFact`,
  `M68kOwnedCartridgeRegionFact` — discovery-time facts consumed only by C4/codegen lowering
  decisions (constant-propagation/ownership proofs), i.e. "safe transformations" (pipeline.md stage
  5, "Analysis"), not CPU ISA semantics and not raw Genesis device hardware.

**Rationale:** these are the "which of several statically discovered outcomes does this program
have, and is it ready to compile" concerns pipeline.md calls "Analysis" and the refactor contract
calls `recompiler/`; they reference `cpu/m68k/` facts (by provenance, never by copy) but are not
themselves ISA semantics.

---

## `codegen/c11/`

Contract rule: "emits already-understood programs"; "must not rediscover instructions" (RULE 5).

**Current owner:** the `emit_*` functions spread across `m68k_pipeline.cpp`/
`m68k_pipeline_frontend.cpp`/`m68k_pipeline_direct_flow.cpp`, plus `c_emitter.hpp`/`c_emitter.cpp`.

**Symbols that belong in `codegen/c11/`:**
- `M68kMemoryEmissionContext`, `emit_m68k_operation_c` — the one shared per-`M68kIrOperation`
  C-lowering definition (contract RULE 5: "codegen only renders").
- `emit_m68k_general_startup_runtime_block_c`, `emit_m68k_general_startup_runtime_c` (both
  overloads), `emit_m68k_general_startup_bridge_c` (both overloads), `emit_m68k_frontend_c` — the
  Genesis-startup-shaped C emission entry points (these read `machine/genesis/`'s
  `FrontendAnalysis`/`FrontendPartialProgram` but only render already-decided structure).
- `emit_c_manifest` (`c_emitter.hpp`/`c_emitter.cpp`) — currently a header-manifest prototype per
  `pipeline.md`'s own "Initial Shape" note; still belongs conceptually in `codegen/c11/` once real,
  even though it emits no instruction semantics today.
- **Corrected: `emit_m68k_direct_flow_c`, `emit_m68k_structured_direct_flow_c`** (implemented in
  `src/m68k_pipeline_direct_flow.cpp`, declared in `m68k_pipeline.hpp`) — direct-flow C emission.
  The original version of this document listed these under `cpu/m68k/`; they render already-decided
  block/edge/state structure as C11 text and decide no instruction support themselves, so they
  belong here under RULE 5, not with CPU semantics. They are reached today only through the
  `direct_flow.hpp`/`direct_flow.cpp` compatibility forwarder's `emit_direct_flow_c` (see "Temporary
  SEG-002/SEG-003 compatibility surfaces" above); that forwarder should be retired once callers use
  these (or an equivalent scenario-emission entry point T005/T006 establish) directly.

**Rationale:** every symbol above takes an already-fully-decided analysis/program artifact and
produces deterministic C11 text; none of them decode, discover, or decide instruction support, per
RULE 5's explicit prohibition on "codegen-driven discovery" (section 35.4).

---

## `platforms/genesis/runtime/`

Contract section 23 names `tools/genesis_startup_bridge_runtime.{h,c}` by exact path as the future
first-class runtime module, "not ordinary tooling."

**Current owner:** `tools/genesis_startup_bridge_runtime.h` (350 lines) and
`tools/genesis_startup_bridge_runtime.c` (775 lines), in full.

**Symbols that belong in `platforms/genesis/runtime/` (everything in both files):**
- ABI/state types: `GenesisOwnedCartridgeRegion`, `GenesisVdpDmaPhase`, `GenesisVdpDmaState`,
  `GenesisVdpState`, `GenesisDeviceState`, `GenesisRuntime`, `GenesisAccessWidth`,
  `GenesisAccessDirection`, `GenesisStopClass`, `GenesisCpuVariant`, `GenesisInstructionProvenance`,
  `GenesisBusKind`, `GenesisBusRegion`, `GenesisBusAccess`, `GenesisMappingClaim`,
  `GenesisDiagnosticCategory`, `GenesisProvenance`, `GenesisRuntimeStop`, `GenesisCpuDimensions`,
  `GenesisReportMetadata`, `GenesisControlTransferKind`, `GenesisControlTransfer`,
  `GenesisAccessResultKind`, `GenesisDispatchFunction`.
- Behavior: `genesis_route_access`, `genesis_internal_dispatch_inconsistency_stop`,
  `genesis_runtime_run`, `genesis_write_sanitized_report`, `genesis_write_full_report`, and every
  static helper in the `.c` file (memory access ABI, stop classes, diagnostic categories,
  provenance, control transfer, finite dispatch, memory/device routing, sanitized/full report
  writing — the exact list contract section 23 enumerates).

**Rationale:** this is the generated program's native runtime ABI, independent of the host C++
recompiler (RULE 6) — the contract's own section 23/27 distinction between this file pair and
`tools/genesis_startup_bridge.py` (orchestration tooling that *consumes* this ABI) is already
structurally true today; only the physical location (`tools/` instead of `platforms/genesis/runtime/`) needs
to move, per Phase 7.

**Note on device-vs-runtime split:** the VDP-specific and controller-I/O-specific portions of this
pair are *also* listed under `device/sega/genesis/` above, because the target tree's own
`runtime/components/sega/genesis/{bus.c,ram.c,io.c,vdp.c}` shape (section 25) further decomposes
`platforms/genesis/runtime/` into per-device component files once a second machine (Sega CD/32X) exists to
prove the reuse boundary. For this first migration (contract section 32's explicit "do not build
the whole future tree"), all of it lands as one `platforms/genesis/runtime/` module; the finer
`runtime/components/...` split is deferred exactly as section 32 instructs.

---

## `apps/segarecomp/`

Contract rule (section 30): "parse arguments, load files, choose workflow, invoke public
application/recompiler APIs, present results"; must not own instruction semantics, Genesis
controller policy, memory-map rules, frontier construction, or C lowering.

**Current owner:** `apps/segarecomp/main.cpp`, in full (427 lines).

**Symbols/logic that belong in `apps/segarecomp/`:**
- `print_usage`, `parse_hex`, `main` itself, and every CLI argument-parsing/dispatch block for
  `inspect`, `analyze`, `emit-c`, `emit-moveq-c`, `emit-direct-flow-c`, `m68k-frontend`/
  `emit-m68k-frontend-c`, `genesis-rom-startup`/`emit-genesis-rom-startup-c`/
  `genesis-general-startup`, `emit-general-startup-bridge-c`, `probe-genesis-startup-decode`,
  `probe-genesis-startup-mapping`.

**Logic that must move OUT of `main.cpp` when this module is created (contract section 30's own
prohibition list; currently violated by the present single-file CLI):**
- `kProbeMemoryContext` (a synthetic `M68kMemoryEmissionContext`) and
  `m68k_instruction_kind_name`/`probe_instruction_kind_name` — these are `codegen/c11`-shaped
  probe/presentation helpers, not argument parsing.
- The startup-state construction inside the `genesis-rom-startup` family's command body (already
  flagged under `machine/genesis/` above) — this is machine composition, not CLI dispatch.
- The mapping-claim construction inside `emit-general-startup-bridge-c`'s command body — also
  `machine/genesis/` composition currently inlined into the CLI.

**Rationale:** `main.cpp` today does far more than "parse arguments ... present results"; it
directly constructs Genesis machine state, decides probe support classification, and picks the
Genesis fixed-startup register defaults — every one of those is section 30's own named violation
("main.cpp knowing too much about CPU semantics, startup profiles, analysis composition, and
emission", section 6).

---

## `tools/bridge/` (and other `tools/*` destinations)

Contract section 27: `tools/genesis_startup_bridge.py` is orchestration tooling ("invoke segarecomp
-> generate C -> compile generated C + runtime -> execute binary -> validate canonical reports ->
compare runs / evidence"), target path `tools/bridge/genesis.py`, consuming the runtime library
rather than owning it.

**Current owner / target:**
- `tools/genesis_startup_bridge.py` -> `tools/bridge/genesis.py`. Already structurally
  runtime-consuming, not runtime-owning (it shells out to the built `segarecomp` binary and a C
  compiler; it embeds no runtime ABI logic itself) — only the physical path needs to move.
- `tools/sonic_startup_inventory.py` and `tools/inventory/stage1_classifier.py` (imported by it) —
  **resolved: tooling, staying under `tools/` (already at its target path, section 7 lists
  `tools/inventory/` explicitly).** The original version of this document listed this as an open
  T002 ownership question; it is not. `sonic_startup_inventory.py` already consumes segarecomp's
  own production CPU decode/mapping surface as its sole semantic authority rather than
  reimplementing it: every traced instruction/access is classified only through the real
  `probe-genesis-startup-decode`/`probe-genesis-startup-mapping` CLI probes (confirmed by this
  document's own author reading `tools/sonic_startup_inventory.py`'s module docstring and
  `_probe_decode`/`_probe_mapping` functions), never a scanner-private CPU/memory model.
  `stage1_classifier.py` must remain tooling-only normalization logic over those same probe
  results — it must never become a second decoder or support-policy implementation independent of
  `cpu/m68k`/`machine/genesis`. The dependency direction half of that guarantee (no `src/`/`include/`
  production file may import `tools.inventory`/`stage1_classifier`) is already independently
  enforced today by a dedicated regression, `tests/stage1_classifier_tooling_only_test.py`, not
  merely an intention; the semantic-authority half is established by `sonic_startup_inventory.py`'s
  own already-verified design (it classifies exclusively through the production
  `probe-genesis-startup-decode`/`probe-genesis-startup-mapping` CLI, never a private model).
  Nothing about this needs to change for T002 or any later SEG-014 task; it is not a pending
  decision.
- `tools/ghidra.py` — **resolved: unambiguous tooling**, not an architecture question at all. It is
  pure container/MCP lifecycle management (confirmed by reading the file: process/health-check
  orchestration only) with no segarecomp-pipeline semantic logic of its own; the target tree's own
  `tools/ghidra/` destination already matches. The original version of this document listed it
  alongside the genuinely-open `sonic_startup_inventory.py`/`stage1_classifier.py` question only
  because the issue's Scope asked every tool be addressed explicitly rather than silently assumed
  — that has now been done, and there is nothing ambiguous here to revisit.
- The private workflow/backlog tooling that once sat under `tools/` is explicitly
  outside the recompiler/runtime pipeline entirely and has since moved out of this repository; it is not
  named anywhere in the architecture contract's target tree because it is not part of it.

**Rationale:** section 27 is explicit that this Python driver "is architecturally different from
the runtime C files" and "should consume the first-class runtime library rather than own it" — true
today in substance, so this is a pure path move (`tools/genesis_startup_bridge.py` ->
`tools/bridge/genesis.py`), not an ownership change.

---

## `tests/*`

Contract section 31's suggested mature shape (`tests/unit/{core,cpu,device,machine}`,
`tests/integration/genesis`, `tests/differential/m68k`, `tests/generated_runtime`, `tests/bridge`,
`tests/cli`, `tests/fixtures`) does not yet exist; today all 59 files (including this task's own
`architecture_baseline_verifier_test.py`) are flat under `tests/`. Per section 31 and Phase 10
("reorganize tests where useful ... after production module boundaries stabilize"), this task
records only a category-level mapping, not a file-by-file plan:

| Category (future `tests/` subtree) | Representative current files |
| --- | --- |
| `unit/cpu/m68k` (host-only, no compiled-C execution) | `moveq_test.cpp`, `direct_flow_test.cpp`, `m68k_pipeline_test.cpp`, `m68k_frontend_rejection_test.cpp`, `tests/tools/*_test_harness.cpp` |
| `differential/m68k` (Musashi-oracle-backed) | `moveq_static_slice_test.py` (oracle branch), `m68k_batch_b_musashi_differential_test.py`, `m68k_batch_c_musashi_differential_test.py`, `tst_l_adversarial_test.py` (oracle branch) |
| `integration/genesis` (analysis-only CLI, no compile/execute) | `genesis_general_startup_cli_test.py`, `genesis_reset_image_cli_test.py`, `genesis_reset_image_adversarial_test.py`, `probe_genesis_startup_test.py`, `m68k_frontend_static_slice_test.py` |
| `generated_runtime` (compile-only or host-linked-runtime tests) | `genesis_startup_runtime_c2_test.py` .. `genesis_startup_runtime_vdp_test.py`, `genesis_startup_runtime_controller_io_test.py` |
| `bridge` (full generate/compile/link/execute via `tools/genesis_startup_bridge.py`) | `genesis_startup_bridge_c4_test.py` .. `genesis_startup_bridge_owned_cartridge_region_test.py`, `move_an_usp_bridge_test.py`, and this task's own `architecture_baseline_verifier_test.py` |
| `cli`/`unit/core`/`unit/device` (ingestion, tooling, restricted-path, backlog/PR policy) | `ingestion_cli_test.py`, `ingestion_adversarial_test.py`, `rom_test.cpp`, `ghidra_tool_test.py`, `restricted_files_test.py`, `stage1_classifier_test.py`, `stage1_classifier_tooling_only_test.py` |
| local-only (gitignored fixture required, `SKIP_RETURN_CODE 77`) | `sonic_first_unsupported_local_test.py`, `sonic_general_startup_controller_frontier_local_test.py`, `sonic_startup_inventory_*_test.py` |

**Rationale:** section 31's own text says do not reorganize "merely for cosmetic directory symmetry
if doing so breaks stable fixture/document references without value" — this category table exists
so T002+ can reorganize incrementally alongside each production module's own move, not as a
prerequisite big-bang test move.

---

## Ownership decisions and deferred seams identified by T001

**Renamed from "Unresolved ownership decisions before T002."** The original title implied every
item here blocks T002 from starting; it does not. T002's own scope is CPU-semantics-only (decode/
lift/IR/CCR/effects/control-flow) and does not touch Genesis-specific code or frontier/partial-
program composition at all, so most items below are either already resolved by this correction pass
or are deliberately scoped to a specific *later* SEG-014 child task, not T002. Nothing below should
be read as blocking T002 from starting.

### Resolved by this correction pass

- **MOVEQ and direct-flow are temporary compatibility facades, not permanent modules.** See the
  dedicated "Temporary SEG-002/SEG-003 compatibility surfaces" section above. `decode_moveq`/
  `lift_moveq` already delegate to the shared `decode_m68k_instruction`/`lift_m68k_instruction`;
  `emit_moveq`'s operation lowering already uses the shared `emit_m68k_operation_c`; `direct_flow.cpp`
  is already an explicit one-line-forwarder compatibility surface. Both standalone modules are slated
  for deletion by **SEG-014-T006** ("remove obsolete temporary forwarding facades"), not T002; T002
  may retain a thin, semantically-empty compatibility adapter if reviewability requires it.
- **Direct-flow C emission belongs to `codegen/c11`, not `cpu/m68k`.** `emit_m68k_direct_flow_c`/
  `emit_m68k_structured_direct_flow_c` were misattributed to `cpu/m68k/` in the original version of
  this document; corrected in both the `cpu/m68k/` and `codegen/c11/` sections above.
- **Genesis address-space routing belongs to `machine/genesis/`; device protocol belongs to
  `device/sega/genesis/`.** `m68k_route_genesis_device_access`, `M68kAbsoluteOperandRegion`,
  `m68k_resolve_absolute_test_operand`, and the synthetic-work-RAM window policy were misattributed
  to `device/sega/genesis/` in the original version of this document; corrected in both sections
  above, citing the contract's own `MachineAddressRouter` concept (section 13.3). Execution of the
  actual code move is **SEG-014-T004**'s scope, not T002's — T002 does not move Genesis-specific
  code at all.
- **`tools/sonic_startup_inventory.py`/`stage1_classifier.py`/`ghidra.py` are tooling, full stop.**
  No first-class module is warranted for any of them; see the `tools/bridge/` section above for the
  full reasoning (including the existing `stage1_classifier_tooling_only_test.py` regression). This
  was incorrectly left as an open T002 question in the original version of this document.
- **`ImageOffset` convergence direction.** Converge `MoveqImageOffset` (`moveq.hpp`) and the
  reset-image `ImageOffset` (`rom.hpp`) toward one `core/`-owned, fixed-width (`uint64_t`)
  `ImageOffset`; preserve compatibility aliases temporarily if needed; do not keep two semantically
  identical image-offset concepts permanently. T002 establishes the `core/` type (since it already
  relocates `MoveqImageOffset`); T004 (which relocates `rom.hpp`'s reset-image validator) should
  reuse it rather than keep a second definition. Structural cleanup only; no provenance/report
  behavior may change as a result.
- **`m68k_genesis_canonical_ea_address` is `cpu/m68k/` content, misnamed.** Its actual computation
  (absolute-word sign-extension to a 24-bit bus address) is generic MC68000 addressing-mode
  mechanics with zero Genesis-specific decision inside it; Genesis's own address-space *mapping* of
  the resulting bus address is a separate, already-distinguished `machine/genesis/` step. T002
  should rename it (e.g. `m68k_canonical_bus_address`) when it relocates `cpu/m68k` semantics.
- **`DecodeOutcome` is not a permanent `core/` type.** It stays attached to the temporary MOVEQ
  compatibility surface (deleted by T006) because it names a specific MC68000 mnemonic
  (`decoded_moveq`); the architectural decode-result API is the shared `M68kDecodeResult`/
  `RejectedM68kDecode`/`M68kDecodedInstruction` family, which already carries no such value. Old
  CLI/report strings that currently read from `DecodeOutcome` must still be preserved where required
  (contract section 34) — that is a wire-compatibility constraint on whichever task retires the
  MOVEQ facade, not a reason to keep `DecodeOutcome` itself as permanent architecture.

### Deferred to SEG-014-T003

- **`FrontendPartialProgram`/`UnresolvedFrontier`/`GenesisFrontierClass`/`M68kCpuFrontierKind` split
  between `cpu/m68k` and `recompiler/`.** Per contract section 20, "the MC68000 module may produce
  CPU-specific unresolved control-flow facts, but promotion into a machine-level partial program
  belongs above it" — today these are one undifferentiated type family entirely inside
  `m68k_pipeline.hpp`, populated only by `discover_m68k_general_startup` (which currently does both
  the CPU-discovery and Genesis-scenario-discovery jobs in one function). T003's own scope is
  exactly this: "Move MC68000 static block/edge discovery ... behind the T002 CPU boundary" plus
  "Promote partial-program/multi-exit/frontier and compilation-plan concerns that are not
  intrinsically MC68000 into an explicit `recompiler` owner." The concrete mechanical seam — whether
  `cpu/m68k/` emits a CPU-only unresolved-fact type (e.g. a `CpuFrontier` per section 20's own
  suggested vocabulary: `CpuFrontier`/`DeviceFrontier`/`ControlFlowFrontier`/`MappingFrontier`/
  `SchedulingFrontier`) that `recompiler/` then promotes into today's `UnresolvedFrontier`/
  `GenesisFrontierClass`, and whether `M68kCpuFrontierKind` is the CPU-only half of that split — is
  T003's decision to make with real code in front of it, not this document's to pre-decide.
- **`M68kFrontendProfile`'s CPU-discovery-policy vs. Genesis-workflow-scenario split.** The enum
  mixes `direct_flow` (a CPU-route selector) with `genesis_rom_startup`/`general_startup` (Genesis
  workflow/scenario selections); this mixing is itself evidence the enum is not a single clean
  ownership unit. T003 begins the split per its own scope ("move startup/scenario profile
  composition upward where it represents a Genesis/application workflow rather than ISA semantics");
  **SEG-014-T004** completes it by landing the Genesis-scenario half in `machine/genesis/` alongside
  the rest of Genesis workflow composition.

Nothing else in this document is left open: every other symbol above has one stated target module
and rationale, and every classification change made by this correction pass is marked inline where
it occurs.
