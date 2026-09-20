# Generalized-startup partial-program and persistent Genesis runtime ABI contract (SEG-007-T028)

## Purpose and boundary

This is a documentation-only architecture contract. It defines the seam a later implementation task
(SEG-007-T029) must build, precisely enough that T029 needs no further architecture decision. It
introduces no code, fixture, or test, and changes no `src/`, `include/`, `tests/`, `tools/`, or
build-system file.

It consumes only already-committed evidence:

- SEG-007-T026's independently validated result: the former `TST.W absolute_long` CPU-form frontier
  is closed; the authorized hash-pinned `segarecomp genesis-general-startup <rom>` production route's
  current sanitized stopping result is domain **device** (Genesis bus/memory), category
  `unsupported_device_region_controller_io` — a controller-I/O access shape outside SEG-007-T021's one
  narrow implemented selector.
- SEG-007's own architecture guardrail, reaffirmed by [the controller-I/O compatibility
  policy](genesis-controller-io-startup-read-compatibility-policy.md): controller semantics must never
  live in `test_absolute_long`, a `TST.L`-specific emitter case, or any other single-instruction-kind
  special case; they have exactly one ownership seam (`statically lifted CPU memory operation` →
  `Genesis bus/address resolver` → `controller-I/O device read` → `typed device result or typed
  fail-closed result`).
- [The MC68000 pipeline migration contract](m68k-pipeline-migration-contract.md) and [the Genesis
  startup shared-route ownership contract](genesis-startup-shared-route-ownership-contract.md), whose
  "no startup-only bypass," "no target-byte fetch/decode," and "one shared owner" rules this bridge
  extends rather than relitigates. The latter documents the original selected forms' "2-/6-byte spans";
  ADR 0008 governs the current shared general-startup route-provenance capacity separately.
- The current-repository facts below (`include/segarecomp/m68k_pipeline.hpp` and
  `src/m68k_pipeline*.cpp`; exact line numbers may drift, the cited names and shapes do not):
  - `FrontendResult` is `std::variant<FrontendAnalysis, FrontendRejected>`. `FrontendRejected` carries
    no `decoded`/`ir`/`static_blocks`/`static_edges`/`static_frames` field, so a failed
    `discover_m68k_general_startup` call today discards the `FrontendAnalysis` prefix it had already
    accumulated.
  - No CLI command emits C for the `general_startup` profile. `emit_m68k_frontend_c` special-cases only
    `M68kFrontendProfile::genesis_rom_startup` (a fixed five-operation, single-shot, non-persistent
    generated `main`) and otherwise falls through to `emit_m68k_structured_direct_flow_c(analysis.
    direct_flow, analysis.units, ...)`, a function `discover_m68k_general_startup` never populates
    (`analysis.direct_flow`/`analysis.units` stay default-constructed on that route).
  - `execute_m68k_frontend_startup` (`src/m68k_pipeline.cpp`) is a host-side C++ interpreter used only
    by the legacy fixed-graph `genesis_rom_startup` route. It is not generated-native-program execution
    and is not wired to `general_startup`.
  - Controller-I/O values are today resolved and folded at translation time only:
    `m68k_resolve_absolute_test_operand`, `M68kAbsoluteTestOperand`, and
    `M68kMemoryEmissionContext::test_operand_value` are documented as "the already-verified constant
    value to fold." `m68k_route_genesis_device_access`/`m68k_controller_io_access` are host-side C++
    functions invoked during static analysis, not functions the generated C program calls at runtime.
    There is no runtime memory/device routing boundary that generated C calls into today.
  - No `GenesisRuntime`/persistent-runtime-context type, precompiled-PC dispatcher across independently
    generated units, or compile-and-run driver exists for the generalized route. The only
    `dispatch(...)` in the repository is the private, single-program `int dispatch(uint32_t pc,
    uint32_t d[8], uint16_t *sr, uint32_t *next_pc, const char **block_json, const char **edge_json)`
    state machine `emit_m68k_structured_direct_flow_c` (the `direct_flow` backend) generates inside one
    program's own output; it has no persistent runtime context and no cross-unit contract.
  - `StartupState` (`d[8]`, `a[8]`, `sr`, `pc`) is today only a host-side C++ struct consumed by
    `execute_m68k_frontend_startup`'s interpreter loop. It is not generated-C-visible and is not shared
    across multiple generated translation units. `StartupBoundary`, `StartupFailure`, and
    `StartupExecution` are its accompanying host-side report types, used only by that interpreter's
    report schema.
  - `DirectFlowDiagnostic` (`include/segarecomp/m68k_pipeline.hpp`) is the single existing category
    taxonomy already covering `rom_write_prohibited`, `unmapped_data_access`,
    `unsupported_device_region_controller_io`, `unsupported_instruction_form`,
    `reached_unresolved_direct_edge`, `startup_graph_mismatch`, `discovery_budget_exhausted`,
    `return_context_missing`, and every other currently defined rejection/frontier category.
  - `M68kMemoryAccessWidth` is `enum class M68kMemoryAccessWidth { byte = 1, word = 2, long_word = 4 }`
    and `M68kMemoryAccessDirection` is `enum class M68kMemoryAccessDirection { read, write }` (`read =
    0`, `write = 1`), both with explicit or default-sequential integer values already fixed by the host
    source. `M68kMemoryAccessRequest` (`address`, `width`, `direction`, an optional
    `source_provenance`) is the existing host-side struct already carrying exactly this data at every
    call site that resolves a memory/device access; `FrontendRejected` has no width/direction field of
    its own today.
  - `discover_m68k_general_startup`'s recursive `walk` function today rejects an `RTS` reached with an
    empty call-frame stack (`frame_stack.empty()`) as `return_context_missing`, unconditionally — an
    existing, stable, well-defined host-side production behavior with exactly one call site
    (`src/m68k_pipeline_frontend.cpp`), unrelated to and predating this bridge. An empty `frame_stack`
    at that point means only that no `JSR`/`BSR` is currently open on this discovery pass's own explored
    path; it proves nothing about whether an external caller exists in reality (for example a reset
    entry has no caller at all, so this condition is not evidence of "an unmodeled caller returning
    successfully" — it is simply an unresolved return with no known valid target, exactly as
    `return_context_missing`'s existing category name and behavior already state). `execute_m68k_
    frontend_startup` (the unrelated legacy interpreter) also produces a same-named
    `"return_context_missing"` string category for its own, separate runtime call-stack mismatch check
    (`src/m68k_pipeline.cpp`); that is a different code path with a different trigger and is not touched
    by this contract. **This contract does not change this behavior for ordinary production discovery.**
  - `walk` terminates one exploration branch successfully (returns `true`, contributing no rejection)
    only via its `visited_states` cache: `if (!visited_states.insert(state_key).second) return true;`
    — i.e. today's only route to a fully accepted `FrontendAnalysis` is a closed, cyclic control-flow
    graph (for example a self-looping branch), never a graph with a genuine, non-looping exit.
  - `execute_op`/`execute_m68k_frontend_startup`'s existing RTS handling already reads the 32-bit return
    target from stack memory, validates it against the call's own expected continuation, increments the
    stack pointer by four, and sets `PC` to the validated target — rejecting with `return_target_mismatch`,
    `invalid_stack_alignment`, or `invalid_stack_range` on failure, per [the Genesis startup shared-route
    ownership contract](genesis-startup-shared-route-ownership-contract.md)'s documented "RTS pop/
    validate/dispatch sequence." This bridge's own RTS lowering reuses this same existing pattern rather
    than inventing a second one.

## Revision history

- **2026-09-01 (ADR 0015 / SEG-007-T142).** Added the compatible, stop-owned
  `c4_lowering_dimensions` report field. `schema_version` remains `1`.

- **Initial delivery.** Defined all twelve required elements, but (a) used C++-only host types
  (`FrontendRejected`, `enum class GenesisFrontierClass`, undefined `GenesisAccessWidth`/
  `GenesisAccessDirection`) directly inside "generated C" snippets, which cannot compile as C11; (b)
  let a runtime frontier absorb malformed/truncated/ambiguous-provenance conditions that must always be
  build-time rejections; (c) implied the T021 controller-I/O selector could succeed through the new
  runtime routing boundary, reintroducing device semantics into T029's controller-free scope; (d) left
  `FrontendPartialProgram`'s composition, the ROM-hash binding, and the compile-and-run driver's wire
  protocol underspecified enough that T029 would still have had to invent them.
- **Second revision** corrected all four defects: §1 separated host-side C++ discovery types from a
  genuinely strict-C11 runtime ABI with one explicit lowering table; §7 restored build-time-rejection
  precedence for every unsafe condition regardless of an already-accepted prefix; §6 made every
  T029 device-region access unconditionally fail closed; §1 gave `FrontendPartialProgram` an exact
  composition; §10 operationalized the ROM-hash binding; §13 fully specified the driver/report
  protocol.
- **Third revision** closed three further gaps: (1) no representation existed for a fully accepted
  program's *successful* completion — only `GENESIS_CONTINUE_AT_PC`/`GENESIS_STOP` and a `"stop"`-only
  wire result existed; a `GENESIS_COMPLETE` outcome was added. (2) §13's earlier commercial-mode
  cross-invocation comparison flag asked the caller to re-supply a prior run's temporary file that the
  same design also required to be deleted when that prior run ended — an impossible, self-contradictory
  lifecycle; §13 replaced it with `--compare-runs`, one driver invocation that launches and compares
  both runs itself over anonymous, in-process pipes, with no temporary file ever created for the
  comparison and no caller-re-supplied-report-path flag of any kind. (3) `GenesisProvenance` covered
  only a hand-picked subset of the host provenance actually available; §2.1 added a complete,
  field-by-field disposition table over every relevant field of `FrontendRejected`, its nested
  `InstructionProvenance`/`RejectedDirectFlow`/`mapping_claims`/`accesses`, and
  `UnresolvedFrontier.access`, so no field is left for T029 to silently keep or drop. That revision's
  (1) had a defect a subsequent review found and this revision corrects: see below.
- **This (fourth) revision** corrects `GENESIS_COMPLETE`'s design, which the third revision had gotten
  wrong: it unconditionally reclassified every `return_context_missing` (an `RTS` reached with an empty
  discovery call-frame stack) as successful completion, suppressed that `RTS`'s real stack-pop/PC/A7
  effect, and recorded completion only by block entry address. This was invalid: an empty call-frame
  stack proves nothing about an external caller (a reset entry, for instance, never has one);
  reclassifying `return_context_missing` unconditionally weakened an existing, unrelated diagnostic
  category's precedence, which this contract and SEG-007-T029 both prohibit; suppressing the RTS's real
  effect invented instruction-specific compatibility behavior with no hardware or project-policy
  grounding; and a block-address-only completion record loses call context if the same physical block
  is reachable from more than one static call context. §1.4 now defines an explicit, opt-in
  `FrontendCompletionContract`/`FrontendProgram.synthetic_completion` declaration, used only by the
  project-authored synthetic bridge-validation fixture; §3 now executes the declared terminal `RTS`
  with its full, real stack-pop/A7-increment/PC-set effect, exactly like every other `RTS`, and produces
  `GENESIS_COMPLETE` only when the validated, popped return target equals the declared sentinel among
  the finite set of statically authorized targets — never by block identity alone, so the same physical
  block safely serves both a normal call context and the completion context. An `RTS` reached with an
  empty call-frame stack and no matching declared completion contract continues to produce
  `return_context_missing` exactly as unmodified production behavior on `main` already does. No element
  of the original twelve-point outcome, T028's scope, or any prior round's other preserved decisions
  (pure-C11 ABI separation, `"completed"`/`"stop"` wire discriminant, build-time-rejection precedence,
  unconditional T029 device fail-closed behavior, complete provenance disposition, ROM option (a) and
  SHA-256 binding, `--compare-runs` anonymous-pipe behavior, artifact structure, driver ownership,
  Musashi boundary, runtime-decoder prohibition, folding-versus-routing precedence) changed in kind —
  only the completion-semantics flaw above was corrected. (A sixth review then found one leftover
  contradictory sentence from the removed design immediately after §7's diagnostic-category table,
  fixed in the same round; a seventh review confirmed the fix and found no other leftover.)
- **This (eighth) revision** closes the final driver-to-emitter integration gap the fourth/fifth rounds
  left open: §1.4 defined `FrontendProgram.synthetic_completion`, but neither `tools/genesis_startup_
  bridge.py` nor its `emit-general-startup-bridge-c` invocation exposed any argument that could
  construct a `FrontendCompletionContract`, so the accepted synthetic completion fixture could not
  reach it through the one required project-owned driver at all — it would have had to bypass the
  driver or invent an undocumented argument. §13.1 adds the driver's paired, synthetic-only
  `--completion-rts`/`--completion-sentinel` options (both-or-neither; synthetic-mode-only; exit code 8
  otherwise); §13.2 adds the matching emitter surface (`--synthetic-completion-rts`/`--synthetic-
  completion-sentinel`, paired, its `synthetic-` prefix normative) and the exact forwarding mapping, and
  states that the accepted fixture must use this exact command through the driver, never a private
  bypass; §13.4 documents exit code 8's expanded meaning; §9 adds the driver-level validation cases.
  The driver performs no semantic validation of its own beyond well-formed-address syntax; every
  architecture-level invariant remains exactly §1.4's existing responsibility. No completion semantics,
  no prior round's decision, and no file beyond the two documentation files changed.
- **This (ninth) revision** generalizes §1.1's `FrontendPartialProgram.frontier` (exactly one
  `UnresolvedFrontier`) into `frontiers` (a bounded, deduplicated `std::vector<UnresolvedFrontier>`,
  1..`m68k_discovery_max_frontier_exits`), for the reason recorded in SEG-007-T064's own Evidence:
  generalizing the single-frontier representation ahead of real-ROM validation, per SEG-007-T063's
  inability to promote any `FrontendPartialProgram` for the pinned Sonic ROM under the single-exit
  design, with the multi-exit representation validated first against project-authored synthetic
  fixtures in that same task before any real-ROM re-execution was attempted. §1.1 also adds the named
  bound constant and the deterministic ordering/deduplication rule (sorted by `(source address, target
  address or zero, class ordinal)`, deduplicated on that identical tuple); exceeding the bound fails the
  whole promotion closed under "no safe frontier stub available" (§7's existing general rule), never a
  silent drop of the excess exit(s). §3/§4's generated dispatcher/block-tail lowering gains one
  `genesis_frontier_stop`-family comparison line per retained exit, emitted inside the one block whose
  outgoing edge reaches it (at most two per block, since a block's terminal instruction has at most two
  outgoing edges) — multi-exit dispatch happens entirely inside each block function's own tail, never
  inside `genesis_dispatch` itself, so this introduces no second control-transfer mechanism. §7's
  `known_but_unemitted_target` row becomes producible for the first time, for a best-effort sibling exit
  whose target address is known but whose deeper diagnostic cannot be losslessly retained as one of the
  four precise classes; its own diagnostic category is host-side bookkeeping only and is never lowered
  to the pure-C ABI for this class, which instead always emits the literal
  `GENESIS_DIAG_KNOWN_BUT_UNEMITTED_TARGET`/`GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET` pair. §13.3's
  static CLI report (the `genesis-general-startup` route's own non-wire diagnostic text) becomes a JSON
  array of retained exits; the compiled bridge binary's own sanitized wire report is unchanged in shape,
  since a running program only ever reports the one exit it actually dispatched to at runtime, never the
  full static exit set — these are two distinct report shapes and this revision does not conflate them.
  No other element of §1-§14 changed in kind — the pure-C11 ABI/host separation, build-time-rejection
  precedence, unconditional device fail-closed behavior, the ROM-hash binding, `--compare-runs`
  behavior, the Musashi boundary, and the runtime-decoder/second-control-transfer-mechanism prohibition
  are all unchanged.
- **This (tenth) revision** (SEG-007-T064's own Checkpoint 7, added by explicit operator direction after
  the ninth revision above was already complete and independently adversarially validated) refines,
  rather than removes, §14's runtime-stop privacy boundary: see ADR 0004
  (`docs/decisions/0004-generic-frontier-classification-privacy-boundary.md`) for the full rationale.
  §14 gains two new, narrowly-derived, present-only-for-their-own-applicable-`stop_class` classification
  fields — `opcode_line` (a standard, publicly documented MC68000 top-4-bit opcode-line name, for
  `GENESIS_STOP_UNSUPPORTED_CPU_FORM` only) and `region_class` (one of this project's own already-cited,
  already-implemented address-space region buckets, for `GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS`/
  `GENESIS_STOP_UNSUPPORTED_MEMORY_REGION` only) — derived only from fields the frontier representation
  already legitimately holds, never a new raw address/byte/opcode/offset value and never a new,
  not-yet-independently-cited hardware region name. §7's diagnostic-category table rows for these three
  classes are updated to note the new field each may now carry. No other element of §1-§14 changed in
  kind, and no other prior round's decision (pure-C11 ABI separation, build-time-rejection precedence,
  unconditional device fail-closed behavior, ROM-hash binding, `--compare-runs` behavior, the Musashi
  boundary, the runtime-decoder/second-control-transfer-mechanism prohibition, or every other already-
  sanitized field's own continued exclusion) changed.
- **This (eleventh) revision** (SEG-007-T064's own Checkpoint 8, added by explicit operator direction
  after the tenth revision above was already complete and independently adversarially validated, PASS)
  corrects a scope error an independent adversarial review found in the tenth revision immediately above:
  see ADR 0005 (`docs/decisions/0005-cli-static-report-outside-wire-report-privacy-boundary.md`). §14
  governs, and has only ever governed, the compiled bridge binary's own wire report (§13.3); it does not,
  and never did, govern the CLI's own static discovery-time report (`genesis-general-startup`), which has
  carried full raw provenance unconditionally, by default, with no sanitized/full split of its own, for a
  plain `FrontendRejected` result since long before SEG-007-T064 existed. The tenth revision's own text
  did not make this distinction clearly enough, permitting a reading that §14 covered the CLI report too;
  this revision corrects that reading. `opcode_line`/`region_class` (tenth revision, ADR 0004) and the
  raw-provenance fields Checkpoint 8 itself adds to the CLI report's `partial` branch (restoring
  consistency with that same report's own pre-existing `rejected`-branch behavior) both live in the CLI's
  own static report, outside §14's actual governance; §14's own restriction, for the one report surface
  it has always actually governed (the compiled bridge binary's own wire report), is completely unchanged
  and exactly as strict as before. No other element of §1-§14 changed in kind.
- **This (twelfth) revision** (SEG-007-T077, ADR 0006) narrows §10's chosen option (a) with a scoped
  carve-out: a runtime-computed, non-constant-foldable effective address reaching §6's boundary for a
  **read** may resolve against a statically owned, generated, build-time-embedded copy of a `Mapping
  Claim` region discovery already proved, instead of unconditionally failing closed. This closes a
  documentation/implementation drift ADR 0006's own Context records: §6's prior text overstated that "no
  accepted generated operation may ever call `genesis_route_access` for a ROM read," which SEG-007-T067
  through SEG-007-T072's own generalization of `write_move`/`write_movea`/`test_operand`/`write_clr`/
  `logical_and_immediate`/`movem_transfer` onto register-indirect/postincrement/predecrement/`d16(An)`
  effective-address families had already made false in practice (every one of those forms is
  unconditionally routed through §6 at runtime regardless of what address it resolves to, matching §11's
  own existing "not proven statically constant" routing rule) -- SEG-007-T074/T075/T076's own evidence is
  the proof. §6 gains a fourth recognized region kind (a bounds-checked, read-only, generated cartridge-
  data region) with the exact proof obligation ADR 0006 states in full: region mapping/bounds from
  `MappingClaim`, implicit ownership/immutability from `raw_cartridge_rom` construction, hardware-window
  containment (`target_end <= 0x00400000`), and the generated backing data itself -- never the exact
  effective address or value at translation time, distinct from §11's own narrower scalar-constant-fold
  precedent. No C4 lowering call site changed; no second control-transfer mechanism, no new frontier
  representation kind, and no restructuring of `genesis_dispatch` was introduced. Restated as unchanged:
  the generated program never reads the original source ROM file/image at runtime under any revision,
  never fetches or decodes an instruction at runtime under any revision, a ROM write remains
  unconditionally `GENESIS_DIAG_ROM_WRITE_PROHIBITED`, and every read outside every proven region/window
  remains exactly as fail-closed as before this revision. No other element of §1-§14 changed in kind.

## 1. Partial-program representation

### 1.1 Host-side discovery types (C++, never emitted into generated C)

`FrontendResult` becomes a three-alternative variant:

```cpp
using FrontendResult = std::variant<FrontendAnalysis, FrontendPartialProgram, FrontendRejected>;
```

`FrontendAnalysis` (a fully accepted, budget-satisfied program) and `FrontendRejected` (a build-time
reject with no accepted prefix, defined in §7) are unchanged in meaning. `FrontendPartialProgram` is
new, with an exact composition — not a flattened restatement of `FrontendAnalysis`'s fields:

```cpp
struct FrontendPartialProgram {
  FrontendAnalysis accepted_prefix;
  // SEG-007-T064: a bounded, deduplicated, deterministically ordered set of
  // one or more retained runtime exits, replacing this struct's original
  // single `UnresolvedFrontier frontier` field. Always
  // 1..m68k_discovery_max_frontier_exits entries (see the named bound below);
  // exceeding the bound fails the whole promotion closed under "no safe
  // frontier stub available" (§7's general rule), never a silent truncation.
  std::vector<UnresolvedFrontier> frontiers;
};

// SEG-007-T064: bounds the number of distinct retained runtime exits a
// single FrontendPartialProgram may carry
// (`include/segarecomp/m68k_pipeline.hpp`,
// `m68k_discovery_max_frontier_exits`). This is an internal engineering
// bound on this project's own bounded multi-exit representation, exactly
// like `m68k_discovery_max_instructions`/`_blocks`/`_call_frame_depth` --
// it is not a Genesis hardware or timing fact and carries no hardware
// citation. Starting value: 4 (see the header's own comment for the
// doubling-search-style justification against SEG-007-T064's own synthetic
// Checkpoint 4 fixtures).
inline constexpr std::uint32_t m68k_discovery_max_frontier_exits = 4U;

// SEG-007-T064: `frontiers`' deterministic ordering/deduplication rule.
// Every candidate exit is reduced to the tuple
// `(diagnostic.provenance->source.address.value, target_address_or_zero,
// static_cast<int>(class_))`, where `target_address_or_zero` is
// `access->address.value` when `class_` is `unsupported_device_access` or
// `unsupported_memory_region`, and `0` otherwise. `frontiers` is sorted
// ascending by this tuple; two exits reducing to the identical tuple are the
// same exit and are retained once. Two exits that share a source address but
// differ in target or class are two genuinely distinct exits and are both
// retained (subject only to the bound above) -- this contract does not
// invent a "first-classified-wins" or "reject-on-conflicting-class" rule for
// that shape, since the tuple key already treats them as different exits.

enum class GenesisFrontierClass {
  unsupported_cpu_form,
  unsupported_device_access,
  unsupported_memory_region,
  unresolved_indirect_target,
  known_but_unemitted_target,
  unsupported_interrupt_or_scheduling_event,
};

struct UnresolvedFrontier {
  GenesisFrontierClass class_;   // coarse discovery-time classification of *why* discovery stopped
  FrontendRejected diagnostic;   // reuses FrontendRejected's existing provenance/category/address
                                  // field set verbatim; this is the sole host-side diagnostic/
                                  // provenance carrier for source-instruction facts, adding no
                                  // second provenance shape for those fields
  std::optional<M68kMemoryAccessRequest> access;  // populated only when class_ is
                                                    // unsupported_device_access or
                                                    // unsupported_memory_region: FrontendRejected
                                                    // has no width/direction field today, so the
                                                    // exact M68kMemoryAccessRequest discovery
                                                    // already held at the point of resolving the
                                                    // access is carried here directly, unmodified.
                                                    // Its own optional source_provenance must be
                                                    // set to the exact same value as diagnostic.
                                                    // provenance (never a second, independently
                                                    // derived InstructionProvenance) -- see §2.1
};
```

`accepted_prefix` is a complete, independently valid `FrontendAnalysis` value, subject to these
invariants:

- `accepted_prefix.static_blocks.size() >= 1` (at least one fully accepted block; with zero, §7 always
  produces `FrontendRejected` instead — see "no safe frontier stub available").
- Every entry in `accepted_prefix.decoded`/`accepted_prefix.ir`/`accepted_prefix.static_blocks`/
  `accepted_prefix.static_edges`/`accepted_prefix.static_frames` was fully decoded, lifted, and
  provenance-validated exactly as a fully accepted `FrontendAnalysis`'s entries are today — a
  `FrontendPartialProgram` never contains a partially decoded or partially validated entry.
- The operation that produced `frontier` is **not** included anywhere in `accepted_prefix` as if it
  were successfully decoded, lifted, or emitted content; it is represented exclusively by
  `frontier.diagnostic`'s provenance fields.
- `frontier.diagnostic.category` (a `DirectFlowDiagnostic` value, for example
  `unsupported_device_region_controller_io`) reuses the one existing category taxonomy; `class_` adds
  only the coarser runtime-frontier classification §7 needs, never a second provenance representation.
  Exception: for `class_ == known_but_unemitted_target`, no current `DirectFlowDiagnostic` value means
  "a valid target known but not emitted in this pass" (§7 states the pure-C-only category this uses
  instead).

`discover_m68k_general_startup` returns `FrontendPartialProgram` exactly when discovery reaches a §7
runtime-frontier category with at least one already-accepted `static_block` and every invariant above
holds; it returns plain `FrontendRejected` in every other case (§7 general rule).

### 1.2 Generated/runtime pure-C11 ABI types (§2–§6; never contain a C++ type, `std::` name, or
### `enum class`)

Every type the generated program or its fixed runtime-support file uses is standalone C11: fixed-width
integer types (`<stdint.h>`), plain C `enum`/`struct` with explicit stable integer values, fixed-size
arrays, and explicit `uint8_t has_*`/count presence fields in place of `std::optional`/`std::vector`.
§1.3 states the one explicit lowering from §1.1's host types into this ABI.

### 1.3 Lowering rule (the single explicit mapping between §1.1 and §1.2)

Every value that crosses from a host-side C++ discovery type into generated C source text or the wire
report defined in §13 goes through exactly one correspondence: this document's `GenesisDiagnosticCategory`/
`GenesisStopClass`/`GenesisAccessWidth`/`GenesisAccessDirection`/`GenesisBusKind`/`GenesisBusRegion`/
`GenesisCpuVariant` tables (§7's diagnostic-category table, §2's stop-class table, and §2.1's
provenance-field tables). SEG-007-T029's C++ emitter (the implementation of the new
`emit-general-startup-bridge-c` CLI subcommand, §13) is the only code that ever consults this
correspondence: it reads a host-side `UnresolvedFrontier` and emits literal C initializer syntax
referencing the matching pure-C enumerator name and literal `has_*`/count/value fields for
`GenesisProvenance` (§2.1), taken from `frontier.diagnostic`'s existing optional fields for
source-instruction provenance and from `frontier.access` (§1.1's `M68kMemoryAccessRequest`, present
only for a device/memory-access frontier) for the `access_address`/`access_width`/`access_direction`
fields `FrontendRejected` itself has no field for. No function in the generated program or its
runtime-support file ever performs this conversion at runtime, because the generated program never
contains or links against a host C++ analysis type in the first place; a runtime-detected stop (§6's
device-routing failures, §4's dispatch-inconsistency stop, §7's budget-exhaustion stop) instead
constructs its `GenesisRuntimeStop` directly from the pure-C enumerators, with no host C++ type ever
involved.

This contract forbids ever emitting a raw C++ enum ordinal (for example
`static_cast<int>(DirectFlowDiagnostic::unsupported_device_region_controller_io)`) directly into
generated C or the wire report. Every value crossing the host/generated or wire boundary uses this
document's pinned pure-C enumerator name and explicitly assigned integer value (§2, §2.1, §7), never
the host enum's ordinal position, which is an implementation detail that could silently change if
`DirectFlowDiagnostic`'s declaration order ever changes.

### 1.4 Explicit synthetic completion contract

`GENESIS_COMPLETE` (§2, §3) exists only for the project-authored synthetic bridge-validation fixture,
authorized exclusively through one explicit, opt-in declaration supplied as discovery *input* — never
inferred from `return_context_missing`, an empty call-frame stack, or any other error category. Ordinary
production discovery (any `general_startup` run without this declaration) is completely unaffected: an
`RTS` reached with an empty call-frame stack still produces `return_context_missing` exactly as
unmodified behavior on `main` already does (Purpose section), with no change to its category, provenance,
precedence, or any other existing route's behavior.

```cpp
struct FrontendCompletionContract {
  M68kProgramAddress terminal_rts_address;   // must identify a fully decoded and lifted RTS
                                              // terminating a discovered static block
  M68kProgramAddress sentinel_return_pc;     // must be a valid even 24-bit address; see the
                                              // build-time-provable invariants below
};

struct FrontendProgram {
  // ...existing fields unchanged (cpu_variant, image, mapping_claims, analysis_entries, profile,
  // startup_ingress)...
  std::optional<FrontendCompletionContract> synthetic_completion;
};
```

`synthetic_completion` is accepted only for the `general_startup` bridge's project-authored synthetic
mode; it is harness metadata a test fixture supplies to `discover_m68k_general_startup`, never a
hardware fact and never inferred from any diagnostic category. Commercial mode and every ordinary
production `FrontendProgram` simply never populate this field — there is no implicit default and no
separate "commercial completion" concept. §13.1 defines the driver's paired `--completion-rts`/
`--completion-sentinel` options as the one project-owned CLI surface that can construct a
`FrontendCompletionContract`; those options are accepted only with `--mode synthetic` and are rejected
outright, before any hashing, generation, compilation, or execution, if supplied with `--mode
commercial` (§13.1's own exit code). §13.2 defines the one matching emitter surface,
`--synthetic-completion-rts`/`--synthetic-completion-sentinel` on `emit-general-startup-bridge-c`, which
the driver forwards only in synthetic mode and never in commercial mode. No commercial invocation this
contract authorizes can ever produce a populated `synthetic_completion`.

**Build-time-provable invariants**, checked by `discover_m68k_general_startup` when `synthetic_
completion` is present and the current traversal reaches an `RTS` at `terminal_rts_address` with an
empty call-frame stack (every other empty-call-frame-stack `RTS`, at any other address, is entirely
unaffected and still produces `return_context_missing`):

- `terminal_rts_address` must equal the source address of a fully decoded and lifted `RTS` operation
  that terminates one of this discovery pass's own `static_blocks` (i.e. the declaration must name a
  real instruction this exact discovery run actually reaches and accepts, not an arbitrary address).
- `sentinel_return_pc` must be an even, 24-bit-clean address (the same validity class every other direct
  target in this pipeline already requires).
- `sentinel_return_pc` must not overlap any mapped image interval (`mapping_claims`), any discovered
  instruction's own address span, any discovered block entry, any known static-call continuation, or any
  runtime device/memory address `genesis_route_access` (§6) could route as executable code. This is a
  defensive safety margin: the design in §3 never dispatches to `sentinel_return_pc` as a PC (reaching it
  short-circuits directly to `GENESIS_COMPLETE` before any dispatch is attempted), but the sentinel must
  still be provably distinct from every real address this discovery pass or this bridge's routing
  boundary could otherwise treat as live code or data, so it can never be confused with one.
- If any of the above cannot be proven at discovery time, the whole result is a build-time
  `FrontendRejected` under "no safe frontier stub available" (§7) — the same disposition an
  unrepresentable §2.1 provenance field already uses, never a runtime possibility and never a silent
  fallback to ordinary `return_context_missing` handling for a *declared* but invalid contract.

**Completion record.** On success, `FrontendAnalysis` gains one new optional field, populated only when
`synthetic_completion` was supplied and validated (never for ordinary production discovery, and never
reduced to a block address alone — per this section's own controlling requirement):

```cpp
struct FrontendCompletionRecord {
  InstructionProvenance terminal_rts;       // the declared RTS's full source/address/raw-byte/length
                                             // provenance, not merely its block's entry address
  M68kProgramAddress sentinel_return_pc;    // the validated sentinel, carried alongside the RTS's own
                                             // provenance so call context is never lost
};

struct FrontendAnalysis {
  // ...existing fields unchanged, plus §1.1's mapping_claims/units/static_blocks/static_edges/
  // static_frames/startup_ingress...
  std::optional<FrontendCompletionRecord> completion;
};
```

Discovery continues exploring normally past the declared `RTS`'s own block (this one block contributes
no further successor edge for the empty-call-frame-stack traversal state specifically, since that state
is now a validated, provable terminal boundary rather than a rejection) while every other traversal
state, including a real call reaching this exact same physical block (§4), is completely unaffected and
continues to be explored and validated exactly as it already is today.

## 2. Persistent Genesis runtime ABI

A named generated-C-visible runtime context is declared once in the fixed, non-generated runtime-support
header §12/§13 names, `tools/genesis_startup_bridge_runtime.h`. Its persistent CPU-state portion is:

```c
typedef struct GenesisRuntime {
  uint32_t d[8];
  uint32_t a[8];      /* a[7] is the architectural stack pointer (A7/SSP) */
  uint32_t usp;       /* persistent User Stack Pointer; SEG-007-T085 policy */
  uint16_t sr;
  uint32_t pc;
  uint8_t  work_ram[65536];  /* persistent 64 KiB Genesis work RAM */
} GenesisRuntime;
```

The runtime header may append separately documented machine/device fields; this excerpt does not claim
to be its complete layout. Every generated function shares one `GenesisRuntime` instance by pointer; no
generated function owns a private copy of D/A/USP/SR/PC/RAM. This contrasts explicitly with today's `StartupState`: `StartupState` is
a host-side C++ struct consumed only by `execute_m68k_frontend_startup`'s interpreter loop, has no
persistent RAM member, and is never emitted into generated C or shared across independently generated
units. `GenesisRuntime` is this bridge's generated-C-visible superset, matching `StartupState`'s
register field names and widths so the two remain trivially comparable, and adding the persistent RAM
byte array `StartupState` has never had.

The remaining pure-C11 ABI types, used by §3–§6. These are presented in expository order; the actual
`tools/genesis_startup_bridge_runtime.h` (§12) must declare `GenesisDiagnosticCategory` (§7's table)
and every §2.1 provenance type before `GenesisRuntimeStop` below, which references them, since C
requires a type's declaration to precede its use:

```c
typedef enum GenesisAccessWidth {   /* numerically identical to the host M68kMemoryAccessWidth
                                        values, which are already explicit; this is a direct integer
                                        identity, never a re-derivation */
  GENESIS_ACCESS_BYTE = 1,
  GENESIS_ACCESS_WORD = 2,
  GENESIS_ACCESS_LONG = 4,
} GenesisAccessWidth;

typedef enum GenesisAccessDirection {  /* numerically identical to the host
                                           M68kMemoryAccessDirection values */
  GENESIS_ACCESS_READ  = 0,
  GENESIS_ACCESS_WRITE = 1,
} GenesisAccessDirection;

/* §7's runtime-frontier classification, plus two runtime-only additions (7, 8) that have no
   host-side discovery-time counterpart because they can only be known while the generated program
   itself is running, never at static-discovery time. */
typedef enum GenesisStopClass {
  GENESIS_STOP_UNSUPPORTED_CPU_FORM                      = 1, /* host GenesisFrontierClass::unsupported_cpu_form */
  GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS                 = 2, /* host ::unsupported_device_access */
  GENESIS_STOP_UNSUPPORTED_MEMORY_REGION                 = 3, /* host ::unsupported_memory_region */
  GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET                = 4, /* host ::unresolved_indirect_target */
  GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET                = 5, /* host ::known_but_unemitted_target */
  GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT = 6, /* host ::unsupported_interrupt_or_scheduling_event */
  GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY           = 7, /* runtime-only; §4, §6 */
  GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED              = 8, /* runtime-only; §7 */
} GenesisStopClass;
```

§2.1 defines `GenesisProvenance` and its nested types (`GenesisCpuVariant`, `GenesisInstructionProvenance`,
`GenesisBusKind`, `GenesisBusRegion`, `GenesisBusAccess`, `GenesisMappingClaim`) together with the
complete field-disposition table those types exist to satisfy. The remaining top-level ABI types
follow §2.1:

```c
/* §7's diagnostic-category table assigns the explicit values referenced here. */
typedef struct GenesisRuntimeStop {
  GenesisStopClass stop_class;
  GenesisDiagnosticCategory diagnostic_category;  /* §7 */
  GenesisProvenance provenance;                    /* §2.1 */
} GenesisRuntimeStop;

typedef enum GenesisControlTransferKind {
  GENESIS_CONTINUE_AT_PC = 0,
  GENESIS_STOP           = 1,
  GENESIS_COMPLETE       = 2,  /* §1.4/§3/§4/§5: a fully accepted program's legitimate terminal
                                   boundary was reached; never produced by an unknown PC, unresolved
                                   target, unsupported operation, routing failure, malformed input,
                                   or instruction-budget exhaustion (§7) */
} GenesisControlTransferKind;

typedef struct GenesisControlTransfer {
  GenesisControlTransferKind kind;
  uint32_t next_pc;              /* valid only when kind == GENESIS_CONTINUE_AT_PC; zero-initialized
                                     and unread otherwise */
  GenesisRuntimeStop stop;       /* valid only when kind == GENESIS_STOP; zero-initialized (stop_class/
                                     diagnostic_category value 0, has_* fields 0, counts 0) and unread
                                     when kind is GENESIS_CONTINUE_AT_PC or GENESIS_COMPLETE -- a
                                     GENESIS_COMPLETE result carries no stop diagnostic/provenance at
                                     all, by §1.4's construction (a completion is not any kind of
                                     failure) */
} GenesisControlTransfer;
```

`GenesisRuntimeStop` deliberately contains **no** copy of `GenesisRuntime`: only `stop_class`,
`diagnostic_category`, and `provenance` (each a handful of scalar/small-fixed-array fields, not the
64 KiB work-RAM array). A failing operation leaves `*runtime` completely unchanged (§4's block functions
apply no partial effect before returning `GENESIS_STOP`, and §6's routing boundary mutates nothing on a
`GENESIS_ACCESS_FAIL` result), so the single shared `GenesisRuntime` instance the caller already holds
by pointer **is** the retained last-committed state; nothing needs to be copied into the stop
descriptor to preserve it. The same is true for `GENESIS_COMPLETE`: the declared completion `RTS`
applies its full, real stack-pop/A7-increment/PC-set effect exactly like any other `RTS` (§3) and no
further effect after that, so `*runtime` at the moment `GENESIS_COMPLETE` is returned — with `pc`/`a[7]`
already reflecting that real effect — is the final, complete state. A complete report is therefore
always the pair `(GenesisRuntime *runtime, GenesisControlTransfer result)` — the runtime context plus
either a `GENESIS_STOP`'s small stop descriptor or a `GENESIS_COMPLETE`/`GENESIS_CONTINUE_AT_PC`
marker — never a state snapshot embedded inside either. §13 defines exactly how this pair is serialized
into the wire report.

### 2.1 Field-by-field provenance disposition

`GenesisProvenance` must represent every field of full available source/address/access provenance;
T029 does not choose which fields are retained. Every field of `FrontendRejected`, its optional
`InstructionProvenance`, its `RejectedDirectFlow` (`.direct`), its `mapping_claims`, its `accesses`,
and `UnresolvedFrontier.access` is classified below as exactly one of: **(1) carried verbatim**,
**(2) normalized** into an explicitly defined fixed representation, **(3) redundant** (an authoritative
equivalent is named), **(4) inapplicable** to an executable runtime frontier (a concrete reason is
given), or **(5) forbidden from sanitized commercial output but retained in the private/full report**.
No field is silently omitted.

**`FrontendRejected`'s own fields:**

| Field | Disposition | Detail |
| --- | --- | --- |
| `profile` | (4) inapplicable | Always `general_startup` for this bridge (the only profile that produces `FrontendPartialProgram`); conveys no per-instance information, so it is not represented on the wire at all. |
| `category` | (1) carried verbatim | As `GenesisRuntimeStop.diagnostic_category` (§7's table), not inside `GenesisProvenance` itself. |
| `image_source_id` | (4) inapplicable | `rejected(...)` populates this host-only source-ID field for every rejection, not only `invalid_frontend_image_source_id`; a representable rejection may therefore retain it in one of `FrontendPartialProgram.frontiers[*].diagnostic` before later lowering. It is deliberately excluded from generated and wire reports under §14's privacy boundary: source IDs may identify a local input, while the separately verified ROM SHA-256 (§10) is the bridge's permitted, hash-bound ingestion-identity fact. |
| `supplied_image_source_id` | (4) inapplicable | Host-only build-time rejection context, excluded from the wire for the same privacy/SHA-256 reason. It remains distinct from `image_source_id`: it records the caller-supplied identifier even when the canonical retained source ID is unavailable (for example an empty supplied ID), so the host diagnostic does not collapse absent/invalid identity into a different value. |
| `declared_image_byte_length` | (4) inapplicable | Populated only alongside `frontend_image_byte_length_mismatch`, likewise always a build-time reject. |
| `actual_image_byte_length` | (4) inapplicable | Same reason as `declared_image_byte_length`. |
| `source_address` | (3) redundant | Every code path reaching a currently-producible runtime-frontier category sets this to the exact same value as `provenance->source.address` (via the shared `set_source` helper and the decode/operand-resolution rejection helpers). Authoritative equivalent: `GenesisProvenance.instruction.source_address` (below). |
| `image_offset` | (3) redundant | Same reasoning as `source_address`. Authoritative equivalent: `GenesisProvenance.instruction.image_offset`. |
| `provenance` (optional `InstructionProvenance`) | (2) normalized | Expanded field-by-field into `GenesisProvenance.instruction` (a `GenesisInstructionProvenance`, below) rather than carried as one opaque value, with its own presence flag `has_instruction_provenance`. |
| `available_bytes` | (4) inapplicable | Populated only for `truncated_instruction`/discovery-budget-style decode-time rejections, which §7 classifies as build-time rejects for every category `discover_m68k_general_startup` can currently produce; a build-time reject never generates C and therefore never produces a `GenesisRuntimeStop`. |
| `requested_length` | (4) inapplicable | Same reasoning as `available_bytes`. |
| `instruction_length` | (3) redundant when `provenance` is populated (every currently-reachable frontier category: every such path sets `instruction_length = provenance->length.value` verbatim); (4) inapplicable otherwise (build-time-reject-only). Authoritative equivalent: `GenesisProvenance.instruction.length`. |
| `mapping_claims` (optional `vector<MappingClaim>`) | (5) forbidden from sanitized, retained in full/private report | Populated (exactly one element, the instruction's own containing claim) for `unsupported_cpu_form`/`unsupported_device_access`/`unsupported_memory_region`; absent for `unresolved_indirect_target`. Address/structural metadata; see `GenesisMappingClaim` below for its own fields. |
| `direct` (`RejectedDirectFlow`, always present) | (2) normalized | Expanded field-by-field below rather than carried as one opaque value. |
| `accesses` (`vector<StartupBusRecord>`) | (1) carried verbatim for representation, (5) forbidden from sanitized | Populated (exactly one `instruction_read` element, the instruction fetch itself) for `unsupported_cpu_form`/`unsupported_device_access`/`unsupported_memory_region`; absent for `unresolved_indirect_target`. See `GenesisBusAccess` below. |

**`InstructionProvenance` (`FrontendRejected.provenance`'s nested type):**

| Field | Disposition | Detail |
| --- | --- | --- |
| `source.cpu_variant` | (1) carried verbatim | As `GenesisInstructionProvenance.cpu_variant` (`GenesisCpuVariant`, below); only `mc68000` exists today, carried for forward compatibility rather than dropped. |
| `source.address` | (1) carried verbatim | As `GenesisInstructionProvenance.source_address` — this is the authoritative value §7's redundant top-level `source_address` refers to. |
| `source.image_offset` | (1) carried verbatim | As `GenesisInstructionProvenance.image_offset` — authoritative value for the redundant top-level `image_offset`. |
| `bytes` (the primary two-byte word) | (1) carried verbatim | As `GenesisInstructionProvenance.primary_bytes[2]`. Retained even though `accesses[0].bytes` (when populated) repeats these same two bytes as its prefix, because `accesses` is absent for `unresolved_indirect_target`, making `primary_bytes` the only source of the primary word for that class; not redundant in every case, so not eliminated. |
| `length` | (1) carried verbatim | As `GenesisInstructionProvenance.length` — the authoritative value for the redundant top-level and `.direct` `instruction_length` fields. Overflow rule: see below. |

**`RejectedDirectFlow` (`FrontendRejected.direct`)'s own fields:**

| Field | Disposition | Detail |
| --- | --- | --- |
| `category` | (3) redundant | Always set to the same value as top-level `category` by the shared `rejected(...)` helper. Authoritative equivalent: top-level `category` (→ `GenesisRuntimeStop.diagnostic_category`). |
| `block.entry` | (3) redundant | `set_source` sets this to the same address as top-level `source_address`. Authoritative equivalent: `GenesisProvenance.instruction.source_address`. |
| `provenance` + `has_provenance` | (3) redundant | Every reachable path sets these to the identical value as top-level `provenance`. Authoritative equivalent: `GenesisProvenance.instruction` (§2.1). |
| `available_bytes` | (4) inapplicable | Same reasoning as top-level `available_bytes`; never explicitly set for a currently-reachable frontier category. |
| `requested_length` | (4) inapplicable | Same reasoning. |
| `instruction_length` + `has_instruction_length` | (4) inapplicable | Never explicitly set for a currently-reachable frontier category (only the top-level `instruction_length` is set by those code paths). |
| `target` + `has_target` | (3) redundant when populated (`unsupported_device_access`/`unsupported_memory_region`, where it equals the resolved EA address `UnresolvedFrontier.access.address` also carries — authoritative equivalent: `GenesisProvenance.access_address`); (4) inapplicable for `unsupported_cpu_form` (no target concept — the instruction itself, not an operand, is unsupported) and `unresolved_indirect_target` (the whole point is that no target address is known). |
| `mapping_claims` (`.direct`'s own copy) | (3) redundant | `set_claims` always sets this to the same value as top-level `mapping_claims`. Authoritative equivalent: top-level `mapping_claims` (`GenesisProvenance.mapping_claims`, below). |
| `unresolved_reason` | (4) inapplicable | Only populated alongside `discovery_budget_exhausted`-driven build-time rejects (`m68k_discovery_max_blocks`/`_instructions`/`_call_frame_depth`); never for a currently-reachable runtime-frontier category. |

**`MappingClaim` (each element of `mapping_claims`):**

| Field | Disposition | Detail |
| --- | --- | --- |
| `name` | (5) forbidden from sanitized, retained in full/private report | As `GenesisMappingClaim.name`/`.name_length` (below). |
| `target_begin`, `target_end` | (5) forbidden from sanitized, retained in full/private report | As `GenesisMappingClaim.target_begin`/`.target_end`. |
| `image_begin`, `image_end` | (5) forbidden from sanitized, retained in full/private report | As `GenesisMappingClaim.image_begin`/`.image_end`. |

**`StartupBusRecord` (each element of `accesses`):**

| Field | Disposition | Detail |
| --- | --- | --- |
| `ordinal` | (1) carried verbatim | As `GenesisBusAccess.ordinal`; always `0` for this bridge's current single-element `accesses`, carried anyway rather than dropped. |
| `kind` | (2) normalized | As `GenesisBusAccess.kind` (`GenesisBusKind`, below); always `instruction_read` for `discover_m68k_general_startup` today, the other four values are reachable only by the unrelated legacy interpreter and are mirrored for completeness, not currently produced by this bridge. |
| `address` | (3) redundant | Always equal to `provenance.source.address` for this bridge's one populated access (the instruction fetch is always of the source instruction itself). Authoritative equivalent: `GenesisProvenance.instruction.source_address`. |
| `bytes` (the full verified span, unlike `InstructionProvenance.bytes`'s primary-word-only two bytes) | (1) carried verbatim | As `GenesisBusAccess.raw_bytes[GENESIS_MAX_RAW_BYTES]`/`.raw_byte_count`. Overflow rule: see below. |
| `region` | (2) normalized | As `GenesisBusAccess.region` (`GenesisBusRegion`, below); always `raw_cartridge_rom` for `discover_m68k_general_startup` today. |
| `instruction` (nested `InstructionProvenance`) | (3) redundant | Identical to the already-carried `GenesisProvenance.instruction` (this access record is the fetch of that same instruction). Authoritative equivalent: `GenesisProvenance.instruction`. |

**`UnresolvedFrontier.access` (`M68kMemoryAccessRequest`, populated only for `unsupported_device_access`/`unsupported_memory_region`):**

| Field | Disposition | Detail |
| --- | --- | --- |
| `address` | (1) carried verbatim | As `GenesisProvenance.access_address`. |
| `width` | (1) carried verbatim | As `GenesisProvenance.access_width` (`GenesisAccessWidth`). |
| `direction` | (1) carried verbatim | As `GenesisProvenance.access_direction` (`GenesisAccessDirection`). |
| `source_provenance` (optional `InstructionProvenance`) | (3) redundant by design rule | §1.1 requires T029 to set this to the exact same value as `diagnostic.provenance`, never a second, independently derived value. Authoritative equivalent: `GenesisProvenance.instruction`. |

**Overflow rule (applies uniformly to every variable-length host field mapped to a fixed-capacity wire
field: `StartupBusRecord.bytes` → `GenesisBusAccess.raw_bytes`, `MappingClaim.name` → `GenesisMapping
Claim.name`, and the `mapping_claims`/`accesses` vectors → their fixed-capacity wire arrays):** if the
host value's actual size exceeds the wire representation's fixed capacity, this is detected at
generation time (every one of these sizes is statically known before generation: verified instruction
length, mapping-claim name length, and claim/access counts are all discovery-time facts, never a
runtime unknown) and causes a build-time `FrontendRejected` under "no safe frontier stub available"
(§7 condition 3: no defined representation exists without an invented/truncated value). This can never
happen at runtime and never silently truncates provenance.

**Pure-C11 types satisfying this table** (declared in `tools/genesis_startup_bridge_runtime.h`, §12):

```c
typedef enum GenesisCpuVariant { GENESIS_CPU_MC68000 = 1 } GenesisCpuVariant;  /* mirrors host
                                                                                   CpuVariant; only
                                                                                   mc68000 exists
                                                                                   today */

typedef struct GenesisInstructionProvenance {
  GenesisCpuVariant cpu_variant;
  uint32_t source_address;      /* InstructionProvenance.source.address (M68kProgramAddress.value) */
  uint64_t image_offset;        /* InstructionProvenance.source.image_offset (MoveqImageOffset.value) */
  uint8_t  primary_bytes[2];    /* InstructionProvenance.bytes: the primary two-byte word only */
  uint32_t length;              /* InstructionProvenance.length.value: the full verified span,
                                    including any extension words; authoritative instruction length */
} GenesisInstructionProvenance;

typedef enum GenesisBusKind {   /* mirrors host StartupBusKind; only instruction_read is reachable
                                    through discover_m68k_general_startup today -- the other four are
                                    reachable only through the unrelated legacy interpreter */
  GENESIS_BUS_INSTRUCTION_READ = 1,
  GENESIS_BUS_DATA_READ        = 2,
  GENESIS_BUS_DATA_WRITE       = 3,
  GENESIS_BUS_STACK_READ       = 4,
  GENESIS_BUS_STACK_WRITE      = 5,
} GenesisBusKind;

typedef enum GenesisBusRegion {  /* mirrors every StartupBusRecord.region string literal present
                                     anywhere in the current repository; discover_m68k_general_startup
                                     itself only ever produces raw_cartridge_rom today */
  GENESIS_REGION_RAW_CARTRIDGE_ROM  = 1,
  GENESIS_REGION_SYNTHETIC_WORK_RAM = 2,
} GenesisBusRegion;

/* `address_space_contract.h` defines this shared C/C++ capacity as UINT32_C(12),
   per ADR 0008. The generated-runtime ABI, translation-time eligibility gates,
   and runtime validators use this one capacity; it is engineering headroom over
   the general-startup decoder's 10-byte maximum, not a hardware fact. */
#define GENESIS_MAX_RAW_BYTES SEGARECOMP_GENESIS_MAX_RAW_INSTRUCTION_BYTES
typedef struct GenesisBusAccess {
  uint64_t ordinal;
  GenesisBusKind kind;
  uint32_t address;
  uint8_t  raw_bytes[GENESIS_MAX_RAW_BYTES];
  uint8_t  raw_byte_count;      /* StartupBusRecord.bytes.size(); must be <=
                                   GENESIS_MAX_RAW_BYTES; see the overflow rule */
  GenesisBusRegion region;
} GenesisBusAccess;

#define GENESIS_MAX_NAME_LENGTH 64U
typedef struct GenesisMappingClaim {
  char     name[GENESIS_MAX_NAME_LENGTH];
  uint8_t  name_length;          /* MappingClaim.name's exact byte length; name[] is not relied on to
                                     be NUL-terminated, name_length is authoritative; see overflow rule */
  uint32_t target_begin;         /* MappingClaim.target_begin.value */
  uint32_t target_end;           /* MappingClaim.target_end.value */
  uint64_t image_begin;          /* MappingClaim.image_begin.value */
  uint64_t image_end;            /* MappingClaim.image_end.value */
} GenesisMappingClaim;

#define GENESIS_MAX_MAPPING_CLAIMS 4U   /* today at most one; headroom per the overflow rule */
#define GENESIS_MAX_BUS_ACCESSES   4U   /* today at most one; headroom per the overflow rule */

typedef struct GenesisProvenance {
  uint8_t  has_instruction_provenance;
  GenesisInstructionProvenance instruction;

  uint8_t  has_access;               /* UnresolvedFrontier.access, populated only for
                                         unsupported_device_access/unsupported_memory_region */
  uint32_t access_address;
  GenesisAccessWidth access_width;
  GenesisAccessDirection access_direction;

  uint8_t  mapping_claim_count;      /* 0..GENESIS_MAX_MAPPING_CLAIMS */
  GenesisMappingClaim mapping_claims[GENESIS_MAX_MAPPING_CLAIMS];

  uint8_t  bus_access_count;         /* 0..GENESIS_MAX_BUS_ACCESSES */
  GenesisBusAccess bus_accesses[GENESIS_MAX_BUS_ACCESSES];
} GenesisProvenance;
```

`GenesisProvenance` is not "the pure-C equivalent of a hand-picked subset" of any host structure: the
table above accounts for every field of every host structure this bridge's `UnresolvedFrontier` can
carry, and every field the table assigns disposition (1) or (2) has an explicit, named slot in the
types above; every disposition-(3) field is instead reachable only through its named authoritative
equivalent; every disposition-(4) field is named as inapplicable with its reason; every disposition-(5)
field exists in `GenesisProvenance` but is excluded from the sanitized report specifically (§13.3/§14),
never from the full report.

## 3. Generated-unit calling convention

The generated-unit granularity is one plain C function per discovered `M68kStaticBlock` — the same
atomic control-flow-linear granularity `emit_m68k_structured_direct_flow_c` already uses for its
per-unit functions (see §12 for why this granularity, not a per-frame or per-instruction granularity,
is chosen). Each generated block function:

```c
static GenesisControlTransfer genesis_block_%08X(GenesisRuntime *runtime);
```

reusing the existing `m68k_block_%08X`/`analysis.units[i].id` naming already produced by
`M68kStaticBlock`/`StaticEmissionUnit` provenance, and returns one typed control-transfer descriptor
(§2). A block function applies every statically lifted operation's effect to `*runtime` (through the
same shared `emit_m68k_operation_c`/`M68kOperationEffect` lowering the existing routes already use —
this bridge adds no second per-instruction semantic owner), then returns `GENESIS_CONTINUE_AT_PC` with
the next statically known PC target for an ordinary fallthrough/branch/call transition, or `GENESIS_STOP`
with a populated `GenesisRuntimeStop` when the block's own build-time-accepted content is exhausted at
the recorded frontier (§1.3's lowering produces this literal `GenesisRuntimeStop` initializer at
generation time) or when a routed access fails (§6).

**Every `RTS`, with no exception, applies its full, real instruction effect** — this bridge invents no
instruction-specific shortcut and no suppressed effect, matching [the Genesis startup shared-route
ownership contract](genesis-startup-shared-route-ownership-contract.md)'s already-documented pop/
validate/dispatch sequence:

1. Read the 32-bit return target from runtime stack memory through `genesis_route_access` (§6) — the
   normal work-RAM routing boundary every stack read already must use (§11); this can fail exactly like
   any other routed read (misaligned or out-of-range stack access), producing `GENESIS_STOP` with
   `diagnostic_category = GENESIS_DIAG_INVALID_STACK_ALIGNMENT`/`GENESIS_DIAG_INVALID_STACK_RANGE` as
   applicable, with no further effect applied.
2. Validate the popped value against the finite set of statically authorized return targets for this
   exact `RTS` instruction, determined entirely at generation time: every `M68kStaticCall` continuation
   whose `callee` is this block (there may be more than one, if this block is a callee of more than one
   static call site), **plus**, only when this exact `RTS`'s address equals `FrontendAnalysis.
   completion->terminal_rts.source.address` (§1.4), the one additional authorized value `FrontendAnalysis.
   completion->sentinel_return_pc`. A block whose `RTS` has no declared completion contract has exactly
   the same authorized-target set it would have without this correction at all — nothing changes for it.
3. If the popped value matches none of these statically authorized targets, return `GENESIS_STOP` with
   `diagnostic_category = GENESIS_DIAG_RETURN_TARGET_MISMATCH` — the existing category, unchanged — with
   no further effect applied; this is finite static target validation against a generation-time-known
   set, never generic runtime target discovery or opcode decoding.
4. Otherwise, increment `runtime->a[7]` by four exactly as `RTS` requires, and set `runtime->pc` to the
   validated target. This step is identical for every authorized target, known continuation or sentinel
   alike — nothing about it depends on which kind of target matched.
5. Only *after* this real effect has been applied does the block function decide its return value: if
   the validated target was a known static-call continuation, return `GENESIS_CONTINUE_AT_PC` with that
   same target (dispatch proceeds normally, §4); if the validated target was exactly the declared
   sentinel, return `GENESIS_COMPLETE`, with `next_pc`/`stop` both zero-initialized and unread (§2) —
   `runtime->pc`/`runtime->a[7]` already hold the real, post-`RTS` state from step 4, which is exactly
   what is preserved into the final `GenesisRuntime` (§5).

This is the **sole** generation-time condition that authorizes `GENESIS_COMPLETE`: the validated,
popped-from-stack return target of the one specific, statically identified `RTS` a `FrontendCompletion
Record` names equals its declared sentinel. Completion is never authorized by block identity alone — the
same physical block, if reached through a real static call in a different traversal, validates its
popped target against that call's own continuation in step 2/3 exactly as above and returns
`GENESIS_CONTINUE_AT_PC`, never `GENESIS_COMPLETE`, because the real stack value it reads in that context
is the real call's own pushed continuation, not the sentinel. One context can never change another
context's behavior, because both are decided by the same real, runtime-observed stack value, never by
which block or which discovery path reached it.

A block function never returns a guessed PC, never itself decides device or memory routing beyond the
normal §6 boundary every read/write already uses, and never emits `GENESIS_COMPLETE` for any reason
other than step 5 above. Every read/write that must route through §6's boundary does so through that
one call, and the block function propagates any boundary failure (§6's `GENESIS_ACCESS_FAIL` result,
already carrying a populated `GenesisRuntimeStop`) as `GENESIS_STOP` without applying any further effect.

**Sentinel initialization (synthetic mode only).** When `FrontendAnalysis.completion` is present, the
generated program's own initial-state construction — which already establishes `d[]`/`a[]`/`pc`/
`work_ram` as generation-time literal initializers, exactly as the existing `genesis_rom_startup` route's
`a[7] = UINT32_C(initial_ssp)`/`pc = UINT32_C(entry)` precedent already does — additionally writes the
declared sentinel's four bytes, in big-endian MC68000 order, into `work_ram` at the byte offset the
initial `a[7]` (`M68kStartupIngress::initial_ssp`) resolves to within `work_ram`, via literal generated
assignment statements (for example `runtime.work_ram[offset+0] = 0x..; ...; runtime.work_ram[offset+3] =
0x..;`), emitted only for this one synthetic fixture. This initialization requires and the emitter must
verify: the initial `a[7]` is correctly aligned (even, matching every other stack-access alignment rule
already enforced); and the complete four-byte sentinel value fits entirely inside `work_ram`'s bounds at
that offset. A failure of either check is a build/driver error (the existing "no safe frontier stub
available" build-time-reject disposition, §1.4), never represented as `GENESIS_COMPLETE` or any other
runtime result. This is project-authored synthetic fixture state, not a device or ROM semantic: it
exists only to give the declared terminal `RTS` a real, valid stack value to read in step 1 above, so
that instruction executes with no shortcut, exactly like every other `RTS`.

## 4. Precompiled-PC dispatcher contract

One generated function maps every statically discovered PC target to its corresponding generated block
function:

```c
static GenesisControlTransfer genesis_dispatch(GenesisRuntime *runtime) {
  if (runtime->pc == UINT32_C(0x...)) return genesis_block_00000...(runtime);
  if (runtime->pc == UINT32_C(0x...)) return genesis_block_00000...(runtime);
  /* ...one comparison per discovered M68kStaticBlock entry, matching the existing
     per-unit `if (*pc == ...)` shape emit_m68k_structured_direct_flow_c already emits... */
  return genesis_internal_dispatch_inconsistency_stop(runtime);
}
```

`genesis_dispatch` is used only for PC targets already resolved at translation time (every
`static_block` entry address); it performs no generic opcode-based dispatch, no runtime target-byte
fetch, and no runtime instruction decode. Reaching `genesis_dispatch` with a PC that matches no
compiled-in block entry is impossible for a correctly generated bridge program (every accepted block's
`GENESIS_CONTINUE_AT_PC` target is one of the addresses this table already compares against, and every
other exit is `GENESIS_STOP` or `GENESIS_COMPLETE`), so it is treated as a defensive generator-defect
signal, never a frontier and never a completion: `genesis_internal_dispatch_inconsistency_stop`
(defined once in the runtime-support file, §12) returns `GENESIS_STOP` with `stop_class =
GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY` and `diagnostic_category =
GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY` (§7), never a silent continuation or a fallback decode.
Every unresolved or unemitted target is instead resolved to a `GENESIS_STOP` directly inside the block
that reaches it, by §1's `UnresolvedFrontier`, before dispatch is ever attempted against it.

`genesis_dispatch` itself never emits `GENESIS_COMPLETE`: it only calls a block function and returns
that call's exact result unchanged, so `GENESIS_COMPLETE` can only ever originate inside the one block
function containing the declared completion `RTS` (§3), and only in step 5 of that instruction's own
normal execution, after its real stack-pop/A7-increment/PC-set effect and only when the validated,
popped target equals the declared sentinel — never merely because dispatch reached that block. The
runtime-support driving loop (§12) that calls
`genesis_dispatch` in a bounded cycle handles the three outcomes as follows: on `GENESIS_CONTINUE_AT_PC`
it stores `next_pc` into `runtime->pc` and, if the per-run dispatch-count bound has not yet been
reached, calls `genesis_dispatch` again; on `GENESIS_STOP` it stops the loop and writes the wire
report's `"stop"` result (§13.3); on `GENESIS_COMPLETE` it stops the loop and writes the wire report's
`"completed"` result (§13.3), immediately, without any further dispatch. The dispatch-count bound
itself, once reached without either `GENESIS_STOP` or `GENESIS_COMPLETE` having occurred, produces a
`GENESIS_STOP` with `stop_class = GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED`/`diagnostic_category =
GENESIS_DIAG_INSTRUCTION_BUDGET_EXHAUSTED` (§7) — this is always a typed stop, never treated as, or
substituted for, `GENESIS_COMPLETE`, regardless of how many dispatches occurred. An unknown PC,
unresolved target, unsupported operation, routing failure, malformed input (a build-time reject, which
never reaches this loop at all — generation itself is refused), and instruction-budget exhaustion can
therefore never be reported as normal completion under any circumstance; `GENESIS_COMPLETE` has exactly
one origin (§3) and no other code path in the generated program or its runtime-support file may
construct a `GenesisControlTransfer` with `kind == GENESIS_COMPLETE`.

This differs from the existing private `dispatch(...)` inside `emit_m68k_structured_direct_flow_c`:
that function dispatches within one generated program's own self-contained state machine using a
transient `uint32_t d[8]`/`uint16_t sr` pair local to `main`, with no persistent runtime context and no
contract for calling across independently generated translation units. `genesis_dispatch` instead
takes and mutates the one shared, persistent `GenesisRuntime *runtime` and is the one routing point
through which every independently generated unit is reached, in this bridge or a later one.

## 5. Typed runtime-stop/report format

`GenesisRuntimeStop` (§2) generalizes `StartupFailure`/`StartupBoundary`'s existing shape (category,
provenance) to the persistent multi-unit `GenesisRuntime`, reusing their field intent rather than
inventing an unrelated report shape, while deliberately omitting a duplicated machine-state snapshot
(§2's "no copy of `GenesisRuntime`" rule) because the shared `GenesisRuntime` the caller already holds
is itself the retained last-committed state.

A `GenesisRuntimeStop` is produced only after every already-committed operation in the current block
has been fully applied to `*runtime`; the operation that could not proceed applies no partial effect,
so `*runtime` at the moment `GENESIS_STOP` is returned is exactly the state as of the last successfully
committed operation — never a mixture of committed and attempted-but-failed effects. `GENESIS_COMPLETE`
carries no `GenesisRuntimeStop` at all (§2's `GenesisControlTransfer.stop` is unread for that kind):
completion is a success outcome, not any kind of failure, so it has no diagnostic category and no
provenance to report — only the final `*runtime` state, which by §3's normal `RTS` execution already
includes the real post-`RTS` `pc` (equal to the declared sentinel) and `a[7]` (incremented by four from
its value immediately before that `RTS`), exactly as any other successfully executed `RTS` would leave
them. This is exactly what the wire report's `"completed"` result carries (§13.3). §14 states the
privacy boundary distinguishing this full local report (for either result) from any output a commercial
run may persist.

## 6. Shared runtime memory/device routing boundary

One generated-code-callable entry point, declared once in the fixed runtime-support header (§12) and
defined once in the fixed runtime-support source file (§13):

```c
typedef enum GenesisAccessResultKind { GENESIS_ACCESS_OK = 0, GENESIS_ACCESS_FAIL = 1 } GenesisAccessResultKind;

GenesisAccessResultKind genesis_route_access(GenesisRuntime *runtime, uint32_t address,
                                              GenesisAccessWidth width,
                                              GenesisAccessDirection direction,
                                              uint32_t *value /* in for write, out for read */,
                                              GenesisRuntimeStop *stop_out /* populated on FAIL */);
```

`genesis_route_access` recognizes four region kinds for correct classification. As implemented by T029,
whose scope contained no controller semantics, it routed only persistent work RAM to a success path,
with every device-region access and every ROM read reached at runtime unconditionally
`GENESIS_ACCESS_FAIL`. SEG-007-T077 (ADR 0006) narrowly widens the ROM case only, for reads only, under
a bounded proof obligation stated below; device-region access remains completely unconditionally
fail-closed, unchanged:

- **Persistent Genesis work RAM** — every in-range read or write succeeds against
  `runtime->work_ram`; this is the only region T029's `genesis_route_access` ever actually mutates or
  returns `GENESIS_ACCESS_OK` for.
- **Device regions** (for example the bounded controller-I/O interval) — recognized by address so an
  in-range device access is never misclassified as `unmapped_data_access`, but **T029 must not port,
  duplicate, move, or reinterpret `m68k_controller_io_access`'s selector semantics into runtime-support
  C**. The existing host-side translation-time `m68k_controller_io_access`/
  `m68k_route_genesis_device_access` and T021's accepted selector remain exactly as they are today,
  used only by the existing translation-time folding path for other routes; they are not called from,
  reimplemented inside, or in any way reused by T029's generated runtime-support C. Every device-region
  access this bridge's `genesis_route_access` recognizes — including the exact shape T021's
  translation-time selector accepts — returns `GENESIS_ACCESS_FAIL` with
  `diagnostic_category = GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO` (or the applicable
  existing category) unconditionally in T029. **Defining this boundary's existence authorizes no new
  device semantics.** A later, separately evidenced and authorized controller-implementation task
  (gated behind SEG-007-T031 per this bridge's Non-goals) may connect explicitly authorized runtime
  semantics through this same seam; until then it is fail-closed for one hundred percent of
  device-region accesses reached at runtime, with no exception.
- **ROM** — recognized for correct region classification, and, since ADR 0006 (SEG-007-T077), a bounded
  runtime read-success path for a **statically owned, generated cartridge-data region** — never as a
  general-purpose runtime read path for an arbitrary address, and never as any kind of write path. §10
  states this widening precisely and its exact non-goals; in outline: C4 already accepts and lowers
  several representable, non-constant-foldable effective-address forms (register-indirect,
  postincrement, predecrement, `d16(An)`) by routing them through this boundary at runtime regardless of
  what address they resolve to at execution time (§11's own "every access whose target address is not
  proven statically constant" rule), so a runtime-computed address that happens to resolve into ROM
  always reached this boundary in practice, before ADR 0006, exactly as SEG-007-T074/T075/T076's own
  evidence demonstrates — it simply always failed unconditionally on arrival. ADR 0006 does not create a
  new call site or a new class of accepted operation; it gives that pre-existing, already-reachable ROM
  read case a sound, bounds-checked success outcome exactly when the address falls inside a mapping
  claim discovery independently proved (`MappingClaim`), that claim's target range never exceeds the
  fixed hardware ROM window (`[0, 0x00400000)`), and the compiled program serves the read only from its
  own build-time-embedded copy of that claim's data — never from a live read of the original ROM file,
  and never through instruction fetch/decode. A read whose address is not proven to fall inside such a
  region still returns `GENESIS_ACCESS_FAIL` with `stop_class = GENESIS_STOP_INTERNAL_DISPATCH_
  INCONSISTENCY` and `diagnostic_category = GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY` (§7) exactly
  as before ADR 0006 — the same defensive generator-defect category §4 uses, because it means either a
  genuine generator defect or a genuinely unmapped/unprovable read, never a silently accepted guess. A
  ROM **write** reached at runtime always returns `GENESIS_ACCESS_FAIL` with `diagnostic_category =
  GENESIS_DIAG_ROM_WRITE_PROHIBITED`, unconditionally, matching every earlier route's ROM-write
  behavior; this is a legitimate, expected fail-closed result (ROM is never writable under any ROM-
  ownership option), not a generator defect, and ADR 0006 changes nothing about it.

Every routed access is width/direction/address-checked before mutation; a `GENESIS_ACCESS_FAIL` result
mutates nothing and populates `*stop_out` per §2/§2.1/§5.

## 7. Build-time-rejection-versus-runtime-frontier-versus-completion distinction

**General rule:** a condition is an executable runtime frontier (`FrontendPartialProgram`, §1) only
when **all four** of the following hold; otherwise it is always a build-time rejection
(`FrontendRejected`, with the accepted prefix always discarded — regardless of how many blocks were
already accepted before the failing operation was reached):

1. At least one `M68kStaticBlock` was already fully accepted before the failing operation.
2. The failing operation itself has full, trustworthy provenance (it was itself validly decoded and
   lifted — a verified source address, image offset, raw bytes, and length — and only its further
   *semantics* are unsupported or unroutable).
3. A safe typed stop can be emitted: the condition maps to a defined `GenesisStopClass`/
   `GenesisDiagnosticCategory` pair (the table below) with no invented value, and every field §2.1's
   disposition table assigns fits its fixed wire representation without overflow.
4. No guessed semantic effect is required to represent the stop (the stop itself, not a fabricated
   continuation, is the only thing emitted).

"No safe frontier stub available" is this general rule's failure as a whole — a semantic/provenance
condition, not a synonym for condition 1 alone. For example: three blocks already accepted, then an
operation with a truncated extension word is reached — condition 1 holds but condition 2 fails, so this
is still a build-time reject, even though a nonempty prefix exists. Conversely, the very first operation
being a known-valid-but-unsupported CPU form fails only condition 1 (zero accepted blocks) — this is
also a build-time reject, for the more specific reason that no prefix exists to retain.

An `RTS` reached with an empty call-frame stack is, in every case, still governed by exactly the same
unmodified production behavior it already has on `main` (`return_context_missing`, a build-time reject,
per the Purpose section) — this contract does not add a third disposition for it and does not evaluate
it against the four-condition frontier test either. `GENESIS_COMPLETE` (§1.4) is not a reclassification
of this category at all: it is authorized only by an explicit, opt-in `FrontendCompletionContract`
declared as discovery *input*, and only for the one declared `RTS` instruction whose *validated,
runtime-popped* return target equals the declared sentinel (§3) — a fact `discover_m68k_general_startup`
can prove and record at discovery time (`FrontendAnalysis.completion`, §1.4) for the declared instruction,
but which the generated program itself only actually confirms at runtime by reading the real stack value
(§3), exactly like it confirms any other `RTS`'s validated continuation. `GENESIS_COMPLETE` is
represented on the wire (§2, §3, §5, §13) as its own outcome, never as `GENESIS_STOP`/`"stop"` and never
folded into any `GenesisStopClass`/`GenesisDiagnosticCategory` value; it exists alongside build-time
rejection and runtime frontier as a third possible generated-program outcome, but it is authorized by an
explicit declaration and a real runtime-validated stack read, never by a discovery-time condition alone.

| Category | Disposition | Which condition fails (if build-time reject) |
| --- | --- | --- |
| Malformed/truncated instruction | **Always build-time reject** | Condition 2 (no trustworthy full-instruction provenance) |
| Conflicting/ambiguous mapping or structure claim | **Always build-time reject** | Condition 2 (the claim about what code even is at that address is itself unverifiable) |
| Invalid provenance | **Always build-time reject** | Condition 2 |
| "No safe frontier stub available" in general (including zero accepted blocks and §2.1's overflow rule) | **Always build-time reject** | Condition 1, 3, or 4, whichever fails |
| An `RTS` reached with an empty call-frame stack, with no matching declared `FrontendCompletionContract` for that exact instruction | **Always build-time reject** (`return_context_missing`), unchanged, exactly as unmodified production behavior on `main` | Not evaluated against conditions 1-4 at all; no `GenesisFrontierClass` exists for this condition |
| An `FrontendCompletionContract`'s declared invariants (§1.4: real `RTS`, valid even 24-bit non-overlapping sentinel) cannot be proven | **Always build-time reject** ("no safe frontier stub available") | Condition 3 (no safe representation without an invented/unprovable value) |
| Known-valid-but-unsupported CPU form (a real, well-formed MC68000 form whose semantics are not yet emitted) | Executable runtime frontier, `GENESIS_STOP_UNSUPPORTED_CPU_FORM` — only when conditions 1-4 all hold; since SEG-007-T064's tenth/eleventh revisions (§14, ADR 0004/0005), the CLI's own static discovery report (not the wire report) may also carry `opcode_line` | — |
| Unsupported device access | Executable runtime frontier, `GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS` — only when conditions 1-4 all hold; since SEG-007-T064's tenth/eleventh revisions (§14, ADR 0004/0005), the CLI's own static discovery report (not the wire report) may also carry `region_class` | — |
| Unsupported memory region | Executable runtime frontier, `GENESIS_STOP_UNSUPPORTED_MEMORY_REGION` — only when conditions 1-4 all hold; since SEG-007-T064's tenth/eleventh revisions (§14, ADR 0004/0005), the CLI's own static discovery report (not the wire report) may also carry `region_class` | — |
| Unresolved/indirect target | Executable runtime frontier, `GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET` — only when conditions 1-4 all hold | — |
| Known-but-not-yet-emitted target | Executable runtime frontier, `GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET` — only when conditions 1-4 all hold; producible since SEG-007-T064, for a best-effort sibling exit (§1.1) discovery knows the target address of but whose own deeper diagnostic cannot be losslessly retained as one of the four precise classes above; still never producible merely because T029's first implementation emits every discovered `static_block` with no artificial scope limit (that reason no longer applies) | — |
| Unsupported interrupt/scheduling event | Executable runtime frontier, `GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT` — only when conditions 1-4 all hold; not currently producible by T029's first implementation (Genesis interrupts/scheduling remain entirely out of scope) | — |

Malformed/truncated instructions and ambiguous mappings are never reclassified as
`GENESIS_STOP_UNSUPPORTED_CPU_FORM`: only a genuinely valid-but-unemitted CPU form may become that
runtime frontier, matching the corrected precedence above.

A runtime frontier, by construction:

- applies no guessed semantic effect for the operation that triggered it;
- performs no partial memory/device mutation for that operation;
- retains the last committed `GenesisRuntime` state exactly (§2, §5 — the shared instance is simply
  left unmodified);
- gives a deterministic typed diagnostic with retained provenance (§2, §2.1, §5);
- never silently continues past the frontier; and
- never falls back to a runtime interpreter or opcode decoder — reaching a frontier always stops
  execution of the generated bridge program, it never triggers dynamic decode of any kind.

### `GenesisDiagnosticCategory` — the pure-C diagnostic taxonomy

`GenesisDiagnosticCategory` is a pure C11 `enum` mirroring every `DirectFlowDiagnostic` value with a
pinned, explicitly assigned integer (declaration order in `include/segarecomp/m68k_pipeline.hpp`,
numbered starting at 1; this is a fixed table this contract pins, not a value mechanically re-derived
from the C++ enum's ordinal position at build time), plus three runtime/generation-only additions this
bridge owns (35, 36, 37) that have no `DirectFlowDiagnostic` counterpart because they describe
conditions the translation-time taxonomy does not model. The `#` column is this pure C11 enum's formal
definition: `typedef enum GenesisDiagnosticCategory { GENESIS_DIAG_ODD_INSTRUCTION_ADDRESS = 1, ...,
GENESIS_DIAG_KNOWN_BUT_UNEMITTED_TARGET = 37 } GenesisDiagnosticCategory;`, with every enumerator
assigned exactly the integer this table's `#` column states, in the order listed.

| # | `DirectFlowDiagnostic` (host, C++) | `GenesisDiagnosticCategory` (pure C11) |
| - | --- | --- |
| 1 | `odd_instruction_address` | `GENESIS_DIAG_ODD_INSTRUCTION_ADDRESS` |
| 2 | `unmapped_instruction_address` | `GENESIS_DIAG_UNMAPPED_INSTRUCTION_ADDRESS` |
| 3 | `truncated_instruction` | `GENESIS_DIAG_TRUNCATED_INSTRUCTION` |
| 4 | `illegal_instruction` | `GENESIS_DIAG_ILLEGAL_INSTRUCTION` |
| 5 | `valid_but_unsupported_instruction` | `GENESIS_DIAG_VALID_BUT_UNSUPPORTED_INSTRUCTION` |
| 6 | `unsupported_instruction_form` | `GENESIS_DIAG_UNSUPPORTED_INSTRUCTION_FORM` |
| 7 | `odd_direct_target` | `GENESIS_DIAG_ODD_DIRECT_TARGET` |
| 8 | `conflicting_address_mapping` | `GENESIS_DIAG_CONFLICTING_ADDRESS_MAPPING` |
| 9 | `invalid_address_mapping` | `GENESIS_DIAG_INVALID_ADDRESS_MAPPING` |
| 10 | `unmapped_direct_target` | `GENESIS_DIAG_UNMAPPED_DIRECT_TARGET` |
| 11 | `mid_instruction_direct_target` | `GENESIS_DIAG_MID_INSTRUCTION_DIRECT_TARGET` |
| 12 | `reached_unresolved_direct_edge` | `GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE` |
| 13 | `invalid_frontend_image_source_id` | `GENESIS_DIAG_INVALID_FRONTEND_IMAGE_SOURCE_ID` |
| 14 | `frontend_image_byte_length_mismatch` | `GENESIS_DIAG_FRONTEND_IMAGE_BYTE_LENGTH_MISMATCH` |
| 15 | `invalid_mapping_claim` | `GENESIS_DIAG_INVALID_MAPPING_CLAIM` |
| 16 | `vector_fixture_id_mismatch` | `GENESIS_DIAG_VECTOR_FIXTURE_ID_MISMATCH` |
| 17 | `vector_image_sha256_mismatch` | `GENESIS_DIAG_VECTOR_IMAGE_SHA256_MISMATCH` |
| 18 | `vector_cpu_variant_mismatch` | `GENESIS_DIAG_VECTOR_CPU_VARIANT_MISMATCH` |
| 19 | `vector_execution_entry_space_mismatch` | `GENESIS_DIAG_VECTOR_EXECUTION_ENTRY_SPACE_MISMATCH` |
| 20 | `execution_entry_inside_discovered_instruction` | `GENESIS_DIAG_EXECUTION_ENTRY_INSIDE_DISCOVERED_INSTRUCTION` |
| 21 | `execution_entry_inside_discovered_block` | `GENESIS_DIAG_EXECUTION_ENTRY_INSIDE_DISCOVERED_BLOCK` |
| 22 | `execution_entry_not_discovered_block_start` | `GENESIS_DIAG_EXECUTION_ENTRY_NOT_DISCOVERED_BLOCK_START` |
| 23 | `vector_block_instruction_count_mismatch` | `GENESIS_DIAG_VECTOR_BLOCK_INSTRUCTION_COUNT_MISMATCH` |
| 24 | `effective_address_not_24bit` | `GENESIS_DIAG_EFFECTIVE_ADDRESS_NOT_24BIT` |
| 25 | `odd_effective_address` | `GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS` |
| 26 | `rom_write_prohibited` | `GENESIS_DIAG_ROM_WRITE_PROHIBITED` |
| 27 | `unmapped_data_access` | `GENESIS_DIAG_UNMAPPED_DATA_ACCESS` |
| 28 | `unsupported_device_region_controller_io` | `GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO` |
| 29 | `invalid_stack_alignment` | `GENESIS_DIAG_INVALID_STACK_ALIGNMENT` |
| 30 | `invalid_stack_range` | `GENESIS_DIAG_INVALID_STACK_RANGE` |
| 31 | `return_context_missing` | `GENESIS_DIAG_RETURN_CONTEXT_MISSING` |
| 32 | `return_target_mismatch` | `GENESIS_DIAG_RETURN_TARGET_MISMATCH` |
| 33 | `startup_graph_mismatch` | `GENESIS_DIAG_STARTUP_GRAPH_MISMATCH` |
| 34 | `discovery_budget_exhausted` | `GENESIS_DIAG_DISCOVERY_BUDGET_EXHAUSTED` |
| 35 | *(none — runtime-only, §4/§6)* | `GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY` |
| 36 | *(none — runtime-only, below)* | `GENESIS_DIAG_INSTRUCTION_BUDGET_EXHAUSTED` |
| 37 | *(none — generation-only)* | `GENESIS_DIAG_KNOWN_BUT_UNEMITTED_TARGET` |

Row 31 (`return_context_missing`) remains fully reachable through `discover_m68k_general_startup` itself,
unconditionally, for every `RTS` reached with an empty call-frame stack that has no matching declared
`FrontendCompletionContract` (§1.4) — this is unmodified production behavior, not narrowed by this
bridge. It also still applies to `execute_m68k_frontend_startup`'s own, unrelated runtime check, and to
any other future producer. `GENESIS_COMPLETE` is never a reclassification of category 31: it is produced
only when the one specific, declared-and-validated `RTS` instruction's real, runtime-popped return
target is observed to equal its declared sentinel (§1.4, §3) — a fact confirmed by the generated
program's own execution, not inferred from the empty-call-frame-stack condition alone. Only a bounded
subset of these
37 values is reachable by a correctly generated T029 bridge program today (chiefly 26, 28, 35, 36, and
37 by name only per its own row's note); the remainder are included so this is a complete, stable
mirror of the existing taxonomy rather than a second, independently curated one. A future
`DirectFlowDiagnostic` addition receives the next unused integer (38, 39, ...) appended to this table;
no previously assigned value in this table is ever renumbered.

`GENESIS_DIAG_INSTRUCTION_BUDGET_EXHAUSTED` (36) pairs with `GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED`
(§2) and matches the existing `instruction_budget_exhausted` stop-reason string convention every other
generated-C route in this repository already uses (`emit_moveq_c`, the `direct_flow` dispatch loop, and
the `genesis_rom_startup` route): the runtime-support driving loop (§4, §12) counts total dispatched
operations and returns this stop once a fixed, generation-time-configured bound is reached, guaranteeing
the generated bridge program always halts even if the accepted prefix's control flow would otherwise
loop indefinitely before reaching the baked frontier or completion boundary. This is a normal, expected
termination condition, not a compatibility failure, exactly as it is for every other existing route —
and, per §4, it is always represented as `GENESIS_STOP`, never substituted for `GENESIS_COMPLETE`.

## 8. Musashi boundary

Musashi remains valid only as a CPU-oracle differential/validation tool. It must never be required to
build or run a generated bridge program; must never supply the production CFG or decide which
generated block executes; must never execute an unsupported instruction at runtime on this bridge's
behalf; must never supply controller/VDP/device semantics; and must never become a hidden interpreter
fallback for a runtime frontier. Every claim this bridge or its successor makes about generated-program
behavior is established by compiling and running the generated C itself (§13); Musashi may only be used
afterward, out of band, to differentially confirm the same accepted instructions' CPU-state effects,
exactly as SEG-012's migration contract already scopes it. A declared completion `RTS` (§1.4/§3) applies
its full, real `RTS` effect exactly like any other `RTS`, so it remains eligible for the same
CPU-oracle differential comparison every other `RTS` already has: a synthetic fixture may feed Musashi
the identical initial state, including the sentinel value placed on the stack (§3), and independently
confirm Musashi computes the same popped target, `A7` increment, and `PC` update — an ordinary `RTS`
oracle vector, not a special case Musashi is asked to understand `GENESIS_COMPLETE` itself; Musashi
never learns of, or needs to know, that the popped value happens to be a declared sentinel rather than a
real call's continuation.

## 9. Bridge's own validation procedure

A later independent validation task must prove, for a project-authored synthetic multi-unit fixture (no
commercial ROM required for this bridge's own validation):

- persistent D/A/SR/PC state is correctly shared and mutated across at least two independently
  generated block functions reached through `genesis_dispatch`, not reset or duplicated per unit;
- persistent work RAM (`GenesisRuntime.work_ram`) correctly retains a write made by one generated unit
  and observed by a later one;
- big-endian byte/word/long behavior is correct wherever §6 or a block function selects it;
- stack (`a[7]`) and call/return state are correctly preserved across a generated call/return pair
  using this bridge's dispatch, not the legacy `genesis_rom_startup` interpreter;
- **a project-authored, fully accepted, single-`M68kStaticBlock` synthetic program (for example an
  entry-address `MOVEQ #5,D0` immediately followed by `RTS`, with an explicit `FrontendCompletionContract`
  declaring that `RTS`'s address as `terminal_rts_address` and an independently chosen, invariant-
  satisfying address as `sentinel_return_pc`) produces `FrontendAnalysis.completion` containing that
  `RTS`'s full provenance and the declared sentinel, compiles, initializes the stack with the declared
  sentinel per §3's sentinel-initialization rule, executes, and reports `"result": "completed"` with the
  independently expected final `GenesisRuntime` state — `d[0] == 0x00000005`; `pc` equal to the declared
  sentinel; `a[7]` equal to its initial value plus four (the real `RTS` pop effect, §3); every other
  register and every `work_ram` byte outside the four sentinel-holding bytes unchanged from the initial
  state — the concrete acceptance vector this contract requires;**
- **the identical program, generated and executed with no `FrontendCompletionContract` declared at all,
  still produces the existing `return_context_missing` result unchanged — proving this correction adds
  no implicit completion and does not weaken or alter that category's existing behavior;**
- **a synthetic fixture that initializes the stack with a value equal to neither the declared sentinel
  nor any statically known call continuation produces `GENESIS_STOP`/`GENESIS_DIAG_RETURN_TARGET_
  MISMATCH` — the existing typed mismatch stop, unmodified — never `GENESIS_COMPLETE` and never a guess;**
- **a synthetic fixture where the exact same physical block containing the declared completion `RTS` is
  also reachable as the callee of a real, statically known `JSR`/`BSR` from elsewhere proves, by running
  that call path specifically, that the block returns `GENESIS_CONTINUE_AT_PC` to the real call's own
  continuation in that context — never `GENESIS_COMPLETE` — because the real stack value read in that
  context is the call's own pushed continuation, not the declared sentinel; this is the concrete
  proof that one context cannot change another context's behavior;**
- a second synthetic fixture whose completion `RTS` is reached only after at least one intermediate
  block confirms `GENESIS_COMPLETE` propagates correctly through `genesis_dispatch` and the driving loop
  (§4) and is never substituted by, or confused with, `GENESIS_STOP`;
- a third synthetic fixture that exhausts the instruction-budget bound before reaching either a stop or
  the declared completion `RTS` confirms the budget result is always `GENESIS_STOP`/
  `GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED`, never `GENESIS_COMPLETE`;
- a fixture whose declared `FrontendCompletionContract` fails one of §1.4's build-time-provable
  invariants (for example a sentinel that overlaps a mapped image interval, or a `terminal_rts_address`
  that does not name a real, discovered `RTS`) produces a build-time `FrontendRejected` under "no safe
  frontier stub available," never a `GenesisRuntimeStop` and never `GENESIS_COMPLETE`;
- **driver-level validation of §13.1/§13.2's completion surface, all performed through
  `tools/genesis_startup_bridge.py` itself (never a private bypass):**
  - **`--mode synthetic` with both `--completion-rts`/`--completion-sentinel` supplied**: the driver
    forwards `--synthetic-completion-rts`/`--synthetic-completion-sentinel` to the emitter; discovery
    validates the contract; the sentinel is initialized on the synthetic stack (§3); the declared `RTS`
    executes normally; the result is `"completed"` — this is the acceptance vector above, run
    end-to-end through the driver, not a direct emitter-API call;
  - **`--mode synthetic` with neither option supplied**: the emitter receives no completion arguments;
    `FrontendProgram.synthetic_completion` remains empty; the same program's empty-call-frame-stack
    `RTS` retains `return_context_missing` exactly as unmodified production behavior;
  - **only `--completion-rts` supplied**: driver exit code 8; no hashing, generation, compilation, or
    execution occurs;
  - **only `--completion-sentinel` supplied**: driver exit code 8; no hashing, generation, compilation,
    or execution occurs;
  - **`--mode commercial` with either or both options supplied**: driver exit code 8; no hashing,
    generation, compilation, execution, or full-report creation of any kind occurs;
  - **both options syntactically valid but semantically violating §1.4** (for example a sentinel that
    overlaps a mapped interval): the emitter/discovery produces the documented build-time rejection
    (driver exit code 1); no generated program is ever compiled;
  - **the accepted synthetic completion fixture is generated, compiled, and executed exclusively
    through `tools/genesis_startup_bridge.py`** — proof that no test or later implementation
    instantiates `FrontendProgram` directly, calls the emitter API directly, or implements a private
    generate/compile/run path, matching §13.2's own stated requirement;
- generated C is deterministic byte-for-byte across repeated generation of the same fixture/options,
  including the embedded ROM SHA-256 (§10);
- generated C compiles strictly as C11 (`-std=c11 -Wall -Wextra -Werror -pedantic`) and every generated
  or runtime-support type is genuinely standalone C11 (no C++ type, `std::` name, or `enum class`
  reachable from the compiled translation unit — an architecture-inspection check, not only a compiler
  check, since some invalid constructs a permissive compiler might still accept are still forbidden by
  this contract);
- `genesis_dispatch` correctly reaches every statically known PC target and no other;
- an unknown-PC or unsupported-form condition produces a safe `GENESIS_STOP`, never a guess or crash;
- no mutation occurs after a `GenesisRuntimeStop`-producing operation (the last-committed `GenesisRuntime`
  state is provably unchanged by the failing operation itself);
- retained source/address/access provenance on every `GenesisRuntimeStop` matches the fixture's known
  answer exactly, **field by field against §2.1's disposition table — every disposition-(1)/(2) field
  is present with the expected value, every disposition-(3) field's authoritative equivalent is present
  and correct, and no field is silently absent that the disposition table requires to be present for
  the fixture's own category**;
- a synthetic fixture whose verified instruction length or mapping-claim name exceeds §2.1's fixed
  wire capacity produces a build-time `FrontendRejected` under "no safe frontier stub available," never
  a truncated `GenesisRuntimeStop`;
- every T029 device-region access is unconditionally fail-closed (§6), including the shape T021's
  translation-time selector accepts;
- the ROM-hash binding (§10) correctly detects a deliberately mismatched input in a negative fixture;
- the driver/report protocol (§13) round-trips the canonical wire schema for both the `"completed"` and
  `"stop"` results, correctly distinguishes sanitized from full reports, correctly enforces
  commercial-mode's stdout/log/persistence restriction in a negative fixture, and **correctly performs
  a `--compare-runs` two-execution comparison entirely in-process (no temporary file), reporting
  `reports_match: true` for two runs of the same deterministic fixture and the distinct `--compare-runs`
  mismatch exit code (§13.4) for a fixture deliberately made non-deterministic (for example one that
  reads uninitialized memory) to produce differing full reports**;
- no second decoder, IR, CFG, or CPU-semantic owner exists (architecture inspection, per §7 of [the
  Genesis startup shared-route ownership contract](genesis-startup-shared-route-ownership-contract.md));
- generated code performs no runtime target-byte fetch or decode (grep/architecture inspection over the
  generated output, as today's migration contract already requires); and
- the generated bridge program links and runs with no Musashi dependency (§8).

## 10. ROM/data ownership and lifetime

**Chosen option: (a), with a narrowly scoped carve-out (ADR 0006, SEG-007-T077).** The authorized ROM
is a generation-time-only input. Every byte this bridge's generated program needs is fully resolved
during static discovery/decode/lift, and the generated program never reads the original ROM *file*
again at runtime, under any revision of this option — not through §6's boundary, not embedded as a data
blob read live from that file, and not via any other path. This is the strictest of the three offered
options and was chosen because nothing in T026's identified frontier (a controller-I/O *device* access,
not a ROM *data* access) required runtime ROM reads, and the minimal-need principle (this document and
[the M68000 pipeline migration contract](m68k-pipeline-migration-contract.md) alike) forbids adding a
capability no currently scoped work exercises.

ADR 0006 (SEG-007-T077) narrows this only as follows, exactly the scoped instance of option (c) this
section's own "Invariant binding SEG-007-T029" paragraph (below) anticipated and required its own
architecture decision for: a runtime-computed (non-constant-foldable) effective address reaching §6's
boundary for a **read** may resolve successfully against a **statically owned, generated, build-time-
embedded, read-only copy** of a `MappingClaim` region discovery already proved — never against a live
read of the original ROM file, never against a region discovery did not prove, and never for a write.
The static proof obligation covers the region's mapping, ownership, immutability, bounds, and the
generated backing data itself; it never requires the exact effective address or the exact returned
value to be known at translation time, which is what distinguishes this from §11's narrower scalar
constant-fold precedent. Every other element of option (a) — no runtime read of the original ROM file,
no runtime ROM write, no runtime instruction fetch/decode, and fail-closed behavior for every address
outside every proven region/window — remains exactly as strict as before this widening. See ADR 0006
(`docs/decisions/0006-generic-cartridge-data-region-ownership.md`) for the full proof obligation and the
resolution of the apparent tension between a single, whole-ROM-spanning `MappingClaim` (as today's
production bridge generation actually constructs it) and this section's own "not a general-purpose
runtime read path for arbitrary addresses" non-goal.

This keeps every commercial-derived byte out of Git regardless of implementation detail, because the
entire generated bridge program — including any translation-time-folded scalar constant a §11 read
produces — is itself a local, gitignored generation artifact under the existing project rule that no
generated source is committed; nothing this bridge produces is ever a candidate for a Git commit (§13's
driver enforces this operationally by requiring its output directory to already be gitignored).

### Operational ROM-identity hash binding

ROM identity is hash-bound end to end, not merely asserted because a hashed file happened to be the
generation command's input:

1. Before generation, the driver (§13) computes the authorized local input's SHA-256 and, if the caller
   supplied an expected pinned hash, verifies equality before proceeding; a mismatch is a driver failure
   (§13's exit-code table), never a generation attempt.
2. The `emit-general-startup-bridge-c` CLI subcommand (§13) takes this SHA-256 as an explicit input and
   the emitter embeds it, as a normalized 64-lowercase-hex-character constant, into the generated C
   source (`static const char GENESIS_BRIDGE_ROM_SHA256[65] = "...";`) and into every wire report the
   compiled program later produces (`rom_sha256`, §13).
3. After compiling and running the generated program, the driver reads the executable's reported
   `rom_sha256` field and verifies it exactly equals the SHA-256 it computed from the input file in
   step 1, before accepting that run's result as valid evidence.
4. A mismatch at either step 1 or step 3 is a driver/build failure with its own exit code (§13); it is
   never represented as a `GenesisRuntimeStop`, a `GENESIS_COMPLETE` result, or conflated with a
   legitimate CPU/device frontier.

Recording this SHA-256 is within existing project policy: [the local commercial game corpus
policy](../testing/commercial-games.md) and T026's own evidence already record hash/provenance as the
approved minimum non-infringing evidence category. A 64-character hex digest exposes no ROM byte,
address, offset, opcode, or disassembly by itself.

### Invariant binding SEG-007-T029

As a binding invariant on SEG-007-T029: the generated program never fetches ROM *instructions* for
runtime opcode decoding under any future revision of this option. Only already-statically-resolved
immutable data may ever be read again at runtime; §6 states the exact defensive fail-closed category
(`GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY`) a ROM read must return when it cannot be proven against
such data. This paragraph anticipated that a later, separately evidenced task might widen this to option
(b) or (c) and required its own architecture decision to do so; ADR 0006 (SEG-007-T077) is that decision,
and it widens only to a narrowly scoped instance of option (c) — a proven-immutable, generated,
build-time-embedded region, never the original ROM file, never instruction bytes. Restated as unchanged
by ADR 0006: a generated program must never fetch or decode ROM *instruction* bytes at runtime, under
any revision of this option, and never opens or reads the original ROM file at runtime.

## 11. Static folding versus runtime routing precedence

A read may remain a translation-time constant only when its target region and value are both proven
immutable and read-only at discovery time — the same category the existing `test_operand_value`/
`M68kAbsoluteTestOperand` legacy folding behavior already uses for `TST.L absolute_long` on the
unrelated `direct_flow`/legacy routes. Under §10's option (a), this is limited in practice to ROM reads
whose address and byte content are both statically fixed and verified, matching today's precedent
exactly; the constant-folded value becomes an ordinary C literal baked into the operation's generated
effect, never a runtime ROM access.

Every one of the following **must** route through §6's boundary at runtime instead of being folded,
with no exception, for this bridge:

- every write of any kind (including a write whose target address is statically known);
- every read of persistent work RAM (mutable by definition, so never provably constant);
- every device-region access (controller-I/O or any future device region), regardless of apparent
  determinism — this is this bridge's own policy, distinct from and not modifying the existing legacy
  `test_operand_value`/`TST.L`-specific folding path other routes still use unchanged; and
- every access whose target address is not proven statically constant (§7's `unresolved_indirect_
  target`/`unsupported_memory_region` frontiers apply instead of folding).

No side-effectful or device-region access may ever be folded as a constant regardless of any apparent
immutability. This decision changes no existing resolver precedence and no existing fail-closed
category: `effective_address_not_24bit` then `odd_effective_address` then region resolution remain
authoritative exactly as [the controller-I/O compatibility policy](genesis-controller-io-startup-read-compatibility-policy.md)
already states, and every currently fail-closed device/memory category remains fail-closed unless a
future, separately evidenced task explicitly widens it.

## 12. Generated artifact structure

A generated unit is a **plain C function** (`static GenesisControlTransfer genesis_block_%08X(...)`,
§3) — not a separate C translation unit. All generated units for one bridge program, plus
`genesis_dispatch` (§4), are emitted together into **one** generated `.c` file, exactly matching the
single-file emission style `emit_m68k_frontend_c`/`emit_m68k_structured_direct_flow_c` already use.

The pure-C11 ABI types (§2, §2.1), `genesis_route_access` (§6), and `genesis_internal_dispatch_
inconsistency_stop`/the instruction-budget-counting driving loop (§4, §7) are declared and defined once
each, in a small, project-owned, non-generated pair of files that do not vary with ROM content:

- `tools/genesis_startup_bridge_runtime.h` — declares `GenesisRuntime`, `GenesisOwnedCartridgeRegion`
  (ADR 0006, SEG-007-T077), `GenesisAccessWidth`, `GenesisAccessDirection`, `GenesisStopClass`,
  `GenesisCpuVariant`, `GenesisInstructionProvenance`, `GenesisBusKind`, `GenesisBusRegion`,
  `GenesisBusAccess`, `GenesisMappingClaim`, `GenesisDiagnosticCategory`, `GenesisProvenance`,
  `GenesisRuntimeStop`, `GenesisControlTransferKind` (including `GENESIS_COMPLETE`),
  `GenesisControlTransfer`, `GenesisAccessResultKind`, and the `genesis_route_access`/`genesis_internal_
  dispatch_inconsistency_stop` prototypes.
- `tools/genesis_startup_bridge_runtime.c` — defines `genesis_route_access`,
  `genesis_internal_dispatch_inconsistency_stop`, and the instruction-budget-counting driving loop that
  calls `genesis_dispatch` in a bounded cycle (§4, §7) and, on `GENESIS_STOP` or `GENESIS_COMPLETE`,
  writes the wire report (§13).

Each generated `bridge.generated.c` file `#include`s `genesis_startup_bridge_runtime.h` and is compiled
and linked together with `genesis_startup_bridge_runtime.c`; the generated file itself never redefines
any ABI type.

The minimal granularity sufficient for the first executable partial program is **one function per
discovered `M68kStaticBlock`** (§3): this is the smallest existing structural unit with a stable
identity (`m68k_block_%08X`), a well-defined single-entry/single-fallthrough-or-branch-exit boundary,
and no cross-block aliasing, so it needs no new discovery concept. A per-instruction granularity would
multiply call overhead and dispatcher table size with no compatibility benefit; a per-frame granularity
(`M68kStaticFrame`) is coarser than needed for the first bridge, since a frame may span many blocks
whose individual frontier points must still be distinguishable.

This document explicitly rejects any speculative multi-file, multi-translation-unit, or build-system
change beyond this: one generated `.c` file per bridge program, compiled and linked against the one
fixed `.h`/`.c` runtime-support pair, with **no** new CMake target, library, or build option. This
matches the currently scoped synthetic bridge validation (§9) and the eventual Sonic partial program
alike; neither requires more structure than this.

## 13. Compile-and-run driver ownership and wire protocol

One project-owned, documented, non-test tool: `tools/genesis_startup_bridge.py`. It performs the full
generate → compile → run → validate cycle for a `general_startup` partial program end to end, using the
exact, deterministic, versioned protocol below, so SEG-007-T029 has no report layout, exit code,
privacy mode, or IPC mechanism left to invent.

### 13.1 CLI

```text
tools/genesis_startup_bridge.py \
  --segarecomp <path to the segarecomp binary> \
  --cc <path to a C11 compiler> \
  --rom <path to the authorized local ROM> \
  --entry <hex program entry address> \
  --out-dir <output directory; defaults to build/genesis-startup-bridge/> \
  --mode {synthetic|commercial} \
  [--expect-sha256 <64-lowercase-hex-char pinned SHA-256 to verify the ROM against>] \
  [--full-report-path <path>] \
  [--compare-runs] \
  [--completion-rts <even 24-bit hex address> --completion-sentinel <even 24-bit hex address>]
```

`--out-dir` must resolve to a path the project's `.gitignore` already covers (its default,
`build/genesis-startup-bridge/`, already satisfies this via the existing `build/` ignore rule); the
driver refuses to run (exit code 6, §13.4) if an explicitly supplied `--out-dir` is not already
ignored, so a generated artifact can never be accidentally committed.

`--full-report-path` requests a plain on-disk copy of the full report (§13.3) for direct inspection.
It is accepted only with `--mode synthetic`; the driver refuses to run (exit code 8, §13.4) if it is
supplied together with `--mode commercial`, since a commercial run's full report must never touch disk
(§13.6). It is unrelated to, and never combined in the same execution with, `--compare-runs`'s
in-process transport (§13.2/§13.6), which is available in both modes.

`--compare-runs` requests the two-execution, in-process comparison described in §13.2/§13.6. It never
accepts, and this contract defines no flag for, a caller-supplied prior run's report path or file:
comparison is always performed by exactly one driver invocation that launches and owns both runs
itself. The earlier cross-invocation comparison flag this revision removes — which accepted a prior
invocation's temporary-file path while the same design required deleting that file when the prior
invocation ended — is removed entirely; no caller-re-supplied-report-path mechanism of any kind
survives in this contract.

`--completion-rts`/`--completion-sentinel` are the driver's paired, synthetic-only surface for
constructing §1.4's `FrontendCompletionContract` — the sole project-owned way to reach that contract at
all, so the accepted synthetic completion fixture never has to bypass the driver or invent an
undocumented argument. Their semantics are exact:

- Both options must be supplied together, or both omitted entirely. Supplying exactly one is invalid
  argument usage: the driver refuses to run with exit code 8 (§13.4), before any hashing, generation,
  compilation, or execution.
- They are accepted only with `--mode synthetic`. Supplying either one (or both) with `--mode
  commercial` is also exit code 8 (§13.4), checked before any hashing, generation, compilation, or
  execution — the same precedence `--full-report-path`'s commercial-mode rejection already uses.
- The driver parses only their syntax (each must be an even, 24-bit-clean hex address, the same
  well-formedness check every other direct address argument in this pipeline already applies); it
  performs no further validation of its own. Every architecture-level invariant (naming a real
  discovered `RTS`; the sentinel not overlapping any mapped interval, discovered instruction, block
  entry, known continuation, or routable device/memory address) remains exactly §1.4's existing
  build-time-provable-invariant responsibility, checked by `discover_m68k_general_startup` itself, not
  duplicated or pre-validated by the driver.
- Omitting both leaves `FrontendProgram.synthetic_completion == std::nullopt` for that run; no implicit
  completion declaration exists under any argument combination.
- `--completion-rts` maps to `FrontendCompletionContract.terminal_rts_address`; `--completion-sentinel`
  maps to `FrontendCompletionContract.sentinel_return_pc` (§13.2 states the exact forwarding to the
  emitter). Meanings of `--full-report-path`, `--compare-runs`, commercial-mode privacy handling, and
  every existing exit code are unchanged by these two options.

### 13.2 Generation, compilation, execution

**Argument validation (before step 1, before any hashing, generation, compilation, or execution).**
The driver first validates `--completion-rts`/`--completion-sentinel` (§13.1): if exactly one is
supplied, or if either is supplied together with `--mode commercial`, the driver exits immediately with
exit code 8 (§13.4) and performs no hashing, generation, compilation, or execution of any kind. If
both are supplied and `--mode synthetic` is active, validation proceeds to step 1 below with both
values held for step 2's emitter invocation. If neither is supplied, validation proceeds to step 1
exactly as it already did before this correction, with no completion arguments involved at all.

**Emitter CLI wiring.** `emit-general-startup-bridge-c` gains one paired, optional argument set:

```text
emit-general-startup-bridge-c \
  --rom <rom> \
  --entry <entry> \
  --rom-sha256 <sha256> \
  [--synthetic-completion-rts <address> \
   --synthetic-completion-sentinel <address>]
```

- `--synthetic-completion-rts` and `--synthetic-completion-sentinel` are paired: both or neither. Their
  `synthetic-` prefix is normative, not decorative — it communicates, at the call site itself, that
  these are harness metadata the caller declares, never a ROM/hardware fact discovery derives.
- When both are present, the emitter constructs exactly
  `FrontendCompletionContract{.terminal_rts_address = parsed_completion_rts, .sentinel_return_pc =
  parsed_completion_sentinel}` and assigns it to `FrontendProgram.synthetic_completion` before calling
  `discover_m68k_general_startup`. When absent, the emitter leaves that field empty (`std::nullopt`).
- A malformed or incomplete pair (not both present, or either fails the same even-24-bit-clean syntax
  check §13.1 already applies) is rejected before discovery is even invoked — a build-time
  `FrontendRejected`, driver exit code 1, exactly like any other rejected generation.
- A syntactically valid pair whose semantic invariants fail is rejected by `discover_m68k_general_
  startup`'s existing §1.4 build-time validation under "no safe frontier stub available" — the emitter
  does not duplicate or pre-check those invariants itself; §1.4 remains their one owner.
- No completion value is ever inferred from the ROM, the entry address, `return_context_missing`, the
  stack, or any other diagnostic — the only two sources for `FrontendCompletionContract`'s fields are
  these two explicit arguments.
- These arguments alter no existing route or emitter command: `emit-m68k-frontend-c`, `emit-direct-
  flow-c`, `analyze`, and every other existing `segarecomp` command are completely unaffected, and
  `emit-general-startup-bridge-c` itself behaves exactly as before whenever both arguments are omitted.

**Driver-to-emitter forwarding** (the exact, only mapping; §13.1 states the driver-side validation that
gates it):

```text
--completion-rts      -> --synthetic-completion-rts
--completion-sentinel -> --synthetic-completion-sentinel
```

The driver never forwards either emitter option in commercial mode, per §13.1's argument validation
above.

1. Compute `input_sha256 = SHA256(open(--rom, "rb").read())`. If `--expect-sha256` was given and
   differs, stop immediately with exit code 4 (§13.4); no generation is attempted.
2. Invoke the emitter. **Ordinary synthetic or commercial run** (no `--completion-rts`/`--completion-
   sentinel` supplied): `<segarecomp> emit-general-startup-bridge-c --rom <rom> --entry <entry>
   --rom-sha256 <input_sha256>`, leaving `FrontendProgram.synthetic_completion` empty. **Synthetic
   completion fixture** (both `--completion-rts`/`--completion-sentinel` supplied, `--mode synthetic`
   only, per the argument validation above): `<segarecomp> emit-general-startup-bridge-c --rom <rom>
   --entry <entry> --rom-sha256 <input_sha256> --synthetic-completion-rts <completion-rts>
   --synthetic-completion-sentinel <completion-sentinel>` — the driver forwards `--completion-rts`'s
   value to `--synthetic-completion-rts` and `--completion-sentinel`'s value to `--synthetic-
   completion-sentinel` unchanged; it never forwards either emitter option when `--mode commercial` is
   active, matching the argument validation above. This is the **only** driver/emitter path that
   populates the completion contract; the accepted synthetic completion fixture must use this exact
   command through `tools/genesis_startup_bridge.py` and must not instantiate `FrontendProgram`,
   invoke the emitter API directly, or implement a private generate/compile/run path inside a test.
   In both cases the driver captures stdout and the process exit status; a rejected generation (the
   `FrontendRejected` case, emitted as the existing `/* translation rejected: ... */` convention,
   including a syntactically valid but semantically invalid completion pair rejected by §1.4's
   build-time-provable-invariant check) is a driver exit code 1; nothing is written to `--out-dir` as
   compilable source in this case.
3. On acceptance, write the captured C source to the fixed filename `<out-dir>/bridge.generated.c`
   (overwritten each run — deterministic, not uniquely named per run, so repeated invocations are
   directly comparable).
4. Compile: `<cc> -std=c11 -Wall -Wextra -Werror -pedantic -o <out-dir>/bridge <out-dir>/
   bridge.generated.c tools/genesis_startup_bridge_runtime.c`. A nonzero compiler exit is driver exit
   code 2. The resulting `<out-dir>/bridge` executable is used, unmodified, for every execution below —
   `--compare-runs`'s two runs launch this exact same binary twice, never regenerating or recompiling
   between them.
5. Execute. The generated program itself always writes exactly one line of canonical JSON (§13.3, the
   **sanitized** report) to stdout, terminated by `\n`, and exits 0 for either a well-formed
   `"completed"` or `"stop"` result (§13.3 distinguishes "well-formed" from "crashed"; a crash — a
   nonzero exit or any stdout content that is not exactly one parseable sanitized-report line — is a
   driver exit code 3, distinct from a legitimate result of either kind, which is always exit 0 from
   the generated program itself). How the full report (§13.3) is produced depends on the arguments
   below:
   - **No `--full-report-path` and no `--compare-runs`**: the generated program is invoked with
     neither argument; no full report is produced at all, by either mode. This is the default,
     lowest-privilege execution.
   - **`--full-report-path <path>` (synthetic mode only, §13.1)**: the driver invokes the program with
     `--full-report-path <path>`; the program writes the full report to that exact path as a plain
     file, in addition to the sanitized report on stdout. This path is caller-supplied and persists
     after the driver exits — an ordinary synthetic-mode inspection artifact, unrelated to `--compare-
     runs`'s transport.
   - **`--compare-runs` (either mode)**: see the dedicated sequence below. No `--full-report-path`
     argument is ever passed to either of the two child invocations `--compare-runs` performs, in
     either mode; the full report is transported exclusively over an anonymous pipe, never a path.
6. Parse the sanitized-report line. Validate `schema_version == 1` and `rom_sha256 == input_sha256`
   (from step 1); a mismatch or malformed/missing field is driver exit code 4 (hash mismatch) or 5
   (protocol/schema violation) respectively (§13.4). Only after this validation succeeds is the run's
   result accepted as valid evidence.

**`--compare-runs` sequence** (replaces the removed cross-invocation comparison flag; performed after step 4 above,
using the one compiled `<out-dir>/bridge` binary, instead of step 5's single execution):

1. For run 1: create one OS anonymous pipe (`read_fd`, `write_fd`). Spawn `<out-dir>/bridge
   --full-report-fd <write_fd>` (the write end is inherited by the child; the child's other arguments
   are identical to run 2's) with the child's stdout captured separately for the sanitized report. The
   driver closes its own copy of `write_fd` immediately after spawning (standard anonymous-pipe
   pattern), reads all bytes from `read_fd` until the child closes `write_fd` (end of stream), then
   closes `read_fd`. The child writes exactly one canonical full-report JSON record (§13.3) to
   `write_fd` and closes it when done, using the exact same `GenesisRuntime`/result the sanitized
   report on its stdout also describes.
2. For run 2: repeat step 1 exactly, using a **fresh** pipe pair and a fresh spawn of the same
   `<out-dir>/bridge` binary with identical input, options, and arguments (aside from the pipe's own
   file descriptor number, which is a per-spawn OS artifact, not a semantic option).
3. Both runs' sanitized reports (from each spawn's stdout) and full reports (from each spawn's pipe)
   are now entirely in driver process memory; nothing was written to any filesystem path for either.
4. Validate both full reports' `schema_version` and `rom_sha256` exactly as step 6 above; a violation in
   either run is that run's already-defined distinct exit code (3 crash, 4 hash mismatch, 5 schema
   violation) — `--compare-runs` never masks or replaces these.
5. Compare: byte-for-byte equality of the two runs' canonical full-report JSON encodings (§13.3's fixed
   field order and encoding make this equivalent to structural equality for two runs of the identical
   binary against identical input) — this is the one explicitly selected comparison method; no separate
   field-wise/structural comparator is defined or needed.
6. Immediately after the comparison, the driver discards both in-memory full-report byte buffers (drops
   its only references to them) and ensures every pipe file descriptor from both runs is closed —
   guaranteed via a `try`/`finally`-style construct that runs this cleanup even if step 4's validation
   or step 5's comparison raised an error.
7. Output: the driver's stdout/return value is the sanitized report derived from run 1, with one
   additional boolean field, `reports_match` (§13.3), appended. If both runs succeeded and `reports_
   match == true`: driver exit code 0. If both runs succeeded and `reports_match == false`: driver exit
   code 7 (§13.4) — a valid comparison that detected different full reports. In synthetic mode only, the
   driver may additionally return both full reports verbatim to its own caller (a human, a test, or a
   later validation task) for direct inspection, since project-authored synthetic fixtures carry no
   commercial-ROM privacy concern (§13.6); in commercial mode neither full report is ever included in
   this output.

"Parsing a typed report" means parsing this defined JSON wire protocol from the child process's stdout
(and, when used, its `--full-report-path` file or `--compare-runs`'s anonymous pipe) — never attempting
to exchange an in-process C structure directly with the Python driver.

### 13.3 Canonical report schema

Both reports are single-line, UTF-8, canonical JSON: keys emitted in exactly the fixed order below (not
alphabetical, not dict-insertion-order-dependent), 32-bit values as `"0xHHHHHHHH"` (8 hex digits,
matching the existing `hex(value, 8)` convention used throughout this repository's other generated-C
reports), 16-bit values as `"0xHHHH"` (4 hex digits), byte buffers as lowercase-hex or base64 as noted,
and a trailing `\n` with no other content on the line. `result` is an explicit discriminant with exactly
two values, `"completed"` or `"stop"`; every reader must branch on it rather than inferring the result
kind from which other fields are present.

**Sanitized report** (always written to stdout by the generated program, in both modes; safe to print,
log, or record as backlog evidence, matching T026's existing sanitization discipline):

```json
{
  "schema_version": 1,
  "report_kind": "sanitized",
  "rom_sha256": "<64 lowercase hex chars>",
  "result": "stop",
  "stop_class": "unsupported_device_access",
  "diagnostic_category": "unsupported_device_region_controller_io",
  "cpu_dimensions": null,
  "c4_lowering_dimensions": null
}
```

or, for a completed program:

```json
{
  "schema_version": 1,
  "report_kind": "sanitized",
  "rom_sha256": "<64 lowercase hex chars>",
  "result": "completed",
  "stop_class": null,
  "diagnostic_category": null,
  "cpu_dimensions": null,
  "c4_lowering_dimensions": null
}
```

The canonical sanitized key order is `schema_version`, `report_kind`, `rom_sha256`, `result`,
`stop_class`, `diagnostic_category`, `cpu_dimensions`, `c4_lowering_dimensions`; `reports_match`, when
requested, remains appended last. `stop_class`, `diagnostic_category`, `cpu_dimensions`, and
`c4_lowering_dimensions` are the one fixed rule this document states for `"completed"`: all four are
always `null` (never omitted, never a placeholder value) when `result
== "completed"`, since a completion has no stop diagnostic (§5). When `result == "stop"`,
`cpu_dimensions` is `null` unless `stop_class` is `unsupported_cpu_form`, in which case it is an object
with only `{"family": "...", "size": "...", "addressing_mode_class": "..."}` — the same safe CPU
dimensions T026's evidence discipline already permits, never a raw address, offset, opcode/extension
word, disassembly, or RAM byte. When `--compare-runs` is used, the sanitized report additionally carries
one appended field, `"reports_match": true` (or `false`), as its final key.

`c4_lowering_dimensions` is `null` for every stop other than `c4_lowering_gap`. A `c4_lowering_gap`
stop has `cpu_dimensions: null` and a non-null, finite whitelisted object `{"family":"..."}` selected
by that generated stop function. An `unsupported_cpu_form` retains its existing non-null
`cpu_dimensions` rule and has `c4_lowering_dimensions: null`.

**`cpu_dimensions` is representable per the actually-reached `result`/`stop_class` of one execution, never
per the whole compiled program (SEG-007-T073).** The generated program's `GenesisReportMetadata` compiles
in a single static `cpu_dimensions` value derived at emission time from *any* `unsupported_cpu_form`
frontier the emitter found anywhere in the source program, independent of whether that particular frontier
is ever actually reached by any one execution's dispatch (a program's dispatch can instead reach a
completion or a different stop class entirely, including the defensive
`GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY` fallback §3 already defines for a PC outside every
statically discovered block). The report-writing path's metadata-validity gate consults that compiled
static value only when the actually-reached `stop_class` is `unsupported_cpu_form`; for a completion or
for every other actually-reached `stop_class`, it must accept the report and the wire report must always
emit `cpu_dimensions: null`, regardless of what static value happens to be compiled into the same
program's `GenesisReportMetadata`. A validity gate that instead required the compiled static value to be
absent (`GENESIS_CPU_DIMENSIONS_NONE`) whenever the actually-reached stop was not `unsupported_cpu_form`
would incorrectly reject an otherwise valid, already-well-formed stop pair whenever some *other*, unreached
part of the same program also happened to have an `unsupported_cpu_form` frontier — this is a
report-representability defect, not a runtime-capability limitation, and correcting it adds no new stop
class, diagnostic category, or `GenesisCpuDimensions` family.

**Full report** (produced only via `--full-report-path` (synthetic mode only) or `--compare-runs`'s
anonymous pipe (either mode); never written to stdout, stderr, or any other path; §13.5/§13.6 govern its
handling):

```json
{
  "schema_version": 1,
  "report_kind": "full",
  "rom_sha256": "<64 lowercase hex chars>",
  "result": "stop",
  "runtime": {
    "d": ["0x........", "...x8"],
    "a": ["0x........", "...x8"],
    "usp": "0x........",
    "sr": "0x....",
    "pc": "0x........",
    "work_ram_base64": "<base64 of the complete 65536-byte GenesisRuntime.work_ram>"
  },
  "stop_class": "unsupported_device_access",
  "diagnostic_category": "unsupported_device_region_controller_io",
  "c4_lowering_dimensions": null,
  "provenance": {
    "has_instruction_provenance": true,
    "instruction": {
      "cpu_variant": "mc68000", "source_address": "0x........", "image_offset": 0,
      "primary_bytes": "hhhh", "length": 6
    },
    "has_access": true, "access_address": "0x........", "access_width": "long", "access_direction": "read",
    "mapping_claim_count": 1,
    "mapping_claims": [
      {"name": "cartridge_rom", "target_begin": "0x........", "target_end": "0x........",
       "image_begin": 0, "image_end": 0}
    ],
    "bus_access_count": 1,
    "bus_accesses": [
      {"ordinal": 0, "kind": "instruction_read", "address": "0x........",
       "raw_bytes": "hhhhhhhh", "raw_byte_count": 6, "region": "raw_cartridge_rom"}
    ]
  }
}
```

or, for a completed program:

```json
{
  "schema_version": 1,
  "report_kind": "full",
  "rom_sha256": "<64 lowercase hex chars>",
  "result": "completed",
  "runtime": {
    "d": ["0x........", "...x8"],
    "a": ["0x........", "...x8"],
    "usp": "0x........",
    "sr": "0x....",
    "pc": "0x........",
    "work_ram_base64": "<base64 of the complete 65536-byte GenesisRuntime.work_ram>"
  },
  "stop_class": null,
  "diagnostic_category": null,
  "c4_lowering_dimensions": null,
  "provenance": null
}
```

`runtime` is always present, for both results, and always the complete, final `GenesisRuntime` (§2's
"the shared instance is the retained state" rule applies equally to a stop and a completion). For
`"completed"` specifically, `runtime` is the state *after* the declared completion `RTS`'s full, real
effect (§3): `pc` equals the declared sentinel and `a[7]` equals its value immediately before that `RTS`
plus four, exactly as any other successfully executed `RTS` would leave them — never a pre-`RTS`
snapshot and never a state with that effect suppressed.
`stop_class`/`diagnostic_category`/`c4_lowering_dimensions`/`provenance` are always `null` together when `result == "completed"`;
when `result == "stop"`, `provenance` is `GenesisProvenance` (§2.1) serialized field-for-field exactly
as declared there, with every `has_*`/count field present so no field is ever silently omitted versus
silently zero. The full-report runtime object serializes `usp` immediately after `a`, matching the
runtime context's CPU-state declaration order. `runtime.work_ram_base64` carries the complete work-RAM
contents only in the full report; the sanitized report never contains RAM content in any form. `usp` is
never added to the sanitized report; commercial mode continues to prohibit persisted full reports.
The full report places `c4_lowering_dimensions` immediately after `diagnostic_category`; it follows the
same null/non-null rule as the sanitized report.

### 13.4 Driver exit-code table

| Exit code | Meaning |
| --- | --- |
| 0 | Generation, compilation, execution, and report validation all succeeded; the sanitized (and, if requested, full) report was parsed and hash-validated. With `--compare-runs`, this additionally requires `reports_match == true`. |
| 1 | Generation rejected (`FrontendRejected`, a build-time reject per §7). |
| 2 | Compilation failed. |
| 3 | Execution failed: the generated binary crashed, exited nonzero, or its stdout was not exactly one parseable sanitized-report line. With `--compare-runs`, either run failing this way produces this code. |
| 4 | ROM SHA-256 mismatch (§13.2 step 1, or either run's report in step 6/the `--compare-runs` sequence). |
| 5 | Report protocol/schema violation (wrong or missing `schema_version`, missing required field, malformed JSON) in any report the driver parses. |
| 6 | `--out-dir` is not an already-gitignored path (§13.1). |
| 7 | `--compare-runs` performed a valid, complete comparison of two successfully executed runs and found their full reports differ (`reports_match == false`). |
| 8 | Invalid argument combination: `--full-report-path` supplied together with `--mode commercial`; `--completion-rts`/`--completion-sentinel` supplied without its pair; or either `--completion-rts` or `--completion-sentinel` supplied together with `--mode commercial` (§13.1). Checked before any hashing, generation, compilation, or execution. |

### 13.5 Temporary-artifact lifetime

`--out-dir`'s contents (`bridge.generated.c`, the compiled `bridge` binary) persist across invocations
for synthetic-mode inspection and debugging, and are themselves covered by the existing "generated
source is never committed" rule via `--out-dir`'s mandatory gitignored location (§13.1).

`--full-report-path`'s file (synthetic mode only) is an ordinary caller-supplied path: the driver writes
it and leaves it in place for the caller to inspect and delete; it carries no privacy restriction (§13.6)
and is not a temporary file this driver manages.

`--compare-runs` creates **no temporary file of any kind**. Both full reports exist only as in-memory
byte buffers received over anonymous OS pipes for the duration of one driver invocation's comparison
(§13.2, §13.6) and are discarded (references dropped, descriptors closed) immediately after the
comparison, in a guaranteed cleanup path that runs on both success and error. Nothing is ever written to
any filesystem path for this comparison, in either mode.

### 13.6 Commercial-mode privacy handling (see §14 for the general rule)

In `--mode commercial`, the driver never writes a full report to stdout, stderr, or any persistent log
or file (`--full-report-path` is refused outright in this mode, §13.1). The only way a commercial run's
full report ever exists is transiently, in driver process memory, during a `--compare-runs` invocation
(§13.2): both runs' full reports are received over anonymous, driver-owned, in-process pipes — never a
named file, named pipe, or any path a second process could open — held only long enough to validate and
compare them, and discarded immediately afterward (§13.5). The driver's output in commercial mode is
always exactly the sanitized report (§13.3), plus `reports_match` when `--compare-runs` was used; the
full report's content is never included, printed, logged, or returned to the caller in commercial mode
under any argument combination.

Synthetic mode carries no such restriction: `--full-report-path` writes an ordinary on-disk full report
for inspection, and `--compare-runs` may additionally return both full reports verbatim to its own
caller (§13.2 step 7), since project-authored synthetic fixtures carry no commercial-ROM privacy
concern.

## 14. Runtime-stop privacy boundary

A generated bridge program's full report (§13.3), for either result, may retain full local provenance
(address, access, work-RAM content) for local inspection and for deterministic comparison performed
entirely in memory or via an on-disk synthetic-mode artifact (§13.5, §13.6). Any output a commercial
(authorized local ROM) run persists, prints, or commits must instead be reduced to the permitted
sanitized report (§13.3) already established by T026's evidence discipline: `result`, `stop_class`/
`diagnostic_category` (both `null` for `"completed"`) and, where CPU-related, only safe CPU dimensions
(family, size, addressing-mode classification) plus the approved ROM SHA-256 (§10) and, when `--compare-
runs` was used, the `reports_match` boolean — never a raw address, offset, opcode/extension word,
disassembly, RAM byte, or local path. Deterministic cross-run comparison (§13.2/§13.6) may consume the
complete, unsanitized full reports opaquely (byte-for-byte equality only, entirely in memory, never via
a temporary file) without ever printing, logging, or committing their private fields; only the sanitized
report may appear in any committed evidence, log, or backlog record.

**Scope correction (ADR 0005, `docs/decisions/0005-cli-static-report-outside-wire-report-privacy-
boundary.md`): this section governs, and has only ever governed, the compiled bridge binary's own wire
report (§13.3) — the report a `FrontendPartialProgram` produces once it has been lowered to C, compiled,
and executed. It does not govern, and has never governed, the CLI's own static discovery-time report
(`genesis-general-startup`, `format_m68k_frontend_result`), a separate, always-single-shape, always-full-
detail local tool output that has included full raw provenance (`source_address`, `provenance.raw_bytes`,
`mapping_claims`, `accesses`) by default, unconditionally, for a plain `FrontendRejected` result since
long before this contract's own SEG-007-T064 Checkpoint 7 amendment below — see `format_m68k_frontend_
result_legacy` and every prior real-ROM validation task in this milestone (SEG-007-T041 through T063) that
already exercised it. The immediately-following Checkpoint 7 amendment's own original text incorrectly
claimed otherwise (a parenthetical extending this section's scope to that CLI report); that claim is
withdrawn by ADR 0005 and is not, and was never, an accurate description of this project's own actual
behavior.**

**SEG-007-T064's Checkpoint 7/8 fields, described here for history/completeness only -- not governed by
this section, per ADR 0005 above.** `format_m68k_frontend_result`'s `partial` branch (the CLI's own static
discovery-time report, `genesis-general-startup`) carries, per frontier: `category`/`class` (already
existed before this task); `opcode_line` (ADR 0004), present only when `class ==
GENESIS_STOP_UNSUPPORTED_CPU_FORM` -- the standard, publicly documented MC68000 "opcode line" (the top 4
bits of the frontier's own primary instruction word), a coarse, 16-way, ISA-encoding-structure fact
identical for any program containing that bit pattern; `region_class` (ADR 0004), present only when
`class` is `GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS`/`GENESIS_STOP_UNSUPPORTED_MEMORY_REGION` -- one of
`raw_cartridge_rom`/`synthetic_work_ram`/`hardware_frontier` (the same buckets `probe-genesis-startup-
mapping` already computes unconditionally, with no ROM at all) or the already-cited GTO1 controller-I/O
window bucket, never a new, not-yet-independently-cited Z80-bus/VDP/other hardware region name; and
(SEG-007-T064 Checkpoint 8) `source_address`, `image_offset`, `provenance` (`source_address`/
`image_offset`/`raw_bytes`/`length`), `mapping_claims`, and (for the two access-bearing classes)
`access_address`/`access_width`/`access_direction` -- the exact same raw-provenance field set this
report's own pre-existing `rejected`-result branch has always carried unconditionally (see
`format_m68k_frontend_result_legacy`), restoring consistency between the two branches rather than
introducing new detail this report did not already, elsewhere, unconditionally provide. `opcode_line`/
`region_class` are absent (no key, never null) for every other class; the Checkpoint 8 fields are present
for every element of a `partial` result's own `"frontiers"` array, unconditionally, exactly as the
pre-existing `rejected` branch's own fields already are for a plain rejection. None of this affects the
compiled bridge binary's own wire report (§13.3), which this section's own privacy boundary continues to
govern exactly as strictly as before, unchanged in kind by either checkpoint.

## Non-goals

- Any implementation, fixture, or test. SEG-007-T029 implements exactly this contract's seam.
- Controller/device semantic work, VDP, rendering, audio, Z80, interactive input, six-button support,
  or any title-screen/framebuffer claim. Controller-specific behavior remains gated behind
  SEG-007-T031's outcome and whatever controller-implementation task a later SEG-007 refinement creates
  from it; §6's device lane structurally exists but is unconditionally fail-closed in T029 and
  authorizes no new device semantics on its own.
- Any new CPU instruction form, runtime opcode decoder/interpreter/JIT fallback, or full-ROM discovery.
- Redesigning or weakening any existing decode/mapping/provenance/diagnostic precedence, the legacy
  `genesis_rom_startup` route, or the unrelated `direct_flow` route. `return_context_missing`'s existing
  category, behavior, and precedence inside `discover_m68k_general_startup` are completely unchanged for
  every program that does not declare a `FrontendCompletionContract` (§1.4) for the exact `RTS` reached;
  `FrontendAnalysis` gains only two new optional fields (`completion`, populated exclusively when such a
  contract was declared and validated, §1.4) and `FrontendResult` gains only the one new third
  `FrontendPartialProgram` variant (§1.1) — no existing field's meaning changes for any program that
  does not opt in.
- Commercial-ROM inspection or any private/commercial-derived address, byte, opcode, disassembly, path,
  or trace in this document or any artifact it authorizes.
