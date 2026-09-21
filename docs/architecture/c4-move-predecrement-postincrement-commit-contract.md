# C4 MOVE predecrement/postincrement deferred-commit contract (SEG-007-T069)

> **Correction note.** An earlier version of this record's Q3 rejected only the shape where source and
> destination were *both* `address_predec`/`address_postinc` on the same register, and separately
> claimed the asymmetric source-mutating/destination-non-mutating-same-register shape (for example
> `MOVE.W -(A0),(A0)`) was safely representable. That claim was a defect: under this record's own Q2
> mechanism, a non-mutating destination reads the live register array directly, which is still stale at
> that point when the source's own writeback is correctly deferred past both accesses. Q3's rejecting
> check is widened below to also cover that shape; see Q3 for the full trace and rationale. The reverse
> asymmetric shape (destination mutating, source non-mutating referencing the same register) was already
> correctly treated as representable and is unchanged.

## Purpose and boundary

This is a migration/mechanism contract for a follow-on implementation task, not an implementation,
fixture, or hardware claim. It resolves the design question SEG-007-T068 deliberately left open: how
`write_move`'s C4 block-emission case (`case M68kIrKind::write_move:` in `emit_m68k_operation_c`,
`src/m68k_pipeline.cpp`, currently lines 3510-3534) can lower a MOVE whose source and/or destination
effective address is `address_predec`/`address_postinc`, without ever letting a `GENESIS_STOP` reached
during that one instruction's lowering leave an address register partially mutated. It does not
implement that lowering; SEG-007-T069's own scope is architecture decision only (see
the SEG-007-T069 backlog record).

It consumes, and does not relitigate:

- The runtime frontier's own already-accepted invariant (§7,
  [`genesis-generalized-startup-runtime-bridge-contract.md`](genesis-generalized-startup-runtime-bridge-contract.md)):
  "A runtime frontier, by construction: applies no guessed semantic effect for the operation that
  triggered it; performs no partial memory/device mutation for that operation; retains the last
  committed `GenesisRuntime` state exactly." This contract's whole purpose is choosing a MOVE-specific
  mechanism that satisfies that existing invariant for the one operand shape it currently cannot
  represent.
- SEG-007-T068's evidence: after `write_move` was widened onto the shared resolver-fact/runtime-routing
  pattern `write_clr`/`test_operand` already use, the real Sonic ROM's authorized route now records a
  deterministic pre-emission `c4_prefix_lacks_retained_resolver_fact` FAIL from `require_fact`
  (`src/m68k_pipeline_frontend.cpp`, currently lines 2839-2872, specifically the unconditional
  predecrement/postincrement rejection at line 2870) whenever a `move`/`tst`/`clr` operand is
  `address_predec`/`address_postinc`.
- `movem_transfer`'s own already-adversarially-validated (SEG-007-T067, PASS) deferred-writeback
  technique for `address_predec`/`address_postinc` (`case M68kIrKind::movem_transfer:`, currently
  lines 3878-4249, with the predecrement/postincrement branches at lines 4012-4045): snapshot the
  address register into one local C variable, mutate only that local across the per-slot loop, route
  every slot through `genesis_route_access` via the existing `m68k_emit_routed_read`/
  `m68k_emit_routed_write` boundary (lines 3045-3079), and write the mutated local back into the live
  register-file array in exactly one statement, placed strictly after every slot's routed access has
  already succeeded.

## What was directly read to ground this decision

- `src/m68k_pipeline.cpp`: `write_move` (lines 3510-3534), `movem_transfer`'s predecrement/postincrement
  branches (lines 4012-4045), the shared EA primitives `m68k_emit_runtime_ea_address` (2985-3022),
  `m68k_emit_runtime_ea_guard` (3030-3036), `m68k_emit_routed_read`/`m68k_emit_routed_write`
  (3045-3079), `m68k_emit_ea_read` (3090-3160), `m68k_emit_materialized_ea_read` (3166-3179),
  `m68k_emit_ea_write` (3187-3239+), `m68k_is_statically_foldable_control_ea` (947-950), and every
  other `emit_m68k_operation_c` case that calls these primitives: `test_operand` (3343-3368),
  `compare`/`compare_immediate`/`compare_address` (3369-3395), `subtract`/`subtract_immediate`/
  `subtract_quick`/`subtract_address` (3396-3440), `add`/`add_immediate`/`add_quick`/`add_address`
  (3441-3474), `logical_and`/`logical_and_immediate`/`logical_or`/`logical_or_immediate`/
  `exclusive_or`/`exclusive_or_immediate` (3475-3509), `write_movea` (3535-3560), `write_clr`
  (3561-3578), `write_swap` (3589-3610), `sign_extend_word`/`sign_extend_long` (3611-3639),
  `load_effective_address` (3640-3666), `push_effective_address` (3673-3708), `link_frame`
  (3709-3739), `unlink_frame` (3740-3763), and `bit_test`/`bit_change`/`bit_clear`/`bit_set`
  (3764-3823).
- `src/m68k_pipeline_frontend.cpp`: `retain_fact` (1728-1773) and its per-`M68kInstructionKind` call
  sites (1774-1788), and `require_fact` (2839-2872) with its per-instruction-kind call sites
  (2873-2880).
- `include/segarecomp/m68k_pipeline.hpp`: the `M68kInstructionKind` enumerator list (242-271+) that
  names every currently selected instruction kind, to identify exactly which kinds share the EA
  primitives above.
- `docs/architecture/genesis-generalized-startup-runtime-bridge-contract.md` §6 (897-950, the
  `genesis_route_access` boundary) and §7 (952-1016, the build-time-rejection-versus-runtime-frontier
  distinction and the runtime-frontier no-partial-mutation invariant quoted above).

## Decision

**Selected mechanism: a MOVE-specific deferred-address-commit technique confined entirely to
`write_move`'s own block-emission case, generalizing `movem_transfer`'s already-validated
single-register snapshot/mutate-local/route/writeback-last technique to MOVE's independent
source-and-destination (at most two registers, at most one slot each) shape. The shared
`m68k_emit_ea_read`/`m68k_emit_ea_write`/`m68k_emit_runtime_ea_address` primitives are not
restructured.**

### Q1 — Scope: `write_move`-local technique, not a shared-primitive restructuring

`write_move` gains its own narrowly-scoped predecrement/postincrement handling, written directly in
its `case M68kIrKind::write_move:` block, exactly mirroring `movem_transfer`'s technique (snapshot the
touched address register(s) into local C variables, mutate only the locals, route every access through
the existing `m68k_emit_routed_read`/`m68k_emit_routed_write` boundary, write the final mutated
value(s) back into the live address-register array in one statement per touched register, strictly
after every routed access this instruction performs has already succeeded). It never calls
`m68k_emit_runtime_ea_address`/`m68k_emit_ea_read`/`m68k_emit_ea_write` for a source or destination
operand whose mode is `address_predec`/`address_postinc`; those primitives' existing general contract,
and every other EA mode `write_move` still routes through them unchanged (`data_register`,
`address_register`, `immediate`, `absolute_word`, `absolute_long`, `pc_disp16`, `address_indirect`,
`address_disp16`), is untouched.

This is chosen over restructuring the shared primitives because the primitives are reused, unchanged,
by every other currently selected `M68kIrKind` that can carry a memory-referencing EA. Restructuring
their general contract (for example, deferring every mode's register mutation into a caller-supplied
local rather than mutating the live register file directly in the prelude) would require independently
re-auditing generated-C behavior for all of the following current callers, none of which this task
touches:

| Caller (`M68kIrKind` case) | Corresponding `M68kInstructionKind`(s) | Predecrement/postincrement exposure today |
| --- | --- | --- |
| `test_operand` | `tst` | Source only; already unconditionally rejected by `require_fact` (line 2870), same as `move`/`clr`. Out of scope here (Non-goals: "any C4 IR kind other than `write_move`'s ... gap"). |
| `compare`/`compare_immediate`/`compare_address` | `cmp`, `cmpi`, `cmpa` | Source (materialized) and destination (plain read, no write). Never covered by `require_fact` (its switch checks only `move`/`tst`/`clr`) — a same-hazard-class gap left open by this decision. |
| `subtract`/`subtract_immediate`/`subtract_quick`/`subtract_address` | `sub`, `subi`, `subq`, `suba` | Source (materialized) and destination RMW (read via `m68k_emit_ea_read`, write retargeted to `address_indirect` so the mutation is never re-applied — see lines 3419-3424). Destination read's own prelude-mutates-before-access hazard is unguarded, same class left open. |
| `add`/`add_immediate`/`add_quick`/`add_address` | `add`, `addi`, `addq`, `adda` | Same RMW shape as subtract (lines 3457-3463). Same unguarded gap. |
| `logical_and`/`logical_and_immediate`/`logical_or`/`logical_or_immediate`/`exclusive_or`/`exclusive_or_immediate` | `logical_and`, `andi`, `logical_or`, `ori`, `eor`, `eori` | Same RMW shape (verified structurally identical to add/subtract by inspection of 3475-3509). Same unguarded gap. |
| `write_movea` | `movea` | Source only (materialized), no destination memory access (destination is always `An` direct). Never covered by `require_fact` (its switch does not include `movea`). Same unguarded gap. |
| `write_clr` | `clr` | Destination only; already unconditionally rejected by `require_fact`. Out of scope here, same as `tst`. |
| `write_swap` | `swap` | Destination is always `data_register` by ISA definition (no memory form exists) — not exposed to this hazard at all. |
| `sign_extend_word`/`sign_extend_long` | `ext_w`, `ext_l` | Destination is always `data_register` by ISA definition — not exposed. |
| `load_effective_address` | `lea` | Computes an address only (`m68k_emit_runtime_ea_address` directly, never `m68k_emit_ea_read`/`m68k_emit_ea_write`); LEA performs no memory access at all, so a predecrement/postincrement source still mutates the register with no corresponding routed access to fail — a distinct, narrower risk shape not addressed by this decision. |
| `push_effective_address` | `pea` | Its own push target is always `{address_predec, 7}` (A7); the `pea`'s own `source_ea` may independently be `address_predec`/`address_postinc` too. Never covered by `require_fact`. Same unguarded gap. |
| `link_frame` | `link` | Push target is always `{address_predec, 7}` (A7), unconditionally. Never covered by `require_fact`. Same unguarded gap. |
| `unlink_frame` | `unlk` | Pop source is always `{address_postinc, 7}` (A7), unconditionally. Never covered by `require_fact`. Same unguarded gap. |
| `bit_test`/`bit_change`/`bit_clear`/`bit_set` | `btst`, `bchg`, `bclr`, `bset` (bit-number source is never a memory EA) | Destination RMW, same shape as add/subtract (lines 3773-3781, 3804-3811). Same unguarded gap. |

This table is this decision's complete answer to "name every other current caller that would need
re-audit" if the shared primitives were restructured instead: at minimum `test_operand`, `compare`
family, `subtract` family, `add` family, the three logical families, `write_movea`, `write_clr`,
`push_effective_address`, `link_frame`, `unlink_frame`, and the four `bit_*` kinds. A restructuring
would touch every one of them; a `write_move`-local technique touches none of them. This decision does
not fix any of the "same unguarded gap" rows above (all remain out of scope, per this task's
Non-goals); it records them only because Q1 requires this exact naming.

### Q2 — Different address registers: both single-slot transfers complete before either commits

`write_move` computes, in encoded source-then-destination order (matching the instruction's own already
-established source-before-destination lowering order, and matching real MC68000 EA-calculation order):

1. If `source_ea.mode` is `address_predec`/`address_postinc`: snapshot
   `memory->address_registers[source_ea.reg]` into one local, `m68k_move_src_ea`. For predecrement,
   subtract the operand width from that local immediately (before the source access), exactly like
   `movem_transfer`'s `ea_local -= width;` at line 4028. For postincrement, leave the local unchanged
   for the access and record that width must be added to it before its eventual writeback.
2. Route the source read through `m68k_emit_routed_read` using `m68k_move_src_ea` (or the plain
   register-relative address expression for a non-mutating source mode, unchanged from today) as the
   address, exactly as `movem_transfer`'s `emit_slot` already does per read slot.
3. If `destination_ea.mode` is `address_predec`/`address_postinc`: snapshot
   `memory->address_registers[destination_ea.reg]` into a second local, `m68k_move_dst_ea` (a distinct
   local unless `destination_ea.reg == source_ea.reg`, which Q3 below rejects outright before reaching
   this point), applying the same predecrement-before-access / postincrement-after-access local
   arithmetic as step 1.
4. Route the destination write through `m68k_emit_routed_write` using `m68k_move_dst_ea` as the
   address.
5. Only after **both** step 2's and step 4's routed statements have been emitted (i.e., placed strictly
   later in the generated C than both of them) does `write_move` emit the live-register-array writeback
   statement(s): `memory->address_registers[source_ea.reg] = m68k_move_src_ea;` if the source mutated,
   and `memory->address_registers[destination_ea.reg] = m68k_move_dst_ea;` if the destination mutated.
   Both writebacks — when both operands mutate different registers — are placed together, after both
   accesses, never one immediately after its own operand's access.

This is a direct generalization of `movem_transfer`'s per-register discipline to MOVE's two-operand
shape; the sole novel requirement beyond `movem_transfer`'s own precedent (which never had a second,
independently addressed register in the same instruction) is deferring **both** writebacks past
**both** accesses, not past only each operand's own access. It is why `m68k_emit_materialized_ea_read`'s
existing convention of appending `result.postlude` immediately after materializing the source value
(used by `add`/`subtract`/`compare`/etc.) is **not** reused for `write_move`'s predecrement/
postincrement operands: that convention commits a source's postincrement before a second, independent
memory operand is even attempted, which is exactly the violation this decision must prevent for MOVE's
two-EA shape.

### Q3 — Same address register: rejected whenever the mutating operand is the source

A MOVE whose source is `address_predec`/`address_postinc` and whose destination's own effective-address
computation reads that same address register — whether the destination is itself
`address_predec`/`address_postinc` (for example `MOVE.W -(A0),-(A0)` or `MOVE.L (A2)+,(A2)+`), or the
destination is a non-mutating register-relative mode that still reads the register's *current* value to
form its own address, `address_indirect` or `address_disp16` (for example `MOVE.W -(A0),(A0)` or
`MOVE.L (A2)+,4(A2)`) — is **rejected**, not given an explicit combined semantic, for every one of these
shapes.

**Why this must cover the non-mutating-destination shape too (the defect this revision corrects).** An
earlier draft of this record rejected only the both-`address_predec`/`address_postinc` shape and
separately claimed, in the "Acceptance shape" section below, that the asymmetric
source-mutating/destination-non-mutating-same-register shape was safely representable "with only one
local/one writeback instead of two." That claim was wrong. Trace `MOVE.W -(A0),(A0)` under Q2's
mechanism exactly as specified: step 1 snapshots A0 into `m68k_move_src_ea` and decrements the local
(not the live register) before the source access; the source access at step 2 correctly reads from
`m68k_move_src_ea` (A0-2). The destination, being non-mutating `address_indirect`, is never snapshotted
into a local under Q2 as written — it is still routed through the untouched, general
`m68k_emit_ea_write`/`m68k_emit_runtime_ea_address` address expression, which reads
`memory->address_registers[0]` **directly from the live array**. Per Q2 step 5 and Q5, the source's
writeback to the live array is deferred until *after* both accesses complete — so at the moment the
destination's address expression evaluates, the live A0 is still its stale, pre-decrement value. The
destination write therefore targets the original A0, not A0-2: wrong. Real MC68000 hardware fully
computes and applies the source's own auto-update *before* beginning destination EA calculation (this
record already states that hardware fact above for the both-mutating shape); the deferred-writeback
mechanism as specified in Q2 does not reproduce that behavior for a non-mutating destination that reads
the same register, because a non-mutating operand is never routed through a local in the first place.

**Why the reverse asymmetric shape is unaffected and remains representable.** A MOVE whose destination
is `address_predec`/`address_postinc` and whose source is a non-mutating register-relative mode reading
the same register (for example `MOVE.W (A0),-(A0)`) does *not* have this problem. Source-before-destination
is this project's own established lowering order (Q2), and it also matches real hardware order here: the
non-mutating source is evaluated and its access completes — using the live, not-yet-modified register
value, which is still correct at that point in program order — strictly before the destination's own
predecrement/postincrement local is even computed. There is no stale-value read: the source's read of the
live array happens before the destination local's arithmetic ever touches that register conceptually
(the live array itself is never mutated until the destination's own deferred writeback, which happens
after the source has already been fully consumed). This shape is untouched by this revision and remains
representable exactly as the record already states.

**Why the "different address registers" shape (Q2) is unaffected.** Q2's mechanism only defers a
register's *own* writeback past both accesses; it never causes one register's local mutation to be
substituted for a *different* register's value anywhere. When `source_ea.reg != destination_ea.reg`, the
destination's address expression — whether routed through a local (if the destination is itself
`address_predec`/`address_postinc`) or through the live array (if it is a non-mutating mode) — never
reads `address_registers[source_ea.reg]` at all, so it can never observe a stale value belonging to the
*source's* register. The staleness hazard this revision fixes is specifically a same-register aliasing
hazard: it cannot arise when the two operands name different registers. Q2's reasoning and mechanism are
therefore confirmed unaffected by this correction and require no change.

Real MC68000 hardware resolves the same-register aliasing shapes above (both the both-mutating shape and
the source-mutating/destination-referencing-but-non-mutating shape) by fully applying the source's own
auto-update before beginning destination EA calculation — but this repository has no differential
(Musashi-oracle or otherwise) evidence for these specific narrow shapes, and the project charter requires "Never
claim instruction support without differential or fixture-based evidence." This task's own Notes section
explicitly states no hardware fact is at stake in resolving open questions 1, 2, 4, and 5 (they are
purely about this project's own generated-C commit-ordering discipline); asserting the correct
composed-aliasing value here would require exactly the kind of unverified hardware claim that boundary
excludes. Rejecting every same-register shape where the source is the mutating operand is therefore the
honest, bounded choice, not merely the conservative one — and, per the analysis above, it is also the
*only* choice available to this revision: extending Q2's deferred-writeback mechanism to make the
non-mutating destination read the source's local instead of the live array would require touching how a
non-mutating EA's address expression is generated for this one aliasing case (a mechanism change, not
merely a wider rejection), which this revision deliberately does not adopt — see the note at the end of
this section.

The exact rejecting check the follow-on implementation must add, inside `write_move`'s own
block-emission case, evaluated before any C is emitted for the instruction (mirroring how
`movem_transfer` itself falls through to `ok = false` for an EA mode its own switch does not handle,
lines 4046-4048):

```cpp
const bool source_mutating = operation.source_ea.mode == M68kEaMode::address_predec ||
                              operation.source_ea.mode == M68kEaMode::address_postinc;
const bool destination_reads_same_register_address =
    operation.destination_ea.mode == M68kEaMode::address_predec ||
    operation.destination_ea.mode == M68kEaMode::address_postinc ||
    operation.destination_ea.mode == M68kEaMode::address_indirect ||
    operation.destination_ea.mode == M68kEaMode::address_disp16;
if (source_mutating && destination_reads_same_register_address &&
    operation.source_ea.reg == operation.destination_ea.reg) {
  // The source's own auto-update mutates this address register before the
  // destination's address is computed; the destination's own EA computation
  // (mutating or not) reads that same register to form its address, and this
  // repository has no differential evidence for the hardware-correct
  // composed value. Not represented; emit no C for this instruction (same
  // convention as any other declined EA combination already in this switch).
}
```

This check strictly widens the narrower both-mutating-only check an earlier draft of this record used:
every case the narrower check rejected (`destination_mutating && same reg`) is still rejected here,
because `address_predec`/`address_postinc` are included in `destination_reads_same_register_address`;
the check additionally now rejects the non-mutating-destination-same-register shapes
(`address_indirect`/`address_disp16` destination with `destination_ea.reg == source_ea.reg`) that the
earlier draft incorrectly left representable. It deliberately does **not** fire when only the destination
is mutating and the source is a non-mutating same-register reference (`MOVE.W (A0),-(A0)`), per the
"reverse asymmetric shape" analysis above — that shape remains representable under Q2's mechanism
unchanged.

When this check fires, `write_move` emits no C for the instruction (the same "no output for this
operation" signal `read.ok`/`write.ok` already produce today for any other declined combination), which
existing frontier classification already turns into the established `GENESIS_STOP_UNSUPPORTED_CPU_FORM`
runtime frontier (§7 of the runtime bridge contract) when the four frontier conditions hold — never a
build-time reject, since the instruction itself is otherwise fully decoded/lifted with trustworthy
provenance; only this one narrow operand combination is declined at the emission stage, exactly like
every other currently-unrepresented EA combination in this same switch.

This is a deliberately bounded, honestly labeled non-goal of the follow-on implementation task, not a
silent mis-lowering: any same-register MOVE where the source is the mutating operand and the destination
reads that same register (mutating or not) remains outside this project's supported envelope until a
later task supplies differential evidence for its correct combined semantic. A design alternative was
considered and rejected for this revision: rather than widening the rejection, the non-mutating
destination's address expression could instead be made to substitute the source's already-computed local
(`m68k_move_src_ea`) for the live register read whenever this same-register aliasing is detected, which
would make the shape representable with the hardware-plausible value instead of rejecting it. This
record deliberately does not adopt that alternative, because it would assert a specific composed-aliasing
hardware semantic without the differential evidence the project charter requires — the same reasoning that already
governs the both-mutating shape above applies identically here. A future task may revisit this choice if
and when such evidence becomes available.

### Q4 — `retain_fact`/`require_fact`: remains deliberately fact-free for MOVE's mutating operands

`write_move`'s predecrement/postincrement operands record no `M68kStaticMemoryFact` and require none,
exactly like `movem_transfer`'s own already-established precedent (see the code comment at
`src/m68k_pipeline.cpp` lines 3955-3964: "C4 never pre-populates `memory->movem_transfer_regions`/
`movem_transfer_values`... and it retains no per-slot `M68kStaticMemoryFact` for MOVEM at all
(`retain_fact`'s own switch only ever records move/tst/clr) -- deliberately the smaller of this task's
two documented design options, since every representable MOVEM EA family's per-slot address is already
a pure function of the instruction's own statically-decoded EA... computable directly at C4 lowering
time with no new fact retention shape required"). The same reasoning applies verbatim to MOVE's
register-relative operands: `m68k_is_statically_foldable_control_ea` (lines 947-950) already excludes
`address_predec`/`address_postinc` from ever being "statically foldable" (it accepts only
`absolute_word`/`absolute_long`/`pc_disp16`), so `M68kStaticMemoryFact` — which represents an
already-resolved **absolute** address's region and, for a ROM read, its folded constant — has no
meaningful content to record for a register-relative runtime address in the first place. `retain_fact`
(lines 1728-1773) is therefore left completely unchanged by this decision: it already declines to
record a fact for these modes (its own first line, "if `(!m68k_is_statically_foldable_control_ea(ea))
return;`", is unconditional and mode-based, not instruction-kind-based).

`require_fact` (lines 2839-2872) does need one narrow change, confined to its `move` call sites (lines
2873-2875) only: its existing unconditional line 2870 (`if (ea.mode == address_predec || ea.mode ==
address_postinc) return false;`) must no longer apply when the calling instruction kind is `move` —
predecrement/postincrement operands must pass this consistency gate unconditionally for `move`,
deferring all representability/aliasing validation entirely to `write_move`'s own block-emission case
(Q2's deferred-commit mechanism, plus Q3's aliasing rejection), exactly mirroring how every other
non-foldable EA already passes this same gate unconditionally today (the comment at lines 2851-2861
already states the general design principle this extends: "each instruction kind's own block-emission
switch below independently decides whether that EA is representable"). The rejection at line 2870 must
remain unchanged and still apply, exactly as today, for `tst`'s call site (line 2877) and `clr`'s call
site (line 2879): fixing TST/CLR's own predecrement/postincrement gap is explicitly out of this task's
scope (Non-goals: "any C4 IR kind other than `write_move`'s ... gap"), and both remain deliberately
rejected until a dedicated follow-on task addresses them on their own evidence.

Concretely, this means `require_fact`'s single shared lambda must become aware of which instruction
kind is calling it (an added parameter, or three separate call-site-local checks replacing the single
shared lambda for this one line) — a small, explicit, auditable change confined to
`src/m68k_pipeline_frontend.cpp` lines 2839-2880, not a new fact-retention mechanism.

### Q5 — Ordering guarantee and how the mechanism achieves it

**Guarantee:** for any `GENESIS_STOP` reached anywhere during `write_move`'s own lowering of one MOVE
instruction (a source-read routing failure or a destination-write routing failure), every address
register that instruction touches — zero, one, or two of them, since Q3 rejects the only way it could
ever be exactly one register touched twice — must remain exactly at its value from before this
instruction began executing. No partial decrement/increment may ever become observable.

**How the mechanism achieves it**, by construction:

1. Every arithmetic mutation (predecrement subtraction, postincrement addition) this decision's
   mechanism performs is applied only to a local C variable (`m68k_move_src_ea`/`m68k_move_dst_ea`),
   never directly to `memory->address_registers[reg]`, at any point before the final writeback
   statement(s).
2. `m68k_emit_routed_read`/`m68k_emit_routed_write` (lines 3045-3079) are, by their own existing
   construction (unchanged by this decision), each a single self-contained generated-C statement/block
   that either succeeds and falls through to the next statement, or fails and unconditionally executes
   `return transfer;` inside its own `if` — an immediate function return guaranteeing no
   later-in-program-text statement for this invocation of `emit_m68k_operation_c`'s generated block ever
   executes.
3. The (up to two) live-register-array writeback statements are placed, in the generated C, strictly
   after both the source access's routed statement and the destination access's routed statement — with
   no other code path reaching them.
4. Therefore: if the source access fails, control returns before the destination access is even
   attempted, so no writeback executes — every touched register (in this case, only the source's, since
   the destination access was never reached) remains unmodified. If the source access succeeds but the
   destination access then fails, control returns from inside the destination access's own generated
   statement, which is still textually before both writeback statements — so, again, no writeback
   executes, including for the source register that already read successfully. Only when **both**
   accesses succeed does control reach past both access statements to the writeback statement(s), at
   which point committing both is safe because neither commit can any longer be followed by a failure
   from this instruction.
5. This is exactly the reasoning that already makes `movem_transfer`'s own single-register case correct
   today (its per-slot loop's one writeback statement at lines 4031/4045 is reached only if every one of
   the loop's `emit_slot` calls — each itself a routed access with this identical early-return-on-failure
   shape — has already completed without returning). This decision's only addition is deferring **both**
   of MOVE's (up to two) writebacks past **both** of MOVE's accesses, since MOVE, unlike MOVEM, can touch
   two independently addressed registers in one instruction.

## Acceptance shape for a follow-on implementation task

- **Representable:** any MOVE whose source and/or destination is `address_predec`/`address_postinc`,
  except the aliasing shapes below (Q3). This includes the "different address registers" shape (Q2,
  confirmed unaffected by Q3's aliasing check — see Q2-unaffected analysis in Q3), and, of the
  one-operand-mutating shapes, precisely those where either the two operands name different registers,
  or the *destination* is the mutating operand and the source is a non-mutating mode referencing the same
  register (for example `MOVE.W (A0),-(A0)`) — the same mechanism with only one local/one writeback
  instead of two. The one-operand-mutating shape where the *source* is the mutating operand and the
  destination is a non-mutating `address_indirect`/`address_disp16` mode referencing the *same* register
  (for example `MOVE.W -(A0),(A0)`) is **not** representable; see rejected below.
- **Explicitly rejected:** any MOVE where the source is `address_predec`/`address_postinc` and the
  destination's own address computation reads that same address register — whether the destination is
  itself `address_predec`/`address_postinc`, or a non-mutating `address_indirect`/`address_disp16` that
  still reads the register's current (would-be-stale) value (Q3) — by the exact widened check shown
  above, added inside `write_move`'s own block-emission case, before any C is emitted. The reverse
  asymmetric shape (destination mutating, source non-mutating referencing the same register) is not
  rejected and remains representable.
- **Ordering guarantee:** preserved for every representable combination by deferring every touched
  register's live-array writeback past every routed access this one instruction performs (Q5).
- **Fact retention:** no change to `retain_fact`; `require_fact`'s predecrement/postincrement rejection
  narrows to apply only to `tst`'s and `clr`'s call sites, not `move`'s (Q4).
- **Untouched:** `m68k_emit_ea_read`, `m68k_emit_ea_write`, `m68k_emit_runtime_ea_address`,
  `m68k_emit_routed_read`, `m68k_emit_routed_write`, `m68k_emit_runtime_ea_guard`, and every other
  `M68kIrKind` case listed in the Q1 table (all remain exactly as they are today; none of their current
  latent single-operand predecrement-before-access hazard rows are fixed by this decision).

## Explicit non-claims

This contract makes no new addressing-mode, instruction-kind, or hardware-timing claim. It does not
resolve the same-register-aliasing MOVE semantic (Q3); it does not fix `tst`/`clr`'s own unconditional
predecrement/postincrement rejection; and it does not fix the structurally identical, currently
unguarded single-operand hazard already present today in `compare`/`subtract`/`add`/the three logical
families/`write_movea`/`push_effective_address`/`link_frame`/`unlink_frame`/the four `bit_*` kinds
(listed in the Q1 table for completeness only). Any of those remain open for a separately scoped,
separately evidenced future task.

## SEG-021-T005 amendment: Q3 same-register aliasing is lowered, not declined

Q3's outright decline of a mutating source whose address register also feeds the destination EA is superseded for
the routed lowering. The MC68000 completes the source EA calculation (including its auto-update) before it forms the
destination EA (pinned Musashi agrees; the direct lowering was already validated against it by the T003/T004 rows).
The routed lowering now keeps every architectural write deferred exactly as Q2 requires, and derives the destination
address from the source's updated LOCAL (`m68k_move_src_ea`) instead of the live register:

- destination `(An)` / `d16(An)` reading the same register: `m68k_move_dst_ea = m68k_move_src_ea (+ d16)`, routed write, no
  destination commit;
- destination `(An)+` / `-(An)` on the same register: `m68k_move_dst_ea` starts from `m68k_move_src_ea`, is stepped as
  usual, and its commit is emitted last so `(An)+,(An)+` nets two steps.

Every live-register commit is still emitted textually after both routed accesses, so a runtime stop leaves the
pre-instruction register file. Evidence: `tests/m68k_routed_lowering_test.py` runs the routed and direct lowerings on
identical vectors (every size, both source update kinds, all four destination kinds, A7 byte stepping) and requires
identical D/A/SR/PC/memory; the direct lowering is Musashi-validated by `tests/fixtures/m68k-conformance-vectors.json`.
The same amendment lowers NOT with an auto-updating destination through the NEG/ADD single-local commit (one routed
read and one routed write at the same address, one commit after both).
