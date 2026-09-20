# ADR 0011: Non-Recursive Static-Discovery Call/Return Ownership and Checkpoint-Directed Demand-Driven Expansion

- Status: Accepted
- Date: 2026-08-30
- Amends: ADR 0010 ("Static-Discovery Fixpoint Completeness and a Bounded-Resource Ceiling").
  **Supersedes** ADR 0010's Decision §§1-2 claim that a host-recursive walk plus a finite mapped image
  plus `(addr, stack_signature(frame_stack))` memoization is by itself a sufficient
  completeness/termination argument ("the `(addr, stack_signature)` state space is finite" because
  `stack_signature` length "is bounded by `m68k_discovery_max_call_frame_depth`" — ADR 0010 lines
  57-58), and ADR 0010's §2 choice to leave `m68k_discovery_max_call_frame_depth` unchanged with no
  exhaustion disposition ("`m68k_discovery_max_call_frame_depth` (`2U`) ... [is] unchanged. ... not the
  completeness gate" — ADR 0010 lines 156-159). SEG-007-T126's executed evidence falsified both: an
  ephemeral, fully reverted implementation spike removed the block-count cap exactly as ADR 0010 §2
  specifies, and the authorized generated-native route then deterministically reached
  `discovery_budget_exhausted` at the **unchanged** `m68k_discovery_max_call_frame_depth` structural
  bound instead — a foldable, ordinary, fully-provenanced direct subroutine-call family, not a
  block/instruction-budget case. ADR 0010's partial-program seam (§3) could not represent that stop,
  because it is a call-context-admission refusal reached while walking, not a discovered-block-entry
  boundary its `build_analysis` pruning understands.
  **Leaves intact**: ADR 0010 §1's completeness *definition* (fixpoint of the walk, not a
  block/instruction count is the completeness criterion); ADR 0010 §2's retirement of
  `m68k_discovery_max_blocks` as an independent gate and its bounded-resource-ceiling justification
  method for `m68k_discovery_max_instructions`; and ADR 0010 §§3-4's `discovery_prefix_boundary`
  frontier class and its IR/C11/runtime representation and re-entry-refusal argument, none of which
  SEG-007-T126 implemented (the spike was fully reverted before reaching that code) and none of which
  this ADR changes — this ADR reuses and generalizes them (Decision §4) rather than replacing them.
  ADR 0010's own general principles also stand: arbitrary block/instruction/depth counts are not
  semantic completeness; engineering resource ceilings are not Genesis hardware facts; deterministic
  fail-closed behavior is required; no runtime decoding; provenance must survive through every stage.
- Related: ADR 0002, ADR 0003, ADR 0006, ADR 0007, ADR 0009.
- Implemented by: a successor task, not this ADR. This ADR changes no `src/`, `include/`, `runtime/`,
  `tools/`, or `tests/` file, and no `docs/architecture/*` or capability-map file.

## Context

### Established facts about the current walker (direct inspection, current head)

`M68kStaticGraphWalker::walk` (`src/cpu/m68k/static_discovery.cpp`) is a single host-recursive C++
member function taking `(Address addr, std::vector<M68kStaticCall> frame_stack)` — one C++ stack frame
per target-program call and per best-effort branch-side re-walk (lines 633-851). Its traversal-state
memoization key is `state_key = {addr, stack_signature(frame_stack)}` (lines 634-635), where
`stack_signature` is the ordered list of call-site source addresses of every frame currently on
`frame_stack` (lines 618-623) — so `(address, full call-stack signature)` is the traversal-state
identity today, not address alone. `JSR`/`BSR` push one `M68kStaticCall{caller, continuation, callee}`
onto `frame_stack` and recurse into `callee` (lines 752-781, 789-810); `RTS` pops the top frame and
recurses into its `continuation` (lines 714-733). Call-frame admission is capped: `JSR`/`BSR`/indirect
JSR handlers all refuse to recurse once `frame_stack.size() == limits_.max_call_frame_depth` (lines
752-758, 789-795, 953-958), producing `DirectFlowDiagnostic::discovery_budget_exhausted` with
`unresolved_reason = "m68k_discovery_max_call_frame_depth"` and failing the walk closed.
`max_call_frame_depth` is `2U` (`include/segarecomp/machine/genesis/frontend.hpp` line 87) — a
recursion-admission guard, unrelated to any Genesis-hardware property, exactly as ADR 0010 already
established for the block/instruction constants it retired or re-justified.

A sufficiently large `m68k_discovery_max_instructions` value was separately found (SEG-007-T126) to
overflow the host C++ stack in the Debug build of this same recursive walker — i.e., host-recursion
depth is already at the edge of host-process safety even before this ADR removes any depth cap.

### Direct inspection: C4 already lowers `RTS` as a proven finite-candidate membership check, not a call-frame simulation, for the runtime-routed profile

`src/codegen/c11/m68k.cpp`'s `M68kIrKind::return_from_subroutine` lowering has two distinct bodies,
selected by `memory->runtime_routing` (lines 574-617):

- **`runtime_routing == true`** (the Genesis general-startup profile — `src/codegen/c11/frontend.cpp`
  sets this flag pervasively for the routed lowering path, e.g. lines 769, 2011-2343, 2536) emits code
  that: reads the real popped return value through `genesis_route_access` at the current SSP (lines
  578-590, matching ordinary 68000 RTS semantics — an actual bus-routed 32-bit read, not a
  walker-simulated value); compares it against `memory->runtime_return_targets`, a finite literal
  candidate list (lines 591-595); on non-membership, returns a deterministic fail-closed
  `genesis_static_stop` naming the RTS instruction and the observed address (lines 596-597); on
  membership, advances `A7` by 4 and sets the program counter to the observed value (line 598), which
  the unchanged `genesis_dispatch` then consumes on the next drive iteration exactly like any other
  `GENESIS_CONTINUE_AT_PC` transfer. **This is already ADR 0009's bounded-computed-indirect-target
  pattern** (prove a finite candidate set statically; evaluate the runtime-computed value; membership
  gate; dispatch through the existing block-identity dispatcher; fail closed on non-membership) applied
  to a return address instead of a PC-indexed brief EA — it does not use, and does not need, a
  walker-maintained call-frame stack at code-generation time.
- **`runtime_routing == false`** (a narrower, non-bus-routed lowering profile, not used by
  `genesis-general-startup`) instead emits a fixed-size emitted-code shadow call stack
  (`frame_ids_array`, `frame_continuations_array`, `frame_depth`, lines 601-616) that duplicates the
  walker's per-call identity into the generated program itself. This ADR does not change this
  unrelated profile; it is out of scope because it is not part of the authorized route this ADR
  answers.

`memory->runtime_return_targets` (`include/segarecomp/codegen/c11/m68k.hpp` line 38) is populated
(`src/codegen/c11/frontend.cpp` lines 2347, 2544-2545) from every discovered
`M68kStaticEdgeKind::return_to_continuation` edge whose **source instruction address** matches this
specific RTS — i.e., it is already keyed by the RTS's own address and unions one continuation per
distinct call site reaching it (`src/cpu/m68k/static_discovery.cpp`'s `return_key = {addr,
call.caller.source.address.value}`, lines 724-727, already dedupes per call site, not per full call
chain). `M68kStaticCall{caller, continuation, callee}` (`include/segarecomp/cpu/m68k/static_program.hpp`
line 38) is itself already an address-keyed, single-call-site identity, independent of call-chain
depth. **The per-call-site union that ultimately backs `runtime_return_targets` was never the source of
the depth limitation.** The limitation is entirely the walker's own recursion-admission refusal at
`frame_stack.size() == max_call_frame_depth` and the `stack_signature`-keyed memoization that refusal
exists to bound.

### `known_but_unemitted_target`'s actual current role (direct inspection)

`GenesisFrontierClass::known_but_unemitted_target` (SEG-007-T064) exists today
(`include/segarecomp/machine/genesis/frontend.hpp`, `src/machine/genesis/frontend.cpp`,
`src/codegen/c11/frontend.cpp`, `runtime/genesis/runtime.h`/`.c`). Direct inspection of its only
production call site (`src/machine/genesis/frontend.cpp` lines 1495-1515) shows it is a **secondary,
best-effort fallback classification**: it is applied to a *secondary* failure (a discarded conditional-
branch sibling exit alongside an already-promoted *primary* frontier) only when that secondary
failure's own precise `classify_frontier` result fails its own `runtime_frontier_eligible` check; it is
then retried once more under `known_but_unemitted_target`'s own (weaker) eligibility test, and dropped
entirely if that also fails. It has no independent triggering role, is never the primary reason a
program promotes to `FrontendPartialProgram`, and is bounded together with every other frontier class
by the shared `m68k_discovery_max_frontier_exits` (`4U`) cap across the whole program.
`GenesisFrontierClass::discovery_prefix_boundary` (ADR 0010 §3) does not yet exist in the codebase —
SEG-007-T126's spike that would have added it was fully reverted before implementation was validated —
but it was designed from the start as a **primary**, first-class frontier class with its own diagnostic
category, its own stop constant, and its own `known_but_unemitted_target`-style admission exemptions
(ADR 0010 lines 174-179), specifically for "known target, not yet emitted."

## Decision

### 1. Call/return ownership: runtime-stack-owned return address, proven as a finite candidate set, dispatched through unchanged `genesis_dispatch`

The static walker stops simulating a call-frame stack at all. Concretely:

- `JSR`/`BSR` admission is unchanged in what it *records* — it still constructs exactly one
  `M68kStaticCall{caller, continuation, callee}` per call site (unchanged struct, unchanged ownership:
  `static_program.hpp` / `static_discovery.*`) — but it no longer threads an accumulating
  `frame_stack` through the walk. Admission independently schedules **two** further walk targets, not
  one nested recursive call: the callee's entry address (proves the subroutine body, exactly as today)
  and the call's own continuation address (proves "what comes after the call," today only reached
  indirectly via a matching `RTS` popping a `frame_stack` entry). Both are ordinary address-only walk
  targets from here on — discovering "what comes after a call" no longer depends on discovering a
  matching return.
- `RTS` no longer resolves, walks, or recurses into any target. It is decoded exactly once like any
  other instruction, and it schedules **no** further walk target — the walk path through an `RTS`
  simply ends there, exactly like the walk path through a proven `walk_indirect_control` candidate set
  ends until each candidate is separately (and already independently) walked. `RTS`'s actual return
  target is a **generated-runtime** fact (the literal popped SSP value the target program computed),
  never a statically-simulated one.
- The proof obligation `RTS` still needs — "which addresses may this return legally land on, so the
  generated code can reject anything else" — is satisfied by treating a return exactly like ADR 0009's
  bounded computed-indirect-target model, with a new, simpler finite-value producer: **the whole-
  program set of every `M68kStaticCall.continuation` address discovery has proven** (deduplicated,
  sorted, deterministic order). Every `RTS` site's generated membership check (already exactly this
  shape today for the `runtime_routing` profile — Context above) tests the runtime-observed popped
  value against this one shared set, then dispatches through the unchanged `genesis_dispatch` on
  membership, and fails closed with full source provenance on non-membership — structurally identical
  to `m68k_indirect_target_member` (ADR 0009).

**This is a deliberate, justified context-insensitive identity, not a context-sensitive abstraction**,
selected over a finite per-call-site or per-entry context abstraction for a stated reason: the
membership check is a *safety net*, not the dispatch decision. The actual jump target is always the
literal value the generated program's own faithfully-executed 68000 stack semantics produced; a
well-formed call graph guarantees that value is always some discovered call's continuation address,
regardless of which specific call reached this exact `RTS` at runtime. Making the candidate set
whole-program instead of per-call-site cannot cause a correctly-executing program to be wrongly
rejected (every legitimate return address is a member by construction) and cannot cause dispatch to an
address that is not a real, statically proven, emitted block (only proven continuation addresses are
ever members) — both are the only two soundness properties this gate exists to guarantee. A per-call-
site or per-subroutine-entry context abstraction would be *more precise* (it could reject a
theoretically corrupted return address sooner), but nothing in current executed evidence motivates that
precision, and building it now would be exactly the kind of speculative abstraction the repository's
"add abstractions only when a current target exercises them" rule forbids. If future executed evidence
shows this coarseness is actually insufficient, a later ADR may introduce a bounded per-entry
abstraction without disturbing this ADR's walker, IR, or dispatch seams — it would only need to widen
the value-set producer, exactly as ADR 0009 already anticipates for its own indirect-target sets ("a
later ADR may add a new producer without changing the target-EA-set abstraction," ADR 0009 line 104).

`M68kStaticEdgeKind::return_to_continuation` and the `runtime_return_targets` union mechanism are
**unchanged in shape** (Context above already shows they are address/call-site-keyed, never
`stack_signature`-keyed); the only change is that discovery may now populate the shared whole-program
set from every call site the (now-unbounded-depth, address-only) walk proves, instead of being starved
by `max_call_frame_depth` before it ever reaches a call site three or more levels deep.

`m68k_discovery_max_call_frame_depth` is **retired as a discovery-walker admission gate**, the same
disposition ADR 0010 gave `m68k_discovery_max_blocks`: the hard refusal in the `JSR`/`BSR`/indirect-JSR
handlers (`static_discovery.cpp` lines 752-758, 789-795, 953-958) is removed, because there is no more
`frame_stack` for it to bound. Its only remaining potential use is the unrelated, out-of-scope
non-`runtime_routing` C11 lowering profile's fixed-size shadow-stack arrays (Context above); the
implementing task decides, by inspecting that profile's own actual needs, whether to keep, rename, or
retire the constant for that unrelated purpose — this ADR does not require touching that profile.

### 2. Host recursion: convert the walker to an explicit iterative worklist

`M68kStaticGraphWalker::walk` becomes an iterative traversal over an explicit, heap-allocated work
queue (implementing task's choice of container — a `std::vector<Address>` treated as a stack is
sufficient) seeded initially by the single reset entry (Decision §4 below generalizes this to a small
seed *set*). Each iteration pops one address, decodes it through the existing decode-cache/ceiling
machinery unchanged, and — using the **same per-instruction-kind branching the current `switch` in
`walk` already contains** — pushes zero, one, or two further addresses onto the queue instead of making
zero, one, or two recursive `walk(...)` calls. This is a mechanical control-flow transformation of the
existing per-kind logic (branch/fallthrough push both successors; `JSR`/`BSR` push callee and
continuation per Decision §1; `RTS` pushes nothing per Decision §1; ADR 0009's
`walk_indirect_control` pushes every admitted candidate, unchanged), not a semantic one, and it is
required independently of Decision §1: T126 already found host-recursion depth from decode-order alone
(not call depth) can overflow a Debug host build. Owned entirely by `src/cpu/m68k/static_discovery.cpp`;
no other seam's contract changes because of this transformation alone. `visited_states_` becomes a
plain `std::unordered_set<Address>` (or `std::set`) — the `stack_signature` component is dropped
entirely, matching Decision §1.

### 3. Termination argument (replaces ADR 0010's incomplete one)

With the traversal-state key redefined to a bare `Address` (Decision §1-2), termination follows from a
genuinely finite-state argument, not a restatement of ADR 0010's:

1. The reachable state space is a subset of the fixed, finite mapped cartridge/RAM image address range
   for the entry's target address space — a hard finite bound independent of program structure.
2. `decode_cache_` / `visited_states_` guarantee every address is decoded and explored **at most once**;
   every re-reach of an already-visited address is O(1) and enqueues no further work (unchanged from
   today — this property never depended on `stack_signature`).
3. The only ways new work is ever enqueued are: (a) an instruction's own direct successors — at most two
   (fallthrough and/or taken branch); (b) a `JSR`/`BSR`'s callee entry address and its own continuation
   address, per Decision §1 — at most two, and independent of call-chain depth or nesting; (c) ADR
   0009's proven finite indirect-candidate set — a hard-capped, monotone fixed point over already-
   decoded facts (ADR 0009's own value-set cap of 256 members, "the analysis iterates the direct graph
   to a fixed point," ADR 0009 lines 91-93, already established sound and unaffected by this ADR); and
   (d) nothing at all for `RTS` (Decision §1). Every enqueue source is therefore bounded per admitted
   address by a small constant, and none of them depend on any depth or context value.
4. `m68k_discovery_max_instructions` (ADR 0010 §2's single bounded-resource ceiling, unchanged by this
   ADR) additionally caps the total number of distinct addresses discovery will ever decode for one
   entry to a fixed constant, independent of program structure, call depth, or recursion shape.

The walk therefore visits a strictly bounded, finite number of distinct states, enqueuing a strictly
bounded amount of further work at each step, and terminates in finite time either by exhausting all
admissible work before the ceiling (a genuine fixpoint, ADR 0010 §1's completeness definition,
unchanged) or by tripping the ceiling first (ADR 0010 §3's deterministic partial-prefix boundary,
unchanged). Unlike ADR 0010's argument, this bound is **not** contingent on keeping any depth constant
small — it holds even with `m68k_discovery_max_call_frame_depth` retired, because no traversal-state
component depends on call-chain depth or context at all.

### 4. Checkpoint-directed demand-driven expansion: adopted, reusing and generalizing ADR 0010 §§3-4

**Adopted as the strongly preferred direction.** Mechanism, evaluated against and reusing the existing
codebase rather than inventing a parallel one:

1. **Multi-entry discovery.** The walker's entry becomes a small seed *set* rather than a single
   address: the fixed reset entry, plus (from round 2 onward) every address the driver below has
   confirmed a real generated-native run actually transferred control to and stopped at. Each seed is
   walked to fixpoint/ceiling exactly per Decision §§1-3, independently; because Decision §1 removes
   all call-context from the traversal-state key, seeds cannot interact through call context (there is
   none), so this generalization adds no new termination risk: Decision §3's finite-state argument
   applies per seed, and the total set of addresses that can ever *become* a seed is itself bounded by
   the same finite reachable-address argument, since a round can only ever promote an address that a
   **prior** round's discovery already statically proved reachable (never a fresh, previously-
   unreachable address) — the seed set is monotonically non-decreasing and bounded by the same finite
   state space Decision §3 already bounds.
2. **Boundary marker, reused verbatim.** Every address discovery proves reachable/callable but does not
   include in the *current* round's walked-and-emitted set — today this only happens at the resource
   ceiling; going forward it also happens whenever the driver deliberately excludes an address from the
   current round's seed set to keep a batch small — is represented exactly as ADR 0010 §3-4 already
   fully specifies: `GenesisFrontierClass::discovery_prefix_boundary`, `classify_frontier`,
   `runtime_frontier_eligible`'s `known_but_unemitted_target`-style exemptions,
   `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY`, `build_genesis_frontier_stop_function`'s
   `genesis_frontier_stop_<addr>` emission, and the unchanged re-entry-refusal argument (ADR 0010 §4:
   `genesis_dispatch` selects only an already-emitted block identity; the boundary's body is an
   unconditional fail-closed stop; there is no path past it without a new build). Only the **trigger
   condition** generalizes, from "only the single resource-ceiling trip" to "any address proven-but-
   not-included-in-this-round"; the representation is unchanged and is not reimplemented.
3. **`known_but_unemitted_target` does not generalize to this role and is not reused for it.** Context
   above shows it is a narrow, secondary, best-effort fallback for a discarded conditional-branch
   sibling with no independent triggering role and a shared 4-exit-wide cap across the whole program.
   `discovery_prefix_boundary` — already fully specified by ADR 0010 §§3-4 and left untouched by this
   ADR — is the correct, already-existing seam for a primary "known target, not yet emitted" boundary;
   `known_but_unemitted_target` is unchanged, untouched, and keeps its existing narrow role exactly as
   SEG-007-T064 left it.
4. **Build/execute/expand loop.** Owned by build-time tooling outside discovery/emission themselves
   (e.g., an extension of the existing `tools/genesis_startup_bridge.py`-class driver used for the
   authorized route today) — not decided in file-level detail by this ADR, since it touches no `src/`,
   `include/`, `runtime/`, or `tools/` file:
   - Run discovery and C11 emission for the current seed set (per items 1-2 above).
   - Build and run the generated program deterministically from the fixed reset state.
   - If the run's `GenesisControlTransfer` stops with `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY`, that
     stop's address is a **confirmed-reached** boundary (real generated-native execution reached it,
     not merely static discovery) — add it to the next round's seed set, and repeat: re-run discovery,
     rebuild, re-run from reset.
   - Stop the loop when the run reaches the declared title-screen checkpoint (a separate successor's
     concern, out of this ADR's scope) or when it stops with any **other** class — an
     `unsupported_cpu_form`/`unsupported_device_access`/`unsupported_memory_region`/
     `unresolved_indirect_target`/`unsupported_interrupt_or_scheduling_event` stop is a real capability
     gap requiring its own implementation task, never an address to auto-expand past.
5. **Invariant preservation.** No step of this loop performs runtime opcode decode. Every expansion
   step is a build-time-only discovery+emission re-invocation of the exact same static pipeline; at
   runtime, `genesis_dispatch` only ever selects among already-emitted block identities, exactly as
   ADR 0002 and ADR 0009 already require, and a `discovery_prefix_boundary` stop's body remains an
   unconditional fail-closed stop with no target-byte access anywhere (ADR 0010 §4, unchanged).

### 5. SEG-007 milestone-selection vocabulary

For SEG-007 capability-map and evidence language going forward:

- **`RUNTIME_SELECTED`**: a capability gap the authorized generated-native route's own actual execution
  (the reset-to-checkpoint run, driven through `genesis_dispatch`/`genesis_runtime_drive`, not static
  discovery alone) has **actually reached and stopped at**, before the declared title-screen checkpoint.
  Only a `RUNTIME_SELECTED` gap may justify or select SEG-007 implementation work.
- **`STATIC_REACHABLE_GAP`**: a gap discovered only along a control-flow path the authorized route's
  actual generated-native execution has **not** (yet) reached — an eagerly-discovered branch sibling, an
  unexecuted call target beyond the currently executed/emitted prefix, or any boundary found purely by
  widening static-discovery scope (including a Decision §4 `discovery_prefix_boundary` that no run has
  yet stopped at). May shape batching or public-source lookahead (`PUBLIC_PATH_PREREQUISITE`-style,
  the project charter), but must never itself be classified, recorded, or selected as a SEG-007 `RUNTIME_SELECTED`
  blocker, and must never appear as a `RUNTIME_SELECTED` capability-map row.
- **`EXECUTION_SELECTED`**: the parent category for any evidence class where actually-executed
  generated-native evidence — never static lookahead alone — is what selects or justifies the described
  work. `RUNTIME_SELECTED` (a CPU/device capability gap) and `MILESTONE_OBSERVABLE` (below, an
  observable/frame-extraction concern) are both `EXECUTION_SELECTED`; `STATIC_REACHABLE_GAP` is not.
- **`MILESTONE_OBSERVABLE`**: once deterministic VDP/device state produced by actual execution is
  sufficient for useful frame extraction, the earliest blocker to producing or comparing that observable
  frame — even where CPU execution could technically continue further — is reported under this category
  rather than as an unrelated `RUNTIME_SELECTED` CPU/device gap. Where deterministic device state exists
  but no useful frame can yet be extracted, the specific sub-state is `observable_missing`
  (the project charter); this is not itself grounds to keep chasing unrelated execution work.

**Unconditional guarantee, independent of whether Decision §4 is adopted or a future ADR rejects it.**
Every frontier discovery ever surfaces carries a reachability provenance tag, mechanically derived from
how it was found, not from how "close" or "certain" it looks statically:

- `runtime_confirmed` — a prior authorized generated-native execution run (the two-byte-identical-run
  protocol every prior SEG-007 task already uses) actually transferred control to and stopped at this
  exact address.
- `static_only` — every other case, including every frontier discovered purely by widening or eagerly
  running static discovery, regardless of discovery mechanism.

Only a `runtime_confirmed` frontier may ever be classified, recorded, or selected as SEG-007
`RUNTIME_SELECTED`, written into `docs/development/sonic-title-critical-capabilities.md`'s
`RUNTIME_SELECTED` row, or cited as justification for a SEG-007 implementation task; a `static_only`
frontier is mechanically `STATIC_REACHABLE_GAP` and disqualified from all of the above no matter how the
discovery/expansion strategy is implemented. If Decision §4 is adopted, this guarantee is a *structural*
property of the expansion loop itself (a round only ever adds a seed that a prior run actually stopped
at — item 4 above already only promotes `runtime_confirmed` addresses by construction). If Decision §4
is rejected by a future ADR in favor of a wider or eager static-discovery strategy, that future ADR must
still explicitly preserve this tagging discipline in whatever mechanism it substitutes: it is not
satisfied merely by rejecting demand-driven expansion, and it is not conditioned on this ADR's Decision
§4 disposition at all.

### 6. Ownership / seam boundary (for the implementing successor task)

| File | Change |
| --- | --- |
| `src/cpu/m68k/static_discovery.cpp` | `walk` becomes iterative (Decision §2); traversal-state key drops `stack_signature` (Decision §1, §3); `JSR`/`BSR` schedule callee + continuation independently instead of recursing with an accumulated `frame_stack`; `RTS` schedules nothing and stops resolving/walking a target; the `max_call_frame_depth` admission refusal is removed; a new whole-program call-continuation candidate-set fact is accumulated (Decision §1). No change to decode-once caching, the block/instruction resource ceiling, or ADR 0009's indirect-candidate walking. |
| `include/segarecomp/machine/genesis/frontend.hpp` | `m68k_discovery_max_call_frame_depth`'s doc comment is rewritten to reflect retirement as a walker gate (Decision §1); `GenesisFrontierClass::discovery_prefix_boundary` is added per ADR 0010 §3 (unchanged by this ADR beyond the generalized trigger condition, Decision §4). |
| `src/machine/genesis/frontend.cpp` | `classify_frontier`/`runtime_frontier_eligible`/`c4_stop_class` gain `discovery_prefix_boundary` exactly per ADR 0010 §3-4; entry-set plumbing for Decision §4's multi-seed discovery if the implementing task lands §4 in the same task. |
| `src/codegen/c11/m68k.cpp` | `return_from_subroutine`'s `runtime_routing == true` lowering is retargeted to read `runtime_return_targets` from the new shared whole-program call-continuation set (Decision §1) instead of a per-walk frame-stack-derived list; its existing membership-check/dispatch shape is otherwise unchanged. The `runtime_routing == false` profile is untouched (out of scope, Context above). |
| `src/codegen/c11/frontend.cpp` | `c4_stop_class` gains `discovery_prefix_boundary` per ADR 0010 §3-4 (unimplemented pending this ADR). |
| `runtime/genesis/runtime.h`, `runtime/genesis/runtime.c` | `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY` per ADR 0010 §4 (unimplemented pending this ADR). |
| `tools/` (a new or extended driver, e.g. building on `tools/genesis_startup_bridge.py`) | Decision §4's build/execute/expand loop, if the implementing task adopts it in scope: re-invoke discovery+build+run with an expanded seed set on a confirmed-reached `discovery_prefix_boundary` stop. |
| `tests/` | Re-point/extend the static-discovery boundary and pipeline fixtures for the iterative walker, retired depth cap, and (if landed) `discovery_prefix_boundary`/multi-seed behavior, using synthetic/legally redistributable inputs only, per SEG-007-T119/T126's established correction pattern. |
| `docs/development/sonic-title-critical-capabilities.md` | Vocabulary and row reconciliation by the implementing task's PR, not this ADR. |

Must **not** be touched by the implementing task per this ADR: MC68000 decode/lift/`effects`/
`M68kOperationEffect` semantics or addressing modes; the ADR 0009 computed-indirect mechanism itself
(its finite-value analysis, its per-candidate walk, and its EA evaluation are reused, not altered); the
ADR 0007 runtime watchdog; the ADR 0006 cartridge-data read path; device/VDP/Z80/PSG/controller-I/O
routing; rendering; interrupt or timing models; the wire report schema beyond the one additive stop
enumerator ADR 0010 §4 already specifies.

### 7. Invariant preservation

- **Deterministic output.** The iterative worklist processes addresses in a deterministic order derived
  from the input image and entry set (implementing task fixes an explicit, documented push/pop order,
  exactly as today's recursive walk has a deterministic call order); the whole-program call-continuation
  set is sorted/deduplicated before emission; the resource ceiling and its trip point remain fixed
  constants. `FrontendPartialProgram` emission remains byte-for-byte deterministic.
- **Fail-closed on genuinely unsupported behavior.** Unchanged. A non-member return value still produces
  a deterministic fail-closed stop with full source provenance; every existing unsupported-instruction,
  unsupported-device, and unresolved-edge category is untouched.
- **No interpreter / JIT / runtime opcode decode.** `genesis_dispatch` still selects only among
  statically emitted block identities; `RTS`'s runtime lowering reads an architectural register/stack
  value and tests set membership, exactly like ADR 0009's EA evaluation — it never fetches or decodes a
  target-program opcode. Decision §4's expansion loop performs every decode at build time only.
- **Source-address provenance through every IR stage.** Unchanged: every discovered/emitted block and
  every boundary marker keeps its full `InstructionProvenance`, exactly as today.
- **No Sonic-specific heuristics, allowlists, or ROM-specific tables.** The context-insensitive
  candidate-set rule, the iterative traversal, and the demand-driven expansion trigger are all generic
  and input-independent; no address list, ROM offset, or per-title branch is introduced.

### 8. Relationship to existing ADRs

- **ADR 0002.** Unchanged. This ADR only changes *how* static discovery proves reachability and *how*
  it structures its own host traversal; the static-dispatch boundary, the "no target-byte fetch / no
  decoder" rule, and the fail-closed-trap-vs-reject distinction are exactly as ADR 0002 and ADR 0010
  already establish.
- **ADR 0003.** Discovery remains the single owner of block/edge/call/frame identity; `M68kStaticCall`
  and its edges are unchanged in shape (Decision §1); no new per-profile projection or executor is
  introduced. `M68kOperationEffect` is untouched.
- **ADR 0006.** No interaction; unaffected by call/return-ownership or discovery-traversal changes.
- **ADR 0007.** Unaffected; complementary as ADR 0010 already records (a static-discovery-stage rule
  vs. a runtime emitted-loop watchdog).
- **ADR 0009.** Fully reused, not altered. Decision §1 explicitly models `RTS`'s return-target proof as
  a new instance of ADR 0009's bounded computed-indirect-target pattern (a new finite-value producer —
  the whole-program call-continuation set — feeding the same membership-check/dispatch shape); ADR
  0009's own indirect-branch/indirect-call mechanism, its 256-member value-set cap, and its fixed-point
  argument are untouched and remain the model for any future finite-index-value producer.
- **ADR 0010.** As stated in the header: §1 (fixpoint completeness definition) and §2's
  `m68k_discovery_max_blocks` retirement / `m68k_discovery_max_instructions` resource-ceiling method are
  preserved; §3-4 (`discovery_prefix_boundary` and its IR/C11/runtime representation) are preserved and
  generalized (Decision §4), not replaced; only §§1-2's termination-argument dependency on
  `stack_signature`/`max_call_frame_depth`, and §2's silence on that constant's own exhaustion, are
  superseded.

## Consequences and Next-Task Shape

A successor implementation task builds exactly Decision §§1-3 (context-insensitive call/return ownership,
the iterative worklist, and the corrected termination argument) against the seam boundary in §6, re-
executes the authorized generated-native Sonic route, and records whatever frontier it actually reaches
next using the §5 vocabulary — a `RUNTIME_SELECTED` capability gap, or, if the route's own execution
actually stops at a `discovery_prefix_boundary`, the honest next step per Decision §4. That task decides,
using its own executed evidence, whether Decision §4's build/execute/expand loop is needed in the same
task to make forward progress, or whether §§1-3 alone already carry the route past the call-frame-depth
family entirely (in which case §4 remains adopted architecture for whenever it is next needed, not a
requirement of that specific task). This ADR does not specify the Sonic title-screen checkpoint's
operational definition (reset/input protocol, frame-stopping criterion, oracle comparison, frame digest)
— that remains a separate, independent successor exactly as this task's non-goals require.
