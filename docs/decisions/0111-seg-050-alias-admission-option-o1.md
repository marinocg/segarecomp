# ADR 0111: SEG-050-T002 alias / RAM-thunk selective admission: option O1 (alias keyed by its byte-identity source)

- Status: accepted (checkpoint C2). Classification **`ALIAS_ADMISSION_IMPROVED`**. Conclusion 1 of SEG-050, independent of any ML result (Problem B, ADR 0112+).
- Related, unchanged: ADR 0049 (alias descriptor, runtime byte-identity guard), ADR 0077 (image artifact), ADR 0094 (structural prune), ADR 0080 (validator), ADR 0104 finding 3, ADR 0110 (pre-registration and baselines).

## 1. Problem (verified, not assumed)

Under Optimized, the first (alias-less) emission is ML-admitted, but every later emission of the ADR 0049 / RAM-thunk preparation falls back with `prune_rejected / outside_region`. Cause (code): the model proposes only cartridge ranges, while the prune keyed EVERY identity by its execution address, so a work-RAM alias identity is never in R; and the validator makes every `execution_alias` identity a mandatory root. Decomposition of the three cases the task names:

| case | status |
| --- | --- |
| (a) selected ROM source span whose copied identity is not mapped | **the defect**; fixed by O1 |
| (b) proven source span excluded by the proposal | not a mapping defect: model recall (ADR 0099). O1 keeps it fail-closed; O2 would widen R |
| (c) RAM thunks / non-verbatim RAM code | no cartridge source; stay fail-closed under every pre-registered option (thunks also carry the closure edges of the calling copy, see 4) |

## 2. Options (pre-registered in the task; evaluated unchanged)

- O0 status quo. O1: an alias identity is in R iff its byte-identity source (`alias_source_address`) is in the selected region set. O2: the exact descriptor source span is added to R before the unchanged prune.
- Experiment: a 3-line local patch (SHA-256 `a356428ce68d5c22...`, not committed) switched the mode in `hybrid_admission.cpp`; `O3` (below) was an extra, UNREGISTERED exploratory arm.

## 3. Results (gen2 / B35; `-O0` screen builds for the admission lines; `K/U` of the alias-bearing emission)

| image / emission | O0 | O1 | O2 | O3 (exploratory, unregistered) |
| --- | --- | --- | --- | --- |
| Golden Axe, alias emission (U=250,058; 432 B copy, no thunks) | reject `outside_region` | **admit K=83,924 (33.6 %)** | admit, identical to O1 (source already in R) | admit, identical |
| Flicky, alias-only emission (U=87,953; 49,152 B mixed code/data copy) | reject `outside_region` | reject `pruned` (data decodes of the copy have edges leaving K) | admit K=53,428 (60.8 %) | admit K=45,875 (52.2 %) |
| Flicky, emissions with RAM thunks (rounds 3-16) | reject | reject | reject | admit all 16, final K=45,889 of 87,967 (52.2 %) |
| Flicky complete build | broad (fallback) | broad (fallback `prune_rejected`) | broad (thunk rounds) | not built end-to-end |

Complete shipped builds (`segarecomp build`, gcc -O1, `--jobs 8`, single samples, baseline noise floor +/-25 % CPU, ADR 0110 section 9):

| build | wall s | CPU s | generated C | effective |
| --- | --- | --- | --- | --- |
| Golden Axe Compatibility (two samples) | 104.9 / 104.8 | 714.2 / 709.8 | 163,252,739 B | broad |
| Golden Axe Optimized gen2, status quo | 84.9 | 533.9 | 163,252,739 B | broad (fallback) |
| **Golden Axe Optimized gen2 + O1** | **53.4** | **325.2** | **67,228,249 B (-58.8 %)** | `ml_region_gen2`, admitted 33.56 %, validator accepted |
| Golden Axe Optimized v1 + O1 | 38.3 | 237.2 | 50,241,735 B | `ml_region`, admitted 24.9 % |
| Flicky Optimized gen2 + O1 | 224.3 | 1,058.0 | 86,659,819 B | broad (fallback; RAM thunks) |

Behavioural checks on Golden Axe: the Compatibility, gen2+O1 and v1+O1 executables run to the same bounded instruction budget (60 M) with byte-identical stdout and identical `SOUND_SUMMARY` digests (6,677 virtual frames, `stop_class` 0); two independent gen2+O1 builds produced byte-identical generated C; the compile with `-std=c11 -Wall -Wextra -pedantic` emitted no warning.

## 4. Decision

**Adopt O1** (`prune_genesis_region_admission`): a copy-alias identity (not a RAM thunk) is in R when its RAM execution address is (an explicit RAM range keeps its meaning) OR the immutable source address it is byte-identical to is. Nothing is added to R, no source span is widened, no reachability is inferred, the structural closure and the validator are untouched (every alias identity stays a mandatory root), the runtime guards are untouched, and a build without aliases is identical (tested). The rule uses only existing authority: the descriptor and its byte-identity source.

Not adopted:
- **O2**: it only changes a complete-build outcome where the model misses an alias source, and then by compiling the whole span (Flicky's emission grows to 60.8 % of the universe because the copy is mixed code/data); in the corpus it never produced a complete-build benefit (Flicky is blocked by thunks). It widens R beyond the model's proposal.
- **Thunks / O3**: Flicky's 14 RAM thunks are `JMP` entries the copied code calls; the prune closure removes the copy because the called thunk identity is outside R. An exploratory arm that treats every mandatory (`execution_alias`) identity as a member of K0 (exactly what the validator already requires) admitted all 16 Flicky emissions. It is NOT in the pre-registered set and was not adopted; it is recorded as a narrowly scoped follow-up (needs its own pre-registration and review, because it makes thunk admission independent of the proposal).

## 5. Invariants and tests

`tests/genesis_region_prune_test.cpp` gains `alias_source_mapping` (all with the UNCHANGED production validator accepting every K): correct proven copy (source outside R rejected as `outside_region`, source inside R admitted, unrelated span rejected, explicit RAM range unchanged); unproven RAM thunk fail-closed; descriptor with a source leaving the image and overlapping descriptors rejected; partially covered copy rejected, completely selected copy admitted; alias whose absolute edge escapes K classified `pruned`, admitted once the target is selected; no alias == plain region rule; repeat determinism; a plan of the alias-less emission refused for the alias emission (`alias_set_mismatch`). Mutation check: with the pre-change prune six of these expectations fail. Descriptor modification / missing descriptor / runtime-guard behaviour are covered by the existing generated tests (`genesis_immutable_copy_alias_generated_test`, `genesis_m68k_alias_build_test`), which pass.

## 6. Scope statement

No default or recommendation change: Compatibility stays default and gen2 stays an explicit opt-in (ADR 0103/0109). Flicky still falls back (thunks). Descriptor addresses and offsets are private and are not recorded; only coarse aggregate sizes and counts appear. The no-alias identity is established by the explicit expectation that the result equals the plain region rule (the predicate falls through to the unchanged address test for non-alias identities) rather than against a pre-change binary.
