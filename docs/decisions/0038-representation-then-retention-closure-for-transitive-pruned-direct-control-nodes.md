# ADR-0038: Rooted representation-then-retention closure for transitive pruned direct control

- Status: Accepted
- Date: 2026-09-15
- Deciders: SEG-007-T233
- Extends: ADR-0028 §9's existing representation vocabulary and ADR-0031's
  transactional rebuild precedent; neither existing authority is weakened.
- Related: SEG-007-T225, SEG-007-T226, SEG-007-T228, SEG-007-T229,
  SEG-007-T230, and SEG-007-T232.

## Context and authoritative graph

T232's 463-node population included outgoing destinations which were not erased
block entries. T233's erased-entry definition is authoritative because it is
tied to the completed-prefix transaction immediately feeding ADR-0028 §9:

1. a node is an entry erased by that transaction which participates as source
   or erased-entry destination of an unsafe outgoing relation;
2. an edge is such a relation with both endpoints erased in that transaction;
3. roots/leaves are computed only in that induced graph; and
4. address, decoded, leader, or boundary membership is not representation.

Current instrumentation reproduced 349 nodes, 243 edges, 165 roots, 116 leaves,
maximum depth 7, and no cycle. This reconciles the old populations: 463 was an
endpoint-expanded diagnostic graph; 349 is the block-retention graph §9 can
transactionally rebuild.

### Existing representation ownership of leaves

Each of the 116 leaves was independently classified against representations
which exist before this decision. All are `unresolved / no truthful owner`:

| Causal prune category | Ordinary executable | Destination-global typed frontier | Relation-specific typed | Unresolved |
|---|---:|---:|---:|---:|
| Reachability orphan | 0 | 0 | 0 | 12 |
| Orphan RTS | 0 | 0 | 0 | 65 |
| Unresolved computed control | 0 | 0 | 0 | 0 |
| Unsafe edge outside the induced graph | 0 | 0 | 0 | 39 |
| **Total** | **0** | **0** | **0** | **116** |

An existing outer frontier is not automatically the truthful owner of an
interior leaf. The leaf acquires a typed owner only after the rooted transaction
below establishes a live retained predecessor and final eligibility validates
the destination-global record.

## Decision

**Select Candidate B: rooted representation closure, then retention closure.**
The initial all-at-once transaction was insufficient evidence because two
unreachable retained islands poisoned the whole-prefix eligibility gate. The
correct mechanism peels those islands and every dependency retained only by
them before deciding whether any subset can commit.

### Per-node states

Every graph node is in exactly one state at commit:

1. ordinary retained executable block;
2. semantically pruned with a validated destination-global typed frontier;
3. relation-specific typed representation, only under an existing relation
   contract; or
4. unresolved with no truthful representation.

The executed fixed point ended at 243 ordinary retained blocks, 90
destination-global `known_but_unemitted_target` frontiers, zero relation-specific
representations, and 16 unresolved nodes. The totals sum to 349. Unresolved
nodes are outside the committed rooted dependency closure and authorize neither
retention nor dispatch.

### Deterministic rooted transaction

1. Prove the finite graph acyclic. A cycle fails the transaction closed before
   changing representation or retention.
2. Classify nodes in deterministic reverse-topological order. A provisional
   representation is a full typed-frontier proposal with decoded provenance,
   never bare membership.
3. Rebuild through the existing completed-prefix predicate with provisional
   facts supplied at every seam which already accepts typed representation:
   outgoing-edge safety, live-return continuation safety, and final return-edge
   filtering. Keep proposals separate from `candidate_frontier_addresses`,
   because that set also suppresses block construction.
4. Compute rooted reachability using the same roots, retained edges, frames,
   and block traversal as `runtime_frontier_eligible`.
5. Atomically mark every unreachable retained component forbidden. Rebuild from
   scratch; ordinary pruning then removes all retained ancestors which depended
   on that component. Remove every provisional fact owned by the peeled
   component. This is a deterministic component/dependency closure, never an
   arbitrary node subset.
6. For the rooted remainder, retain proposals only for non-retained graph
   destinations of rooted retained sources. Rebuild until both retained and
   proposed sets are unchanged. Each productive round strictly grows the
   forbidden set or changes the finite proposal set; an explicit ceiling fails
   closed rather than committing a partial transaction.
7. Validate every final proposal through unmodified
   `runtime_frontier_eligible`. Commit the prefix and all destination-global
   frontiers together only when every retained entry is rooted, every proposal
   validates, and all existing graph/frame/Tier-1 invariants pass. Otherwise
   restore the original prefix and frontier vector.

The prototype converged in three rounds. Round 1 found 7,156 retained blocks,
of which 7,154 were rooted, and atomically peeled two. Round 2 rebuilt 7,154
rooted blocks and reduced the proposal set to 90. Round 3 reproduced the same
retention and proposal sets. All 90 final typed frontiers passed the unmodified
eligibility gate, so the rooted subset was committable. Compared with baseline,
the transaction retained 443 additional blocks across the prefix.

### Candidate disposition

- **A rejected:** representation without retention cannot create the retained
  paths required by final frontier eligibility.
- **B selected and proved:** the dependency-closed rooted fixed point commits a
  non-empty coherent subset after peeling all unreachable components.
- **C rejected:** no parallel pruned-subgraph runtime object is needed; B uses
  existing blocks, frontiers, provenance, roots, edges, and frames.
- **D rejected:** a rooted Candidate-B subset exists and passes final static
  representation eligibility, so global fail-closed non-recovery is too strong.

## Preserved invariants

- T225 bare boundary/leader/decoded membership is never representation.
- T226/T230 Tier-1 target sets remain all-members atomic; no set is widened or
  subsetted.
- T228/T229 frontiers are destination-global and committed atomically.
- Return edges and frames retain their exact call identity; peeling is applied
  before final frame/edge validation.
- ADR-0028 §9 remains the exact-target authority; this closure adds reach, not
  a second target authority.
- No block is fabricated, no discovery ceiling is raised, and no target is
  runtime-learned.
- No interpreter, JIT, runtime opcode decoder, second dispatcher, hint change,
  or title-specific rule is introduced.
- Sorted graph traversal and atomic commit make output deterministic. Cycles,
  ceiling exhaustion, stale Tier-1 facts, or any final eligibility failure roll
  back the whole transaction.

## Required implementation fixtures

SEG-007-T234 or its refined replacement must use project-authored fixtures for:

1. each leaf-owner/prune-category row above;
2. a two-level chain which commits after its destination becomes representable;
3. an unresolved sibling which keeps its dependent parent uncommitted;
4. multiple predecessors observing one destination-global frontier;
5. an unreachable retained island whose entire dependent closure is peeled,
   alongside a rooted component which still commits;
6. no rooted surviving component, requiring complete rollback;
7. a genuine cycle and ceiling exhaustion, both failing closed atomically;
8. return/frame identity and Tier-1 all-members atomicity regressions; and
9. byte-identical output across two runs.

## Consequences

Candidate B is architecture-proved at static representation/retention. The
freshly rebuilt canonical evidence run is preserved under ignored agent-run
label `seg007-t233-rooted-closure-evidence`; its normalized summary records the
349/243/165/116/depth-7/acyclic graph, three rounds, two peeled entries,
243 retained graph blocks, 90 typed frontiers, 16 unresolved nodes,
`dangling_dependency_count=0`,
`retained_ancestor_depending_on_removed_fact_count=0`, and `valid=1`.
The static prototype exposed a different build-time ownership frontier;
emission/runtime advancement was not measured (`compile_attempts=0`). Current
production remains at its unchanged runtime-selected blocker. The existing
T234 plan lacks component peeling and must be refined before implementation.
