# ADR 0060: Z80 Generated Runtime ABI, Owner Emission Shape and Differential Harness

- Status: Accepted
- Date: 2026-09-29
- Task: SEG-008-T003
- Related: ADR 0056 (contract), ADR 0057 (oracle), ADR 0058 (static-code strategy), ADR 0059 (placement),
  `docs/testing/z80-conformance-harness.md`.

## Context

ADR 0058 fixed the strategy (broad immutable-image AOT, B1 owners, window-relative owners, two outcome classes) and ADR
0059 the placement. T003 builds the first real pipeline and had to settle the concrete shapes those ADRs left open.

## Decisions

1. **Owner signature and chaining.** An owner is `struct Z80OwnerRef f(Z80Runtime *rt, uint16_t window_base)` and returns
   the next owner (direct binding, invariant windows only) or a null reference to return to the dispatcher with
   `state.pc` set. The struct wrapper makes the type self-referential without a `void *`. Absolute owners ignore
   `window_base`. The dispatcher `z80_run(rt, deadline)` is generated per image (it needs the entry table); its boundary
   logic is in the ABI header so that hosts and tests read one definition.
2. **Boundary split.** Each owner prologue only *tests* (deadline reached, NMI latched, INT acceptable) and returns to the
   dispatcher with PC = its own address; the dispatcher performs the interrupt response (NMI 11 T, IM1 13 T, IM2 19 T, IM0
   RST-only 13 T, R + 1, IFF, one-boundary deferral, NMOS `LD A,I/R` P/V quirk, discarded NMI edge during an NMI response)
   and halted-cycle accounting. A prefix_lock owner entered with in-prefix-run clear runs the same prologue first.
   Every interrupt mode performs the acknowledge transaction (the pinned oracle does), IM1 ignoring the byte.
3. **Accounting order.** The emitter increments R before the row's statements (so `LD A,R` observes its own increments and
   `LD R,A` overwrites them) and adds T-states after them (callbacks see the start-of-instruction cycle count). Q is
   `F` for a row that writes flags and 0 otherwise.
4. **Undecoded rows.** A decoded start whose form has no lowering row is not emitted (`no_owner`, fail closed) rather than
   stubbed: the entry table stays exact, and coverage cannot claim a form before its row exists. Rows live one family per
   file, registered once in the registry; a form claimed by two rows is an error.
5. **RSS shape.** The shared header carries only the ABI include. Owners are units published with
   `publish_declaration = false` (an additive `translation_units.hpp` parameter; default behaviour unchanged); a direct
   binding declares its successor in block scope; the main TU declares every owner of a flat entry table (entry table + dispatcher); above 65,536 owners the generic chunked table (`emit_compiled_entry_table_chunked`, SEG-008-T009) moves the table and each chunk's owner declarations into per-chunk `entry` TUs and the main TU keeps only the chunk directory and dispatcher.
   Tables of at most 65,536 entries (every 64 KiB image, about 37 MiB of C and 33 TUs) stay flat; the chunked form was needed at 512 KiB scale (ADR 0058 T009 addendum).
6. **Window-relative variants.** Per offset, the classification of each admissible window is compared; equal
   semantics share one body, differing ones become `switch (window_base)` variants (a stub of the right kind in one window,
   a full owner in another). Windows of one image may expose different sub-ranges (SMS slot 0 exposes offsets 0x0400-0x3FFF of a bank that slots 1 and 2 expose whole): an owner exposed by only some of the image's window bases also gets a base switch whose default is `no_owner`, and two windows of one image at the same base may not overlap. A `prefix_lock` start cannot occur in a window-relative owner (non-code or another window
   breaks every fetch circle) and is rejected at emission.
7. **Harness.** Vectors are plain text shared by both sides. The comparator is Z80-local (SEG-020 order: cpu, memory, io,
   timing); `m68k_first_divergence.py` is M68k-schema-bound and is not touched. The oracle adapter normalises the core's
   in-chain PC (ADR 0057), maps the internal EI/RETI deferral, `LD A,I/R` marker and NMI-reject request bit, and steps
   with `z80_run(1)` (completing DD/FD chains) or a raw budget for the endless prefix run. Oracle credit is a committed
   digest-attributed manifest; the coverage tool measures `lowers`..`aot_admitted` itself through public entry points.
8. **Not wired here.** The secondary kosarev/z80 check and SingleStepTests sampling named in ADR 0057 are not part of the
   T003 record's scope and stay open for a later task.

## Consequences

- T004-T007 add lowering rows, vector rows and a manifest per family without touching shared files.
- The T002 tests registered after the CMake label loop carried no tier label; T003 labels them.
