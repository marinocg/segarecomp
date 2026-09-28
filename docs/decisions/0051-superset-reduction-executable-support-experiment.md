# ADR 0051: Superset-Reduction Executable-Support Experiment (Rejected as a Gen-3 Basis)

- Status: Accepted (decision: REJECT the hypothesis as the Gen-3 basis; keep Gen-2 broad immutable-ROM AOT)
- Date: 2026-09-29
- Task: SEG-024-T001
- Related, unchanged: ADR 0002 (static translation, fail closed), ADR 0009 (computed indirect control flow),
  ADR 0011 (whole-program RTS continuation set), ADR 0039 (independent immutable-ROM AOT identities),
  ADR 0048 (push-then-RTS computed jump), ADR 0049 (immutable-copy aliases).

## Question

Can segarecomp keep the Gen-2 broad immutable-ROM AOT set as a conservative universe `U` of independently valid
instruction identities and emit only a much smaller set `L ⊆ U` that a proof-driven executable-support analysis
shows architectural control flow can select, keeping every unresolved dynamic-control source conservative
(`unknown -> KEEP`), without a general symbolic-execution system?

## Experiment (report-only; generated output unchanged)

- Generic, CPU-neutral least fixed point (`libs/recompiler/.../executable_support.hpp`): opaque ordered identities,
  typed support edges, typed target domains (`exact_set`, `return_continuation`, `exception_continuation`,
  `bounded_region`, `any_candidate`), provenance of the first support reason, deterministic rounds. Domains are
  inputs formed before the fixed point; `narrow_target_domain` rejects any narrowing without independent evidence,
  any live-set-derived evidence (anti-circularity) and any non-subset proposal.
- MC68000 projection (`libs/cpu/m68k/.../control_support.hpp`): fixed successors and dynamic-site families read from
  the existing owner `m68k_operation_effect` plus the lifted condition/control EA; no new decode/lift/semantics.
  The only new fact is the operand-width interval of `(d8,PC,Xn.W)` targets (base ± 32 KiB).
- Genesis composition (`platforms/genesis/machine/.../executable_support.hpp`): `U` = `immutable_rom_aot_entries`;
  roots = reset entry + installed vectors 2..63 from the immutable vector table (the extracted
  `genesis_vector_handler_bus_address` rule); domains: Gen-2 authority for RTS (reconstructed ADR 0011 set, ADR 0048
  push-window RTS = any), RTE/RTR and runtime-owned JMP/JSR = any (Gen-2 dispatches them through the compiled-entry
  lookup), Tier-1 exact sets where discovery proved them. Conditional variants model RTS as live-call continuations
  only and RTE as resuming a live boundary; their premises are stated, not proven.
- CLI: `emit-general-startup-bridge-c ... --immutable-rom-aot --executable-support-report <path>` writes sanitized
  aggregate JSON (counts, FNV digests; no addresses). `*_without_any` runs are explicitly UNSOUND floors: every
  `any_candidate` site ablated, i.e. the best any future refinement of the remaining unresolved sites could reach.

## Results (Debug build, Apple clang, arm64; six authorized local Genesis titles, sanitized)

| title class | U | sound L (current facts) | live any-sites reachable without any expansion | floor, all any ablated | fixed flow only |
| --- | --- | --- | --- | --- | --- |
| 512 KiB platformer A | 246,293 | 246,293 (0%) | 41 | 17,853 (92.8%) | 2,081 |
| 1 MiB platformer B | 496,387 | 496,387 (0%) | 87 | 38,780 (92.2%) | 1,672 |
| 1 MiB platformer C | 498,276 | 498,276 (0%) | 17 | 14,737 (97.0%) | 6,940 |
| 512 KiB action D | 249,843 | 249,843 (0%) | 27 | 16,029 (93.6%) | 438 |
| 1 MiB racing E | 496,950 | 496,950 (0%) | 15 | 14,929 (97.0%) | 331 |
| 512 KiB beat-em-up F | 247,761 | 247,761 (0%) | 62 | 20,142 (91.9%) | 1,385 |

- In every title the first broad domain reached is an exception return (RTE), 2-9 rounds from the vector roots.
- Every live broad family's sole contribution equals the entire otherwise-removable set: one selectable unresolved
  site anywhere forces `L = U`. Families: RTE (6/6), `JMP/JSR (An)` (5/6), `(d8,PC,Xn.W)` jump tables (5/6),
  `JMP (d8,An,Xn)`, `JMP d16(An)`, ADR 0048 push-window RTS (title-specific).
- Conditional return models (unproven premises) still leave 4-26 live broad sites and `L = U` in 6/6.
- One bounded generic refinement (operand-width regions, sound, no value analysis) changed no sound result and made
  the floor worse (22-43% of `U` retained in 5/6 titles): 64 KiB regions admit data decodes whose own dynamic sites
  and fallthrough chains snowball.
- The existing Gen-2 RTS authority is itself inflated by data decodes: the reconstructed return set (1.5k-4.2k
  targets) is dominated by continuations of call-shaped data words (title A: fixed flow 2,081 -> 17,853).
- Determinism: repeated runs are byte-identical; generated C is byte-identical with and without the report.
- Cost is not the obstacle. Synthetic series (high-entropy filler + known code): 512 KiB / 1 / 2 / 4 MiB ->
  U = 238k / 476k / 952k / 1.9M; frontend analysis incl. enumeration 0.9 / 1.8 / 3.5 / 7.2 s; nine-run support
  matrix 1.7 / 3.5 / 7.3 / 15.0 s; emission 9.8 / 19.9 / 40.6 / 82.9 s producing 0.34 / 0.63 / 1.23 / 2.42 GB of C.
  All linear; `U` grows with image size, not code size (fixed flow = 4 identities at every size).

## Decision

REJECT the superset-reduction hypothesis as the Gen-3 basis. Keep Gen-2 broad immutable-ROM AOT as the production
representation. Keep the experiment as opt-in, report-only measurement tooling; it carries no admission,
emission or removal authority.

## Why

1. With current facts the sound answer is `L = U` for every measured title; the reduction is all-or-nothing and
   the gate is crossed within a handful of rounds from architectural roots.
2. Crossing it needs sound resolution of every selectable RTE, `(An)`/`(d8,An,Xn)`/`d16(An)` and jump-table site
   (15-87 per title): exception-frame integrity across every handler (interprocedural stack discipline), index
   ranges plus table extents, and pointer-table provenance. Those are multi-instruction, path-sensitive and in
   general interprocedural facts, i.e. a general abstract interpreter.
3. Any register-provenance proof across an instruction boundary must also exclude other entries into the
   producer-consumer window (which broad domains themselves create, so proofs and `L` become mutually dependent and
   need an inductive-over-time argument) and interrupts whose handlers may alter the register before RTE. Only
   intra-instruction facts (operand width) are immune, and they were measured to be too weak.
4. The families differ per title, so a narrow family-specific refinement does not generalize.
5. An under-approximated `L` never executes wrong code (dispatch membership stays exact and fails closed), but it
   reintroduces the incompleteness that broad AOT exists to remove.

Positive findings retained: the candidate-universe model handles overlapping starts uniformly (Gen-2 CFG discovery
rejects a direct branch into a discovered instruction), whole-image analysis is cheap and linear, and the generic
layer needs no MC68000 assumption (identities are opaque; alignment, PC effects, bus width and indirect-EA forms
stay in the CPU/machine layers).

## Revisit only if

- a proof-producing value analysis built for another reason resolves every live broad site on a multi-title corpus
  (measure with this report: `current_facts` must fall materially below `U`);
- exception returns gain a proven or runtime-enforced (fail-closed, shadow-frame) resumption domain whose
  completeness cost is measured on long no-hints runs of several titles;
- a full observed-PC coverage transport exists, so runtime-selected targets outside a proposed `L` can be counted
  (today only the 64-entry recent-PC ring exists, so the unsound floors could not be falsified against execution);
- generated-code size or compile time again becomes a measured blocker (SEG-022-T010 concluded it is not).

For a future Z80 target the same generic layer would apply unchanged, but variable-length decoding, `JP (HL)/(IX)/
(IY)`, `RET` and cartridge banking make identities bank-qualified and the unresolved-site problem at least as
severe; nothing here motivates a different Z80 design.
