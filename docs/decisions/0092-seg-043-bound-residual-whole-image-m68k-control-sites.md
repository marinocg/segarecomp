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

### T001 refinement made in T002: exact fall-through accounting

T001's first cut assumed every discovered instruction falls through to the next one. That over-approximation
is *not* purely conservative: an extra predecessor edge can hand a node a predecessor it does not have and
thereby mask an unaccounted entry. The report therefore gained one thin projection of the existing
`m68k_control_successors()` data, `no_fallthrough` (instructions with no sequential successor; calls and
TRAPs are excluded because their continuation is reached through the return). The layout edge is now
`q -> q + length(q)` only for `q` outside `no_fallthrough`, and statically pushed code addresses
(`pushed_code_addresses`, a manual `PEA`/`RTS` call) join the program roots as entries whose stack is not an
ordinary call frame. T001's accepted pairs were re-measured under this stricter graph (see T002 evidence).

## SEG-043-T002 -- branch-entered / shared-epilogue RTS containment (implemented)

The SEG-042 rule contained a `rts_computed` site only if its dominating entry `s` was a key of
`call_target_continuations`, and used *only* that call set. It never looked at the other entries of `s`.

`frame_continuations(s, p)` now accounts for every structural entry of `s` (predecessors of `s` outside the
region that loop back to it):

* a static call into `s` -> its continuation;
* any other entry (branch, real layout fall-in, shared epilogue reached by branch, tail transfer) -> the same
  question asked of the entering instruction's own dominating frame, recursively (depth <= 4, continuation set
  <= 256, cycles Unknown);
* fails closed when `s` is a program root or a statically pushed address, when an entering instruction has no
  sound dominating frame, when no entry is known, or when any bound is hit.

It is a structural **proposal**, not a stack-discipline proof: it assumes the `rts` pops the return slot of
its frame's entry. The `rts_computed` class exists exactly because segarecomp's own return-slot analysis could
not prove that, so the unmodified SEG-031 consumer remains responsible only for what it checks.

**Consumer limits (measured, relevant to T006).** `--external-m68k-facts` re-verifies each entry's
mapping/evenness/decode and rejects the whole file when it is malformed, but it does **not** check that a
`contained` fact's target set is *complete*. Mutating a real Cool Spot fact by dropping a legitimate
continuation was silently accepted (12 islands, unchanged result); an odd or unmapped target demotes that site
to `whole_image`; a malformed file is rejected outright; an extra unrelated target is accepted (a superset is
sound). Completeness of a contained set therefore rests on the harvester proof, and any future `H < U` claim
must be backed by the runtime-PC-escape check required by ADR 0080 rather than by the consumer.

## SEG-043-T003 -- PC-index selector-domain recovery (STOP: no sound credited domain exists)

Residual `pc_index_explicit` sites: Sonic 1 four (`0x390` pinned `invalidated`; three `index_unknown` with
`interrupt_resumption_unproven`), Sonic 2 two (both `interrupt_resumption_unproven`), Cool Spot none.

**Where precision is lost (measured with the private per-point trace).** For the Sonic 1 loop site the entry
state is clean at the region entry (stack pointer known, no register facts) and Unknown inside the
region: at an interruptible point of the loop `apply_resumptions` deliberately joins an *unproven* interrupt
handler resumption, which makes every D/A register and the stack pointer Unknown(`interrupt_resumption_unproven`),
and the previously resolved targets are `invalidated` by the growing fixed point. The selector is never a
missing finite domain in the credited analysis; it is a domain the analysis refuses to carry across an
interrupt boundary it cannot prove transparent (ADR 0079 decision on interrupt resumption).

**Uncredited attribution experiment** (`--diagnostic-transparent-handlers --assume-no-z80-ram-writes`; never
credited, never fed to a fact file): the same sites would resolve to finite sets on Sonic 1 `0x390` (8
targets), `0xb5a` (25) and `0x71f94` (5), and Sonic 2 `0x390` (16); Sonic 1 `0x72a60` stays `width_only` and Sonic
2 `0x44e` stays `target_outside_image` even under the ablation. So finite selector domains exist, but only
under the transparent-handler premise that SEG-038/039/040 (ADR 0086/0087/0088) already tried and failed to
discharge soundly. SEG-043 does not build another abstract domain or re-run that STOPped path.

**Fallback.** The angr program-order exact attempt also cannot be credited at these sites for the same reason
(it never models interrupt resumption); the harvester guard now covers `interrupt_resumption_unproven` *and*
`invalidated`. The one site that is not itself `interrupt_resumption_unproven`-detailed (Sonic 1 `0x390`) was
re-tried from T001's improved region at the unchanged bound: `resource_exhausted` after 100000 steps = Unknown.
Result: closed via a credited finite domain **0**; closed via angr **0**; unresolved **6/6**, every one with
the named reason above. Cool Spot (no PC-index site) is unchanged.
