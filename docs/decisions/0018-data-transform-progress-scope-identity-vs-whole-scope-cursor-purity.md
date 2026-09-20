# ADR 0018: Data-Transform Progress — Scope Identity as a Partition Key, Not Whole-Scope Cursor-Write Purity

- Status: Accepted
- Date: 2026-09-02
- Relates to: ADR 0017 (bounded generated data-transform progress proof — this
  ADR refines its admission logic), ADR 0007 (generated-runtime loop-progress
  watchdog, no-progress window `W`, single never-replenished global
  `progress_credit`), ADR 0016 (bounded static finite-loop progress proof — the
  `M68K_LOOP_PROOF_MAX_*` constants), ADR 0009 (bounded retained static-analysis
  facts consumed by guarded C11 lowering), ADR 0006 (generic immutable
  cartridge-data region ownership), ADR 0013 (four-seed / four-round Phase-B
  discovery bounds).
- Does not amend, and does not relitigate: ADR 0007's no-progress window `W`, its
  `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` global never-renewable ceiling, or the
  `instruction_budget_exhausted` fail-closed stop; ADR 0016's admitted
  finite-control-loop family or the numeric values of its resource caps; ADR
  0013's four-seed / four-round Phase-B bounds; ADR 0002's static-dispatch
  boundary (no target-byte fetch, no decoder, no interpreter, no JIT, no second
  dispatcher); the wire report schema; the stop / diagnostic enums;
  `STOP_DIAGNOSTIC_PAIRS`; ADR 0006's cartridge-data ownership model. All
  per-writer semantic requirements of ADR 0017 are preserved verbatim.

## Context

SEG-007-T155 accepted and implemented ADR 0017 end to end (full gate 85/85;
independent adversarial PASS) and re-executed the authorized pinned Sonic Phase-B
route. The route terminal was **unchanged**: `instruction_budget_exhausted` at
`rounds = 4`, `seed_count = 4`, with **zero** `genesis_note_data_progress` calls
in the round-4 generated C.

With the corrected T155 producer (deterministic cyclic/SCC identity derived
*before* compatible-writer grouping; candidate cap counted against structurally
eligible scoped candidates; collision-free `uint32_t` proof-ordinal runtime
identity), the round-4 data-dependent decompression / bitstream-traversal loop
that reaches the ADR 0007 watchdog resolves — in normalized scope-shape classes —
to exactly **one** cyclic scope: a single strongly connected region of roughly 20
basic blocks / roughly 59 covered instructions, containing exactly **2 compatible
`(An)+` post-increment advancing writers on one shared output cursor** (consistent
with ADR 0017's own "separate `(a1)+` writers" note). That scope is rejected
**solely** because its block count exceeds `M68K_LOOP_PROOF_MAX_BLOCKS` (16) and
its covered-instruction total exceeds `M68K_LOOP_PROOF_MAX_INSTRUCTIONS` (32) —
normalized reason `scoped_cyclic_region_exceeds_frozen_ADR0016_scope_caps`. It is
not small and rejected for a different reason.

ADR 0017 as written applies the ADR 0016 caps, and a whole-scope
cursor-`An`-write purity scan, to the **entire derived cyclic scope**. Because the
Sonic decompressor's innermost cyclic region is a large multi-block SCC, that
couples ADR 0017 eligibility to the size of surrounding decompressor control flow
that has nothing to do with whether the output cursor advances soundly.

### Operator-supplied throwaway diagnostic experiment (evidence only)

A disposable spike (no commits, no PR, no repo history; fully reverted) was run
against the then-unmerged T155 branch. It removed (a) the whole-SCC
block/instruction cap rejection and (b) the whole-scope purity traversal, while
preserving exact per-writer semantic eligibility, compatible-writer grouping, the
incompatible-`An`-writer fail-closed guard, the shared proof ID, all
C4/generated-C/runtime guards, low-water semantics, and the unchanged
watchdog/global credit.

Observed result: `EXPERIMENT_WRITER_LOCAL_NOT_ADMITTED` — data-progress proof
count 0 before and after; zero generated data-progress calls; ADR 0007 terminal
unchanged. The spike detected roughly 7–9 real auto-updating `write_move`
advancers across rounds on multiple address registers in both directions/steps,
but every candidate failed because, once cyclic-scope confinement was removed, the
preserved incompatible-cursor-writer rule became **program-wide**: each candidate
architectural `An` was also written elsewhere in the aggregate by unrelated
`MOVEA` / `ADDA` / `LEA` / addq-family / reload operations, so no `An` was
globally pure and every advancing group was rejected.

### Precise interpretation

The experiment disproved the architecture **"program-wide compatible-writer
grouping + program-wide cursor purity"**. It established that *some*
locality/partitioning is required so unrelated uses of the same architectural
register do not invalidate each other. It did **not** establish that ADR 0017
must prove *whole-scope* cursor purity. "Writer-local data progress is disproven"
is a stronger claim than the evidence supports and is explicitly **not** adopted
here.

## The two responsibilities ADR 0017 conflated

ADR 0017 used the cyclic scope for two distinct purposes:

1. **Locality / identity.** Deciding which advancing writer operations belong to
   the same data-transform progress stream, so that unrelated loops that happen to
   reuse the same architectural `An` are not conflated into one `proof_id`. The
   experiment shows this **is** necessary.
2. **Whole-scope semantic purity.** Inspecting *every* operation in the cyclic
   scope and proving that no operation other than an admitted compatible advancing
   writer ever writes the cursor `An`. This is what couples ADR 0017 eligibility
   to `M68K_LOOP_PROOF_MAX_BLOCKS` / `_INSTRUCTIONS` and to arbitrary surrounding
   decompressor CFG size.

This ADR determines whether responsibility 2 is necessary for ADR 0007
soundness. It is not.

## Soundness analysis: is whole-scope cursor-write purity necessary?

The question is whether an **uninstrumented modification of the cursor `An`
between two valid instrumented writer observations** can violate ADR 0007
boundedness, given the current runtime protections (all unchanged by this ADR):

- the admissible region is the finite `GENESIS_REGION_SYNTHETIC_WORK_RAM`
  (`work_ram[65536]`);
- generated C emits a note only after routed-write success, after the single `An`
  commit, and only when the live work-RAM bounds / no-wrap / exact-auto-update
  guards all hold; `remaining_bound` is the direction-normalized always-decreasing
  quantity;
- the watchdog keeps one `data_slot_low_water` = the smallest `remaining_bound`
  ever seen for the active `proof_id`;
- progress is credited **only** for a matching-`proof_id` note whose
  `remaining_bound` is **strictly smaller** than `data_slot_low_water`; equal or
  larger is establish/replace or no-progress;
- each credited progress consumes exactly one unit of the single global
  `progress_credit` pool, which is **never replenished** and shared with the
  loop-progress family;
- the no-progress window `W` and the `instruction_budget_exhausted` stop are
  unchanged.

### Case A — cursor rewind / reset by an uninstrumented operation

Low water `remaining_bound = 1000`. An uninstrumented operation moves the cursor
backward. The next instrumented writer observes `remaining_bound = 1500` → not
strictly smaller than 1000 → **no progress**. The cursor returning to exactly 1000
→ equal → **no progress**. Only a future genuine write that produces
`remaining_bound < 1000` is ever credited. The low-water ratchet is monotone
non-increasing regardless of what any uninstrumented operation does to the cursor,
so a rewind or reset can **never** manufacture credit. Sound.

### Case B — cursor forward mutation by an uninstrumented operation

Low water `remaining_bound = 1000`. An uninstrumented operation advances the
cursor. The next instrumented writer observes `remaining_bound = 800` → strictly
smaller → credited; low water becomes 800; one global credit unit is consumed.

Can this cause unbounded execution? No:

- For one active `proof_id` slot instance, `remaining_bound` is confined to
  `[0, 65536]` and every credited step strictly lowers a monotone ratchet toward
  0. The number of credited steps for one slot instance is therefore at most
  `region_size / step` ≤ 65536 — a finite constant — whether each decrease is
  "earned" by a real instrumented write or "assisted" by an uninstrumented forward
  jump. Once the cursor reaches the region end the generated-C live guard emits no
  further note.
- Across re-entries / slot replacements, each newly established slot instance is
  independently bounded the same way, but **every** credited step also permanently
  consumes one unit of the never-replenished global `progress_credit`
  (`GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT`, shared with loop-progress). The total
  number of credited steps over the entire run is therefore bounded by that fixed
  global constant, after which no step is credited, `steps_since_progress` reaches
  `W`, and the run fails closed.

An uninstrumented forward mutation can at most cause the mechanism to spend
bounded global credit somewhat faster or slower; it cannot remove the termination
guarantee.

### Case C — uninstrumented incompatible writer of the cursor `An`

`MOVEA` / `ADDA` / `LEA` / addq-family / reload writing the same `An` between two
instrumented observations reduces to Case A (backward / reset → never credited) or
Case B (forward → bounded credit). Neither breaks boundedness.

The only thing a whole-scope purity scan would buy is **precision**: it avoids
spending bounded credit on a register that is being reused for a non-cursor
purpose. In the pathological case where a genuinely non-terminating loop contains
one qualifying `(An)+` writer plus an unrelated reload of that `An`, the worst
outcome is that the loop is granted up to the fixed global credit ceiling of
bounded credits before failing closed — it runs longer before the **same
deterministic `instruction_budget_exhausted` stop**. This is exactly the
"consequence" ADR 0017 already accepts for "two data-transform loops alternated by
an infinite outer loop": slot replacement lets each re-credit, but the global
never-replenished credit guarantees termination. That precision is not worth
coupling admission to surrounding CFG, and — critically — an *unknown* intervening
mutation is no more dangerous than a *known* arbitrary one: both reduce to Case A
or Case B. No counterexample distinguishes them, so no residual "unknown writer of
`An`" scan over non-member operations is retained.

### Repeated low-water observations

A matching-`proof_id` note whose `remaining_bound` equals the current low water is
not progress and does not consume credit; a strictly smaller one lowers the
ratchet and consumes exactly one unit. No sequence of equal or oscillating
observations renews anything.

### Finite shared global credit

`progress_credit` is decremented by at most one unit per dispatch across both note
families (deterministic loop-progress-before-data-progress order) and is never
replenished. This is the termination guarantee, and it is independent of whether
the surrounding cyclic scope is "pure".

### Conclusion

No concrete counterexample requires whole-scope cursor-`An`-write purity for ADR
0007 soundness. The termination guarantee is the finite-region low-water ratchet
per slot instance **plus** the finite never-replenished global credit ceiling.
Whole-scope purity is a false-positive-avoidance (precision) feature, not a
soundness requirement, and is not retained from conservatism alone.

## Decision

Adopt **scope identity as a deterministic partition key, with writer-local
semantic certification** for the ADR 0017 data-transform progress proof.

Pipeline over the final validated cross-seed aggregate CFG (unchanged from ADR
0017 §"Retained-fact lifecycle"):

1. Derive the same deterministic cyclic / SCC partition semantics established by
   the corrected T155 producer: SCCs partition the vertex set, scope identities are
   deterministic, and scopes are ordered canonically by minimal entry. The
   implementation may replace repeated forward/backward reachability with a
   deterministic aggregate-linear SCC algorithm (for example Tarjan / Kosaraju-
   equivalent semantics) as required by the ADR-0018 aggregate-derived work bound.
   This partition is retained **only** as a partition key.
2. Within each derived scope, enumerate the advancing generated writers.
3. Partition / group those writers by
   `scope_identity + cursor_An + direction + fixed_step + progress_region`.
4. Validate **each writer operation independently** against the retained
   aggregate. Per-writer requirements are **unchanged** from ADR 0017: exact
   provenance / IR operation identity; generated **routed** write; decoded `(An)+`
   or `-(An)`; fixed direction; fixed step (decoded access width, with the
   A7-byte step-by-2 rule); complete CPU-owned `M68kOperationEffect` register-write
   footprint for that exact writer (with the `An` auto-update in
   `address_register_write_mask`); `GENESIS_REGION_SYNTHETIC_WORK_RAM` as the only
   admissible region; independent C4 revalidation of that member; successful routed
   write strictly before the note; live work-RAM bounds and no-wrap; exact
   architectural auto-update commit. Unknown / unsupported operations are never
   instrumented.
5. Assign every compatible member of one group a single shared deterministic
   `proof_id` (the collision-free `uint32_t` proof ordinal). Per-writer `proof_id`
   assignment remains forbidden.

**Scope identity prevents** unrelated program uses of the same architectural
register in **disjoint** scopes from colliding on one `proof_id` or feeding
inconsistent `remaining_bound` streams into one runtime slot — the failure mode
the throwaway experiment reproduced when locality was removed entirely.

**ADR 0017 no longer requires** that every one of the surrounding instructions in
a cyclic scope be proven free of cursor-`An` writes, no longer gates data-progress
admission on the block count or covered-instruction total of the surrounding
decompressor control flow, and performs **no** footprint-completeness check on any
operation other than a proposed proof member.

### Resource bounds are aggregate-derived, not ADR 0016 caps repurposed

ADR 0018 exists precisely because surrounding CFG size is irrelevant to
writer-local data-progress soundness, so it does **not** reinterpret
`M68K_LOOP_PROOF_MAX_BLOCKS`, `M68K_LOOP_PROOF_MAX_INSTRUCTIONS`,
`M68K_LOOP_PROOF_MAX_CANDIDATES`, or any other ADR 0016 shape cap as an ADR 0018
maximum writer / member / group / proof / "scopes examined" count.

The final validated Phase-B aggregate is already finite and bounded by the
existing discovery architecture (ADR 0013). ADR 0018 uses that natural aggregate
bound directly and deterministically:

- SCC / scope-identity count ≤ retained blocks;
- eligible advancing-writer count ≤ retained operations;
- group count ≤ eligible writers;
- proof count ≤ groups;
- traversal work proportional to retained `blocks + edges + operations`.

SCCs and scope identities are derived, and eligible writers enumerated, across the
**complete** retained aggregate. Unrelated earlier SCCs or writer groups must
never crowd out a later valid proof by address / order traversal;
`M68K_LOOP_PROOF_MAX_CANDIDATES` is not applied to "scopes examined" (that would
reproduce the previously-fixed T150 candidate-starvation class).

`m68k_prove_finite_loop_progress` and every ADR 0016 constant / semantic are left
**completely unchanged**. No cap increase (16 → 32, 32 → 64, or any constant
fitted to the observed ~20 / ~59 scope shape) is adopted. If a fixed-size data
structure genuinely requires an explicit ADR 0018 representation / work capacity,
it is defined as an ADR-0018-specific bound with an independent generic
justification recorded here — never by aliasing an ADR 0016 cap's meaning and
never a value chosen to fit Sonic.

### Incompatible cursor writes inside an admitted scope

An operation inside an admitted scope that writes the cursor `An` but is not an
admitted compatible advancing writer no longer invalidates the proof by any means.
It is not instrumented, it emits no note, and it is **not** footprint-checked. Its
effect on soundness is bounded exactly as analyzed above (Cases A–C): a rewind /
reset is never credited; a forward mutation is credited only within the per-slot
finite-region ratchet and the finite global credit ceiling; an *unknown*
intervening mutation is no more dangerous than a *known* one. The only
footprint-completeness requirement is on the candidate writer itself (Decision
step 4): an advancing operation proposed as a proof member whose
`M68kOperationEffect` register-write footprint is incomplete / unknown is rejected
as a member, because only then can generated C guarantee that the committed cursor
equals the pre-access cursor adjusted by the decoded step.

## Consequences

- The Sonic round-4 decompression loop's single large cyclic scope is no longer
  rejected for exceeding `M68K_LOOP_PROOF_MAX_BLOCKS` / `_INSTRUCTIONS`; its 2
  compatible `(An)+` writers are certified writer-locally and share one
  `proof_id`, so generated round-4 C can emit `genesis_note_data_progress` and the
  ADR 0007 watchdog can observe genuine finite forward motion through work RAM.
- A genuinely non-advancing transform, a reset / rewound cursor, a region escape,
  a wrap, repeated same-offset writes, and unbounded re-entry all still stop
  deterministically at the unchanged `instruction_budget_exhausted` stop, bounded
  by the unchanged never-replenished global `progress_credit`.
- ADR 0007's window `W` and global ceiling, ADR 0016's admitted family and the
  numeric values of its caps, ADR 0006's cartridge-data model, ADR 0002's
  static-dispatch boundary, the wire schema, the stop / diagnostic enums, and ADR
  0013's four-seed / four-round Phase-B bound are all untouched.
- All T155 downstream machinery is preserved: the CPU producer and typed
  `M68kGeneratedDataProgressProof`, the `write_move` footprint-completeness entry,
  the retained typed field on `FrontendAnalysis` /
  `FrontendPartialProgram.accepted_prefix`, C4 per-member revalidation, the guarded
  `genesis_note_data_progress` codegen path, the `GenesisDataProgressNote` runtime
  ABI, and the ADR 0007 watchdog `data_slot`. Only the static admission logic
  changes: whole-scope caps + whole-scope purity + residual non-member
  footprint-completeness scan → writer-local certification with scope identity as a
  partition key and aggregate-derived enumeration bounds.
- A later task wanting a different progress family (a computed-index scatter
  cursor, a second admissible region, a non-`An` cursor) must still emit its own
  CPU/codegen-proven typed note through the same seam and existing semantic owner,
  or record why that is unsound; it must not reintroduce a fixed total cutoff, a
  runtime opcode decoder, or program-wide register grouping.
- If, after SEG-007-T156 implements this ADR and re-executes the Sonic Phase-B
  route, the next terminal is a fresh `discovery_prefix_boundary` that ADR 0013
  cannot promote because the four-seed / four-round bound is exhausted, that is a
  separate executed Phase-B scalability frontier requiring its own full refinement
  (monotonic accumulated discovered-program state, not more retained seeds); it is
  explicitly not a `4 → 8` retune inside the implementation task.
