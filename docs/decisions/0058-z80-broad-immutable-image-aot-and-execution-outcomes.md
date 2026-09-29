# ADR 0058: Z80 Static-Code Strategy: Broad Immutable-Image AOT (B1), Image-Identity Dispatch and Execution Outcomes

- Status: Accepted (decision: ADOPT broad per-address AOT, one owner per instruction start (B1), as the Z80
  default; no reachability mechanism and no superblocks by default)
- Date: 2026-09-29
- Task: SEG-008-T001
- Related: ADR 0002 (static translation, fail closed), ADR 0039 (M68K independent immutable-ROM AOT identities),
  ADR 0051/0053 (report-only experiments), ADR 0056 (Z80 contract), ADR 0059 (placement).

## Question

Is broad 16-bit AOT practical for the Z80 under stated budgets? Broad AOT treats every address of an immutable code
image as a candidate instruction start and gives every legal decode a generated-native owner. If it is not
practical, what is the smallest reachability mechanism the evidence justifies?

## Experiment (report-only; not retained)

The experiment used a throwaway Python classifier and emitter plus a C++ driver. The classifier was driven only by
`tests/fixtures/z80-legal-forms.json` (DD/FD chains per its prefix rules; typed truncation at the image edge). The
driver compiled the product `translation_units.cpp` unchanged, and the emitter used `emit_compiled_entry_table`
semantics with Z80 names. After the decision the code was intentionally **not retained**, following the ADR
0051/0053 precedent: it has no production consumer, and T003 builds the real pipeline.

- **Emitted bodies.** Each owner body was realistic, unvalidated C11: full inline S/Z/Y/H/X/PV/N/C computation,
  memory and I/O callbacks, R increment, T-state accounting, and a deadline check at every owner. All 1,446
  encodings plus chain and ignored-prefix cases compiled with zero warnings under
  `-std=c11 -Wall -Wextra -Wpedantic -Werror`.
- **Host.** Apple M2 Pro (12 logical CPUs, 16 GiB), macOS 26.2, Apple clang 21.0.0, `-O1`. Timings are single
  runs, so expect about 10% noise.
- **Inputs.** All synthetic, fixed seeds, 64 KiB:
  - `dense`: documented forms with realistic control-flow density and patched in-image targets;
  - `random`: uniform bytes;
  - `zero` (all 0x00) and `ff` (all 0xFF);
  - `dd_only`, `fd_only` and `ddfd_alt`: whole-image DD, FD and alternating DD/FD chains;
  - `mixed_prefix`: chains of 1-1,024 prefixes before real opcodes, with a final 1,275-byte chain into the edge;
  - plus 1-32 banked 16 KiB images.

  No commercial image was used; the optional SMS aggregate was skipped because no authorized image exists.
- **Strategies:**
  - **A:** reachability from 0x0000, RST 08..38 (0x38 = IM1) and NMI 0x66 over direct flow.
  - **B1:** one owner per legal start; the direct successor is returned as the next-owner pointer.
  - **B2:** per-start straight-line superblocks with a cap of 16.
  - **Dispatch:** the exact binary search from `compiled_entry_table.hpp` compared with a dense 64K pointer table.

### Pre-declared budgets (per 64 KiB image; fixed in the experiment specification before any measurement)

- generated C <= 128 MiB;
- `-j8` compile <= 90 s and `-j1` compile <= 300 s;
- peak compiler RSS <= 1.5 GiB;
- executable <= 48 MiB;
- exact lookup median <= 100 ns;
- byte-identical output across two runs.

### Results (per 64 KiB image, -O1)

| input | strategy | legal starts / owners | typed truncations | generated C MiB | exe MiB | j8 / j1 s | compiler RSS MiB | exact lookup ns rnd/seq | budgets |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| dense | A | 10,603 | 0 | 6.0 | 2.1 | 2.2 / 11.0 | 98 | 31.7 / 21.3 | pass |
| dense | **B1** | 65,536 | 0 | 35.7 | 12.2 | 8.8 / 49.7 | 227 | 34.0 / 19.6 | **all pass** |
| dense | B2 (dup 6.46) | 65,536 | 0 | **155.3** | 35.4 | 37.6 / 207.7 | 544 | 33.1 / 19.8 | **miss C** |
| random | B1 | 65,536 | 0 | 34.0 | 11.5 | 6.9 / 42.9 | 221 | 32.4 / 18.8 | all pass |
| random | B2 (dup 5.57) | 65,536 | 0 | 121.1 | 25.4 | 20.7 / 138.1 | 448 | 32.2 / 19.2 | pass (95% of C) |
| zero | B1 (= A) | 65,536 | 0 | 22.2 | 7.5 | 3.9 / 23.7 | 216 | 32.9 / 18.8 | all pass |
| zero | B2 (dup 16.0) | 65,536 | 0 | **130.9** | 7.5 | 11.4 / 71.9 | 379 | 34.7 / 20.2 | **miss C** |
| ff | B1 / B2 | 65,536 | 0 | 32.4 | 13.7 | 5.8 / 36.3-37.2 | 226 / 236 | 32.2 / 18.6 | all pass |
| dd_only / fd_only / ddfd_alt | A, B1, B2 | 0 | 65,536 | ~0 | 0.03 | 0.1 / 0.1 | 38 | 1.1 / 0.9 | all pass |
| mixed_prefix | B1 | 64,261 | 1,275 | 29.8 | 10.1 | 5.2 / 31.7 | 254 | 25.6 / 21.0 | all pass |
| mixed_prefix | B2 (dup 3.43) | 64,261 | 1,275 | 63.9 | 16.7 | 9.9 / 60.8 | 442 | 24.4 / 19.9 | all pass |

- **Smaller measurements.** B1 at `-O2` (dense): j8 12.0 s, j1 57.3 s, 210 MiB. The shared all-declarations
  header is 2.06 MiB. B1 costs 355-571 bytes of C per owner and B2 1,042-2,485.
- **Dispatch.** The exact binary search costs 19-42 ns (64K keys) and 25-61 ns (512K keys), and its table uses 896 KiB
  per 64K owners. A dense table costs about 0.95 ns and 512 KiB. Binary search meets the budget everywhere, so the
  dense variant is **not justified** under the decision rule. Lookups happen only on indirect, return, interrupt
  and resume transitions, because B1 chains direct successors.
- **Prefix adversaries.**
  - Whole-image chains produce 65,536 typed truncations, 0 owners and a fail-closed lookup.
  - In `mixed_prefix`, 63,998 of 64,261 owners are chain suffixes (mean 450, max 1,024 prefixes per owner).
    Generated size tracks the number of starts, not chain length: 486 bytes per owner against 544 for random
    bytes. Per-address chain starts are therefore safe.
  - The chain running into the edge yields exactly 1,275 truncations, with no owner, no symbol and no table entry.
  - Classifier hazard: a naive per-address chain rescan is O(n²) (2^31 steps on a DD image). The classifier must
    precompute chain ends in one O(n) pass.
- **Banked scaling (B1, keys (image, address)).** Cost is linear per 16 KiB bank: 9.1 MiB of C, 3.1 MiB of
  executable, about 1.8 s at j8 and about 11 s at j1 (11.7 s measured for one bank).
  - A measured 32 x 16 KiB (512 KiB ROM) case produced 291.6 MiB of C and a 100.4 MiB executable, with j8 82.8 s and
    j1 368 s using the packed shard key.
  - Its peak compiler RSS was 1,363 MiB (1.33 GiB) with 33 TUs. The naive `image << 16 | address` shard key crowded
    16 KiB banks into half the shards and used 2,075 MiB (2.03 GiB).
  - B2 at 512 KiB is extrapolated to about 1.24 GiB of C and over 4 GiB RSS per TU. It was not run.
- **Reproducibility.** Run 1 and run 2 were byte-identical (sha256 over the output tree) in all 32 cases.
- **Whole-ROM verdict.** The budgets are per 64 KiB of code, and B1 meets them per 64 KiB at every scale measured.
  No whole-program budget was pre-declared, so none is claimed. A 512 KiB banked ROM is 8x the code: its j1 time
  (368 s) and executable (100.4 MiB) are about 8x the per-64 KiB figures. Its j8 time (82.8 s) and per-process
  RSS (1.33 GiB) stay within the per-image limits only with the packed shard key.
- **Multi-window multiplier (not measured).** Owners embed absolute addresses, so a bank that a platform can map
  into k different CPU windows needs one image identity, and one owner set, per (bank, window). Its cost is k
  times its per-window figures. SEG-009 must state its mapper's actual window multiplicity and measure the product
  before relying on these figures.

## Decision

1. **Broad immutable-image AOT with B1 granularity is the Z80 default.**
   - Every address of every executable immutable code image is a candidate start.
   - Every legal decode that does not reach past the image edge gets exactly one owner (a C function).
   - An owner transfers to its direct successor by returning the successor owner, or by a direct call where the
     lowering chooses.
   - B1 met every budget on every input with at least 3.6x margin on generated C. B2 missed the C budget on
     dense code and on a NOP sled for no lookup benefit. Strategy A is unnecessary: broad AOT is practical, so no
     reachability mechanism is introduced (SEG-008 Notes question 10), and no M68K discovery tiers are imported.
   - Superblocks may return later only as a measured, selective optimisation with its own ADR.
2. **Dispatch** uses the generic exact `compiled_entry_table.hpp` binary search with Z80 table names. A dense table
   may be added only in the generic codegen layer, and only if a later measurement shows the binary search missing
   a budget.
3. **Owner key.** The key is (code-image identity, 16-bit address). The lookup key is the 32-bit value
   `image_id << 16 | address`.
   - Address arithmetic wraps at 0xFFFF in the CPU address space.
   - An instruction's bytes never wrap inside an image: they are read from the image's own byte sequence.
   - The TU shard key is the dense position (`image ordinal x image length + offset`). The lookup key is not used
     for sharding, so that banked images spread over every shard (the experiment's crowding finding). The shard
     count scales with total owners to keep compiler RSS within budget.
4. **Direct binding and mapping-sensitive transfers.**
   - Code images and their windows are generation-time platform input. The platform declares each window as
     either **statically invariant** (always this image) or **mapping-sensitive** (banked).
   - A direct target (JP/JR/CALL/DJNZ/RST, or fall-through) binds to an owner symbol **only if the target lies in
     a statically invariant window** (SEG-008 Notes answer 9). No other exception exists. In particular, being in
     the same banked window as the source owner is not enough. Any instruction can write memory or I/O, including
     the mapper control registers, directly or through a CALL/RST push. Binding across such a write would run
     stale code from the previous image instead of failing closed.
   - Every other direct target, including fall-through inside a mapping-sensitive window, and every
     runtime-selected target, sets PC and returns to the dispatcher. The
     runtime-selected targets are `JP (HL)/(IX)/(IY)`, `RET`/`RETI`/`RETN` (conditional or not), the IM1/IM2/NMI/
     IM0-RST handler PC, and resume after deadline, HALT or interrupt.
   - The dispatcher asks the platform, through the runtime ABI, for the current code-image identity of the PC,
     then performs an exact lookup of (identity, PC).
   - No discovery or refinement cycle exists: a miss is a typed stop (Notes question 11).
   - Faster binding inside banked windows, for example a prologue identity re-check, is a possible later
     optimisation. It needs its own ADR with a soundness argument covering writes to the mapping control.
5. **Image edges.**
   - An instruction or prefix chain that would read past the end of its image is a **typed truncation**. It gets
     no owner and no table entry, and a transfer to it fails closed with `no_owner`.
   - A platform may declare a composite invariant image, for example a fixed region physically adjacent to
     another invariant region, to make straddling instructions legal. The CPU library never assumes adjacency.
   - The chain classifier is linear-time.
6. **Resume and deadline.**
   - Every instruction boundary is an owner start, so any boundary resumes through the entry table. Each
     repeated block-instruction iteration is also a boundary: the iteration re-enters its own owner.
   - Each owner prologue checks the cycle deadline and pending acceptable interrupts (EI and IFF1-changing
     RETI/RETN deferral, and prefix indivisibility, per ADR 0056) before executing its instruction.
   - When the deadline has been reached, or an interrupt must be taken, the owner returns to the runtime with PC =
     its own address.
   - A DD/FD chain is one owner, so no resume point exists inside a chain.
7. **Execution outcomes.** One result enum carries two distinct classes.
   - **Resumable (not errors):**
     - `deadline`: stopped at an instruction boundary with cycles >= deadline;
     - `halted`: HALT executed, or still halted when the deadline arrives. PC = HALT+1, and halted cycles are
       accounted by the runtime.
   - **Fail-closed errors (typed stops, never recovered by decoding):**
     - `no_owner`: the target was never emitted (including typed truncations and image-edge straddles);
     - `mutable_code`: no immutable code image is mapped at the target (RAM, or modified/self-modifying code);
     - `unknown_image_identity`: the platform reports an identity that was not compiled;
     - `excluded_form`: reserved; the ADR 0056 scope currently excludes nothing;
     - `im0_unsupported_acknowledge_byte` (ADR 0056 §5).
   - A predicate distinguishes the classes, so the two can never be confused.

## Code-image statement (Scope item 6)

- **Supported: immutable banked ROM.** Bank switching is immutable code-image remapping: owners are keyed by
  (image identity, address), and the platform reports the current identity through the runtime ABI. SEG-009
  crosses Master System bank boundaries through this mechanism. Mapper policy stays entirely outside `libs/cpu/z80`.
- **Fail closed: mutable code.** RAM-generated code, code modified after compilation and self-modifying code stop
  with `mutable_code` until a later architecture explicitly supports them. The same applies to a ROM-to-RAM copy
  until SEG-032-style materialization supplies a static image identity for it.
- T001 adds no platform mechanism.

## Consequences

- T002's decoder must classify all 65,536 addresses of an image in linear time and return typed truncation at the
  edge.
- T003 builds the owner and entry emission with packed dense shard keys, the runtime ABI outcome enum above,
  and a synthetic two-image test in which mapping-sensitive transfers dispatch by the current identity.
- Generated-size budgets for large banked ROMs are governed by the linear per-bank figures above. A future emitter
  that factors flag helpers will be smaller than the experiment's fully inline bodies.
