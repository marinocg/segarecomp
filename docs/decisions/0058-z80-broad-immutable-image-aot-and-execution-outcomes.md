# ADR 0058: Z80 Static-Code Strategy: Broad Immutable-Image AOT (B1), Image-Identity Dispatch and Execution Outcomes

- Status: Accepted (decision: ADOPT broad per-address AOT, one owner per instruction start (B1), as the Z80
  default, with window-relative owners for images admissible in several windows and a logical fetch mapping that
  wraps at 0xFFFF; no reachability mechanism and no superblocks by default)
- Date: 2026-09-29
- Task: SEG-008-T001
- Related: ADR 0002 (static translation, fail closed), ADR 0039 (M68K independent immutable-ROM AOT identities),
  ADR 0051/0053 (report-only experiments), ADR 0056 (Z80 contract), ADR 0059 (placement).
- Amended by ADR 0071 (SEG-033): the semantics below are unchanged; an owner is now a bounded multi-entry host function selected by PC or window offset, and PC-independent effects are shared functions.

## Question

Is broad 16-bit AOT practical for the Z80 under stated budgets? Broad AOT treats every address of an immutable code
image as a candidate instruction start and gives every legal decode a generated-native owner. If it is not
practical, what is the smallest reachability mechanism the evidence justifies?

## Experiment (report-only; not retained)

The experiment used a throwaway Python classifier and emitter plus a C++ driver. The classifier was driven only by
`tests/fixtures/z80-legal-forms.json` (DD/FD chains per its prefix rules). The first run truncated instructions at the storage-image edge; that rule is superseded by §5, and the second experiment below reclassified the same inputs. The
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
  - Whole-image chains produced 65,536 typed truncations and 0 owners under the superseded edge rule. Under §5's
    wrapping fetch they are 65,536 `prefix_lock` starts (second experiment, Part 1).
  - In `mixed_prefix`, 63,998 of 64,261 owners are chain suffixes (mean 450, max 1,024 prefixes per owner).
    Generated size tracks the number of starts, not chain length: 486 bytes per owner against 544 for random
    bytes. Per-address chain starts are therefore safe.
  - Under the superseded edge rule, the chain running into the edge yielded 1,275 truncations. Under wrapping fetch
    those 1,275 starts become full owners: the chain continues at 0x0000. The longest chain grows from 1,024 to
    2,130 prefixes, and generated size still tracks starts, not chain length.
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

## Second experiment: 16-bit fetch wrap and an SMS-shaped multi-window machine (report-only; not retained)

Review of the first result raised two points. Z80 fetch wraps at 0xFFFF (§5). A banked code image can be mapped
into several windows, so owners with absolute PCs would multiply. A second throwaway experiment measured both
without a ROM. The environment was the same host and compiler, with the product `TranslationUnitSharder` and
`emit_compiled_entry_table` unchanged.

**Part 1: reclassify the first experiment's 64 KiB inputs as a full invariant 64 KiB mapping with wrap**
(classification only, 0.01-0.08 s per image, linear):
- `dense`, `random`, `zero` and `ff` are unchanged, with 65,536 owners each.
- `dd_only`, `fd_only` and `ddfd_alt`: 65,536 `prefix_lock` starts each, instead of truncations.
- `mixed_prefix`: 65,536 full owners (+1,275; the final chain wraps into the chain at 0x0000).

**Part 2: SMS-shaped logical map, from public Sega-mapper documentation.**
- The map:
  - 0x0000-0x03FF is invariant (bank 0, first 1 KiB);
  - slot 0 (0x0400-0x3FFF), slot 1 (0x4000-0x7FFF) and slot 2 (0x8000-0xBFFF) can each hold any bank;
  - 0xC000-0xFFFF is non-code (RAM).
- Bank contents were dense synthetic documented code (seeds 82000+bank) or uniform random bytes (83000+bank).
- Broad B1 was applied under every admissible (bank, window) instance: 1,541,120 window instances for 32 banks,
  2.94x the single-window count. Direct binding was allowed only into the invariant window (§4).
- Two owner representations were compared:
  - **N (naive):** one owner per (bank, window) instance, with absolute PC constants.
  - **R (window-relative):** one owner per (bank, offset), shared by all windows. It receives the window base
    (`PC & 0xC000`) and computes every PC-dependent value from base + offset: fall-through, pushed return, JR/DJNZ
    target, repeat self-target and deadline PC. The lookup key is (bank identity, `PC & 0x3FFF`).
  - The invariant 1 KiB keeps its own absolute owners in both representations, because the byte after 0x03FF
    belongs to slot 0, not to bank 0.

Pre-declared whole-program budgets for a 512 KiB SMS-shaped ROM, fixed in the experiment specification before
measuring:
- generated C <= 1 GiB;
- -j8 compile <= 300 s and -j1 compile <= 1,800 s;
- peak per-compiler-process RSS <= 1.5 GiB (maximum over serial and parallel runs);
- executable <= 256 MiB;
- exact lookup median <= 100 ns;
- byte-identical output.

| ROM | rep | entries (window instances) | gen C MiB | exe MiB | j8 / j1 s | peak compiler RSS MiB | lookup ns rnd/seq | identical | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 256 KiB dense | R | 263,168 (771,072) | 162.2 | 50.5 | 31.3 / 188.5 | 1,075 | 32.5 / 22.4 | yes | - |
| 256 KiB dense | N | 771,072 | 425.3 | 143.7 | 90.5 / 587.1 | 1,831 | 57.1 / 27.6 | yes | - |
| **512 KiB dense** | **R** | 525,312 (1,541,120) | **323.8** | **100.9** | **71.6 / 412.2** | **1,466** | 37.2 / 23.9 | yes | **PASS (all 7)** |
| 512 KiB random | R | 525,312 (1,541,120) | 311.4 | 95.2 | 62.2 / 385.1* | 1,471 | 38.0 / 24.3 | yes | PASS (all 7) |
| 512 KiB dense | N | 1,541,120 | 850.4 | **287.2** | 195.4 / 1,262.8 | **2,891** | 76.8 / 28.4 | yes | **MISS (RSS, exe)** |

\* The random-R -j1 time and serial RSS were measured on 12 of 48 owner TUs plus the main TU, and the time
extrapolated. The main TU dominates RSS, and the full parallel run peaked at 1,126 MiB.

- **R scales about linearly.** From 256 to 512 KiB it is 2.0x in C and executable size, and 2.19x in -j1 time. It
  costs about 1.1x the C and 1.02-1.12x the compile time per owner of the single-window B1 baseline (1.12x for
  the 512 KiB dense case), despite serving about 2.94 window
  instances per owner.
- **N misses** the RSS budget (2,891 MiB = 2.82 GiB) and the executable budget (287 MiB) at 512 KiB. Its all-declarations
  shared header alone costs about 745 MiB in every TU, so adding shards cannot fix it.
- **R's peak RSS is the main TU** (1.43 GiB, 95% of budget): the entry table plus the all-owner declaration
  header. Owner TUs peak at 1,173 MiB (1.15 GiB).
- **Stubs.** Starts whose instruction crosses its window edge become typed stubs: 7 of 1,541,120 window instances
  (dense) and 9 (random). No prefix lock can occur under this map, because non-code breaks every fetch circle.
- **Execution cost of the strict binding rule.** 999,987 of 1,000,000 bounded smoke steps returned to the
  dispatcher, at about 32 ns per owner step (3.7 ns with unrestricted chaining in the first experiment).
  N and R produced identical cycle totals in the bounded smoke runs.
- **Authorized local SMS images** (ephemeral, sanitized aggregate: size classes and counts only; not a CI input).
  - Three images were classified under the same map with window-relative owners: 128 KiB, 256 KiB and 512 KiB.
    Every admissible window instance resolves to a full owner, except 6 of 386,048 window instances in the
    128 KiB image, which cross a window edge and become typed stubs. None has a prefix lock, and the longest
    prefix chain is 4.
  - One build of the 512 KiB image met all 7 budgets:
    - 291.0 MiB of C and an 89.6 MiB executable;
    - -j8 55.5 s; -j1 about 339 s, extrapolated from 6 of 48 owner TUs plus the main TU;
    - peak compiler RSS 1,465 MiB, set by the same main TU;
    - lookup 38/24 ns; byte-identical; zero warnings.
  - The synthetic dense banks are therefore slightly conservative.
- **Not run.** 1 MiB (R about 2x the 512 KiB figures by linearity) and 512 KiB random N (N already rejected).
- **Measurement note.** Serial compiles report a higher per-process RSS than parallel ones on this host. All RSS
  figures in this ADR are the maximum over serial and parallel runs.

## Decision

1. **Broad immutable-image AOT with B1 granularity is the Z80 default.**
   - Every address of every executable immutable code image (under every admissible window mapping, §5) is a
     candidate start.
   - Every start whose bytes resolve statically through the logical code mapping (§5) gets exactly one owner, a C
     function. A start in a prefix lock gets a small `prefix_lock` owner. Every other start gets a small typed
     fail-closed stub owner. No start is ever left without an owner.
   - An owner transfers to its direct successor by returning the successor owner, or by a direct call where the
     lowering chooses.
   - B1 met every budget on every input with at least 3.6x margin on generated C. B2 missed the C budget on
     dense code and on a NOP sled for no lookup benefit. Strategy A is unnecessary: broad AOT is practical, so no
     reachability mechanism is introduced (SEG-008 Notes question 10), and no M68K discovery tiers are imported.
   - Superblocks may return later only as a measured, selective optimisation with its own ADR.
   - **Owner representation: window-relative (R) for multi-window images.** The second experiment measured this
     on an SMS-shaped map.
     - When the logical code mapping admits an image in more than one window of the same stride and alignment,
       each (image, offset) gets one owner shared by every admissible window. A window may expose only a
       sub-range of the image, such as SMS slot 0 (0x0400-0x3FFF).
     - A shared owner can be a full owner in one window and a typed stub in another: an instruction crossing the
       window's end resolves by the adjacent window's class. The owner selects the stub kind at runtime from the
       base, and T003 must implement and test this.
     - The owner receives the window base and derives every PC-dependent value from base + offset.
     - The lookup key is (image identity, offset within the window).
     - An image admissible in exactly one window, and every invariant window, keeps absolute-PC owners.
     - Naive per-(image, window) owners (N) are rejected: they missed the 512 KiB whole-program budgets.
     - Broad B1 is therefore practical for the Master System machine: a 512 KiB SMS-shaped ROM passes all seven
       pre-declared whole-program budgets.
2. **Dispatch** uses the generic exact `compiled_entry_table.hpp` binary search with Z80 table names. A dense table
   may be added only in the generic codegen layer, and only if a later measurement shows the binary search missing
   a budget.
3. **Owner key.** The key is (code-image identity, 16-bit address). The lookup key is the 32-bit value
   `image_id << 16 | address`. For window-relative owners (§1) the address component is the offset within the
   window, and the dispatcher passes the window base derived from PC.
   - Address arithmetic, including instruction fetch, wraps at 0xFFFF in the CPU address space (§5).
   - The TU shard key is the dense position (`image ordinal x image length + offset`). The lookup key is not used
     for sharding, so that banked images spread over every shard (the experiment's crowding finding). The shard
     count scales with total owners to keep compiler RSS within budget.
4. **Direct binding and mapping-sensitive transfers.**
   - Code images and their windows are generation-time platform input. The platform declares each window as
     either **statically invariant** (always this image) or **mapping-sensitive** (banked).
   - A direct target (JP/JR/CALL/DJNZ/RST, or fall-through) binds to an owner symbol **only if the target lies in
     a statically invariant window** (SEG-008 Notes answer 9).
   - For window-relative owners (§1), only **absolute** targets (JP nn, CALL nn, RST p, and their conditional
     forms) can be classified statically. Base-relative targets (fall-through, JR, DJNZ, the repeat self-target)
     are never statically bound, because the same shared owner reaches different windows from different bases. No other exception exists. In particular, being in
     the same banked window as the source owner is not enough. Any instruction can write memory or I/O, including
     the mapper control registers, directly or through a CALL/RST push. Binding across such a write would run
     stale code from the previous image instead of failing closed.
   - Every other direct target, including fall-through inside a mapping-sensitive window, and every
     runtime-selected target, sets PC and returns to the dispatcher. The
     runtime-selected targets are `JP (HL)/(IX)/(IY)`, `RET`/`RETI`/`RETN` (conditional or not), the IM1/IM2/NMI/
     IM0-RST handler PC, and resume after deadline, HALT or interrupt.
   - The dispatcher asks the platform, through the runtime ABI, for the current code-image identity and window of
     the PC, then performs an exact lookup of the owner key (§3):
     - (identity, PC) for absolute-PC owners, i.e. invariant windows and images admissible in exactly one window;
     - (identity, PC - window base) for window-relative owners, whose window base is passed to the owner.
   - No discovery or refinement cycle exists: a miss is a typed stop (Notes question 11).
   - Faster binding inside banked windows, for example a prologue identity re-check, is a possible later
     optimisation. It needs its own ADR with a soundness argument covering writes to the mapping control.
5. **Logical fetch mapping (not storage-image edges).**
   - **Architecture.** The Z80 fetches every instruction byte at the next logical address, PC + 1 wrapping
     0xFFFF -> 0x0000. There is no architectural "image edge". `3E` at 0xFFFF with `42` at 0x0000 is an ordinary
     `LD A,42h`. The pinned oracle confirms this for operand, displacement, opcode and chain bytes (ADR 0057).
   - **Generation-time logical code mapping.** Platform input assigns every 16-bit logical address one class:
     - an **invariant window**: always the same immutable bytes. Every invariant window has **its own code-image
       identity**, distinct from any banked image, even when its bytes are a copy or sub-range of a banked image.
       Example: the SMS fixed first 1 KiB is bank 0's bytes, but bank 0 can also sit in a slot. Its owners are
       absolute-PC owners whose successor bytes differ from a banked instance of the same offset. A shared
       identity would give two different owner bodies the same key;
     - a **mapping-sensitive window**: banked; the admissible images are declared, and the current one is
       reported at runtime;
     - **non-code**: RAM, I/O or unmapped.

     A full immutable 64 KiB mapping is one invariant window covering the whole space, so fetch simply wraps. The
     CPU library knows only this abstract mapping and no mapper policy.
   - **Classifying one owner start** (window W, image X). Each fetched byte at a logical address A (wrapping)
     resolves as follows:
     - **A in W:** the byte comes from X at A's offset.
     - **A in an invariant window:** the byte comes from that window's image.
     - **A in a different mapping-sensitive window:** the image cannot be statically identified. The start
       gets a fail-closed stub owner, `unresolved_fetch_mapping`.
     - **A in non-code:** the start gets a fail-closed stub owner, `mutable_code`. Fetching instruction bytes
       from mutable memory is mutable code.
   - **Prefix lock.** A DD/FD run that revisits the same (mapping state, logical address, effective prefix)
     without reaching a non-prefix opcode can never terminate.
     - Its start gets a `prefix_lock` owner. **Entered with the in-prefix-run state clear**, which is the case at
       an ordinary instruction boundary, the owner runs the normal boundary prologue (§6): deadline check, then
       acceptance of a pending acceptable INT or NMI. Then it sets in-prefix-run.
     - While in-prefix-run is set, it accounts 4 T-states and R += 1 per prefix and advances the next-fetch PC
       with wrap. It never accepts INT or NMI, and it returns the resumable `prefix_lock` outcome at the deadline
       with in-prefix-run still set.
     - Resuming with in-prefix-run set skips interrupt acceptance and continues the run. Every other outcome and
       every instruction boundary leaves it clear. Resume and entry use the same owner-key lookup (§3, §4), so this
       explicit state bit, part of the Z80 state structure in the runtime ABI, is what distinguishes them.
     - The pinned oracle confirms acceptance at entry (IM1 13 T, NMI 11 T) and none inside the run (ADR 0057).
     - Only RESET, which is platform policy, leaves the run.
     - This is the one resumable state inside a prefix run. It is safe because no effective instruction ever
       follows.
   - Chain ends are precomputed in one linear pass over the circular logical address space. The naive
     per-address rescan is O(n²).
6. **Resume and deadline.**
   - Every instruction boundary is an owner start, so any boundary resumes through the entry table. Each
     repeated block-instruction iteration is also a boundary: the iteration re-enters its own owner.
   - Each owner prologue checks the cycle deadline and pending acceptable interrupts (EI and IFF1-changing
     RETI/RETN deferral, and prefix indivisibility, per ADR 0056) before executing its instruction.
   - When the deadline has been reached, or an interrupt must be taken, the owner returns to the runtime with PC =
     its own address.
   - A DD/FD chain that reaches an opcode is one owner, so no resume point exists inside it. The only exception is
     the `prefix_lock` state with in-prefix-run set (§5).
   - The prologue is skipped only when resuming with in-prefix-run set.
7. **Execution outcomes.** One result enum carries two distinct classes.
   - **Resumable (not errors):**
     - `deadline`: stopped at an instruction boundary with cycles >= deadline;
     - `halted`: HALT executed, or still halted when the deadline arrives. PC = HALT+1, and halted cycles are
       accounted by the runtime.
     - `prefix_lock`: inside an endless DD/FD run (§5). PC is the next fetch address, in-prefix-run is set, and the
       state is not interruptible until RESET.
   - **Fail-closed errors (typed stops, never recovered by decoding):**
     - `no_owner`: the (identity, address) key was never emitted. Under broad AOT this only happens for an address
       outside the declared code windows, or for an inconsistent platform identity;
     - `mutable_code`: no immutable code image is mapped at the target, or an instruction byte would be fetched
       from non-code (RAM, or modified/self-modifying code);
     - `unresolved_fetch_mapping`: an instruction's bytes continue into a different mapping-sensitive window, whose
       image cannot be statically identified;
     - `unknown_image_identity`: the platform reports an identity that was not compiled;
     - `excluded_form`: reserved; the ADR 0056 scope currently excludes nothing;
     - `im0_unsupported_acknowledge_byte` (ADR 0056 §5).
   - A predicate distinguishes the classes, so the two can never be confused.

## Code-image statement (Scope item 6)

- **Supported: immutable banked ROM.** Bank switching is immutable code-image remapping: owners are keyed by
  (image identity, address or window offset, §1/§3), and the platform reports the current identity through the
  runtime ABI. SEG-009
  crosses Master System bank boundaries through this mechanism. Mapper policy stays entirely outside `libs/cpu/z80`.
- **Fail closed: mutable code.** RAM-generated code, code modified after compilation and self-modifying code stop
  with `mutable_code` until a later architecture explicitly supports them. The same applies to a ROM-to-RAM copy
  until SEG-032-style materialization supplies a static image identity for it.
- T001 adds no platform mechanism.

## Consequences

- T002's decoder decodes from a logical fetch function (address -> byte, or unresolved/non-code) rather than
  from a storage slice. It wraps at 0xFFFF and classifies all starts of a mapping in linear time into owner,
  `prefix_lock`, `unresolved_fetch_mapping` or `mutable_code`. Z80 provenance records the start's (identity,
  address) and the logical byte count; the bytes may wrap.
- T003 builds the owner and entry emission with packed dense shard keys, the runtime ABI outcome enum above,
  and a synthetic two-image test in which mapping-sensitive transfers dispatch by the current identity. It also
  builds the window-relative owner representation with a synthetic image admissible in two windows.
- Compiler-memory headroom for large banked ROMs is set by the entry-table/declaration TU (1.43 GiB at 512 KiB).
  Owners in mapping-sensitive windows never reference each other (§4), so T003 must not declare every owner in
  the shared header. Only the entry-table TU needs them, and it may be sharded.
- The strict binding rule makes most steps dispatcher lookups (about 32 ns per step at 512 KiB). A faster sound
  binding (§4) is the natural later optimisation, and it needs its own ADR.
- Generated-size budgets for large banked ROMs are governed by the linear per-bank figures above. A future emitter
  that factors flag helpers will be smaller than the experiment's fully inline bodies.

## SEG-008-T009 addendum: budgets re-measured with the real lowerings on the corrected SMS map

`tools/z80_static_budget.py` (-O1, same host class, per-process peak RSS by wait4, -j8 and -j1, exact lookup via the generated
main TU's static lookup, two-run byte-identity). All 64 KiB shapes emit 65,536 full owners, no typed truncation, no unlowered start.

**Correction of the first T009 measurement.** The first re-measurement modelled the SMS-shaped image wrongly: it exposed each
bank at 0x4000, 0x8000 and **0xC000** and omitted slot 0 (0x0400-0x3FFF), so it treated the RAM region as banked ROM. Its figures
(compiler RSS 1,015-1,559 MiB) are superseded. The model is now exactly the second experiment's map above, expressed with
`CodeWindow` semantics (image offset `o` is exposed at `base + o`): the invariant first 1 KiB (base 0, offsets 0-0x3FF), and each
bank in slot 0 (base 0x0000, offsets 0x0400-0x3FFF), slot 1 (base 0x4000, offsets 0-0x3FFF) and slot 2 (base 0x8000, offsets
0-0x3FFF); nothing is exposed at 0xC000-0xFFFF. `z80_static_budget_shape_test` pins the map (a negative control shows it rejects
the old shape). Slot 0 exposing a sub-range exposed a T003 gap: the Z80 emitter required every window of an image to expose the
same range, contradicting the "a window may expose only a sub-range" decision above. The emitter now plans one owner per (image,
offset) over the union of the windows' ranges, each window contributing a variant only for the offsets it exposes;
`sms_*` oracle-checked scenarios cover slot 0/1/2, the invariant end crossing into slot 0 and the RAM region.

**Corrected result, before any fix.** With the flat entry table the corrected 512 KiB shapes measured (dense / random):
generated C 360.2 / 355.7 MiB, executable 122.9 / 121.4 MiB, -j8 99.9 / 83.4 s, -j1 504.4 / 472.5 s, lookup 46.7 / 47.0 ns, and compiler
peak RSS **1,388 / 1,549 MiB against the 1,536 MiB budget: random exceeded it by 0.9%**. The peak process was the main TU (the
whole entry table plus all 525,312 owner declarations), which grows linearly with the owner count.

**Fix (generic).** `emit_compiled_entry_table_chunked` (next to the flat `emit_compiled_entry_table`, which is unchanged) splits
the same sorted bindings deterministically into consecutive chunks of at most 65,536 entries. Each chunk is a unit of its own
TU family (through the existing `TranslationUnitSharder`) with its own owner declarations, tables and exact binary search; the
main TU keeps only a sorted table of each chunk's first key and one exact binary search over it. Lookup semantics are exact
(exact key or NULL); the output is byte-identical across runs; no dense pointer table and no Z80-specific dispatcher. Tables with
at most 65,536 entries stay flat (every 64 KiB image and every M68K/Genesis image is unchanged). The Z80 emitter enables it with
`EmitOptions::entry_chunk_entries` (default 65,536; a test hook lowers it so `sms_map` runs through the chunk path against the
oracle, including entries on both sides of a chunk boundary).

**Final figures** (units: MiB, seconds, ns; RSS = max compiler process over -j8 and -j1):

| shape | generated C | exe | -j8 / -j1 | compiler RSS | exact lookup rnd / seq | budgets |
| --- | --- | --- | --- | --- | --- | --- |
| 64K dense (legal instructions with operands) | 47.1 | 15.0 | 10.3 / 59.0 | 248 | 39.4 / 19.4 | all pass |
| 64K random | 46.1 | 14.8 | 9.6 / 57.8 | 250 | 36.4 / 18.6 | all pass |
| 64K zero | 36.0 | 10.9 | 5.7 / 35.8 | 245 | 34.4 / 19.5 | all pass |
| 64K 0xFF | 34.0 | 17.0 | 8.2 / 51.7 | 244 | 36.2 / 19.8 | all pass |
| 512K SMS map, dense, 32 banks (chunked entry table) | 368.0 | 123.3 | 125.0 / 564.7 | 868 | 65.0 / 24.2 | all pass |
| 512K SMS map, random (chunked entry table) | 363.4 | 121.8 | 86.8 / 494.7 | 921 | 66.3 / 21.3 | all pass |
| 512K authorized local SMS image (aggregate only; chunked) | 339.6 | 116.1 | 72.1 / 453.2 | 876 | 66.4 / 21.6 | all pass |

The 64K rows were measured before the entry-chunk change; images of at most 65,536 owners emit byte-identical output under it.

Budgets (pre-declared, unchanged): 64K: C <= 128, exe <= 48, -j8 <= 90 s, -j1 <= 300 s, RSS <= 1,536, lookup <= 100 ns; 512K: C <= 1,024,
exe <= 256, -j8 <= 300 s, -j1 <= 1,800 s, RSS <= 1,536, lookup <= 100 ns. All pass; the two-run byte-identity holds for every shape.

- Real lowerings cost about 1.3x the T001 stub-era C per owner at 64K and 1.1x at 512K; every size, time and lookup budget passes
  with at least 1.4x margin (executable at 512K 2.1x; -j1 at 512K 3.2x; -j8 at 512K 2.4x; RSS 1.7x; lookup 1.5x). The table rows are the final code (the last emitter change, a base check on owners exposed by only some window bases, grew 512K C by about 2%).
- The chunk directory adds one more binary search, so the 512K exact lookup is about 67 ns (was 47 ns flat), still inside 100 ns.
  Peak RSS at 512K is now 868-921 MiB (was 1,388-1,549 flat), consistent with no TU carrying the whole table (the lookup-bench link, which still includes the main TU, fell from about 1.2 GiB to about 0.8 GiB). Single runs: compiler RSS varied by hundreds of MiB between repeated identical inputs in the earlier measurement, so treat the figures as a range.
- The dispatcher round trip (image query + lookup + one owner step, not the budgeted quantity) is about 100-300 ns.

## Amendment (SEG-032-T003, 2026-10-01)

"RAM-generated code ... stop with `mutable_code`" is refined for one platform-declared case by ADR 0073: a Genesis sound-RAM image
materialized at build time is a RAM-backed code image whose every instruction is byte-verified against the live RAM before it
executes (`Z80_ERROR_CODE_MISMATCH` otherwise). Self-modifying code, code modified after the verification of the preceding
instruction and any replacement opcode remain fail-closed; immutable images are unchanged.
