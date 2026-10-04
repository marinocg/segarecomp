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
4. **CPU-owned region vocabulary.** `image(id)`, `mutable_ram`, `io_device`, `unknown`. The stack is not a separate region: it is
   `mutable_ram` at the tracked absolute A7 offset. Regions, offsets and points-to live in `libs/cpu/m68k/analysis`; the generic core is
   unchanged. The CPU library names no machine (`m68k_reuse_boundary_test`): `mutable_ram` is the machine-configured RAM region whose
   extent the machine view supplies. The Genesis platform layer maps it onto its work RAM (`$E00000-$FFFFFF`, 64 KiB mirrored) and keeps
   the Genesis naming (for example the report's `work_ram_*` fields); in this record "work RAM" means that Genesis instance of
   `mutable_ram` (T009 correction cycle 1).
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
   - **Interrupt and handler-stack precision (SEG-030-T006, implemented as the staged `frames` domain; it does not hold for Sonic:
     `entry_unknown`, and no validated handler-stack window is built, record T006 advancement stop).** Without `frames` the
     consequence above applies unchanged. With `frames` (CPU-owned in `libs/cpu/m68k/analysis`, record T006):
     - **Implemented.** SR status tracking (S and I2-I0) through MOVE/ANDI/ORI/EORI to SR, STOP, proven RTE and the 68000 reset state
       (S = 1, I = 7, SSP = the long at vector 0); per-boundary interrupt eligibility (level > mask, level 7 and unknown levels always);
       the 6-byte group 1/2 frame at A7 - 6 only when S = 1 is proven; one analysed partition per handler instance (handler, parent
       partition), entered with the accepted mask and the frame address, so that handler code is never joined with the code it
       preempts; nesting and preemption per the mask at every boundary of every analysed partition, non-resuming instances included (a
       handler on its own chain, deeper than the depth bound, or with an Unknown frame address is not analysed, and is then also
       entered with an Unknown entry in the unknown-entry partition, whose own boundaries are taking points too: SEG-030-T008
       correction); per-partition asynchronous writers (the stores, interrupt frames
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
     - **Two premises, two consumers.** *Discovery* (`D`, roots, recall, the site and read reports) keeps the challenger's machine
       root premise: only the delivered vectors of ADR 0021 / ADR 0043 (level-6 autovector and the synchronous vectors) are roots
       and credited handler instances. *Asynchronous writers, interrupt eligibility and status* use the hardware premise: real
       hardware delivers the level-4 (VDP H-interrupt) and level-2 (external, I/O port) autovectors once the program enables their
       source through device registers, which the analysis does not model, so non-delivery is never proven. The installed interrupt
       vectors the machine model does not deliver (`GenesisReachabilityRoots::potential_interrupts`: the spurious vector 24 and the
       autovectors 25-31 other than IRQ6) are filtered by a named machine premise into `M68kFrameConfig::potential_interrupts`, and
       each selected vector enters a handler instance wherever its level is eligible exactly like the delivered level 6 (admission,
       nesting, mask, frame address, per-partition writers, frame integrity). Such an instance is *writer-only*
       (`M68kHandlerInstance::credited` false, also for every instance below it): its handler is never seeded as a root and its
       points never enter `D`, the site reports or the read counts (they are reported apart as writer-only instances and points);
       its stores still count as stores, release stores included. An admitted instance whose entry is Unknown, or that nests on its
       own chain, is an unanalysed resuming interrupt as before (every cell of its parent asynchronous, the parent's status Unknown
       after those boundaries).
     - **The Genesis interrupt-source premise** (`GenesisInterruptPremise::genesis_board`, platform-owned in
       `platforms/genesis/analysis_report/include/segarecomp/genesis_analysis_report/interrupt_premise.hpp`; the CPU library stays
       machine-agnostic and takes the source set as configuration). On the Genesis main 68000 the interrupt request levels are a
       subset of {2, 4, 6}, every acknowledge autovectored (vectors 26, 28, 30): level 6 is the V-interrupt (VDP register 1 IE0),
       level 4 the H-interrupt (VDP register 0 IE1), level 2 the external interrupt (VDP register 11 IE2 together with the I/O-port
       TH interrupt). Levels 1, 3, 5 and 7 are never asserted. There is no spurious interrupt (vector 24 needs /BERR during the
       acknowledge cycle; every acknowledge is autovectored through /VPA) and no uninitialized interrupt (vector 15 needs a vectored
       acknowledge); the cartridge slot carries no IPL, /VPA or /BERR line. This holds for a plain cartridge and with a Mega-CD or
       32X attached (their interrupts go to the sub-68000 and the SH-2s). Public sources: Genesis Plus GX `core/vdp_ctrl.c`
       (`vdp_68k_irq_ack` sets only levels 6, 4 and 0, `M68K_INT_ACK_AUTOVECTOR`) and `core/input_hw/lightgun.c` (level 2 gated by
       register 11 bit 3); BlastEm `genesis.c` (schedules only levels 6, 4 and 2); MAME `sega/megadriv.cpp` (lines 6 and 4);
       plutiedev "VDP registers" (IE0/IE1/IE2) and the cartridge-slot pinout (no interrupt pins); the project's
       `docs/references/genesis-controller-io-read-semantics-contract.md` (TH interrupt). ADR 0043 already marks vectors 15 and 24
       "not produced by Genesis". Caveat: the VDP's IPL0-inactive wiring is not schematic-verified, but every cited emulator agrees.
       The potential sources are therefore the installed handlers of levels 2 and 4 (level 6 is delivered). The *unconfigured*
       premise (`GenesisInterruptPremise::unconfigured`, the conservative MC68000 default) keeps every installed interrupt vector;
       under it a level-7 or spurious handler is never masked, nests on itself without bound, and makes every cell asynchronous.
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
   - **Z80 store-freedom proof (SEG-030-T010; implemented as a Genesis-owned report-only proof, record T010; it does not hold for
     Sonic: `image_set_unknown`, `m68k_store_into_z80_ram_unbounded`).** This is a SEG-030 precision blocker, not a future candidate. T010 attempts a conservative static proof that no reachable Z80 store can target 68K work RAM while the analysed
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

   **Return-slot integrity premise (SEG-030-T008).** An RTS at its activation's entry stack delta (any RTS when the delta is not
   tracked) pops the return cell `(A7).L`. With the memory domain it is classified from that cell and from the *recorded return
   slot* (the long cell a JSR/BSR pushed its return address into; every later store is related to it):
   - **normal:** the cell holds only return addresses a call pushed into it, or the slot is recorded and no store in `D` may have
     written it under the existing alias rules;
   - **computed** (`rts_computed`, resolved): the cell holds a precise set with a value no call pushed into it. The set is sound
     because the cell is precise; its targets are seeded into `D` as a resolved site, so escape checks cover it;
   - **Unknown(`return_slot_rewritten`):** a store with a known target (strong or weak) may have written the slot and the cell is
     not precise, or the cell is only partially known (strided, width-only). No call pushed the slot and the cell is Unknown:
     Unknown(`initial_memory`) (a root's RTS) or Unknown(`set_bound`);
   - **premise:** what remains is assumed away by the named premise: *a return slot is not rewritten by stores whose target the
     analysis cannot relate to it, nor by asynchronous writers*. The causes are reported per site (`return_slot_premise_sites`, by
     cause): `unknown_target_store` (an Unknown-target store or an undescribed writer, including the opaque entry of an unknown
     effect), `opaque_callee` (the slot's memory went through an opaque call continuation: a merged, recursive or unproven
     callee), `async_writer`, `external_writer`, and `slot_untracked` (A7 is Unknown at the RTS, so no store can be related to the
     slot).

   This is the SEG-026/SEG-029 call-continuation premise that the T002 baseline inherits. It is falsifiable by runtime escapes and
   by the T008 randomized differential (a concrete run that rewrites a premise site's slot is counted as `premise_violation`). It
   is never applied when the analysis has precise or weak knowledge of a contradicting write: that is `computed` or
   `return_slot_rewritten`. A `computed` or Unknown RTS is never an exit of a proven activation (T005 summaries, T006 merged-callee
   exits), so its callers' continuations are opaque (`return_slot_rewritten`). Without the frames domain an RTS away from the entry
   delta is a typed Unknown return site (`stack_unbalanced`). `--domains baseline` keeps the inherited behaviour exactly (no
   memory, no classification, no new report field): the T002 baseline output stays byte-identical.
9. **Driver rounds.** A monotone configuration (pinned sites, asynchronous cells, callee summaries, merged callees) grows as
   `config_{r+1} = config_r ⊔ computed(r)`, at most R = 16 rounds. A mandatory final validation checks that the values derived from the
   final solution are below the configuration used to compute it. Non-convergence switches the dependent domain off and reports
   `iteration_bound`. The baseline configuration has nothing to grow and converges in one round (the solver-owned pin-and-restart runs
   inside it).
10. **Report families.** `pc_index_explicit`, `pc_index_width_only`, `jsr_an`, `jmp_an`, `jsr_d16_an`, `jmp_d16_an`, `jsr_an_index`,
    `jmp_an_index`, `rte`, `rtr`, `rts_computed` (plus `unclassified`). Each family reports sites, resolved, and Unknown by generic
    reason x CPU sub-reason. Sub-reasons: `none`, `base_unknown`, `region_exit`, `set_bound`, `target_outside_image`, `width_only`,
    `store_poison`, `async_writer`, `initial_memory`, `external_writer`, `context_bound`, `stack_unbalanced`, `frame_unproven`,
    `interrupt_resumption`, `invalidated`, `return_slot_rewritten` (SEG-030-T008). Ordinary RTS is modelled through call continuations and is
    not a computed site; an RTS whose return slot is rewritten is (decision 8).
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
  store freedom). T006 is implemented as the `frames` domain (record T006); T010 is implemented as the Genesis Z80 work-RAM
  store-freedom proof (record T010), which keeps the blanket external writer on Sonic with typed reasons. Neither proof holds on
  Sonic (T006: `entry_unknown`; T010: `image_set_unknown`, `m68k_store_into_z80_ram_unbounded`). A validated handler-stack window
  was evaluated by an unsound step-0 upper bound and not built (record T006, advancement stop): the remaining blocker is intrinsic
  (`base_unknown` Unknown-target stores through pointers loaded from work RAM), not environmental.
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
  - A handler is not analysed when it is already on its parent's chain, the chain is at depth 3, or its frame address is Unknown.
    An unanalysed resuming child makes its parent's asynchronous writers every cell and the parent's status Unknown after the
    boundaries where it can be taken. (Superseded by the T008 correction below: a non-resuming parent no longer prevents analysis,
    and every unanalysed credited taking also enters the handler with an Unknown entry.)
  - Handlers with no live instance are seeded in a dead-handler partition (tag 254) so that `D` keeps its roots; every cell is
    asynchronous there.
  - Installed interrupt vectors the machine model does not deliver are potential sources under the hardware premise of decision 7:
    an unanalysed resuming interrupt wherever their level is eligible, never seeded (`D` unchanged). Superseded by advancement
    iteration 3: the Genesis premise selects levels 2 and 4 only, and each is analysed as a writer-only instance.
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
- **Advancement iteration 3: the Genesis interrupt-source premise and writer-only instances** (frames domain only; `baseline`,
  `memory` and `contexts` outputs stay byte-identical on Sonic).
  - *Premise.* The level choice of the iteration-2 diagnostic is now a resolved hardware fact, recorded with its public sources in
    decision 7 and applied by the platform (`GenesisInterruptPremise::genesis_board`): the potential sources are the installed
    level-2 and level-4 handlers; the level-7 and spurious vectors are no longer sources. The CPU library keeps taking the source
    set as configuration; `unconfigured` keeps the conservative MC68000 default (every installed interrupt vector).
  - *Writer-only instances.* A potential source enters a handler instance wherever its level is eligible, exactly like level 6:
    admission, mask, nesting, frame address, per-partition writers and frame integrity. Its instance (and every instance below it)
    is writer-only: never seeded, never in `D`, the site reports or the read counts. Its points are reported apart
    (`writer_only` instances and points), and an unanalysed one is counted as `<cause>/writer_only`. An instance whose entry is
    Unknown still clobbers its parent as before. The `undelivered_interrupt` cause is gone.
  - Fixtures:
    - `analysis_m68k_frames_test`: a level-4 handler at I = 3 is a writer-only instance at S = 1, I = 4. The main flow's writers
      are bounded: its cell is asynchronous and the other cell stays precise. Its handler and RTE are absent from `D` and the
      return sites. At I = 4 and I = 5 there is no instance, and with an Unknown supervisor stack it is
      `entry_unknown/writer_only` and every cell is asynchronous.
    - `analysis_genesis_interrupt_premise_test`: under the Genesis premise the installed level-2 and level-4 handlers are writers
      and the level-7 and spurious handlers are not. Under the unconfigured default they are, and their unbounded self-nesting makes
      every cell asynchronous.

    Five mutants are killed (`analysis_mutation_test`):
    - potential sources are ignored;
    - writer-only points are credited;
    - the Genesis premise drops level 4;
    - the Genesis premise drops level 2;
    - the Genesis premise admits level 7.
  - **Sonic attract oracle** (Release; seven runs in parallel on one host, while the mutation gate was running; sanitized aggregates):

    | measure | iteration 2 (step 3) | iteration 3 (credited) | iteration 3 + ablation (diagnostic) |
    | --- | --- | --- | --- |
    | potential sources (installed, undelivered) | 7 | 2 | 2 |
    | live / status Unknown / masked / eligible | 6,550 / 6,550 / 0 / 6,550 | 6,550 / 6,239 / 311 / 6,239 | same as credited |
    | S proven / raising | 0 / 30 | 311 / 28 | same |
    | frame address Unknown: S unproven / A7 Unknown | 6,532 / 5 | 6,234 / 6 | same |
    | first frames round: eligible / masked / A7 Unknown (`context_bound` / `none`) | 6,550 / 0 / 6,204 (1,885 / 4,319) | 6,203 / 347 / 6,204 (1,885 / 4,319) | same |
    | instances analysed / writer-only / unanalysed | 0 / - / `entry_unknown` 3, `undelivered_interrupt` 3 | 0 / 0 / `entry_unknown` 3, `entry_unknown/writer_only` 2 | same |
    | writer-only points / integrity failures / clobbered partitions | - / 0 / 1 | 0 / 0 / 1 | same |
    | `async_all` / ranges / release stores / Unknown-target stores | true / 0 / 1,969 / 1,930 | true / 0 / 1,962 / 1,923 | same |
    | work-RAM reads: precise / `external_writer` / `async_writer` | 0 / 428 / 0 | 0 / 429 / 0 | 0 / 0 / 420 |
    | other work-RAM-or-Unknown reads Unknown (`base_unknown`; `context_bound`) | 577 (468; 15) | 576 (467; 15) | 585 (467; 24) |
    | `D` / recall / escapes; width-only, `jsr_an`, `jmp_an`, `rte`, `rts_computed` resolved | 6,806 / 43.13% / 0; 0, 0, 0, 0, 0 | same | same |
    | frames rounds / all-round iterations / wall / peak RSS | 5 / 5,662,615 / 134 s / 336 MB | 5 / 5,662,714 / 140 s / 338 MB | 5 / 5,662,840 / 137 s / 338 MB |

    - Both credited runs and both ablation runs are byte-identical (private, aggregate and compare-tool outputs).
    - The premise removes the non-maskable sources, so 311 live points now have a proven S = 1 and a mask that excludes every
      potential level. The remaining 6,239 Unknown statuses follow from the level-6, level-4 and level-2 instances all being
      unanalysable: each is eligible at main-flow points whose A7 is Unknown, so each is `entry_unknown`. That clobbers the main
      flow, whose status bound becomes Unknown. The first frames round already has 6,204 eligible points with S proven and A7
      Unknown (`context_bound` 1,885, unproven continuation 4,319). Frame integrity is never evaluated, because no interrupt instance
      is admitted.
    - The dominant remaining cause of status and frame Unknown is therefore the main flow's Unknown A7 at interrupt-eligible
      points: the continuation of an unproven callee (an unresolved computed site, an RTS away from its entry delta) and the
      context bound. It is no longer the interrupt-source premise. The remaining work-RAM read Unknowns stay dominated by
      `async_all` and the external writer (T010).
- **Advancement stop (step-0 gate).** Before building a validated handler-stack window, a step-0 gate measured its best case.
  - *What was measured.* An unsound diagnostic upper bound, never credited and never committed as a mode. It forced A7 into a fixed
    work-RAM window (2 KiB and 8 KiB gave identical results), with and without the Z80 ablation, and additionally with frame
    integrity forced. Result: **NO-GO**.
    - Frames never validated within R = 16.
    - Instances were admitted in round 1, then all failed frame integrity; the instance count grew from 3 to 27.
    - Clobbered partitions rose from 1 to 17; the main flow was `async_all` again.
    - Precise work-RAM reads were 0 without the ablation, and at most 25 with the window, forced integrity and the ablation.
    - No site was resolved; `D`, recall and escapes were unchanged.
    - The resolved activation graph is already cyclic (1 recursive component, with 2 `jsr_(An)` sites unresolved), so the planned
      window's acyclicity premise fails.
    - Cost was about 570 s per run.
  - *Conclusion.* The dominant blocker is now intrinsic, not environmental: Unknown-target stores whose base is not A7
    (`base_unknown`). They exceed 1,000 already in round 1, before any handler instance exists. Their bases are pointers loaded from
    work RAM, which is never precise. The cycle is self-sustaining: Unknown reads give Unknown pointers, which give Unknown-target
    stores, which give `async_all` and frame-integrity failure, which keep the reads Unknown.
  - *Decision.* The validated stack window was therefore **not built**.
  - *Environmental proofs.* T006 (interrupt/handler stack, this record) and T010 (Z80) are implemented soundly and attempted. On
    Sonic neither holds (T006: `entry_unknown`; T010: `image_set_unknown` and `m68k_store_into_z80_ram_unbounded`), so the affected
    facts stay typed Unknown, as the invariant requires.

### T010: Z80 work-RAM store-freedom proof

- **Ownership and seam.** The proof is Genesis-owned (`platforms/genesis/analysis_report/{include/.../z80_ram_write_proof.hpp,
  src/z80_ram_write_proof.cpp}`, report-only, linked only by the report driver and tests). The M68K library receives only a typed
  bound: `M68kMemoryConfig::external_writer_bound` (nullopt, the default, is the unchanged blanket rule; otherwise a release store
  makes exactly the bound's work-RAM ranges asynchronous; an empty set means no Z80 writes). The library also reports, as a generic
  observation, the merged known bus ranges and the Unknown-target count of the stores that may touch configured ranges
  (`observed_store_ranges`; the driver observes the Z80 area `$A00000-$A0FFFF`). The diagnostic `--assume-no-z80-ram-writes`
  ablation is unchanged and never consults the proof.
- **Dependency-guard amendment (T009 correction cycle 1).** The proof decodes Z80 code, so the report-only library
  `segarecomp_genesis_analysis_report` links `segarecomp::cpu_z80`. `cpu_z80_dependency_test` (ADR 0059) allows exactly that target
  by name; every other non-z80 target that links `cpu_z80`, production targets included, still fails, and negative controls in the
  test prove it. The library stays report-only and is never linked by the CLI or a production target (`analysis_core_boundary_test`).
- **Data contract.** Input: the Z80 image set (every content Z80 RAM can hold when the Z80 leaves reset; nullopt is
  `image_set_unknown`), and the 68K Z80-area stores as groups (known ranges, Unknown-target count, proven held under `/RESET` or
  not). Output: `none | ranges | all`, the physical work-RAM ranges, the image content hashes, typed reasons (non-empty iff `all`) and
  aggregate counts.
- **Proof.** A bounded, flow-sensitive constant analysis per image from the architectural Z80 reset state (contract section 4 rule
  6), not a value-set analysis: one constant or Unknown per 8-bit register, IX, IY and SP; the nine-bit bank latch as three-valued
  bits, Unknown at entry (Z80 `/RESET` does not change it and the 68K may have written it); IFF with the EI shadow and IM as small
  sets; return slots written by CALL/RST/interrupt/PUSH with bounded value sets. Loads are always Unknown. Every reachable start must
  decode from image bytes. Stores are classified per byte: Z80 RAM (`& $1FFF`) is local data unless it hits a reachable instruction
  byte (`self_modifying_store`); the bank window maps every completion of the latch, so a completion at or above `$1C0` adds the
  work-RAM byte and one inside `$140-$141` (the Z80 area itself) is `window_store_into_z80_area`; an Unknown address is
  `store_target_unknown`. Interrupts are accepted wherever IFF may be enabled: IM 0 and IM 1 enter `$0038` (acknowledge byte `$FF`,
  contract section 8), IM 2 is `interrupt_mode_unbounded`; computed jumps need a known register (`indirect_control`); returns need a
  known SP whose slot holds a bounded set (`return_unbounded`, `stack_pointer_unknown`). The 68K side: a store not proven under
  `/RESET` that may hit a reachable instruction byte or a read return slot is `m68k_store_into_z80_code`; an Unknown-target store
  not under `/RESET` is `m68k_store_into_z80_ram_unbounded`; any store that may reach the bank register makes the latch Unknown at
  every Z80 point. No BUSREQ-only interval is credited: a BUSREQ pause resumes the same program, so a code-byte store under it is a
  live patch (the ADR 0073 live-operand hazard).
- **Credit (decision 9 style post-fixed point).** The driver starts from the Z80-only bound (the blanket rule whenever that fails),
  runs the analysis, and re-proves over that run's own Z80-area stores. The bound is credited only when the re-proof's ranges are
  covered by the bound the run used; otherwise the bound grows (join; `all` is the blanket rule) and the analysis reruns (at most
  four runs; non-stabilization is `proof_not_stable` and the blanket rule).
- **Fixtures** (`analysis_genesis_z80_proof_test`, project-authored Z80 and MC68000 encodings):
  - constant stores below `$8000`: `none`; the same store flipped to `$9000` (unknown bank): `all`;
  - a nine-write bank select of `$1FF`/`$1C0` then a window store: `ranges` (exactly that byte); a ROM bank, or a RAM bank changed to
    a ROM bank before the store: `none`; a partially known latch: both work-RAM completions;
  - an unknown bank value, a `(HL)` store with an Unknown HL, a PUSH with an Unknown SP: `all`; a PUSH at the reset SP under a RAM
    bank: `ranges`;
  - EI + IM 1 (and IM 0) with a `$0038` handler window store: `all` under an unknown bank, `none` under a ROM bank, `ranges` under a
    RAM bank (RETI returns through the bounded slot); IM 2: `all`;
  - JP (HL) with an Unknown HL: `all`; with a constant HL: bounded;
  - a 68K store into an operand byte (or its mirror, or a read return slot) while the Z80 may run: `all`; under `/RESET` or into a
    data byte: `none`; a 68K Unknown-target store while released: `all`; a 68K bank-register store: the latch becomes Unknown;
  - CALL/RET from two sites: `none`; RET without a slot, a Z80 store into its own operand, code beyond the image, an unknown, empty
    or oversized image set: `all`;
  - driver (frames domain): unknown image set keeps `external_writer` (the read is `external_writer`); a proven-free image is credited
    (no external writer, the read is precise); an image writing the read byte makes it `async_writer`, another byte keeps it precise;
    a 68K store into the image's code invalidates the optimistic bound (second run, blanket rule); the ablation never credits.

  Eleven mutants are killed (`analysis_mutation_test`): the Z80 cannot store RAM; a bank change is ignored; every 68K store is
  treated as under `/RESET`; no store is; window stores are unclassified; a stack push (SP in the window) is unclassified; a 68K
  operand rewrite is allowed; the EI enable is never accepted; the M68K domain ignores the bound; a `ranges` bound credits no writer;
  the optimistic bound is credited without re-validation.
- **Sonic attract oracle** (Release, report-only, sanitized aggregates; frames runs on one host in parallel with the other domains):

  | measure | memory | contexts | frames (credited, 2 runs) | frames + ablation (diagnostic) |
  | --- | --- | --- | --- | --- |
  | proof outcome / reasons | `all` / `image_set_unknown`, `m68k_store_into_z80_ram_unbounded` | same | same | same (bound `ablation`) |
  | credited bound / `external_writer` | blanket / true | blanket / true | blanket / true | ablation / false |
  | Z80 images analysed | 0 | 0 | 0 | 0 |
  | 68K Z80-area stores: merged known ranges / Unknown-target / bank latch volatile | 3 / 1,171 / yes | 3 / 1,894 / yes | 3 / 1,923 / yes | 3 / 1,923 / yes |
  | work-RAM reads: precise / `external_writer` / `async_writer` | 0 / 412 / 0 | 0 / 428 / 0 | 0 / 429 / 0 | 0 / 0 / 420 |
  | `D` / recall / escapes | 6,765 / 42.74% / 0 | 6,806 / 43.13% / 0 | 6,806 / 43.13% / 0 | 6,806 / 43.13% / 0 |
  | frames wall / peak RSS | - | - | 141 s / 338 MB | - |

  - The proof does not hold, so `external_writer` is unchanged. Apart from the new `z80_ram_write_proof` member of the aggregate (and
    of the private output that embeds it), the `baseline`, `memory`, `contexts` and `frames` outputs are byte-identical to the T006
    head; the baseline output, which has no memory object, is fully identical. Both credited frames runs are byte-identical.
  - First reason: no static derivation of the Z80 image set exists. The driver is uploaded by 68K code from a compressed cartridge
    stream (ADR 0073 materializes it from a build-time snapshot, which is observation and never a proof input). Second, independent
    reason: 1,923 68K stores have an Unknown target while the Z80 may run, so 68K writes into Z80 code and operand bytes cannot be
    bounded (the ADR 0073 live-operand class). A prior read-only classification of the driver (no call, return, push or interrupt
    entry; constant SP; constant store targets below `$8000`; a constant ROM bank) fits the proof's handled subset, but that
    classification came from observation and is not a static input here.
  - Successor frontier (not implemented): a static Z80 image-set derivation from the 68K upload (immutable cartridge source plus a
    bounded decompressor model) together with BUSREQ/RESET interval tracking in the M68K domains and a bound on the Unknown-target
    68K stores. That is generic immutable cartridge-data ownership plus a new M68K domain, not a further T010 fold.

### T007: integrated fixed point and multi-title O/D/U measurement

- **Configuration.** One report-only driver run per title with every admitted domain enabled together (`--domains all`: address,
  memory, contexts and frames) in a single fixed-point expansion: memory rounds, contexts warm rounds and frame rounds, each over the
  solver's own pin-and-restart, with the T010 Z80 proof crediting its bound. The baseline is `--domains baseline` from the same head.
  Coverage oracles are the 23,200-frame no-input `--execution-coverage` runs; they only falsify (`reachability_coverage_compare.py`),
  never feed the analysis. Sanitized aggregates only.
- **Oracle validity.** The compare tool now reports an `oracle` block from the coverage summary (outcome, frame target/published,
  witness overflow, `complete`). Sonic, Sonic 2, Cool Spot, OutRun and Streets of Rage reach the frame target with no witness
  overflow and no unknown retirement (`complete`). Golden Axe stops early (guest stop, 55 frames): **static-only**, excluded from the
  falsified-title quorum. The quorum is therefore Sonic plus four further titles.
- **Results** (baseline -> all; recall = |O ∩ D| / |O|; escapes = observed first entries from a resolved computed site outside its
  proven target set, all families):

  | title | U | D | D/U (all) | O | O ∩ D | O - D | D - O | recall | escapes | resolved / sites (all) | wall / peak RSS (all) |
  | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
  | Sonic | 246,293 | 6,765 -> 6,806 | 2.76% | 10,512 | 4,493 -> 4,534 | 6,019 -> 5,978 | 2,272 -> 2,272 | 42.74% -> 43.13% | 0 | 5 / 45 | 137 s / 337 MB |
  | Sonic 2 | 496,387 | 6,660 -> 6,792 | 1.37% | 2,478 | 1,326 -> 1,436 | 1,152 -> 1,042 | 5,334 -> 5,356 | 53.51% -> 57.95% | 0 | 3 / 67 | 131 s / 618 MB |
  | Cool Spot | 498,276 | 6,907 -> 6,907 | 1.39% | 4,928 | 4,031 -> 4,031 | 897 -> 897 | 2,876 -> 2,876 | 81.80% -> 81.80% | 0 | 0 / 22 | 26 s / 603 MB |
  | OutRun | 496,950 | 9,757 -> 9,757 | 1.96% | 5,286 | 5,156 -> 5,156 | 130 -> 130 | 4,601 -> 4,601 | 97.54% -> 97.54% | 0 | 26 / 56 | 195 s / 619 MB |
  | Streets of Rage | 247,761 | 1,841 -> 1,949 | 0.79% | 8,758 | 1,535 -> 1,643 | 7,223 -> 7,115 | 306 -> 306 | 17.53% -> 18.76% | 0 | 5 / 16 | 3.2 s / 309 MB |
  | Golden Axe (static-only) | 249,843 | 17,790 -> 17,843 | 7.14% | 571 | 536 -> 536 | 35 -> 35 | 17,254 -> 17,307 | 93.87% -> 93.87% | 0 | 25 / 39 | 25 s / 687 MB |

  - Observed PCs outside `D` first entered from a resolved site: **0** on every title (no falsification). Resolved sites executed by
    the oracles: Sonic 5/5, Sonic 2 3/3, OutRun 16/26, Streets of Rage 5/5 (Golden Axe 1/25; Cool Spot has none).
  - RTE/RTR counterexamples to normal resumption: 0 on every title.
- **Per-family outcome (all).** Resolved sites are `pc_index_explicit` everywhere, plus one `jmp_an` on Sonic 2 and one `jsr_an`
  on Golden Axe. Unknown by reason:
  - `pc_index_width_only`: every site `unsupported_transfer/width_only` (Sonic 9, Sonic 2 10, Golden Axe 5);
  - `rts_computed` (Sonic 26, Sonic 2 39, Cool Spot 14, OutRun 23, Streets of Rage 7): dominated by `stack_unbalanced`, plus a few
    `base_unknown` and, on Cool Spot, `context_bound`;
  - `jsr_an`/`jmp_an` unresolved: `base_unknown`, `region_exit`, `external_writer` (Cool Spot) and `context_bound` (Sonic 2);
  - `rte`: `interrupt_resumption` on every title (Golden Axe, frames off: `none`); `rtr`: no sites.
  - Overlapping starts / exception-raising decodes (all): Sonic 70 / 31, Sonic 2 3 / 0, Cool Spot 0 / 0, OutRun 367 / 149,
    Streets of Rage 159 / 192, Golden Axe 798 / 686.
- **Memory consumer after T006/T010.** On every title `async_all` holds, **0 precise work-RAM reads**, and the Z80 proof is `all`
  (`image_set_unknown`, `m68k_store_into_z80_ram_unbounded`; blanket bound). Work-RAM-or-Unknown reads by class (total:
  `external_writer` / `base_unknown` / `region_exit` / `context_bound` / `stack_unbalanced` / `set_bound`; `initial_memory`,
  `store_poison` and `async_writer` are 0 everywhere):
  - Sonic 1,005: 429 / 467 / 77 / 15 / 11 / 6 (T004 contrast retained: 412 `external_writer`; ablation 411 `async_writer` + 1
    `store_poison`);
  - Sonic 2 1,020: 550 / 378 / 74 / 2 / 12 / 4;
  - Cool Spot 1,021: 545 / 207 / 87 / 146 / 36 / 0;
  - OutRun 1,280: 704 / 327 / 116 / 131 / 2 / 0;
  - Streets of Rage 265: 58 / 68 / 27 / 12 / 100 / 0;
  - Golden Axe 1,707: 449 / 907 / 87 / 264 / 0 / 0.
  - Frames: validated on the five complete titles, with 0 analysed handler instances (every candidate `entry_unknown`, some also
    `writer_only`) and one clobbered partition; the main flow stays `async_all`.
- **Rounds and bounds.** Solver complete on every title, 0 solver and 0 driver restarts. Memory rounds / contexts rounds / frames
  warm + frame rounds: Sonic 7 / 5 / 9 + 5; Sonic 2 5 / 3 / 14 + 3; Cool Spot 4 / 2 / 9 + 2; OutRun 7 / 5 / 13 + 5; Streets of Rage
  6 / 4 / 16 + 4 (the warm start reached R = 16 without settling; the validated rounds that follow converged). Contexts validated and
  converged on the five complete titles. One bound is exhausted, and it is typed: on Golden Axe a frames warm solve exhausts the
  iteration bound, so `frames` is switched off with `iteration_bound` and contexts return their latest validated round (`converged`
  false). No title returns a partial result.
- **Determinism.** Two `all` runs per title: private output, aggregate and compare-tool output byte-identical on all six titles; the
  metrics files differ only in wall time and peak RSS.
- **Production unchanged.** Since `main` (`397bc5e`), production-target sources changed only in the report-only analysis library,
  the report driver and the reachability challenger (a shared roots/push-window owner; not on the emitter path), and
  `analysis_build_graph_test` keeps the analysis out of the production targets. Generating Sonic's startup-bridge C with
  `emit-general-startup-bridge-c --immutable-rom-aot` (generation only, nothing executed) from `main` and from this head gives
  byte-identical output (all 62 generated files, units and manifest; emitter stdout/stderr also identical).
- **Conclusion.** The integrated fixed point is sound on every oracle (0 escapes, 0 falsifications) and bounded. Its gains come from
  contexts/frames continuation precision: Sonic 2 +4.44 pp, Streets of Rage +1.23 pp, Sonic +0.39 pp; Cool Spot and OutRun are
  unchanged. Memory facts do not survive on any title (`async_all`, 0 precise reads). The blockers remain the intrinsic
  Unknown-base store cycle (`base_unknown` targets through pointers loaded from work RAM, record T006 advancement stop) and the
  environmental writers (Z80 image set unknown and unbounded 68K Z80-area stores, record T010; handler entries `entry_unknown`).

### T008: adversarial soundness and mutation gate

- **Mutant mapping.** `analysis_mutation_test` now also builds `analysis_m68k_value_test` (T003) and `analysis_m68k_memory_test`
  (T004). Every mutant of the T008 record maps to a killed mutant; none is justified as equivalent.

  | record mutant | `analysis_mutation_test` mutant | killing fixture |
  | --- | --- | --- |
  | dropped weak update | `m68k_memory_weak_update_dropped` (new) | memory: a weak update joins |
  | ignored unknown-base poison | `m68k_memory_unknown_base_not_poisoned` (new) | memory: an Unknown-target store poisons every cell |
  | excluded stack writer that actually aliases | `m68k_memory_push_at_a7` (new) | memory: stack aliasing (new fixture: PEA with A7 = field + 4) |
  | ignored interrupt-handler writer | `m68k_memory_async_writer_ignored` (new); `m68k_unanalysed_handler_precise`, `m68k_undelivered_interrupt_ignored` | memory: policy; frames |
  | raised context bound | `m68k_context_bound_raised` (new) | contexts: context exhaustion (now at the literal K = 8) |
  | congruence widened to certainty | `m68k_congruence_as_exact` (new) | value: strided only |
  | RTE proven without a frame | `m68k_rte_without_frame` (new) | frames: RTE unproven SR |
  | skipped store-derived invalidation | `m68k_store_derived_invalidation_skipped` (new); `m68k_solver_pin_ignored`, `stale_target_not_pinned` | memory: invalidation |
  | out-of-region offset accepted | `m68k_offset_region_exit_accepted` (new) | value: stepping past the extent |
  | non-deterministic order | `nondeterministic_order` | core: pinned site |
  | dropped interrupt-mask poison | `m68k_unknown_sr_treated_as_masked` | frames |
  | incorrect handler nesting | `m68k_preemption_at_equal_mask` | frames |
  | wrong supervisor-stack provenance | `m68k_frame_at_a7`, `m68k_reset_ssp_ignored` | frames |
  | incorrect RTE SR restoration | `m68k_rte_status_not_restored` | frames |
  | Z80 cannot store RAM | `z80_proof_cannot_store_ram` | Z80 proof |
  | ignored Z80 bank change | `z80_proof_bank_change_ignored` | Z80 proof |
  | ignored BUSREQ/reset interval | `z80_proof_reset_interval_ignored`, `z80_proof_reset_interval_never_held` | Z80 proof |
  | stale memory fact after policy growth | `m68k_partition_policy_dropped` | frames |
  | stale summary after discovered writer | `m68k_stale_summary_accepted`, `m68k_stale_summary_kept` | contexts |

- **Randomized synthetic differential: halted on an unsound result.** Before the seeded generator was built, a hand probe of its
  "unbalanced stack" shape falsified the call-continuation model. Minimized synthetic reproducer (flat image at 0, work RAM
  `$E00000-$FFFFFF`, reset entry `$200`, reset SSP `$FFFF00`; the same result with every domain off and with `frames`):

  ```text
  $200: JSR ($300).L      $206: NOP; BRA *
  $300: MOVE.L #$400,(A7) ; overwrite the return slot at the entry stack delta
  $306: RTS               ; concretely PC <- $400
  $400: NOP; BRA *
  ```

  The solve is complete. `$400` is not in `D`. No site at `$306` is reported Unknown: an RTS at the entry delta is an ordinary
  return, modelled through the call continuation at `$206` (decision 10), with no check that the return cell still holds the
  pushed return address. Under decision 8 an escape not justified by a typed Unknown site is a falsification. The randomized
  differential, its seeds and its counts are therefore not delivered by this record; the return-slot premise needs a decision
  first (declare it, or check the return cell and report such an RTS as a computed or typed Unknown site).

- **Return-slot resolution (T008 part 2; decision 8 amended with the return-slot integrity premise).**
  - CPU-owned in `libs/cpu/m68k/analysis`. The abstract memory records *return slots*: the long cell a JSR/BSR pushed its return
    address into, recorded whether or not the policy lets the cell hold the value. Every later store is related to the recorded slots:
    a known-target store that may touch a slot marks it `rewritten`; an Unknown-target store or an undescribed writer marks it
    `unknown_store`. Slots join pointwise and are kept only when every path recorded them; at most 512 (beyond: forgotten).
  - `M68kFiniteAdapter::classify_return_slot` classifies an RTS at the entry delta (any RTS when the delta is untracked) as
    normal / premise / computed / Unknown (decision 8). A computed RTS emits computed edges to the targets its callers' continuations
    do not already reach, and is reported as a resolved `rts_computed` site with its whole target set. A computed or Unknown RTS
    makes its activation unproven (`return_slot_rewritten`), so no T005 summary and no T006 merged-callee exit is derived from it.
    A site whose computed targets were lost is pinned and reported `invalidated`, even when its final class is normal.
  - Without the frames domain an RTS away from the entry delta is now a typed Unknown return site (`stack_unbalanced`); before, it
    made its activation unproven without a site (found by the differential below on a push window under `address+memory+contexts`).
  - The result and the report carry `return_slots` (sites, normal, computed, unknown, `return_slot_premise_sites` by cause).
    `--domains baseline` is unchanged: no memory domain, no classification, no new field.
- **Fixtures** (`analysis_m68k_return_slot_test`): the reproducer with every domain resolves the RTS to `{$400}` and `$400` is in
  `D` (no summary; the continuation is opaque `return_slot_rewritten`); the same shape with the baseline domains is unchanged, and
  under `memory` or `contexts` without frames (A7 never located) it is the premise (`slot_untracked`, counted); an untouched slot
  and a slot rewritten with its own return address are normal with a summary; a weak rewrite with an Unknown value is
  Unknown(`return_slot_rewritten`) and never the premise; a weak rewrite with a precise value is a computed return to
  `{$206, $400}`; an Unknown-base store is the premise (`unknown_target_store`, counted), and a later precise rewrite of the same
  slot is computed, not the premise; a root RTS is Unknown(`initial_memory`); the output is deterministic.
- **Mutants** (`analysis_mutation_test`, which now also builds the return-slot fixture). Killed: a precise rewrite ignored
  (`m68k_return_slot_precise_rewrite_ignored`), a weak rewrite treated as normal (`m68k_return_slot_weak_rewrite_normal`), and a
  known-target store never related to the slots (`m68k_return_slot_store_not_related`).
- **Randomized differential** (`tests/analysis_m68k_differential_test.cpp`, test-only; it links only the analysis library).
  - *Generator.* A seeded SplitMix64 generator builds small synthetic images: a main flow, eight subroutines (a subroutine calls only
    later ones), eight landing pads, an optional level-6 interrupt handler (vector 30) and an optional TRAP #0 handler. Blocks cover
    object init and object loops (`DBF`), field dispatch (an object pointer, a field load and `JSR (A2)`), PC-indexed dispatch
    (`JMP (2,PC,D0.W)` over a four-entry table, the index from an immediate, an object field or a global, masked with `ANDI.W`),
    direct calls (`JSR`/`BSR.W`), stores through known and Unknown bases, balanced pushes, conditional skips, TRAP and SR mask
    changes. Subroutine epilogues cover the plain RTS, strong, weak and Unknown-base return-slot rewrites, an unbalanced pop, and
    `PEA`/`MOVE.L -(A7)` push windows. Handlers write no register (register preservation is the resumption premise).
  - *Executor.* A test-only interpreter of exactly that subset (anything else ends the run), from the 68000 reset state (S = 1,
    I = 7, SSP `$FFFF00`), with seeded register inputs (half drawn from a pool of return-slot, object and global addresses) and
    seeded work RAM, a 3,000-step budget, and a level-6 interrupt injected at boundaries its mask permits. It records whether each
    RTS pops a slot still holding its call's pushed address, and whether each RTE pops an intact interrupt frame.
  - *Checks*, against `baseline`, `address+memory+contexts` and every domain: a transfer from a resolved site lies in its target
    set; a precise D0-D7 (16 and 32 bits) or A0-A7 value before an instruction holds the concrete value; every reached PC is in `D`
    unless the run left through a typed-Unknown site (`explained`, checking stops); an RTS whose slot was rewritten concretely is
    resolved, typed Unknown, or a premise site (`premise_violation`, counted apart; the baseline inherits the premise wholesale),
    and a normal classification there is unsound. Each analysis runs twice with byte-identical serialization; so does the
    concrete transcript.
  - *Seeds and counts.* Seeds `0x5E6030008 + 0 .. 399` (400 images, none rejected), about 55 s on the dev (Debug) build:
    `baseline` 164 clean / 221 explained / 15 premise violations; `address+memory+contexts` 164 / 232 / 4; every domain 171 / 226 /
    1, with 4,699 checked steps, 8,570 precise values, 102 resolved transfers and 16 concrete slot rewrites (411 injected
    interrupts). No bound was reached and every determinism check held.
  - **Finding (frames domain, not a premise violation), now corrected.** Two seeds of the list (base + 19 and base + 175, decimal seeds ending 795 and 951) falsified
    a precise A7 at the level-6 handler entry. Minimized synthetic reproducer (every domain, reset SSP `$FFFF00`):

    ```text
    $200: MOVE #$2300,SR    $204: TRAP #0    $206: BRA *
    vector 30 (IRQ6) -> $1800: NOP; RTE
    vector 32 (TRAP) -> $1900: NOP; NOP; RTE
    ```

    Concretely TRAP enters `$1900` with A7 `$FFFEFA` and I = 3, and a level-6 interrupt taken inside the TRAP handler enters `$1800`
    with A7 `$FFFEF4`. The analysis reported A7 = `{$FFFEFA}` at `$1800`: a handler taken inside a non-resuming instance was
    `non_resuming_parent` (unanalysed), so the interrupt handler was never entered from that partition, and the handler's points
    carried only the main-flow instance's entry. The same hole existed for every unanalysed taking (an Unknown frame address,
    nesting, the depth bound, a widened entry) whenever the same handler also had an analysed instance, and for a vector raised
    inside code whose state is not modelled.
  - **Correction (SEG-030-T006 instance model; `finite_adapter.{hpp,cpp}`).** Every boundary of every modelled partition is a taking
    point:
    - A non-resuming instance is an analysed parent like any other; its eligible interrupts and raised vectors enter child instances
      at its own frame address (the `non_resuming_parent` cause is gone).
    - A taking whose state is not modelled enters the handler with an Unknown entry. Every unanalysed credited contribution (any
      cause) and every credited instance dropped by the entry-A7 widening adds its handler to `M68kFrameConfig::unknown_entries`
      (monotone, part of the post-fixed-point validation). Those handlers are seeded in the unknown-entry partition (tag 255; roots
      Unknown, every cell asynchronous, credited to `D`), so the per-PC join at the handler entry is Unknown, never only the
      analysed instances' precise entries. The unanalysed contribution keeps its consequences on its parent (writers every cell,
      status clobbered after its taking boundaries).
    - The unknown-entry partition is itself a taker (cause `unmodelled_parent`): a delivered interrupt eligible there, or a TRAP,
      divide-by-zero or CHK raised there, enters its handler in the same partition, and a resuming one clobbers its status. The
      dead-handler partition (tag 254) now holds only handlers that no modelled boundary takes; its boundaries never run, so it is
      not a taker.
    - Writer-only partitions were already parents; a delivered vector taken there remains a writer-only child (the discovery
      premise of decision 7).
    - Report: `frames.instances.unknown_entry_handlers` and the `unmodelled_parent` cause.
    - Fixture (`analysis_m68k_frames_test`, `interrupt_preempts_non_resuming`): the reproducer has three instances and the level-6
      entry A7 is exactly `{SSP - 12, SSP - 6}`; an Unknown A7 at a taking point inside the TRAP instance makes the entry Unknown
      (`entry_unknown`, one unknown-entry handler); a TRAP raised inside the unknown-entry partition makes the TRAP entry Unknown
      although its main-flow instance is precise. The nested-TRAP fixture now has two synchronous instances.
    - Mutants (killed): `m68k_non_resuming_instance_not_preemptible`, `m68k_unanalysed_taking_entry_dropped`,
      `m68k_unknown_entry_partition_not_taker`. `m68k_unanalysed_handler_precise` now covers both Unknown-entry partitions.
  - **A second defect found by a 2,000-seed run (outside the fixed list).** Seed base + 1,920 crashed the contexts derivation
    (`std::out_of_range`): it read the computed edges of a site the solver had pinned (pin-and-restart suppresses them, so their
    targets had no state). `point_facts` now ignores a pinned site's computed edges (the site is already an unknown effect). It is
    pinned as its own CTest entry, `analysis_m68k_differential_pinned_call_test`.
  - **Registered** (`analysis_m68k_differential_test`, labels `full`, about 56 s on the Debug build). Seeds `0x5E6030008 + 0 .. 399`
    (400 images, none rejected), 0 unsound, 0 non-deterministic in every configuration: `baseline` 164 clean / 221 explained / 15
    premise violations; `address+memory+contexts` 164 / 232 / 4; every domain 171 / 228 / 1 (4,709 checked steps, 8,571 precise
    values, 102 resolved transfers, 16 concrete slot rewrites, 411 injected interrupts). A 2,000-seed run (base + 0 .. 1,999) after
    both fixes: 0 unsound in every configuration (every domain 852 / 1,142 / 6 premise violations).
- **Sonic attract oracle, every domain (Release driver at this head, sanitized).** `D` 6,806, `O ∩ D` 4,534, recall 43.13%, 0 escapes:
  unchanged from T007. 240 RTS sites are classified: 0 normal, 0 computed, 0 Unknown, 240 premise sites (`slot_untracked` 237: A7 is
  Unknown at the RTS, lost through opaque continuations and Unknown handler entries, record T006; `external_writer` 3). The
  `rts_computed` family is unchanged (26 Unknown: 22 `stack_unbalanced`, 4 `base_unknown`); contexts, summaries and continuations are
  unchanged. 136 s / 338 MB. `--domains baseline` private, aggregate and compare-tool outputs are byte-identical to T007; two
  `--domains all` runs are byte-identical.
- **Sonic delta of the frames correction (Release driver rebuilt at the corrected tree, sanitized).** `D` 6,806, recall 43.13%, 0
  escapes; the compare-tool output is byte-identical to the previous T008 run, and so are the work-RAM reads (0 precise; 429
  `external_writer`, 467 `base_unknown`) and the return-slot classification. Frames stays validated with 0 analysed instances (as
  before, every interrupt instance is `entry_unknown`), and is now more conservative: 6 handlers are entered with an Unknown entry
  (`unknown_entry_handlers`), 5 credited and 2 writer-only takings occur inside the unknown-entry partition (`unmodelled_parent`),
  and 2 partitions are clobbered (was 1). The handlers' code now runs in the unknown-entry partition (solver points 12,765 to 12,852),
  which settles 6 more callee summaries (277; 288 of 363 activations balanced) and adds 7 Unknown-target stores (1,930). Frames
  rounds 5 to 7; 159 s / 366 MB (was 136 s / 338 MB). `--domains baseline` private, aggregate and compare-tool outputs stay
  byte-identical to T007; two `--domains all` runs are byte-identical.
