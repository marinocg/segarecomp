# ADR 0092: SEG-043 Bounding residual whole-image M68K control sites

- Status: In progress (combined SEG-043-T001..T006 delivery; the final classification is recorded in the
  last section when SEG-043-T006 closes).
- Related, unchanged: ADR 0080 (SEG-031 hybrid admission; `whole_image -> broad` is the sound consequence
  of an unproven site), ADR 0089/0090/0091 (external facts, the SEG-042 harvester contract and measured
  baseline).

## SEG-043-T001 -- CFG-shaped proof scopes (CONTINUE, implemented)

### Region definition (generic, report facts only)

Predecessor graph `preds`: every static branch/taken-branch/call-target edge of `static_successors`, plus the
contiguous-layout edge `q -> q + length(q)` for every discovered instruction (`discovered_lengths`). The
layout edge is a deliberate **over-approximation** (it also appears after an unconditional `bra`/`jmp`/`rts`):
an extra predecessor can only add nodes the closure must account for, never hide an entry, so it is
conservative.

For a candidate entry `s` (the unchanged nearest-first candidate generator) and a site `p`, the **region** is
the backward closure of `p` over `preds`, cut at `s`. The candidate is accepted iff `s` *dominates* `p` in
that graph, i.e. the closure

1. reaches no program root other than `s`;
2. reaches no node with no known predecessor (an unaccounted entry);
3. stays within a fixed 4096-node bound (exhaustion = Unknown, never raised to obtain a result);
4. actually reaches `s`.

Every edge into a region node other than `s` therefore originates inside the region. An ordinary loop
backedge is internal whatever its numeric address; a genuine second entry walks the closure to a root, to an
unaccounted node or past the bound and is rejected. This replaces the SEG-042 linear `[start_pc, site_pc]`
test (kept as `--scope-model span` for before/after measurement only).

### Soundness argument and falsification

* The new rule is an exact dominator test on a graph that over-approximates the true control flow of what
  segarecomp has already proven. Like the old rule it cannot see edges that no analysis has resolved
  (unresolved dynamic targets, return-continuation edges, interrupt entry); those remain the SEG-031
  closure/`validate_genesis_hybrid_round` backstop's responsibility, unchanged.
* It is strictly stronger than the span rule in one respect: a program root inside the region is a genuine
  entry and is now rejected (the span rule could accept it when the nearer root candidate was itself rejected).
* Synthetic adversarial cases (tests/segarecomp_recomp_map_harvest_test.py): (1) internal backedge whose
  source is numerically past the site -> span rejects, region accepts; (2) genuine second external entry ->
  rejected (root and unaccounted variants); (3) nested internal loops -> accepted; (4) outside branch with an
  independent entry -> rejected, outside node reachable only through the region -> accepted; (5) no closed
  boundary (root inside region, start not an ancestor, bound exhaustion) -> rejected. A 600-graph randomized
  test compares `entry_closed_region` with an independently written brute-force dominance oracle.
* Real titles: every accepted (start, site) pair on Sonic 1, Sonic 2 and Cool Spot was re-checked with a
  second, independently written forward-reachability oracle: 0 violations.
* **Discovered finding (fail-closed correction).** Exact-target facts for sites whose report detail is
  `interrupt_resumption_unproven` are no longer credited by the harvester. segarecomp's own analysis refuses to
  resolve those sites precisely because an asynchronous interrupt between the defining instruction and the
  site may change the input; angr executes in program order and never models interrupt resumption, so an
  `exact` fact would silently assume transparent handlers. The unguarded run produced one such fact on Cool
  Spot (a `jmp_an`, single target); it is recorded as an *uncredited* measurement only.

(Measurements and the T001 disposition are in the task Evidence; the closing sections of this ADR collect the
final numbers.)
