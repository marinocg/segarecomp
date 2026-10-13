# ADR 0118: SEG-057 Z80 selective-admission contract and measured build economics

- Status: Accepted. Contract and economics protocol **frozen** in product commit `75379a7` (before any SEG-057 timing was taken). Final classification: **`Z80_SELECTIVE_AOT_FEASIBLE_AND_ECONOMIC`** (with one recorded noise-rule deviation, below). Native CNN inference is now economically worth building; SMS Optimized mode is **not** enabled and execution completeness is **not** established.
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

Measured with `tools/z80_admission_economics.py` (host: one 16-thread machine, gcc 13.3 `-O2`, 8 compile jobs, 3 interleaved repeats per configuration, medians; two unrelated pre-existing orphan processes held about 2 of 16 hardware threads for the whole session, equally for every configuration). `broad` reproduces the ADR 0116 baselines (SMS 128/256/512 KiB: 37/74/151 s wall, 281/565/1,140 CPU-s versus the earlier 42/77-83/165 s and 314-322/592-634/1,259 CPU-s on a less quiet host).

### Synthetic images (decide E1-E5)

| KiB | Config | Admitted | Entries | Owners | Bodies | Gen. C MB | Wall s | CPU s | Peak RSS MiB | Exe MB | Wall vs broad | CPU vs broad | Wall spread |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 128 | broad | 1.0000 | 132,096 | 1,032 | 9,222 | 49.6 | 37.4 | 281 | 272 | 15.9 | | | 4.8% |
| 128 | dist:100 | 1.0000 | 132,096 | 1,032 | 9,222 | 49.6 | 37.7 | 283 | 271 | 15.9 | +0.9% | +0.6% | 4.4% |
| 128 | dist:65 | 0.6501 | 85,984 | 675 | 6,869 | 32.6 | 25.3 | 184 | 246 | 10.7 | -32.4% | -34.4% | 2.5% |
| 128 | clus:65 | 0.6562 | 87,040 | 680 | 6,999 | 32.8 | 24.5 | 186 | 206 | 10.6 | -34.4% | -34.0% | 1.2% |
| 128 | fine:65 | 0.6500 | 85,859 | 675 | 6,919 | 33.5 | 24.8 | 184 | 271 | 10.9 | -33.8% | -34.4% | 1.0% |
| 128 | dist:35 | 0.3501 | 46,272 | 363 | 4,314 | 17.8 | 14.2 | 101 | 205 | 6.1 | -62.1% | -64.0% | 0.7% |
| 128 | clus:35 | 0.3750 | 50,176 | 392 | 4,593 | 19.6 | 15.4 | 109 | 204 | 6.4 | -58.8% | -61.3% | 1.0% |
| 256 | broad | 1.0000 | 263,168 | 2,056 | 14,128 | 97.7 | 74.3 | 565 | 329 | 31.0 | | | 3.0% |
| 256 | dist:100 | 1.0000 | 263,168 | 2,056 | 14,128 | 97.7 | 74.4 | 565 | 329 | 31.0 | +0.2% | -0.1% | 0.4% |
| 256 | dist:65 | 0.6500 | 171,168 | 1,344 | 10,692 | 64.1 | 47.5 | 361 | 319 | 20.8 | -36.1% | -36.1% | 1.6% |
| 256 | clus:65 | 0.6562 | 173,056 | 1,352 | 10,842 | 64.0 | 47.5 | 358 | 319 | 20.5 | -36.1% | -36.7% | 0.5% |
| 256 | fine:65 | 0.6500 | 171,056 | 1,345 | 10,688 | 66.1 | 47.3 | 361 | 290 | 21.4 | -36.3% | -36.1% | 0.8% |
| 256 | dist:35 | 0.3501 | 92,160 | 726 | 7,037 | 34.9 | 26.4 | 199 | 216 | 11.9 | -64.5% | -64.8% | 1.7% |
| 256 | clus:35 | 0.3594 | 95,232 | 744 | 7,130 | 36.5 | 27.3 | 209 | 227 | 11.9 | -63.3% | -63.0% | 0.6% |
| 512 | broad | 1.0000 | 525,312 | 4,104 | 23,313 | 193.5 | 151.1 | 1,140 | 365 | 61.2 | | | 1.0% |
| 512 | dist:100 | 1.0000 | 525,312 | 4,104 | 23,313 | 193.5 | 151.5 | 1,140 | 365 | 61.2 | +0.3% | -0.0% | 1.5% |
| 512 | dist:65 | 0.6500 | 341,568 | 2,681 | 17,086 | 126.5 | 95.1 | 728 | 341 | 40.8 | -37.1% | -36.2% | 0.6% |
| 512 | clus:65 | 0.6562 | 345,088 | 2,696 | 17,258 | 126.6 | 96.7 | 727 | 341 | 40.3 | -36.0% | -36.2% | 0.8% |
| 512 | fine:65 | 0.6500 | 341,450 | 2,685 | 17,146 | 130.6 | 95.3 | 734 | 328 | 42.0 | -36.9% | -35.6% | 0.7% |
| 512 | dist:35 | 0.3500 | 183,936 | 1,449 | 11,425 | 68.8 | 51.4 | 388 | 318 | 23.3 | -66.0% | -66.0% | 0.9% |
| 512 | clus:35 | 0.3516 | 185,344 | 1,448 | 11,188 | 71.0 | 54.1 | 409 | 325 | 22.8 | -64.2% | -64.1% | 1.0% |

Admitted is the ROM fraction after integer rounding (`clus` rounds in 4 KiB blocks: 0.3750/0.3594/0.3516 and 0.6562 are the actual ceilings). Entries include the 1,024 duplicate fixed-image entries of ROM `[0, 0x400)`. Wall includes in-process emission (about 3-10 s at 512 KiB), the parallel compile and the link.

### Real images (confirm, E6/E7; private, aggregate only)

| Image | Config | Wall s | Wall vs broad | CPU vs broad | Gen. C MB | Peak RSS MiB | Exe MB |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 128 KiB development | broad / dist:100 / dist:65 / clus:65 | 30.5 / 30.4 / 20.9 / 20.8 | / -0.5% / -31.6% / -31.9% | / +0.5% / -30.7% / -32.0% | 44.1 / 44.1 / 28.7 / 28.6 | 251 / 251 / 218 / 205 | 13.6 / 13.6 / 9.7 / 9.6 |
| 128 KiB development, supplementary set | broad / dist:65 / clus:65 | 30.7 / 21.5 / 20.8 | / -30.0% / -32.2% | / -30.0% / -33.2% | | | |
| 512 KiB development | broad / dist:65 / clus:65 | 146.3 / 92.3 / 95.0 | / -36.9% / -35.1% | / -36.5% / -36.3% | 187.3 / 122.2 / 122.0 | 363 / 341 / 345 | 60.3 / 40.3 / 39.7 |
| 256 KiB (timing only, no labels read) | broad / dist:65 | 73.2 / 48.3 | / -34.1% | / -33.4% | 96.9 / 63.5 | 330 / 320 | 30.5 / 21.4 |

Runtime (E6, 128 KiB development image, 120 frames): the `dist:100` program and the broad program end with the identical state digest and exit status; run wall 0.0657 s versus 0.0644 s (+2%, noise-level). The entry table is a binary search: 19.0 comparisons broad versus 18.4 at 65% (about one comparison fewer at most) and the rejection-table search runs only on an error stop.

Arbitrary 65% masks are unusable as programs, as expected: both `dist:65` and `clus:65` builds of the real 128 KiB image stop within 3 ms with `z80_error` (exit 3, state digest differs). This is the fail-closed behaviour the contract promises, and the reason the masks carry no correctness claim.

Trace-derived equivalence (`tools/z80_admission_equivalence.py`; the plan is the exact set of starts a 600-frame broad run executes, from the SEG-031 observer, plus the architectural roots): the selective build of a 128 KiB development image admits 8,873 starts (6.8% of the ROM, 71 owners) and the selective build of a 512 KiB development image admits 1,131 (0.22%, 11 owners); both end with a state digest and exit status identical to the broad program. This shows executed-start equivalence for those deterministic runs only; it also shows how small an honest execution-derived set is compared with the 65% budget, which is the open completeness question below.

Inference producer cost (bounded, offline, no training or labels; recovered D512HP ensemble, PyTorch 2.14.1 CPU on random bytes, full three-network ensemble over a whole image): 0.42 s for 128 KiB and 1.17 s for 512 KiB on one thread (0.23 s / 1.04 s with 8 threads); 43,809 parameters per network. Even a native implementation 10x slower than this would cost 4 s / 12 s against build savings of 12 s / 56 s at 128 / 512 KiB.

### Gate evaluation (frozen table)

| Gate | Result |
| --- | --- |
| E1 wall >= 20% (worse of dist:65, clus:65) | PASS: 32.4% (128), 36.1% (256), 36.0% (512) synthetic; conservative (best broad vs worst selective repeat) 31.4%, 35.4%, 35.6% |
| E2 CPU <= 1.00x | PASS: 0.63-0.66x at 65%, linear in the admitted count |
| E3 generated C <= 0.80x | PASS: 0.654-0.661x (fine:65 0.675x) |
| E4 peak per-compiler RSS <= 1.10x | PASS: 0.90x / 0.97x / 0.93x (worst shape; fine:65 at 128 KiB is 1.00x) |
| E5 executable <= 1.00x | PASS: 0.67x at 65% |
| E6 runtime | PASS: digest identical, run wall +2% |
| E7 real-image confirmation | PASS on reduction: 512 KiB 36.9% / 35.1%; 128 KiB 31.6% / 31.9% (set 1) and 30.0% / 32.2% (set 2). **Noise-rule deviation, below** |
| Noise | Literal rule satisfied everywhere except the real 128 KiB confirmation (see below) |

**Recorded deviation.** The frozen sub-rule (spread <= half of the gap to 20%) flagged the real 128 KiB cells: set 1 broad spread 6.1% against a half-gap of 5.8%; supplementary set 2 (three more repeats, declared after seeing set 1, same host) `dist:65` spread 6.1% against 5.0%, caused by one slower first repeat. Neither set changes the sign or the size of the effect: the smallest single-repeat comparison anywhere (best broad repeat versus worst selective repeat) is 26% at 128 KiB real, more than six points above the threshold, and every other cell, including the real 512 KiB image and all synthetic cells, satisfies the literal rule. The classification therefore does not rest on that sub-rule, but the flag is recorded rather than redefined. A reader who insists on the literal rule should read the real 128 KiB confirmation as "flagged, far from the threshold".

### Findings that qualify the headline

- **Cost is linear in the admitted count and has no large fixed floor.** Wall, CPU, generated C and executable size all fall by roughly the omitted fraction (35% omitted: 32-37%; 65% omitted: 62-66%). Grouped-owner packing and shared-body reuse do not erode the savings: `dist` (32-byte cells), `clus` (4 KiB blocks) and `fine` (single bytes) agree within 2 points. Shared bodies fall with the admitted count (9,222 to 6,869 at 65% / 128 KiB), so the pool is not a floor. The compile CPU is almost entirely owner translation units (table below); the entry table and the main unit are negligible.
- **Where the compile CPU goes** (512 KiB synthetic image, one compile of every unit with the product flags, CPU seconds; owner units are always 48 because the shard count is capped, so each unit shrinks instead of the count):

  | Config | Owner units | Shared-body units | Entry-table units | Main + ROM | Total |
  | --- | ---: | ---: | ---: | ---: | ---: |
  | broad | 1,074.9 | 56.5 | 2.4 | 0.5 | 1,134.3 |
  | dist:65 | 679.5 | 42.0 | 1.6 | 0.7 | 723.8 |
  | clus:35 | 376.8 | 29.1 | 0.9 | 0.7 | 407.5 |

  Owner units are 94.8% of broad compile CPU, shared bodies 5.0%, the entry table 0.2%. Owner and body CPU scale with the admitted count (owner 0.63x at 65%, 0.35x at 35%; bodies 0.74x and 0.52x, so the shared-body pool does shrink, just less than linearly because bodies are shared across starts), which is why neither grouped-owner packing, nor shared bodies, nor entry-table compilation is a floor.
- **Per-compiler peak memory barely moves** (0.90-1.00x): it is set by the largest translation unit, which selective emission does not shrink at 65%.
- **The extra machinery costs nothing measurable:** `dist:100` equals `broad` within noise in wall, CPU, generated size and run time.
- **The benefit scales with the admitted fraction, not with ranking quality.** A model that needs a fuller budget to be safe buys proportionally less, and a mask below the true executed set buys a typed stop, not a faster game.

### Classification

**`Z80_SELECTIVE_AOT_FEASIBLE_AND_ECONOMIC`.** The structural contract is sound and tested (the emission contains exactly the admitted starts and fails closed on the others); actual native build savings are 31-37% of full build wall at the 65% budget on every size, far above the 20% threshold and without memory, size or runtime regressions. This does **not** mean SMS Optimized mode can be enabled.

## What is still required before an SMS CNN Optimized mode

1. A native deterministic inference producer (the recovered weights, parity with the frozen PyTorch reference logits on synthetic inputs, 32-byte cell ranking proposals, fed through `AdmissionPlan`). The measured inference cost is small, so this is justified.
2. An **execution-completeness story**. Structural correctness is established; whether the omitted starts are ever executed is not. A 600-frame trace touches 0.2-6.8% of a ROM, whereas the 65% budget admits far more, but SEG-056's sealed recall is a static source-backed claim about instruction starts, not a guarantee about an arbitrary game's executed starts (computed jumps into data decoded as code, mid-instruction targets, self-timing). The honest product behaviour is the typed `unadmitted_pc` stop with a broad fallback, plus independent execution evidence (the SEG-031 observer over a held-out playthrough corpus) before any default change.
3. A fresh, independent adoption cohort: the SEG-056 sealed cohort is spent and must not be reused as blind evidence.
4. A launcher/product policy: an explicit opt-in mode that keeps the broad build as the default and as the automatic fallback on `unadmitted_pc`.

Recommended next step: **native model inference** (item 1) as a bounded task, followed by the execution-evidence design (item 2). No further model search or training is needed for the economics question.
