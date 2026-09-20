# ADR 0010: Static-Discovery Fixpoint Completeness and a Bounded-Resource Ceiling

- Status: Accepted
- Date: 2026-08-30
- Amends: ADR 0002 ("Static Translation and Fail-Closed Execution"), specifically which arm of its
  "either reject translation or produce a deterministic fail-closed trap with source provenance" rule
  applies to Genesis static-discovery *incompleteness* (as opposed to genuinely unsupported behavior).
- Supersedes: the deterministic doubling-search-with-headroom discovery-budget method recorded in the
  `m68k_discovery_max_*` doc comments and adopted by SEG-007-T054, T057, T087, T117, and T119. It does
  not re-open, contradict, or renarrow those tasks' shipped constant values, ADR 0009, or SEG-007-T124.
- Related: ADR 0003, ADR 0006, ADR 0007, ADR 0009.
- Implemented by: SEG-007-T126 (this ADR decides; it changes no `src/`, `include/`, `runtime/`,
  `tools/`, or `tests/` file and no `docs/architecture/*` or capability-map file).

## Context

### The runtime-selected frontier this ADR answers

After ADR 0009's bounded PC-indexed computed `JSR`/`JMP` mechanism landed (SEG-007-T124), the
authorized generated-native route (`genesis-general-startup` on the pinned Sonic image, SHA-256
`46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6`, two byte-identical runs) walks
*through* the `JSR (d8,PC,Xn)` and stops deterministically at `pipeline_stage = static_discovery` /
`category = discovery_budget_exhausted` on `m68k_discovery_max_blocks` (`192U`). The triggering
instruction is an ordinary foldable `BSR.W`; provenance resolves fully from `raw_cartridge_rom`;
`mapping_claims = null`. This is the fourth-plus recurrence of the static-discovery budget family at
the deliberate 32x-of-original ceiling, and SEG-007-T119 recorded the doubling-with-headroom method
itself as exhausted ("no further doubled headroom candidate reachable").

### What the discovery engine actually does (direct inspection)

`discover_m68k_static_graph` (`src/cpu/m68k/static_discovery.cpp`, lines 1042-1047) runs a single
`M68kStaticGraphWalker` (lines 280-305). The core is one recursive walk, `walk(addr, frame_stack)`
(lines 633-851):

- **Traversal-state memoization.** Every `walk` call first forms `state_key = {addr,
  stack_signature(frame_stack)}` and returns immediately if `visited_states_.insert(state_key)`
  reports the state was already seen (lines 634-635). `stack_signature` is the ordered list of
  call-site source addresses (lines 618-623).
- **Decode-once.** A not-yet-seen address is decoded exactly once through `decode_instruction`
  (lines 434-613) and cached in `decode_cache_`; a re-reached address reuses the canonical decode
  without re-decoding (lines 638-645).
- **Block-entry budget.** `note_block_entry` (lines 336-347) inserts a newly recognized block-entry
  address unless `block_entries_.size() == limits_.max_blocks`, in which case it writes
  `DirectFlowDiagnostic::discovery_budget_exhausted` with `unresolved_reason =
  "m68k_discovery_max_blocks"` (lines 338-343) and the walk unwinds `false`.
- **Instruction budget.** `decode_instruction` refuses to decode when `instructions_used_ ==
  limits_.max_instructions` (lines 437-444, counter incremented at line 611), producing the same
  diagnostic with `unresolved_reason = "m68k_discovery_max_instructions"`.
- **Call-frame depth.** `JSR`/`BSR`/indirect-call handlers refuse to recurse past
  `limits_.max_call_frame_depth` (lines 752-758, 789-795, 953-958).
- **ADR 0009 indirect targets.** `analyze_finite_index_values` (lines 866-899) is itself a monotone
  worklist iterated to a fixed point over the already-decoded set; `walk_indirect_control` (lines
  950-1003) admits and walks each proven candidate.

**Termination does not depend on any block or instruction count.** The walk terminates
unconditionally because: (i) each address is decoded at most once and `decode_cache_` /
`visited_states_` make every re-reach O(1) and non-recursive (lines 634-645); (ii) `stack_signature`
length is bounded by `m68k_discovery_max_call_frame_depth` (lines 618-623, 752, 789, 953), so the
`(addr, stack_signature)` state space is finite; (iii) the mapped image is finite and every odd,
unmapped, mid-instruction, non-foldable, or over-depth edge fails closed *without* recursing. This is
the same bounded-state-space argument ADR 0009 already relies on for its own value-flow fixed point
(`docs/decisions/0009-computed-indirect-control-flow-target-resolution.md`, lines 88-93).

`m68k_discovery_max_instructions` (`256U`) and `m68k_discovery_max_blocks` (`192U`) in
`include/segarecomp/machine/genesis/frontend.hpp` (lines 60-87) are therefore **working-set caps**,
not completeness parameters. The doubling method treated them as if a recurring route signature were
telling us the "right" semantic size, and each recurrence doubled toward a self-imposed 32x ceiling.

### How budget exhaustion currently rejects the whole program

`discover_m68k_general_startup` builds `M68kStaticDiscoveryLimits` from those constants
(`src/machine/genesis/frontend.cpp`, line 984), runs the walk (line 986), and on any primary failure
calls `partial_or_rejection` (line 1544). That helper (lines 1467-1538) classifies the failure via
`classify_frontier` (lines 1440-1455). `classify_frontier` maps `reached_unresolved_direct_edge` to
`GenesisFrontierClass::unresolved_indirect_target` (lines 1450-1451) but returns `std::nullopt` for
every other category, including `discovery_budget_exhausted` (default arm, lines 1452-1453). A
`std::nullopt` class forces `reject()` (lines 1476-1494) — a `FrontendRejected`, i.e. whole-program
rejection. Additionally `runtime_frontier_eligible` (lines 257-350) rejects any diagnostic whose
`diagnostic.direct.unresolved_reason` is non-empty (line 266), which every budget diagnostic sets.
So budget exhaustion has two independent locks keeping it out of the partial-program path.

The partial-program path itself is production-proven for the other frontier classes: `build_analysis`
retains every completed block and iteratively prunes any block with an edge to neither a retained
block nor a recognized frontier address (lines 1132-1159); `build_genesis_frontier_stop_function`
(`src/codegen/c11/frontend.cpp`, lines 862-927+) emits one `genesis_frontier_stop_<8-hex-addr>`
function per retained exit; `c4_stop_class` (lines 808-820) maps the class to a `GENESIS_STOP_*`
constant; `genesis_dispatch` registers the stop function at the frontier PC and the runtime drive
loop (`runtime/genesis/runtime.c`, lines 1039-1080) consumes only `GENESIS_CONTINUE_AT_PC` (line
1043) and returns every `GENESIS_STOP` verbatim. `c4_diagnostic` already contains
`GENESIS_DIAG_DISCOVERY_BUDGET_EXHAUSTED` (line 839).

### Shape of the reachable startup prefix (PUBLIC_PATH_PREREQUISITE)

Bounded public-source lookahead (`sonicretro/s1disasm`-style; **not executed evidence**, and it does
not override the runtime-selected block-budget blocker): the reset-to-first-frame path is a bounded,
mostly linear hardware-init sequence (TMSS / VDP register-table load / RAM-VRAM-CRAM-VSRAM clears /
PSG mute / Z80 load+start / checksum loop) followed by a main loop that dispatches on a work-RAM mode
byte. That mode byte is a RAM read, so ADR 0009's finite-index analysis (`MOVEQ` / `ANDI.W` only,
lines 95-104 of ADR 0009) classifies it `unknown`: the mode dispatch stays a single
`reached_unresolved_direct_edge` → `unresolved_indirect_target` frontier and does **not** expand
discovery into the whole game. The reachable static prefix is thus bounded and modest — larger than
the historical 192-block cap, far smaller than "the whole program".

## Decision

**Select option (a): retire SEG-007-T119's doubling-with-headroom method; make fixpoint the
completeness definition; replace the family of tuned semantic caps with one deterministic
bounded-resource ceiling; and replace whole-program rejection on ceiling exhaustion with deterministic
partial-prefix emission through the existing `FrontendPartialProgram` seam.**

Option (b) (a structurally different discovery strategy) is rejected: the walker is *already* a
memoized worklist that terminates by fixpoint independent of the block count (inspection above), so a
new strategy, CFG representation, or seam would be gratuitous abstraction contrary to the repository's
"add abstractions only when a current target exercises them" rule. Option (c) as a *standalone*
completeness answer is rejected because it would permanently freeze the emitted prefix at an
engineering cap even though discovery can finish the reachable prefix; its partial-emission mechanism
is, however, exactly the right *fail-closed* branch for a genuinely oversized program and is adopted
for that role below.

### 1. Completeness guarantee (replaces "reached N blocks")

Discovery is **complete** for an entry when `M68kStaticGraphWalker::run` returns with `failure_`
unset: every admitted control edge (direct branch, fallthrough, direct call, return continuation, and
every ADR 0009 indirect candidate) has been walked, and every reachable instruction decoded exactly
once. This state is always reached in finite time by the memoization + finite-image + bounded-stack
argument recorded in Context. No block or instruction count is part of this definition.

### 2. The `m68k_discovery_max_*` constants become one bounded-resource ceiling

- `m68k_discovery_max_blocks` is **retired as an independent semantic gate**. The hard rejection in
  `note_block_entry` (`static_discovery.cpp` lines 338-343) is removed. Block-entry registration
  still happens; it is simply no longer independently capped. T126 chooses the minimal-churn
  representation: either delete the field from `M68kStaticDiscoveryLimits` and its constant, or keep
  the field and set it to a value `>=` the instruction ceiling so it can never trip first. The doc
  comment is rewritten to describe a derived quantity, not a doubling search.
- `m68k_discovery_max_instructions` becomes the **single deterministic bounded-resource ceiling**: an
  upper bound on the number of distinct instruction addresses discovery will decode for one entry.
  Because `block_entries_`, `edges_`, `frames_`, `decode_cache_`, `decode_order_`, and every retained
  fact are all `O(decoded instructions)`, this one number bounds the entire discovery working set.
- **The new justification method (replaces doubling-with-headroom).** The ceiling value is fixed
  **once**, by a stated resource argument, in the same category ADR 0007 established for
  `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` — "a bounded project-engineering ceiling ... not console
  timing, not a hardware fact"
  (`docs/decisions/0007-generated-runtime-loop-progress-watchdog.md`, lines 48-52). Concretely: pick
  a peak discovery working-set memory budget `B` for the reference build (a small, stated number of
  MiB), measure `s = sizeof` of one decoded/cached instruction plus its amortized edge/fact overhead,
  and set the ceiling to `floor(B / s)` rounded down to a documented figure. T126 records the actual
  `B`, `s`, and arithmetic in the doc comment. The value is a *resource limit*, so a round memory
  budget is legitimate; what is forbidden is choosing or re-choosing the *instruction count* to make
  a particular ROM's `discovery_budget_exhausted` signature "stop recurring".
- **When it is next revisited.** Only when a future *fully supported, proven-reachable* prefix is
  shown — by discovery reaching fixpoint under a temporary local raise with no unsupported frontier
  in between — to legitimately need a larger working set, or when the reference build's memory budget
  `B` is deliberately changed. Never in response to a recurring route signature, and never by
  doubling.
- `m68k_discovery_max_call_frame_depth` (`2U`) and `m68k_discovery_max_frontier_exits` (`4U`,
  `frontend.hpp` line 55) are **unchanged**. They are structural/representational bounds on the
  static-program and partial-program shape, not the completeness gate, and were explicitly not
  implicated by T117 or T119.

### 3. Deterministic fail-closed condition (replaces whole-program rejection)

When the single resource ceiling *is* reached (a genuinely oversized reachable region — not the
pinned Sonic prefix), discovery must **not** produce a `FrontendRejected`. Instead:

- `decode_instruction` still stops at the ceiling with `discovery_budget_exhausted` at the exact
  instruction address whose decode would have crossed it, retaining full `InstructionProvenance`
  (source address, image offset, primary bytes, length) exactly as today.
- A new `GenesisFrontierClass::discovery_prefix_boundary` value is added
  (`include/segarecomp/machine/genesis/frontend.hpp`, line 53).
- `classify_frontier` (`src/machine/genesis/frontend.cpp`, lines 1440-1455) maps
  `DirectFlowDiagnostic::discovery_budget_exhausted` → `discovery_prefix_boundary`, with no
  frontier-access requirement.
- `runtime_frontier_eligible` (lines 257-350) admits the new class with the **same exemptions
  `known_but_unemitted_target` already has** (lines 298-299, 343-350): it accepts the underlying
  category, may carry a candidate target address, and needs no `access` request. The
  `unresolved_reason`-non-empty disqualifier (line 266) is relaxed *for this class only* (or T126
  clears `unresolved_reason` on this path before promotion) — the reason string's information is
  preserved in host-side evidence, not the wire frontier.
- The existing `partial_or_rejection` / `build_analysis` / `build_genesis_frontier_stop_function`
  path then emits every completed block discovered before the ceiling plus **one**
  `discovery_prefix_boundary` frontier exit at the frontier instruction. `build_analysis`'s existing
  independence pruning (lines 1132-1159) guarantees the retained prefix has no dangling edge.

### 4. Boundary-marker representation: IR -> C11 -> runtime

- **IR: no change.** The boundary marker is a frontier *exit*, not an IR operation — identical to how
  `unresolved_indirect_target` and `known_but_unemitted_target` work today (neither adds an IR node).
  IR continues to carry only the already-required source provenance of the *discovered prefix*
  instructions (`docs/architecture/pipeline.md`, line 26). No `M68kIrOperation`, `M68kIrKind`, or
  `DirectFlowIrKind` value is added or changed.
- **C11:** `c4_stop_class` (`src/codegen/c11/frontend.cpp`, lines 808-820, and the mirror in
  `src/machine/genesis/frontend.cpp`, lines 19-28) gains
  `GenesisFrontierClass::discovery_prefix_boundary -> "GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY"`.
  `c4_diagnostic` already resolves `GENESIS_DIAG_DISCOVERY_BUDGET_EXHAUSTED` (line 839).
  `build_genesis_frontier_stop_function` (lines 862-927+) emits the usual uniquely-named
  `genesis_frontier_stop_<8-hex-source-address>` returning a `GENESIS_STOP` transfer with the new
  stop class, the discovery-budget diagnostic category, and the frontier instruction provenance. The
  emitted block/dispatch assembly is unchanged in structure.
- **Runtime:** `runtime/genesis/runtime.h` `GenesisStopClass` (lines 400-409) gains
  `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY = 9`; `runtime.c`'s `genesis_stop_class_name` and the
  stop/diagnostic pair table gain the corresponding arm. No ABI struct field is added; no drive-loop
  behavior changes — `genesis_runtime_drive` already returns every non-`CONTINUE` transfer verbatim
  (line 1043).
- **Re-entry refusal without a decoder.** The frontier PC is registered in the emitted block list
  bound to its `genesis_frontier_stop_<addr>` function. When `genesis_dispatch` is next asked to
  continue at that PC it selects that function, which returns `GENESIS_STOP`. `genesis_dispatch`
  never fetches or decodes a target opcode (ADR 0002, lines 16-19; ADR 0009, lines 28-36); it selects
  only an emitted block identity, and the boundary is one such identity whose body is an
  unconditional fail-closed stop. There is no path by which execution proceeds past the boundary.

### 5. Ownership / seam boundary

Adds or changes (SEG-007-T126 scope):

| File | Change |
| --- | --- |
| `include/segarecomp/machine/genesis/frontend.hpp` | Rewrite the `m68k_discovery_max_*` doc block (lines 60-87); retire `m68k_discovery_max_blocks` as a gate; redefine `m68k_discovery_max_instructions` as the resource ceiling with the derived-value method; add `GenesisFrontierClass::discovery_prefix_boundary` (line 53). |
| `src/cpu/m68k/static_discovery.cpp` | Remove the `note_block_entry` hard rejection (lines 338-343); keep the `decode_instruction` ceiling check (lines 437-444) as the single guard. No change to `walk`, edge/frame model, ADR 0009 indirect handling, provenance, or `stack_signature`. |
| `src/machine/genesis/frontend.cpp` | `classify_frontier` (lines 1440-1455) maps `discovery_budget_exhausted`; `runtime_frontier_eligible` (lines 257-350) admits the new class with `known_but_unemitted_target`-style exemptions; `c4_stop_class` (lines 19-28). |
| `src/codegen/c11/frontend.cpp` | `c4_stop_class` (lines 808-820); the new class flows through the unchanged `build_genesis_frontier_stop_function`. |
| `runtime/genesis/runtime.h`, `runtime/genesis/runtime.c` | New `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY` enumerator plus name/pair-table arms. |
| `tests/` | Re-point the discovery-budget boundary fixtures (section below). |
| `docs/development/sonic-title-critical-capabilities.md` | Capability-map reconciliation (by T126's PR, not this ADR's). |

Must **not** be touched: MC68000 decode/lift/`effects`/`M68kOperationEffect` semantics or addressing
modes (ADR 0003, lines 93-118); the ADR 0009 computed-indirect mechanism, its finite-value analysis,
or its per-candidate walk; the ADR 0007 runtime watchdog; the ADR 0006 cartridge-data read path;
device / VDP / Z80 / PSG / controller-I/O routing; rendering; interrupt or timing models;
`genesis_dispatch` / `genesis_runtime_drive` control-transfer logic; the wire report schema beyond
the one additive stop enumerator.

### 6. Invariant preservation

- **Deterministic output.** `decode_order_` and `block_entry_order_` are insertion-ordered vectors
  (`static_discovery.cpp` lines 293-297, 1010-1012); the walk order is a deterministic function of
  the input image and entry; the resource ceiling is a fixed constant; the frontier instruction at
  which it trips is therefore deterministic. The `FrontendPartialProgram` emission path is already
  byte-for-byte deterministic in production.
- **Fail-closed on genuinely unsupported behavior.** Unchanged. Unsupported instructions, devices,
  and unresolved edges still produce their existing categories and stops. Only the *incompleteness*
  signal — hitting an engineering working-set cap, which is not "unsupported behavior" — moves from
  ADR 0002's "reject" arm to its "deterministic fail-closed trap with source provenance" arm.
- **No interpreter / JIT / runtime opcode decode.** The boundary is a static stop function;
  `genesis_dispatch` still selects only emitted block identities; no target-byte access is added
  anywhere (ADR 0002 lines 16-19; ADR 0009 lines 28-36; `pipeline.md` lines 27-29).
- **Source-address provenance through every IR stage.** Every discovered/emitted block keeps its
  provenance unchanged; the boundary marker carries the frontier instruction's full
  `InstructionProvenance`, exactly like every existing frontier stop
  (`build_genesis_frontier_stop_function`, `src/codegen/c11/frontend.cpp` lines 906-927).
- **No Sonic-specific heuristics, allowlists, or ROM-specific tables.** The fixpoint rule and the
  resource ceiling are generic and input-independent; no address list, ROM offset, or per-title
  branch is introduced.

### 7. Relationship to existing ADRs

- **ADR 0002.** This ADR refines *which* of ADR 0002's two sanctioned outcomes applies to discovery
  incompleteness: the source-provenanced fail-closed trap, not whole-program rejection. ADR 0002's
  static-dispatch boundary, its "no target-byte fetch / no decoder" rule, and its
  "successful exit means no reachable behavior escaped the declared envelope" property are unchanged
  — a partial program with a boundary exit is a non-success exit and makes no completeness claim past
  the boundary.
- **ADR 0003.** Discovery remains the single owner of block/edge/call/frame identity; no new
  per-profile projection or executor is introduced; the partial prefix flows through the existing
  shared `build_analysis`. `M68kOperationEffect` is untouched.
- **ADR 0006.** No interaction. The boundary is control-side; runtime-computed cartridge-*data* reads
  still route through `genesis_route_access` exactly as ADR 0006 specifies, and ADR 0006 explicitly
  leaves instruction dispatch unaffected (lines 128-131).
- **ADR 0007.** Complementary and non-overlapping. ADR 0007 is a *runtime* finite-progress guard for
  emitted DBcc/DBF loops; this ADR is a *static-discovery* completeness rule at an earlier pipeline
  stage. They share only the "labelled project-engineering ceiling, not a hardware fact" pattern for
  their respective bounds. The ADR 0007 watchdog still governs the emitted prefix's runtime loops
  unchanged.
- **ADR 0009.** Untouched. The computed-indirect target-set mechanism, its bounded value-flow fixed
  point, and its per-candidate admission/walk run under the retired-cap / fixpoint regime like every
  other edge. A RAM-indexed mode dispatch with an `unknown` index remains a single
  `unresolved_indirect_target` frontier — that is ADR 0009's existing fail-closed result, and any
  future finite-index producer for it is a separate ADR, not this one.

## Consequences and SEG-007-T126 Acceptance Shape

SEG-007-T126 implements exactly section 1-5 above and must meet all of:

1. **Observable generated-native-route outcome.** For `genesis-general-startup` on the pinned Sonic
   image (SHA-256 `46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6`), two
   byte-identical runs: the route walks *through* the current `BSR.W` block-budget frontier and every
   subsequent statically clean block of the reset init prefix, and stops at a **qualitatively
   different** boundary — the RAM-indexed computed mode dispatch (`unresolved_indirect_target`), the
   next genuinely unsupported CPU/device operation in the init prefix, or, only if the reachable
   supported prefix legitimately exceeds the new resource ceiling, a deterministic
   `FrontendPartialProgram` whose final exit is the `discovery_prefix_boundary` marker at a stable
   source-provenanced address. `discovery_budget_exhausted` for `m68k_discovery_max_blocks` must no
   longer appear for this route, and the route must never return `FrontendRejected` for a
   budget/ceiling reason. T126 reports whichever of these outcomes actually occurs as the new
   normalized frontier.
2. **Determinism.** Byte-identical generated C across two runs with identical input and options;
   where a frame is produced, an identical frame digest; generated C compiles under the strict C11
   contract; the generated program contains no runtime instruction fetch or decode. Focused
   strict-C11 generated-output compilation is included for the emitter change.
3. **Regression-fixture correction pattern.** The discovery-budget boundary fixtures
   (`cpu_m68k_static_discovery_boundary_test`, `cpu_m68k_static_discovery_tests`,
   `m68k_pipeline_tests`, and any fixture asserting the block-count rejection at the named constant)
   are re-pointed, using synthetic / legally redistributable inputs only:
   - a synthetic fully-supported graph with more distinct blocks than the retired 192 cap that
     asserts fixpoint completion with **no** budget stop and all blocks emitted;
   - a synthetic graph engineered to exceed the resource ceiling that asserts a deterministic
     `FrontendPartialProgram` whose final exit is `discovery_prefix_boundary` at a stable provenanced
     address, byte-identical across repeats, and **not** a `FrontendRejected`;
   - an unchanged assertion that a genuinely unsupported instruction or edge still fails closed with
     its existing category and stop.
   Fixtures already referencing the constants symbolically need no literal edit.
4. **Capability-map reconciliation** (`docs/development/sonic-title-critical-capabilities.md`, in
   T126's PR): the block-budget recurrence lineage row is superseded by a new `SUPPORTED` row
   "Static-discovery fixpoint completeness + bounded-resource ceiling (ADR 0010)"; the
   `m68k_discovery_max_blocks` `discovery_budget_exhausted` signature must no longer be presented as a
   live `RUNTIME_SELECTED` frontier; the newly exposed init-prefix frontier becomes the new
   `RUNTIME_SELECTED` blocker, with `observable_state` updated per the actual result (`observable_missing`
   if deterministic device state exists but no useful frame can yet be extracted).

The runtime-selected block-budget blocker is considered resolved only when the pinned route
demonstrably no longer stops on `m68k_discovery_max_blocks` and the fixpoint-completeness plus
bounded-resource-ceiling plus partial-boundary behavior is proven by the synthetic fixtures above and
independent adversarial review inside T126's PR.
