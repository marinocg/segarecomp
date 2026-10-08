# ADR 0090: SEG-042-T001 Real-Title External-Proof Scope and Producer Qualification Contract

- Status: Accepted (decision: CONTINUE to SEG-042-T002 under the contract below).
- Date: 2026-10-08.
- Task: SEG-042-T001 (part of the combined SEG-042-T001..T007 delivery).
- Related, unchanged: ADR 0089 (SEG-041: `ADOPT HYBRID RECOMP-MAP / ANGR EXPERIMENTAL`; the
  `GenesisExternalM68kFacts` consumer, `genesis_hybrid_container()`/`validate_genesis_hybrid_round()`,
  and `tools/segarecomp_angr_m68k_facts.py`'s corrected exhaustiveness/RTE-exclusion proof are all
  reused unmodified here); ADR 0080 (SEG-031 hybrid admission planner and its containment ladder).

## Question

SEG-041-T008 deliberately left one limitation narrow and unresolved: a site fact produced from a
starting scope/premise is globally valid only when that starting scope is already known to cover
every relevant reachable execution context that can reach the site. SEG-041-T008's own experiment used
`--ram-premise`/`--caller-asserts-premise-completeness`, an explicit caller-owned assertion the tool
cannot itself verify -- acceptable for a project-authored synthetic under direct operator control, not
acceptable as the basis for an *automatic*, real-title harvester with no hand-entered site list (SEG-042's
mandate). What is the smallest generic (title-independent) rule by which SEG-042-T002's automated
harvester can itself decide, for an arbitrary eligible site, whether a candidate starting scope is sound
-- without building a new symbolic-execution/whole-program-reachability framework?

## Decision

### 1. Eligible site families (the qualification contract SEG-042-T002 must honor)

Reusing `GenesisAnalysisFamily` (`platforms/genesis/analysis_report/include/.../report.hpp`) exactly as
segarecomp's own report already classifies every uncovered dynamic-control site -- no new family, no
title-specific logic:

| family | eligible? | mechanism (section below) |
|---|---|---|
| `jsr_an`, `jmp_an`, `jsr_d16_an`, `jmp_d16_an`, `jsr_an_index`, `jmp_an_index` | yes | exact-target proof (§2) |
| `pc_index_explicit`, `pc_index_width_only` | yes | exact-target proof (§2) |
| `rts_computed` | yes | structural containment (§3), **not** exact-target proof |
| `rte` | **no** -- structurally excluded | honest `unresolved`; see "RTE" below |
| `rtr` | yes, if ever observed | exact-target proof (§2); SEG-041-T002 found no usable-semantics gap for `RTR` specifically (only `RTE`'s exception-frame pop was found absent) |
| `unclassified` | no | honest `unresolved` |

**RTE is never eligible, structurally, independent of any proof-path check.** SEG-041-T002 found `RTE`
lifts to a bare "return to address 0" with no SR/PC pop and no frame-format read under every available
p-code 68000-family variant -- not an imprecise model, an absent one. A site whose own family is `rte`
is never handed to the harvester at all (SEG-042-T002 must filter it out before attempting anything, not
rely on the proof-path poison below to reject it after the fact). The proof-path `RTE` exclusion inherited
from `tools/segarecomp_angr_m68k_facts.py` (poisoning any path that merely *fetches* an `RTE` word en
route to a different site) remains additionally in force for every other family, unchanged.

### 2. Exact-target proof: sufficient starting scope

For a site whose family uses an address-register/index-derived target (every family above except
`rts_computed`), the starting scope is **sufficient** -- and the resulting `exact` fact globally valid
-- when both of the following hold, derived entirely from segarecomp's own existing, already-sound
report facts (never from a new reachability framework):

1. **A deterministic `start_pc` candidate, derived only from existing report facts.** Walk backward
   through the new `discovered_lengths` map (PC -> instruction length, a thin per-PC projection of the
   already-existing `report.discovered` map the existing flat `discovered` address list is built from)
   from the site's own PC along strictly contiguous predecessors (`prev_pc + length(prev_pc) == pc`, i.e.
   the straight-line layout segarecomp's own exact discovery already proved reachable) until the first
   address that is itself a proven call-target entry (a key of the new `call_target_continuations` map,
   §4 below) is reached, or the walk cannot continue (a gap, or the program's own root). This candidate
   needs no new analysis: `discovered_lengths`'s contiguity and `call_target_continuations`'s keys are
   both thin projections of already-existing report facts.
2. **No external entry into the candidate span.** The one residual risk a purely local, nearest-preceding-
   entry heuristic cannot rule out on its own is a jump from *outside* the candidate span landing *inside*
   it from somewhere this walk never saw. This is checked directly, generically, against the new
   `static_successors` map (§4.4): reject the candidate (fall back to the next earlier call-target entry,
   or `unresolved` if none remains) if any discovered address outside `[start_pc, site_pc]` has a
   successor strictly inside `(start_pc, site_pc]`. Once this check passes, `start_pc` is a genuine unique
   entry for every real execution reaching the site, derived entirely from existing exact facts.
3. **No injected premise.** The producer is invoked with **no** `--ram-premise` and consequently no
   `--caller-asserts-premise-completeness`. Every register and every memory location the explored path
   reads before it is written starts angr-symbolic (the existing `blank_state` default). A fully symbolic
   register/memory start is a strict superset of every concrete value any real caller could have supplied,
   so it automatically subsumes every actual calling context's influence on the queried register --
   this is why no caller-context enumeration or union step is needed for this family: starting from the
   verified entry with nothing concretely assumed already covers every context that can reach it. **Do
   not use `--ram-premise`/`--caller-asserts-premise-completeness` to manufacture a credited real-title
   fact** (SEG-042's explicit instruction) -- that mechanism remains available only for an operator-
   directed, individually-reviewed experiment (as SEG-041-T008 used it), never inside the automatic
   harvester.

If no candidate in the backward walk ever passes the external-entry check (including the degenerate case
where the walk reaches the program's own root without finding one), SEG-042-T002 must not fabricate a
scope: classify the site `unresolved` honestly. "Unknown context coverage means no exact fact" is enforced
structurally by this rule, not by a trust flag.

### 3. Structural containment for `rts_computed` sites (reusing the existing island/container machinery)

A `rts_computed` site's uncertainty is a return address popped from the stack, not a register angr can
usefully bound by symbolic execution from an arbitrary start (a fully-symbolic stack read at that point
is, correctly, unconstrained -- not a useful exact-target attempt, and not worth spending a producer run
on). SEG-041/042's central hypothesis is that this uncertainty does not need exact reconstruction, only
sound containment:

- The function whose epilogue is the `rts_computed` site is identified by the identical backward-walk-
  plus-external-entry-check mechanism §2 defines (reusing `discovered`/`static_successors`); its entry PC
  is, by construction, a key of `call_target_continuations`. segarecomp's own existing report **already
  knows, exactly**, every call site that can transfer into it (every ordinary `JSR`/`BSR` edge the
  ordinary discovery resolves exactly; this is D's own exact closure, nothing angr-derived). The set of
  feasible return addresses for that epilogue is therefore bounded, structurally, by
  `call_target_continuations[entry]` -- a fact segarecomp can derive and re-verify entirely on its own,
  with **zero angr dependency**, from data it already has.
- SEG-042-T002 emits this as a **`contained`** (not `exact`) fact line (`fact <pc> contained
  <entry>,...`) whose entries are exactly that continuation-PC set (sorted, deduped) -- using the
  identical `segarecomp.m68k_external_facts.v1` artifact and the identical, unmodified
  `genesis_hybrid_container()`/`validate_genesis_hybrid_round()` consumer SEG-041-T008 built. The
  `exact`/`contained` keyword on each fact line (the format's own existing discriminator; there is one
  `producer` line per whole file, not per fact) is sufficient provenance for this harvester's design,
  since every `exact` line it writes comes from `explore_exact_target_pc` and every `contained` line
  comes from this structural mechanism; the single combined file's `producer` line still names the
  harvester as a whole (`segarecomp-recomp-map-harvest-v1`). **No parallel island planner is built; this
  reuses the SEG-031 consumer's existing `points_to_region` container tier exactly.**
- Every entry in `call_target_continuations` comes, by construction, from an already-exactly-discovered,
  already-exactly-resolved static `JSR`/`BSR` (D's own closure); there is no "the caller itself is
  unresolved" case for this specific map. The residual risk is a *different*, currently-unresolved
  dynamic site elsewhere whose own (unknown) target set happens to include an address inside this island
  -- `static_successors` cannot name an edge no analysis has resolved yet. SEG-042-T002 does **not** need
  its own transitive-widening loop to stay sound against this: the unmodified SEG-031 closure/fixed-point
  loop (`plan_genesis_hybrid_admission`'s existing `rounds`) already re-admits every island's entries
  into D itself and re-solves, and the independent `validate_genesis_hybrid_round` re-verification
  (SEG-031/ADR 0080's pre-existing "freshly recompute and compare" property, never weakened by SEG-041)
  fails the whole round closed if admitting this island reveals a reachable address outside it. This
  harvester's one-round initial guess (`call_target_continuations[entry]`) only needs to be a reasonable
  attempt at soundness, not a self-proof of it: an insufficient guess costs a wasted closure round or an
  honest `broad` fallback, never an unsound credited result.
- This mechanism deliberately does not touch `RTE`'s own semantics: containment here only ever reasons
  about which exact call sites exist in segarecomp's own call graph, never about executing or modeling an
  exception return.

### 4. Proof-path CPU-legality requalification (the smallest practical production-side mechanism)

SEG-041-T002 found concrete over-acceptance on every available p-code 68000-family variant (genuine
68010+/68020+-only forms, and Coldfire/68020/68030-specific reserved-space repurposing, all decoded as
valid where real MC68000 silicon would trap). The existing producer only screens for one named
instance of this class directly (`RTE`, because it is legal-but-semantically-unsupported, not
illegal). A real title's proof paths can fetch other words no screen currently catches.

This task adds the smallest possible mechanism rather than a new cross-language certificate framework:
a tiny, bounded, non-production batch CLI,
`segarecomp-m68k-primary-word-classify --words <hex4>[,<hex4>...]`
(`apps/m68k-primary-word-classify/main.cpp`, a standalone top-level diagnostic tool living outside
`platforms/genesis/analysis_report/` specifically because that directory's `CMakeLists.txt` is reserved
by `analysis_core_boundary_test` for exactly the analysis-report driver's own three targets; linked only
against `segarecomp::cpu_m68k`, never installed, never linked into `segarecomp`/the compiler/runtime), that
calls the exact same pure generation-time authority the decoder itself uses,
`m68k_classify_primary_word` (ADR 0043 section 3, `libs/cpu/m68k/src/legality.cpp`), and prints each
word's classification (`legal` / `line_a_emulator` / `line_f_emulator` / `illegal`). This is **not** the
independent test-side legal-form dataset (`tests/fixtures/m68k-legal-forms.json`, deliberately decoupled
from every consumer except its two already-whitelisted ones) -- it is a direct, bounded query over
production code, exactly as legitimate a consumer as the decoder itself. No decoding logic is
duplicated in Python: SEG-042-T002's harvester collects the set of distinct opcode words every explored
path fetches (the same per-step fetch the existing `RTE` check already performs), and after exploration
closes, issues **one** batched call per site with the deduped word set; if any word classifies as
anything other than `legal`, the whole proof is poisoned (`unsupported_proof_path_illegal_word`),
exactly like an errored or unconstrained path -- a proof-path screen, not a general instruction
blacklist, not RTE emulation, not p-code patching.

### 5. Fail-closed rules (unchanged, restated for the automated harvester)

- Resource exhaustion (step bound hit with an unresolved path remaining) is `Unknown`, never a credited
  result, regardless of how the bound is reached.
- An errored or unconstrained path poisons the whole proof for that site.
- A proof path that fetches `RTE`, or any word the new classifier rejects, poisons the whole proof.
- A `rts_computed` containment set that cannot be shown complete (a non-exact caller in the transitive
  closure, a bound exceeded) falls back to `unresolved`, never a partial/best-effort island.
- Bounds (`--max-steps`, `--max-entries`, the containment widening bound) are fixed before the harvest
  run and are never raised solely because a desired result has not yet appeared.

## CONTINUE/STOP decision

**CONTINUE** to SEG-042-T002 under this contract. The mechanism needed is smaller than a new framework:
reuse of (a) the existing report's own per-site family/entry attribution, (b) the existing, unmodified
exact-target producer with the no-premise discipline above, (c) the existing SEG-031
`points_to_region`/`genesis_hybrid_container` consumer for the new call-graph-containment producer class,
and (d) one new, tiny, bounded CLI over an existing pure function. No new IR, no new whole-program
reachability framework, no RTE semantics, no title-specific logic.

## Consequences

- SEG-042-T002's harvester must: filter out `rte`/`unclassified` sites before attempting anything;
  derive `start_pc` for every other family from the existing report's own entry attribution, never a
  caller-supplied premise; derive `rts_computed` containment from the existing exact call graph, never
  from angr; batch-classify every proof path's fetched words through the new CLI before crediting
  `exact`.
- The known two Sonic 1 `RTE` sites are expected to remain `unresolved` (structurally excluded, not a
  proof-path failure); a real measured blocker, not a mechanism defect, per ADR 0089.
- This contract is deliberately silent on title-specific addresses, call graphs, or thresholds; every
  rule above is expressed over segarecomp's own existing report structures and is checked generically by
  SEG-042-T002 against whatever a real ROM's report actually contains.
