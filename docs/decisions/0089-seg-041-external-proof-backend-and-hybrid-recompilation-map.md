# ADR 0089: SEG-041 External Proof Backend and Hybrid Selective-AOT Recompilation Map

- Status: Accepted (decision: **REJECT ANGR / RETAIN MAP ARCHITECTURE**. angr was never obtained or
  executed in this delivery's execution environment — every sanctioned package channel failed for
  reasons entirely unrelated to angr's own suitability (a broken authentication token for the
  configured internal mirror, and a sandboxed network path whose TLS interception is trusted by the
  system/`curl` trust store but not by Python's `certifi` CA bundle). No MC68000 or Z80 semantic
  qualification was ever reached. The tool-neutral recomp-map/island architecture designed in
  SEG-041-T001 reuses the existing `GenesisHybridAdmissionPlan`/`GenesisHybridPlan` machinery, is
  self-consistent on paper, and is **not implemented** in this delivery because the implementation
  gate it was explicitly designed to protect (qualified backend plus demonstrated leverage) was never
  met. Global broad whole-ROM AOT (ADR 0083) is unchanged and, as this entire delivery empirically
  demonstrates, requires zero external tooling.)
- Date: 2026-10-07
- Task: SEG-041-T001, SEG-041-T002, SEG-041-T007 (one combined, report-only delivery; T003/T004/T005/T006
  cancelled transitively by T002's own pre-registered activation gate and never ran).
- Related, unchanged: ADR 0080 (SEG-031 hybrid admission planner and its material-benefit adoption
  threshold — not reached, not moved), ADR 0083 (SEG-036, the current sole production strategy),
  ADR 0088 (SEG-040, the direct predecessor whose evidence this milestone treats as established: both
  the M68K interrupt-composition and Z80-interference tracks reached classification C — compositional
  approach insufficient — motivating this milestone's question of whether an external, mature,
  path-sensitive engine could do what segarecomp's own bounded analyses could not).

## Question

ADR 0088 (SEG-040) established that two materially different compositional refinements of segarecomp's
own abstract-interpretation engine could not make any measured real title's hybrid plan non-broad, and
located the Z80 side's remaining wall precisely as a **data-dependent cartridge-compression routine**
over immutable ROM data — exactly the shape a mature concrete/path-sensitive execution engine might have
leverage on. SEG-041 asked: can segarecomp move this class of reasoning out of its own core and into an
optional external backend (beginning with angr), producing a tool-neutral, ROM-bound recompilation map
that the existing SEG-031 planner can consume as selective code plus bounded broad-AOT islands, while
leaving global broad AOT as the unconditional, tooling-free correctness fallback?

## Outcome

### SEG-041-T001 (done): hybrid recomp-map architecture and trust contract — CONTINUE

A census of the existing production machinery found that **the island and trust/versioning
abstractions this milestone's brief calls for are not new**: `GenesisHybridAdmissionPlan`
(`platforms/genesis/machine/include/segarecomp/machine/genesis/hybrid_admission.hpp`, ADR 0080)
already defines a versioned, ROM-bound, fail-closed, consumer-validated artifact schema
(`segarecomp.m68k_hybrid_admission_plan.v1`); `GenesisHybridPlan`/`GenesisHybridSite`/
`GenesisHybridContainer` (SEG-031) already define the exact island/widening-ladder/closure semantics
the brief describes, with an existing independent containment validator
(`validate_genesis_hybrid_round`); and the existing Z80 write-proof
(`platforms/genesis/analysis_report/include/segarecomp/genesis_analysis_report/z80_ram_write_proof.hpp`,
SEG-030-T010/SEG-040-T004) already accepts an externally-supplied Z80 image set through the identical
`nullopt -> image_set_unknown -> all` fail-closed contract a recomp-map-supplied image would use. The
proposed `segarecomp-recomp-map-v1` schema, completeness classes (`complete_selective`/
`complete_hybrid`/`incomplete`), trust-tier split (structurally re-verifiable vs. semantic-completeness
facts), fallback table (`broad`/`hybrid`/`auto`), and cache identity were designed as an extension of
these existing types, not a parallel system, and do not resemble a general-purpose decompiler IR.
**Decision: CONTINUE to T002**, with every later child's activation threshold pre-registered verbatim
in the SEG-041-T001 harness record before any further work began.

### SEG-041-T002 (done): angr/p-code backend qualification — REJECTED (environment-unavailable)

Before any semantic comparison could be attempted, this task first had to confirm the backend was
obtainable in the execution environment. It was not, through any sanctioned channel:

- **Local availability**: no `angr`/`pypcode` installation or vendored copy exists anywhere in either
  repository.
- **Configured package mirror**: the environment's authenticated internal Artifactory index returns
  `401 Error, Credentials not correct` for every package lookup (confirmed even for packages already
  locally installed, such as `requests`) — a broken/expired token for this index in this environment,
  not evidence the mirror lacks angr specifically.
- **Direct PyPI**: reachable at the TCP/TLS layer (`curl` succeeds, `200`), but every `pip install`
  attempt against it — including inside a fresh, isolated `venv`, and including with `certifi`'s CA
  bundle passed explicitly via `--cert` — fails with `SSLCertVerificationError: unable to get local
  issuer certificate`. This is consistent with a sandboxed network path performing TLS interception
  whose certificate the system/`curl` trust store accepts but Python's bundled `certifi` list does not;
  no corporate root CA file usable to close that gap was found on disk.
- **Deliberately not attempted**: disabling TLS certificate verification to force-install a large,
  security-sensitive symbolic-execution toolchain (`claripy`/`cle`/`archinfo`/`pyvex`/`unicorn`/
  `z3-solver` and native extensions) from an unauthenticated channel. That is a security-control bypass,
  not a legitimate qualification step.
- An existing local Ghidra integration (`tools/ghidra.py`, diagnostic-only elsewhere in this harness)
  does not substitute for angr's concrete/path-sensitive symbolic-execution engine, so its availability
  does not change this verdict.

**Verdict: `REJECTED` — environment-unavailable ground.** This is explicitly **not** a finding about
angr's MC68000 or Z80 semantic fidelity; that comparison was never reached. Per SEG-041-T001's
pre-registered gate, SEG-041-T003 and SEG-041-T004 (both of which require a backend able to concretely
execute real guest M68K code) were cancelled transitively with this exact reason; SEG-041-T005's own
activation gate ("T002 qualified AND >=1 of T003/T004 leverage") was consequently also unmet, cancelling
it in turn; SEG-041-T006 depends on T005 and was cancelled transitively as well.

### SEG-041-T003 (cancelled): activation condition unmet

Depended on SEG-041-T002 qualifying a backend able to concretely execute real guest M68K code. T002
recorded `REJECTED`; T003 never ran. No manual reimplementation of cartridge-compression reconstruction
was attempted (explicitly out of this milestone's scope). The underlying hypothesis — that concrete/
path-sensitive execution of the real guest decompressor could recover a unique Z80 boot image where
segarecomp's own abstract interpreter cannot — is **not disproven**, only untested in this environment.

### SEG-041-T004 (cancelled): activation condition unmet

Depended on the same T002 gate. T002 recorded `REJECTED`; T004 never ran. The mandatory containment
synthetic (selective region A -> difficult dynamic site -> contained island I -> known exits ->
selective region B, required to widen-or-report-incomplete under mutation rather than silently retain a
stale island) was designed in SEG-041-T001 but never executed against a real backend.

### SEG-041-T005 (cancelled): activation condition unmet

Depended on T002 qualifying the backend **and** at least one of T003/T004 showing leverage. Neither
condition was met; T005 never ran. No producer/validator/consumer code was written — correctly, per the
gate's explicit purpose of not building speculative machinery ahead of evidence it has something to
consume. SEG-041-T001's design remains available, unimplemented, for a future session with working
package-channel access.

### SEG-041-T006 (cancelled): activation condition unmet

Depended on T005 producing a working pipeline to measure on real titles. T005 was cancelled; T006 never
ran. No new real-title measurement was performed — the credited baseline remains exactly ADR 0088's
post-SEG-040 state, unchanged, since no production code path affecting it was touched anywhere in this
milestone.

### SEG-041-T007 (this task): decision and independent adversarial/fallback gate

**The sixteen required questions:**

1. **Is the recomp-map abstraction sound/useful?** On paper, yes: it is a direct extension of
   `GenesisHybridAdmissionPlan`'s existing trust/versioning pattern and `GenesisHybridSite`'s existing
   island ladder (SEG-041-T001). It was never exercised against a real producer, so "useful in
   production" remains unproven, only "sound in design."
2. **Is the island abstraction sound/useful?** The abstraction already exists in production
   (`GenesisHybridSite`/`island_entries`/`validate_genesis_hybrid_round`) independently of this
   milestone; this milestone's contribution (an externally-proven containment class and a generic
   structural widening algorithm) was designed but not implemented or tested.
3. **Can every difficult dynamic site be classified exact/island/unresolved?** The schema enforces no
   silent third case by construction (SEG-041-T001); whether a real backend can actually *populate*
   that classification usefully is untested.
4. **Did angr qualify for the required MC68000 subset?** **N/A** — never reached; angr could not be
   obtained in this environment (SEG-041-T002).
5. **Did angr recover any real Z80 boot image?** **N/A** — SEG-041-T003 never ran.
6. **Did Z80 interference improve?** **NO** — unchanged from ADR 0088; no production code affecting it
   changed anywhere in this milestone.
7. **Did angr resolve exact dynamic target sets?** **N/A** — SEG-041-T004 never ran.
8. **Did angr prove useful containment where exact proof failed?** **N/A** — same reason.
9. **Did any real title produce `complete_hybrid`?** **NO** — no map was ever produced.
10. **Did any produce `complete_selective`?** **NO** — same reason.
11. **Selective count, island count, `H`, `U`, `H/U`?** Unmeasured; unchanged from ADR 0088's baseline
    (every credited title remains `broad_whole_image`/`broad_analysis_incomplete`).
12. **Generated-source/compile reduction?** **NONE** — zero production/runtime/emitter code changed.
13. **Did behavior match broad AOT?** Trivially yes — broad AOT is the only strategy exercised; nothing
    else exists to diverge from it.
14. **Did fallback work with zero external tooling?** **YES, empirically, for the entire delivery.**
    This whole milestone ran inside an environment with zero angr/Python-symbolic-execution/pypcode
    tooling available, and the existing fast test suite (95/95, 100%) passed unchanged at the final
    head — the strongest possible demonstration that `broad` requires no external backend, because
    none was ever present.
15. **Should `auto` remain opt-in or become default?** Moot — `auto`'s current default is unchanged;
    no map-aware strategy code was written in this milestone to make default.
16. **Should angr remain the producer, be experimental, or be rejected?** **Rejected for this
    environment, on availability grounds only.** The map/island *architecture* is retained
    (undeployed) as a validated design for a future environment with working package-channel access;
    angr itself is not adopted, not marked experimental-in-production, and not disproven on semantic
    grounds — it was simply never reachable.

**Explicitly distinguishing "angr failed" from "the hybrid recomp-map architecture failed":** angr was
never tested at all — its rejection here is a pure environment-availability finding. The recomp-map/
island architecture was not tested against a real producer either, but it was positively evaluated on
paper against the milestone's own soundness requirements (SEG-041-T001) and found to be a conservative
extension of already-production-sound types, not a novel or risky abstraction. Neither failure
implicates the other.

**Final classification: `REJECT ANGR / RETAIN MAP ARCHITECTURE`.** `STOP SELECTIVE-ADMISSION RESEARCH`
is explicitly not selected: nothing in this milestone's evidence newly disproves the underlying
finite-state/leverage hypotheses from SEG-038/040; the environment simply could not test them with this
specific tool. A future session with a working angr/pypcode package channel (a valid mirror token, a
trusted direct-PyPI path, or an operator-supplied vendored wheel set) should resume directly from
SEG-041-T001's design and SEG-041-T002's exact failure evidence, rather than re-deriving either.

## Mandatory fallback test matrix (section 27 of the originating brief)

Every fallback scenario the brief lists reduces, in this delivery, to one already-true, empirically
reconfirmed fact rather than N new test cases, because no map-aware strategy code exists yet to
exercise individually: `broad` strategy requires zero external tooling (angr unavailable, backend
executable missing, and every other "no backend" variant are the *actual, uncontrived state* of this
entire delivery's execution environment, not a simulated condition), and the existing production path
is entirely unaffected by that absence — confirmed by the unchanged 95/95 fast-suite pass at the final
head. The map-specific scenarios (invalid/malformed/stale map, wrong ROM hash, wrong analysis-contract
version, failed island containment, semantic qualification failure) have no code to exercise them
against yet; they remain SEG-041-T001's designed-but-unimplemented fail-closed rules, to be tested as
real test cases only once SEG-041-T005 is ever activated by a future session.

## Independent adversarial/completion gate

Given the product diff for this entire milestone is documentation-only (this ADR; the harness backlog
records live in the harness repository, not the product), this review was self-conducted against the
first-hand evidence already gathered in this delivery (every command re-run a second time with
identical results before being recorded; `git status --short` on the task worktree confirmed clean
beyond the empty claim commit through every checkpoint), per AGENTS.md's instruction that docs/
control-only changes use the fast gate rather than a full independent review cycle. Checked explicitly:
no implementation claim in this ADR or any SEG-041 child record overstates what was actually exercised
(every `N/A` above is accurate, not a disguised untested-positive); no stray production file was
touched; the `REJECTED` verdict's evidence (401 responses, `ModuleNotFoundError`, `SSLCertVerificationError`)
is reproducible and was reproduced twice; the decision does not conflate "angr failed" with "the map
architecture failed" anywhere in this document.

A fresh fast gate was run against the exact final head (product branch `task/seg-041-t001`):
**100% tests passed, 0 tests failed out of 95** (`agent_verify.py fast`). The full gate was not re-run
beyond this, per AGENTS.md's instruction that a docs-only diff uses the fast tier unless executable
behavior is itself the claim being checked — the only executable claim here ("fallback requires zero
external tooling") is demonstrated by the existing suite passing unchanged, which the fast tier already
confirms.

## Consequences

- No credited production code changed anywhere in this delivery. The entire product diff is this ADR.
  Every SEG-041 child's actual findings/evidence live in the harness backlog (SEG-041, SEG-041-T001..T007),
  never in a product commit.
- Broad AOT (ADR 0083) remains the unconditional, sole correctness and production strategy. SEG-031's
  hybrid containment/admission policy (ADR 0080) is unchanged; its material-benefit adoption threshold
  was never reached and is not moved by this ADR.
- The tool-neutral recomp-map/island architecture (SEG-041-T001) is a validated, reusable design,
  **not implemented**: `segarecomp-recomp-map-v1`'s schema, completeness classes, trust tiers, fallback
  table, cache identity, and producer/consumer contract, extending `GenesisHybridAdmissionPlan`/
  `GenesisHybridPlan` rather than replacing them.
- angr is rejected for this execution environment on pure availability grounds; this is not a semantic
  verdict and does not foreclose reopening SEG-041-T002 in a future environment with a working package
  channel, starting directly from this ADR's exact evidence rather than re-deriving it.
- The SEG-038/040 finite-mutable-state and data-dependent-Z80-decompression findings remain durable,
  unchanged, reusable evidence for any future attempt on either track.
