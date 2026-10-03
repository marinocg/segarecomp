# ADR 0079: M68K Analysis Instantiation and Staged-Domain Admission

- Status: Accepted (SEG-030-T001).
- Date: 2026-10-02
- Task: SEG-030-T001..T010 (T001 accepts this ADR; later children append their records below).
- Contract: `docs/architecture/abstract-analysis-core-contract.md` section 8 (M68K instantiation), Accepted by this ADR.
- Related: ADR 0078 (generic core and adapters), ADR 0076 (STOP list; SEG-030 targets the measured blocker), ADR 0077 (executable
  images), ADR 0049 (immutable-copy aliases), ADR 0048 (push-window RTS), ADR 0051 (RTE as the first broad gate), ADR 0053/0054/0055
  (challenger, exact PC-indexed recovery, store-provenance poisoning). Evidence: `docs/architecture/gen3-evidence-ledger.md` sections 2.2
  and 5.

## Context

SEG-029 delivered a CPU-free deterministic solver and a first-consumer M68K adapter whose fixture-level results equal the SEG-026-T002
challenger. SEG-026-T002/T003 measured the remaining M68K gap on the Sonic attract oracle and showed that local heuristics are exhausted
[ledger 2.2]:

- the strict challenger reaches `D` = 6,765 (2.75% of `U` = 246,293) with 42.74% recall and zero recovery escapes (ADR 0054);
- 94.4% of `O - D` lies behind width-only PC-indexed dispatch, and 59 of 70 observed width-only sites read a register-relative field
  whose base is not locally provable (ADR 0054/0055);
- the structural first gate of 5,367 missing PCs (89.2%) is `JSR (An)` (ADR 0054);
- store provenance resolved 0 of 6 state locations: every one was poisoned by stack, exception-frame, unknown-base register-relative,
  indexed and auto-update stores, and excluding the stack did not help (ADR 0055);
- RTE is the first whole-universe family reached in every title (ADR 0051).

SEG-030 instantiates the core for the M68K against these problems. It stays report-only, `precise set OR Unknown`, and must first
reproduce the SEG-026-T002 strict row exactly.

## Decision

1. **Driver placement.** A new report-only directory `platforms/genesis/analysis_report/` holds:
   - the static library `segarecomp_genesis_analysis_report` (alias `segarecomp::genesis_analysis_report`): the executable-image view
     and the driver core;
   - the executable `segarecomp-genesis-analysis-report`.

   Both link `segarecomp::machine_genesis` plus `segarecomp::cpu_m68k_analysis`. Neither is installed. The `segarecomp` CLI and every
   production target never link either, so production admission, emission and generated execution stay unchanged by construction. The
   driver lives under `platforms/genesis` because the roots, mapping claims and alias descriptors are Genesis machine facts; it is a
   separate directory so that the production machine library stays analysis-free. This amends ADR 0078 decision 1 ("only tests link the
   analysis") by exactly this one report-only target pair.

   **The proof is the build graph, not a text scan** (`tests/analysis_build_graph_test.py`, CTest `analysis_build_graph_test`, labels
   `full fast`). It reads the configured build tree through the CMake file API (codemodel v2; CMake 3.27+ adds the query during the
   same configure through `cmake_file_api`, an older CMake makes the test configure a private tree). Analysis targets are those declared
   under `libs/analysis/`, `libs/cpu/<cpu>/analysis/` or `platforms/genesis/analysis_report/` (or carrying an analysis/driver name);
   test targets are those declared under `tests/`; every other non-utility target is production. For each production target the test
   requires that:
   - its transitive dependency closure (through any intermediate target, including a wrapper declared in a `tests/*.cmake` file)
     contains no analysis target;
   - no link fragment names an analysis/driver library;
   - no source and no include directory lies in an analysis directory;
   - no `#include` of its sources, followed transitively through project headers and resolved against the source directory and the
     target's include directories, reaches an analysis header.

   `analysis_build_graph_plant_test` (label `full`) configures copies of the tree and requires the unmodified control to pass and each
   planted bypass of the text scan to fail: a target name assembled in variables, a `tests/*.cmake` wrapper library linked from the
   app, `target_sources` of a driver source, a target name split across a generator expression, a header-only core link spelled
   through a variable, and a relative `#include` of an analysis header. The text scan of decision 2 stays as a fast pre-check only.
2. **Boundary-test amendment** (`tests/analysis_core_boundary_test.py`; a fast text pre-check, not the proof: see decision 1).
   - The analysis-linkage rule now scans every `CMakeLists.txt` **and** `*.cmake`. It admits `libs/analysis/`, `libs/cpu/<cpu>/analysis/`,
     exactly `platforms/genesis/analysis_report/CMakeLists.txt`, and `tests/` (`CMakeLists.txt` and top-level `*.cmake`).
   - A second rule, `segarecomp(::|_)genesis_analysis_report\b|segarecomp-genesis-analysis-report`, is allowed only in that driver
     `CMakeLists.txt` and in `tests/`.
   - That directory declares exactly the library, its alias and the executable, and contains no `install(`.
   - No production source (`apps/`, `platforms/`, `libs/` outside the analysis libraries) includes an analysis or driver header.
   - Planted self-checks: `apps/segarecomp` and `platforms/genesis/machine` are rejected and the driver directory is accepted; a planted
     driver reference in the apps `CMakeLists.txt` is caught; a planted `*.cmake` analysis link is caught; a planted analysis include is
     caught.
3. **Image view.** `GenesisM68kAnalysisImage : M68kAnalysisImage` is built from `genesis_m68k_executable_images(program)`; the view fails
   closed when the set does not validate.
   - **Decode / mapped:** only the `immutable_input` cartridge images and the `static_proof` ADR 0049 alias images at their work-RAM
     execution base. The rules reproduce the challenger's decoder exactly:
     - exactly one program-space mapping claim owns the instruction's first byte, and it is a cartridge image's claim;
     - the span stays inside the cartridge mapping, or inside the alias for an alias PC;
     - no other claim owns any byte of the span;
     - decode profile `general_startup`.
   - **Immutable read:** only `immutable_input` bytes at their cartridge address. An alias execution address is mutable work RAM and is
     never read as immutable, even though its bytes are proven equal to the source when the copy runs.
4. **CPU-owned region vocabulary.** `image(id)`, `work_ram`, `io_device`, `unknown`. The stack is not a separate region: it is `work_ram`
   at the tracked absolute A7 offset. Regions, offsets and points-to live in `libs/cpu/m68k/analysis`; the generic core is unchanged.
5. **Admission under the four-part rule.** Every domain below is CPU-owned in `libs/cpu/m68k/analysis`. No generic change: `solver.hpp`
   and `finite_value.hpp` stay untouched, and CPU sub-reasons (decision 10) map onto the six generic reasons. Abstract memory is not
   CPU-free (it names the stack, exception frames and MOVEM/MOVEP spans), and ADR 0076 forbids a generic memory emulator.

   | domain (child) | consumer | precision problem | why simpler domains are insufficient | observable acceptance |
   | --- | --- | --- | --- | --- |
   | address region + offset, points-to for A0-A7, exact offset set or stride widening only to the region extent (T003) | `JSR/JMP (An)`, `d16(An)`, `(d8,An,Xn)` sites | `JSR (An)` is the first gate of 5,367 missing PCs (89.2%); 59 of 70 width-only sites read a register-relative field (ADR 0054/0055) | the baseline tracks data registers only; an address register is always Unknown | per-family resolved counts with zero escapes on the oracle; fixtures for object-slot loops, code pointers from immutable tables, unknown bases, auto-increment, region overflow, strided-only Unknown |
   | abstract memory with object-field identity and region alias exclusion (T004) | width-only `(d8,PC,Xn)` dispatch fed by object fields | 0 of 6 state locations resolved; every one poisoned by stack, frame, unknown-base, indexed and auto-update stores (ADR 0055) | without regions an unknown-base store must poison all memory; excluding the stack alone resolved nothing | width-only sites becoming exact only from strong-updated cells; fixtures for field dispatch, interfering unknown-base stores, stack exclusion, IRQ writers, initial-memory Unknown |
   | bounded call contexts and summaries (T005) | object pointers passed across calls; the ADR 0054 `index_unknown` via-return site | call continuations are opaque all-Unknown entries; context-insensitive merge loses object identity | a context-free fixed point joins every caller's object region | sites resolved only under contexts, with `context_bound` reported; fixtures for two callers, context exhaustion, recursion, clobbers, unbalanced stacks |
   | exception and return-frame state, interrupt mask and handler instances (T006, `frames`) | RTE/RTR (ADR 0051 first gate), computed RTS and the asynchronous-writer set | RTE/RTR discover nothing under the strict model; push-window RTS is unresolved | the baseline has no stack cells | `rts_computed`/RTE/RTR resolved only from code-built frames; handler-entry frames stay Unknown (`interrupt_resumption`) |

   **Not admitted (candidates):** intervals beyond the finite-set bound (no measured site hit the 4,096 bound); general widening (the
   stride form above widens only to a region extent, never to certainty); pin minimization (ADR 0078 T003 record: cascaded
   over-invalidation, a precision loss only); selective symbolic execution (ADR 0076: later evaluation only).
6. **Context bound.** Call string of depth k = 1, encoded in the adapter's opaque 64-bit point: `point = (ctx << 24) | pc`, with
   `ctx = 0` (no context) or the call-site PC + 1. `D` is the set of the low 24 bits of reached points. At most K = 8 contexts per callee
   entry; beyond K the callee is merged into context 0 in the next driver round and reported `context_bound` (generic `state_bound`).
   SEG-030-T006: with the `frames` domain the context's top byte is the partition tag (0: the main flow; a handler instance; 254: the
   handlers without an analysed instance, kept for `D`): `point = (tag << 48) | (ctx << 24) | pc`. Calls stay in their partition and
   a merged callee is merged per partition.
7. **Asynchronous writers.** The machine-delivered vector roots (`genesis_reachability_roots`, shared with the challenger) are
   potential asynchronous writers.
   - The asynchronous cell set is the union of the store cells over the code reachable from those roots in `D` (every edge of the
     final solution, calls, continuations and computed edges included). Asynchronous cells are never strong-updated and read
     `Unknown(async_writer)` in every context, except possibly inside the one handler that writes them, and only when that handler can
     be neither re-entered nor preempted by another writer of the cell. SEG-030 does not take that exemption: an asynchronous cell is
     Unknown in the writing handler too.
   - An Unknown-target store in handler code poisons all of work RAM (every cell becomes asynchronous).
   - **Nesting.** TRAP, illegal-instruction, CHK, TRAPV, divide-by-zero and the other synchronous vectors do not raise the interrupt
     mask, so an interrupt may arrive inside their handlers, a TRAP may execute inside handler code, and handler code may lower the mask
     itself. The entry A7 of a handler is therefore the join of the absolute A7 over **every** point that can be interrupted or can raise
     an exception, handler points included, minus the frame; it is Unknown when any of them is Unknown. Without interrupt-mask tracking
     every boundary is interruptible, and the join over handler points (each entry A7 minus a further frame) never converges, so the
     handler entry A7 is Unknown. Consequence: when interrupts can be taken, the nested exception frame is a handler store through an
     Unknown address, and every work-RAM cell is asynchronous. The implementation applies exactly this consequence for the Genesis
     driver (`M68kMemoryConfig::interrupts`).
   - **Interrupt and handler-stack precision (SEG-030-T006, implemented as the staged `frames` domain).** Without `frames` the
     consequence above applies unchanged. With `frames` (CPU-owned in `libs/cpu/m68k/analysis`, record T006):
     - **Implemented.** SR status tracking (S and I2-I0) through MOVE/ANDI/ORI/EORI to SR, STOP, proven RTE and the 68000 reset state
       (S = 1, I = 7, SSP = the long at vector 0); per-boundary interrupt eligibility (level > mask, level 7 and unknown levels always);
       the 6-byte group 1/2 frame at A7 - 6 only when S = 1 is proven; one analysed partition per handler instance (handler, parent
       partition), entered with the accepted mask and the frame address, so that handler code is never joined with the code it
       preempts; nesting and preemption per the mask (a handler on its own chain, deeper than the depth bound, inside a non-resuming
       instance, or with an Unknown frame address is not analysed); per-partition asynchronous writers (the stores, interrupt frames
       and asynchronous writers of the resuming child instances: interrupts and divide-by-zero/CHK/TRAPV); a frame-integrity check
       (a child that may rewrite its saved SR makes the parent's status Unknown after the boundaries where it can be taken); RTE/RTR
       and an RTS away from the entry stack delta resolved only from code-built frame cells.
     - **Still Unknown.** An unanalysed resuming child makes its parent's writers every cell. A handler RTE never resumes anywhere the
       analysis names (`interrupt_resumption`). The frame address after a call into a callee with an unknown effect, or an unbalanced
       callee, is Unknown (its A7 is not restored); a balanced callee (a summary or a proven merged callee) returns at the caller's
       own A7. An instruction that always raises only non-resuming synchronous vectors ends its path and is not an unknown effect;
       an unresolved computed site and a TRAP/TRAPV whose continuation is not modelled stay unknown effects. The status of an opaque
       continuation is its own partition's status bound (relative to the closure premise of decision 8), never another partition's. Register preservation across an interrupt remains the existing resumption
       premise; only the saved SR is checked.
     - **Two premises, two consumers.** *Discovery* (`D`, roots, recall, analysed handler instances) keeps the challenger's machine
       root premise: only the delivered vectors of ADR 0021 / ADR 0043 (level-6 autovector and the synchronous vectors) are roots and
       analysed handlers. *Asynchronous writers, interrupt eligibility and status* use the hardware premise: real hardware delivers
       the level-4 (VDP H-interrupt) and level-2 (external, I/O port) autovectors once the program enables their source through
       device registers, which the analysis does not model, so non-delivery is never proven. Every installed interrupt vector the
       machine model does not deliver (the spurious vector 24 and the autovectors 25-31 other than IRQ6;
       `GenesisReachabilityRoots::potential_interrupts`, `M68kFrameConfig::potential_interrupts`) is therefore a potential
       asynchronous source wherever its level is eligible (level > mask; level 7 and the spurious vector always): an unanalysed
       resuming interrupt, so that partition's asynchronous writers are every cell and its status is Unknown after those boundaries
       (frame integrity is not proven for code the analysis never visits). Its handler is never seeded, so `D` is unchanged. Narrowing
       this to the levels the board can assert, or analysing those handlers as instances, would be a separate precision step.
   - Handler entry: registers and work RAM Unknown, A7 as above.
   - Initial work RAM is Unknown (`initial_memory`). ADR 0055's zero-reset assumption is not reused: real hardware leaves work RAM
     undefined.
   - VDP DMA only reads 68K memory (memory-to-VRAM/CRAM/VSRAM; fill and copy stay inside VDP memory), so it never writes 68K work RAM
     (`docs/references/genesis-vdp-dma-contract.md`, GTO1 section 7).
   - **Z80-originated writes into 68K work RAM are real.** The Z80 writes 68K work RAM (`$E00000-$FFFFFF`, 64 KiB mirrored)
     through its `$8000-$FFFF` bank window when the 9-bit bank register is at least `$1C0`.
     - The project machine model routes these writes (`platforms/genesis/runtime/z80_machine.c`).
     - Sources: `docs/architecture/genesis-z80-audio-contract.md` (bank-window section, U3), which cites ARES ("the APU can write to CPU
       RAM"), GPGX and the Sega *Genesis Technical Overview* section 4.
     - Whether banked reads work is disputed, but that does not matter here: this model only concerns writers.
     - YM2612, PSG, controller/IO and the Z80-control registers are bus slaves and never write 68K RAM.
     - Sega CD and 32X add-on bus masters are unsupported and explicitly excluded.
   - **Credited policy.** Z80 code and the bank value are runtime state. Until the SEG-030-T010 proof below succeeds, the exclusion
     relies on the power-on state: the Z80 is held in reset (`/RESET` asserted, BUSREQ not
     requested; `docs/architecture/genesis-z80-audio-contract.md` section 4 rule 1, citing GPGX `zstate = 0` and ARES `resLine = 0`),
     so it cannot run, and cannot write 68K RAM, until the 68K writes the Z80 control block. A store in `D` counts as a potential
     release when its target may alias the Z80 control block (memory mode, BUSREQ `$A11100`, RESET `$A11200`; the driver uses the whole
     `$A11000-$A11FFF` block to cover any partial-decode mirror) **or has an Unknown target** (an undescribed writer included), whatever
     value it stores. If any such store exists:
     - every mutable work-RAM cell is potentially written externally at every program point;
     - every read of mutable work RAM is `Unknown` (`external_writer` -> `unknown_input`), and no work-RAM strong-update fact survives.
   - **Exclusion.** A program that provably never releases the Z80 (no such store in `D`, under the closure premise) excludes
     external writers; so does a successful T010 proof below.
   - **Z80 store-freedom proof (owned by SEG-030-T010, a new bounded task; pending).** This is a SEG-030 precision blocker, not a
     future candidate. T010 attempts a conservative static proof that no reachable Z80 store can target 68K work RAM while the analysed
     program runs. Inputs:
     - the 68K release/BUSREQ/RESET state;
     - the known materialized Z80 executable images (ADR 0077 / SEG-028 model; Z80 support from SEG-008/009/032/033);
     - the Z80 bank-register writes;
     - the Z80 store instructions and their targets.

     Acceptable proof shapes: the bank register never reaches a 68K-RAM-selecting value; no reachable Z80 store uses the banked
     `$8000-$FFFF` window; Z80 stores into the window occur only while BUSREQ/reset prevents Z80 execution; or a bounded finite Z80
     control/data proof over the materialized images. A full second Z80 value-set analysis is not built unless evidence requires it.
     Runtime observation is never proof; it may only falsify. If the proof does not succeed, `external_writer` stays true with a
     recorded reason. A successful proof must carry a mutation test in which changing the relevant bank or store fact makes the proof
     fail.
   - **Premise ablation.** A labelled ablation that assumes no Z80 work-RAM writes (`--assume-no-z80-ram-writes`) may be reported for
     sensitivity only. It is diagnostic-only, never credited, never mixed into `D`, recall or credited counts, and distinct from the
     T010 proof.
8. **Closure premise.** Every fact is relative to the discovered set `D`. Runtime escapes on the coverage oracle are the falsifier;
   nothing observed is fed back. Every memory-derived resolution is reported next to the count of unresolved sites it depends on.
9. **Driver rounds.** A monotone configuration (pinned sites, asynchronous cells, callee summaries, merged callees) grows as
   `config_{r+1} = config_r ⊔ computed(r)`, at most R = 16 rounds. A mandatory final validation checks that the values derived from the
   final solution are below the configuration used to compute it. Non-convergence switches the dependent domain off and reports
   `iteration_bound`. The baseline configuration has nothing to grow and converges in one round (the solver-owned pin-and-restart runs
   inside it).
10. **Report families.** `pc_index_explicit`, `pc_index_width_only`, `jsr_an`, `jmp_an`, `jsr_d16_an`, `jmp_d16_an`, `jsr_an_index`,
    `jmp_an_index`, `rte`, `rtr`, `rts_computed` (plus `unclassified`). Each family reports sites, resolved, and Unknown by generic
    reason x CPU sub-reason. Sub-reasons: `none`, `base_unknown`, `region_exit`, `set_bound`, `target_outside_image`, `width_only`,
    `store_poison`, `async_writer`, `initial_memory`, `external_writer`, `context_bound`, `stack_unbalanced`, `frame_unproven`,
    `interrupt_resumption`, `invalidated`. Ordinary RTS is modelled through call continuations and is not a computed site.
11. **Resource constants.** Solver 1,000,000 iterations and 2^20 points (unchanged, ADR 0078 decision 4); finite set 4,096; points-to at
    most 8 `(region, offset-set)` pairs; exact offset set at most 64, else strided; memory at most 512 cells per state (overflow: memory
    Unknown, `state_bound`); K = 8; R = 16. SEG-030-T006: handler-instance chain depth at most 3, at most 253 instance tags (beyond:
    the frames domain fails closed, `state_bound`), an instance's entry A7 widened to Unknown after 4 growing rounds; the frames
    rounds run after at most R warm-start contexts rounds and are themselves bounded by R.
12. **ADR 0076 STOP check.**
    - Universal CPU IR: no; the adapter reads `M68kIrOperation` and CPU effect owners only.
    - Generic memory emulator in the core: no; abstract memory is CPU-owned and bounded.
    - Banking in generic domains: no; regions are CPU-owned and Genesis has no 68K banking here.
    - Epoch/signature concepts in generic image types: no; the view consumes ADR 0077 types unchanged.
    - Arbitrary self-modifying code: no; only `immutable_input` and `static_proof` bytes execute.
    - Whole-program path-sensitive symbolic execution: no; one flow-sensitive state per point and context.
    - Mandatory SMT: no.
    - Per-game schemas or target metadata: no; no title-specific list or branch.
    - Unbounded context sensitivity: no; k = 1, K = 8.
    - All-or-nothing alias precision: no; unresolvable stores poison a region or all memory and are reported, never assumed away.
    - Deleting broad AOT: no; report-only.
    - **Title quorum:** the SEG-031 gate needs Sonic plus at least one further title with a reproducible workload. Four further titles
      have a 23,200-frame no-input oracle (Sonic 2, Cool Spot, OutRun, Streets of Rage); Golden Axe stops early and counts as static-only.

## Consequences

- T002 lands the report-only driver and the oracle-level regression baseline before any domain is credited.
- T003..T006 add the admitted domains in `libs/cpu/m68k/analysis`, each inert when its flag is off, so the baseline stays reproducible.
- T006 (extended) and T010 own the two environmental writer proofs of decision 7 (interrupt/handler-stack precision; Z80 work-RAM
  store freedom). T006 is implemented as the `frames` domain (record T006); T010 is pending.
- A domain whose child cannot meet its observable acceptance stops and records why; zero measured gain is an acceptable recorded result.
- **Accepted tradeoff.** Complexity is allowed when it is required for sound, general static recovery, provided it stays bounded,
  deterministic, CPU-owned where appropriate, evidence-driven and fail-closed. The invariant is "precise proof OR typed Unknown": a
  fact that cannot be proven stays Unknown with its reason and is never excluded unsoundly. No speculative machinery is added without a
  concrete consumer.

## Records

### T002: report-only driver and oracle-level baseline

- `genesis_reachability_roots(const FrontendProgram &)` and `genesis_push_window_rts(...)` are now public in the challenger's
  translation unit and shared by the challenger and the driver. The challenger's CLI private output on Sonic is byte-identical before
  and after the extraction.
- Driver: `segarecomp-genesis-analysis-report --rom --rom-sha256 (--reset-entry | --entry --mapping-base) [--immutable-copy-alias]...
  --private-output [--universe] [--domains baseline|address,memory,contexts,frames|all] [--compare-challenger] [--metrics-output]
  [--max-iterations N] [--max-points N]`. The digest is verified. stdout carries sanitized aggregates only; wall time and peak RSS go
  only to `--metrics-output`. The private format `segarecomp.m68k_core_report.private.v1` is a superset of the challenger's v1 and is
  consumed unchanged by `tools/reachability_coverage_compare.py`. Staged domain flags are rejected until their children land.
- **Baseline (Sonic attract oracle, unchanged; `--domains baseline --universe --compare-challenger`).**

  | measure | SEG-026-T002 challenger | M68K core driver |
  | --- | --- | --- |
  | `U` | 246,293 | 246,293 |
  | `D` (D/U) | 6,765 (2.75%) | 6,765 (2.75%) |
  | `O ∩ D` / `O - D` / `D - O` | 4,493 / 6,019 / 2,272 | 4,493 / 6,019 / 2,272 |
  | observed recall | 42.74% | 42.74% |
  | resolved PC-index sites / escapes | 4 / 0 | 4 / 0 |
  | overlapping starts / rejected / exception-raising decodes | 70 / 1 / 31 | 70 / 1 / 31 |

  The in-process comparison reports 0 core-only and 0 challenger-only PCs, 0 differing PC-index sites and 0 per-family
  unresolved-site differences; the compare-tool reports of both private outputs are identical. Two runs are byte-identical (private and
  stdout). Cost on the dev (Debug) build: about 48 s and about 300 MB peak RSS, dominated by finite-set copies in the solver.
- Fixtures (`analysis_report_test`, `analysis_report_driver_test`): alias execution, alias reads never immutable (the same dispatch
  resolves in the cartridge and fails closed from the alias), an instruction crossing the alias end rejected, roots helper equal to the
  challenger, baseline equal to the challenger on the fixture, bounds exhaustion with no partial `D`, fail-closed CLI input, and the
  private output consumed by the compare tool with zero escapes on a synthetic coverage directory.

### T003: address region plus offset and register points-to

- **Domain** (`libs/cpu/m68k/analysis/address_value.{hpp,cpp}`, CPU-owned; the generic core is unchanged). Each of A0-A7 holds
  bottom, at most 8 `(region, offset set)` pairs, or Unknown with a generic reason and a CPU sub-reason
  (`M68kAnalysisSubReason`, the decision 10 vocabulary; the report's sub-reason type is now an alias of it). A region is an extent of
  32-bit register values whose bus addresses lie in one machine region; the Genesis view reports the cartridge image (the unique
  owning `immutable_input` claim), work RAM (`$E00000-$FFFFFF`; alias execution addresses are work RAM) and the I/O/device window
  (`$A00000-$DFFFFF`). An offset set is exact (at most 64) or strided `{lo, stride, hi}`. Offsets stay inside `[0, size]` (one past
  the end is admitted). Arithmetic leaving that range is `Unknown(region_exit)`, never a clamp.
- **Widening.** Joins keep the tight hull and the gcd congruence. A strided set that keeps growing widens after 64 strict growths
  (`m68k_strided_growth_bound`, a new CPU resource constant), and only to the region extent with its congruence. It never widens to
  certainty, and a strided set is never enumerated into targets.
- **Transfer** (only when `M68kAnalysisConfig::domains.address`; with the flag clear every An stays `Unknown(unknown_input)`):
  - `LEA` (absolute, PC-relative, `(An)`, `d16(An)`, `(d8,An,Xn)`; the register-relative forms keep region and congruence);
  - `MOVEA` from An, Dn, an immediate or exact immutable image bytes (word entries sign extended; odd word/long addresses
    excluded; any non-immutable byte gives `non_immutable_read`);
  - `ADDA`/`SUBA`/`ADDQ`/`SUBQ` to An, `EXG`, and the `(An)+`/`-(An)` auto-updates of size-exact kinds (a byte access through A7
    steps 2);
  - the MOVEM base update;
  - a `CMPA #imm,An` + `Bcc` edge filter (exact sets through the shared subtraction/condition owners; strided sets by an unsigned
    interval only).

  Every other An writer is Unknown. Where the effect owner does not claim a complete footprint, the An writes come from the
  M68000PRM entries: `LEA` writes its An; `MOVEQ`, `DBcc`, `EXT`, `SWAP`, `MULx` and `DIVx` write only a Dn; `JSR`/`BSR`/`PEA`
  write only A7; any other incomplete kind writes all of A0-A7. With the domain enabled, `LEA`/`PEA` also keep the data registers.
  The data owner reports them as writing every Dn; the baseline is unchanged.
- **Sites.** `JMP/JSR (An)`, `d16(An)` and `(d8,An,Xn)` resolve only from an exact set. Each target must be an even, mapped image PC.
  Odd targets are excluded and counted. One unmapped target fails the whole site (`non_immutable_read/target_outside_image`).
  Other Unknown outcomes:
  - a strided base: `set_bound/set_bound`;
  - a width-only index or entry under the strict policy: `width_only`;
  - an Unknown base: its reason, with `base_unknown`;
  - a solver-pinned site: `invalidated`.

  Computed edges use the existing pin-and-restart. Resolved sites leave the unresolved-site lists.
- **Compare tool.** `computed_site_escape_check` falsifies every resolved computed site per report family. It counts first entries
  whose witness predecessor is the site, both retire and interrupt-resumption witnesses. The existing `pc_index_recovery_check`
  output is unchanged.
- **Fixtures** (`analysis_m68k_value_test`, `analysis_report_test`, driver test):
  - an object-slot loop (one work-RAM region, stride kept, exact exit);
  - an unguarded walk ending `region_exit`;
  - a code pointer from an immutable table;
  - `d16(An)` and `(d8,An,Xn)` JSR/JMP;
  - an unknown base (mutable load, unwritten register);
  - auto-increment, predecrement, A7 byte push, `ADDA.W` and `EXG`;
  - image-region overflow and one-past-the-end;
  - a strided-only set staying `set_bound` with no member taken;
  - a width-only index (strict rejected, measurement admitted);
  - one target outside the image failing the whole site;
  - Genesis region extents;
  - the baseline inert;
  - deterministic outputs.
- **Sonic attract oracle (report-only, sanitized).**

  | measure | `--domains baseline` | `--domains address` |
  | --- | --- | --- |
  | `D` (D/U) | 6,765 (2.75%) | 6,765 (2.75%) |
  | `O ∩ D` / `D - O` / recall | 4,493 / 2,272 / 42.74% | 4,493 / 2,272 / 42.74% |
  | resolved computed sites / escapes | 4 / 0 (`pc_index_explicit`) | 4 / 0 (`pc_index_explicit`) |
  | `jsr_an` sites: resolved / Unknown | 0 / 2 | 0 / 2: `unsupported_transfer/base_unknown` 1, `unsupported_transfer/region_exit` 1 |
  | `jmp_an` sites: resolved / Unknown | 0 / 1 | 0 / 1: `unknown_input/base_unknown` |
  | `d16(An)` / `(d8,An,Xn)` sites | 0 | 0 |
  | solver iterations | 110,079 | 159,181 |

  The baseline private and aggregate outputs are byte-identical to the T002 baseline. Two address-domain runs are byte-identical.
  The measured gain is zero, which is a valid recorded result (ADR 0055). Both `JSR (An)` sites take their pointer from a
  PC-relative table whose index comes from a mutable-memory byte: one index is Unknown and the other spans entries that are not
  pointers. The `JMP (An)` base enters through an opaque entry. The remaining gate is therefore memory and object-field
  provenance plus calling context (T004/T005), not the address arithmetic itself.

### T004: abstract memory, object-field identity and alias exclusion

- **Phase A corrections.**
  - Decision 1 now names the build-graph check as the proof (planted bypasses all caught; the text scan is a pre-check).
  - Decision 7 now covers nested handlers (entry A7 joined over every interruptible or raising point, handler points included), the
    asynchronous-cell rule in every context (the writing-handler exemption is permitted but not taken), and the Z80 release rule (an
    Unknown-target store or any store that may alias the control block; the power-on held-in-reset state is the cited basis).
  - A solve that exhausts a bound makes the driver exit 3 (`solver.complete` false). `tools/reachability_coverage_compare.py`
    rejects such a report (exit 4).
  - The driver reads at most the image size limit before hashing (a device or an oversized file is rejected). It rejects a repeated
    `--max-iterations`/`--max-points`.
  - `GenesisM68kAnalysisImage::immutable_read` admits only 1, 2 or 4 bytes, like the flat view; the CPU owners never request
    another width, so the challenger equality is unaffected.
  - The T002 equality with the challenger holds for valid executable-image sets. An invalid alias set fails closed for the whole set
    (the driver exits 1 with no report); it never degrades to a partial view.
- **Domain** (`libs/cpu/m68k/analysis/abstract_memory.{hpp,cpp}`, CPU-owned; the generic core is unchanged).
  - Cells are `(region, physical offset, width 1/2/4)` of work RAM only. The physical offset folds the 64 KiB mirror and the
    register upper byte (a new `mirror` field of the region extent).
  - A cell holds a precise finite value or a points-to value. An absent cell is Unknown, with `initial_memory`, `store_poison` or
    `set_bound` as the reason. The join is the pointwise join of the common cells. Over 512 cells drops every cell (`state_bound`).
  - A singleton physical target is a strong update; any other exact target is weak (a matching cell is joined, every other
    overlapping cell is removed). A strided target is a weak summary update of its congruent members, never enumerated. Reading a
    stride over uninitialized members stays Unknown.
  - An Unknown target, or a spill past a region end, poisons every cell. A wrap of the mirror removes the region.
  - Alias exclusion is structural: a store touches only the cells its target set overlaps.
- **Writer description** (`m68k_memory_writes`, from the M68000PRM entries).
  - Every lifted kind is listed; an undescribed kind poisons every cell. On Sonic `D` there are 0 undescribed writers.
  - MOVE stores its source value and CLR stores zero, both at the operation size.
  - Read-modify-write forms store an Unknown value at the operation size (byte-only and word-only forms at their architectural
    width). MOVEP spans `2 * size` bytes and MOVEM `count * size` bytes (below An for `-(An)`).
  - JSR/BSR push their return address and PEA its effective address below A7; LINK pushes an Unknown value.
  - DIVx/CHK/TRAPV/TRAP/STOP and instruction exceptions write a frame at an Unknown supervisor-stack address, because supervisor mode
    is not proven.
  - The destination address is computed after the source's `(An)+`/`-(An)` update (`MOVE.L (A0)+,(A0)`).
- **Reads.** MOVE/ADD/SUB/AND/OR into Dn, and MOVEA, read precise cells. The data owner sees a substituted source register that
  carries the cells' value; this needs equal and complete effect footprints, and the owner itself is unchanged. Without precise cells
  the original operation is used, so a byte stays width-only 0..255. Code pointers stored in cells feed T003's `(An)` sites.
- **Policy rounds** (decision 9, R = 16, final validation = convergence).
  - The asynchronous ranges are the store cells of the code reachable from the handler roots. A handler store through an Unknown
    address makes every cell asynchronous, and so does the interrupt consequence of decision 7, which applies to the Genesis driver.
  - The external writer is derived from any release store. Non-convergence would switch the memory domain off (`iteration_bound`).
  - `--domains memory` implies `address`. `--assume-no-z80-ram-writes` is the labelled, never-credited premise ablation.
  - An address-only comparator run reports the sites resolved only with memory (decision 8).
- **Fixtures** (`analysis_m68k_memory_test`, plus driver determinism and labels in `analysis_report_driver_test`):
  - lattice and store semantics, including mirror/upper-byte aliasing, strong/weak updates, the summary cell over uninitialized RAM
    (`initial_memory`), the policy and the cell bound;
  - field dispatch with a complete store set resolving exactly (width-only without the domain);
  - an Unknown-base store poisoning the field (`store_poison`);
  - pushes and PEA at a known A7 excluded (and poisoning through an Unknown A7);
  - an IRQ-handler writer (`async_writer`: everything under interrupts, the handler's cell only for synchronous handlers, a disjoint
    handler store leaving the field);
  - a nested TRAP handler plus IRQ6 reading the IRQ-written cell (Unknown with interrupts on and off);
  - the `(A0)+,(A0)` self-alias;
  - a store-derived proof invalidated by the code it exposed (pinned, target not discovered);
  - a Z80 release store (`external_writer`), its ablation (diagnostic resolution), and a never-releasing program keeping its facts;
  - determinism, and the domain inert when off.
- **Sonic attract oracle (report-only, sanitized).**

  | measure | baseline | address | memory (credited) | memory + ablation (diagnostic) |
  | --- | --- | --- | --- | --- |
  | `D` / `O ∩ D` / `D - O` / recall | 6,765 / 4,493 / 2,272 / 42.74% | same | same | same |
  | resolved computed sites / escapes | 4 / 0 | 4 / 0 | 4 / 0 | 4 / 0 |
  | `pc_index_width_only` resolved / Unknown | 0 / 9 (`width_only`) | 0 / 9 | 0 / 9 | 0 / 9 |
  | `jsr_an` / `jmp_an` resolved | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
  | external writer / every cell asynchronous | - | - | yes (Z80 release store present) / yes | no (assumed) / yes |
  | work-RAM reads: precise / `external_writer` / `async_writer` | - | - | 0 / 412 / 0 | 0 / 0 / 411 (+1 `store_poison`) |
  | other memory-operand reads Unknown (`base_unknown`, `region_exit`) | - | - | 597 | 597 |
  | rounds / final-solve iterations | 1 / 110,079 | 1 / 110,712 | 2 / 159,149 | 1 / 159,149 |
  | wall / peak RSS (Debug, includes comparator) | 49 s / 299 MB | 50 s / 297 MB | 156 s / 317 MB | 105 s / 309 MB |

  The baseline outputs are byte-identical to the T002 baseline. Two credited runs and two ablation runs are each byte-identical. The
  comparator reports 0 sites resolved only with memory and 0 lost, with 13 unresolved sites next to them. The measured gain is zero,
  as decision 7 predicts.

  Under the credited model, the program releases the Z80, so every work-RAM read is `external_writer`. The ablation shows that
  removing that premise would not help: interrupts can preempt any boundary, the handler stacks therefore have Unknown addresses,
  and every cell is asynchronous.

  Disposition:
  - T004's local abstract-memory semantics (cells, strong/weak updates, alias exclusion, writer description, policy rounds) are
    accepted.
  - The credited Sonic gain is zero because of two environmental writer proofs that do not yet exist: possible Z80 stores into work
    RAM (`external_writer`, 412 reads) and unbounded interrupt/handler-stack aliasing (`async_writer`, 411 reads plus 1 `store_poison`
    under the ablation).
  - These are SEG-030 precision blockers, not postponed work: T006 (extended) owns the interrupt/handler-stack proof and T010 owns the
    Z80 store-freedom proof (decision 7).
  - SEG-030 must attempt a bounded sound proof for both.
  - A proof that does not succeed leaves the affected reads Unknown with their reason; it never becomes an unsound exclusion. A zero
    or modest gain after both proofs have been attempted is valid recorded evidence.

### T005: bounded call contexts and callee summaries

- **Points and contexts** (`libs/cpu/m68k/analysis/finite_adapter.{hpp,cpp}`, CPU-owned; the generic solver is unchanged).
  - With `M68kAnalysisConfig::domains.contexts` (it implies `memory`, which implies `address`), a point is `(context << 24) | pc`, as
    decision 6 states. Context 0 holds the roots and the merged callees; a call site's context is its PC + 1 (k = 1).
  - A call edge (BSR/JSR, and the computed targets of `JSR (An)`/`JSR (d8,PC,Xn)`) enters the callee in the call site's context.
  - Every other edge stays in the current context.
  - Reports stay per PC. A site is resolved only when it resolves in every context that reaches it; its targets are then the union.
    Otherwise the first unresolved context's outcome (in point order) is reported. A work-RAM read is precise only when it is
    precise in every context.
- **Stack delta.** A CPU-owned scalar of the state: A7 minus A7 at the entry of the current activation, modulo 2^32.
  - It is an exact set of at most 64 values, else Unknown.
  - Roots and callee entries start at 0. Every A7 writer goes through the T003 address transfer, with A7 a synthetic exact pointer.
    This covers pushes, pops, the byte step of 2, MOVEM, `LEA d16(A7),A7` and `ADDA`.
  - PEA subtracts 4.
  - SR writers (a possible supervisor/user stack switch), LINK and UNLK make the delta Unknown.
  - A call's continuation keeps the caller's delta.
  - **Correction (T006 review).** A resuming exception's continuation and a pushed code address's continuation start at an Unknown
    delta, never bottom. They are reached only after code the activation does not summarise (a handler ending in RTE, an RTS through
    a pushed address) whose effect on A7 is never proven. A bottom delta joined away at the next RTS, so with
    `exception_continuations` a path that popped its caller's return slot after a TRAP passed as balanced and produced a precise
    but wrong summary. The typed per-PC queries `m68k_query_data_register`/`m68k_query_address_register` join every point of the PC
    (every context and partition) instead of reading the context-0 point alone. Both have fixtures and killed mutants.
  - With contexts only, JSR/BSR/PEA also move A7 by -4 in the address domain. The callee is then entered at an exact frame, and its
    summary returns at one. The T003/T004 address transfer is unchanged when the domain is off.
- **Activations and proofs** (each round, from the solution).
  - An activation is either a call-site context (all of its points) or a merged callee. A merged callee is walked from its
    context-0 entry over every non-callee edge.
  - An activation is **unproven** in four cases:
    - an RTS not at delta {0}, or an RTE/RTR (`stack_unbalanced`);
    - an unresolved or pinned computed site, a TRAP/TRAPV whose exception continuation is not modelled, an exception-raising
      instruction or an undecodable point (an unknown effect, `none`);
    - membership in a cycle of the activation graph (recursion, `context_bound`);
    - an unproven nested activation (its reason is inherited).
  - The proof uses the strongly connected components of the activation graph. Each component is resolved after the components it
    reaches.
  - A proven call-site context with at least one exit has a **summary**: the join of the input states at its exits (RTS at delta
    0), with the return address popped (A7 + 4).
    - The summary holds the register values, A0-A7 points-to and the whole abstract memory, so a callee's strong, weak, strided
      and poisoning stores, and the async/external policy, all reach the continuation.
    - The continuation receives the summary of the call-site context, with the caller's own stack delta.
  - Otherwise the continuation is opaque, typed by the context's sub-reason:
    - `context_bound` for a merged callee or a recursive activation: generic `state_bound` in registers, A0-A7 and absent memory
      (`m68k_memory_generic_reason` maps `context_bound` to `state_bound`);
    - `stack_unbalanced`: generic `unsupported_transfer`;
    - the T004 opaque entry for an unresolved callee or an unknown effect.
  - An unresolved PC-indexed index whose generic reason is `state_bound` is reported `context_bound`. Only a merged or recursive
    continuation produces it.
- **Bounds** (decision 6 applied, with one documented correction).
  - The starting merges are the callee entries that have more than K = 8 distinct call sites in the context-free (T004) solution.
  - A round in which a callee reaches more than K contexts merges it into context 0 for the next round and is not valid.
  - So a returned round never exceeds K (`max_contexts_per_callee` is reported).
  - A merged callee's continuation is Unknown(`context_bound`).
  - **Correction:** the merged callee's balance is still checked, from its context-0 walk, so its callers are not made unproven by
    the merge alone.
- **Rounds** (decision 9 applied, with one documented correction).
  - The T004 result (contexts off) is computed first. It is the starting memory policy, the comparator, and the fallback.
  - Each contexts round solves under the current configuration (memory policy, merges, summaries, opaque labels), then derives the
    configuration its own solution implies.
  - A round is **valid** when all three hold:
    - every summary it used is at or above the derived one, compared on precision content (Unknown labels are ignored);
    - no callee exceeds K;
    - the derived memory policy is at or below the used one.
  - A valid round's configuration is a post-fixed point, so its solution is sound.
  - The memory policy and the merges grow by join, as in T004.
  - **Correction:** summaries are not join-only.
    - A valid round replaces each summary with the more precise derived one; if they differ only in Unknown labels, the used one
      is kept.
    - A summary that a newly discovered writer exceeds is joined with the derived one, never kept. This is how invalidation enters
      the round and pin-and-restart model.
    - A join-only chain cannot do this: its first, opaque-continuation summaries would bound every later round.
  - The run stops at a valid round that reproduces its own configuration (`converged`).
  - At R = 16 rounds the latest valid round is returned (`converged` false).
  - With no valid round, or a contexts solve that exhausts a solver bound, the contexts domain is switched off. The T004 result is
    returned, with the reason recorded.
  - Contexts rounds use the solver's own pin-and-restart unchanged.
- **Driver.** `--domains contexts` is accepted (`frames` was still rejected until T006).
  - The aggregate adds `context_depth`/`context_bound` to the bounds, plus a `contexts` object: validation, rounds, contexts,
    merges, activations, summaries, unproven and opaque continuations by sub-reason, and a memory-only comparator.
  - `--domains baseline` and `--domains memory` (credited and ablation) are byte-identical to T002/T004 on Sonic: private,
    aggregate and compare-tool outputs.
- **Fixtures** (`analysis_m68k_contexts_test`, plus the deterministic contexts run in `analysis_report_driver_test`):
  - two callers with different objects resolving one handler site per context;
  - a balanced return keeping the caller's index (the ADR 0054 shape, `index_unknown/unknown_input` without contexts);
  - a callee field clobber, strong and conditional (memory effects in the summary);
  - an unbalanced callee and its caller (`stack_unbalanced`);
  - an unresolved callee and an unresolved exit (an unknown effect);
  - recursion (`context_bound`, `state_bound`);
  - K and K + 1 call sites (summaries kept, then a merge into context 0);
  - summary invalidation after a newly discovered writer. It converges at a later round with D3 {4, 8}, and every lower round bound
    returns a valid round, never the stale precise D3 = 4;
  - a handler subroutine's store, discovered only with contexts, entering the asynchronous set;
  - the stack delta across push, MOVEM, PEA and pops;
  - determinism, and the domain inert when off.
- **Mutations.** `analysis_mutation_test` now also builds the contexts fixture. Six new mutants are killed:
  - a stale summary accepted;
  - a stale summary kept;
  - an unbalanced RTS summarized;
  - recursion not `context_bound`;
  - summary memory effects dropped;
  - the writer set ignoring callee-context stores.
- **Sonic attract oracle (report-only, sanitized; Release build of the same sources, comparator runs included).**

  | measure | baseline | memory (credited) | contexts (credited) | contexts + ablation (diagnostic) |
  | --- | --- | --- | --- | --- |
  | `D` (D/U) | 6,765 (2.75%) | 6,765 (2.75%) | 6,806 (2.76%) | 6,806 (2.76%) |
  | `O ∩ D` / `O - D` / `D - O` | 4,493 / 6,019 / 2,272 | same | 4,534 / 5,978 / 2,272 | 4,534 / 5,978 / 2,272 |
  | observed recall | 42.74% | 42.74% | 43.13% | 43.13% |
  | resolved computed sites / escapes | 4 / 0 | 4 / 0 | 5 / 0 | 5 / 0 |
  | `pc_index_explicit` resolved / Unknown | 4 / 1 (`unknown_input`) | 4 / 1 | 5 / 0 | 5 / 0 |
  | `pc_index_width_only` resolved / Unknown | 0 / 9 (`width_only`) | 0 / 9 | 0 / 9 | 0 / 9 |
  | `jsr_an` resolved / Unknown | 0 / 2 | 0 / 2 (`base_unknown` 1, `region_exit` 1) | same as memory | same as memory |
  | `jmp_an` resolved / Unknown | 0 / 1 | 0 / 1 (`unknown_input/base_unknown`) | same | same |
  | work-RAM reads: precise / `external_writer` / `async_writer` | - | 0 / 412 / 0 | 0 / 428 / 0 | 0 / 0 / 419 |
  | other work-RAM-or-Unknown reads Unknown | - | 597 (`base_unknown` 535, `region_exit` 62) | 577 (`base_unknown` 467, `region_exit` 77, `context_bound` 16, `stack_unbalanced` 11, `set_bound` 6) | 586 (as credited, `context_bound` 25) |
  | contexts: call-site contexts / merged callees / max per callee | - | - | 335 / 12 / 8 | same |
  | activations: balanced / recursive / summaries | - | - | 347: 272 / 1 / 263 | same |
  | continuations: summary / opaque `context_bound` / `stack_unbalanced` / unknown | - | - | 431 / 233 / 49 / 49 | same |
  | rounds (memory + contexts) / validated, converged | 1 | 2 | 2 + 7 / yes, yes | 1 + 7 / yes, yes |
  | final-solve iterations / points | 110,079 / 6,766 | 159,149 / 6,766 | 403,102 / 12,263 | 403,102 / 12,263 |
  | wall / peak RSS (Release) | 3.5 s / 308 MB | 11.0 s / 317 MB | 72.7 s / 362 MB | 67.8 s / 366 MB |

  - On the dev (Debug) build, the T002-T004 cost basis, the credited contexts run takes about 1,121 s and 346 MB peak RSS, with
    outputs byte-identical to the Release run.
  - The contexts rounds total 2,818,102 solver iterations. Every solve stays under the 10^6 iteration and 2^20 point bounds.
  - Two credited and two ablation contexts runs are each byte-identical.
  - The memory-only comparator reports 1 site resolved only with contexts, 0 lost, and 12 unresolved sites next to it.
  - The gain is the ADR 0054 `index_unknown` PC-indexed site reached through a return continuation: it is now resolved from a
    callee summary. It adds 41 discovered PCs, all observed, and 0 escapes.
  - Every work-RAM read stays Unknown under both the credited model and the ablation, so the object-field `JSR (An)` gate (2,480
    attributed missing PCs) and the 9 width-only sites stay blocked. Their causes are the T006 interrupt/handler-stack proof and the
    T010 Z80 store-freedom proof, not calling context.

### T006: interrupt mask, handler instances and code-built frames

- **Status** (`libs/cpu/m68k/analysis/frames.{hpp,cpp}` for the public MC68000 rules; `finite_adapter.{hpp,cpp}` for the domain).
  - With `M68kAnalysisConfig::domains.frames` (it implies `contexts`, `memory` and `address`) the state carries the CPU-owned status
    `(S << 3) | I` as a generic finite set (Unknown: SR not tracked).
  - The reset entry starts at S = 1, I = 7 with A7 = the long at vector 0; any other root starts Unknown.
  - MOVE/ANDI/ORI/EORI to SR and STOP replace it; a proven RTE restores it from its frame; RTR leaves it unchanged.
  - A7 is kept across an SR writer only when S = 1 is proven before and after (no USP/SSP switch).
- **Eligibility and frames.** An interrupt of level L is eligible at a boundary when L > I (level 7 and unknown levels always). A
  synchronous vector is raised where `m68k_raised_vectors` says so (privilege violation only when user mode is possible). The 6-byte
  frame is at A7 - 6 only when S = 1 is proven; otherwise the frame address is Unknown (`frame_unproven`).
- **Partitions.** A point is `(tag << 48) | (context << 24) | pc`. Tag 0 is the main flow. Every other tag is one handler instance
  (handler, parent partition), entered with S = 1, the accepted mask (or the parent's for a synchronous vector) and the frame
  address joined over the parent's taking boundaries. Partitions never share an edge.
  - A handler is not analysed when it is already on its parent's chain, the chain is at depth 3, its parent is non-resuming, or its
    frame address is Unknown. An unanalysed resuming child makes its parent's asynchronous writers every cell and the parent's
    status Unknown after the boundaries where it can be taken.
  - Handlers with no live instance are seeded in a dead-handler partition (tag 254) so that `D` keeps its roots; every cell is
    asynchronous there.
  - Installed interrupt vectors the machine model does not deliver are potential sources under the hardware premise of decision 7:
    an unanalysed resuming interrupt wherever their level is eligible, never seeded (`D` unchanged).
- **Per-partition asynchronous writers.** A partition receives the stores and interrupt frames of every analysed resuming child
  instance (interrupts; divide by zero, CHK, TRAPV) and, transitively, that child's own writers. Frame integrity: a resuming
  instance whose writes (or its resuming descendants') may reach its saved SR word clobbers the parent's status after its taking
  boundaries.
- **Returns.** RTE/RTR, and an RTS away from the activation's entry delta (`rts_computed`), are resolved only from precise frame or
  return cells written by analysed code. Every other one stays Unknown with its reason; a handler RTE is `interrupt_resumption`.
- **Rounds and bounds.** Contexts rounds first settle with no frames configuration (warm start). The frames configuration
  (instances, per-partition policies, clobbered partitions, the per-partition status bounds) then grows by join, only from settled
  rounds, and only a validated round is returned. Otherwise the T005 contexts result is returned with the reason. Bounds: instance
  chain depth 3, 253 instance tags (beyond: the domain fails closed), 4 growth rounds per instance entry A7 (then Unknown), R = 16.
- **Driver.** `--domains frames` is accepted. The aggregate adds a `frames` object: validation, rounds, instances by class,
  unanalysed causes, live-point counts (status Unknown, S proven, eligible, masked, potential-eligible, raising, frame address
  Unknown by cause), the same counts for the first frames round, main-flow asynchronous writers, and return outcomes.
- **Fixtures** (`analysis_m68k_frames_test`, plus the deterministic frames run in `analysis_report_driver_test`):
  - the reset mask excluding level 6 (no instance, precise reads), and the domain inert when off;
  - level 6 enabled: one instance at SSP - 6 with I = 6, the main flow's writers exactly the handler cell and its frame;
  - a handler raising the mask (no nesting) and lowering it (unbounded nesting, every cell asynchronous);
  - nested synchronous handlers (non-resuming parent; a resuming divide nested in itself; the single resuming divide);
  - an interrupt preempting a synchronous handler;
  - Unknown status and Unknown supervisor stack;
  - RTE/RTR from a code-built frame, unproven and modified frames, computed RTS from PEA and pushed constants;
  - frame integrity (saved SR rewritten directly or through a handler subroutine; saved PC only leaves SR intact);
  - an installed but undelivered level-4 handler that writes a cell: with I = 3 the cell is asynchronous; with I = 5 it stays
    precise; the delivered-only premise would have kept it precise.
- **Mutations** (`analysis_mutation_test` builds the frames fixture). Ten frames mutants are killed: an Unknown SR treated as masked;
  preemption at an equal mask; the frame at A7 instead of A7 - 6; the reset SSP ignored; RTE not restoring SR; partition policy
  growth dropped; dead-handler code without asynchronous writers; frame integrity ignored; a handler RTE not labelled
  `interrupt_resumption`; an undelivered installed interrupt not treated as a writer. Two T005 corrections add two contexts mutants
  (bottom exception-continuation delta; context-0-only query).
- **Sonic attract oracle (report-only, sanitized; Release build, seven runs in parallel on one host).**

  | measure | frames (credited) | frames + ablation (diagnostic) |
  | --- | --- | --- |
  | `D` (D/U) | 6,806 (2.76%) | 6,806 (2.76%) |
  | `O ∩ D` / `O - D` / `D - O` | 4,534 / 5,978 / 2,272 | same |
  | observed recall / escapes | 43.13% / 0 | same |
  | `pc_index_explicit` / `pc_index_width_only` resolved / Unknown | 5 / 0; 0 / 9 (`width_only`) | same |
  | `jsr_an` / `jmp_an` resolved / Unknown | 0 / 2 (`base_unknown` 1, `region_exit` 1); 0 / 1 (`unknown_input/base_unknown`) | same |
  | `rte` / `rtr` resolved / Unknown | 0 / 2 (`interrupt_resumption`); 0 sites | same |
  | `rts_computed` resolved / Unknown | 0 / 26 (`stack_unbalanced` 22, `unknown_input/base_unknown` 4) | same |
  | work-RAM reads: precise / `external_writer` / `async_writer` | 0 / 428 / 0 | 0 / 0 / 419 |
  | other work-RAM-or-Unknown reads Unknown | 577 (`base_unknown` 471, `region_exit` 77, `context_bound` 12, `stack_unbalanced` 11, `set_bound` 6) | 586 (`context_bound` 21) |
  | memory: `async_all` / async ranges / release stores / Unknown-target stores | true / 0 / 1,969 / 1,930 | same |
  | handler vectors delivered / potential (installed, undelivered) | 24 / 7 | same |
  | instances analysed / dead handlers / unanalysed (`entry_unknown`, `undelivered_interrupt`) | 0 / 9 / 3, 3 | same |
  | live points / status Unknown / S proven | 6,550 / 6,550 / 0 | same |
  | eligible / masked / potential-eligible / raising | 6,550 / 0 / 6,550 / 30 | same |
  | delivered-vector frame address Unknown: S unproven / A7 Unknown | 6,532 / 5 | same |
  | first frames round: status Unknown / eligible / masked / frame address Unknown | 0 / 6,550 / 0 / 6,204 (S proven, A7 Unknown) | same |
  | contexts: call-site contexts / merged / activations / summaries | 343 / 12 / 357 / 271 | same |
  | rounds (memory + warm + frames) / validated, converged | 2 + 9 + 10 / yes, yes | same |
  | final-solve iterations / points; all rounds | 403,860 / 12,765; 7,679,399 | 403,860 / 12,765; 7,679,525 |
  | wall / peak RSS (Release, contended) | 181.4 s / 338 MB | 181.5 s / 336 MB |

  - Two credited and two ablation frames runs are each byte-identical (private, aggregate and compare-tool outputs).
  - `--domains baseline`, `memory` and `contexts` stay byte-identical to their previous outputs after the T006 corrections (the
    Genesis driver runs with `exception_continuations` off, so the continuation-delta correction does not reach them).
  - `D` is unchanged from contexts: the frames domain adds no discovered PC and no resolved site.
  - Under the hardware premise the installed level-7 and spurious vectors are eligible at every boundary, the reset mask included,
    so the first frames round already finds an unanalysed resuming interrupt in the main flow and the returned round has every live
    status Unknown. Under the superseded delivered-only premise the returned round had 6,239 of 6,550 live points with an Unknown
    status (311 proven S = 1 and masked), while its first frames round had none (6,203 eligible, 347 masked); that loss between the
    first and the returned round is recorded as observed, not diagnosed here.
  - The main flow's asynchronous writers are every cell in both models and every work-RAM read stays Unknown, so the object-field
    `JSR (An)` gate and the 9 width-only sites stay blocked. Narrowing the potential sources to the interrupt levels the board can
    assert, or analysing those handlers as instances, is a separate precision step; it would not by itself remove the external
    writer (T010).
- **Advancement iteration 2: return-continuation A7 and status precision** (frames domain only; `baseline`, `memory` and `contexts`
  outputs stay byte-identical on Sonic).
  - *Relative A7 at a balanced summary.* A summary exists only for a balanced activation (every exit an RTS at stack delta {0}), so
    its continuation now takes the caller's own A7 at the call instead of the summary's A7. That A7 is the join of the exits over
    every invocation of the context: a nested callee's context is shared by every invocation of its outer callee, so one
    Unknown-A7 invocation made every invocation's continuation Unknown. The summary's registers, A0-A6 and memory stay as they
    are: they are joins over every invocation's exits (abstract-memory join is an intersection), sound for each one, so no
    A7-relative cell keeps a stale absolute address. An unbalanced or otherwise unproven callee is never rebased.
  - *Non-resuming raises.* An instruction that always raises only non-resuming synchronous vectors (ILLEGAL, line 1010/1111;
    `m68k_vector_class`) ends its path and no longer makes its activation unproven. Unresolved computed sites, undecodable points
    and a TRAP/TRAPV whose continuation is not modelled stay unknown effects. Merged callees already used the balanced-merged rule.
    The three unproven merged activations contain an unresolved computed site (2) or an RTS away from the entry delta (1), so none
    can be proven.
  - *Per-partition status bound.* `M68kFrameConfig::status_bounds` holds one bound per live partition: its entry statuses, its SR
    writers' results and its proven RTEs' restored statuses. It is Unknown when one of the partition's clobber levels is eligible
    under that join, or when a resuming synchronous child may clobber it. An opaque continuation takes its own partition's bound;
    the dead-handler partition's bound is Unknown. Validation and growth are per partition.
  - Fixtures (`analysis_m68k_frames_test`):
    - a nested callee shared by a known-A7 and an Unknown-A7 invocation keeps the known invocation's A7, while an unbalanced
      callee's continuation stays Unknown;
    - an ILLEGAL path keeps its callee balanced, while TRAP and an unresolved `JMP (A0)` stay unproven;
    - a level-6 handler with an unresolved call is not nested when the main flow runs at I = 3, and is still nested when the handler
      lowers its own mask.

    Five mutants are killed (`analysis_mutation_test`):
    - the absolute summary exit A7 is kept;
    - an unbalanced callee is rebased;
    - a non-resuming raise is still an unknown effect;
    - an unmodelled TRAP terminates the path;
    - the whole-program status bound is used.
  - **Sonic attract oracle, credited frames run** (Release; per step; sanitized aggregates):

    | measure | before | 1: relative A7 | 2: non-resuming raises | 3: partition bound |
    | --- | --- | --- | --- | --- |
    | live / status Unknown / masked / eligible | 6,550 / 6,550 / 0 / 6,550 | same | same | same |
    | frame address Unknown: S unproven / A7 Unknown | 6,532 / 5 | same | same | same |
    | first frames round, A7 Unknown: total (`context_bound` / `none` / `stack_unbalanced`) | 6,204 (539 / 5,643 / 22) | 6,204 (1,885 / 4,319 / 0) | same as 1 | same as 1 |
    | main-flow A7-dropping continuation edges at the first frames round: summary / unproven merged / opaque `none` / unresolved callee | 250 / 27 / 26 / 5 | 0 / 27 / 26 / 5 | same as 1 | same as 1 |
    | instances analysed / unanalysed (`entry_unknown`, `undelivered_interrupt`) / integrity failures / clobbered partitions | 0 / 3, 3 / 0 / 1 | same | same | same |
    | `async_all` / ranges / Unknown-target stores | true / 0 / 1,930 | same | same | same |
    | work-RAM-or-Unknown reads Unknown (`external_writer`; `base_unknown`; `context_bound`) | 1,005 (428; 471; 12) | 1,005 (428; 468; 15) | same as 1 | same as 1 |
    | `D` / recall / escapes; width-only, `jsr_an`, `jmp_an` resolved | 6,806 / 43.13% / 0; 0, 0, 0 | same | same | same |
    | frames rounds / all-round iterations / wall / peak RSS | 10 / 7,679,399 / 181 s / 338 MB | 10 / 7,682,482 / 180 s / 336 MB | 10 / 7,682,482 / 184 s / 338 MB | 5 / 5,662,615 / 134 s / 336 MB |

    - Step 3 was run twice; both runs are byte-identical (private, aggregate and compare-tool outputs).
    - Steps 1 to 3 remove the summary-continuation cycle, but the credited counts do not move: every point it covered is also
      downstream of a genuinely unproven continuation.
    - The first A7 loss on the main flow is now a sound typed Unknown. It is a continuation of one of these:
      - a merged callee with an unresolved computed site or an RTS away from its entry delta;
      - a call-site context that contains (20) or inherits (20) an unresolved computed site;
      - a context that pops its caller's return slot (RTS at a non-zero delta: 7 local, 32 inherited);
      - an unresolved callee.
    - Step 2 removes no Sonic cause: its only always-raising activation is also unbalanced and undecodable.
    - Independently, every partition stays clobbered under the hardware premise, because the installed level-7 and spurious vectors
      are eligible at every mask.
    - A private diagnostic restricted the potential sources to levels 2 and 4. It is never credited, and the level choice is not a
      hardware fact. It still left 6,239 of 6,550 live statuses Unknown (311 masked), because level 2 and level 4 are eligible at
      the main flow's mask. Step 4 (frame-integrity attribution) is therefore not reached: no instance is admitted.
