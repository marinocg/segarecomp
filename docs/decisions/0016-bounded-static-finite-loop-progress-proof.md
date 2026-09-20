# ADR 0016: Bounded Static Finite-Loop Progress Proof

- Status: Accepted
- Date: 2026-09-01
- Relates to: ADR 0007 (generated-runtime loop-progress watchdog) and ADR 0009
  (bounded retained static-analysis facts consumed by guarded C11 lowering).

## Context

ADR 0007 permits an instruction-owned finite-progress note for DBcc-like
semantics but leaves a multi-instruction conditional backedge at its watchdog.
An emitter-local ADD/CMP/branch recognizer is unsound: C emission does not own
the decoded CPU effects, CFG completeness, or a retained proof of the runtime
register relation.  It must not create a second data-flow model from selected
emission operations.

ADR 0009 establishes the applicable precedent: bounded static analysis reaches a
fixed point under hard resource caps, retains a typed provenance-keyed fact, and
C11 evaluates live architectural state only after validating that fact.  Unknown
or unrepresentable analysis states fail closed rather than becoming partial
proofs.  That precedent authorizes a restricted finite-loop proof; it does not
authorize a broad general path-sensitive abstract interpreter.

## Decision

Adopt a bounded static finite-loop proof family.  CPU/static analysis owns a
typed `M68kFiniteLoopProgressProof`, keyed by loop identity and conditional
backedge provenance.  The fact contains:

- loop and backedge identity;
- the induction address or data register;
- the fixed nonzero update and its arithmetic width/domain;
- the invariant architectural bound register;
- comparison operand order and branch condition/polarity; and
- the finite covered loop blocks and edges.

This decision explicitly authorizes T150 to extend the CPU-owned
`M68kOperationEffect` contract.  For every operation admitted to this proof,
the effect must provide a conservative, complete architectural D/A
register-write footprint as masks or finite sets.  The footprint includes
explicit `Dn`/`An` destinations, decoded effective-address postincrement and
predecrement updates, and `A7` or any other implicit architectural register
write.  An unknown or incomplete footprint rejects the operation and therefore
the proof.  `affects_condition_codes` remains the CPU-owned CCR fact.  The
analyzer consumes these CPU facts; it must not create analyzer-local second
semantics for register or CCR effects.

The producer uses a bounded fixed-point walk of the already decoded static CFG.
It has explicit hard caps on loop candidates, covered blocks and edges,
fixed-point states, joins, and proof work.  Cap exhaustion, an unknown state, or
an incomplete result produces no fact and no progress note.

An admitted proof is a closed, bounded, reducible restricted loop with exactly
one conditional backedge and one induction register.  The induction register
has exactly one statically fixed positive update on every iteration, including
decoded CPU-semantics implicit effective-address updates.  The initial admitted
family is only generic unsigned, monotonically increasing `Dn`/`An` induction
toward an invariant `Dn`/`An` upper bound.  Neither register may have a direct
or implicit write in the covered loop other than that one admitted induction
update; a load or reload that writes a register and any unknown implicit effect
are register writes and reject the proof.  There may be no intervening CCR
writer between the admitted comparison and conditional branch.

At the conditional-backedge source ID for a fixed loop identity, let `I` be the
live induction value, `B` the invariant upper-bound value, and `S` the positive
fixed update.  The guarded note is
`floor((B - I) / S) + 1`, calculated with widened arithmetic.  Exact guards
must establish the admitted unsigned comparison and taken-backedge relation,
`I <= B`, representability of `B - I` in the widened type, positive `S`, that
the update cannot wrap before exit (`I <= max - S`, or an equivalent
current-step/next-step guard), and `quotient + 1 <= UINT32_MAX`.  Guard failure
emits no note.  Signed, decreasing or negative-step, lower-bound, mixed-width,
or otherwise ambiguous cases all fail with no proof.  Multiple updates,
nonconvergence, ambiguous CFG, unresolved or indirect flow, calls, unsupported
operations, unknown effects, or any other ambiguity likewise reject the proof.
This restricted register-effect rule does not require general memory-alias
analysis merely because the bound is runtime-valued.

The initial engineering caps are stable project constants:
`M68K_LOOP_PROOF_MAX_CANDIDATES = 16`, `M68K_LOOP_PROOF_MAX_BLOCKS = 16`,
`M68K_LOOP_PROOF_MAX_EDGES = 32`, `M68K_LOOP_PROOF_MAX_INSTRUCTIONS = 32`,
`M68K_LOOP_PROOF_MAX_WORKLIST_STATES = 64`, `M68K_LOOP_PROOF_MAX_JOINS = 64`,
and `M68K_LOOP_PROOF_MAX_WORK = 512`.  Exhaustion produces no proof.  T150
must not tune these values for Sonic.

C11 lowering consumes only the retained typed proof.  It may evaluate the live
architectural induction and bound registers and calculate a finite
`remaining_bound` only after guards verify the proved runtime relation.  A guard
failure emits no note.  A successful guarded note uses only the existing
opcode-agnostic `genesis_note_loop_backedge` seam; it performs no target fetch,
decode, interpretation, JIT, or second dispatcher.

## SEG-007-T185 amendment: the outer-loop re-entry check is loop-local

The proof rejects a candidate whose branch fall-through exit target lies on a
path back to the loop header (an outer-loop renewal/re-entry that would defeat
ADR-0007's instance lifetime).  This decision depends only on loop-relevant
structure: whether the exit target is backward-reachable from the header.  The
original implementation instead ran a forward reachability flood from the exit
target over the retained graph, bounded by `M68K_LOOP_PROOF_MAX_WORK` /
`M68K_LOOP_PROOF_MAX_WORKLIST_STATES`.  In a small entry-rooted discovery
prefix that flood stayed within bounds, but under a large multi-unit one-shot
static aggregate (ADR-0025/0026/0027/0028) the exit target's undeduplicated
forward frontier exceeds the worklist bound long before it can conclude the
exit never re-enters the header, so an otherwise-admitted finite loop lost its
proof purely because of unrelated downstream program size.

The corrected check computes the set of blocks backward-reachable from the
header by a bounded reverse flood and rejects the candidate iff the exit target
is in that set.  This is semantically identical (a node can reach the header
iff it is backward-reachable from it) but bounded by the loop's own
preheader/renewal fan-in rather than the total downstream graph.  A genuine
renewal path consists entirely of blocks that reach the header, so it is still
fully represented and still rejected; an ordinary preheader is upstream of the
header and never the branch's downstream exit target, so it is still admitted.
If the header's backward-reachable set does not close within the existing
bounds the candidate is conservatively rejected.  No cap is raised and
ADR-0007's watchdog window is unchanged.

## Consequences

- CPU/static analysis, not the emitter, owns loop membership, CPU-effect
  classification (including complete D/A write footprints and CCR facts),
  convergence proof, provenance, deterministic fact ordering, and all resource
  limits.
- C11 codegen validates and consumes `M68kFiniteLoopProgressProof`; it may not
  recognize ADD/CMP/branch instruction triples or reconstruct the proof.
- ADR 0007's runtime/watchdog behavior and DBcc handling remain unchanged.
  Its protections for genuine infinite/nonmonotonic/mutation loops and
  outer-loop re-entry/nonrenewal continue to apply; the static-translation
  invariant remains unchanged.
- The implementation must use legal project-authored synthetic positives and
  reject adversarial shapes before it can re-execute the authorized Phase-B
  route.  No title-specific constants, addresses, code maps, or raw commercial
  content belong in the proof, fixtures, or durable evidence.
