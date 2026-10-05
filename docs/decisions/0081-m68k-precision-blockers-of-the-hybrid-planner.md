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
   store, plus (b) one bounded landing store in the next bus region (work RAM, but also an untracked region: it holds no cell, yet the
   release and observed-store consumers read the target extent, so the landing pair is kept). A bus hole adds nothing; anything the clip
   is not defined for stays the original (fail-closed) target. The replacement over-approximates the touched bytes and is never a
   strong update and never carries a value: the clipped member is not the real store position, so a store whose target was replaced
   records no value and erases the cells it may touch (found by the independent gate in two rounds; fixtures use known-value
   stores, multi-member sets and a bus hole). Soundness fixtures
   cover the landing (a cell on the landing bytes is Unknown), the in-region part (the last cell of RAM), the wrapped bus and the
   negative that a store which lands on the return slot still makes it `Unknown(return_slot_rewritten)`; six mutants (resolution
   disabled, landing dropped, untracked landing dropped, in-region part dropped, clipped member dropped, value joined) are killed.
3. **Private diagnostics.** `segarecomp-genesis-analysis-report --trace-points <path>` writes the abstract state of every point of the
   final solve (exact PCs; private, ignored locations only) so that each trigger is attributed to the fact that became Unknown.
   `--diagnostic-transparent-handlers` (also in the plain report, whose aggregate then carries the same marker) is an **uncredited ceiling measurement**, in the family of
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

### SEG-034-T002 (interrupt-resumption register effects): stopped

The refinement the milestone describes is a bounded per-register handler effect (register never written, written then restored from a
soundly tracked save slot, finite output), never `handler not analysed -> registers preserved`. It was evaluated against the measured
handler closures before anything was built:

| title (handler analysed standalone from its vector) | instructions | registers written | stores with an Unknown target | note |
| --- | --- | --- | --- | --- |
| Sonic 1 | 3,600 | all of D0-D7 and A0-A6 | 1,094 | saves and restores through MOVEM; also holds 9 computed returns |
| Sonic 2 | 119 | MOVEM save/restore of all 15 | 11 | its routine table is indexed by a RAM byte (the closure includes table-junk code) |
| Cool Spot | 12 | none directly | 1 | trivial handler; the chain breaks upstream (below) |

* "Never written" cannot hold for any register of a non-trivial handler. "Restored from a save slot" needs the slot not rewritten by
  any store the analysis cannot place; the Unknown-target stores of the closure (hundreds to a thousand) cannot be excluded from the
  handler's own frame without the provenance of every pointer they use, and assuming they do not alias the frame is the
  register-preservation premise SEG-030-T009 removed.
* The dependency is circular and self-reinforcing: an unresolved dispatch gives its call continuation an Unknown status, an Unknown
  status makes every boundary interrupt-eligible with no proven supervisor frame address, the handler is then unanalysed, and the
  unanalysed handler clobbers the register that would have resolved the dispatch.
* Cool Spot's handler is small enough to be provable, but its main flow loses the status and the stack pointer first, at an unproven
  (stack-unbalanced) activation in the initialisation path (11,245 of 11,300 main-flow points then have an Unknown status). The interrupt class is not that
  title's first loss.
* The uncredited ceiling (decision 3) shows the class cannot move any title on its own: with interrupt resumption assumed away
  entirely, Sonic 1 still has 23 whole-image triggers, Sonic 2 64 and Cool Spot 22.

Stop rule applied: the class needs a register-preservation or frame-integrity premise, or the provenance of every pointer a handler
uses, and even that does not change a title's admission. No handler-effect machinery was added; the SEG-030 differential, mutation and
SEG-031 hybrid gates are unchanged by this task.

### SEG-034-T003 (relative A7 and computed returns): stopped

Every remaining `stack_unbalanced` site was classified from its stack delta (private trace, counts only here):

* Sonic 1 (5 sites, all in the sound driver run from the handler): one idiom. A routine pops its own return address and then
  returns, so the RTS pops the *caller's* return slot (delta +4, precise). Two of the five are reached with the deltas {0, +4} joined:
  one RTS shared by a normal and a skipping path. Cool Spot: the push-then-RTS dispatch (`MOVE.L (An,Dn),-(A7); RTS`, the pushed
  value a ROM table entry selected by a range-guarded register), the same skip idiom, and a compiler-emitted frame thunk: a leaf
  routine pops its own return address into a register, builds the caller's frame (LINK, MOVEM, SR push), re-pushes the register and
  returns, to be undone by a later tail-jumped epilogue thunk. Cool Spot's main flow first loses its stack pointer in a call into that
  sound-driver API from the initialisation path (SR save/restore pairs and these thunks). The tracked deltas are already relative to
  the activation entry (SEG-030-T005); the balanced `MOVEM`/`LINK` forms are already proven.
* The popped cell's value is not an analysed fact. It is readable only if no asynchronous or external writer is credited, which the
  Z80 release and the unanalysed handler forbid in these titles (every work-RAM read is Unknown), or from the recorded return slot,
  which is exactly the return-slot integrity premise (ADR 0079 decision 8), here extended from the RTS at its own slot to an RTS that
  pops a caller's slot or a pushed value. A symbolic relative return stack (return cells keyed by the activation entry, not by an
  absolute A7) is the smallest abstraction, and it still rests on that extended premise, because every store the analysis cannot
  place may alias those cells.
* The joined deltas {0, +4} need the delta split per path (a partition), a larger change than the milestone's "smallest relative-A7
  abstraction".

Stop rule applied: the class needs the integrity premise to be widened to new sites, which is a trust decision (an ADR 0079
amendment), not a precision refinement. It is recorded as a candidate that requires that decision; nothing was implemented, and it
does not change a title's admission (with the interrupt class assumed away the remaining sites include the PC-indexed families
below).

### SEG-034-T004 (pointer and base provenance): stopped

Each `base_unknown` / `context_bound` site was traced back to the missing provenance fact (with the interrupt class assumed away: Sonic 1:
3 `JMP/JSR (An)` and 4 computed returns; Cool Spot: 7 `JMP/JSR (An)` and 4 computed returns; Sonic 2: 2 `JSR (An)`):

* The object dispatcher: `A1 <- ROM pointer table[object-id byte read from RAM]; JSR (A1)`. The missing fact is the value set of a
  RAM byte (the object id) written from level-layout data. Its only static bound is the operand width (256 entries), which ADR 0080
  decision 3 forbids as an authority. It is the same fact that leaves nine PC-indexed sites width-only on Sonic 1.
* A decompressor continuation passed in a register by its caller (`JMP (A3)`) in a callee merged past the context bound K = 8. A
  per-call-site context would resolve it; raising K or the depth globally for one site is excluded by the milestone.
* Computed returns whose popped cell has an Unknown base after the unbalanced idiom of T003.

No finite object or region identity exists for these pointers: the objects are runtime-allocated slots of a RAM array whose
contents come from ROM data. A field-sensitive refinement of an array of homogeneous objects requires a heap abstraction the
milestone excludes. Stop rule applied.

### SEG-034-T005 (closure remeasurement and classification)

After the only successful refinement (the spill resolution), the planner was rerun from scratch on all six titles (sanitized):

| title | whole-image triggers before -> after | closure rounds reached | outcome |
| --- | --- | --- | --- |
| Sonic 1 | 14 -> 13 | 1 (no title leaves round 1) | `broad_whole_image` |
| Sonic 2 | 5 -> 5 | 1 | `broad_whole_image` |
| Cool Spot | 22 -> 22 | 1 | `broad_whole_image` |
| OutRun, Streets of Rage, Golden Axe | incomplete -> incomplete | - | `broad_analysis_incomplete` |

Classification: **C** (the closure never reaches a second round; the bounded refinements do not remove the triggers). Removing the
interrupt class would not give B either: the ceiling run shows 22-64 whole-image triggers per title in the next class, and the
newly reached code exposes more unbounded dispatch (on Sonic 1 and Sonic 2, `D` grows from 1,276 and 335 to about 6,800 and the trigger count grows with it).

### SEG-034-T006 (conditional second-round refinement): not activated

T005 found no title with a small second-round blocker set and one coherent cause; the first round is still whole-image everywhere.
Not activated.

### SEG-034-T007 (decision)

**Outcome: C. Broad AOT remains the production strategy for every CPU / image class (ADR 0080 decision 12 unchanged); the hybrid stays
an opt-in measurement instrument.** No title produced a non-broad plan, so there is no broad-versus-hybrid pair to build or time; the
SEG-031 measurements (identical trees, executables and states at ratio 1.000000) stand, and the production emission was rechecked
byte-identical on the final head (below). ADR 0080 decision 11 is untouched: no threshold moved.

What would have to hold for a selective admission, by class, and what each costs:

| class | what removes it | cost / status |
| --- | --- | --- |
| interrupt resumption (every title) | a named, counted register-preservation / frame-integrity premise for delivered handlers, or the provenance of every pointer a handler uses | a trust decision SEG-030-T009 refused; alone it moves no title (ceiling run: 22-64 triggers remain) |
| computed returns (Sonic 1, Sonic 2, Cool Spot) | the return-slot integrity premise widened to RTS that pop a caller's slot or a pushed value, plus a per-path split of the stack delta | an ADR 0079 amendment; still rests on a premise |
| RAM-indexed dispatch (object routine tables, every title) | the value set of an object-id byte filled from level data; the only static bound is the operand width | forbidden authority (ADR 0080 decision 3); a heap/object abstraction is excluded |
| external writer (Z80) | the materialized Z80 images as an input of the store-freedom proof (SEG-030-T010) | a plumbing change; not a Sonic blocker today because the handler class dominates |
| resource completion (OutRun, Streets of Rage, Golden Axe) | a summary representation that does not climb over shared stack-slot records | a new analysis design |

The decisive row is the third: even with the first, second and fourth removed, the object dispatch leaves a whole-image trigger on
every complete title, and it has no sound bounded refinement under the current authority set. A materially smaller sound hybrid is
therefore **not reachable by precision work in this architecture**; the only real reduction found is the spill resolution, which
removes a defect (one trigger on one title) and leaves the admission unchanged.

Consequences:

- Generated-C economics (220 MB for Sonic 1) are a representation question (SEG-022 / SEG-025 style), to be reconsidered in a separate
  evidence-driven milestone; this milestone adds no helper or factoring scheme.
- Admission narrowing would need an explicit decision to admit the operand-width authority (and a measurement of its closure on the
  authorized corpus) together with the premise decisions above; each is a separate architecture decision, not a refinement.
- The instruments stay: `--trace-points` (private attribution) and `--diagnostic-transparent-handlers` (an uncredited ceiling) cost
  nothing when unused, and the planner reruns the question after any such decision.
