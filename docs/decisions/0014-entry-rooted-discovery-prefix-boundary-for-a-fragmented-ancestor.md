# ADR 0014: Entry-Rooted Discovery-Prefix Boundary Construction for a Fragmented-Ancestor Boundary

- Status: **Accepted** — the SEG-007-T139 feasibility spikes establish that this architecture
  resolves its own motivating problem on the authorized pinned route: deterministic entry-connected
  admission succeeds; retained block-graph reachability succeeds with **zero orphaned retained
  blocks**; the real 18-exit open frontier is completely probe-verified and representable within the
  fixed 64-exit representation-safety bound; `partial_or_rejection` produces a `FrontendPartialProgram`;
  and P1-P6 are structurally satisfiable. The remaining pinned-route emission stop is a **separate,
  pre-existing C4 CPU/IR lowering frontier** (7 retained-prefix lowering gaps, characterized in
  "Feasibility evidence") — a CPU decode/lift/IR/C11 capability owner, **not** evidence that this
  boundary architecture is undecided. Architecture acceptance belongs to SEG-007-T139; orthogonal
  CPU/C4 capability completion does not own this ADR's decision status.
- Date: 2026-09-01
- Decided by: SEG-007-T139, from independent inspection of the current static-discovery
  admission-ordering, `M68kStaticGraphWalker::run`, `build_analysis`, `partial_or_rejection`,
  `classify_frontier`, and `runtime_frontier_eligible` seams at this repository head, consuming
  SEG-007-T138's recorded diagnosis of the fragmented-ancestor boundary condition, and revised from
  two ephemeral, reverted feasibility spikes of the selected mechanism against the authorized pinned
  route.
- Amends: ADR 0013 (§2, §4, §5) and ADR 0010 (§3), in each case only where noted below. Selects a
  traversal order (breadth-first / entry-connected) within the container choice ADR 0011 §2 already
  delegates to the implementer; that is a selection, not an amendment to ADR 0011. **Raises the value
  of `m68k_discovery_max_frontier_exits` (`4U` → `64U`)** — a partial-program *representation-safety*
  bound introduced by SEG-007-T064, sized then only for that task's synthetic multi-exit fixtures,
  never widened, and never before selected as the pinned-route blocker; it is explicitly **not** one
  of the discovery-resource ceilings (`m68k_discovery_max_instructions`, `m68k_discovery_max_blocks`,
  `m68k_discovery_max_call_frame_depth`) and is not sized by their doubling-search method
  (Decision §3).
  **Supersedes** ADR 0013 §2's claim that "the retained prefix is byte-identical to the prefix the
  ceiling truncates today" (the admitted set is now the entry-connected breadth-first prefix, which is
  a different deterministic set from the pre-ADR-0014 last-in-first-out worklist prefix, and for the
  pinned route ADR 0013's mechanism currently emits no prefix at all — it fails whole-program closed,
  SEG-007-T138); ADR 0013 §2's "exactly one boundary provenance probe" per ceiling trip (the probe is
  now run once per boundary address in a bounded boundary set); ADR 0013 §4's "exactly one boundary
  address" framing and ADR 0013 §5's single-referring-block treatment of the "ceiling reached
  mid-block" case (generalized to every open control-flow edge at the admitted-region frontier); and
  ADR 0013 §5's assertion that "a boundary address can never later be admitted" (a boundary address
  may have been admitted into an incomplete block, but the retained-prefix construction structurally
  excludes it from `prefix.decoded` / `prefix.ir` and from emission).
  **Leaves intact**: ADR 0013 §2's side-effect-free probe discipline and its mandatory
  `M68kDecodeProfile::general_startup` decode; ADR 0013 §3's empty-`unresolved_reason` +
  `has_target == false` discriminator; ADR 0013 §4's single *producer* rule (the instruction
  admission ceiling remains the sole static producer of the class); ADR 0013 §6's emitted
  representation (the `GenesisFrontierClass` value, `classify_frontier` arm, both
  `runtime_frontier_eligible` mirrors, both `c4_stop_class` mirrors, the `genesis_dispatch` frontier
  arm, `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY = 9`, and the driver `STOP_DIAGNOSTIC_PAIRS` entry),
  which this ADR reuses unchanged; ADR 0013 §7's Phase A / Phase B separation, its one-way
  dependency, its fixed constants (`m68k_discovery_max_seed_entries = 4U`,
  `m68k_expansion_max_rounds = 4U`), and its terminal outcomes (`expansion_no_progress`,
  `expansion_seed_limit_reached`); ADR 0013 §8's invariant-preservation guarantees *except* its
  "Deterministic output" byte-identical-to-today sub-clause, which is superseded in the same way as
  ADR 0013 §2's identical sentence (Decision §7 below); ADR 0011
  Decisions §§1-3 and §5 in full; ADR 0010 §1's fixpoint completeness definition, §2's retirement of
  `m68k_discovery_max_blocks` as an independent gate and its bounded-resource-ceiling justification
  method, §3's core choice that ceiling exhaustion is ADR 0002's *fail-closed trap* arm rather than
  its *whole-program reject* arm, and §4's IR no-change rule and re-entry-refusal argument (as
  corrected by ADR 0013 §6).
- Related: ADR 0002, ADR 0009, ADR 0011, ADR 0012.
- Implemented by: SEG-007-T140, not this ADR. This ADR changes no `src/`, `include/`, `runtime/`,
  `tools/`, or `tests/` file, no `docs/architecture/*` file, and no capability-map row.

## Context

### A. What SEG-007-T138 diagnosed, independently reconfirmed at this head

SEG-007-T138 ran the authorized generated-native route on the pinned commercial image
(SHA-verified, commercial mode, `--diagnose-frontier`, two byte-identical runs) and recorded a
deterministic `result = rejected`, `category = discovery_budget_exhausted` at the `static_discovery`
pipeline stage. Normalized shape, image-independent:

1. The stop is the **ADR 0013 instruction admission ceiling** (`m68k_discovery_max_instructions`),
   not the retired block-entry or call-frame gates.
2. The ADR 0013 §2 **boundary provenance probe SUCCEEDS**: a clean `M68kDecodeProfile::general_startup`
   decode with full `InstructionProvenance`, one reconstructed `raw_cartridge_rom` mapping claim, one
   `instruction_read` bus record, empty `unresolved_reason`, and no `direct.has_target`.
3. `classify_frontier` therefore returns `GenesisFrontierClass::discovery_prefix_boundary`
   (the ADR 0013 §3 discriminator holds).
4. **No runnable partial program is emitted anyway.** `runtime_frontier_eligible` fails the promotion
   at its entry-rooted reachability check, so `partial_or_rejection` fails the whole promotion closed
   to a whole-program `FrontendRejected`.

Direct seam inspection at this head confirms the mechanism:

- `M68kStaticGraphWalker::run` (`src/cpu/m68k/static_discovery.cpp`) is now the ADR 0011 §§1-3
  address-only iterative worklist. It is a `std::vector<Address>` drained **last-in-first-out**
  (`worklist_.back()` / `pop_back()`), i.e. a depth-first admission order, and `JSR`/`BSR` push
  `[callee, continuation]` so the continuation is popped first. The `m68k_discovery_max_instructions`
  check (`decode_instruction`, checked before any decode) is the single resource ceiling; on a trip
  it runs the ADR 0013 §2 probe and stores the boundary issue as the walk's primary issue.
- `compute_candidate_frontier_addresses` (`src/machine/genesis/frontend.cpp`) returns **only** the
  primary failure's source address plus every best-effort sibling (SEG-007-T062/T064) failure's
  source address — the set of addresses discovery *explicitly recorded an issue for*. It is **not**
  the set of open control-flow edges at the admitted-region frontier.
- `build_analysis(completed_blocks_only = true, exclude_frontier_instruction = true)` retains only
  fully decoded blocks, then **iteratively erases** any retained block that has an outgoing edge
  whose target is neither a retained block entry nor a member of
  `compute_candidate_frontier_addresses()`. A depth-first-admitted prefix that dives deep along one
  path leaves the *un-taken* successors of conditional branches on that path unadmitted; those
  successor addresses are not in `compute_candidate_frontier_addresses()` (discovery recorded no
  issue for them — it simply never reached them), so every block that branches to one is erased, and
  the erase cascades toward the entry. Blocks downstream of an erased block that have no other
  retained predecessor are not erased (they have no dangling *outgoing* edge) but become unreachable
  from the entry.
- `runtime_frontier_eligible` (both mirrors, `src/machine/genesis/frontend.cpp` and
  `src/codegen/c11/frontend.cpp`) then runs a breadth-first reachability walk from
  `prefix.startup_ingress->entry` over the retained edges and rejects the promotion unless every
  retained block entry **and** the boundary address are reachable. For the pinned route the
  admitted-instruction working set is a depth-first-order prefix, not an entry-connected control-flow
  prefix: the boundary's own referring block and a multi-region span of retained blocks are
  unreachable from the reset entry through retained edges, and the boundary address itself is
  unreachable. The promotion fails whole-program closed — which is ADR 0013's fail-closed intent, but
  it means the pinned route produces no generated execution at all.

### B. Why this is a genuine architecture fork and not a bounded fix

ADR 0013 §5 fixes the retained-prefix invariant (P1-P5) by **reuse**: it asserts that
`build_analysis(true, true)` plus `partial_or_rejection`'s `retained_addresses == sibling_addresses`
equality already construct and validate an entry-rooted prefix, and that the "ceiling reached
mid-block" case is covered by the single existing `reaches_frontier` synthesized-fallthrough arm for
**one** referring block. That is correct only when the admitted working set is already
entry-connected and the ceiling truncates a single straight-line block. It does not define how to
construct a runnable, entry-rooted `discovery_prefix_boundary` partial program when the global
depth-first admission counter trips many call frames deep and fragments the boundary's entire
ancestor path. Each of SEG-007-T138's candidate resolutions amends ADR 0013's accepted mechanism
and/or ADR 0010 §3, so the choice among them is an ownership decision for partial-program admission
ordering and prefix construction, recorded here.

## Decision

Adopt candidate **(a)** — entry-connected admission ordering so the admitted working set is always
entry-rooted (Decision §1) — **together with Decision §1b** (the retained *block graph* must be
entry-reachable, not only the instruction stream: model every retained call as reaching its
continuation) and a bounded generalization of ADR 0013 §5's "ceiling reached mid-block" rule from a
single referring block to **every open control-flow edge at the admitted-region frontier**, each
represented as a probe-verified `discovery_prefix_boundary` exit, bounded by the
`m68k_discovery_max_frontier_exits` **representation-safety bound raised `4U → 64U`** (Decision §3).
Candidate **(d)** — permanent whole-program rejection — is retained only as the **bounded fail-closed
fallback** when the frontier is genuinely wider than `64` or when any boundary probe legitimately
fails; because a construction mechanism is provided and demonstrated to resolve the motivating
problem on the pinned route (entry-connected, zero orphaned blocks, 18 probe-verified exits within
the `64` bound, `FrontendPartialProgram` produced), no alternative Phase B seed mechanism is
introduced and ADR 0013 §7 Phase B is preserved verbatim.

Candidates (b) and (c) are rejected: (b) "retain the truncated ancestor call chain as completed
blocks" would emit blocks whose bodies were never fully decoded, lifted, or operand-validated,
violating the no-incomplete-semantic-state rule (P3 below), and completing them would require
decoding past the fixed ceiling, which the non-goals forbid; (c) "redefine the boundary as the
shallowest entry-connected truncation point on the boundary's ancestor chain" reintroduces the
call-context / ancestor-chain identity that ADR 0011 §1 deliberately retired from the walker's
traversal state, and would need a per-call-site notion the address-only worklist no longer carries.

### 1. M1 — Entry-connected admission order

The static walker's worklist discipline is changed so that an address is **admitted** — decoded
through the ceiling-guarded `decode_instruction`, counted against `m68k_discovery_max_instructions`
at its existing increment site, cached, block-registered, lifted, and made emittable — **only** if it
is a member of the seed set (ADR 0013 §7: round 1 is exactly `{reset entry}`) or a direct
control-flow successor of an already-admitted address.

- The existing `std::vector<Address>` worklist is drained **first-in-first-out** (breadth-first) from
  the seed set. ADR 0011 §2 already delegates container choice to the implementer ("a
  `std::vector<Address>` treated as a stack is sufficient"); selecting a queue instead is within that
  delegation and does not reopen ADR 0011.
- Every address that is *processed* — and therefore reaches the admission increment — was enqueued by
  a previously processed address, which was itself reachable from the seed set through admitted
  addresses. So at every point during the walk the set of admitted addresses induces a control-flow
  subgraph that is connected and contains the seed. This is the structural property the pre-ADR-0014
  depth-first drain does not guarantee once continuation-first `JSR`/`BSR` scheduling and best-effort
  sibling exploration interleave.
- **Best-effort sibling exploration** (SEG-007-T062/T064) that begins from an address not connected
  to the admitted region must not register retained blocks and must not count admissions. It remains
  a purely diagnostic secondary pass whose issues may only become represented exits through the
  bounded boundary set of Decision §2, never through silent block retention.
- **Determinism.** Successors are enqueued in the existing fixed order (fallthrough before taken
  branch; callee before continuation; ADR 0009 indirect candidates in their existing sorted order),
  and the queue is drained in enqueue order, so the admitted set for a given seed set is a
  deterministic pure function of the image. ADR 0011 §3's termination argument is order-independent
  (every address processed at most once via `visited_states_`; bounded enqueues per address; the
  fixed ceiling caps the total) and is unaffected by the last-in-first-out → first-in-first-out
  change.

The implementer may choose any traversal order that **provably** maintains the connected-admitted-
prefix invariant and is deterministic; first-in-first-out from the seed set is the reference choice.

### 1b. M1b — The retained *block graph* must be entry-reachable, not only the instruction stream

The SEG-007-T139 spikes established that changing the drain order alone is **necessary but not
sufficient**. `runtime_frontier_eligible`'s promotion gate walks reachability over the *reconstructed
retained block graph* (`prefix.static_blocks` + `prefix.static_edges`), not over the raw admitted
instruction stream. Two independent gaps appear at the horizon of any bounded 256-instruction prefix:

- **Pure first-in-first-out (breadth-first) orphans call continuations.** A retained `JSR`/`BSR`
  continuation block is currently reachable in that graph **only** through a synthesized
  `return_to_continuation` edge, which `synthesize_return_edges` produces **only** when the callee's
  `RTS` was itself admitted. A breadth-first prefix admits many subroutine *entries* without admitting
  their `RTS`, so their continuations become unreachable islands (the spike measured 80 of 111
  retained blocks orphaned this way on the pinned route).
- **Pure last-in-first-out (depth-first) orphans branch siblings** — SEG-007-T138's original
  diagnosis (the un-taken conditional-branch successor on the deep path is never admitted and its
  referring block is cascade-erased).

Decision M1b: the partial-program reachability model treats **every retained call as reaching its
continuation**, independent of whether the callee's `RTS` was admitted. Concretely, the retained
static graph gains a `call`-source-to-`continuation`-target reachability relation for every retained
`M68kStaticFrame` (both `runtime_frontier_eligible` mirrors and `build_analysis`'s own pruning use
it), so a retained continuation is reachable exactly when its call site is. The callee region left
un-admitted by the ceiling is **separately** represented as a Decision §2 boundary exit; no
instruction past the ceiling is admitted, decoded, lifted, or emitted (P3). The relation is
conservative for reachability only: a continuation block whose callee body reaches a fail-closed
boundary stop at runtime is emitted but is simply never dispatched to — it makes no completeness or
liveness claim, exactly as ADR 0002 already permits for any non-success exit. This is a bounded,
generic change to the reachability model — not a new admission budget, not an interpreter, and not a
runtime decode. It is the smallest change that makes the retained block graph as connected as the
retained instruction stream, which is what P1 requires. ADR 0011 Decision §1's "a call always has a
continuation reachable independently of discovering a matching RTS" already establishes this exact
principle for *admission scheduling*; M1b applies the same principle to the *retained-graph
reachability check*.

### 2. M2/M3 — The bounded, probe-verified boundary set

On a ceiling trip, the single producer (the `m68k_discovery_max_instructions` ceiling handler in
`M68kStaticGraphWalker`) enumerates, deterministically from its own traversal state (`edges_`,
`block_entries_`, `decode_cache_`), the **boundary address set**:

> the ceiling-trip address, plus every distinct address that is the target of an admitted block's
> outgoing control-flow edge and is not the entry of a fully decoded block.

This is the set of open control-flow edges at the admitted-region frontier. It subsumes ADR 0013
§5's single "ceiling reached mid-block" case (the mid-block continuation address is one such target,
reached through the existing synthesized `fallthrough` edge) and the un-taken conditional-branch
siblings that Context §A shows currently cause the cascade erase.

For **each** boundary address the producer runs **exactly one** ADR 0013 §2 boundary provenance
probe — the same side-effect-free `M68kDecodeProfile::general_startup` decode, the same image-global
offset repair, the same `set_pc_based_image_offset` / `reconstruct_single_mapping_claim` /
`reconstruct_instruction_read_access` reconstruction, no `target`, no `unresolved_reason`. The probe
must not mutate any walker state and must not use a narrower profile (ADR 0013 §2 and its "Binding
terminal outcomes" item 1 are preserved exactly). The ceiling-trip address's probe result is the
walk's **primary** issue; each other boundary address's probe result is a **secondary** issue of the
same `discovery_budget_exhausted` category with empty `unresolved_reason`.

`compute_candidate_frontier_addresses` (`src/machine/genesis/frontend.cpp`) is extended so that these
secondary boundary issues contribute their source addresses to the candidate-frontier set exactly as
best-effort sibling failures already do (the existing `diagnostic.provenance`-present restriction is
retained — a probe that produced no provenance is not a boundary and its address is not a safe edge
target). `classify_frontier` maps each such `discovery_budget_exhausted` issue to
`discovery_prefix_boundary` under the unchanged ADR 0013 §3 discriminator, and each flows through the
unchanged ADR 0013 §6 emitted representation as one `genesis_frontier_stop_<addr>` exit.

### 3. M4 — Bounded, fail-closed

The whole promotion fails whole-program closed to a `FrontendRejected` (ADR 0002's / ADR 0010 §3's
reject arm, unchanged in this case) when **any** of the following holds:

- any boundary-address probe legitimately fails (unmapped or conflicting source, truncated, illegal,
  unsupported form, decoded length exceeding the covering claim) — ADR 0013 §2 and its terminal
  outcome 1, preserved: provenance is never fabricated to force a boundary;
- the boundary address set has more than `m68k_discovery_max_frontier_exits`
  (`include/segarecomp/machine/genesis/frontend.hpp`) members — exceeding it is a deterministic
  build-time refusal, never an unsound artifact;
- `partial_or_rejection`'s existing `retained_addresses != sibling_addresses` equality check fails
  (`src/machine/genesis/frontend.cpp`) — unchanged.

#### `m68k_discovery_max_frontier_exits`: from a synthetic-fixture bound to a principled
representation-safety bound

`m68k_discovery_max_frontier_exits` was introduced by SEG-007-T064 as "the smallest value that
provably covers this task's own synthetic Checkpoint 4 fixtures", with the *same* "internal
engineering bound, not a hardware fact" discipline as the discovery-resource ceilings — but it is a
different **kind** of quantity. It does not gate discovery, does not appear in any
`discovery_budget_exhausted` signature (SEG-007-T087 code-reading confirmed this), and is not sized by
the doubling-search-with-headroom method. It bounds only the count of `UnresolvedFrontier` records a
promoted `FrontendPartialProgram` may carry, i.e. the number of `genesis_frontier_stop_<addr>`
functions and `runtime->pc` comparison arms the generated stop dispatcher contains. It had **never**
been widened and had **never** been the pinned-route blocker before ADR 0014 exposed a legitimate
entry-connected frontier wider than `4`.

The SEG-007-T139 spike measured a real, deterministic, fully probe-verified entry-connected frontier
of **18** distinct exits on the authorized route. ADR 0014 raises the bound to **`64U`** as a fixed
**representation-safety** bound, justified from the partial-program working-set model rather than by
tuning until Sonic passes:

- **Related structural quantity.** A retained prefix has at most `m68k_discovery_max_blocks` (`192U`)
  blocks; every open-edge boundary exit is a distinct not-yet-retained control-flow successor of a
  retained block, so the *distinct-target* frontier is loosely related to (and in a well-formed
  entry-connected prefix well below) the block count. `64U` is **one third of `m68k_discovery_max_
  blocks`**, rounded to a power of two — a deliberate structural fraction, not a tight bound.
- **Headroom over evidence.** `64U` is ~3.5× the deterministically measured worst case (`18`) on the
  authorized route, a deliberate margin, not a fit.
- **It is a safety bound, not a completeness parameter.** Exceeding `64` open exits is a deterministic
  fail-closed whole-program `FrontendRejected`, never a truncated or unsound partial. SEG-007-T140
  **must** carry a synthetic `bound + 1` (`65`-exit) discovery fixture that asserts this fail-closed
  path, alongside the positive fixtures.
- **No doubling loop.** If a future capability advance ever produces a genuinely wider entry-connected
  frontier, the response is a fresh explicit representation-model decision (does the generated stop
  dispatcher stay linear-scan or become a jump table; is a wide fragmented frontier still the right
  cut), **not** a reflexive re-doubling of this constant. ADR 0014 fixes `64U` and forbids retuning it
  by route signature.

A whole-program rejection under any Decision §3 clause remains a **complete, bounded, acceptable
terminal outcome**, exactly as ADR 0013's "Binding terminal outcomes and acceptance" establishes for
a Phase A probe failure. SEG-007-T140 records it as the truthful execution-selected frontier and
makes the appropriate bounded successor or `needs_full_refinement` handoff; it must not be "resolved"
by weakening a fail-closed rule, fabricating provenance, or raising `m68k_discovery_max_instructions`,
`m68k_discovery_max_blocks`, or `m68k_discovery_max_frontier_exits` beyond the `64U` this ADR fixes.

### 4. Retained-prefix invariant (replaces ADR 0013 §5's P1-P5)

The implementing task asserts the following directly in project-authored tests on every promoted
partial program. P1-P2 are the mechanically checkable entry-rooted-connectivity guarantee the
acceptance requires; they are enforced by the breadth-first reachability walk in both
`runtime_frontier_eligible` mirrors — **extended by Decision §1b's call-to-continuation reachability
relation** — and are made **structural** (not merely enforced) by Decision §1 + §1b together.

- **P1 (retained-block connectivity).** In the retained static graph, every retained block entry is
  reachable from the seed entry through retained edges only — retained-block-to-retained-block edges,
  retained-block-to-boundary edges, **and the Decision M1b call-to-continuation reachability relation
  for every retained frame**. Equivalently: the reachability walk at
  `src/machine/genesis/frontend.cpp` and its `src/codegen/c11/frontend.cpp` mirror reach every
  retained block entry. This is the property pure breadth-first or pure depth-first admission does
  **not** give on its own (Decision §1b); it is made structural by M1 + M1b together.
- **P2 (boundary connectivity).** Every represented `discovery_prefix_boundary` address is the target
  of a retained block's outgoing edge and is reachable from the seed entry through retained edges.
- **P3 (no incomplete semantic state).** `prefix.decoded.size() == prefix.ir.size()`, each pair
  satisfies `independently_decoded_and_lifted`, and every retained instruction was admitted through
  the full per-kind operand read/write validation switch. A probed boundary instruction is never
  present in `prefix.decoded`, never lifted into `prefix.ir`, and never emitted — even when it was
  admitted into a block that the retained-prefix construction then discards as incomplete (the
  `exclude_frontier_instruction` break in `build_analysis` and block-completeness pruning enforce
  this).
- **P4 (no dangling edge).** Every outgoing edge of every retained block targets a retained block
  entry or a represented frontier / boundary address.
- **P5 (clean termination).** Every retained block ends in a transfer instruction or carries a
  `fallthrough` edge to a retained block entry or to a represented boundary address.
- **P6 (exit representation and bound).** The set of represented exit source addresses equals the
  candidate-frontier-address set `build_analysis` treated as safe edge targets, and its size is
  `<= m68k_discovery_max_frontier_exits` (`64U`); if the open-edge frontier exceeds that bound the
  whole promotion fails closed (Decision §3). SEG-007-T140's test suite asserts both a positive
  wide-frontier case and the `bound + 1` fail-closed case.

### 5. Emitted representation and runtime dispatch — unchanged

This ADR adds no `GenesisFrontierClass` value, no `M68kIrOperation` / `M68kIrKind` /
`DirectFlowIrKind` value, no runtime enumerator, no ABI struct field, and **no new constant** (it
raises the *value* of the existing `m68k_discovery_max_frontier_exits`, Decision §3). Every
`discovery_prefix_boundary` exit in the bounded set flows through the ADR 0013 §6 representation
already in production: `classify_frontier`, both `runtime_frontier_eligible` mirrors, both
`c4_stop_class` mirrors, `build_genesis_frontier_stop_function`, the `genesis_dispatch` frontier arm,
`GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY = 9` with its name and stop-pair arms, and the driver
`STOP_DIAGNOSTIC_PAIRS` entry. `genesis_dispatch`'s frontier arm already emits one `runtime->pc`
comparison per represented frontier exit, so a bounded set of boundary exits needs no dispatcher
change. Runtime dispatch and re-entry select **only among already-emitted function identities**; a
boundary body is an unconditional fail-closed stop that touches no target byte; no runtime opcode
fetch, decode, or target-address computation is added anywhere. Phase B (ADR 0013 §7) performs every
decode at build time only.

### 6. Preserved provisions (explicit)

This ADR explicitly preserves, unchanged:

- the fixed per-walk resource ceiling **value** `m68k_discovery_max_instructions = 256U` — not
  widened, searched, retuned, or replaced; Decision §1 changes only *which* addresses reach its
  existing increment site, and per ADR 0011 §4.1 each seed is still walked to fixpoint or ceiling
  independently at that same per-walk value;
- the **single static producer** of `GenesisFrontierClass::discovery_prefix_boundary` — the
  instruction admission ceiling handler in `M68kStaticGraphWalker` (ADR 0013 §4). It now emits one
  primary and up to `m68k_discovery_max_frontier_exits - 1` (`63`) secondary boundary exits per walk
  instead of exactly one exit, and no other code path produces the class;
- the **empty-`unresolved_reason` + `has_target == false` discriminator** (ADR 0013 §3): every
  boundary issue in the bounded set leaves `unresolved_reason` empty and sets no `target`; every
  other budget producer keeps its non-empty reason and remains a whole-program rejection;
- **ADR 0011 Decisions §§1-3** (address-only iterative worklist, runtime-stack-owned return
  membership over the whole-program call-continuation set, the finite-state termination model) and
  **ADR 0011 §5** (`runtime_confirmed` vs `static_only` reachability-provenance tagging; a
  `static_only` address may never become a seed);
- **deterministic byte-identical generated C** for identical authorized input and options
  (Decision §1's determinism clause; the boundary set is enumerated and ordered deterministically;
  exit ordering continues to use the existing `frontier_sort_key`);
- the invariant that **runtime dispatch selects only already-emitted identities and never fetches or
  decodes a target instruction** (Decision §5);
- ADR 0013 §7's Phase A / Phase B separation, its one-way dependency, its fixed
  `m68k_discovery_max_seed_entries` / `m68k_expansion_max_rounds` constants, and its
  `expansion_no_progress` / `expansion_seed_limit_reached` terminal outcomes.

### 7. Amended provisions (explicit)

- **ADR 0013 §2.** The probe is run once per boundary address in the Decision §2 bounded set rather
  than once per ceiling trip. "The retained prefix is byte-identical to the prefix the ceiling
  truncates today" is superseded: the admitted set is the entry-connected breadth-first
  256-instruction prefix, deterministic and byte-identical *across runs* for identical input and
  options, but not identical to the pre-ADR-0014 depth-first prefix (and for the pinned route ADR
  0013's mechanism emits no prefix, so there is nothing to be identical to). The probe's decode
  profile, side-effect-freedom, offset repair, and no-fabrication rules are unchanged.
- **ADR 0013 §4.** "Exactly one boundary address" becomes "one primary boundary address plus a
  bounded set of secondary boundary addresses, at most `m68k_discovery_max_frontier_exits` in total,
  all from the one producer." The single-producer rule itself is preserved.
- **ADR 0013 §5.** P1-P5 are replaced by Decision §4's P1-P6. The "ceiling reached mid-block"
  single-referring-block treatment is generalized to every open control-flow edge at the
  admitted-region frontier. "A boundary address can never later be admitted within the same round" is
  superseded by "a boundary address may have been admitted into an incomplete block; the
  retained-prefix construction structurally excludes it from `prefix.decoded` / `prefix.ir` and from
  emission" (P3). ADR 0013 §7's Phase A positive-case phrasing "prefix satisfies P1-P5" reads as
  "P1-P6" after this replacement; §7's mechanism, dependency direction, constants, and terminal
  outcomes are otherwise preserved verbatim.
- **ADR 0013 §8.** Its "Deterministic output" guarantee is preserved in the across-runs,
  identical-input-and-options sense; its sub-clause asserting the admitted prefix is byte-identical
  to today's ceiling-truncated prefix is superseded exactly as ADR 0013 §2's identical sentence is
  (the admitted set is now the entry-connected breadth-first prefix). Every other ADR 0013 §8
  invariant-preservation guarantee is unchanged.
- **ADR 0010 §3.** The deterministic partial-prefix emission on ceiling exhaustion is refined: the
  emitted prefix is the entry-connected admitted region, and the boundary is the bounded probe-
  verified open-edge set rather than the single ceiling-trip instruction. The choice that ceiling
  exhaustion routes to ADR 0002's fail-closed-trap arm (when a runnable prefix exists) or its
  reject arm (when the frontier is unrepresentable or over the cap) is unchanged.
- **ADR 0011 §2.** Not amended: the container-drain order is selected (first-in-first-out) within the
  choice ADR 0011 §2 already delegates. Recorded here for traceability only.
- **`m68k_discovery_max_frontier_exits` (SEG-007-T064).** Value raised `4U → 64U` and re-characterized
  from a synthetic-fixture-sized capacity to a fixed partial-program **representation-safety** bound
  justified from the working-set model (Decision §3). Its role — bounding the `UnresolvedFrontier`
  count of a promoted `FrontendPartialProgram` and, on overflow, failing the whole promotion
  closed — is unchanged. It remains outside the discovery-resource-ceiling family and is not sized by
  the doubling-search method.
- **Partial-program reachability model (Decision §1b).** `runtime_frontier_eligible`'s retained-graph
  reachability walk and `build_analysis`'s pruning gain a call-source-to-continuation-target
  reachability relation for every retained `M68kStaticFrame`, so a retained call continuation is
  reachable exactly when its call site is, independent of whether the callee's `RTS` was admitted.
  This adds no edge kind, no admitted instruction, and no runtime behavior; it is the smallest change
  that makes P1 hold for a bounded breadth-first prefix. ADR 0011 Decision §1 already establishes the
  same "a call reaches its continuation without discovering a matching RTS" principle for admission
  scheduling.

## Consequences and Next-Task Shape

SEG-007-T140 is the implementing successor and consumes this section verbatim when it starts. The
task-PR gate forbids editing an existing sibling record from a decision pull request, so this pull
request edits only the SEG-007-T139 record and adds this ADR; SEG-007-T140's record, the backlog policy document,
no capability-map row, and no other sibling record are touched.

### Feasibility evidence (SEG-007-T139 ephemeral spikes, reverted before publish)

Two ephemeral local feasibility spikes exercised the selected mechanism against the authorized pinned
route (`m68k_discovery_max_instructions = 256` unchanged, no Phase B). **All spike production changes
were reverted before this pull request was published**; the diff is the two documentation files only.
Results are normalized and non-reconstructable, deterministic across repeated runs.

**Spike 1 — FIFO drain alone, `m68k_discovery_max_frontier_exits` still `4`.**

- `entry_connected = true` — the first-in-first-out-admitted 256-instruction stream is entry-rooted by
  construction (M1).
- The full post-ceiling worklist is **18** distinct open-edge frontier addresses (1 primary + 17
  secondary), and **every one of the 18 passes its ADR 0013 §2 provenance probe**.
- Promotion still fails: (a) `18 > 4`, and independently (b) `runtime_frontier_eligible`'s
  retained-*block-graph* reachability check rejects the primary — **80 of 111 retained blocks are
  unreachable** in that graph, dominated by `JSR`/`BSR` continuations whose `return_to_continuation`
  edge is absent because the callee's `RTS` was not admitted within the ceiling. This is the finding
  that produced Decision §1b: a drain-order change alone is not sufficient.

**Spike 2 — FIFO drain + Decision §1b call-to-continuation reachability + `m68k_discovery_max_frontier_exits = 64`.**

- `discover_m68k_general_startup` returns a **`FrontendPartialProgram`** — `promoted = true`,
  `frontiers = 18`, `retained_entries = 111`, `unreached_entries = 0`. The pinned route is
  **representable** under the selected design: an entry-connected 256-instruction prefix with 18
  probe-verified `discovery_prefix_boundary` exits, well within the `64` bound, `retained_addresses ==
  sibling_addresses`.
- **C11 emission then stops** on `preflight_m68k_general_startup_c4`: **7 retained-prefix preflight
  gap rows** (families `add`, `clr`, and five further IR-kind rows; a mix of
  `requires_architecture_decision` and `missing_dispatcher` gaps). These are unsupported
  instruction/operand *lowering* forms inside the accepted 256-instruction prefix — a **CPU
  decode/lift/IR/C11 semantics frontier**, the same shape as every prior "behind the budget sits a
  further CPU-instruction frontier" row in `docs/development/sonic-title-critical-capabilities.md`.
  They are **not** a property of the discovery/boundary mechanism and do **not** re-open this
  decision. The generated artifact could not be run because emission did not complete.

**What the evidence establishes.** The revised mechanism (M1 FIFO + M1b block-graph connectivity +
`64` representation bound) **resolves the motivating architectural problem** on the authorized pinned
route: entry-connected admission succeeds, block-graph reachability succeeds with zero orphaned
retained blocks, the real 18-exit frontier is completely probe-verified and representable within the
fixed `64`-exit bound, `partial_or_rejection` produces a `FrontendPartialProgram`, and P1-P6 are
structurally satisfiable. **This ADR is `Accepted`.** Decision §3's whole-program rejection remains a
real terminal outcome for genuinely over-`64` or probe-failing frontiers, but it is **not** the
outcome on the current route under this architecture.

The only remaining pinned-route failure is a **separate, pre-existing C4 CPU/IR lowering frontier**:
the accepted 256-instruction prefix contains 7 lowering-gap instruction/operand forms, so
`emit_m68k_general_startup_runtime_c` currently returns
`/* translation rejected: C4 retained-prefix preflight gaps */`. That family is a CPU
decode/lift/IR/C11 capability owner. It is retained here as **lookahead evidence**, not as a gate on
this ADR's status: architecture acceptance belongs to SEG-007-T139, and completing an orthogonal
emitted-prefix CPU-lowering capability does not own ADR 0014's decision.

**Lookahead handoff for the C4 lowering frontier.**

- SEG-007-T140 implements the Accepted ADR 0014 mechanism and re-demonstrates the representable
  entry-rooted partial prefix (from-scratch: `FrontendPartialProgram` promotes, P1-P6 hold, frontier
  `<= 64`, every probe succeeds).
- If C11 emission still stops on the independently identified retained-prefix lowering family,
  SEG-007-T140 records that truthful CPU/C4 frontier and creates the **smallest bounded successor**
  for it — a CPU decode/lift/IR/C11 task — or uses `needs_full_refinement` **only if** classification
  proves that family is genuinely architectural. SEG-007-T140 does **not** absorb the 7 CPU/C4 gaps
  into the discovery-boundary task merely to make an emit demonstration pass.
- Once those gaps are implemented, the route is re-executed normally. If generated-native execution
  reaches `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY`, ADR 0013 §7 Phase B activates exactly as already
  decided (`runtime_confirmed`-only seeds, `256U` per-seed ceiling, accepted seed/round limits).
- If a from-scratch re-run ever shows the pinned route is **not** representable under the committed
  implementation — not promotable, frontier genuinely `> 64`, or a probe fails — SEG-007-T140
  **stops** with `needs_full_refinement` and does not force a pass by weakening a fail-closed rule or
  raising a bounded limit. (Spike 2 shows this is not expected.)

**Binding scope for SEG-007-T140.**

1. Implement Decision §1 (entry-connected / breadth-first admission ordering; best-effort sibling
   exploration made non-admitting; deterministic drain order documented) in
   `src/cpu/m68k/static_discovery.cpp`, **and Decision §1b** (the call-to-continuation reachability
   relation for every retained frame in both `runtime_frontier_eligible` mirrors and in
   `build_analysis`'s pruning, `src/machine/genesis/frontend.cpp` + `src/codegen/c11/frontend.cpp`).
   No change to decode-once caching, the discovery-resource ceiling values, ADR 0011 §§1-3's
   call/return ownership, or ADR 0009's indirect-candidate walking.
2. Implement Decision §2 (the producer enumerates the bounded boundary address set from its own
   traversal state; one ADR 0013 §2 probe per boundary address; primary plus secondary
   `discovery_budget_exhausted` issues with empty `unresolved_reason`) and the
   `compute_candidate_frontier_addresses` extension in `src/machine/genesis/frontend.cpp`.
3. Implement Decision §3, including **raising `m68k_discovery_max_frontier_exits` `4U → 64U`** with the
   representation-safety-bound doc comment from Decision §3 (it is not folded into the
   doubling-search-method comment block for the discovery-resource ceilings), the over-`64` /
   probe-failure / represented-exit-mismatch fail-closed clauses, and assert Decision §4's P1-P6
   directly in project-authored, legally redistributable synthetic tests:
   - a positive case where the admission counter trips several call frames deep and a runnable
     entry-connected boundary set is emitted with every retained block (including every `JSR`/`BSR`
     continuation whose callee `RTS` is *not* admitted) and every boundary address reachable from the
     reset entry;
   - a deterministic byte-identical repeat and a strict-C11 compilation of representative generated
     output;
   - the **`bound + 1` (65-exit) fixture** asserting the over-`64` whole-program fail-closed
     rejection;
   - adversarial-negative cases (a probe decode failure falls back to whole-program rejection; a
     forged boundary carrying `direct.has_target` or a mismatched mapping-claim / bus record is
     refused by `runtime_frontier_eligible`; a non-empty-`unresolved_reason` budget stop is not
     classified as a boundary; a retained continuation whose call site is *not* reachable is still
     rejected).
4. Re-demonstrate representability from scratch (`FrontendPartialProgram` promotes, P1-P6 hold,
   frontier `<= 64`, every probe succeeds) under the committed implementation, then re-execute the
   authorized pinned Sonic route (`tools/genesis_startup_bridge.py --mode commercial
   --diagnose-frontier`, at least two byte-identical runs) and record the truthful normalized pipeline
   stage / category. Per the SEG-007-T139 feasibility evidence the expected result is a
   **representable `FrontendPartialProgram`** whose C11 emission then stops at the **C4 retained-prefix
   lowering frontier** (unsupported instruction/operand lowering forms in the accepted prefix —
   `add` / `clr` and further IR kinds). SEG-007-T140 records that as the truthful execution-selected
   frontier and does **not** treat it as a task failure, weaken a fail-closed rule, or raise a bounded
   limit. If representability itself fails, SEG-007-T140 stops with `needs_full_refinement`.
5. Implement ADR 0013 §7 Phase B in the same task **only if** a real generated-native run stops with
   `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY`; otherwise record the run's real stop class as the next
   execution-selected frontier and do not claim expansion. Per the feasibility evidence a boundary
   stop is **not** expected until the C4 lowering gaps (item 6) are implemented and the route
   re-executes past emission; if that later re-execution reaches
   `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY`, Phase B activates exactly as already decided. The
   per-seed ceiling stays `256U`; the accepted `m68k_discovery_max_seed_entries` /
   `m68k_expansion_max_rounds` limits and the `expansion_no_progress` / `expansion_seed_limit_reached`
   terminal outcomes apply unchanged.
6. Reconcile `docs/development/sonic-title-critical-capabilities.md` in SEG-007-T140's own pull
   request from the truthful executed result, and create the **smallest bounded continuation
   successor** that owns the C4 retained-prefix lowering frontier as a CPU decode/lift/IR/C11 task
   (or record `needs_full_refinement` **only if** that family's classification proves genuinely
   architectural). SEG-007-T140 must not absorb the 7 CPU/C4 gaps into the discovery-boundary task.
   Independent adversarial validation happens inside SEG-007-T140's pull request.

**Binding non-goals for SEG-007-T140.** Amending this ADR or ADR 0013 or reopening ADR 0011
Decisions §§1-3; widening, searching, retuning, or replacing the value of
`m68k_discovery_max_instructions` or `m68k_discovery_max_blocks`; raising
`m68k_discovery_max_frontier_exits` beyond the `64U` this ADR fixes, or retuning it by route
signature; adding a new discovery-budget subsystem or resource ceiling; promoting a `static_only`
address to a seed; an interpreter, JIT, or runtime opcode fetch / decode; any Sonic-specific address,
seed, code map, or heuristic; any device / VDP / Z80 / PSG / controller-I/O / rendering / timing work;
the ADR 0009 indirect mechanism itself; the ADR 0007 watchdog; the ADR 0006 cartridge-data path; the
non-`runtime_routing` C11 lowering profile; and **implementing any of the C4 retained-prefix lowering
gaps** — those belong to a CPU decode/lift/IR/C11 successor, not to SEG-007-T140.

**Binding terminal outcomes for SEG-007-T140.** Each of the following is a complete, bounded,
acceptable result and none is a task failure: a representable `FrontendPartialProgram` whose C11
emission stops at the C4 retained-prefix lowering frontier, with the smallest bounded CPU
decode/lift/IR/C11 successor created for it (**the expected outcome on the current authorized route**
per the SEG-007-T139 feasibility evidence); a Decision §3 whole-program rejection for a genuinely
over-`64` or probe-failing frontier; a real generated-native run that stops at a non-boundary class;
`expansion_no_progress`; `expansion_seed_limit_reached`; a `needs_full_refinement` handoff if
representability itself fails or if the C4 family's classification proves genuinely architectural.
None authorizes a capability-status change beyond the truthful executed result, weakening a
fail-closed rule, fabricating evidence, or raising a bounded limit. ADR 0014's `Accepted` status is
**not** contingent on any of these — architecture acceptance is SEG-007-T139's, and the orthogonal C4
lowering capability is a downstream successor.

## Relationship to existing ADRs

- **ADR 0002.** Unchanged. A partial program whose exits include one or more `discovery_prefix_boundary`
  markers is a non-success exit that makes no completeness claim past any boundary. This ADR only
  makes ADR 0010 §3's fail-closed-trap arm reachable for a boundary many call frames deep.
- **ADR 0009.** Fully reused, not altered. Its indirect-candidate walk is one source of admitted-block
  outgoing edges whose targets may enter the Decision §2 boundary set; its membership-gated dispatch
  is unchanged.
- **ADR 0010.** §1, §2, and §4 preserved; §3's partial-emission mechanism refined (Decision §7).
- **ADR 0011.** Decisions §§1-3 and §5 preserved in full; §2's delegated container choice is
  exercised, not amended. Decision §1b applies ADR 0011 Decision §1's "a call reaches its continuation
  independently of discovering a matching RTS" principle — established there for admission
  scheduling — to the retained-graph reachability check as well; this is a consistent extension, not
  an amendment.
- **ADR 0012.** No interaction.
- **ADR 0013.** §2, §4, and §5 amended as enumerated in Decision §7; §8's byte-identical-to-today
  sub-clause superseded there in the same way as §2's identical sentence; §3, §6, §7, and the rest of
  §8 preserved.
