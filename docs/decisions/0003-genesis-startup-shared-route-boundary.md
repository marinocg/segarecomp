# ADR 0003: Genesis Startup Shared-Route Boundary

- Status: Accepted
- Date: 2026-08-08 (revised 2026-08-08, 2026-08-09)
- Amends: ADR 0001, ADR 0002

## Context

SEG-005-T003 deferred merging Genesis startup analysis into SEG-003's direct-flow route because
startup needed call/return, absolute-memory, and RAM/bus facts that direct flow had no model for.
SEG-005-T004's ownership contract then assigned startup call/return, A7/RAM, and structured-C
lowering to single shared owners, and required that SEG-005-T008 leave "no startup-specific
projection, executor, or emitter."

An earlier revision of this ADR treated that requirement as ambiguous between "the startup and
direct-flow code must share literal control-flow" and "each profile may keep its own dedicated
executor/emitter as long as there is exactly one copy of it," and resolved the ambiguity in favor
of the second reading without actually removing the duplicated per-operation semantics that made
the first reading true. That was a definitional resolution, not an implementation one, and did not
satisfy T004's inherited contract: it left `execute_m68k_frontend_startup` and the
`genesis_rom_startup` branch of `emit_m68k_frontend_c` as self-contained interpreters/emitters that
independently implemented what each selected MC68000 operation *means* (register/CCR effect,
absolute-memory address/offset, call push, return pop/validate), rather than consuming that meaning
from a shared definition. The 2026-08-08 revision below corrected the C++ execution/lowering
ownership for the static-program representation, call/return identity, and address policy, but a
subsequent review found `execute_m68k_frontend_startup` still directly decided each selected
operation's *execution-semantic* meaning inline (rather than only applying it to host storage), and
`emit_m68k_frontend_c` still special-cased MOVEQ's PC-advance outside the shared C-lowering owner.
The first 2026-08-09 revision (see "Per-operation execution semantics have one owner each" and
"Structured-C lowering" below) closed both gaps with a shared `M68kOperationEffect`/
`m68k_operation_effect` execution-semantic layer and folding MOVEQ's PC-advance into
`emit_m68k_operation_c` — but that revision's `M68kOperationEffect` still reduced JSR/RTS to a bare
`control` marker (`call`/`ret`) with no `call_target`/return facts beyond JSR's target address, so
`execute_m68k_frontend_startup` still independently defined "JSR pushes 4 bytes and jumps to the
target" and "RTS pops 4 bytes and jumps to the observed value" — including one call site that
computed the shared effect for RTS and then discarded it with `[[maybe_unused]]`. A second
2026-08-09 revision closed this by extending `M68kOperationEffect` with typed `M68kStackEffectKind`/
`stack_width` and `M68kPcEffectKind`/`direct_target` fields (see the same two sections below), so the
effect layer is now the sole owner of every selected operation's stack push/pop *intent and width*
and PC-transition *kind*, not only its register/memory facts. T008's actual scope throughout has been
implementing the missing shared ownership, not redefining what "shared" requires.

`DirectFlowIrKind`/`DirectFlowAnalysis` (SEG-003) still represents only register-result and
short-branch operations over a bounded direct-flow CFG; it has no representation for call/return
frames, absolute-memory access, or RAM/bus state, and extending it to add those is out of scope
(new capability). So the shared MC68000 static-program representation for these facts cannot be
`DirectFlowAnalysis` reused wholesale — it has to be its own profile-neutral representation, as the
diagram below requires:

```text
validated profile ingress
        |
shared MC68000 decoded operations        (decode_m68k_instruction)
        |
shared MC68000 lifted operations         (lift_m68k_instruction)
        |
shared static-program / typed-edge repr. (M68kStaticBlock/M68kStaticEdge/M68kStaticCall/M68kStaticFrame,
        |                                  discover_m68k_static_call_return)
shared execution-semantic effect layer   (M68kOperationEffect/m68k_operation_effect: the sole decision
        |                                  of what each selected M68kIrKind requests -- register write,
        |                                  memory register/address, stack push/pop intent+width, and
        |                                  PC-transition kind (advance/direct target/observed return))
shared execution/lowering semantics      (m68k_move_result_ccr; m68k_startup_absolute_operand_alignment;
        |                                  m68k_startup_ram_operand_in_range/m68k_startup_ram_offset;
        |                                  emit_m68k_operation_c, owning every selected operation's
        |                                  full C lowering, including MOVEQ's PC advance)
        |
profile-specific application/reporting   (execute_op/execute_m68k_frontend_startup apply the shared
                                           effect to their own state/host RAM; StartupExecution/
                                           StartupFailure/StartupBusRecord/StartupBoundary report
                                           schemas; the C harness's RAM/frame-array declarations and
                                           printf footer)
```

## Decision

Full control-flow unification of startup and direct-flow analysis/execution/emission remains
rejected for the same structural reason as before: `DirectFlowIrKind`/`DirectFlowAnalysis` cannot
represent call/return or memory facts without new capability. But "no startup-only projection,
executor, or emitter" is satisfied only when every semantic fact that *can* be shared — decode,
lift, per-operation CCR update, absolute-operand address policy, RAM-window addressing, and every
selected operation's full C lowering — has exactly one implementation, used by every profile that
needs it, and no profile independently re-derives any of those facts. SEG-005-T008 implements this:

- **Static-program representation.** `M68kStaticBlock`, `M68kStaticEdge`, `M68kStaticEdgeKind`,
  `M68kStaticCall`, and `M68kStaticFrame` (renamed from their prior `Startup*` names) are the
  profile-neutral shared static-program representation named in T004's contract, held directly on
  `FrontendAnalysis` alongside `decoded`/`ir`. `M68kStartupOperation` — a struct that duplicated
  `decoded`/`ir`/`raw_bytes`/`extension` into a second per-operation record — is deleted;
  `analyze_startup_profile`, `execute_m68k_frontend_startup`, and `emit_m68k_frontend_c` all consume
  `FrontendAnalysis::decoded`/`ir` directly by index, so there is exactly one decoded/lifted copy of
  every selected operation, never a startup-only projection of it.
- **Call/return identity has one owner.** `discover_m68k_static_call_return` (T006) is the sole
  place the call/continuation/callee identity is computed. `execute_m68k_frontend_startup`'s JSR
  handling previously recomputed `continuation = source_address + 6` independently and
  reconstructed a local call record to compare against the discovered one; it now reads
  `analysis.static_frames.front().call` directly (with a defensive identity check against a forged
  analysis, not a re-derivation) and `emit_m68k_frontend_c` sources the same field for its generated
  continuation literal. Neither backend has its own formula for this fact.
- **Per-operation execution semantics have one owner each.** `M68kOperationEffect`/
  `m68k_operation_effect` is now the sole decision of what every selected `M68kIrKind` requests at
  execution time: MOVEQ's sign-extended D0 register write and `pc = advance`/`pc_delta = 2`; the
  absolute-long forms' memory register/address and `pc = advance`/`pc_delta = 6`; JSR's
  `stack = push_static_continuation`/`stack_width = 4` and `pc = direct_target`/`direct_target =
  <callee>`; and RTS's `stack = pop_static_return`/`stack_width = 4` and `pc =
  observed_stack_return`. SEG-003's `execute_op` and SEG-005's `execute_m68k_frontend_startup` both
  consume this function instead of independently redeciding it; previously
  `execute_m68k_frontend_startup` inlined all of this directly for every kind (and, for JSR/RTS, a
  first 2026-08-09 revision left the shared effect as a bare, largely-ignored `call`/`ret` marker —
  the RTS call site discarded its computed effect with `[[maybe_unused]]` — while the actual "push/pop
  4 bytes, jump to target/observed value" facts stayed independently defined inline; `execute_op`
  separately inlined MOVEQ's identical sign-extension/register write). What the effect layer
  deliberately does NOT decide: the pushed continuation *value* (still sourced only from
  `discover_m68k_static_call_return` via `analysis.static_frames`/`analysis.static_edges` — T006
  remains its sole owner), the observed return *value* (still a real host-RAM read the adapter
  performs and validates), and whether/how a requested effect succeeds against RAM/A7 bounds (still
  the adapter's job, since the effect layer has no memory, RAM-window, or call-identity model to
  reuse). `m68k_move_result_ccr` (already shared with SEG-003's `execute_op`) is unchanged. Two
  shared predicates, `m68k_startup_absolute_operand_alignment` and
  `m68k_startup_ram_operand_in_range`, plus the shared `m68k_startup_ram_offset`, remain the sole
  implementation of "is this MOVE.L Abs.L operand 24-bit-clean/even/in-range" and "what RAM byte
  offset does it project to." `execute_m68k_frontend_startup` also fails closed (`category =
  "invalid_startup_analysis"`) before any bus/RAM/A7/PC/frame mutation if `FrontendAnalysis::decoded`/
  `::ir` disagree in length or per-element identity (typed source address, image offset, verified
  length, decoded-kind-to-lifted-kind mapping, and operand/extension), via a small compatibility
  predicate independent of `lift_m68k_instruction`, and now also guards each JSR/RTS branch with a
  structural check that the shared effect actually has the expected stack/PC shape before applying
  it (a belt-and-suspenders check against a future inconsistency, not reachable via any current
  fixture).
- **Structured-C lowering has one owner for every selected operation, including MOVEQ's PC advance.**
  `emit_m68k_operation_c` previously only emitted the CCR-affecting text for
  `write_d0_absolute_long`/`read_absolute_long_d1` and nothing for `call_absolute_long`/
  `return_from_subroutine`; the RAM/stack access and control-transfer C text for all four of those
  kinds was written independently inside `emit_m68k_frontend_c`'s `genesis_rom_startup` branch. That
  text is now inside `emit_m68k_operation_c` itself, gated by an optional
  `M68kMemoryEmissionContext` (RAM/A7/frame-array identifiers and the one already-discovered
  continuation value) that only the kinds needing it require; as of 2026-08-09 this also includes
  MOVEQ's `"pc += 2U;\n"`, previously appended separately by `emit_m68k_frontend_c` itself.
  `emit_m68k_frontend_c`'s startup branch is now a thin loop over `analysis.ir` that calls this one
  function per operation, plus the harness scaffolding (variable declarations, the fixed report
  footer) that has no per-operation semantic content.

What legitimately remains profile-specific, per T004's own "profile-specific reporting/
instrumentation" allowance, is not semantic ownership:

- `analyze_startup_profile`'s fixed five-operation graph traversal (`analyze_startup_profile`) has
  no SEG-003 equivalent to reuse — SEG-003 has no call/return graph shape at all — so it is
  implemented exactly once, here, and nowhere else.
- `execute_m68k_frontend_startup`'s host-array RAM/stack simulation is likewise the only place any
  profile models memory at all; it *applies* the shared effect/address/CCR/identity facts above to a
  host buffer (address validation, backing storage, bus/boundary recording, A7/frame bookkeeping),
  but does not separately define what a selected operation's register write, memory access, stack
  push/pop, or PC transition means — that is `m68k_operation_effect`'s sole responsibility. JSR's
  push and PC-to-target application, and RTS's pop/validate/dispatch sequence, both stay here, since
  applying a requested stack effect against real A7/RAM bounds, and validating an observed return
  value against the discovered call/frame/edge identity, are runtime facts the effect layer has no
  way to know; the effect layer only supplies the requested stack kind/width and PC-transition kind
  (a direct jump for JSR, a jump to whatever the adapter observes on the stack for RTS) — never the
  pushed/observed value itself, and never whether the request succeeds.
- The `StartupExecution`/`StartupFailure`/`StartupBusRecord`/`StartupBoundary` report schemas and
  the C harness's variable declarations/printf footer are observability/serialization concerns, not
  instruction semantics.

## Consequences

Reviewers checking "no startup-only projection, executor, or emitter" must find, for each selected
operation, exactly one function that defines its decode, its CCR/state effect, its address policy
(where applicable), and its C lowering — and confirm the startup-specific code that remains is
limited to graph traversal shape, host-buffer application of already-shared facts, and
report/harness serialization. Any future instruction or state fact that is genuinely shared between
profiles must be factored into one exported function rather than reimplemented per profile; any
fact with no SEG-003 equivalent (because SEG-003's IR domain structurally cannot represent it)
belongs in exactly one place in the shared pipeline module, never duplicated.
