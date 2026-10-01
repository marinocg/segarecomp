# ADR 0071: Z80 Bounded Multi-Entry Owners, Shared Effect Bodies and the Production Build Policy

- Status: Accepted (SEG-033). Amends ADR 0058 sections 1 and 3 (the representation of an owner, not its semantics) and records
  the production compile policy measured for the Master System route.
- Date: 2026-10-01

## Context

ADR 0058 adopted broad immutable-image AOT: every legal instruction start of every code image has an exact entry
`(image identity, logical address | window offset)` and a generated-native owner. The first implementation emitted one C
function per start, so a 512 KiB SMS image produced 525,312 host functions. The production route
(`segarecomp build`) measured on 2026-10-01 (SEG-033-T001) spent 96-98% of its wall time in the host C compiler; a real 512 KiB image
needed about 141 s at the default `-O2`, 4 workers, with a 116 MiB executable. The feasibility experiments behind ADR 0058
ran at `-O1`, 8 workers; the production defaults were `-O2` and 4 workers.

The generic exact-entry table (`CompiledEntryTable`) already maps many keys to one owner symbol (owner-ID compression) and
the generic `TranslationUnitSharder` already places whole functions into deterministic shards. The M68K AOT path groups up to
128 exact entries per owner on top of the same machinery. Z80 used neither property.

## Decision

1. **Bounded multi-entry owners.** Consecutive exact starts of one image and one owner class (absolute-PC or window-relative)
   share one host function of at most `kOwnerGroupEntries = 128` entries. The exact entry table is unchanged: every start keeps
   its own `(identity << 16 | key)` row, and rows of one group name the same owner symbol. The owner selects its entry from
   the PC the dispatcher (or a direct binding) stored in `state.pc`: `PC` for absolute-PC owners, `PC - window_base` (the image
   offset, the same key the dispatcher looked up) for window-relative owners. A selector the group does not own fails closed
   with `no_owner` and never lands on a nearby entry; the per-window-base variant logic of window-relative owners is
   unchanged inside each entry. The bound is a constant, not an option. A bound of 1 reproduces the historical emission byte
   for byte and is the reference mode of the differential gate.
2. **Direct chaining.** A directly bound fall-through to another entry of the same owner is a local `goto` to the entry label
   (the next entry's boundary prologue still runs: deadline and interrupt acceptance are unchanged). A direct binding to an entry
   of another owner returns that owner after storing the successor PC in `state.pc`, because the target now selects its entry
   from `state.pc`. Direct binding remains invariant-window to invariant-window only (ADR 0058 section 4).
3. **Shared effect bodies.** The effect of an instruction (R accounting, the lowered statements, Q, T-states) is emitted once as
   an external function `z80_fb_<n>(Z80Runtime *rt)` when its lowered text does not depend on the instruction's address
   (decided by lowering with two placeholder PCs and comparing text, which the lowering contract guarantees is exact), and
   every entry with identical text calls it. The entry keeps its own prologue and control-flow tail. Bodies live in their own
   translation-unit family, are numbered in plan order and are declared in block scope by the callers, so the shared header
   still declares no owner or body. A body function is external and never inlined across translation units; the measured runtime cost is within noise.
   It is a Z80-private mechanism of about 60 lines; it is not a framework and moves no CPU lowering into common code.
4. **Production compile policy.** Compile workers default to `min(8, hardware concurrency)` (was `min(4, ...)`); `--jobs`
   overrides it. The generated-code optimization level stays `-O2` for every unit: once owners are grouped and bodies shared,
   `-O1` saved at most 4% of compile time on real images (about 10% on synthetic random images) and made the generated program
   about 4% slower, so no generated/handwritten split exists. `--optimize 1` is accepted next to `0` and `2` (an explicit value
   applies to every unit) so the measurement is reproducible. The Genesis route keeps `-O2` and shares the worker default
   (measured: 43 s to 27 s for a real Genesis image, aggregate compiler RSS 1.2 to 1.8 GiB).

## Rejected alternatives

- Any bound other than 128: 16-32 left most of the benefit on the table (the C compiler produced about 30% more object code per
  entry); 64 and 128 are equivalent within noise and 128 sits away from the 32-64 cliff observed with Apple clang; beyond 128 there
  was no further gain.
- `-O1` for generated code (rejected, see 4). A split `-O1` generated / `-O2` handwritten policy (rejected for the same reason).
- Selecting by a runtime dispatcher or opcode decoding: not needed; the entry table already resolves the exact key.
- Reachability pruning, VSA, selective admission, lowering only executed starts: out of scope; broad AOT stays the reference.

## Consequences

- The measured figures (compile CPU, wall time, RSS, executable size, runtime) are recorded in the SEG-033 task records and
  protected by `sms_emission_budget_test.py` (structural shape invariants) and the extended build benchmark.
- `z80_owner_group_differential_test.py` proves every candidate emission identical to the reference on the complete legal-form
  matrix, all conformance scenarios and seeded random programs, for group bounds 1, 2, 3, 7 and 128 with and without shared
  bodies.
- A reader of generated C finds `switch (s->pc)` owners and `z80_fb_<n>` calls instead of one function per address.
  `z80_image_emitter` accepts the spec verbs `group <n>` and `share <0|1>` (and `sms_image_emitter` the matching options) so the
  reference emission stays reproducible.
