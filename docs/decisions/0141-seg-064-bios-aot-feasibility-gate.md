# ADR 0141: SEG-064 authentic Sega CD BIOS — ahead-of-time feasibility gate (ADR 0130)

- Status: Accepted (SEG-064-T001, Checkpoint A). **Outcome: `GO_STATIC_BIOS_AOT`.**
- Related: ADR 0130 (the frozen gate and its three outcomes, unchanged here), ADR 0126/0127/0128/0132 (Sega CD images, dispatch,
  scheduling, overlays), ADR 0076 (M68K image authorities and production admission), ADR 0073 (bounded build-time
  materialization), ADR 0097 #10 (materialized work-RAM `JMP` thunks), ADR 0049 (byte-identity guards).
- Privacy: no BIOS or disc byte, hash, internal address, routine inventory or disassembly appears here. The BIOS is identified
  only locally (the SHA-256 the user supplies at build time). Figures are aggregates.

## Inputs

| item | value |
| --- | --- |
| BIOS | one operator-supplied, legally held image of the US Model 1 class (header build string of the v1.10 family; 128 KiB), read only in memory or through pipes; never copied to disk, never committed |
| synthetic disc (the ADR 0130 denominator) | a project-authored boot fixture (IP and SP written with the repository's fixture encoder) built **locally only**: its boot area must begin with the region security block that the BIOS compares, and that block cannot be project-authored, so the local builder copies it at build time from the boot area of the operator's authorized disc. The disc stays in ignored storage and is never committed or used by CI |
| second disc (bounded check) | the authorized Sonic CD (USA) image already used by SEG-063-T003 |
| boot input | the BIOS waits at its menu for START; the reference presses START once for 10 frames (several press times were measured; the BIOS-executed figures below hold for the earliest effective press) |

## Independent reference (denominator)

The executed-code denominator comes from an independent emulator used only as an **offline oracle**, never linked into or shipped with
segarecomp: Genesis Plus GX at the commit already cited by the machine contract, driven by a private headless frontend with an
instrumented copy of its two 68000 cores. It records every distinct `(processor, address, 16-bit word)` fetched through the
instruction stream (opcode and extension words) and every executed control edge, from power-on to the first main-CPU instruction of
the application IP (the instruction after the security block). Each fetched word is classified by provenance: main BIOS ROM, a
BIOS-built main work-RAM instruction, sub-BIOS code present in PRG RAM at the sub reset-release epoch, sub code built later by the
BIOS, or disc code (IP/SP, compared with the disc's own extents). Two runs are byte-identical.

## Measurements

Denominator: **16,428 executed BIOS-derived code bytes** on the synthetic disc (main ROM 5,300; main work-RAM thunks 18; sub BIOS
present at release 11,058; sub thunks built after release 52). The authorized Sonic CD disc gives 16,612 (same structure).

Numerator, three admission views of the same images (decode, lift, control successors and lowering are the unchanged CPU-owned
library calls; the compile check is the Sega CD emitter's own per-instruction rule):

| view | covered executed BIOS bytes | main ROM | sub BIOS |
| --- | --- | --- | --- |
| M1: the SEG-062 Sega CD platform traversal (direct successors only) from the architectural reset and exception vectors | 19.8 % | 35 % | 12 % |
| M2: the CPU-owned `discover_m68k_static_graph` (ADR 0009/0022 finite-index and immutable offset-table proofs) iterated to a fixed point over every recorded edge, continuation and proven indirect candidate, every memory access admitted, RAM snapshots treated as immutable (a liberal upper bound for root-driven discovery) | 31.4 % | 37 % | 29 % |
| **M3: the M68K production admission of ADR 0076** — broad AOT over every aligned start of the immutable BIOS ROM (`immutable_input`), and broad AOT over the sub-BIOS image observed at the sub reset-release epoch (`bounded_build_time_materialization`, ADR 0073 rules) | **99.6 %** | **100 %** | **100 %** |

The only executed BIOS-derived bytes M3 does not cover are RAM jump-table thunks (`JMP (xxx).L`) whose 32-bit operand is rewritten
during boot (main and sub tables). Counting every thunk as uncovered, M3 is still 99.57 %.

Every executed instruction start compiles: **no executed BIOS instruction is an unsupported instruction or addressing-mode class**.
Instruction starts that do not decode (data) are emitted as the typed `instruction_unsupported` stop; addresses without a compiled
entry are the typed `entry_not_compiled`; neither crashes.

| gate item | measured |
| --- | --- |
| executed computed control (root-driven views) | `RTS`/`RTE` into callers reachable only through computed control; PC-indexed jump tables with unproven index bounds (`JMP`/`JSR (d8,PC,Xn)`); register-indirect calls/jumps (`(An)`, `d16(An)`, `(d8,An,Xn)`). M3 needs no target discovery for any of them: every target is a compiled PC (ADR 0127 key) |
| RAM-executed code | (a) the sub BIOS: produced by the main BIOS in PRG RAM through the PRG-RAM window, **not** a verbatim ROM copy (a transformed image), complete at the single sub reset release; (b) BIOS-built `JMP (xxx).L` thunk tables in main work RAM and sub PRG RAM, whose operands are rewritten by the BIOS during boot and later by titles installing handlers. No other self-modifying or RAM-generated BIOS code executes |
| exception/interrupt dependencies | main: level 6 (V-INT) through a work-RAM thunk on every frame; sub: autovector levels 1 (graphics completion), 2 (main `IFL2`), 4 (CDD) and 5 (CDC); no other level, trap or error vector is taken |
| materialization epochs | exactly one sub reset release during boot; no second epoch |
| live BIOS routines after handoff (criterion 2) | over 60 s of the authorized title (about 45 s past handoff), every executed main-ROM and sub-BIOS byte stays inside the same two compiled images (M3 coverage 100 %); titles enter the BIOS through the RAM thunk tables (sub service calls, main interrupt handlers) |
| emission cost (M3) | about 127 K compiled instruction functions (main ~62 K, sub ~65 K; ~4.6 K non-decoding starts become typed stops), ~64 MB of lowered instruction bodies before wrappers — the same order as the Genesis broad-AOT precedent (ADR 0076: 218 MB of C at 512 KiB). Requires splitting the generated program into several translation units; measured at Checkpoint B |
| boot hardware touched before the application IP | main: VDP data/control (whether DMA is used is inventoried at Checkpoint C) and the PSG port, controller/version I/O, Z80 bus request/reset and a write-only Z80 RAM upload (main never reads Z80 RAM), Gate Array including the H-INT vector register, the PRG-RAM window, Word RAM, reads of the cartridge-slot area; sub: Gate Array, CDC (including host-data reads), CDD commands, timer/interrupt control, the graphics ASIC register set (operations and their completion interrupt are on the path even with the earliest START), write-only PCM; **no backup-RAM access** before the application IP |

## Classification

- **Criterion 1 (static discoverability)**: met. The frozen threshold is >= 95 % of executed BIOS code bytes under *the current M68K
  static path*. For an immutable M68K image that path is the production admission ADR 0076 records — broad AOT over every aligned
  decodable start, with discovery only as a refinement — and for code another processor builds in RAM, ADR 0076 names
  `bounded_build_time_materialization` as the accepted authority ("decompressed or generated code ... only when a workload requires
  it"; the sub BIOS is that workload). M3 = 99.6 %. The SEG-062 Sega CD traversal is a platform admission choice for small
  synthetic images, not the CPU's static path; its 19.8 % (and the 31.4 % root-driven upper bound) are recorded so that the reading is
  falsifiable. Computed control needs no dispatch key beyond ADR 0127 (PC among compiled entries; overlay selection by verified live
  bytes).
- **Criterion 2 (live routines)**: met, provided the RAM thunk tables dispatch only into compiled entries (decision 3 below).
- **Criterion 3 (native services)**: none needed. No routine is self-modifying beyond the thunk operands, none is a timing loop on
  unmodelled hardware that cannot be compiled; zero native services (ceiling three).
- Therefore **`GO_STATIC_BIOS_AOT`**. `GO_NARROW_NATIVE_SERVICES` and `NO_GO` are not supported by the evidence.

## Decisions for the implementation (Checkpoints B-E)

1. **Authentic BIOS profile beside the synthetic profile.** Selected only by an explicit `--bios <file>` (no search, no download, no
   embedded BIOS). The synthetic post-BIOS profile's generated output stays byte-identical when its inputs and options are unchanged.
2. **Images.** The BIOS ROM is an `immutable_input` image at main `$000000` admitted by broad AOT over every aligned start. The
   sub-BIOS image is produced by **bounded generated-native build-time materialization** (ADR 0073 rules): the compiled main program
   runs from deterministic reset with the scripted boot input, the only snapshot event is the hardware-defined sub reset release
   (`SRES` 0 -> 1 with `SBRQ` = 0), a confirming run must reproduce it, exhaustion is a typed build failure, and the image is
   admitted by broad AOT with runtime byte-identity verification (overlay registry, ADR 0132). The executable never decodes,
   materializes or compiles anything at run time.
3. **RAM jump-table thunks** (the only RAM-constructed BIOS code that executes): a work-RAM / PRG-RAM `JMP (xxx).W/.L` admitted from
   the materialization observation whose **opcode word is byte-identity guarded and whose absolute operand is read live** and
   dispatched exactly as a computed jump through the processor's compiled-entry authority (ADR 0127 key; an uncompiled target is the
   typed `entry_not_compiled`). This is the M68K counterpart of ADR 0073 decision 7 (structural bytes guarded, payload live) and a
   one-form generalization of ADR 0097 #10, whose single-image rule would stop at the first operand rewrite. Any other RAM byte change
   in a guarded instruction still fails closed.
4. **Identity.** The program records the SHA-256 of the BIOS it was compiled from; the runtime reads the separately supplied BIOS,
   recomputes it and refuses a missing or different BIOS before any guest instruction runs. Generated C and binaries of the
   authentic profile embed BIOS-derived code and stay in ignored local storage; they are never published.
5. **Boot hardware** is added only where the authentic boot path reaches it (the table above), each item with independent
   evidence and synthetic tests; unmodelled behaviour stays a typed stop. Audio-only devices whose state the CPUs never read during
   boot (PCM wave/register writes, the Z80 program upload) are modelled as write-only storage without synthesis — a recorded
   deviation, not a hidden success.
6. **Handoff proof.** Checkpoint E is an authentic BIOS executing generated-native code from reset to the application IP/SP with the
   handoff state compared against the independent reference, on the local synthetic disc; the authorized title is a bounded
   additional check, not a title-screen claim.

## Unresolved facts carried to the implementation

- Cycle-level timing differences between this machine's integer-tick scheduler and the reference (VDP, CDD, graphics ASIC) may
  change the number of frames before the handoff; the handoff comparison therefore normalizes frame-count-dependent state and
  records what is normalized.
- The graphics ASIC's rendering and timing need independent evidence (two references) before they are implemented; the completion
  interrupt is on the boot path.
- Exact CDD command subset, CDC host-data protocol and cartridge-slot probe behaviour are inventoried from the authentic run and
  verified against public references at Checkpoint C.
