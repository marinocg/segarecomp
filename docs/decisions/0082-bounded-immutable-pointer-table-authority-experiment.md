# ADR 0082: Bounded Immutable Pointer-Table Authority (Experiment): STOP

- Status: Accepted (decision: **STOP**. No authority was built; the experiment is closed at its inventory gate.)
- Date: 2026-10-05
- Task: SEG-035-T001..T004 (one combined, report-only delivery).
- Related, unchanged: ADR 0079 (analysis instantiation), ADR 0080 (hybrid admission; decision 3 is the operand-width rule),
  ADR 0081 (the avenue this ADR evaluates), ADR 0054 / 0055 (historical evidence only), ADR 0051 (width regions snowball).

## Question

ADR 0081 left one avenue open: *bounded immutable pointer-table authority*. Can an indirect M68K dispatch whose target is read from
an immutable table be resolved to a sound finite target set when the **index is bounded by program facts**, never by operand width,
and does that remove enough whole-image fallback pressure to justify a production selective-AOT milestone?

## Proof contract (what would have been credited)

A table-derived target set is credited only when every component is independently proven:

| component | accepted authority | rejected |
| --- | --- | --- |
| table base | exact PC-relative / absolute / `LEA` of a proven immutable address, or an exact pointer from authoritative immutable bytes | unknown, width-derived, runtime-observed |
| index domain | exact finite set, a mask that cuts the maximum, a dominating guard that cuts the maximum, add/sub/shift of a bounded value, a complete finite memory cell from the SEG-030 memory domain | `width_only_domain` (a byte is not 0..255, a word is not 0..65535), a one-sided guard whose other bound is the width |
| access extent | the exact finite access-address set that follows from base + domain + scale + entry width | labels, adjacent data/code, disassembly shape, external metadata |
| entry reads | every possible read from immutable image bytes | any read leaving the image, mutable, or aliased: the whole site stays unresolved |
| targets | every target mapped, even, valid under the existing M68K rules (an odd target raises an address error and is excluded exactly as today) | any unmapped or invalid target fails the whole site; no partial set |
| enumeration | the existing finite-set bound; exceeding it is `Unknown(resource_bound)` | truncation |

Invariant, kept explicit: **width is not authority.** Runtime coverage is a falsifier only.

## Method (sanitized; exact addresses stay in ignored private artifacts)

1. The SEG-034 planner (`--hybrid-plan`, Release, `main` at `28ff66a`) was rerun on Sonic 1, Sonic 2 and Cool Spot, credited and with
   the uncredited `--diagnostic-transparent-handlers` ceiling. The SEG-034 aggregates were reproduced exactly (13 / 5 / 22 credited
   whole-image triggers, 23 / 64 / 22 ceiling).
2. `--trace-points` now also writes every data-register slot and its CPU-owned `width_derived` flag (private output only; the aggregates,
   plans and private plan outputs of all six runs are byte-identical with and without it).
3. Every whole-image dispatch site was read in context with a local disassembler (private), and classified by the contract above.

## Findings

**1. The authority already exists for the program-bounded cases.** The SEG-030 finite-value analysis credits exact sets, masks that cut
the maximum, dominating guards, scaling, and immutable entry loads including the `MOVEA.L table(PC,Dn),An; JSR/JMP (An)` form, and
clears `width_derived` only when a bound cuts the maximum. In the ceiling run it resolves 5 PC-indexed sites on Sonic 1 (8, 25, 8, 58
and 5 targets) and 3 PC-indexed sites plus one `JMP (An)` pointer-table site on Sonic 2 (16, 8 and 4 targets). Nothing a new
"authority" would credit is missing there.

**2. Credited run: zero sites for a new authority.** The 6 explicitly bounded PC-indexed sites (Sonic 1: 4, Sonic 2: 2) that remain
whole-image are exactly the ones the analysis resolves in the ceiling run; they fail only because the register is Unknown across an
unproven interrupt boundary (`interrupt_resumption_unproven` / `invalidated`). That is a premise, not a table or index proof gap.
Cool Spot's 7 `(An)` sites have the same cause in the credited run.

**3. Ceiling run (downstream), unresolved dispatch sites by class** (non-return sites):

| class | Sonic 1 | Sonic 2 | Cool Spot |
| --- | --- | --- | --- |
| RAM byte index, no mask, no guard (`width_only`; PC-indexed two-level, or `(An)` object/routine tables) | 10 | 10 + 2 `(An)` | 4 (`(An)`) |
| one-sided guard, upper bound is the byte width | 1 | 0 | 0 |
| RAM word / unknown index (bit-loop counters of a decompressor, word read from RAM) | 0 | 11 | 0 |
| program-bounded but the access set fails target validation | 0 | 1 | 1 |
| mutable function-pointer cell (not a table) | 0 | 0 | 2 |
| `JMP (An)` continuation register (not a table) | 1 | 0 | 0 |
| **total unresolved non-return sites** | 12 | 24 | 7 |
| already resolved by the existing analysis | 5 | 4 | 0 |

No site has a bounded index and is blocked only by its base: every failure is the index domain (width or unknown). No index of the 26
width-only sites is bounded by a program fact: the object id, game-mode / routine counter and
sound-command bytes are RAM values whose only static bound is the width. This is the same classification ADR 0054 measured with
the strict challenger (9 width-only on Sonic 1 including the one-sided-guard byte; 1 target outside the image and 9 width-only /
13 unknown on Sonic 2), and ADR 0055 already stopped the store-provenance route for those cells; SEG-030's memory domain cannot give
them a complete finite value set because Unknown-target stores poison every cell.

**4. The only guard-bounded candidates fail target validation, and show the accidental-target risk.**
- Cool Spot: a pointer-table dispatch whose index is guarded below 32 and doubled (32 reads of a long at stride 2 from an exact
  immutable base). Of the 32 access addresses, 16 produce odd targets (address error, excluded exactly as today), 13 distinct valid
  targets, and **3 produce targets outside the image**. Under the fail-closed rule the whole site stays unresolved. The guard
  bounds the index soundly but is looser than the data's own invariant (even opcodes only), so the access set contains junk. Admitting
  the 13 would be plausibility filtering, which this experiment forbids.
- Sonic 2: a mask-bounded VBlank dispatch (32 entries) has one target outside the image: the same outcome the analysis already reports.

**5. Even a perfect table authority does not unblock any title.** Upper bound: if every table-related unresolved site in finding 3
were resolved, whole-image triggers would still be at least 12 (Sonic 1), 40 (Sonic 2) and 17 (Cool Spot) in the *uncredited* ceiling
run (computed returns 9 / 39 / 14, RTE 2 / 1 / 1, plus non-table `(An)` sites: 1 / 0 / 2), and every credited run stays blocked by the
interrupt class first. `hybrid_total == U` on every title with or without it.

**6. Measurements (A credited, B uncredited ceiling; `credited:false` on every ceiling output).** Candidate sites for a new authority:
0 credited, 0 ceiling (2 near-candidates fail validation, 1 is one-sided). Sites resolved by a new authority: 0. Exact targets
recovered by it: 0. Whole-image triggers before = after (13 / 5 / 22 credited; 23 / 64 / 22 ceiling), `D` before = after (1,276 / 335 /
6,907 credited; 6,806 / 6,792 / 6,907 ceiling), outcome `broad_whole_image` on all three, closure rounds 1, new closure sites 0.

## Decision: STOP

Pre-registered STOP conditions met: the real remaining table sites are width-only with no independent program bound; the cells that
would need a bound would need the heap / store-provenance framework ADR 0055 stopped; the only program-bounded candidates produce
accidental targets and fail validation; at most one near-candidate per title class could benefit; and no downstream blocker class
changes (the computed-return and interrupt classes remain).

Per the inventory gate ("if the answer is zero, stop without building machinery") no authority, synthetic fixtures or mutants were
added: there is no code whose soundness would need them. The product change is this ADR and the private `--trace-points` register dump.
Broad production output is untouched (nothing outside the report driver's private diagnostic changed).

**Sound selective admission has now failed both** general bounded precision refinements (ADR 0081) and bounded immutable
pointer-table authority (this ADR) for current real-title economics. Recommended direction: shift optimisation effort to the **broad-AOT
representation / emitter** (generated C of about 220 MB and a 35 MiB executable for Sonic 1 are the practical blocker); do not start
another abstract-analysis subsystem. The instruments stay (`--trace-points`, `--diagnostic-transparent-handlers`) and the question can be
re-run cheaply if a premise decision (interrupt / return-slot integrity) is ever taken.
