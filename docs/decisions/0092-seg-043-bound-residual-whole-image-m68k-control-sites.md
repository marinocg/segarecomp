# ADR 0092: SEG-043 Bounding residual whole-image M68K control sites

- Status: Accepted. Decision: **PARTIAL CONTAINMENT IMPROVEMENT, STILL `H = U`; the interrupt-resumption
  premise is the sole remaining blocker class; broad AOT remains the production default; no SEG-044.**
- Related, unchanged: ADR 0080 (SEG-031 hybrid admission; `whole_image -> broad` is the sound consequence
  of an unproven site), ADR 0089/0090/0091 (external facts, the SEG-042 harvester contract and measured
  baseline).

## SEG-043-T001 -- CFG-shaped proof scopes (CONTINUE, implemented)

### Region definition (generic, report facts only)

Predecessor graph `preds`: every static branch/taken-branch/call-target edge of `static_successors`, plus the
contiguous-layout edge `q -> q + length(q)` for every discovered instruction (`discovered_lengths`). In
T001's first cut that layout edge was a deliberate over-approximation (it also appeared after an unconditional
`bra`/`jmp`/`rts`); the T002 refinement below shows this is **not** purely conservative (an extra predecessor
can mask an unaccounted entry) and replaces it with an exact fall-through projection.

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
## SEG-043-T004 -- RTE / interrupt-return semantics (not activated; cancelled by its own gate)

Gate: every non-RTE residual site closed on at least one real title. After T001-T003: Sonic 1 has 4 non-RTE
residual sites, Sonic 2 has 2, Cool Spot has 9. The gate is not met on any title and RTE is the dominant
blocker on none. No RTE work was started.

## SEG-043-T005 -- unchanged SEG-031 planner, final measurement

| | U | D | whole_image: baseline / SEG-042 / SEG-043 | contained facts | islands (entries) | rounds | H | H/U | outcome |
|---|---|---|---|---|---|---|---|---|---|
| Sonic 1 | 246293 | 1276 | 13 / 7 / **6** | 7 | 7 (13) | 1 | 246293 | 1.000000 | broad_whole_image |
| Sonic 2 | 496387 | 335 | 5 / 4 / **3** | 2 | 2 (5) | 1 | 496387 | 1.000000 | broad_whole_image |
| Cool Spot | 498276 | 6907 | 22 / 12 / **10** | 12 | 12 (49) | 1 | 498276 | 1.000000 | broad_whole_image |

Residual (19 sites): 4 `rte`; 13 non-RTE sites whose input the credited analysis cannot carry across an
interrupt boundary (`interrupt_resumption_unproven`, or the same-cause `invalidated`): Sonic 1 four, Sonic 2
two, Cool Spot seven; and 2 Cool Spot `rts_computed` regions that a program root enters.

## Final answers

1. **Did CFG-shaped scope improve real sites?** Yes, as a sound scope: it flips 7 site scopes (S1 1, S2 1, CS 5) from
   `no_sound_starting_scope` to a verified dominating region (including the motivating Sonic 1 loop site), and
   enabled two new Cool Spot RTS facts. It produced no new credited *exact* fact; every exact-target attempt that
   became possible hit an interrupt-sensitive site (not creditable) or the unchanged step bound.
2. **Did generalized RTS containment improve real sites?** Yes: two branch-entered epilogues (Sonic 1, Sonic 2)
   are now contained, and one Cool Spot fact was corrected from an under-approximate 10-continuation set to 25.
3. **Did finite selector-domain recovery close PC-index sites?** No. 0 of 6 closed. The credited analysis loses
   the selector at an unproven interrupt resumption; finite domains exist only under the uncredited
   transparent-handler ablation.
4. **Was RTE work activated?** No (gate not met).
5. **Did any real title reach `complete_hybrid` / `H < U`?** No. All three remain `broad_whole_image`, H/U =
   1.000000.
6. **Economics?** Not measured (no `H < U`), as the pre-registered precondition requires.
7. **Does broad AOT remain the production default?** Yes, unchanged (ADR 0080 decision 11).

## Classification

**PARTIAL CONTAINMENT IMPROVEMENT, STILL `H = U`: the interrupt-resumption premise is now the sole remaining
blocker class on every title.** 13 of the 19 residual sites (and 100% of the residual non-RTE sites except two
program-root-entered Cool Spot RTS regions) are interrupt-boundary sites that the credited analysis refuses to
resolve and that an external program-order prover cannot credit. This is the class SEG-038/039/040 (ADR
0086/0087/0088) attempted and stopped on; SEG-043 adds the evidence that it is the *only* thing left for the
non-RTE sites, not the scope, reachability or RTS-entry problems this milestone targeted.

Honest limits that remain: (a) `contained` sets are structural proposals under the assumption that the `rts`
pops its entry frame's return slot; the SEG-031 consumer checks mapping/evenness/decode and closure, not the
completeness of a contained set (reproduced by mutation: a dropped continuation is silently accepted), so any
future `H < U` result must be backed by the ADR 0080 runtime-PC-escape check; (b) angr runs here under the
pure-68000 p-code language because this image's pypcode lacks `CPU32`; no exact fact was credited, so no
credited result depends on that substitution.

## Successor decision

No SEG-044 is registered. The only mechanism that could change the result is a sound interrupt-boundary
(handler-effect / resumption) proof for the 13 interrupt-sensitive sites plus RTE exception-frame semantics for
the 4 `rte` sites. That is a whole-program analysis architecture that SEG-038, SEG-039 and SEG-040 already
attempted without a credited improvement; naming it again would be an endless refinement chain, not a bounded
unattempted mechanism. Selective-AOT research is not declared dead: the structural-containment machinery landed
here is reusable and would immediately pay off if a sound interrupt-boundary proof ever exists, but no such proof
is in sight at bounded cost. Broad AOT remains the production strategy.


## SEG-043-T006 -- independent adversarial review (PASS WITH FINDINGS) and hardening

An independent reviewer (separate agent, no edit rights) re-derived the real-title numbers (all matched, regenerated
fact files byte-identical), wrote its own dominance checks (0 violations on 20000 random programs / 215k accepted
pairs under a stack-exact oracle, and 0 on the three real titles), verified `no_fallthrough` against opcodes, and
independently recomputed all 21 contained fact sets (0 mismatches). Architecture rules (static/AOT only, no
title-specific logic, `whole_image -> broad` and SEG-031 unmodified, consumer re-verification unchanged) confirmed.

Findings and disposition:

* **Major -- region is a dominance region, not a stack-frame region.** A static call inside the region to the
  entry (recursion) or to a callee body that only the entry reaches pushes a continuation the `rts` may pop; the
  first cut skipped it (reviewer fuzz: 188 incomplete credited sets in 40000 adversarial programs). **Fixed**:
  `frame_continuations` adds the continuation of every in-region static call whose callee is the entry or lies in
  the region. Facts on the three real titles are byte-identical after the fix (0 such calls in the shipped sets).
  The residual stack-discipline assumption is stated above; the reviewer's fuzz also shows the rule is far better
  than the SEG-042 callers-only rule (5096 bad of 15814 credited) without being a proof.
* **Minor -- call-to-next (get-PC)** also a fall-in: **fixed** (both frames accounted).
* **Major (docs) -- docstring called the consumer a stack-discipline backstop.** **Fixed**: docstring and this ADR
  agree the consumer does not check completeness (mutation reproduced again by the reviewer).
* **Minor -- guard was a two-string denylist and skipped `rts_computed`.** **Fixed**: exact facts need an allowlisted
  (non-interrupt) detail; the interrupt-sensitive set was widened (`interrupt_resumption`, `frame_unproven`) and also
  blocks `rts_computed` containment; a pre-T002 report without `no_fallthrough` now warns loudly.
* **Minor -- stale "layout edge is conservative" sentence**: removed.
* **Note -- T005 headline netting**: the whole_image deltas (S1 7->6, S2 4->3, CS 12->10) are net of the guard; the
  unguarded Cool Spot run credited one single-target `jmp_an` exact fact that is now (correctly) reopened. Scope
  and containment gains are therefore slightly larger than the headline, and that fact is not creditable.
* **Latent -- predecessor graph lacks resolved-dynamic and harvester-exact edges**: zero such edges exist on all
  three full-domain reports; recorded as a precondition if a resolved site ever appears.
