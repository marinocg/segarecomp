# ADR 0021: Bounded Partial Asynchronous Static Hardware Root Discovery

- Status: Accepted
- Date: 2026-09-03
- Relates to: ADR 0013 (discovery-prefix boundary and runtime-confirmed
  expansion, its seed set `S` and `m68k_discovery_max_seed_entries == 4`
  ceiling, and the boundary-probe verification this ADR reuses unchanged),
  ADR 0014 (entry-rooted discovery-prefix boundary for a fragmented ancestor
  — the bounded-partial-prefix mechanism, entry-connected admission (M1),
  block-graph call-to-continuation reachability (M1b), and the P1-P6
  retained-prefix invariants this ADR generalizes to a second static
  discovery root kind), and ADR 0020 (deterministic VBlank / MC68000 IRQ6
  interrupt progression, whose IRQ6 autovector target resolution and RTE/
  dispatch architecture this ADR reuses completely unmodified).
- Does not amend, and does not relitigate: ADR 0020's IRQ/VBlank/exception/
  RTE architecture (this ADR is a discovery/analysis-stage generalization
  only — which static roots are allowed to contribute a bounded,
  entry-connected partial prefix instead of requiring 100%-clean per-root
  walk completion); ADR 0013 §5/§6's emitted representation and runtime
  dispatch, reused completely unmodified; ADR 0014 Decision §5's emitted
  representation and runtime dispatch, reused completely unmodified; any of
  the preserved invariants listed in §3 below (`m68k_discovery_max_
  instructions`, `m68k_discovery_max_blocks`, `m68k_discovery_max_frontier_
  exits`, `m68k_discovery_max_seed_entries`, `m68k_expansion_max_rounds`,
  ADR 0007's `W`/`GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT`).

## 0. Why this decision exists now

SEG-007-T047 (done, PR #269) delivered ADR-0020's complete IRQ6/VBlank/
exception/RTE mechanism with full synthetic, generated-C-compile, and
full-gate validation, but recorded `needs_full_refinement` because the
Sonic IRQ6 handler's own static walk exhausts the per-root
`m68k_discovery_max_instructions` prefix ceiling with an unresolved
`primary_issue`, and the handler entry is only bound into generated `main`
when its walk terminates cleanly within that prefix. The SEG-007
full-refinement PR (`backlog/seg-007-refine`, 2026-09-03) independently
re-inspected `src/machine/genesis/frontend.cpp` (the
`FrontendAnalysis`/`FrontendPartialProgram` builder, roughly lines
1040-1360) and its `src/codegen/c11/frontend.cpp` mirror at that HEAD, plus
ADR-0013 and ADR-0014 in full, and established the following facts, which
this ADR relies on and which the implementer independently re-confirms
against the actual HEAD at implementation time (line numbers below are
approximate and may have drifted):

1. ADR-0014 already generalized the reset-root discovery-prefix-boundary
   mechanism to: entry-connected (breadth-first/FIFO) admission ordering
   (M1); a block-graph call-to-continuation reachability relation so
   retained call continuations are reachable even when the callee's RTS
   wasn't admitted (M1b); a bounded, probe-verified boundary-address
   **set** per ceiling trip (not just one address) that becomes multiple
   `discovery_prefix_boundary` frontier exits, bounded by
   `m68k_discovery_max_frontier_exits = 64U`; and P1-P6 retained-prefix
   invariants enforced by `runtime_frontier_eligible`'s breadth-first
   reachability walk (both `src/machine/genesis/frontend.cpp` and
   `src/codegen/c11/frontend.cpp` mirrors). None of this is
   reset-root-specific in its mechanism, only in how it is currently
   wired.
2. `discover_m68k_static_graph(seed, limits, environment)` — the exact
   per-root walk function — already applies all of #1 generically per
   root/seed; it is already called once per ADR-0013 seed in a loop
   (`seeds`, up to 4 members, `S`) **and** separately, once more, for the
   IRQ6 autovector target (`handler_address`) at roughly lines 1156-1261
   of `src/machine/genesis/frontend.cpp`. The IRQ6 call already produces
   its own `primary_issue`/`secondary_issues` (its own bounded,
   probe-verified boundary set) exactly like any reset-root seed's walk
   would.
3. `runtime_frontier_eligible`'s reachability walk (`src/machine/genesis/
   frontend.cpp` ~lines 481-502, mirrored at `src/codegen/c11/frontend.cpp`
   ~lines 483-485) already seeds its reachability BFS from
   `prefix.irq6_handler_entry` as a second root, in addition to
   `prefix.startup_ingress->entry`, with an existing comment literally
   calling the IRQ6 target "a bounded static hardware discovery root --
   entry-disconnected from the reset control-flow graph but a legitimate
   retained block."
4. C11 emission (`src/codegen/c11/frontend.cpp` ~lines 2136-2142,
   ~3100-3108) already only wires `runtime.irq6_handler_present = 1` when
   the IRQ6 handler's entry block is actually present in the final
   emitted/retained block set (`blocks.contains(...)`) — "the handler
   entry is only dispatchable when it belongs to the safe retained prefix"
   is already the emission-time invariant, automatically, with zero new
   code, as soon as the handler's entry address is genuinely retained.
5. The only thing currently preventing a partial/bounded IRQ6 prefix from
   ever being retained is a single explicit all-or-nothing gate at
   `src/machine/genesis/frontend.cpp` ~lines 1202-1206: `handler_walk_clean
   = !discovery.primary_issue && discovery.secondary_issues.empty()`; when
   false, the entire IRQ6 walk's decoded instructions/blocks/edges/frames
   are discarded and **no** merge happens at all — contrast with the
   reset-seed loop just above it at ~lines 1078-1143, which unconditionally
   merges every seed's decoded instructions/blocks/edges/frames into the
   shared `merged_decoded`/`block_entries`/`merged_edges`/`merged_frames`/
   `indirect_by_source` structures, and folds that seed's own
   `primary_issue`/`secondary_issues` into the shared `primary_issue`/
   `aggregated_secondary_issues` aggregation that later becomes the emitted
   `discovery_prefix_boundary` (or other typed) frontier exits via the same
   `compute_candidate_frontier_addresses`/`classify_frontier`/
   `partial_or_rejection` machinery.

## 1. Decision: a typed `StaticProgramRoot` distinction with exactly two kinds

Adopt a generic typed distinction, **`StaticProgramRoot`**, with exactly
two kinds for now:

- **synchronous entry root** — the existing ADR-0013 `S = {reset entry} ∪
  runtime_confirmed_seeds` seed family (`|S| <= m68k_discovery_max_
  seed_entries == 4`, unchanged, untouched by this decision);
- **asynchronous hardware root** — the IRQ6 autovector target (the only
  instance implemented now), resolved at build time exactly as today
  (ADR-0020 §6, unchanged), **not** a member of `S`, never counted against
  `|S|`, never runtime-promoted.

Both kinds share exactly the same bounded-prefix construction that
ADR-0014 already established and that `discover_m68k_static_graph` already
applies per root: entry-connected admission (M1), block-graph
call-to-continuation reachability (M1b), a bounded probe-verified boundary
set becoming typed `discovery_prefix_boundary` (or other legitimate typed)
frontier exits, and the P1-P6 retained-prefix invariants. The only
root-specific metadata is: which bookkeeping list a root's completed walk
feeds (`S`/`seeds` vs. the async-root slot) and whether it is subject to
`S`'s own `m68k_discovery_max_seed_entries` ceiling (synchronous: yes;
asynchronous: no, per ADR-0020 §6, unchanged).

## 2. Required implementation: replace the all-or-nothing gate with a shared per-root merge

**Sole production owner: `src/machine/genesis/frontend.cpp`.** The
reset-seed merge loop, the IRQ6 `handler_walk_clean` gate,
`merged_decoded`/`block_entries`/`merged_edges`/`merged_frames`/
`indirect_by_source`, `primary_issue`/`aggregated_secondary_issues`, and
T144's supersession bookkeeping are all defined and owned exclusively there
today. `src/codegen/c11/frontend.cpp` does **not** duplicate
`handler_walk_clean`, `merged_decoded`, or `aggregated_secondary_issues` —
it only mirrors `runtime_frontier_eligible`'s own shape-validation switch
(as ADR-0013 §6 / ADR-0014 §5 already require of both
translation-unit-local mirrors) and independently re-derives its own
reachability BFS over the single, already-merged
`FrontendAnalysis`/`FrontendPartialProgram` this task's merge produces.
This task must **not** create or refactor a second merge/aggregation owner
in `src/codegen/c11/frontend.cpp`: that file's existing mirrored
`runtime_frontier_eligible`/partial-frontier validation and its existing
emission machinery (including the existing `blocks.contains(...)`
emission-time dispatchability check) are reused unchanged, except for
tests, or the minimal compatibility edits genuinely required by the newly
accepted partial-async-root prefix shape (confirm and fix only if actually
required at implementation time; do not refactor speculatively).

Replace the IRQ6 block's current all-or-nothing `handler_walk_clean` gate
with the exact same unconditional per-root merge the reset-seed loop
already performs — refactor the two near-duplicate merge blocks
(~lines 1078-1143 and ~lines 1191-1259 as they exist today, both in
`src/machine/genesis/frontend.cpp`) into one shared per-root merge routine
(a local lambda or helper is fine; this is a DRY/reuse cleanup the
codebase already wants, not new architecture) applied to:

- every `S` seed (counted toward `seeds.size() > m68k_discovery_max_
  seed_entries`, exactly as today); and
- the IRQ6 root (never counted toward that ceiling, exactly as today's
  `admit_target`/vector-resolution gating already ensures).

The IRQ6 root's own `primary_issue`/`secondary_issues` must flow into the
**same** shared `primary_issue`/`aggregated_secondary_issues` aggregation
the reset seeds already use (becoming `discovery_prefix_boundary` — or, for
a genuine non-boundary capability gap, whatever other typed frontier class
already legitimately classifies it — exit(s)), subject to the **same**
unmodified `m68k_discovery_max_frontier_exits = 64U` bound and the **same**
unmodified P1-P6/`runtime_frontier_eligible` reachability checks, which
already treat `irq6_handler_entry` as a second reachability root (fact 3
above) — with supersession keyed by root identity, not seed identity (§2a).

`analysis.irq6_handler_entry` must be set to the resolved handler address
whenever the vector itself validly resolves (i.e. move it out from behind
the `handler_walk_clean` gate) — subject to §4's probe-failure fail-closed
rule. Dispatchability is then already automatically and correctly gated
purely by whether the handler's entry block ends up actually
retained/emitted (fact 4 above: `blocks.contains(...)` at emission time),
requiring no new gating logic.

Do **not** add a second discovery-prefix-boundary "subsystem," a second
C11/runtime representation, a second `GenesisFrontierClass` value, or any
IRQ6-specific dispatcher/runtime code — every part of ADR-0013 §5/§6 and
ADR-0014 Decision §5 ("emitted representation and runtime dispatch —
unchanged") is reused completely unmodified; this is purely a
discovery/analysis-stage generalization of which roots are allowed to
contribute a partial, entry-connected prefix instead of requiring
100%-clean completion.

## 2a. Generalize T144's supersession bookkeeping from seed identity to static-program-root identity

SEG-007-T144 introduced cross-seed admitted-address supersession:
`aggregated_secondary_issue_seeds` (one `seed_index` per aggregated issue)
and `admitted_instruction_addresses_per_seed` (one admitted-set per seed,
indexed the same way) let one seed's independently admitted code
retroactively supersede another seed's own stale unresolved candidate for
the same address, provided no genuine contradiction (a `decoded_matches`
mismatch, an indirect target-EA candidate-set mismatch, or a
completion-RTS mismatch — any of which already fails the whole aggregate
closed via `aggregation_conflict`, unchanged) is raised.

The IRQ6 root is **deliberately not a member of ADR-0013's seed set `S`**
(Decision §1 above), so it must **not** be inserted into this bookkeeping
as a fake `seed_index` — doing so would conflate two ceilings/ownership
models this ADR and ADR-0013/ADR-0020 keep strictly separate.

**Decision.** Define a root-identity/provenance concept for aggregation
and supersession, generalizing seed identity to root identity —
conceptually:

```
StaticProgramRootId { kind, ordinal_or_identity }
```

with exactly the two `StaticProgramRoot` kinds from Decision §1:

- synchronous entry / Phase-B seed root (`ordinal_or_identity` = the
  seed's existing index into `S`);
- asynchronous hardware root (`ordinal_or_identity` = the IRQ6 root's own
  single fixed identity — there is exactly one instance today).

Replace the seed-keyed bookkeeping with root-keyed bookkeeping —
conceptually:

- `aggregated_secondary_issue_roots` (replaces `aggregated_secondary_
  issue_seeds`, one `StaticProgramRootId` per aggregated issue, for every
  root kind);
- `admitted_instruction_addresses_per_root` (replaces `admitted_
  instruction_addresses_per_seed`, one admitted-address-set per root, for
  every root kind, including the IRQ6 root's own admitted set).

The supersession comparison (`s != issue_seed` in the existing code)
becomes a `StaticProgramRootId` inequality comparison instead of an
integer-index inequality; every other part of the existing supersession
algorithm, ordering, and `aggregation_conflict` contradiction-detection is
unchanged.

**ADR-0013 seed accounting remains completely separate and unchanged.**
`S = {reset entry} ∪ runtime_confirmed_seeds`, `|S| <= m68k_discovery_max_
seed_entries == 4`, `seeds.size()`, and `seed_count` are computed exactly
as today, from exactly the synchronous-entry-root members only. The IRQ6
root receives a `StaticProgramRootId` **only** for merge/fact-provenance
and supersession bookkeeping purposes — it never becomes a member of `S`,
never increments `seed_count`, and never consumes an `S` slot, exactly as
Decision §1 and ADR-0020 §6 already require.

**Preserved T144 semantics across all independent roots (both
directions):**

- a synchronous-root unresolved secondary issue for address *a*, later
  superseded because the asynchronous IRQ6 root's own walk independently
  admits *a* as a retained decoded instruction (no `aggregation_conflict`)
  — the stale unresolved candidate is dropped, exactly as today's
  same-kind cross-seed case;
- an asynchronous-root unresolved secondary issue for address *a*, later
  superseded because some synchronous-entry seed's own walk independently
  admits *a* as a retained decoded instruction (no `aggregation_conflict`)
  — same;
- any genuinely contradictory fact between roots (mismatched decode,
  indirect-target-EA candidate set, or completion-RTS) for the same
  address, regardless of which roots are involved, still fails the
  **entire** aggregate closed via the existing, unmodified
  `aggregation_conflict` mechanism.

## 3. Preserved invariants — do not touch

`m68k_discovery_max_instructions = 256` (per-root, unchanged),
`m68k_discovery_max_blocks`, `m68k_discovery_max_frontier_exits = 64U`,
`m68k_discovery_max_seed_entries = 4`, `m68k_expansion_max_rounds = 4`,
ADR-0007's `W`/`GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT`. No doubling search.
No widening of any ceiling.

## 4. Probe-verification and fail-closed fallback for the async root's own boundary set — pinned

**Re-inspection correction (this exact wording, not an
implementation-stage choice).** Independent re-inspection of the *current*
multi-root issue aggregation in `src/machine/genesis/frontend.cpp` —
specifically the per-root `primary_issue` -> aggregate
`primary_issue`/`aggregated_secondary_issues` merge (~lines 1078-1143),
`compute_candidate_frontier_addresses` (~lines 1408-1415),
`partial_or_rejection`'s secondary-eligibility loop (~lines 1903-1924),
and the T144 multi-seed orphan-secondary tolerance (~lines 1944-1971) —
shows that the earlier draft of this section overclaimed: it is **not
true** that today's ordinary multi-seed aggregation already rejects a
provenance-less probe-failure issue identically regardless of aggregation
order. Trace, precisely:

- A genuine ADR-0013 §2 boundary-probe failure (unmapped/conflicting
  source, truncated, illegal, unsupported form, or an over-length decode)
  produces an `M68kDiscoveryIssue` with **no provenance** — only
  `category` and a non-empty `unresolved_reason`
  (`"m68k_discovery_max_instructions"`) — exactly the pre-ADR-0013
  ceiling-issue shape, structurally identical for every root kind.
- If that provenance-less issue becomes the aggregate `primary_issue` (the
  first root to fail, or the only root), `failure` is set and
  `partial_or_rejection` runs: `classify_frontier` returns `std::nullopt`
  for it (non-empty `unresolved_reason`), so `partial_or_rejection`'s own
  `!frontier_class` check correctly rejects the **whole build**
  (`reject()`). This is the case the earlier draft correctly described.
- If that same provenance-less issue instead becomes an
  `aggregated_secondary_issues` entry (because some *other* root —
  synchronous or asynchronous — already occupied the aggregate
  `primary_issue`), it is **not** treated the same way.
  `compute_candidate_frontier_addresses` only inserts a secondary's
  address when `secondary.first.provenance` is present (~line 1412) — a
  provenance-less secondary is never added to `sibling_addresses` at all.
  `partial_or_rejection`'s own secondary loop then tries
  `classify_frontier` (returns `std::nullopt`, same reason) and, failing
  that, tries `runtime_frontier_eligible(..., known_but_unemitted_target,
  ...)` — but `runtime_frontier_eligible`'s own top-level structural gate
  unconditionally requires `diagnostic.provenance` for **every** class,
  including `known_but_unemitted_target` (ADR-0013 Context §B). A
  provenance-less secondary therefore fails both branches and is
  **silently dropped** from `frontiers` with **no rejection at all** — the
  candidate build succeeds as though that root's failure never happened.
  This is a distinct mechanism from T144's own orphan-secondary tolerance
  (~lines 1944-1971), which only ever concerns a *provenanced* candidate
  that ends up with no retained edge targeting it after pruning; a
  provenance-less issue never reaches that comparison in the first place,
  in either direction.

**This is exactly the gap that matters for the IRQ6 root**, because §2's
shared per-root merge folds the IRQ6 root's own
`primary_issue`/`secondary_issues` into this same aggregate, and the IRQ6
root will frequently **not** be the first root processed (every `S` seed
is merged before it, per §2's ordering). A genuine IRQ6-root
boundary-probe failure landing in `aggregated_secondary_issues` behind an
already-set synchronous-root `primary_issue` would, under the *ordinary*
aggregation path alone, be silently dropped exactly as traced above —
silently promoting a build with an unrepresentable installed IRQ6 vector,
which is precisely the outcome §4's fail-closed requirement forbids.

**Pinned implementation rule (fixed, not an implementation-stage
choice):**

1. **Absent/zero IRQ6 vector.** Existing inert behavior is unchanged: no
   handler, no boundary, no merge, `irq6_handler_present` stays `0`.
2. **Present, validly resolved async hardware root — ordinary shared
   aggregation is the default for every classifiable issue, boundary or
   not.** Every issue the IRQ6 root's own `discover_m68k_static_graph`
   call returns — its `primary_issue` (if any) and every member of its
   `secondary_issues` — participates in the **ordinary** shared per-root
   `primary_issue`/`aggregated_secondary_issues` aggregation exactly like
   any `S` seed's own issues (§2), with root-identity-keyed supersession
   (§2a), **unless it matches item 3's exact fatal-probe-failure
   discriminator below.** This explicitly and equally includes: a
   successfully probe-verified `discovery_prefix_boundary` issue (the
   ceiling tripped and ADR-0013 §2's probe succeeded); **and** a
   legitimately classifiable non-boundary IRQ6 frontier —
   `unsupported_cpu_form`, `unsupported_device_access`,
   `unsupported_memory_region`, `unresolved_indirect_target`, or any other
   class the existing `classify_frontier` switch already recognizes
   (~lines 1844-1863) — which must flow through ordinary aggregation
   exactly like any `S` seed's own such issue. Neither case is a probe
   failure and neither triggers item 3's early rejection; conflating them
   would directly contradict this very item and would silently defeat
   Stage C outcome B (a genuine CPU/C4/device frontier reached inside the
   IRQ6 handler must still be reachable as an ordinary typed stop, not
   manufactured into a whole-build rejection).
3. **Exact fatal-probe-failure discriminator (the only early-rejection
   case).** The special early whole-build rejection defined by this item
   applies **only** to an IRQ6-root issue that is **both**: (a) its
   translated `category == DirectFlowDiagnostic::discovery_budget_
   exhausted` — i.e. it originated from the IRQ6 root's own per-root
   `m68k_discovery_max_instructions` ceiling trip, the sole producer
   ADR-0013 §4 established; **and** (b) it does **not** satisfy ADR-0013
   §3's discriminator for a valid `discovery_prefix_boundary` (a
   non-empty `direct.unresolved_reason`, or `direct.has_target == true`)
   — i.e. `classify_frontier` returns anything other than
   `discovery_prefix_boundary` for that specific `discovery_budget_
   exhausted` issue. This is exactly ADR-0013 §2's genuine "boundary
   provenance probe failed" terminal outcome, and **only** that — never a
   legitimately classified non-boundary capability gap (item 2 above),
   and never any other unclassifiable category, which behaves exactly as
   it already would for an ordinary `S` seed's own such issue under §2/
   item 4 below. Detect this exact condition **directly from the IRQ6
   root's own `discover_m68k_static_graph` result, before that result is
   folded into the shared aggregation**, so it can never be silently
   orphaned as a dropped secondary regardless of processing order: reuse
   the existing `translate_m68k_discovery_issue` + `classify_frontier`
   machinery (already shared, no new mechanism) on the IRQ6 root's own
   `primary_issue` (if present) and on every entry of its own
   `secondary_issues`, applying exactly the (a)+(b) test above to each; if
   **any** entry matches both (a) and (b), reject the **whole candidate
   build immediately** as a plain `FrontendRejected` (the translated
   failing issue), exactly as a whole-program rejection already looks
   today, and do **not** proceed to fold any part of the IRQ6 root's
   result into the shared aggregation. This check is independent of
   aggregation order: it fires whether or not a synchronous seed has
   already occupied the aggregate `primary_issue`.
4. **Do not modify general T144 orphan-secondary behavior for ordinary
   synchronous roots.** This per-root early-detection check applies
   **only** to the IRQ6 root's own discovery result, and **only** to item
   3's exact (a)+(b) discriminator. It must not weaken, bypass, or
   otherwise change the existing multi-seed orphan-secondary tolerance
   (~lines 1944-1971) or the existing secondary-eligibility/drop behavior
   for ordinary `S`-seed secondaries, unless a later, independent
   architectural finding requires it — no such finding exists here, and
   none is introduced by this task.
5. **Explicitly forbidden fallback.** The implementation must **not**
   silently clear or omit `irq6_handler_present` (i.e. quietly fall back
   to "inert") merely because the IRQ6 root's own boundary proof failed
   while the rest of the program might otherwise still promote. A validly
   resolved, installed IRQ6 vector whose boundary provenance genuinely
   cannot be proven (item 3's exact discriminator) is an unrepresentable
   candidate, not a benign absence — hiding it as inert would let an
   unrepresentable installed interrupt handler surface only much later as
   an unrelated runtime watchdog terminal instead of the correct
   build-time fail-closed rejection required by item 3 above. (This is
   unrelated to, and does not change, the separate cases in item 2 above
   and coverage items 4 and 6's sub-case 3 below — an ordinary
   probe-verified `discovery_prefix_boundary`, or any other legitimately
   classified non-boundary IRQ6 frontier — neither of which *is* a probe
   failure.)

## 5. Required synthetic/adversarial coverage (implement and pass all twelve)

1. Async root whose entire graph fits under 256 instructions: behavior
   remains equivalent to current clean-completion ownership (regression
   coverage for the existing "handler fits, works today" case).
2. Async root whose graph exceeds 256 instructions but whose
   entry-connected first prefix is representable: a runnable partial
   program is emitted and the hardware root is dispatchable
   (`irq6_handler_present == 1`, the retained partial handler block is
   present in the emitted dispatch set).
3. Runtime path that stays entirely within the retained async prefix and
   reaches RTE: proves complete IRQ entry -> handler -> RTE ->
   resumed-mainline execution even though unexecuted handler branches lie
   beyond the static prefix.
4. Runtime path that reaches a prefix cut: deterministically reaches the
   ordinary typed `discovery_prefix_boundary` stop (via `genesis_
   dispatch`'s existing frontier arm, unmodified).
5. Malformed/unmapped/odd/conflicting hardware vector: remains build-time
   fail-closed exactly as today (regression coverage, unmodified
   behavior).
6. Boundary provenance probe failure on an IRQ6-root boundary address: the
   **whole candidate build** fails closed per §4's pinned rule — assert
   explicitly that `irq6_handler_present` is never silently left
   `0`/omitted while the rest of the program still promotes; the build
   must reject entirely. Assert this under **both** of the following
   sub-cases, since §4's re-inspection trace shows they are not
   equivalent under the ordinary aggregation path alone:
   1. the IRQ6 root is the *first* root processed to fail (its own issue
      becomes the aggregate `primary_issue` directly) — the simpler case,
      already correctly rejected by the ordinary `partial_or_rejection`
      path today;
   2. a synchronous `S` seed has **already occupied the aggregate
      `primary_issue`** (e.g. that seed's own walk also hit a boundary or
      a different frontier) before the IRQ6 root's own discovery runs, so
      the IRQ6 root's probe-failure issue would otherwise become an
      `aggregated_secondary_issues` entry — the case §4 item 3's explicit
      early-detection check exists specifically to catch; without it this
      sub-case would be silently dropped (no rejection) rather than
      failing the build closed.
   3. **Negative control (must NOT reject).** An IRQ6-root issue that
      legitimately classifies as a **non-boundary** frontier — e.g.
      `unsupported_cpu_form`, `unsupported_device_access`,
      `unsupported_memory_region`, or `unresolved_indirect_target` (§4
      item 2/item 3's exact (a)+(b) discriminator) — is asserted to flow
      through the **ordinary** shared aggregation and become an ordinary
      typed emitted frontier exit, **not** rejected by item 3's special
      early check, both when it is the first root processed (becomes the
      aggregate `primary_issue` directly) and when a synchronous `S` seed
      has already occupied the aggregate `primary_issue` (it becomes an
      ordinary `aggregated_secondary_issues` entry instead). This is the
      exact case §4 item 2's correction distinguishes from a genuine
      boundary-probe failure, and it is required for Stage C outcome B (a
      genuine CPU/C4/device frontier reached inside the IRQ6 handler) to
      remain reachable at all.
7. Unreachable/orphan retained blocks (from either root) excluded
   according to ADR-0014's P1/P1b invariants.
8. An IRQ6-root outgoing edge to neither retained code nor an accepted
   typed frontier: whole candidate fails closed (unchanged
   `partial_or_rejection` equality check).
9. Deterministic repeated construction and generated C (two runs,
   byte-identical) for a fixture exercising the async-root partial
   prefix.
10. Saturated ADR-0013 `S` (`|S| == 4`, three runtime-confirmed promotions
    plus the reset entry) plus a partial IRQ6 root in the same program:
    proves the hardware root remains outside the Phase-B seed count and
    no 4->5 widening occurs (extends the existing analogous test from
    SEG-007-T047's own coverage, which only tested a *clean* IRQ6 root
    against a saturated `S`; this generalizes it to a *partial* IRQ6 root
    against a saturated `S`). This assertion is kept **separate** from
    items 11-12 below: it proves root identity does not alter `S`/seed
    accounting, not cross-root supersession.
11. **Cross-root supersession, direction 1 (synchronous -> asynchronous).**
    A synchronous-entry seed's walk records an unresolved secondary issue
    for address *a*; the asynchronous IRQ6 root's own, independently
    valid walk separately admits *a* as a retained decoded instruction
    with no `aggregation_conflict`: the stale unresolved candidate for
    *a* is dropped from the aggregate (§2a).
12. **Cross-root supersession, direction 2 (asynchronous -> synchronous).**
    The asynchronous IRQ6 root's walk records an unresolved secondary
    issue for address *a*; a synchronous-entry seed's own, independently
    valid walk separately admits *a* as a retained decoded instruction
    with no `aggregation_conflict`: the stale unresolved candidate for
    *a* is dropped from the aggregate (§2a). Also assert a negative: a
    genuine cross-root contradiction (mismatched `decoded_matches`,
    indirect target-EA set, or completion-RTS) for the same address
    between the IRQ6 root and a synchronous seed still fails the whole
    aggregate closed via `aggregation_conflict`, unchanged.

## 6. Stage C: re-execute the authorized pinned Sonic route and record the truthful result

After implementation, re-execute the authorized pinned Sonic Phase-B route
(`tools/genesis_startup_bridge.py --mode commercial`) at least twice
byte-identically. Record the truthful result honestly using these four
outcome buckets (do not assume any of them in advance):

- **A** — RTE is reached and the previous VBlank RAM wait exits:
  prioritize the milestone-observable/SEG-007-T049 renderer path per
  the project charter if enough deterministic VDP state now exists for useful frame
  composition, otherwise route wherever the next actual runtime-selected
  frontier is.
- **B** — a generated CPU/C4/device frontier is reached inside VBlank
  service: record and route it through the milestone's normal
  classify-or-implement continuation rules (do not speculatively
  implement it inside this same successor unless it is trivially bounded
  and precedented, per the project charter's normal same-task-absorption rules —
  this task does not pre-decide that).
- **C** — a runtime-confirmed async-root `discovery_prefix_boundary` is
  reached: this is a genuinely new, narrower open question (monotonic
  accumulation / runtime-confirmed bounded continuation of an
  asynchronously rooted partial program, without reinterpreting or
  widening the saturated reset-root seed set `S`) — record
  `needs_full_refinement` for exactly that narrower question rather than
  build that larger mechanism speculatively.
- **D** — some genuinely different fail-closed terminal: record it
  truthfully and apply ordinary continuation-disposition rules.

Runtime-selected execution outranks static lookahead; do not assume
outcome A. `docs/development/sonic-title-critical-capabilities.md`
reconciliation happens inside this task's own PR, from the truthful final
executed state.

## Consequences

- The IRQ6 autovector target can now contribute a bounded, entry-connected
  partial prefix exactly like any other static discovery root, instead of
  requiring its entire per-root walk to complete cleanly within the
  `m68k_discovery_max_instructions` ceiling before it can ever be
  dispatchable.
- SEG-007-T144's cross-root supersession bookkeeping is now keyed by
  `StaticProgramRootId` (root kind + ordinal) rather than by raw seed
  index, so the same stale-candidate-superseded-by-independently-admitted-
  code semantics now apply uniformly across both static discovery root
  kinds, in both directions, while ADR-0013's own `S`/`seed_count`/
  `|S| <= m68k_discovery_max_seed_entries` accounting is completely
  untouched by this change — the IRQ6 root never becomes a member of `S`
  and never consumes a seed slot.
- No ceiling changed: `m68k_discovery_max_instructions`,
  `m68k_discovery_max_blocks`, `m68k_discovery_max_frontier_exits`,
  `m68k_discovery_max_seed_entries`, `m68k_expansion_max_rounds`, and
  ADR-0007's `W`/`GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` are all preserved
  byte-for-byte.
- `src/machine/genesis/frontend.cpp` remains the sole production owner of
  the merge/aggregation logic for all `StaticProgramRoot` kinds;
  `src/codegen/c11/frontend.cpp` gains no second merge/aggregation owner
  and continues to only mirror `runtime_frontier_eligible`'s
  shape-validation switch and independently re-derive its own reachability
  BFS over the single, already-merged `FrontendAnalysis`/
  `FrontendPartialProgram`.
- A later task that adds a third `StaticProgramRoot` kind must follow the
  same shared per-root merge mechanism established here (one merge routine
  applied uniformly per root, root-identity-keyed supersession, the same
  §4-style probe-failure discriminator applied directly to that root's own
  discovery result before folding into the shared aggregation) or must
  record, with evidence, why that shared mechanism is unsound for the new
  root kind.
