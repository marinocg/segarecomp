# ADR 0037: Synchronous MC68000 Divide-by-Zero (Vector 5) Exception Entry

- Status: Accepted
- Date: 2026-09-13
- Relates to: ADR 0020 (deterministic VBlank/IRQ6 interrupt progression --
  the exception-frame construction, build-time vector resolution, and RTE
  mechanism reused here), ADR 0013 (discovery-prefix boundary, seed set `S`,
  and the "bounded static hardware root outside `S`" category ADR 0020 §6
  already established for the IRQ6 autovector).
- Extends, without editing its file (numbered-ADR extend-don't-edit
  precedent, ADR 0017's own "Watchdog semantics (ADR 0007 extension)"
  section, itself following ADR 0020's own identical precedent for ADR
  0007): ADR 0020 §7/§8's exception-entry frame-construction primitive (a
  single generic helper parameterized by target handler entry and the
  new SR value, instead of the IRQ6-only inline shape) and ADR 0020 §6's
  build-time vector-resolution technique (parameterized to vector-table
  offset `0x14` instead of `0x78`) -- see "ADR 0020 extension" below.
- Does not amend, and does not relitigate: ADR 0020's IRQ6-specific
  admission/scheduling machinery (SR-mask eligibility, VBlank-pending
  scheduling, `resume_grace_pending`, `dispatch_steps_since_vblank`,
  progress-credit/watchdog accounting) -- that machinery is untouched and
  unreused here. RTE (`genesis_exception_return`, ADR 0020 §9) retains its
  shared architectural frame restoration, with one minimal project-only
  bookkeeping refinement: out-of-band frame-origin provenance arms
  `resume_grace_pending` only for frames created by IRQ6.

## Context

SEG-007-T220 added `MULS.W <ea>,Dn` as the first base-MC68000 multiply/divide
family member. SEG-007-T221 disclosed `DIVS.W <ea>,Dn` directly behind it as
the next discovery-budget frontier. This task (SEG-007-T222) closes the whole
coherent, evidenced family in one batch: `MULU.W`, `DIVS.W`, `DIVU.W`, plus
the one new architectural primitive DIVS.W/DIVU.W require that MULS.W/MULU.W
do not -- a synchronous CPU exception (vector 5, "Zero Divide") raised when
the divisor is zero.

## Phase 0 finding

Direct inspection of `runtime/genesis/runtime.c` (ADR 0020 §7/§8/§9's own
implementation) shows the exception-entry primitive -- validate the complete
six-byte frame extent, write SR then PC, commit `{sr, pc, a[7]}` atomically
only after both writes succeed, then transfer control to the resolved
handler -- is already generic in *shape*: nothing about frame construction
itself is IRQ6-specific. It was, however, inlined directly into
`genesis_irq6_scheduler_and_admit`, hardcoding the target vector/SR mask.
`genesis_exception_return` (RTE) already shares the architectural frame
restoration for both callers. It needs only a minimal project-only refinement:
out-of-band frame-origin provenance makes post-return IRQ6 grace conditional
on an IRQ6-created frame, so a synchronous vector-5 return cannot acquire it.

**Decision: this is a bounded, small generic refactor of an existing
primitive, not a new architectural rule.** No new frame shape, no new
privilege model, no new memory-safety rule is introduced. The refactor
extracts the frame-construction/validate-then-commit sequence into one
shared static helper parameterized by (return PC, target handler entry,
 SR-keep-mask, SR-forced-bits, failure stop/diagnostic). IRQ6 retains its
 byte-identical `T=0, S=1, mask=6` behavior; divide-by-zero uses `T=0, S=1`
 while preserving the original interrupt mask. IRQ6's own
asynchronous admission eligibility (SR mask check, VBlank-pending scheduling,
grace/credit accounting) is NOT part of the extracted helper and is not
touched.

## Decision A -- reuse, don't reinvent

The divide-by-zero exception entry reuses ADR 0020 §7/§8's frame
shape/ordering/memory-safety contract (six-byte basic MC68000 frame -- SR
word then PC long word, no format/vector-offset word; validate-then-commit,
no partial frame ever written) and ADR 0020 §9's shared RTE architectural
restoration as the sole return mechanism. T222 minimally refines RTE's
project-only post-return bookkeeping so out-of-band frame-origin provenance
arms IRQ6 grace only for IRQ6-created frames. The refactor described above
(extracting the shared helper out of `genesis_irq6_scheduler_and_admit`) and
this provenance refinement are recorded here as an **ADR 0020 extension**,
not an edit to ADR 0020's own file, mirroring ADR 0020's own stated precedent
for extending a prior ADR without editing it.

## Decision B -- vector 5 discovery root

Cite the same *M68000 Family Programmer's Reference Manual* (M68000PM/AD Rev.
1 (1992)) Table 6-1 "Exception Vector Assignments" ADR 0020 §6 already cites
as `M2`/its own IRQ6 citation: vector number 5, "Zero Divide", vector offset
`5 * 4 = 0x14`. Genesis cartridge ROM is mapped at address 0 (existing
project fact, reused verbatim). The static discovery / CPU frontend gains a
build-time-only typed fact: the long word at vector-table offset `0x14` is
read from the mapped cartridge image exactly as the reset-vector PC (offset
`0x4`) and the IRQ6 autovector (offset `0x78`) already are, guarded by the
identical "image carries a full vector table" precondition (image at least
`0x100` bytes AND the long word at offset `0x4` equals the resolved startup
 entry), and treated as a new discovery root through the distinct
 `M68kDiscoveryTargetRole::synchronous_exception_vector` role. It shares the
 same build-time resolution, admission, per-root discovery, and aggregation
 mechanics as `interrupt_vector`, but truthfully records a synchronous CPU
 exception rather than asynchronous hardware.

**Discovery-root category: identical to ADR 0020 §6's own rule.** This root
is NOT a member of ADR 0013 §7's seed set `S`, is never runtime-promoted,
never increments `seed_count`, and does not change `m68k_discovery_max_seed_
entries` or `|S|`'s ceiling. It is admitted unconditionally in Phase A,
entirely outside `S`'s bookkeeping, exactly like the IRQ6 autovector root --
a regression test proves `seed_count`/`S` accounting is unaffected by this
addition.

Fail-closed rule, identical to ADR 0020 §6: a non-zero slot resolving to an
odd, unmapped, or otherwise unrepresentable target fails the **build**
(static discovery / emission) closed, never a silent runtime default. A
zero (never-installed) slot, or an image that does not carry a full vector
table, simply establishes no divide-by-zero handler for this program
(`divide_by_zero_handler_present == 0`) -- inert, not a build failure,
mirroring the IRQ6 autovector's own "inert when uninstalled" rule exactly.

**Implementation status.** Vector 5 is admitted as a distinct
`StaticProgramRootKind::synchronous_exception` root. This preserves the
existing per-root fatal-probe checks and cross-root merge/supersession rules
without falsely treating a synchronous CPU exception as a second asynchronous
hardware root. The resolved entry is wired through `FrontendAnalysis` into
generated main; zero/uninstalled slots remain inert and invalid non-zero
targets fail closed.

## Decision C -- synchronous/unmasked semantics

Divide-by-zero is a synchronous, instruction-caused exception, architecturally
distinct from IRQ6's device-generated, asynchronous, SR-mask-eligible
interrupt:

- NOT gated by the SR interrupt-mask field (bits 10-8) -- IRQ6's own
  masking rule does not apply.
- NOT scheduled, NOT "pending" -- it is raised inline, synchronously, at the
  dispatch step of the DIVS.W/DIVU.W instruction whose divisor is zero,
  always taken (never declined, never deferred).
- Does NOT interact with `resume_grace_pending`, `dispatch_steps_since_
  vblank`, or the watchdog progress-credit mechanism -- those remain
  entirely IRQ6-scoped.
- The only shared machinery is the frame-construction helper (Decision A) and
  RTE's architectural restoration. Its project-only post-return grace
  bookkeeping is explicitly origin-aware, so a vector-5 RTE leaves IRQ6 grace
  untouched, including when grace was already set.

## Decision D -- effect-layer contract

Before introducing a new first-class "conditional write" `M68kOperationEffect`
kind, every existing consumer (static discovery, loop-progress analysis,
data-progress analysis, C4 lowering/gap classification, write-footprint
completeness) was inspected. None requires knowing the destination write may
not occur: the conservative existing shape ("Dn may be written", reusing
`resolved_destination_ea`/`affects_condition_codes`, exactly like every other
analyzed instruction's typed fact) remains sound for all of them. No new
effect *kind* is added. `M68kOperationEffect` instead gains two small,
additive, defaulted fields:

```cpp
bool may_raise_synchronous_exception{};
std::uint8_t exception_vector{};
```

set only for `DIVS.W`/`DIVU.W` (`exception_vector = 5`); every existing
consumer ignores these fields safely (default `false`/`0`). The actual
conditional gating (`if (divisor == 0) raise vector 5; else Dn = ...;`) is
implemented purely as an `if` in the C11 emission layer, mirroring this
project's existing routed-access-failure emission pattern -- never a new
effect-layer "kind".

## Non-goals

- No TRAP/TRAPV/CHK/address-error/illegal-instruction/trace/privilege-
  violation support of any kind.
- No perturbation of IRQ6's admission/scheduling/VBlank-pending/SR-mask-
  eligibility/grace logic; that machinery is exercised only by its own
  existing regression coverage, re-confirmed unchanged, not redesigned.
- No discovery-ceiling widening: vector-5 resolution is a bounded discovery
  root outside `S`, exactly like the IRQ6 autovector, and never touches
  `m68k_discovery_max_instructions`, `m68k_discovery_max_seed_entries`, or
  any other ceiling.
- No first-class conditional-write effect kind (Decision D).

## Citations

- **M68000PM/AD Rev. 1 (1992), Table 6-1 "Exception Vector Assignments",
  §6.2 "Exception Vector Assignments":** vector number 5 = "Zero Divide",
  vector offset `$014` (`5 * 4`). This is the same Table 6-1 ADR 0020 §6
  cites for the level-6 interrupt autovector row (vector 30, offset `$078`)
  and the repository's existing `M2` citation (vectors 0/1); the low-numbered
  exception rows (2-15, including vector 5) are on the same table, ahead of
  the autovector rows. Exact printed/PDF page for the vector-5 row is
  recorded by the implementation stage against the archived manual.
- **M68000PM/AD Rev. 1, §6.3.1 "Exception Processing Sequence" / §6.3.6
  "Zero Divide, CHK, and TRAPV Instructions":** divide-by-zero is a
  synchronous exception processed immediately after the faulting
  instruction, using the standard basic exception frame (SR then PC on the
  supervisor stack, S=1, T=0) -- the same architected sequence already cited
  in ADR 0020 §6/§7 for interrupt entry; exact printed/PDF page recorded by
  the implementation stage.

## Consequences

- `MULU.W`, `DIVS.W`, `DIVU.W` are added alongside the existing `MULS.W`,
  closing the base-MC68000 multiply/divide family for the evidenced
  register-direct/immediate source-EA set.
- A generic, minimal synchronous-exception-entry seam exists, reusable by
  any future strictly-synchronous, unmasked, unscheduled MC68000 exception
  that needs only "push frame, jump to build-resolved handler, RTE returns"
  -- without widening this task's own scope to cover any of them.
- `m68k_discovery_max_instructions` remains exactly `256U`.
