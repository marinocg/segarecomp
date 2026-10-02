# ADR 0079: M68K Analysis Instantiation and Staged-Domain Admission

- Status: Accepted (SEG-030-T001).
- Date: 2026-10-02
- Task: SEG-030-T001..T009 (T001 accepts this ADR; later children append their records below).
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
2. **Boundary-test amendment** (`tests/analysis_core_boundary_test.py`).
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
   | exception and return-frame state (T006) | RTE/RTR (ADR 0051 first gate) and computed RTS | RTE/RTR discover nothing under the strict model; push-window RTS is unresolved | the baseline has no stack cells | `rts_computed`/RTE/RTR resolved only from code-built frames; handler-entry frames stay Unknown (`interrupt_resumption`) |

   **Not admitted (candidates):** intervals beyond the finite-set bound (no measured site hit the 4,096 bound); general widening (the
   stride form above widens only to a region extent, never to certainty); pin minimization (ADR 0078 T003 record: cascaded
   over-invalidation, a precision loss only); selective symbolic execution (ADR 0076: later evaluation only).
6. **Context bound.** Call string of depth k = 1, encoded in the adapter's opaque 64-bit point: `point = (ctx << 24) | pc`, with
   `ctx = 0` (no context) or the call-site PC + 1. `D` is the set of the low 24 bits of reached points. At most K = 8 contexts per callee
   entry; beyond K the callee is merged into context 0 in the next driver round and reported `context_bound` (generic `state_bound`).
7. **Asynchronous writers.** The machine-delivered vector roots (`genesis_reachability_roots`, shared with the challenger) are
   potential asynchronous writers.
   - The asynchronous cell set is the union of the store cells over the code reachable from those roots in `D`. Asynchronous cells are
     never strong-updated and read `Unknown(async_writer)` outside handler code.
   - An Unknown-target store in handler code poisons all of work RAM.
   - Handler entry: registers and work RAM Unknown; A7 is the join of the absolute A7 over non-handler points minus the frame (Unknown
     if any is Unknown).
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
   - **Credited policy.** Z80 code and the bank value are runtime state, and SEG-030 does not try to prove a Z80 program free of
     bank-window stores. So if the analysed 68K code contains a reachable store to the Z80 reset/BUSREQ control registers that could release
     the Z80 (a store not provably absent in `D`):
     - every mutable work-RAM cell is potentially written externally at every program point;
     - every read of mutable work RAM is `Unknown` (`external_writer` -> `unknown_input`), and no work-RAM strong-update fact survives.
   - **Exclusion.** Only a program that provably never releases the Z80 (no such store in `D`, under the closure premise) excludes
     external writers.
   - **Premise ablation.** A labelled ablation that assumes no Z80 work-RAM writes may be reported for sensitivity only. It is never mixed
     into `D`, recall or credited counts.
   - **Future candidate.** Z80 bank-store freedom and BUSREQ-interval proofs.
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
    Unknown, `state_bound`); K = 8; R = 16.
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
- A domain whose child cannot meet its observable acceptance stops and records why; zero measured gain is an acceptable recorded result.

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
