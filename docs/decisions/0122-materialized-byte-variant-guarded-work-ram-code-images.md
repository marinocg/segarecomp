# ADR 0122: Materialized, byte-variant-guarded work-RAM code images

## Status
Proposed. Nothing in this ADR is implemented; it needs acceptance before any code change.

## Context
ADR 0049 compiles work-RAM execution only for **verbatim copies of immutable cartridge bytes**: each alias is one
instruction decoded a second time at a work-RAM execution base and guarded, at every fetch, by a comparison of its own bytes
with the RAM bytes; anything else (transformed copies, decompressed code, runtime-generated code, self-modification) fails
closed. ADR 0097's amendment added one narrow precedent beyond that: a work-RAM JMP **thunk** whose bytes have no cartridge
source, materialized at build time from the build-time headless pass and guarded the same way.

A locally held Genesis title (OutRun class; commercial detail deliberately omitted) exposes a class neither mechanism covers:

- The level-4 (horizontal interrupt) vector points into work RAM. The code it reaches is not a ROM copy: its bytes are written
  at run time as immediates by guest stores (an instruction-by-instruction construction), so ADR 0049 reports
  `frontier_not_verbatim_copy`.
- The handler is a small **state machine encoded in its own text**: each activation rewrites the opcode word of its entry
  (alternating between a short branch to a second phase and a compare form), so the same execution address holds **two
  alternating byte images** over time. A single-variant guard fails closed at the first change.
- The handler reprograms the hint counter and writes vertical scroll memory per interval. Without it the per-line perspective
  effect is missing (tens of thousands of dropped H-interrupts over a short run; the runtime counts and drops them, which is
  the existing compatibility policy for an undeliverable level-4 request).

The same class is expected to recur (RAM dispatch stubs, small self-patching routines), while a bounded, closed set of byte
images per address is the common shape. A general interpreter or JIT is excluded by the project's scope.

## Decision (proposed)
1. **Observed images, not decoded RAM.** The build-time headless materialization pass (ADR 0076 bounded execution rules) records,
   for every work-RAM PC at which the program stops fail-closed, the byte image of the maximal contiguous run of
   instructions around it. Images are identified by `(execution_base, length, bytes)`. A bounded number of *variants* per
   `(execution_base, length)` is admitted (initially 4); an address whose variants exceed the bound, or whose image changes
   after the final admitted observation window, stays fail-closed.
2. **Compile each admitted image once, with the existing decoder and lifter**, exactly as ADR 0049 does for an alias (the
   work-RAM execution address is the decode source address; no second decoder, no relocation). Entries carry their image
   identity.
3. **Per-fetch guard chain is the sole authority.** Entering an address selects among its admitted variants by comparing that
   instruction's bytes (as ADR 0049 does for one variant) in a fixed order; no match returns the existing typed
   `known_but_unemitted_target` stop and commits nothing. RAM is never decoded, compiled or cached at run time; a wrong or
   stale image can only fail closed, never execute wrong code. The cost is one comparison per variant per fetch of a
   RAM-resident instruction, which is bounded by decision 1.
4. **Interrupt delivery.** A level-4 or level-6 vector that resolves into a work-RAM address with an admitted image is an
   ordinary compiled handler entry (the ADR 0097 thunk path generalized). The runtime still performs the CPU-owned exception
   entry; the hint counter and per-line capture (ADR 0115) are unchanged. An undeliverable request keeps the existing
   count-and-drop compatibility default and the strict opt-in stop.
5. **Determinism and provenance.** Images and variant order are a pure function of the build-time pass; output stays
   deterministic. Provenance records the execution address and image identity. No image is a committed fixture: images are
   derived from the current ROM at build time, live only in ignored output and the generated program, and never appear in
   committed evidence (commercial-input policy).
6. **Discovery remains path dependent.** Only images reached by the build-time pass exist; a later unseen image fails closed.
   A run without a stop is not proof of complete coverage (unchanged ADR 0049 caveat).

## Alternatives considered
- **Interpreter/JIT for RAM code:** violates the project scope and the portable-readable-C goal; rejected.
- **Static inference of the constructing stores** (symbolic execution of the code writer): attractive and removes the
  observation dependency, but is an unbounded analysis problem in general; may supplement observation later, not replace it.
- **Game-specific patches or a hand-written handler:** forbidden by the delivery rules.
- **Keep dropping the interrupt:** current behavior; visibly wrong output for the class (missing per-line effects).

## Consequences
- Reuses ADR 0049's guard and ADR 0097's thunk path; the new parts are the multi-variant guard chain and the generalization of
  "thunk" to a bounded instruction run.
- Risks: variant explosion (bounded by decision 1), build-time cost of more preparation rounds, and prefetch-queue timing of a
  self-modified instruction two words ahead of the PC, which the whole-instruction execution model does not represent (as in
  ADR 0049).
- Validation plan (no commercial data committed): a synthetic fixture ROM that constructs a small RAM handler from immediates
  and self-toggles one opcode word between two images, run as an H-interrupt handler; tests for variant selection, the
  variant bound, an unseen third image failing closed, deterministic generated output and strict-C11 compilation; a
  differential check of the handler's register/VSRAM effects against the Musashi oracle on the fixture.
- Acceptance criterion for the proposal: the synthetic fixture matches the oracle bit for bit over at least 100 alternations,
  with unchanged output for every existing title.

## Review of assumptions and unresolved risks (SEG-059; the proposal stays Proposed)
- **Path-dependent observation.** The variant set is whatever the build-time pass happened to execute. A passing run proves only
  those images; completeness is unprovable and the first unobserved image stops the run. The proposal must not be described as
  covering a title, only as failing closed outside the observed images.
- **Finite coverage.** A bound (initially 4) turns an unbounded self-modifying handler into a deterministic stop, but it also
  makes behavior depend on a tuning constant; the bound must be justified by a synthetic fixture, not by one title.
- **Prefetch and self-modification visibility.** The 68000 prefetches two words; a store to an instruction that is already
  prefetched is architecturally visible only after the prefetch queue refills. The whole-instruction execution model reads the
  RAM bytes at instruction entry, so a store into the next one or two words of the executing instruction stream can differ from
  hardware. The synthetic acceptance fixture must include a store to the immediately following instruction and compare with
  the pinned Musashi core (whose limited prefetch cache is a reference point, not a hardware proof); until the behavior is understood, the guard chain cannot be accepted.
- **Per-instruction guard authority.** The guard compares the bytes of one instruction per fetch; a variant chain compares one
  per variant. It never validates a multi-instruction image as a unit, so a partially overwritten image falls to the
  fail-closed stop at the first differing instruction, which may already have retired earlier instructions of the run. Whether
  that is acceptable for interrupt handlers (an interrupted run observed mid-way) is open.
- **Interrupt handling.** Making a work-RAM vector deliverable changes interrupt timing for every title with a RAM vector; the
  current count-and-drop default must remain available until the generalized path is proven equivalent, and level 6 must not
  be enabled before level 4.
- **Fail-closed execution of unobserved variants.** The design relies on the stop being committed-nothing; each admitted
  entry needs a test that an unseen image stops with unchanged architectural state.

This section records risks only. No RAM-code materialization, multi-variant guard, runtime-selected code variant or
generalized RAM interrupt handler is implemented by SEG-059.

## Open questions
- Is a variant bound of 4 sufficient for the observed class, or should it be derived from the pass?
- Should the generalized thunk path be extended to level 6 vectors immediately or after level 4 is proven?
- Should the build-time pass drive more iterations by default for titles with RAM vectors, and what is the preparation budget?
