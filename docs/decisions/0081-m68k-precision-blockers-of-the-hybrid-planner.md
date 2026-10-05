# ADR 0081: Precision Blockers of the Hybrid Planner and the Bounded Refinements Measured Against Them

- Status: Accepted (SEG-034-T001; later children append their records and the final decision below).
- Date: 2026-10-05
- Task: SEG-034-T001..T007 (one combined delivery).
- Related: ADR 0079 (M68K analysis instantiation; decisions 7 and 8 are the interrupt-resumption and return-slot rules), ADR 0080
  (hybrid selective admission; decision 11 is the pre-registered production-default rule, unchanged here; decision 12 names the
  blocker classes this ADR measures).

## Context

SEG-031 established that on every authorized complete title `hybrid_total == U`: one uncovered dynamic site with no bound narrower than
the whole program is enough to return the entire broad universe. SEG-034 asks whether bounded, generic, sound refinements of the SEG-030
analysis can remove those whole-image triggers, and whether the result is a materially smaller hybrid. It is an optimisation milestone;
soundness is not negotiable (no register-preservation premise, no restored `RTS returns to the syntactic continuation`, no
operand-width authority, no moved thresholds).

## Decision

1. **Progress metric.** `hybrid_total / U` is not an intermediate metric (any single remaining trigger holds it at 1.0). Each
   refinement is judged by the whole-image trigger count, the targeted-trigger count, the sites reclassified whole-image -> bounded or
   exact, the generic cause distribution (`fallback_cost_by_reason`) and the closure round in which a trigger appears. The final
   production value (`hybrid_total / U`, generated C, compile CPU, binary, runtime) is measured only when a title has no remaining
   trigger or the smallest justified blocker set, and is judged by ADR 0080 decision 11 unchanged.
2. **A store that may run past a region end is resolved, not failed closed.** The M68K memory domain treated a store whose pointer set
   reaches `region.size` (the widening limit is one past the last byte) as "may spill into the next region" and failed closed: every
   cell poisoned, every return slot `rewritten`, every asynchronous range widened to all of memory. The spill is bounded exactly. An
   offset `o <= size` reaches bytes `[o, o + span)`, so the bytes beyond the region lie in `[size, size + span)`: the first `span`
   bytes of the bus region that follows, which the machine view (`region_of`) names. `M68kFiniteAdapter::resolve_store_spill`
   replaces the target by (a) the in-region part clipped to the region's last `span` bytes, which covers every partially in-region
   store, plus (b) one bounded landing store in the next region when that region is tracked (work RAM). An untracked next region
   (cartridge, I/O) holds no abstract-memory cell; a bus hole holds none; anything the clip is not defined for stays the original
   (fail-closed) target. The replacement is an over-approximation of the touched bytes, never a strong update. Soundness fixtures
   cover the landing (a cell on the landing bytes is Unknown), the in-region part (the last cell of RAM), the wrapped bus and the
   negative that a store which lands on the return slot still makes it `Unknown(return_slot_rewritten)`; three mutants (resolution
   disabled, landing dropped, in-region part dropped) are killed.
3. **Private diagnostics.** `segarecomp-genesis-analysis-report --trace-points <path>` writes the abstract state of every point of the
   final solve (exact PCs; private, ignored locations only) so that each trigger is attributed to the fact that became Unknown.
   `--diagnostic-transparent-handlers` (with `--hybrid-plan`) is an **uncredited ceiling measurement**, in the family of
   `--assume-no-z80-ram-writes`: handler instances the frames domain cannot analyse are treated as transparent. It writes no plan, its
   aggregate carries `"credited":false`, and it exists only to answer what the interrupt-resumption class costs. It does not
   reinstate the removed assumption: the default analysis never reads it.

## Records

### SEG-034-T001 (baseline, root causes, the spill defect)

Current-`main` (`81a88fc`) planner, Release driver, sanitized (identical to the SEG-031 table):

| title | `U` | sound `D` | outcome | whole-image triggers (round 1) |
| --- | --- | --- | --- | --- |
| Sonic 1 | 246,293 | 1,276 | `broad_whole_image` | 14 (interrupt 6, computed RTS 8) |
| Sonic 2 | 496,387 | 335 | `broad_whole_image` | 5 (interrupt 3, computed RTS 2) |
| Cool Spot | 498,276 | 6,907 | `broad_whole_image` | 22 (interrupt 8, computed RTS 14) |
| OutRun | 496,950 | - | `broad_analysis_incomplete` | - |
| Streets of Rage | 247,761 | - | `broad_analysis_incomplete` | - |
| Golden Axe | 249,843 | - | `broad_analysis_incomplete` | - |

**Root-cause chain (Sonic 1, `--trace-points`).** The 14 triggers are not independent:

1. The first loss was the spill defect: a decompression loop writing through a pointer into the I/O window widened to the window end;
   the resulting "spill" marked the called routine's return slot `rewritten`, so its activation was unproven and every continuation
   after it (A7, status, registers) was Unknown, so the first interrupt-eligible boundary had no frame address, so the handler was
   never analysed (`entry_unknown`), so every cell was asynchronous (`async_all`) and no memory read was precise. Decision 2 removes
   this link: `return_slot_rewritten` 1 -> 0 and 14 -> 13 triggers on Sonic 1; the other five titles' aggregates are byte-identical
   to the baseline.
2. The cascade does not unlock further. After the fix the game-mode dispatch resolves while the status is masked and is lost again
   (`invalidated`) once code that enables interrupts is reached: from there on the dispatch index (a register live across an interrupt
   boundary) and everything downstream depends on what the unanalysed handler does to registers, memory and the saved frame.

**Why the interrupt class cannot be bounded by a sound per-register effect (T002).** The Sonic 1 VBlank handler's own closure,
analysed from its vector (standalone, 3,600 instructions), writes all of D0-D7 and A0-A6 (no register is write-free), contains 1,094
stores with an Unknown target, and contains computed returns of its own. A register could only be shown preserved by a save/restore
through a stack slot that no Unknown-target store may have rewritten; the 1,094 stores cannot be excluded from the handler's frame
without the pointer provenance of every one of them, and the alternative (assume they do not) is the register-preservation premise
SEG-030-T009 removed.

**Ceiling.** With `--diagnostic-transparent-handlers` (unsound, uncredited) the interrupt class is assumed away entirely:

| title | `D` | whole-image triggers | what remains (generic classes) |
| --- | --- | --- | --- |
| Sonic 1 | 6,806 | 23 | operand-width-only PC-indexed 9, unbalanced / unknown-base computed RTS 9, RTE of the handler 2, `JMP/JSR (An)` unknown base or region exit 3 |
| Sonic 2 | 6,792 | 64 | unbalanced computed RTS 39, PC-indexed (explicit or width-only) 22, `JSR (An)` 2, RTE 1 |
| Cool Spot | 6,907 | 22 | unbalanced / unknown-base computed RTS 14, `JSR/JMP (An)` 7, RTE 1 |

Removing the interrupt class alone therefore exposes the next class on every title; no title reaches zero triggers. The
operand-width-only dispatches are the object-routine tables indexed by a RAM byte, whose only static bound is the operand width,
which ADR 0080 decision 3 forbids as an authority.

**Resource completion (class 4).** Streets of Rage: the frames driver alternates between two contexts configurations
(`no_validated_round` after 16 rounds); the stale summaries observed differ only in the pushed-address sets of the shared absolute stack
slots that the exit states carry, so each round's derivation exceeds the used summary and the climb outlasts the round bound. Golden Axe: a warm
contexts solve exceeds the per-solve iteration bound (1,000,000) on a 72k-point graph. Neither is a ceiling that should be raised; a
structural fix would change what a summary carries and is a new analysis design, not part of this milestone. OutRun, Streets of Rage
and Golden Axe stay `broad_analysis_incomplete` and are not credited.

Determinism: the aggregate JSON of every title is byte-identical across independent runs, and the five titles the refinement does not
touch are byte-identical to the baseline.
