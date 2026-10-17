# ADR 0141: SEG-064 authentic Sega CD BIOS — ahead-of-time feasibility gate (ADR 0130)

- Status: Accepted (SEG-064-T001). **Outcome: `NO_GO`** under ADR 0130 as written. SEG-064 is deferred; Checkpoint E is not
  claimed. A candidate architecture that the evidence supports is recorded below as a **proposal requiring an explicit, human-approved
  amendment of ADR 0130**; it is not adopted by this record.
- Related: ADR 0130 (the frozen gate and its three outcomes, unchanged), ADR 0126/0127/0128/0132 (Sega CD images, dispatch,
  scheduling, overlays), ADR 0076 (M68K image authorities), ADR 0073 (bounded build-time materialization), ADR 0097 #10
  (materialized work-RAM `JMP` thunks).
- Privacy: no BIOS or disc byte, hash, internal address, routine inventory or disassembly appears here. The BIOS was identified only
  locally (by the SHA-256 the user supplies). Figures are aggregates.

## Inputs

| item | value |
| --- | --- |
| BIOS | one operator-supplied, legally held image of the US Model 1 class (header build string of the v1.10 family; 128 KiB), read only in memory or through pipes; never copied to disk, never committed |
| synthetic disc (the ADR 0130 denominator) | a project-authored boot fixture (IP and SP written with the repository's fixture encoder) built **locally only**: its boot area must begin with the region security block the BIOS compares, which cannot be project-authored, so the local builder copied it at build time from the boot area of the operator's authorized disc. Never committed, never used by CI |
| second disc (bounded check) | the authorized Sonic CD (USA) image already used by SEG-063-T003 |
| boot input | the BIOS waits at its menu for START; the reference presses START once for 10 frames |

## Independent reference (denominator)

Genesis Plus GX at the commit already cited by the machine contract, used only as an **offline oracle** (never linked into or
shipped with segarecomp): a private headless frontend drives an instrumented copy of its two 68000 cores and records every distinct
`(processor, address, 16-bit word)` fetched through the instruction stream and every executed control edge, from power-on to the
first main-CPU instruction of the application IP (after the security block). Words are classified by provenance (main BIOS ROM,
BIOS-built main work-RAM code, sub-BIOS code present in PRG RAM at the sub reset release, sub code the BIOS builds after release,
disc IP/SP code). Two runs are byte-identical. The reference's default 512 KiB RAM cartridge was also replaced by an empty slot in
a control run: the boot path to the application IP is the same length.

**Denominator: 16,428 executed BIOS-derived code bytes** on the synthetic disc (main ROM 5,300; main work-RAM thunks 18; sub BIOS
present at release 11,058; sub thunks built after release 52). Sonic CD: 16,612, same structure.

## Criterion 1 — static discoverability (FAILED as written)

ADR 0130 asks how much of that code *the current M68K static path admits from the main and sub reset vectors and the documented
entry tables*. Decode, lift, control successors and the lowering check are the unchanged CPU-owned library calls:

| root-driven view | roots | covered |
| --- | --- | --- |
| M1: the Sega CD platform traversal (ADR 0132 #1; direct successors only) | architectural reset/exception vectors | 19.8 % |
| M1 | + the BIOS's RAM jump tables (both processors' `JMP (xxx).L` operands) as documented entry tables | 32.2 % |
| M2: CPU-owned `discover_m68k_static_graph` (ADR 0009/0022 finite-index and immutable offset-table proofs), fixed point over every edge, continuation and proven indirect candidate, every access admitted, RAM snapshots treated as immutable — a liberal upper bound | architectural vectors | 31.4 % |
| M2 | + the RAM jump tables | 59.5 % |

All are far below 95 %. The missed code is not uncompilable — every executed instruction start compiles — it is reachable only
through computed control the static path does not resolve: `RTS`/`RTE` into callers reached only through computed control,
PC-indexed jump tables with unproven index bounds, and register-indirect calls/jumps.

## Criteria 2 and 3

- *Live routines after handoff*: over 60 s of the authorized title, BIOS code executed after the handoff stays inside the same main
  ROM and sub-BIOS images; titles enter it through the RAM jump tables (sub service calls, main interrupt handlers). Under the
  root-driven views most of it is not statically admitted, so criterion 2 is not met either.
- *Native services*: the gap is hundreds of ordinary BIOS routines reachable through computed control, not up to three uncompilable
  routines. Narrow native services cannot close it (and would amount to reimplementing BIOS logic), so `GO_NARROW_NATIVE_SERVICES`
  does not apply.

**Classification: `NO_GO`.** No speculative authentic-boot implementation is delivered. Per ADR 0130 and the SEG-064 acceptance,
SEG-064 is deferred (not completed) and SEG-065's authentic-boot dependency is reassessed.

## Other measured facts (for any future decision)

| item | measured |
| --- | --- |
| RAM-executed BIOS code | the sub BIOS is produced by the main BIOS in PRG RAM through the PRG-RAM window — **not** a verbatim ROM copy (a transformed image) — complete at the single sub reset release, before any boot input; about 30 % of its executed bytes lie in the range the disc SP later overwrites (transient BIOS code). Both processors execute BIOS-built `JMP (xxx).L` jump tables whose operands are rewritten during boot and later by titles; the main tables exist at the sub release, the sub table is built by the sub BIOS **after** it |
| unsupported instruction / addressing classes reached | none: every executed BIOS instruction start lowers |
| exception/interrupt dependencies | main level 6 (V-INT) through a work-RAM thunk every frame; sub autovector levels 1 (graphics completion), 2, 4, 5; no trap or error vector |
| materialization epochs | exactly one sub reset release during boot |
| boot hardware touched before the application IP | main: VDP and PSG ports, controller/version I/O, Z80 bus request/reset and a write-only Z80 RAM upload, Gate Array (including the H-INT vector register), the PRG-RAM window, Word RAM, two reads of the empty cartridge slot (open bus); sub: Gate Array, CDC including host-data reads, CDD commands, timer/interrupts, the graphics ASIC with its completion interrupt (on the path even with the earliest effective START), write-only PCM; no backup-RAM access |
| emission cost of a broad view (below) | about 127 K instruction functions, ~64 MB of lowered bodies before wrappers (Genesis broad-AOT order, ADR 0076: 218 MB at 512 KiB); needs translation-unit splitting |

## Candidate for a separately approved amendment (NOT adopted)

Broad AOT over every aligned start of the immutable BIOS ROM (the production admission ADR 0076 records for immutable Genesis
images) plus broad AOT over the sub-BIOS image observed at the sub reset release would cover 99.6 % of the denominator (main ROM
100 %, sub BIOS 100 %). This is a different method from criterion 1 — it uses no roots, so against a self-unmodifying ROM it is close
to 100 % by construction and does not answer the question ADR 0130 asked — and it depends on mechanisms the Sega CD architecture
does not have:

1. a Sega CD M68K `bounded_build_time_materialization` producer (ADR 0076 lists the transformed M68K RAM image as a fail-closed
   frontier with no producer; ADR 0132 admits Sega CD code only by root traversal of listed modules); its epoch is the sub reset
   release, which precedes any boot input, so no input script would be needed;
2. a range partition of the sub BIOS into resident and transient images whose live-byte verification excludes mutable data (ADR
   0132's whole-range liveness fails at the first BIOS variable write);
3. an authority for the sub-side jump table, which does not exist at the only hardware-defined epoch, and for rewritten `JMP` operands
   (ADR 0097 #10 admits one byte image per thunk); a guarded-opcode / live-operand thunk dispatched through the compiled-entry lookup
   would keep the ADR 0127 key but is a new decision;
4. independently evidenced boot devices (graphics ASIC operation and completion interrupt, CDC host data, the CDD command subset,
   VDP timing and DMA from the Sega CD bus, controller input), without which criterion 3's "zero native services" would be untested.

Adopting it requires an explicit amendment of ADR 0130 approved by the operator, with its own falsifiable criteria, before any
SEG-064 implementation resumes. This record does not make that decision.

## Consequences

- SEG-064 deferred with this measured blocker; SEG-064-T001 done as the feasibility answer.
- SEG-065 cannot depend on authentic boot; its commercial-title scope is reassessed (harness backlog) against the synthetic
  post-BIOS profile and the SEG-063 module system, or the amendment above.
- The synthetic post-BIOS profile and every SEG-062/SEG-063 output are unchanged (this record changes no code).
