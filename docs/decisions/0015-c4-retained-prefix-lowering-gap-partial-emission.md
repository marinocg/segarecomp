# ADR 0015: C4 Retained-Prefix Lowering-Gap Partial Emission

- Status: **Accepted** — the selected mechanism, **local in-block truncation**, is unchanged across two
  rounds of pre-merge review corrections; neither round reopens the mechanism or opens a second
  architecture task. First round: (1) Decision §2/Q1 corrected from an inaccurate "every block remains
  reachable" claim toward a reachability-narrowing property (truncation may remove downstream
  reachability; it may never fabricate it); (2) Decision §6's finiteness proof re-grounded in the
  still-active `m68k_discovery_max_instructions` ceiling rather than the retired
  `m68k_discovery_max_blocks` gate; (3) a new Decision §7 binds normalized, address-free C4-gap
  classification/diagnostic-pairing plumbing ADR-0011 §5 requires. Second round: (4) Decision §7 is
  corrected again — the classification dimension must be **stop-owned** (a field of `GenesisRuntimeStop`/
  `GenesisControlTransfer`), never modeled after `GenesisReportMetadata.cpu_dimensions`'s existing
  whole-program-static single value, which cannot losslessly distinguish multiple differently-shaped
  exits this mechanism intentionally permits; the sanitized/full report gains a distinct
  `c4_lowering_dimensions` field, leaving `cpu_dimensions` unchanged; (5) Q1/Decision §2's terminology is
  tightened to distinguish the emitted **program-control** prefix (blocks and inter-block transfers,
  never a superset of the accepted retained graph) from the **newly introduced terminal C4 stop sinks**
  this mechanism itself adds (which have no outgoing program-control edge and so are not part of any
  subgraph-of-the-retained-graph claim). Third round (documentation/ownership only, no design change):
  (6) Decision §7 gains an exact protocol binding requiring SEG-007-T142's own pull request to reconcile
  `docs/architecture/genesis-generalized-startup-runtime-bridge-contract.md` §13.3 with the implemented
  wire ABI in the same PR, so the canonical bridge contract and the implemented report schema are never
  left in apparent contradiction. SEG-007-T142 remains the sole implementation successor.
- Date: 2026-09-01
- Decided by: SEG-007-T141, from independent inspection of the current C4 emission seam at this
  repository head (`preflight_m68k_general_startup_c4`, `emit_m68k_general_startup_runtime_c`, the C4
  block dispatcher, and the retained-prefix/partial-program machinery `discover_m68k_general_startup`
  / `runtime_frontier_eligible` / `build_analysis` / `partial_or_rejection` already use for
  `discovery_prefix_boundary`), consuming SEG-007-T140's recorded 7-row C4 preflight-gap result and
  ADR-0014's established boundary-mechanism vocabulary and pattern one pipeline stage earlier.
- Amends: none of ADR 0010, ADR 0011, ADR 0013, or ADR 0014. This decision is scoped entirely to the
  C4 codegen emission stage, which none of those ADRs previously constrained beyond ADR 0002's general
  fail-closed-execution rule. It does not reopen `GenesisFrontierClass`, `UnresolvedFrontier`,
  `m68k_discovery_max_frontier_exits`, or any discovery-resource ceiling.
- Related: ADR 0002, ADR 0011 (§5's `runtime_confirmed` / `static_only` vocabulary), ADR 0013 (the
  emitted-stop-function / dispatcher-arm template this decision reuses), ADR 0014 (the sibling
  decision-then-implementation precedent one pipeline stage earlier).
- Implemented by: SEG-007-T142, not this ADR. This ADR changes no `src/`, `include/`, `runtime/`,
  `tools/`, or `tests/` file, and no capability-map row beyond a correction of a genuinely stale
  statement.

## Context

### A. What SEG-007-T140 established, and the exact stop this decision addresses

SEG-007-T140 implemented ADR-0014's entry-rooted discovery-prefix-boundary construction and
re-executed the authorized pinned Sonic route. Static discovery now deterministically promotes a
runnable, entry-connected `FrontendPartialProgram` (18 probe-verified `discovery_prefix_boundary`
exits, 112 retained blocks; `retained_addresses == sibling_addresses`; P1-P6 hold). C11 emission of
that accepted prefix then deterministically stops at `preflight_m68k_general_startup_c4`, which
reports exactly 7 distinct preflight gap rows (2 `requires_architecture_decision` — postincrement-
addressed `add`- and `write_clr`-family source/destination EAs, predecessor `"deferred address
commit"` — and 5 `missing_dispatcher` — a compare-family instruction pair, an immediate-source compare
form, an immediate-operand bit-set form, and an immediate-count register shift/rotate form).

Per ADR-0011 §5's unconditional guarantee, none of these 7 rows is `runtime_confirmed`: no generated-
native binary was ever produced or run on this route, so all 7 are `static_only` `STATIC_REACHABLE_GAP`
and none may justify or select a SEG-007 implementation task on its own. Picking any subset of the 7
for direct implementation on static-reachability grounds alone would itself violate ADR-0011 §5 and
risk recreating whole-static-CFG capability chasing one pipeline stage later than ADR-0014 foreclosed
it.

### B. Exact current control flow, and why it is whole-emission-closed

Independent inspection of `src/codegen/c11/frontend.cpp` at this repository head confirms the
mechanism:

- `preflight_m68k_general_startup_c4` validates the *whole* `FrontendPartialProgram.accepted_prefix`:
  it rebuilds address-keyed `decoded`/`operations` maps, then walks **every** retained IR operation and
  appends one `M68kC4PreflightRow` per unlowerable shape. The loop discards each instruction's address
  before recording a row, and the accumulated rows are sorted and deduplicated by a tuple with **no
  address field** (`family, ir_kind, operand_role, width, ea_class, auto_update, gap, predecessor`).
  So "7 distinct rows" is 7 distinct unlowerable *shapes*, not 7 instruction sites; the reported
  preflight surface is deliberately address-free, consistent with this project's report-privacy
  discipline, and this decision does not change that reported contract.
- `emit_m68k_general_startup_runtime_c` calls the preflight first and immediately returns a rejection
  comment whenever any row exists — **before** touching `partial.frontiers`, `partial.accepted_prefix.
  static_blocks`, or building any per-block emission state. This single whole-program boolean gate,
  not a per-instruction or per-block decision, is what makes today's emission whole-emission-closed:
  even though the 112 retained blocks may contain only a handful of gap-triggering instructions, none
  of their C text is ever produced.
- By contrast, block emission itself is already structured per-instruction: one `genesis_block_<addr>`
  function per retained block, one `switch` over `M68kIrKind` per instruction inside that function,
  each instruction lowered in its own scope. That switch currently has no fallback arm only *because*
  the preceding whole-program preflight already guarantees every kind reaching it is representable.
  This existing per-instruction emission structure is the seam this decision reuses.

## Decision

Adopt **local in-block body truncation**, keeping the address-free `M68kC4Preflight`/`M68kC4PreflightRow`
reported-diagnostic contract unchanged for its existing reporting callers, and adding a second,
address-carrying internal emission pass that turns each retained-but-unlowerable instruction into a
build-time-defined, unconditional fail-closed emitted stop reached exactly at the point that
instruction would otherwise execute — never a `GenesisFrontierClass` value, `UnresolvedFrontier`, or a
new discovery-time diagnostic.

### 1. Why direct reuse of `GenesisFrontierClass`/`UnresolvedFrontier`/`FrontendPartialProgram` is rejected

`GenesisFrontierClass` and `UnresolvedFrontier` are discovery-time, open-control-flow-edge concepts:
`classify_frontier` produces them from a `FrontendRejected` discovery itself recorded for an
*unadmitted edge target*, and ADR 0013 §4 (preserved by ADR 0014) fixes the single producer of
`discovery_prefix_boundary` as the instruction-admission-ceiling handler in `M68kStaticGraphWalker` —
"no other code path produces the class." A C4 lowering gap is the opposite shape: an already fully
decoded, lifted, and provenance-validated instruction *inside* an already-admitted, already
entry-connected retained block (this is exactly why `preflight_m68k_general_startup_c4` can safely
walk `prefix.ir`/`prefix.decoded` at all — P3 already guarantees those instructions are complete).
Synthesizing a discovery-time `FrontendRejected`/probe-provenance record for a C4 gap to force it into
`GenesisFrontierClass` would fabricate a diagnostic discovery never produced, violating the ADR 0013
§4 single-producer rule and the no-fabricated-provenance discipline ADR 0013 §2 establishes. Reuse is
therefore of the **representation/dispatch shape** (an unconditional, build-time-emitted, provenance-
keyed stop function selected by address comparison, exactly the `build_genesis_frontier_stop_function`
/ `genesis_dispatch` frontier-arm template), not of the discovery-stage type itself.

### 2. Graph-cut mechanics: local in-block truncation, not a block-graph cascade

Given an unlowerable instruction at address `A` inside retained block `B`:

- Only `A` itself, and everything textually after it in `B`'s instruction sequence, is excluded from
  real lowering. Nothing upstream of `A` in `B`, and no other block, is touched.
- Block emission already iterates `block.instructions` in source order inside one C function. The cut
  is: emit `B`'s instructions normally up to (not including) `A`; at the point `A`'s lowering `switch`
  arm would appear, emit an unconditional `return genesis_c4_lowering_stop_<A>(runtime);` instead, and
  stop emitting the remainder of `B`'s body.
- No block *entity* is dropped and no *static, discovery-stage* edge is pruned from
  `prefix.static_blocks` / `prefix.static_edges` — this is a **local in-block body truncation** of the
  *emitted C text*, not a block-graph cascade erase like `build_analysis`'s cascade erase for orphaned
  successors at the discovery/boundary stage. The retained static graph itself, and every proof
  `runtime_frontier_eligible` already performed over it (P1/P2), is completely untouched by this
  mechanism.
- **What is and is not preserved is a runtime-*execution* property, not a static-graph property, and
  the two must not be conflated (Correction, see Q1 below).** `B`'s own emitted control-flow transfer(s)
  — its terminal branch/call/fallthrough `return genesis_dispatch(...)` or direct call — are textually
  *after* every instruction in `block.instructions`, so when `A` occurs strictly before that terminal
  transfer, truncation removes it from the emitted function along with the rest of `B`'s tail: that
  transfer, and whatever block(s) it would otherwise have reached *only* through it, are **not**
  actually reachable in generated execution once `B`'s run hits `A`'s stop. The static P1/P2 entry-
  connectedness guarantee — every retained block *is emitted* and is reachable from the entry in the
  discovery-stage graph — is unaffected and remains exactly as strong as ADR-0014 established it; what
  changes is that an emitted block function can become **dormant** (never executed on the path that hits
  a preceding truncation) without ceasing to exist or ceasing to satisfy P1/P2 as a static artifact.
- This requires M68k static blocks retained by this pipeline to remain single-entry (the block's first
  instruction address equals its block-id entry address, already enforced at discovery), with no
  retained edge targeting a mid-block instruction. The implementing task must assert this precondition
  directly (an adversarial fixture asserting a mid-block target is rejected, not silently mis-handled)
  before relying on it. This precondition is exactly what makes the cut *local*: because no other
  retained block's own edge can target the middle of `B`, no other emitted function's control flow is
  disturbed by truncating `B`'s tail — the only thing that becomes unreachable is what `B`'s own
  (now-removed) tail would have transferred to.

**The post-truncation emitted program-control graph as a prefix of the accepted retained graph, plus
newly introduced terminal stop sinks (Correction — tightened terminology).** An earlier version of this
ADR called the *complete* emitted graph (blocks and stops together) a "subgraph" of the accepted
retained graph. That overstates the claim: a `genesis_c4_lowering_stop_<addr>` function is a genuinely
**new** node with no counterpart in the accepted retained graph (which contains only block and
discovery-stage-boundary nodes), so the complete emitted graph is not literally a subgraph of it once
stop sinks are added. The precise, correct claim distinguishes two disjoint node kinds:

- **Program-control nodes** — the emitted `genesis_block_<addr>` functions and the *inter-block*
  transfers between them (branch/call/fallthrough dispatch, `genesis_dispatch` arms, return targets).
  For these, and **only** these: the set of program-control nodes and transfers actually present *before
  each cut* is a **prefix/subset** of the accepted retained graph's own nodes and edges — truncation may
  omit (never add) a program-control node or transfer relative to what the accepted retained graph
  contains. Truncation may remove a downstream program-control transfer (a block's own terminal branch/
  call/fallthrough emission is dropped whenever a gap precedes it in that block, per Decision §2 above),
  making the block(s) it would have reached unreachable/dormant on that path; it never fabricates a new
  program-control transfer, edge, or program-control node absent from the accepted retained graph — no
  instruction, block, or control-flow target the accepted retained graph did not already contain is ever
  introduced.
- **Terminal C4 stop sinks** — the `genesis_c4_lowering_stop_<addr>` functions this mechanism itself
  introduces at each cut point. These are the **only** newly introduced nodes; they have **no outgoing
  program-control edge** (each is an unconditional `genesis_static_stop`-shaped return with no further
  dispatch, per Decision §3), so they never extend reachability to anything beyond themselves and never
  participate in the program-control prefix/subset relation above. A stop sink is reached, when it is
  reached at all, only by execution falling through its own block's already-program-control-reachable
  prefix up to the cut point.

Together: every instruction actually executed strictly before a truncation stop keeps its full,
unmodified normal semantics — truncation never alters, elides, or partially lowers any instruction that
precedes the gap in its own block; every path that would reach an unlowerable instruction stops — via
the unconditional `genesis_c4_lowering_stop_<A>` call — strictly before that instruction performs any
operand read/write or other observable state mutation; a block whose function becomes **dormant** on a
given prefix (present in the emitted translation unit, never actually invoked because every path to it
was truncated upstream) is permitted and expected, and is not itself reported, provided no fabricated
program-control transfer makes it *falsely* appear reachable; all direct, indirect, call, and return
dispatch in the generated program continues to select **only** among already-emitted, build-time-decided
function identities proven by the existing `runtime_frontier_eligible` / `genesis_dispatch` machinery —
this mechanism adds no new dispatch target and no new dispatch mechanism beyond the terminal stop sinks
it itself introduces.

This is a strictly weaker and more honest claim than "every block reachable before truncation remains
reachable after truncation" (the original, incorrect Q1 wording this ADR is being corrected to replace,
Consequences below), and it is sufficient: no acceptance criterion in this ADR or in ADR-0002's general
fail-closed-execution rule ever required that truncation preserve *downstream* reachability — only that
it never fabricate program-control reachability, never emit a partial/best-guess semantic body, and stop
strictly before any effect of the excluded instruction.

### 3. No-incomplete-semantic-state: build-time unconditional fail-closed stop

Reuse the `build_genesis_frontier_stop_function` *shape*: a generated `genesis_c4_lowering_stop_<8-hex-
source-address>` function whose body is the same unconditional, no-partial-mutation construction the
existing frontier-stop machinery's `genesis_static_stop(class_, category, &source, has_access, address,
width, direction)` call already uses, plus (Decision §7) one additional build-time-constant argument
carrying that exact instruction's own `GenesisC4LoweringDimensions` literal — keyed by the gap
instruction's own already-verified `InstructionProvenance` (already present in
`prefix.decoded`/`operations`, already covered end-to-end by the existing address-keyed
`genesis_attach_route_provenance` chain, which needs no change since it is already built from every
retained instruction, gap or not). Because P3 already guarantees the gap instruction was fully decoded,
lifted, and provenance-validated, the stop occurs strictly *before* any operand read/write for that
instruction — no partial mutation, no partial access, exactly like every existing frontier stop touches
no target byte.

A new `GenesisStopClass`-shaped value distinguishes this family from the existing `c4_stop_class`
cases: **`GENESIS_STOP_C4_LOWERING_GAP`**, reusing the existing `genesis_static_stop` runtime-struct
plumbing unchanged. **This stop must be given its own truthful diagnostic pairing (Correction, see
Decision §7 below) — it must not be paired with an existing, semantically unrelated
`GenesisDiagnosticCategory` value** such as `GENESIS_DIAG_VALID_BUT_UNSUPPORTED_INSTRUCTION` /
`GENESIS_DIAG_UNSUPPORTED_INSTRUCTION_FORM` (which name a *decode/lift-time* CPU-form gap, not a
build-time-known, already-lifted C4-lowering exclusion), `GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY`
(which names a defensive invariant-violation trap, not an expected, catalogued static gap), or
`GENESIS_DIAG_DISCOVERY_BUDGET_EXHAUSTED` (ADR-0013's discovery-stage boundary category, whose
single-producer rule this ADR's Decision §1 explicitly does not reuse). A new
`GENESIS_DIAG_C4_LOWERING_GAP` category is bound instead, per Decision §7.

Runtime dispatch and re-entry select **only among already-emitted function identities** exactly as
today: dispatch to a C4-lowering-gap stop is a direct, unconditional `return` at the gap's in-block
position — it needs **no `genesis_dispatch` arm at all**, which is a further simplification of the
ADR 0013/0014 pattern, not merely a reuse of it (the discovery-stage boundary needed a dispatcher arm
because re-entry could resume execution *at* the boundary address from an arbitrary prior stop; a C4
gap stop is reached only by falling through the enclosing block's own already-reachable body, never by
direct re-entry from `runtime->pc` comparison). No runtime opcode fetch, decode, or target-address
computation is added anywhere.

### 4. Deterministic stop selection order

The existing address-free preflight dedup sort is the wrong ordering primitive for per-instruction stop
selection, since two gap instances with identical shape at different addresses must remain distinct
stop functions. The deterministic rule reuses containers the emitter already builds: blocks are already
iterated by ascending entry address (`std::map<Address, const M68kStaticBlock*>`), and each block's
`instructions` are already in ascending source order. The rule: **iterate blocks in ascending entry-
address order; within each block, scan instructions in file order; the first unlowerable instruction in
a block becomes that block's own truncation stop, keyed by its own source address.** A block with no
unlowerable instruction is emitted in full, unchanged. This composes with the existing `frontier_sort_
key` determinism discipline (a deterministic total order over emitted exits) without introducing a new
sort routine — it falls directly out of containers the emitter already constructs, so generated output
remains byte-identical across runs on identical input/options.

### 5. `add` / `write_clr` `requires_architecture_decision` rows: a narrower, already-catalogued
sub-question

Both rows share the predecessor label `"deferred address commit"` for their `address_predec`/
`address_postinc` auto-update case. `docs/architecture/c4-move-predecrement-postincrement-commit-
contract.md`'s own capability table already names the `add` family's and `write_clr`'s RMW-destination
predecrement/postincrement hazard as an explicitly catalogued, already-known "same unguarded gap" left
open by that decision's MOVE-specific deferred-commit technique. This is retained here as lookahead
only: it is a narrower, already-classified sub-question of that existing contract, not a new
architectural fork, and this decision does not resolve it. The implementing successor may consult that
existing contract directly if and when a real generated-native run selects one of these two rows as
`runtime_confirmed`; this ADR does not decide it and does not authorize implementing it as a
precondition of landing the mechanism.

### 6. Bounding, and why no representation-safety ceiling is needed

Unlike `m68k_discovery_max_frontier_exits`, this mechanism needs no new representation-safety bound —
but the bounding proof must be grounded in the ceiling that is actually still active, not in
`m68k_discovery_max_blocks` (**Correction**: an earlier version of this ADR cited
`m68k_discovery_max_blocks = 192U` as an active per-prefix block-count bound; independent inspection of
`src/cpu/m68k/static_discovery.cpp`'s `note_block_entry` at this repository head confirms ADR-0010 §2
and ADR-0013 already **retired** `m68k_discovery_max_blocks` as an independent discovery admission gate
— "a block-entry count can no longer refuse a block entry or raise a `discovery_budget_exhausted`
failure" — so it is not a sound bound to cite here, and this ADR does not reinstate it as one; the
constant's only remaining role in this codebase is the unrelated `m68k_discovery_max_frontier_exits`
representation-safety derivation in `include/segarecomp/machine/genesis/frontend.hpp`, which this ADR
does not touch).

The correct, currently-active ceiling is `m68k_discovery_max_instructions` (`256U`,
`M68kStaticGraphWalker`'s single resource ceiling, checked in `decode_instruction`, unchanged and
unwidened by this decision). The mechanical bounding proof:

1. Every retained block is registered through `note_block_entry` only for an address that is (or
   becomes) an admitted instruction's address; a block with zero admitted instructions cannot exist in
   `prefix.static_blocks`.
2. Each retained block's instructions are a subset of the addresses admitted during **one seed's own
   walk**, and one walk is capped at `m68k_discovery_max_instructions` (`256U`) distinct instruction
   addresses (ADR-0011 §§1-3's per-walk termination argument, unaffected by this decision). For the
   current accepted prefix this decision is grounded in (SEG-007-T140's single-seed, Phase A, round-1
   promotion — 112 retained blocks, 18 exits), this is also the bound on the whole accepted prefix, since
   there is exactly one walk. **Precision note (independent review correction).** An earlier version of
   this proof equated "one round" with "one walk" and stated the whole accepted prefix is unconditionally
   bounded by `256U`. That equivalence does not generally hold once ADR-0013's already-accepted Phase B
   activates (up to `m68k_discovery_max_seed_entries` = `4U` independently walked seeds per expansion
   round, each its own `256U`-capped walk): a Phase-B-expanded accepted prefix's instruction population is
   bounded by the aggregate `m68k_discovery_max_seed_entries * m68k_discovery_max_instructions`
   (`4U * 256U = 1024U`, ADR-0013's own already-accepted aggregate), not by `256U` alone. This ADR does
   not decide, widen, or reinterpret that aggregate — it is ADR-0013's existing accepted bound — but the
   proof below is stated per-seed-walk so it composes correctly with either case.
3. This mechanism emits **at most one** C4 truncation stop per retained block (Decision §4's
   first-unlowerable-instruction-in-file-order rule), and only for a block that actually contains at
   least one admitted, retained instruction.
4. Therefore the number of distinct `genesis_c4_lowering_stop_<addr>` identities this mechanism can ever
   emit **per admitted seed-walk** is bounded by the number of retained blocks that walk produced, which
   is itself bounded by that walk's own `256U`-capped instruction admissions. Summed across an accepted
   prefix's admitted seed-walks, the total C4 stop population is bounded by
   `(number of seed-walks in the accepted prefix) * 256U` — exactly `256U` for the current single-seed
   prefix this decision is grounded in, and bounded by the already-fixed `1024U` aggregate in the
   Phase-B case — with no new representation-safety constant required either way.

No `GenesisFrontierClass` enumerator, no `UnresolvedFrontier`, and no `m68k_discovery_max_frontier_exits`
bound apply to this C4-owned stop family; `m68k_discovery_max_instructions` remains unchanged at `256U`
and is not widened, searched, retuned, or replaced by this decision.

### 7. Truthful runtime classification of the selected C4-gap capability (Correction — required by
ADR-0011 §5; the dimension must be **stop-owned**, not whole-program-static)

A generic `GENESIS_STOP_C4_LOWERING_GAP` stop class plus ordinary instruction-provenance (source
address, image offset, mapping claims, bus accesses) is **not sufficient** to satisfy ADR-0011 §5's
requirement that the actual generated-native stop select the *exact* normalized capability while
durable evidence remains non-reconstructable: provenance alone tells the implementing successor only
"execution reached *a* C4 gap", never *which one* of the (currently seven, potentially more later)
distinct unlowerable shapes it was — and this project's report-privacy discipline (ADR-0011 §5, the
existing sanitized-vs-full report split) forbids recovering that answer from a raw commercial source
address or opcode. This ADR therefore explicitly binds a normalized, non-address reporting
representation.

**Why the existing `GenesisReportMetadata.cpu_dimensions` precedent cannot be reused for this family
as-is (Correction — an earlier version of this ADR modeled the new dimension after it directly).**
Independent inspection of the current emitter/runtime at this repository head shows
`GenesisReportMetadata.cpu_dimensions` is **exactly one whole-program-static value**, compiled into the
generated bridge once and consulted by `genesis_write_sanitized_report`/`genesis_write_full_report`
only when the actually-reached `stop_class` happens to be `GENESIS_STOP_UNSUPPORTED_CPU_FORM` — it does
not, and structurally cannot, distinguish *which* of several possible exits of that class was actually
reached. `emit_m68k_general_startup_bridge_c` (`src/codegen/c11/frontend.cpp`) makes this ownership
explicit in its own code and comment: when the accepted partial program contains more than one
`unsupported_cpu_form` frontier with *differing* classified dimensions, it explicitly **rejects the
whole translation** ("that ambiguity is exactly as unrepresentable as a missing classification"),
precisely because one static field cannot losslessly represent whichever of several exits the runtime
actually selects. This ADR's own mechanism (Decision §4) intentionally **permits multiple emitted C4-
lowering-gap stops with differing normalized shapes in the same generated program** — that is the whole
point of the mechanism, letting execution select among several statically retained gaps. Modeling the
C4 dimension after `GenesisReportMetadata.cpu_dimensions` would therefore either force the same
whole-program rejection whenever two differently-shaped gaps are retained (defeating Decision §4's own
purpose) or silently misattribute one exit's classification to a different exit's actual runtime
selection. Neither is acceptable. The dimension literal must instead be **owned by the runtime stop
itself**, not by translation-time program metadata.

**Corrected ownership.**

- A new, dedicated enum **`GenesisC4LoweringDimensions`** is added (`runtime/genesis/runtime.h`,
  alongside `GenesisCpuDimensions`, not folded into it): `GENESIS_C4_LOWERING_DIMENSIONS_NONE = 0` plus
  one finite, build-time-defined literal per **distinct represented C4-gap shape**, derived from the
  minimal necessary subset of the existing `M68kC4PreflightRow` classification fields already computed
  by `preflight_m68k_general_startup_c4`: `family` (the existing `std::string_view` IR-family label),
  `ir_kind` (`M68kIrKind`), `operand_role` (`M68kC4OperandRole`), `width` (`M68kMemoryAccessWidth`),
  `ea_class` (`M68kEaMode`), `auto_update` (`M68kC4AutoUpdateClass`), and `gap` (`M68kC4GapClass`). The
  implementer selects the minimal subset of these seven dimensions actually sufficient to distinguish
  the currently known gap shapes (mirroring how `GenesisCpuDimensions`'s three-field wire triple was
  already judged sufficient for its own family); a dimension that never varies across the represented
  shapes need not be carried. No raw source address, opcode word, or `predecessor` free-text label is
  ever placed in this enum, its wire name, or the sanitized report.
- The dimension literal is carried by the **stop itself**: added as a field of `GenesisRuntimeStop`
  (or an equivalently stop-owned normalized-metadata member reachable from `GenesisControlTransfer`,
  the implementer's choice so long as it is per-stop-instance, not per-program-static). It is **not**
  added to, and does not reuse, `GenesisReportMetadata`, whose existing single-value,
  translation-time-static semantics for `GENESIS_STOP_UNSUPPORTED_CPU_FORM` are left completely
  unchanged by this decision.
- Each `genesis_c4_lowering_stop_<addr>`'s generated body constructs its returned
  `GenesisControlTransfer`/`GenesisRuntimeStop` directly with: `GENESIS_STOP_C4_LOWERING_GAP`;
  `GENESIS_DIAG_C4_LOWERING_GAP`; **that exact instruction site's own build-time-computed**
  `GenesisC4LoweringDimensions` literal (a compile-time constant baked into that one generated
  function's own `return`, reused from `genesis_static_stop`'s existing shape or a small variant of it
  that additionally threads this one extra literal argument); and its existing provenance — exactly the
  unconditional, no-partial-mutation construction Decision §3 already establishes, with one additional
  build-time-constant argument. Because the literal is baked into the specific stop function that is
  actually invoked, **no lookup by commercial source address is required to recover it**, and no runtime
  opcode fetch, decode, or target-byte inspection is introduced anywhere. Two gap stops with the *same*
  normalized shape (e.g. two distinct addresses both hitting the same `missing_dispatcher` compare-
  family gap) legitimately construct the same dimension literal — that is expected and matches the
  existing preflight contract's own address-free shape deduplication; the *stop function identity*
  (`genesis_c4_lowering_stop_<addr>`) remains per-address and per-block for dispatch/provenance purposes
  even when the dimension literal it reports is shared.
- A new **`GENESIS_DIAG_C4_LOWERING_GAP`** `GenesisDiagnosticCategory` value is added and paired
  exclusively with `GENESIS_STOP_C4_LOWERING_GAP` in the runtime stop-class/diagnostic-category pair
  validator (the `c4_stop_class`-adjacent mirror in `src/codegen/c11/frontend.cpp` and
  `src/machine/genesis/frontend.cpp`) and in `tools/genesis_startup_bridge.py`'s `STOP_DIAGNOSTIC_PAIRS`
  table (a new `"c4_lowering_gap": {"c4_lowering_gap"}` entry, following the existing one-to-one
  singleton-set precedent `"instruction_budget_exhausted"` / `"internal_dispatch_inconsistency"` already
  use). No existing diagnostic category is repurposed or overloaded for this family.

**Binding stop-owned invariants.**

- A `GENESIS_STOP_C4_LOWERING_GAP` result **must** carry one valid, non-`GENESIS_C4_LOWERING_DIMENSIONS_
  NONE` `GenesisC4LoweringDimensions` value.
- Every non-`GENESIS_STOP_C4_LOWERING_GAP` result — every other stop class and every completion —
  **must** carry `GENESIS_C4_LOWERING_DIMENSIONS_NONE` (the field's zero default; it must never be set
  to a non-`NONE` value by any code path other than a `genesis_c4_lowering_stop_<addr>` function).
- `GENESIS_STOP_C4_LOWERING_GAP` remains paired **exclusively** with `GENESIS_DIAG_C4_LOWERING_GAP`
  everywhere this codebase validates a stop-class/diagnostic-category pair; no existing, semantically
  unrelated category (`GENESIS_DIAG_VALID_BUT_UNSUPPORTED_INSTRUCTION`,
  `GENESIS_DIAG_UNSUPPORTED_INSTRUCTION_FORM`, `GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY`,
  `GENESIS_DIAG_DISCOVERY_BUDGET_EXHAUSTED`) is ever paired with it.
- Runtime/report validation rejects: an invalid or out-of-range `GenesisC4LoweringDimensions` value; a
  missing (`NONE`) dimension on a `GENESIS_STOP_C4_LOWERING_GAP` result; and a non-`NONE` dimension
  attached to any other stop class. This mirrors `genesis_valid_report_metadata`'s existing
  `GENESIS_STOP_UNSUPPORTED_CPU_FORM`-only gating, generalized to a second, independently-validated
  stop-owned field rather than a second whole-program-static one.
- The dimension value is **build-time-selected by the generated stop function itself** — it is never
  derived from runtime `pc`, from re-inspecting `GenesisProvenance`, or from any other runtime-computed
  value; it is a compile-time literal argument to that one function's own unconditional construction,
  exactly like every other constant `genesis_static_stop`-shaped call already bakes in.

**Sanitized/full report field, distinct from `cpu_dimensions`.** A distinct sanitized-report field,
**`c4_lowering_dimensions`**, is added; the existing `cpu_dimensions` field keeps its exact current
whole-program-static `GENESIS_STOP_UNSUPPORTED_CPU_FORM`-only meaning, completely unchanged:

- a `GENESIS_STOP_C4_LOWERING_GAP` stop reports `"cpu_dimensions": null` and a valid, non-null
  `"c4_lowering_dimensions"` object (the analogous normalized wire shape to `cpu_dimensions`'s
  `family`/`size`/`addressing_mode_class` triple, sized to whichever subset of the seven dimension
  fields the implementer selected as sufficient);
- an `unsupported_cpu_form` stop keeps today's exact `"cpu_dimensions"` behavior and reports
  `"c4_lowering_dimensions": null`;
- every other stop and a `completed` result report both fields `null`, exactly as `cpu_dimensions` alone
  does today.

**Exact protocol binding (Correction — required so SEG-007-T142 does not leave two apparently
contradictory authoritative contracts).** `docs/architecture/genesis-generalized-startup-runtime-bridge-
contract.md` §13.3 is this project's own authoritative canonical wire-schema document for exactly the
report shapes this decision extends — it already defines the exact fixed key order, the `"completed"`
all-`null` rule, and `cpu_dimensions`'s own representable-per-actually-reached-stop rule (SEG-007-T073)
in the identical normative style this decision's addition must follow. This ADR therefore binds the
exact schema change precisely, so SEG-007-T142 implements one internally consistent protocol rather than
inventing wire shape ad hoc and leaving §13.3 stale:

- `schema_version` remains `1` — this is a compatible in-repository evolution of the canonical report,
  following this document's own existing precedent (its Revision history records multiple prior
  schema-compatible additions — the ninth revision's `frontiers` generalization, the tenth revision's
  `opcode_line`/`region_class` fields — none of which bumped `schema_version`).
- The sanitized report's canonical key order becomes exactly: `schema_version`, `report_kind`,
  `rom_sha256`, `result`, `stop_class`, `diagnostic_category`, `cpu_dimensions`, `c4_lowering_dimensions`
  — `c4_lowering_dimensions` is appended immediately after the existing `cpu_dimensions` key, the last
  key before any `--compare-runs` extension.
- With `--compare-runs`, `"reports_match"` remains the final appended key, after
  `c4_lowering_dimensions` — its own existing position (appended last) is unchanged by this decision.
- `"completed"`: `stop_class = null`, `diagnostic_category = null`, `cpu_dimensions = null`,
  `c4_lowering_dimensions = null` (all four `null` together, extending §13.3's existing three-field rule
  to the fourth field this decision adds).
- `unsupported_cpu_form`: existing valid `cpu_dimensions` object (unchanged shape/rule); `c4_lowering_
  dimensions = null`.
- `c4_lowering_gap`: `cpu_dimensions = null`; a valid, non-null, stop-owned `c4_lowering_dimensions`
  object.
- Every other stop class: both `cpu_dimensions` and `c4_lowering_dimensions` are `null`.
- The full-report contract gains the same `c4_lowering_dimensions` field, in one fixed canonical
  position (immediately after the existing `stop_class`/`diagnostic_category` pair, alongside
  `provenance`, mirroring where the sanitized report's own dimension fields sit relative to
  `stop_class`/`diagnostic_category`), governed by the identical null/non-null rule as the sanitized
  report above (`null` for `"completed"` and every non-`c4_lowering_gap` stop; a valid, non-null value
  only for a `c4_lowering_gap` stop) — consistent with whatever the runtime writer
  (`genesis_write_full_report`) and `tools/genesis_startup_bridge.py`'s `valid_full` actually implement,
  never merely with the sanitized report's own rule stated once and left implicit for the full report.

The exact canonical-wire validators and key ordering are updated consistently and deterministically:
the generated runtime's own `genesis_write_sanitized_report` and `genesis_write_full_report` (the full
report continues to carry ordinary instruction provenance exactly as every other stop already does,
since it is not commercial-source-restricted the way the sanitized report is); `tools/genesis_startup_
bridge.py`'s `valid_sanitized` / `valid_full` (a new `c4_lowering_dimensions` required key in both, in
the exact position stated above, validated against a finite, explicitly enumerated whitelist of
currently-representable literals — never accepted as an arbitrary/unvalidated string, mirroring exactly
how `cpu_dimensions` is validated today); and adversarial fixtures asserting a stop/dimension-family
mismatch (a `c4_lowering_gap` stop with `cpu_dimensions` non-null, or with `c4_lowering_dimensions`
null; a non-`c4_lowering_gap` stop with `c4_lowering_dimensions` non-null) is rejected by the driver, in
both the sanitized and full report paths. The sanitized `c4_lowering_dimensions` value remains a finite,
explicitly validated, address-free/non-reconstructable representation: no commercial source address,
opcode, image offset, or free-text `predecessor` label is ever required to identify the selected
capability.

**Binding requirement for the implementing successor, including the architecture-contract reconciliation
this correction adds.** SEG-007-T142 must implement this stop-owned dimension/diagnostic-category
plumbing as part of landing the mechanism (not as a follow-on), because without it a real generated-
native run that reaches a C4-lowering-gap stop cannot be truthfully classified to the one exact selected
capability ADR-0011 §5 requires — it would otherwise report only "some C4 gap was reached", which is not
sufficient evidence to select or bound a single successor capability. The normalized
`c4_lowering_dimensions` value the actually-reached stop constructs is the durable, non-reconstructable
evidence SEG-007-T142 uses to classify exactly one capability `runtime_confirmed` / `RUNTIME_SELECTED`
and scope its own created successor; every other statically emitted C4-gap stop (reached by no run)
remains `STATIC_REACHABLE_GAP` and contributes no dimension evidence to anything.
**SEG-007-T142's own pull request must also update `docs/architecture/genesis-generalized-startup-
runtime-bridge-contract.md` §13.3 (adding a new dated revision-history entry per that document's own
established precedent, and updating both the sanitized- and full-report JSON examples and their prose
rules to the exact schema bound above) in the same pull request that implements the ABI/report-writer
change — this task must not land the wire-protocol change while leaving §13.3 stale, and must not defer
that reconciliation to any later task.** `GenesisReportMetadata.cpu_dimensions` and its existing
`GENESIS_STOP_UNSUPPORTED_CPU_FORM`-only semantics are not modified by this task beyond whatever is
strictly required to add the new, independent `c4_lowering_dimensions` field alongside it.

## Invariant restatement (C4 analog of ADR 0014 Decision §4's P1-P6)

The implementing task asserts the following directly in project-authored tests on every emitted
partial C11 translation unit produced under this mechanism:

- **Q1 (program-control prefix, no fabricated reachability; terminal stop sinks are the only new
  nodes).** The emitted **program-control** nodes/transfers (blocks and their inter-block dispatch)
  present *before each cut* are a prefix/subset of the accepted retained graph's own nodes and edges:
  truncation may remove a downstream program-control transfer that existed past a gap (making the
  block(s) it would have reached unreachable, and possibly dormant, on that path), but never fabricates
  a program-control transfer, edge, or node absent from the accepted retained graph. The **only** newly
  introduced nodes are the terminal fail-closed `genesis_c4_lowering_stop_<addr>` sinks this mechanism
  itself inserts at cut points; each has **no outgoing program-control edge**, so it never extends
  reachability beyond itself and is not part of the prefix/subset relation above. Every instruction
  actually executed before a truncation stop retains its full normal semantics. **This replaces an
  earlier, incorrect wording of Q1** — first "every block reachable before this mechanism is applied
  remains reachable after it is applied" (false whenever the first unlowerable instruction in a block
  precedes that block's own original terminal transfer, since truncation necessarily removes that
  transfer along with the block's tail), then a since-tightened "the complete generated execution graph
  is a safe subgraph of the accepted retained graph" (imprecise, since the newly introduced terminal
  stop sinks have no counterpart in the accepted retained graph and so are not literally part of any
  subgraph of it). The static discovery-stage entry-connectedness proof (ADR-0014 P1/P2,
  `runtime_frontier_eligible`) is unaffected by either correction: it concerns the *retained graph
  itself*, which this mechanism never mutates, not the emitted program-control graph, which this
  mechanism may legitimately narrow, plus the disjoint set of terminal stop sinks it introduces.
- **Q2 (no incomplete semantic state).** An unlowerable instruction is never partially lowered,
  best-guess translated, or given a semantic body; it is replaced by exactly one unconditional
  fail-closed stop, reached strictly before any operand access for that instruction.
- **Q3 (single point of truncation per block).** At most one truncation stop is emitted per retained
  block, at the first unlowerable instruction in file order; every instruction preceding it in that
  block is emitted normally with its full semantic body.
- **Q4 (no runtime decode).** Runtime dispatch and re-entry select only among already-emitted,
  build-time-decided function identities; no opcode fetch, decode, or target-address computation is
  added anywhere; a C4-lowering-gap stop needs no `genesis_dispatch` arm.
- **Q5 (deterministic selection order).** For identical input and options, the same block/instruction
  is truncated and the same stop function identities are emitted across runs, byte-identical.
- **Q6 (runtime-confirmed-only classification).** Only a real generated-native execution run that
  actually reaches and stops at an emitted `GENESIS_STOP_C4_LOWERING_GAP` stop may classify that exact
  normalized capability `runtime_confirmed` / `RUNTIME_SELECTED`; every other statically emitted
  C4-lowering-gap stop remains `static_only` `STATIC_REACHABLE_GAP` until a later run actually reaches
  it, per ADR-0011 §5.
- **Q7 (truthful, stop-owned normalized classification, Correction, Decision §7).** Every emitted
  `GENESIS_STOP_C4_LOWERING_GAP` stop carries its own build-time-computed, address-free, **stop-owned**
  `GenesisC4LoweringDimensions` literal (the minimal necessary subset of `family`/`ir_kind`/
  `operand_role`/`width`/`ea_class`/`auto_update`/`gap` sufficient to distinguish the represented
  shapes), constructed directly by that exact stop function — **never** a whole-program-static field
  like `GenesisReportMetadata.cpu_dimensions`, which cannot losslessly distinguish among multiple
  differently-shaped exits (Decision §7's correction). Every non-C4 stop and every completion carries
  `GENESIS_C4_LOWERING_DIMENSIONS_NONE`. `GENESIS_STOP_C4_LOWERING_GAP` is paired exclusively with the
  dedicated `GENESIS_DIAG_C4_LOWERING_GAP` diagnostic category — never with an existing, semantically
  unrelated stop-class/diagnostic pair. The sanitized report's distinct `c4_lowering_dimensions` field
  (leaving `cpu_dimensions` unchanged) is validated against a finite, explicitly enumerated whitelist of
  currently-representable literals, is required to be non-null exactly when the stop is
  `c4_lowering_gap` and null otherwise, and is never accepted as an arbitrary or unvalidated value; no
  raw commercial source address or opcode is ever required to identify which capability an
  actually-reached stop selected.

## Consequences and Next-Task Shape

SEG-007-T142 is the implementing successor. This pull request creates it directly (`state = "ready"`,
`depends_on = ["SEG-007-T141"]`, same parent `SEG-007`), following the same continuation-successor
mechanism a completing implementation task uses, per this task's own Scope item 6 (no pre-existing
sibling record exists for this decision, unlike SEG-007-T139 -> T140).

**Binding scope for SEG-007-T142.**

1. Implement Decision §§2-4 in `src/codegen/c11/frontend.cpp`: the address-carrying internal emission
   pass, per-block first-unlowerable-instruction truncation, the `genesis_c4_lowering_stop_<addr>`
   generated-stop-function shape, and the `GENESIS_STOP_C4_LOWERING_GAP` `GenesisStopClass` value
   (`include/segarecomp/machine/genesis/frontend.hpp` and/or the generated-runtime ABI header, whichever
   already hosts `GenesisStopClass`). Keep `M68kC4Preflight`/`M68kC4PreflightRow`'s existing address-free
   reported contract unchanged for its existing callers.
2. Implement Decision §7's normalized C4-gap classification and diagnostic-pairing plumbing as part of
   landing the mechanism, not as a follow-on: the new, dedicated `GenesisC4LoweringDimensions` enum
   (never folded into or attached via `GenesisReportMetadata.cpu_dimensions`, which keeps its exact
   current whole-program-static `GENESIS_STOP_UNSUPPORTED_CPU_FORM`-only semantics unchanged); the
   **stop-owned** field carrying that literal on `GenesisRuntimeStop` (or an equivalently stop-owned
   member reachable from `GenesisControlTransfer`), constructed directly by each
   `genesis_c4_lowering_stop_<addr>` as a build-time-constant argument; the new
   `GENESIS_DIAG_C4_LOWERING_GAP` diagnostic category paired exclusively with
   `GENESIS_STOP_C4_LOWERING_GAP` in every stop-class/diagnostic-category pair validator this codebase
   maintains (the C11 emitter's own pairing check, the generated runtime ABI, and
   `tools/genesis_startup_bridge.py`'s `STOP_DIAGNOSTIC_PAIRS`); the distinct sanitized/full report field
   `c4_lowering_dimensions` (leaving `cpu_dimensions` untouched) and its `valid_full`/`valid_sanitized`
   deterministic validation, including rejection of a missing/out-of-range dimension on a
   `c4_lowering_gap` stop and rejection of a non-null dimension on any other stop.
3. Update `docs/architecture/genesis-generalized-startup-runtime-bridge-contract.md` §13.3 in this same
   pull request — the same PR that implements the item 2 ABI/report-writer change, never a later or
   separate task — to reconcile it with the implemented runtime/report ABI exactly per this ADR's
   "Exact protocol binding" above: `schema_version` stays `1`; a new dated revision-history entry
   documents the change per that document's own established precedent; the sanitized-report canonical
   key order becomes `schema_version`, `report_kind`, `rom_sha256`, `result`, `stop_class`,
   `diagnostic_category`, `cpu_dimensions`, `c4_lowering_dimensions`, with `"reports_match"` still
   appended last under `--compare-runs`; both the sanitized- and full-report JSON examples and their
   null/non-null prose rules are updated for `"completed"`, `unsupported_cpu_form`, `c4_lowering_gap`,
   and every other stop class exactly as bound above; the full-report contract documents
   `c4_lowering_dimensions` in one fixed canonical position consistently with whatever
   `genesis_write_full_report`/`valid_full` actually implement. This task must not land the wire-protocol
   change while leaving §13.3 stale, and must not defer this reconciliation to a later task.
4. Add project-authored, legally redistributable synthetic C4 fixtures asserting Q1-Q7 directly: a
   positive case with one retained block containing one unlowerable instruction preceded and followed
   by lowerable instructions (asserts Q2/Q3: prefix instructions keep full bodies, the gap becomes
   exactly one unconditional stop, nothing after it in that block is emitted); a case where the first
   unlowerable instruction occurs strictly *before* its block's original terminal branch/call/fallthrough
   transfer, proving that transfer (and any block reachable only through it) is genuinely **absent** from
   the emitted program-control graph rather than asserting the old target remains reachable, and that the
   resulting terminal stop sink itself has no outgoing program-control edge (asserts the corrected Q1
   directly — this must be a positive assertion of *absence*, not merely omission of a reachability
   check); a multi-block case with gaps in two different blocks at different addresses
   (asserts Q4/Q5: deterministic first-in-file-order selection per block, byte-identical two-run repeat,
   no `genesis_dispatch` arm added); a case with two candidate gap shapes in the same block (asserts only
   the first in file order becomes the stop; the rest of that block, including any later
   would-also-be-unlowerable instruction, is never reached by emission because it was already excluded by
   truncation); a strict-C11 compilation of representative generated output; an adversarial
   mid-block-target-if-any-exists case per Decision §2's precondition; a sanitized-report round-trip
   fixture asserting Q7 — a synthetic `c4_lowering_gap` stop carrying a valid, non-null
   `c4_lowering_dimensions` literal and a null `cpu_dimensions` is accepted; a synthetic stop with an
   out-of-whitelist `c4_lowering_dimensions` value, or a `c4_lowering_gap` stop with a null
   `c4_lowering_dimensions` or non-null `cpu_dimensions`, or a non-`c4_lowering_gap` stop with a non-null
   `c4_lowering_dimensions`, is rejected by the driver; and no raw source address ever appears in the
   sanitized dimension field.
5. Re-execute the authorized pinned Sonic route (`tools/genesis_startup_bridge.py --mode commercial
   --diagnose-frontier`, at least two byte-identical runs) once the mechanism is implemented, and record
   the truthful real generated-native result, including its reported normalized dimension literal, per
   this ADR's post-execution routing (Decision §8 of SEG-007-T141's own Scope, restated below).
6. Explicitly forbid implementing any of the 7 currently known static C4 gap rows as a precondition of
   landing the mechanism itself.
7. Route the actual post-execution result:
   - if generated-native execution reaches and stops at one of the emitted `GENESIS_STOP_C4_LOWERING_GAP`
     stops, that exact normalized capability (and only that one) becomes `runtime_confirmed` /
     `RUNTIME_SELECTED`, and SEG-007-T142's own pull request creates the smallest bounded CPU/C4
     continuation successor for that one selected capability or its immediately coherent family — never
     for the other statically-visible-but-unreached gaps, which remain `STATIC_REACHABLE_GAP`;
   - if it instead reaches `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY`, ADR-0013 Decision §7 Phase B
     activates exactly as already accepted;
   - if it reaches any other actual CPU/device/runtime frontier, that real stop is the truthful next
     handoff, recorded and routed to its own existing capability owner;
   - if deterministic VDP/device state produced by that execution becomes sufficient for useful frame
     extraction, follow the `MILESTONE_OBSERVABLE` path (ADR-0011 §5) instead of chasing an unrelated
     CPU/device gap.
8. Reconcile `docs/development/sonic-title-critical-capabilities.md` in SEG-007-T142's own pull request
   from the truthful executed result. Independent adversarial validation happens inside SEG-007-T142's
   pull request.

**Binding non-goals for SEG-007-T142.** Implementing any of the 7 currently known static C4 gap rows
as a precondition of landing the mechanism; making `preflight_m68k_general_startup_c4` report zero
rows for the current prefix; creating more than one further successor per reached stop; a new
`M68kIrKind`, a new persistent-state concept, or a second parallel emission/dispatch path; any
device/VDP/Z80/PSG/controller-I/O/rendering/timing work; an interpreter, JIT, or runtime opcode
fetch/decode; any Sonic-specific address, seed, code map, or heuristic; widening any static-discovery
resource ceiling or `m68k_discovery_max_frontier_exits`; amending this ADR, ADR-0014, or reopening
ADR-0011 Decisions §§1-3.

**Binding terminal outcomes for SEG-007-T142.** Each of the following is a complete, bounded,
acceptable result and none is a task failure: a real generated-native run that reaches and stops at one
emitted `GENESIS_STOP_C4_LOWERING_GAP` stop, with the smallest bounded CPU/C4 successor created for it
(the expected outcome on the current authorized route, since the accepted mechanism makes at least one
of the 112 retained blocks' first-reached gap instruction actually executable); a run that instead
reaches `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY`, activating ADR-0013 Phase B; a run that reaches any
other real CPU/device/runtime frontier, truthfully handed off to its existing owner; a
`MILESTONE_OBSERVABLE` result. None authorizes a capability-status change beyond the truthful executed
result, weakening a fail-closed rule, or fabricating evidence.

## Relationship to existing ADRs

- **ADR 0002.** Unchanged. An emitted `GENESIS_STOP_C4_LOWERING_GAP` stop is a non-success exit that
  makes no completeness claim past it, exactly like every other fail-closed stop this project emits.
- **ADR 0009.** No interaction; this decision does not touch indirect-candidate resolution.
- **ADR 0010, ADR 0011, ADR 0013.** Not amended. This decision is scoped entirely to the C4 codegen
  emission stage, downstream of every discovery-stage guarantee those ADRs establish; it reuses the
  ADR 0011 §5 `runtime_confirmed`/`static_only` vocabulary and the ADR 0013 emitted-stop-function /
  dispatcher-arm template without reopening any of their decisions.
- **ADR 0014.** Not amended; this decision is its explicit C4-emission-stage analog, applying the same
  "generalize the existing partial-program model rather than force progress through static reachability
  alone" pattern one pipeline stage later, with the graph-cut mechanics adapted from block-level
  (ADR 0014) to per-instruction, in-block (this ADR) granularity.
