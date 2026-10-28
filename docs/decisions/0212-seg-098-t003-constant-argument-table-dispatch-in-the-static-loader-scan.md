# ADR 0212: SEG-098-T003 - constant-argument dispatch through immutable offset tables; the BIOS-service loader class stays unresolved

- Status: Accepted (pre-registration first, then result and decision). Related: ADR 0206 (the static loader scan), 0210 (authentic survey; ranks 2 and 3), 0211 (static admission).
- Privacy: project-authored fixtures; authentic measurements are counts and classes only.

## Question and evidence-based choice

ADR 0210 ranked, after the module-table roots: (2) walking past computed calls and immutable PC-indexed dispatch tables, (3) BIOS CD/file-service wrappers. Re-running the
authentic disc with an experiment that makes the dispatch exact showed what actually blocks (below), so T003 implements the bounded, generic part of rank 2 and reports rank 3
unresolved with its exact reason, instead of widening semantics until a commercial example passes.

## Pre-registered rule (written before the matrix was run)

Option `constant_dispatch` (`discover-modules --constant-dispatch`; the build enables it): the walk (a) folds ADD/ADDI/ADDQ/SUB/SUBI/SUBQ into a data register when both operands are
known, (b) treats a MOVEM store of registers to the stack as writing no register, (c) reads a word/long at a known PC-relative address (`d16(PC)`, `(d8,PC,Xn)` with a known index) when
that address lies inside the SP image (the table is *assumed* immutable - a wrong assumption only yields a proposal the runtime byte guard refuses), and (d) follows
`JSR/JMP (d8,PC|An,Xn)` whose base and index are known as an ordinary constant transfer (inlined call, entry proof, load-range note). One call site with a constant selector therefore
reaches exactly one handler; no table extent is guessed and nothing is enumerated. A computed transfer whose target is unknown is the typed refusal `computed_control_unresolved`
(only in this mode; the default scan's output is unchanged). Limits are the existing ones (states, steps, depth, operations, modules).
Not added: symbolic execution, taint tracking, memory modelling, a scripting language, any model authority, any title-specific fact.

| # | Case | Expected |
|---|------|----------|
| 1 | dispatcher with constant selectors (3 arms), rule off | nothing discovered, no new refusal |
| 2 | same disc, rule on | A, B, C discovered, equal to the direct-loader fixture's modules |
| 3 | selector from a hardware register | A and B discovered; C absent; `computed_control_unresolved` |
| 4 | disc with no computed control | rule changes nothing |
| 5 | determinism; no manifest option | holds |
| 6 | BIOS-image build (T002 path) on 2 and 3 | static admission of A, B, C resp. A, B; the same binary runs them; C never executes |
| 7 | authentic disc | measured; the BIOS-service class is judged against it |

Gate: `LOADER_CLASS_GO` (rows 1-6 hold; the class is generic and bounded), with the BIOS-service class reported `UNRESOLVED` if the authentic measurement shows its arguments are not constants.

## Result

`tests/sega_cd_loader_dispatch_test.py` (10 checks) and the `dispatch` / `dispatch_unknown` cases of `tests/sega_cd_bios_unvisited_test.py`: rows 1-6 behaved as pre-registered. One
variation: with the register-fed selector the program (selector value 0) dispatches to compiled module A, so "C fails closed" became "C never executes" (nothing uncompiled is reached).

Authentic disc (aggregate): with the rule the walk goes from 38 steps to 1,185 steps / 60 states, resolves 1 dispatch site (5 immutable table reads), runs through the SP's whole
initialisation sequence including its service-layer handlers, and ends in a constant jump into a zero-filled part of the SP image (code that the service layer loads at run time).
Still 0 CDD/CDC register-programming sites, 0 load operations, 0 modules; 4 BIOS-service call sites (2 with a non-constant function), 3 sites that load a data register from
work-variable memory ((An), (An)+, d16(An)), 1 unresolved computed transfer (`computed_control_unresolved`). The static scan of the authentic disc still inventories 0 modules.

## Decision: `LOADER_CLASS_GO` (constant-argument table dispatch) - and `BIOS_SERVICE_LOADERS_UNRESOLVED`

The dispatch class is generic, bounded and proven by fixtures through the supported build. The BIOS CD/file-service class is **unresolved**: on the authentic disc the walk now
reaches the service layer, and the load parameters live in the SP's work-variable memory (a state machine polled across BIOS calls), not in registers at a call site. Recognising
them would need memory/taint modelling of the SP work area (explicitly out of scope) and the BIOS service ABI as evidence; the survey finds no bounded constant-argument shape to
pre-register. The next evidence-backed opportunity is therefore not another loader class but (T004/T005) coverage accounting and the honest `partial` label: an authentic title's
module inventory from static analysis of the SP alone is 0, and the observed route remains the source of authentic coverage.

## Notes for T005 and consumers (from the adversarial review)

- `complete` in the scan report means only "the walk was not exhausted by a resource limit". It is NOT "no computed transfer was left unresolved": a scan can report `complete: true`
  together with `computed_control_unresolved` refusals. Completeness labelling (T005) must key off the refusal classes, never off `complete` alone.
- The survey object gained `dispatch_resolved`, `table_reads` and `memory_carried_reads` (the last two are counted only with the rule on); modules, operations, refusals, states and
  steps of the default scan are unchanged.
- The folded arithmetic (ADD/SUB widths, ADDQ immediate 8, ADDA/SUBA staying unknown) is exercised through the dispatcher fixtures only; there is no dedicated arithmetic unit test.
