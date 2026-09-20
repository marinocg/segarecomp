# ADR 0020: Deterministic VBlank / MC68000 IRQ6 Interrupt Progression

- Status: Accepted
- Date: 2026-09-03
- Relates to: ADR 0007 (generated-runtime loop-progress watchdog and the
  `genesis_note_loop_backedge` seam and its shared, never-replenished
  `progress_credit` / `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` ceiling), ADR 0016
  (bounded static finite-loop progress proof), ADR 0017 (bounded generated
  data-transform progress proof and its "ADR 0007 extension" precedent), ADR
  0018/0019 (data-transform progress scope identity and finite-region cursor
  progress), ADR 0013 (discovery-prefix boundary and runtime-confirmed
  expansion, its seed set `S` and `m68k_discovery_max_seed_entries == 4`
  ceiling), ADR 0002 (static translation and fail-closed execution), and the
  persistent Genesis device-state and checkpoint-evidence contract
  (SEG-007-T042).
- Amends in place (living architecture contract, not a numbered ADR):
  `docs/architecture/genesis-persistent-device-state-and-checkpoint-evidence-contract.md`
  §§3, 4, 7, and 13.2 — see §1 below. The T042 document text itself is edited so
  no contradictory normative text remains in the repository.
- Extends, without editing its file (numbered-ADR extend-don't-edit precedent,
  ADR 0017's own "Watchdog semantics (ADR 0007 extension)" section): ADR 0007's
  watchdog progress-source accounting — see the "ADR 0007 extension" section
  below.
- Does not amend, and does not relitigate: ADR 0002's static-dispatch boundary
  (no target-byte fetch, no runtime decoder, no interpreter, no JIT, no second
  dispatcher), the wire report schema, ADR 0007's no-progress window `W` /
  `dispatch_budget` value or its `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` ceiling
  value, ADR 0013's four-round / four-seed constants or drive ceiling, ADR
  0016/0017/0018/0019's existing loop-progress / data-progress proof
  mechanisms, the checkpoint-evidence bundle schema (T042 §§12–18), or the
  `MOVE An,USP` / `MOVE`-to-`SR` deferred-privilege compatibility policies
  (which stay scoped to those two instructions only).

## Context (evidence pointer, not itself evidence)

SEG-007-T158 (done) proved, via two byte-identical authorized-ROM runs plus
provenance/attribution confirmation, that the current generated-native Sonic
Phase-B terminal (`VBLANK_WAIT_WITHOUT_IRQ_PROGRESSION`) is a tiny stable CPU
spin on a work-RAM flag, waiting for VBlank, with the MC68000 SR interrupt mask
open (permits level-6 delivery), zero device-register access anywhere in the
wait window, and no admissible ADR 0016/0017/0018/0019 progress shape.
`runtime/genesis/runtime.h`'s `GenesisInterruptState` and
`runtime/genesis/runtime.c`'s `genesis_runtime_drive` were independently
inspected and confirmed to contain no interrupt-pending admission check, no
vector-dispatch route, no MC68000 exception-frame construction, and no RTE
lowering anywhere in the current generated-dispatch architecture.

An access-caused-only VBlank trigger (T042 §13.2's prior premise) can never fire
for this program shape because the wait window performs no device access at all.
The resolution is a device-owned, program-independent VBlank scheduler that
advances at the dispatcher's own block boundary, SR-masked IRQ6 admission inside
the existing finite dispatch loop, a build-time-resolved IRQ6 autovector target,
supervisor-mode-only exception entry, and RTE — all reusing the existing
`GenesisControlTransfer` / `genesis_runtime_drive` control-transfer
architecture, with no second dispatcher, no runtime opcode fetch/decode, and no
Sonic-specific program recognition.

## Decision

### 1. Amendments to T042 §§3, 4, 7, 13.2, and to ADR 0007's watchdog progress-source accounting

Four locations in
`docs/architecture/genesis-persistent-device-state-and-checkpoint-evidence-contract.md`
have their committed text edited in place (not merely referenced) so no
contradictory normative text remains once this task merges:

- **§3 (mutation/commit rules).** A third named mutation category is added: an
  explicit, deterministic, per-dispatch-step scheduling tick performed directly
  by `genesis_runtime_drive` itself (never by a generated block, never by
  `genesis_route_access`), independent of any particular bus access. The
  scheduler counter is `GenesisRuntime`-level production-runtime state, declared
  outside `GenesisDeviceState` entirely (§2 below); this amendment does not make
  it an evidence-bearing field. The only thing this category may do to
  `GenesisDeviceState` proper is, on rollover of that `GenesisRuntime`-level
  counter, set exactly the two named interrupt-device fields
  (`vblank_pending`, `vblank_transition_count`) that §4's amendment permits. It
  never generalizes to "the runtime may mutate device state for any reason."

- **§4 (Allowed / Forbidden device advancement).** A third Allowed primitive is
  added, distinct from the access-counting device-step counter: a
  **dispatch-step scheduling tick** — a counter advanced by exactly one per
  `genesis_runtime_drive` loop iteration (never per CPU instruction, never keyed
  to PC/opcode/instruction identity, never counting a specific register's
  accesses), owned by `GenesisRuntime` itself, whose rollover may set the two
  named interrupt-device fields under §3's gating condition. The Forbidden list
  is unchanged verbatim. A worked counterexample is added paralleling §4.4:
  **Forbidden** — "raise `vblank_pending` after Sonic's wait loop has spun N
  times"; **Allowed** — "raise `vblank_pending` after N `genesis_runtime_drive`
  dispatch-loop iterations, unconditionally, identically for every generated
  program."

- **§7 (interrupt request/delivery boundary).** Two amendments:
  1. **Request-side.** A pending flag may also be set by "the §4-amended
     dispatch-step scheduling tick." Admission is evaluated inside
     `genesis_runtime_drive`, strictly before the ADR 0007
     no-progress/watchdog-termination check for that same dispatch step (§5
     gives the exact ordering). Delivery still reuses `GenesisControlTransfer` /
     `genesis_runtime_drive` with no second control-transfer mechanism.
  2. **Acknowledgment-side.** §7's prior sentence ("Acknowledgment ... never an
     implicit clear on delivery") is itself amended: successful eligible IRQ6
     admission (§5 step 2) consumes the pending VBlank request and clears
     `vblank_pending = 0` as one of this ADR's explicitly named interrupt-state
     mutation sources; a masked/not-admitted interrupt leaves it pending,
     unclearable by anything else. This admission-clears rule **explicitly
     supersedes §7's prior "never an implicit clear on delivery" restriction for
     this IRQ6 mechanism only** and does not reopen or apply to any other
     acknowledgment path.

- **§13.2 (VBlank-onset transition trigger).** The rising-edge (set-to-1) clause
  is amended to: "...only by a routed access, an access-caused state transition,
  or the §4-amended dispatch-step scheduling tick (SEG-007-T047)." The
  rising-edge arithmetic (`vblank_transition_count` advancing by exactly one per
  rising edge, and at no other time) is unchanged. §13.2's clearing-to-0 clause
  still reads "only by the explicit, named acknowledgment side effect §7 already
  requires" verbatim — but that §7 side effect is itself amended above to
  include IRQ6 admission; §13.2 remains accurate only because it defers to §7 by
  reference and must not separately assert the acknowledgment mechanism is
  unchanged.

T042 §7's device-ownership rule and "no second control-transfer mechanism"
requirement remain preserved, not reopened, throughout all four amendments.

#### ADR 0007 extension (watchdog progress-source accounting — extended, not edited)

Per this repository's extend-don't-edit precedent for numbered ADRs (ADR 0017's
own "Watchdog semantics (`genesis_runtime_drive`, ADR 0007 extension)"
section), ADR 0007's file text is not rewritten. This ADR documents the
generalization here:

ADR 0007 (as already generalized by ADR 0017/0019) documents the shared,
never-replenished `progress_credit` / `credit_consumed` pool as having exactly
**two** progress sources — proven finite-loop back edges (loop-progress) and
proven data-transform cursor advances (data-progress) — under an "at most one
shared credit unit per dispatch step" rule.

**This ADR adds a third bounded consumer of that exact same finite,
never-replenished global `progress_credit` pool: a successfully admitted
asynchronous device interrupt (§5 step 2's accounting rule).** ADR 0007's
watchdog semantics are not wholly unchanged by this task; this extension is
stated plainly. ADR 0017/0019's own loop/data accounting is otherwise
unchanged; only the existing "at most one shared credit per dispatch" rule is
generalized to cover all three sources together, never per-source.

Architectural argument: recurring VBlank delivery is not itself a proof that the
generated program terminates; an unbounded sequence of IRQ admissions must not
reset the watchdog window `W` for free. Finite, non-renewable shared credit —
the same pool ADR 0007 already established, not a new one — bounds repeated
device-event progress exactly as it already bounds repeated loop/data progress.
Once that credit is exhausted, recurring IRQ delivery alone can no longer keep
`genesis_runtime_drive` alive indefinitely: the run still deterministically
reaches the existing `GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED` fail-closed
terminal.

No second device-event credit pool, no new total-dispatch cap, and no larger
`GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` ceiling are added — the existing single
pool and the existing `W` / `dispatch_budget` are reused unchanged (§10).

### 2. Device-owned deterministic VBlank scheduling

The scheduler counter is **not** added to `GenesisInterruptState` (which is
embedded by value in `GenesisDeviceState`, in turn embedded by value in
`GenesisDeviceEvidence` — adding a field there would silently change the
checkpoint-evidence bundle schema, contradicting §10). Instead one new
production-runtime-only scheduler field is added **on `GenesisRuntime`
directly, as a sibling of `devices`** — mirroring the existing placement of
`GenesisRuntime.loop_progress` and `GenesisRuntime.data_progress`, which are
dispatcher-loop bookkeeping, never device-observable/evidence-bearing state:

```c
typedef struct GenesisInterruptScheduler {
  uint32_t dispatch_steps_since_vblank;   /* SEG-007-T047: advanced by exactly one per
                                              genesis_runtime_drive dispatch-loop iteration,
                                              never per CPU instruction and never keyed to any
                                              PC/opcode/loop shape. Production-runtime-only:
                                              never a GenesisDeviceState member, never
                                              evidence-bearing. */
} GenesisInterruptScheduler;
```

`genesis_runtime_drive`'s own `for (;;)` loop increments this counter by exactly
one at a single fixed point per iteration: after the existing loop-progress /
data-progress bookkeeping for that step has computed the step's `made_progress`
value, but strictly **before** the existing final no-progress/watchdog check for
that same step (§5). On reaching a new named bounded constant
`GENESIS_VBLANK_DISPATCH_STEP_PERIOD`, it resets to 0 and, if VDP
interrupt-enable gating (§3) permits, sets
`runtime->devices.interrupt.vblank_pending` from 0→1 and increments
`vblank_transition_count` per T042 §13.2's existing rising-edge rule. Only those
two already-existing, already-evidence-bearing fields are touched by rollover;
the scheduler counter itself is never evidence. This is recurring: after
acknowledgment/re-arm (§3), the same counter keeps advancing and can raise
`vblank_pending` again on a later period.

This is a **bounded project compatibility policy**: exact Genesis VBlank cycle
timing is out of this milestone's scope. `GENESIS_VBLANK_DISPATCH_STEP_PERIOD`
is chosen small relative to the existing `dispatch_budget` no-progress window so
at least one VBlank onset is guaranteed well inside one watchdog window — no
larger than roughly `dispatch_budget / 8` for the `dispatch_budget` values this
project's driver already uses (the implementer confirms this against the actual
pinned driver value at implementation time). It is a uniform global scheduler
applied identically to every generated program; it is never tuned to the Sonic
loop. `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` and the `W` / `dispatch_budget`
no-progress-window semantics are unchanged.

### 3. VDP interrupt-enable gating and acknowledgment/re-arm

Gating uses the already-implemented, already-persistent VDP register state
directly — not unconditional enable. VDP register writes are already persisted
at `runtime->devices.vdp.registers[reg]` (the routed CONTROL-port register-write
path). Register #1 (Mode Set Register 2) already has one documented,
already-implemented, already-cited bit precedent in this codebase:
`docs/references/genesis-vdp-dma-contract.md` (citing GTO1, *Genesis Technical
Overview* v1.00, §7 register diagrams, pp. 36–38) establishes register #1's
DMA-enable bit, implemented today as `registers[1] & 0x10` (bit 4). The VBlank
interrupt-enable bit ("IE0") is the neighboring bit in the same register #1 —
bit 5 (`0x20`) per Sega's standard, widely corroborated VDP register map (GTO1
§7 register #1 diagram / its immediately preceding register-description pages;
the implementer records the exact page/row at implementation time — see the
Citations section and the task report for the confirmation status).

Gating is implemented as `(runtime->devices.vdp.registers[1] & 0x20U) != 0U`,
exactly mirroring the existing DMA-enable precedent's bit-test shape. §5's
scheduler-rollover admission of a new `vblank_pending` rising edge additionally
requires this bit to be set. No new VDP capability is required. Gating is never
implemented as unconditional enable; the fallback project-compatibility-policy
branch is reserved only for the genuinely impossible case where independent
citation cannot be confirmed at all.

Acknowledgment/re-arm: `vblank_pending` clears to 0 as part of the
interrupt-admission/delivery step itself (§5) — admission consumes the pending
flag synchronously when it delivers. This is exactly the T042 §7
acknowledgment-side amendment in §1: admission-clears supersedes §7's prior
"never an implicit clear on delivery" restriction for this mechanism only. No
real MC68000 program instruction and no RTE-based implicit acknowledgment is
required for this bounded milestone.

### 4. SR interrupt-mask eligibility

Eligibility uses the existing `GenesisRuntime.sr` interrupt-mask bits (SR bits
10–8, the I2/I1/I0 field per the standard MC68000 SR layout) compared against
interrupt priority level 6: IRQ6 is eligible whenever the current SR mask level
is `< 6` (masks 0–5 permit; mask 6 blocks level-6 and lower; mask 7 blocks
everything except level 7). This is standard architected MC68000 semantics, not
a project policy. Cite *M68000 Family Programmer's Reference Manual* (Motorola,
M68000PM/AD Rev. 1), the same manual already cited as `M1`/`M2`/`M3` in
`docs/references/genesis-reset-image-contract.md`; the exact section/page for
the interrupt-priority-mask comparison rule is recorded at implementation time
(see Citations).

### 5. Interrupt admission point and deterministic ordering in `genesis_runtime_drive`

Admission happens inside the existing `for (;;)` loop in
`genesis_runtime_drive`, inserted at exactly one point: **after** the existing
per-step handling for a `GENESIS_CONTINUE_AT_PC` result has run the
checkpoint-entry check, assigned `runtime->pc = result.next_pc`, and evaluated
the existing loop-progress / data-progress notes (which compute this step's
`made_progress` / `credit_consumed`) — but **strictly before** the existing
final watchdog-termination check for that same step.

**This ordering is load-bearing and is the single most important correctness
property of this section:** admission must run — and, when shared credit remains,
must be able to set `made_progress = 1` for the *current* step — before that
same step's own final watchdog check, never on a later loop iteration. Placing
admission after the watchdog check would let the watchdog terminate the run on
precisely the step where an eligible interrupt was already due.

Complete per-step ordering: `dispatch` → existing loop-progress / data-progress
accounting → §2's scheduler tick / `vblank_pending` re-arm → post-RTE
admission-grace consumption (§9): if `resume_grace_pending` is set, clear it
unconditionally and decline admission for this one boundary → IRQ eligibility
checks (`vblank_pending`, SR mask < 6, handler present) → IRQ admission
(steps 1–4) → IRQ shared-credit accounting (step 2's rule) → the existing final
watchdog-termination check. The grace is a **true one-boundary grace**: it is
consumed on the very first scheduler/admission boundary after the RTE,
regardless of whether an IRQ is pending or the SR mask is open on that boundary,
so it can never survive to a later boundary and decline a genuinely new VBlank
rising edge. An RTE whose restored SR mask is `≥ 6` (the nested-handler case)
still consumes its grace on that same boundary — the consumption point is
reached before the mask check.

At the insertion point, in order:

1. Advance the device scheduler (§2): increment
   `runtime->scheduler.dispatch_steps_since_vblank`; on period rollover, reset
   to 0 and, if gating (§3) permits, set `vblank_pending = 1` and increment
   `vblank_transition_count` (T042 §13.2's rising-edge rule, unchanged). This
   scheduler tick and the `vblank_pending` re-arm are **unconditional** — they
   run even while an IRQ6 handler is executing, so VBlank keeps advancing.

1a. **Post-RTE admission grace (true one boundary).** A successful exception
   return (§9, RTE — the atomic-commit path only) sets
   `runtime->scheduler.resume_grace_pending = 1`. Immediately after the scheduler
   tick / `vblank_pending` re-arm of step 1, and **before** any `vblank_pending`
   / SR-mask / handler-present eligibility check, if `resume_grace_pending` is
   set then clear it **unconditionally** and decline IRQ6 admission for exactly
   this one boundary (no frame push, no CPU/SR/PC/A7 mutation). The next
   drive-loop iteration therefore dispatches the restored PC at least once, and
   after that ordinary pending/mask/admission behaviour resumes with no
   surviving grace. The grace is **not** persisted until the next
   otherwise-eligible interrupt: if no IRQ is pending on this boundary it is
   still consumed, so a stale token can never later suppress a genuinely new
   VBlank rising edge.

   This grace is an **explicit, bounded project compatibility policy**, using
   the same framing §2 already uses for the coarse dispatch-step VBlank
   scheduler — **not** an architected MC68000 rule. The *M68000 Family
   Programmer's Reference Manual* states only that pending interrupts are
   detected between instruction executions and that an eligible pending
   interrupt may begin exception processing before the next instruction
   executes. The grace exists solely because this milestone uses a coarse
   dispatch-step-based synthetic VBlank scheduler rather than cycle-accurate
   device timing; its only purpose is to stop that synthetic scheduler from
   starving the resumed generated instruction stream when its artificial period
   expires during a long handler.

   The grace consumes **no** `progress_credit` and touches **no**
   evidence-bearing state (`resume_grace_pending` is a `GenesisRuntime`-level,
   production-runtime-only sibling of `dispatch_steps_since_vblank`, never a
   `GenesisDeviceState` / `GenesisInterruptState` / checkpoint-evidence member).
   An RTE that restores SR mask `≥ 6` consumes the grace on that same boundary
   (the consumption point precedes the mask check). Nested case: an inner RTE
   returning into a still-masked outer handler consumes its grace on the
   inner-RTE boundary (the very next dispatch step, still inside the outer
   handler); the outer RTE later sets and consumes its own grace on its own
   boundary — each RTE still guarantees ≥ 1 resumed dispatch step before the
   next IRQ6 admission.
2. If `vblank_pending == 1` **and** SR eligibility (§4) holds: perform interrupt
   admission — construct the exception frame (§7), save/replace SR (raise mask
   to 6, clear trace bit T, set supervisor bit S=1), set `runtime->pc` to the
   statically-resolved handler entry (§6, overriding the step's earlier
   `runtime->pc = result.next_pc`), and clear `vblank_pending` (re-arm, §3).
   Exception-frame construction failure (§8) instead produces a normal
   `GENESIS_STOP` result via the existing `GenesisControlTransfer` /
   `GenesisRuntimeStop` shape, using the existing unused
   `GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT = 6` stop class
   paired with a new `GenesisDiagnosticCategory` enumerator appended as the next
   unused integer (T042 §2.2 append discipline) — never a partially-mutated CPU
   state.

   **Watchdog credit accounting for a successfully admitted interrupt (not free
   progress):** delivery itself always happens once admission is eligible;
   whether it also counts as watchdog progress for *this* step is governed by
   the same shared, never-replenished `progress_credit` / `credit_consumed`
   accounting the loop-progress / data-progress evaluation already uses,
   generalized to a third consumer:
   - If this dispatch step already consumed the one shared credit unit
     (`credit_consumed == 1`): the admitted interrupt shares that already-paid
     step — `made_progress` is already `1`, and no additional credit is
     consumed.
   - Else if `progress_credit != 0`: decrement `progress_credit` by one, set
     `credit_consumed = 1`, set `made_progress = 1` for this step.
   - Else (`progress_credit == 0`): the admitted interrupt does not manufacture
     watchdog progress. `made_progress` is left as the loop/data evaluation set
     it; `steps_since_progress` is not reset by this admission.
   No dispatch step ever consumes more than one shared credit unit. Interrupt
   delivery, the exception frame, SR/PC/A7 mutation, and `vblank_pending`
   re-arm are never conditioned on `progress_credit` — a due, eligible
   interrupt is always actually delivered.
3. Else if `vblank_pending == 1` but SR eligibility does not hold (masked):
   leave it pending, do **not** set or influence `made_progress` for this reason
   alone, and let the existing final watchdog check run exactly as today — so an
   undeliverable/masked/persistently-pending interrupt cannot indefinitely
   suppress or fake ADR 0007 termination.
4. Else (`vblank_pending == 0`): do nothing further; the existing final watchdog
   check runs exactly as today.

No second control-transfer mechanism: interrupt delivery goes through the
existing `runtime->pc = <handler entry>` assignment the drive loop already
performs, and the next `dispatch(runtime)` call dispatches into the
already-generated handler block exactly like any other generated block.

### 6. Build-time IRQ6 vector resolution and generated-handler ownership

Cite *M68000 Family Programmer's Reference Manual* Table 6-1 "Exception Vector
Assignments" (the same manual/table already cited as `M2` for vectors 0/1) for
the "Level 6 Interrupt Autovector" row: vector number 30, byte offset
`30 * 4 = 0x78` from the vector-table base. The implementer re-verifies this row
and records the citation (see Citations).

Genesis cartridge ROM is mapped at address 0 (existing project fact). The
static-discovery / CPU frontend gains a build-time-only typed fact: the long
word at vector-table offset `0x78` is read from the mapped cartridge image
exactly as the reset-vector SP/PC at offsets 0/4 already are, and treated as a
new discovery root. `M68kDiscoveryTargetRole`
(`include/segarecomp/cpu/m68k/static_discovery.hpp`) gains a new
`interrupt_vector` role (or a small parallel typed root/fact struct alongside
`M68kStaticDiscoveryResult`'s existing fields — implementer's exact naming, but
it must be a typed, statically retained, build-time-resolved fact, never a
runtime fetch), so the existing discovery/CPU frontend admits, decodes, and
lowers that target through the existing C4/codegen pipeline exactly like any
other discovered block entry. The runtime interrupt-admission step in §5 then
only assigns `runtime->pc` to that already-generated target address — no new
dispatch mechanism, no runtime instruction fetch/decode.

Fail-closed rule: an unmapped, ambiguous/conflicting-claim, or unrepresentable
(e.g. odd address) vector target fails the **build** (static discovery /
emission), never a silent runtime default — reusing the existing
`M68kMappingIssue` / `M68kDiscoveryIssue` typed-issue shape and the existing
odd-address/unmapped-target rejection precedent already established for
`direct_call`'s own admission rule.

**Discovery-root category: a separate bounded static hardware root, outside ADR
0013's seed set `S`, never counted against it.** ADR 0013 §7 defines
`S = {reset entry} ∪ runtime_confirmed_seeds`, with the single ceiling
`|S| <= m68k_discovery_max_seed_entries == 4` — preserved verbatim. The reset
entry itself occupies one of the four slots; Phase B may promote at most three
further runtime-confirmed addresses before `|S|` reaches 4 and
`expansion_seed_limit_reached` terminates further promotion.

The IRQ6 autovector target is **not a member of `S` at all.** It is not modeled
as a Phase B runtime-confirmed seed, never increments `seed_count`, is never
runtime-promoted, and does not change `m68k_discovery_max_seed_entries` or the
maximum size of `S`. It is a third, always-present kind of root — a **bounded
static hardware discovery root**, admitted unconditionally in Phase A, entirely
outside `S`'s bookkeeping — resolved at build time from the mapped cartridge
image's fixed vector-table offsets exactly as the reset PC is (`0x4` for the
reset PC, which seeds `S`'s single Phase A member; `0x78` for the IRQ6
autovector, which is not a member of `S`). "A bounded static root admitted
outside `S`" and "a member of `S`, runtime-confirmed or otherwise" are
different mechanisms with different ceilings and different admission timing.

#### Implementation note — vector-table gating and clean-handler wiring (SEG-007-T047)

The autovector is resolved only when the mapped cartridge image actually carries
a full MC68000 exception vector table: the image is at least `0x100` bytes and
the long word at image offset `0x4` equals the resolved startup entry (the same
reset-PC slot the reset entry is resolved from). This precisely mirrors "resolved
exactly as the reset PC at `0x4` is" and keeps synthetic code-only fixtures, and a
zeroed (never-installed) autovector slot, inert rather than failing the build. A
non-zero slot that resolves to an odd / unmapped / non-24-bit address still fails
the **build** closed via the `direct_call` `validate_m68k_static_call_target`
precedent.

The IRQ6 mechanism is wired into generated `main` (`runtime.irq6_handler_entry` /
`irq6_handler_present = 1`) only when the handler's own static walk terminated
cleanly within the bounded discovery prefix — every path ended at an RTE/RTS with
no unresolved-frontier `primary_issue` and no discovery-prefix truncation. A
handler that exhausts the **per-root `m68k_discovery_max_instructions` prefix**,
or reaches an unsupported instruction, is a **distinct, bounded successor
frontier** — "the IRQ6 autovector handler exhausts its per-root
`m68k_discovery_max_instructions` prefix" — normalized as
`async_static_root_prefix_exhausted`. `m68k_discovery_max_blocks` is **not**
part of this: it was retired as an independent discovery admission gate
(ADR 0010 / 0011 / 0015; `src/cpu/m68k/static_discovery.cpp`, where
`m68k_discovery_max_instructions` remains the single per-root resource ceiling),
so the blocker is precisely per-root instruction-prefix exhaustion and nothing
else. Folding a prefix-truncated partial handler exploration into the reset
analysis would violate the reset walk's own reachability / frontier-exit
invariants, so until a separate decision extends static discovery into
interrupt-handler bodies (a drive-ceiling / prefix-extension policy question,
explicitly out of scope here per the Non-goals — the open full-refinement
question is bounded partial async-static-root discovery, unchanged), the
mechanism stays inert (`irq6_handler_present == 0`) for such a program —
deterministic and fail-safe, never a partial or unsound wiring.

### 7. Exception-entry semantics — supervisor-mode-only subset (explicit, bounded)

This project carries the SR S bit but never enforces privilege
(`docs/architecture/genesis-move-an-usp-startup-compatibility-policy.md`: the
`MOVE An,USP` and `MOVE`-to-`SR` deferred-privilege-check policies apply only to
those two instructions and do not extend further). **This is a new, separate,
explicit contract for interrupt entry / RTE — it does not inherit or extend
those two instructions' policies.**

Minimum correct MC68000 interrupt-entry transition (cite the manual's
documented interrupt-exception sequence — see Citations):

- Save the pre-exception PC (`result.next_pc`, the target the dispatcher was
  about to transfer to) and the complete pre-exception SR (captured before any
  mask/mode changes below).
- Stack pointer for the push: this task's bounded subset uses `runtime->a[7]`
  directly as the interrupt stack pointer, leaving the separate `usp` field
  (SEG-007-T085) untouched by interrupt entry / RTE. This project models no
  SSP/USP split.
- Frame shape: the plain MC68000 (not 68010+) basic exception stack frame —
  6 bytes total, SR (16 bits) at the lower address, then PC (32 bits) at the
  next address, no format/vector-offset word. Push order: decrement A7 by 6
  total, then write SR at `A7` and PC at `A7+2`, big-endian, through the
  existing routed-write boundary (`genesis_route_access`); the implementer
  chooses WORD-then-LONG, LONG-then-WORD, or two WORD writes, consistent with
  big-endian order and documented at implementation time. Reuse existing routed
  memory ownership, never a private interrupt-only memory implementation.
- SR update: interrupt-mask field set to 6, trace bit T cleared to 0,
  supervisor bit S forced to 1. If S was already 1 at admission time, this is
  not an observable mode change. **If S == 0 (user mode) is ever observed at
  admission time, this bounded subset fails closed** with the stop in §5 step 2
  — with a synthetic negative test proving this rejection.
- `runtime->pc` is set to the resolved handler entry from §6.

### 8. Exception-frame memory-safety / fail-closed ordering

**All fallible validation happens before the first frame-memory write; nothing
after the first write may fail.** Before issuing any frame write, compute the
target stack addresses (`A7-2`, `A7-4`, `A7-6` as applicable to the chosen
order) and validate the *complete* six-byte frame extent — every destination
address, alignment, and routing — through the existing routed-write path's own
address/region/alignment checks (either performing the equivalent validation
`genesis_route_access` would perform for each destination before issuing any
write, or structuring the sequence so `genesis_route_access`'s own fail-closed
behavior is consulted for every destination before the first write is issued;
never a parallel address-validation function). Only once complete-frame
validation confirms every write will succeed does the implementation issue the
actual writes and then commit SR/A7/PC.

**There is no permitted path in which the first frame-memory write is issued and
a later validation or write step can still fail.** If validation finds any part
of the frame unwritable (A7 wrap/underflow, unmapped/misaligned/write-prohibited
destination), the whole admission step fails closed **before any frame write
occurs at all**, with no A7/SR/PC mutation and no partial frame committed to
RAM. This is a validate-then-commit (precondition-check-first) sequence
exclusively — never an interleaved write-then-validate-the-rest ordering.

### 9. RTE

RTE is in scope for this task. `M68kInstructionKind::rte` is added (lowercase
snake convention, `include/segarecomp/cpu/m68k/instruction.hpp`) plus its
semantic-operation kind, effect metadata, decode, lift, and C4 / generated-C
lowering, consuming exactly the frame shape §7 constructs.

**Exact stack addressing (matches §7's push order):** §7 writes SR at the lower
address (`A7`, after decrementing) and PC at the next address (`A7+2`). RTE
reads from those same two addresses in the same order, before A7 moves: read the
saved SR from address `SP` (the current `runtime->a[7]`, before restoration) and
the saved PC from address `SP+2` — both routed reads (`genesis_route_access`),
neither committed to any runtime field yet.

**Atomic commit, not incremental restoration:** perform both routed reads first,
validating both succeed, without mutating `runtime->sr`, `runtime->pc`, or
`runtime->a[7]` in between or after only one of them. Only once both reads have
succeeded does the implementation commit all three together —
`runtime->sr = <read SR>`, `runtime->pc = <read PC>`, `runtime->a[7] += 6` — as
a single atomic step. If either routed read fails, the whole RTE fails closed
with no partial architectural restoration: `sr`, `pc`, and `a[7]` all remain
exactly as before the RTE was dispatched.

**Post-RTE admission grace:** on the successful atomic-commit path only, RTE also
sets `runtime->scheduler.resume_grace_pending = 1`. This arms a true one-boundary
IRQ6 admission grace (consumed unconditionally on the very next
scheduler/admission boundary, as described in §5 step 1a): the resumed
instruction stream is guaranteed to advance at least one genuine dispatch step
before the next IRQ6 admission. This is an explicit, bounded project
compatibility policy, not an architected MC68000 rule — the M68000 documentation
only guarantees that pending interrupts are sampled between instruction
executions. It is necessary because §2's coarse synthetic scheduler tick keeps
advancing (and can re-arm `vblank_pending`) while a handler longer than one
VBlank period runs, so without the grace the step that dispatches RTE would
immediately re-admit IRQ6 before the interrupted program executed a single
dispatch step. Because it is consumed unconditionally on the next boundary, it
never survives to decline a genuinely new, later VBlank edge. The grace consumes
no `progress_credit` and touches no evidence-bearing state; a failed RTE sets
nothing.

RTR / TRAP / TRAPV / ILLEGAL / general exception delivery remain explicitly out
of scope — only RTE, only for the exact frame shape this task's interrupt entry
produces.

### 10. Preserved invariants and checkpoint-evidence-schema boundary

ADR 0007's `W` / `dispatch_budget` no-progress window and its
`GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` ceiling value are unchanged — no second
credit pool, no new total-dispatch cap, no larger ceiling. What changes is the
*set of consumers* of the one existing pool: successful eligible IRQ6 admission
(§5 step 2) becomes a third bounded consumer alongside loop-progress and
data-progress, under the same "at most one shared credit unit per dispatch step"
rule, generalized across all three sources together. ADR 0013's
four-round/four-seed constants and drive ceiling are unchanged (§6). The
no-runtime-opcode-decode rule, the no-interpreter/no-JIT rule, no Sonic-specific
PC/address recognition, and no second control-transfer dispatcher all hold. The
RAM spin itself is never counted as progress. Once `progress_credit` reaches 0,
recurring IRQ6 admission still delivers real interrupts but can no longer reset
`steps_since_progress`, so a generated program whose only progress source is
repeated VBlank delivery still deterministically reaches
`GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED`.

**Checkpoint-evidence bundle schema boundary.** This task's new scheduler field
(and any other new `GenesisRuntime`-level scheduler-related storage) is declared
on `GenesisRuntime` as a sibling of `devices` — not inside
`GenesisInterruptState` / `GenesisDeviceState`, and therefore not reachable from
`GenesisDeviceEvidence` by construction. These fields are **production-runtime
state only**. They are not added to `GenesisDeviceEvidence`,
`GenesisCheckpointEvidenceBundle`, `checkpoint_evidence.h`, any
canonical-serialization/digest rule (T042 §14), or the independent-oracle schema
(T042 §16). Existing evidence-category comparisons/digests for already-covered
fields remain byte-identical for any input that does not exercise the new
interrupt mechanism. A later task that decides the bundle should observe
interrupt/scheduler state must do so explicitly — bumping the schema/options
version and updating T042 §14/§15 deliberately.

## Citations

- **M68000PM/AD Rev. 1 (1992), Table 6-1 "Exception Vector Assignments",
  §6.2 "Exception Vector Assignments":** Level 6 Interrupt Autovector = vector
  number 30, vector offset `$078` (`30 * 4`). This is the same Table 6-1 the
  repository already cites as `M2` (rows 0/1, printed p. 6-4 / PDF p. 166); the
  autovector rows (24–31, the seven autovector levels + spurious) continue on
  the same table. Exact printed/PDF page for the vector-30 row is recorded by
  the implementation stage against the archived manual.
- **M68000PM/AD Rev. 1, §6.3.9 "Interrupts":** an interrupt is processed only
  when its priority level is greater than the SR interrupt-priority mask (SR
  bits I2–I0), except level 7 which is non-maskable; interrupt exception
  processing stacks SR then PC on the supervisor stack, sets S=1, clears T, and
  sets the SR interrupt mask to the level of the interrupt being serviced. This
  is the standard architected sequence; exact printed/PDF page recorded by the
  implementation stage.
- **GTO1 (*Genesis Technical Overview* v1.00, 1991), §7, register #1 (Mode Set
  Register 2) diagram, pp. 36–38:** register #1 bit 4 = DMA enable (already
  cited and implemented as `registers[1] & 0x10`), bit 5 = IE0 = VBlank
  interrupt enable (`registers[1] & 0x20`). The DMA-enable bit citation is
  already established in `docs/references/genesis-vdp-dma-contract.md`; the IE0
  bit-5 row on the same register #1 diagram is recorded by the implementation
  stage.

## Consequences

- Generated-native Genesis execution gains a complete, bounded, deterministic
  MC68000 IRQ6 / VBlank interrupt mechanism and RTE, reusing the existing
  control-transfer architecture.
- The `VBLANK_WAIT_WITHOUT_IRQ_PROGRESSION` Sonic Phase-B terminal is resolved;
  repeated real IRQ6 admission + RTE cycles occur on that route.
- ADR 0007's watchdog gains a third bounded credit consumer; termination
  guarantees are preserved because the shared credit pool is finite and
  never replenished.
- The checkpoint-evidence bundle schema is unchanged; the new scheduler state
  is production-runtime-only.
- No interpreter, JIT, runtime opcode fetch/decode, second dispatcher, or
  Sonic-specific heuristic is introduced.
