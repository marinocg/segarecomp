# ADR 0118: SEG-057 Z80 selective-admission contract and measured build economics

- Status: Accepted (contract); economics protocol **frozen** in the commit that introduced this ADR, before any SEG-057 timing was taken. Final classification: see "Results and classification".
- Related: ADR 0058/0071/0073 (Z80 broad immutable-image AOT, owners, shared bodies), 0116/0117 (SEG-051/SEG-056 ML experiments, frozen history, not reopened), 0108 (economics reference `generated C <= 0.80x`, `CPU <= 1.00x`).

## Context

ADR 0116 found that the Z80 AOT had no admitted-start set, no typed unadmitted-PC behaviour and no selective emission, so even a perfect start ranker could not be consumed. ADR 0117 produced a promising offline ranker (D512HP, three seeds, 32-byte cells, top-two logits, roots 0x0000/0x0038/0x0066, frozen budget 0.65) and the weights are preserved (`tools/segarecomp_ml_region_z80_gen2.weights.sgz80w`, SHA-256 `81434de0...c2ac`), but the literal sealed gate failed and nothing measured what selective emission would save. SEG-057 answers one engineering question: **can the emitter safely consume an explicitly supplied subset of Z80 instruction starts, and does that save enough real native build time to justify an inference producer?** It does not enable the model, change the default, or claim a sealed game is playable under selective emission.

## Decision 1: the selective-admission contract

`codegen::z80::EmitOptions::admission` (`std::optional<AdmissionPlan>`) is an optional, explicitly supplied, structural input. Absent, the emission is the merged broad AOT **byte for byte** (verified against the pre-change emitter, see Evidence). Present:

- `AdmissionPlan` holds one `ImageAdmission` per code image: `identity`, `byte_count`, `content_digest` (FNV-1a 64 of the image bytes, a binding checksum rather than a security primitive) and strictly increasing `offsets`. An offset is an **image offset**: the existing owner key space (`base + offset` is the logical address of an absolute-PC owner; the offset itself is the key of a window-relative owner), so banked and multi-window images, shared bank images exposed in several windows, and RAM-backed images keep their current semantics. `bind_admission(image, offsets)` builds the identity/size/digest binding.
- The emitter rejects, leaving nothing behind: an unknown identity, an identity listed twice, an image without an entry, an entry made for different bytes (size or digest), non-increasing or duplicate offsets, an offset past the image, and an offset no window exposes.
- Exactly one owner is emitted for every admitted offset whose start has a lowering row (CPU-owned classification and legal-form lowering are untouched). Admitted offsets without a lowering row stay `unlowered_starts` and keep today's `no_owner` semantics. Typed stubs (`mutable_code`, `unresolved_fetch_mapping`, prefix locks) are emitted only for admitted offsets too.
- Grouping, shared effect bodies, direct owner-to-owner binding and the exact `(identity, key)` entry table are unchanged and are computed over the admitted plans only. A fall-through into an omitted successor is therefore **not** direct-bound: it returns to the generated dispatcher, which misses.
- **Unadmitted-PC outcome.** `Z80_ERROR_UNADMITTED_PC = 23` (`unadmitted_pc`) is a new fail-closed error. The dispatcher emits it only when the exact lookup misses and the generated rejection table (sorted `[lo, hi)` runs of exposed-but-omitted image offsets per identity) contains the offset of `state.pc`; every other miss stays `Z80_ERROR_NO_OWNER`, so `no_owner` is unchanged and backward compatible. The miss happens in `z80_run` before any owner runs: the stop reports `state.pc` = the omitted start with no instruction effect (no register, memory, I/O, T-state or R change from that instruction; a maskable/NMI acceptance at the preceding boundary, which is not an instruction effect, may already have happened, and then the reported PC is the interrupt target). An omitted PC is never redirected to a nearby admitted owner and nothing is decoded, interpreted or compiled at run time.
- Without a plan no rejection table is emitted and the `NO_OWNER` text in `z80_run` is unchanged.
- TU sharding in selective mode uses the admitted rank, so translation-unit count scales with the admitted count; broad sharding is unchanged.
- The selection policy (a CNN, a mask, a trace) is entirely outside the emitter. An ML model is not the correctness authority.

### What this establishes and what it does not

**Structural correctness** (established by tests): the emission contains exactly the supplied admitted starts and deterministically rejects every other exposed start, and the plan contract fails closed. **Execution completeness** (NOT established, not claimable by this ADR): that every start a program executes was admitted. `unadmitted_pc` is a safe typed stop, not evidence that a game is playable under selective emission; deciding completeness would need either a sound reachability analysis (the retired discovery architecture) or execution evidence such as the SEG-031 coverage observer, and SEG-057 deliberately builds neither.

## Decision 2: frozen economics protocol

The question is cost, so the protocol is fixed before measuring.

**Subjects.** SMS Sega-mapper images of 128, 256 and 512 KiB: (a) deterministic synthetic images (xorshift64* bytes, seeds 128/256/512; public and reproducible: the emitter creates an owner at every byte offset, so build cost is dominated by decoded form mix and offset count, which uniformly random bytes exercise); (b) local private real images for confirmation (128 KiB and 512 KiB development images; the 256 KiB images available locally belong to the already-spent sealed cohort and are used for build timing only: no labels are read and nothing is learned from them). No ROM byte, per-ROM map, plan or generated C is committed or reported.

**Configurations** (mask percent = ceil of units, roots first):

| Config | Meaning |
| --- | --- |
| `broad` | no plan: the merged AOT (the control) |
| `dist:100` | selective with every start admitted (control for the selective machinery itself) |
| `dist:65`, `dist:35` | 32-byte cells scattered by a seeded hash (the shape of a learned cell ranker) |
| `clus:65`, `clus:35` | 4 KiB blocks scattered by the same hash (clustered: maximal grouped-owner packing) |
| `fine:65` | single bytes scattered (worst case for grouping; stress only) |

The architectural roots' cells (ROM offsets 0x0000, 0x0038, 0x0066) are admitted first. The 65% budget is the SEG-056 prospective budget, 35% is an aggressive economics control **not an adoption recommendation**. The actual admitted fraction (integer rounding included) is reported for every cell. Masks are structural only and say nothing about executed starts.

**Measurement.** `tools/z80_admission_economics.py` mirrors `segarecomp build --platform master-system --mapper sega -O2` for the headless link set (same generated units, runtime and headless sources, same flags), host gcc 13.3 `-O2`, 8 compile jobs (the product default) on the 16-thread host with no other load, three repeats per configuration with configurations interleaved across repeats. Recorded per build: admitted fraction; emit/compile/link wall; generated C bytes; per-process user+system CPU and peak RSS (`wait4`); native executable size; entry bindings, owner functions, shared effect bodies, translation units. "Full native build wall" = in-process emit + parallel compile + link. Runtime: for the real 128 KiB image, the `dist:100` build runs the headless program for a fixed frame count against the `broad` build; the state digest must be identical and the run wall within the stated guard rail.

**Frozen gates** (evaluated on medians of three; synthetic images decide, real images confirm):

| Gate | Criterion |
| --- | --- |
| E1 wall | the **worse** of `dist:65` and `clus:65` reduces full-build wall by at least **20%** versus `broad`, at each of 128, 256 and 512 KiB |
| E2 CPU | build CPU seconds at 65% are at most 1.00x broad (ADR 0108 G1.3) |
| E3 generated C | generated C at 65% is at most 0.80x broad (ADR 0108 G1.3) |
| E4 memory | peak per-compiler RSS at 65% is at most 1.10x broad |
| E5 size | native executable size at 65% is at most 1.00x broad |
| E6 runtime | `dist:100` is state-digest identical to broad on the real-image run and its run wall is at most 1.10x broad |
| E7 confirmation | the 128 KiB and 512 KiB real images meet E1 at 65% (`dist:65`) as well |
| Noise | the wall spread (max-min)/median of a configuration is at most half of the gap between its reduction and the 20% threshold, else that gate is *inconclusive* |

**Classification** (closed taxonomy): `Z80_SELECTIVE_AOT_FEASIBLE_AND_ECONOMIC` (structure correct and E1-E7 pass), `Z80_SELECTIVE_AOT_FEASIBLE_NOT_ECONOMIC` (structure correct and E1 fails with the noise rule satisfied, or E2-E6 regress unacceptably), `Z80_SELECTIVE_ADMISSION_CONTRACT_BLOCKED` (the structural contract cannot be made sound without a large new subsystem), `Z80_SELECTIVE_ECONOMICS_INCONCLUSIVE` (structure correct but the noise rule prevents a decision). Owner-count reduction alone never counts.

## Validation

`tests/z80_selective_admission_test.py` (hermetic, no ROM/ML/ROCm): over the grouped-owner differential corpus (prefix chains and locks, direct branches, calls/returns, bank/window-relative owners, remap, INT/NMI/IM0-2, HALT, block repeats, deadline resumes, grouped owners with shared bodies, direct-bound fall-through) and seeded random programs: a plan admitting everything emits byte-identical owner/body translation units; a plan derived from the broad run is a strict subset and reproduces every step; omitted starts stop with `unadmitted_pc` at the omitted PC, and for single-instruction steps without interrupts the stop is proven effect-free; the plan contract fails closed and leaves nothing behind; the emission is deterministic.

## Results and classification

(Recorded below after the frozen measurements; see Evidence.)
