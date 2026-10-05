# Gen-3 evidence ledger (SEG-027-T001)

- Status: current-main ledger for the Gen-3 activation decision (ADR 0076).
- Head: product `main` at `472652c` (after SEG-032), dated 2026-10-02.
- Scope: documentation only. Nothing here changes product behaviour, admission, emission or runtime authority.

Every number below has four labels:

- **source**: `committed` (an ADR, test document or task record) or `re-measured` (measured on the head above for this ledger);
- **workload class**: `synthetic` (seeded or project-authored image) or `authorized` (a legally held local commercial image, recorded only
  as sanitized aggregates);
- **head/date**: the head or date the number belongs to;
- **status**: `current`, `current (equivalent head)`, or `superseded by <source>`.

Durable text contains aggregates only: counts, sizes, times, classes. It contains no ROM bytes, addresses, image hashes, per-title
identities beyond the workload names already used in accepted ADRs, traces or local paths.

## 1. Contradictions and superseded statements

| # | earlier statement | later evidence | resolution |
| --- | --- | --- | --- |
| C1 | SEG-009-T013 handoff: "about one owner per ROM byte, about 700-840 B of generated C per ROM byte, selective admission required" for SMS/Z80. | SEG-033 / ADR 0071: bounded 128-entry owners plus shared effect bodies. A real 512 KiB SMS image goes from 525,312 to 14,999 host functions, 341 to 177 MiB of C, 140.8 to 23.0 s build wall, 116.6 to 30.7 MiB executable. Behaviour and digests are unchanged. Re-measured here: 14,999 functions, 186.5 MB (177.9 MiB) of C, 28.0 s wall, 30.7 MiB. | **Superseded.** About 355 B of C per ROM byte and about 35 exact entries per host function. Z80 compile volume is no longer a measured blocker. "Selective admission required" is no longer established for Z80 (section 4). |
| C2 | Old SEG-028 text and ADR 0049 phrasing: "preparation-run observations are never the final correctness authority"; materialization was described as a future static-analysis problem. | SEG-032 / ADR 0073: a bounded, deterministic, generated-native **build-time** materializer inside `segarecomp build` is the production producer of Genesis Z80 RAM images. The **final executable** stays fully static: no discovery, no learning, no decoding. | **Superseded for Z80 RAM images.** Two different statements must be kept apart: "the final executable is static" (an invariant) and "the build process is purely static" (no longer required). ADR 0049's M68K preparation phase is still tooling-only (section 3, S2). |
| C3 | 2026-09-29 Gen-3 assumption: "Z80 images are small, so broad AOT may be sufficient." | Broad Z80 AOT is retained because SEG-033 measured it as affordable: 512 KiB in 23-28 s. Image size was not the argument. | **Replaced.** The reason is now the measurement, not image size. |
| C4 | Pre-SEG-025 M68K volume: 296 MB of C for the 512 KiB reference title, 546 MB for the 1 MiB title (ADR 0052 baseline). | ADR 0052 (SEG-025): 218 / 390 MB. Re-measured here: 218.0 MB, identical. | **Superseded.** Gen-2 viability is judged on the post-SEG-025 output only. |
| C5 | ADR 0051 revisit condition: "a full observed-PC coverage transport exists". | ADR 0053 delivered complete M68K execution-PC coverage. | **Satisfied for M68K only.** It does not exist for SMS/Z80 or Genesis Z80 (section 5). |
| C6 | ADR 0073 T002 record: provisional per-run instruction budget of 400 M. | ADR 0073 T008: frozen at 100 M dispatches. | **Superseded.** |
| C7 | ADR 0073 decision 5 (original): `z80_code_mismatch` fails the whole build. | ADR 0073 T012 (live payload) and the sound-fault amendment (T013): a structural mismatch isolates only the Genesis Z80 sound subsystem, and audio is reported as degraded. | **Superseded.** |
| C8 | 2026-09-29 Gen-3 type names (`ExecutableImage`, `MaterializationEdge`). | No such types exist. The real seams are `codegen::z80::CodeImage` / `ImageSet` and the ADR 0049 descriptor plus `ImmutableRomAotEntry`. | **Hypotheses only.** The smallest seam that the code suggests is preferred (SEG-028). |

## 2. Measurements

### 2.1 M68K immutable cartridge image: broad AOT (production)

Authorized real-title workloads:

| metric | value | source | class | head/date | status |
| --- | --- | --- | --- | --- | --- |
| aligned candidate starts / admitted AOT `U` / rejected | 262,144 / 246,293 / 15,851 | re-measured (`generated_code_scalability_report.py`, no-hints route) | authorized, 512 KiB reference title (Sonic 1) | 472652c | current |
| final compiled-entry set / AOT-owned / block-owned | 246,293 / 245,694 / 599 | re-measured | authorized 512 KiB | 472652c | current |
| generated C (M68K route only) | 218.0 MB in 60 TUs | re-measured | authorized 512 KiB | 472652c | current; equal to ADR 0052 |
| M68K generation wall / peak RSS | 10.7 s / 522 MiB | re-measured | authorized 512 KiB | 472652c | current |
| M68K compile, `-O0`, 1 job (sum of TUs) / largest TU RSS | 45.9 s / 187 MiB | re-measured | authorized 512 KiB | 472652c | current |
| M68K executable, `-O0` | 64.9 MB | re-measured | authorized 512 KiB | 472652c | current; ADR 0052: 64.9 MB |
| compile `-O2`, 8 jobs / executable `-O2` | 17.4 s / 33.7 MB | committed (ADR 0052) | authorized 512 KiB | SEG-025 | current (equivalent head: generated C identical) |
| 1 MiB title: generated C / `-O2` compile / `-O2` executable | 390 MB / 33.3 s / 60.8 MB | committed (ADR 0052) | authorized 1 MiB | SEG-025 | current (equivalent head) |
| `U` on six titles | 246k-498k (about 0.94 x aligned starts) | committed (ADR 0051) | authorized, 6 titles | SEG-024 | current (`U` unchanged by SEG-025) |
| full Genesis `segarecomp build`, default `-O2`, 8 jobs: total wall / compile wall / build CPU / executable | 39.3 s / 28.5 s / 185.5 s / 35.0 MiB | re-measured (`sms_build_benchmark.py --platform genesis`) | authorized 512 KiB | 472652c | current; includes Z80 materialization (2.4) |

Synthetic workloads:

| metric | value | source | class | head/date | status |
| --- | --- | --- | --- | --- | --- |
| synthetic large images, 2 MiB / 4 MiB: generated C, `-O0` compile, `-O0` executable | 0.88 GB, 20.2 s, 266 MB / 1.72 GB, 43.0 s, 519 MB | committed (ADR 0052) | synthetic | SEG-025 | current (equivalent head) |
| `U` growth | linear in image size, not code size (fixed flow = 4 identities at every size) | committed (ADR 0051) | synthetic 0.5-4 MiB | SEG-024 | current |

Reading: M68K broad AOT is affordable at 512 KiB-1 MiB. It is linear and expensive at 2-4 MiB (about 0.43 GB of C per MiB). The cost
grows with image size, not with executed code.

### 2.2 M68K discovery evidence (report-only experiments)

Authorized Sonic 1 attract workload: 23,200 frames, no input. Only aggregates are recorded.

| metric | value | source | head/date | status |
| --- | --- | --- | --- | --- |
| superset reduction: sound `L` vs `U` | `L = U` on 6/6 titles; unsound floors 3-8% of `U` | committed (ADR 0051) | SEG-024 | current, experiment not retained |
| challenger `D` (fixed flow) / `O` / recall | 1,276 (0.52% of `U`) / 10,512 / 9.73% | committed (ADR 0053) | SEG-026-T001 | superseded by the T002 row as the best local result |
| challenger plus exact PC-indexed recovery: `D` / recall / escapes | 6,765 (2.75%) / 42.74% / 0 | committed (ADR 0054) | SEG-026-T002 | current: the measured ceiling of local analysis |
| width-only PC-index dispatch: share of `O - D` | 94.4%. 59 of 70 observed sites read a register-relative field whose base is not locally provable. | committed (ADR 0054/0055) | SEG-026-T002/T003 | current |
| store provenance (strict / unsound ceiling) | 0 of 6 locations resolved / net 0 PCs | committed (ADR 0055) | SEG-026-T003 | current, experiment not retained |
| challenger cost | about 1.2 s including `U` | committed (ADR 0053) | SEG-026-T001 | current |
| SEG-030 sound `all` (`proven_or_unknown`): `D` / recall / escapes | 1,276 (0.52%) / 9.73% / 0; the earlier ~43% `all` recall relied on an unsound interrupt-register assumption | committed (ADR 0079) | SEG-030-T009 | current |
| SEG-031 hybrid admission `D ∪ islands ∪ closure`: hybrid / `U` | 246,293 / 246,293 (1.000000): degenerates to broad in round 1 (14 unbounded sites); Sonic 2 1.000000 (5), Cool Spot 1.000000 (22); OutRun, Streets of Rage, Golden Axe: SEG-030 `all` incomplete, broad | committed (ADR 0080) | SEG-031-T005 | current |

Reading: the remaining M68K gap needs pointer, alias, object-identity and interprocedural program-state analysis. Local heuristics are
exhausted.

### 2.3 Z80 immutable images (SMS): broad AOT (production)

Authorized real images:

| metric | value | source | head/date | status |
| --- | --- | --- | --- | --- |
| 128 / 256 / 512 KiB: build wall s | 7.3 / 12.9 / 23.0 | committed (SEG-033-T009) | fc33904 | current (equivalent head) |
| 128 / 256 / 512 KiB: compile CPU s | 43 / 80 / 148 | committed (SEG-033-T009) | fc33904 | current (equivalent head) |
| 128 / 256 / 512 KiB: generated C MiB | 46 / 88 / 177 | committed (SEG-033-T009) | fc33904 | current (equivalent head) |
| 128 / 256 / 512 KiB: host functions | 7,251 / 11,772 / 14,999 | committed (SEG-033-T009) | fc33904 | current (equivalent head) |
| 128 / 256 / 512 KiB: executable MiB | 8.3 / 15.9 / 30.7 | committed (SEG-033-T009) | fc33904 | current (equivalent head) |
| 128 / 256 / 512 KiB: max compiler RSS MiB | 118 / 169 / 260 | committed (SEG-033-T009) | fc33904 | current (equivalent head) |
| 512 KiB re-measure: exact entries / owners / shared bodies / host functions | 525,312 / 4,104 / 10,895 / 14,999 | re-measured (`sms_build_benchmark.py`) | 472652c | current |
| 512 KiB re-measure: generated C / build wall / compile wall / build CPU / executable / startup | 186.5 MB / 28.0 s / 19.9 s / 155.6 s / 30.7 MiB / 0.49 s | re-measured | 472652c | current; counts exact, CPU +5%; wall +22% on a shared, loaded host (outside the documented ±15%, so the wall figure is not used as a comparison) |

Synthetic images:

| metric | value | source | head/date | status |
| --- | --- | --- | --- | --- |
| random 512 KiB: wall / CPU / host functions / executable | 22.0 s / 148 s / 27,629 / 33.3 MiB | committed (SEG-033-T009) | fc33904 | current (equivalent head) |
| dense-legal 256 KiB: wall / host functions | 14.4 s / 39,345 | committed (SEG-033-T009) | fc33904 | current (equivalent head) |
| 64 KiB | not measured by existing tooling. It is not needed: cost is linear and 128 KiB is the smallest real image measured. | - | - | gap, accepted |

"Equivalent head" means the following. SEG-032 changed the Z80 emitter only for RAM-backed images. Immutable emission is byte-identical:
the golden digests are unchanged (ADR 0073 T012), and the 512 KiB re-measure reproduces the exact counts.

Remaining cost (SEG-033-T009): compile still scales with the number of exact entries (one entry-table row per legal start) and with
per-entry boilerplate. This is a known limitation, not a blocker.

### 2.4 Z80 materialized RAM images (Genesis): build-time materialization plus broad AOT (production)

| metric | value | source | class | head/date | status |
| --- | --- | --- | --- | --- | --- |
| images per workload | at most 2 (a boot stub plus one driver image), 2-5 epochs | committed (ADR 0073 T002/T008/T012) | authorized, 6 titles | SEG-032 | current |
| converging workloads | 5 of 6 converge. The sixth builds with Genesis audio degraded (structural Z80 code replacement, sound-fault isolation). | committed (ADR 0073 T012/T013) | authorized | SEG-032 | current |
| reference workload: images / discovery runs / total runs / epochs / window | 2 / 3 / 4 / 3 / 600 of 600 frames | re-measured (`status.json` of `segarecomp build`) | authorized 512 KiB (Sonic 1) | 472652c | current |
| reference workload: Z80 units final / compiled across iterations / reused | 19 / 31 / 0 | re-measured | authorized | 472652c | current |
| reference workload: generated Z80 C / Z80 objects | 11.4 MB / 3.8 MB | re-measured | authorized | 472652c | current |
| reference workload: emit / Z80 compile / materialize stage | 0.56 s / 2.7 s / 8.5 s (inside a 39.3 s build) | re-measured | authorized | 472652c | current |
| range over converging workloads: materialize stage / build / Z80 C / executable | 11-18 s / 60-115 s / 10.2-10.7 MB / 34-51 MB | committed (ADR 0073 T008, `z80-build-performance.md`) | authorized | SEG-032 | current for stage/build/executable; Z80 C size superseded by the T012 live-payload lowering (re-measured reference workload: 11.4 MB) |
| one image's broad AOT (8 KiB, two-mirror window) | 3.2-4.1 MB of C, about 1 s compile | committed (ADR 0073 T002) | authorized | SEG-032 | current |
| registry / AOT size | at most 8 images (bound), ceiling 16; 2 used | committed / code (`z80_images.hpp`, `z80_materialization.hpp`) | - | 472652c | current |

Supported and degraded classes:

- shape-stable self-patched displacement/immediate operands are supported (live payload);
- structural instruction-form replacement is unsupported and never executes; it latches a permanent sound-CPU fault and audio is
  degraded;
- RAM-backed starts longer than 4 logical bytes are typed `mutable_code` stubs (no workload needs them);
- epochs after the 600-frame window, and input-dependent epochs, are `z80_unknown_image` at run time.

The unsupported structural self-modification boundary is exactly this structural-byte class.

Known cost asymmetry: adding an image renames and reshards every Z80 unit, so 0 units are reused. Per-image emission would make the cost
linear. This is not needed at the measured bound.

## 3. Product seam inventory

Ownership classes:

- **CPU**: owned by a CPU library;
- **platform**: owned by a machine/platform library;
- **codegen**: owned by the C11 code generator;
- **neutral**: already CPU-neutral.

| id | seam | owner, path | contract (one paragraph) | class | real consumers |
| --- | --- | --- | --- | --- | --- |
| S1 | M68K broad immutable-ROM AOT | platform + codegen. `platforms/genesis/machine/src/frontend.cpp` builds `immutable_rom_aot_entries` (`frontend.hpp` `ImmutableRomAotEntry`). `libs/codegen/c11/src/frontend.cpp` emits owners and `genesis_compiled_entry_lookup`. ADR 0039 / 0045 / 0052. | Every aligned start of every structurally valid `raw_cartridge_rom` claim that decodes and lifts with the one decoder becomes an independent executable identity (`U`). Membership conveys no control fact. Compiled-entry precedence: ADR 0039. Bodies are grouped up to 128 entries per owner and factored by exact and own-PC identity. Dispatch is an exact generated table, and a miss fails closed. | platform (Genesis cartridge claims) + CPU (M68K decode/lift) + codegen | `segarecomp build` (Genesis), `emit-general-startup-bridge-c --immutable-rom-aot`, startup bridge, viewer |
| S2 | ADR 0049 immutable-copy alias | platform + codegen. `frontend.hpp` `ImmutableCopyAlias {execution_base, source_base, length}` and `ImmutableRomAotEntry::{execution_alias, alias_source_address}`. Discovery/validation in `platforms/genesis/machine/src/frontend.cpp`; guard emission in `libs/codegen/c11/src/frontend.cpp`; preparation phase in `tools/genesis_startup_bridge.py --discover-copy-aliases`. | The same immutable source bytes are decoded a second time at a work-RAM execution base. Provenance is the execution address, so relative and PC-relative operands are execution-relative and absolute ones stay absolute. A per-instruction byte-identity guard against the immutable bytes is the sole runtime authority, and a mismatch is `known_but_unemitted_target`. Descriptors are generated data and are validated fail-closed. Discovery is an optional bounded preparation phase (at most 64 rounds) in tooling. | platform (Genesis work-RAM window, cartridge claim) + codegen | `emit-general-startup-bridge-c --immutable-copy-alias`, `genesis-reachability-challenger --immutable-copy-alias`, `genesis_startup_bridge.py`. **Not consumed by `segarecomp build`**: `build_command.cpp` passes no alias, so the consumer route compiles no RAM-copy identity today. |
| S3 | reachability challenger | platform. `platforms/genesis/machine/src/reachability_challenger.cpp`, `segarecomp genesis-reachability-challenger`. ADR 0053/0054. | Report-only discovery from architectural roots over `m68k_control_successors`, with a challenger-owned continuation set, optional exact PC-indexed recovery and fixed-point invalidation. It never consults `U`, runtime coverage, hints or external disassemblers. It never authorizes admission. | platform (Genesis roots/vectors) over CPU projections | CLI and tests only (`reachability_challenger_test`, `reachability_pc_index_recovery_test`, `reachability_coverage_compare.py`) |
| S4 | M68K control-successor projection | CPU. `libs/cpu/m68k/.../control_successors.hpp`. | A thin projection of `m68k_operation_effect`: the fixed successors and stacked continuations of one instruction. It adds no semantics. | CPU | S3 |
| S5 | M68K finite-value analyses (two) | CPU + platform. (a) domain and transfer in `libs/cpu/m68k/src/finite_register_values.cpp` (SEG-026-T002, ADR 0054/0055 fix): an `Unknown`-or-exact-finite-set domain for one data register, with a set-size resource bound and immutable-byte reads only; its demand-driven backward evaluator is a memoized recursion with a step budget (`IndexEvaluator` in the platform-owned `platforms/genesis/machine/src/reachability_challenger.cpp`). (b) `M68kStaticGraphWalker::analyze_finite_register_values` in `libs/cpu/m68k/src/static_discovery.cpp` (SEG-007 Gen-2 static discovery): a forward, context-insensitive finite register-state fixed point over the canonical control adjacency, used by Gen-2 Tier-1 computed-control resolution. | Both are M68K-specific fixed-point/recursive analyses with ad-hoc domains and no shared code. What remains after the T003 removal: only the (a) width-correct operand read fix. The store-provenance machinery is gone. | CPU (a: CPU domain + platform evaluator) | (a) S3 only; (b) Gen-2 production static discovery (Tier-1 exact sets) |
| S6 | Z80 code images | codegen. `libs/codegen/c11/include/segarecomp/codegen/c11/z80.hpp`: `CodeImage {identity <= 0xFFFF, kind, bytes, windows, live_bytes}`, `ImageSet`, `CodeWindow {uint16 base, first_offset, length}`, `ImageKind {invariant, banked}`, `emit_image_set`. | Broad Z80 AOT over every legal start of every image. The exact entry key is `(identity << 16) | (PC or window offset)`. Owners are grouped (128) and effect bodies shared (ADR 0071). A RAM-backed image (`live_bytes`) runs a structural guard plus live payload per entry (ADR 0073 decision 7). | codegen, Z80-shaped: 16-bit windows, 16-bit identity, 4-byte live guard | SMS `build_image_set` and `emit.cpp`; Genesis `z80::build_image_set` and `emit_registry` |
| S7 | SMS image/mapping ownership | platform. `platforms/master-system/machine/.../image_set.hpp`, `sms_mapper_contract.h`. ADR 0065. | A validated cartridge becomes an invariant image (first 1 KiB, or the whole 32 KiB `rom_only`) plus one banked image per 16 KiB bank, admissible in slots 0-2. It is derived from the same table as the runtime memory map. RAM is never code. | platform | `segarecomp build --platform master-system` |
| S8 | Genesis Z80 epochs, registry and materialization | platform. `platforms/genesis/machine/.../z80_images.hpp`: `Epoch` (8 KiB RAM plus written bitmap), `Extent`, `content_digest`, `signature_digest` (S1*), `Image`, `Registry` (bound 8), `build_image_set`, `emit_registry`. `z80_materialization.hpp`: `PassRunner {prepare, run}`, `PassObservation`, `materialize` with frozen bounds. `apps/segarecomp/build_command.cpp` `ProcessPassRunner`. | Bounded build-time fixed point over hardware-defined runnable epochs. The content hash names a compiled image; the activation signature selects it at run time. Unknown signature means `z80_unknown_image`. A confirming run must reproduce. Every bound is a constant, and every violation is a typed failure. ADR 0073 decision 8: a later static producer replaces steps 1-4 only. | platform: Genesis epoch, signature, 8 KiB, mirrors | `segarecomp build` (Genesis) |
| S9 | compiled-entry tables and code-image identity | codegen. `libs/codegen/c11/.../compiled_entry_table.hpp` (`emit_compiled_entry_table`, `CompiledEntryBinding`, `CompiledEntryChunking`), `translation_units.hpp` (`TranslationUnitSharder`). | Exact key-to-owner tables with owner-ID compression and 64 Ki-row chunking, plus deterministic TU sharding. M68K keys are 24-bit execution PCs (aliases are work-RAM PCs). Z80 keys are `(image identity, PC or offset)`. | neutral (already shared) | M68K emitter (`frontend.cpp`), Z80 emitter (`z80.cpp`) |
| S10 | M68K execution-PC coverage observer | platform. `platforms/genesis/runtime/runtime.h` `GenesisRuntime.execution_coverage`; `platforms/genesis/viewer/execution_coverage.*`. ADR 0053. | A host-owned, NULL-by-default observer: a 1 MiB bitmap of every retired PC, a first-entry witness with an interrupt-resumption stack, zero semantic effect, excluded from digests, aggregates-only output. | platform: Genesis runtime, M68K PC width | `genesis_startup_bridge.py --execution-coverage`, `reachability_coverage_compare.py`, tests |

Observations used by ADR 0076:

- **Two real non-cartridge executable-image producers exist** and share no type: S2 (M68K, a statically proven verbatim copy, guarded per
  instruction, descriptor-based) and S8 (Z80, build-time materialization, guarded structurally, registry-based). A third, S7, produces
  immutable banked images. S6 is already the code-image artifact for two Z80 producers (S7 and S8). M68K has no artifact type of its own;
  S1/S2 identities are rows of `FrontendAnalysis`.
- **Duplicated M68K-specific value machinery** (S5a and S5b, sharing no code) exists with no shared solver. SEG-026 showed that the needed precision
  (pointer/alias/object) is beyond both.
- **S9 is the only already-neutral shared infrastructure.** The rest is CPU-, platform- or codegen-owned, and each has exactly the
  consumers listed.

## 4. Where selective admission is and is not established

- **M68K:** need demonstrated by SEG-024/026. Cost is linear in image size: 218 MB of C at 512 KiB, 1.72 GB at 4 MiB synthetic. Recall of
  the best local analysis is 42.7%, at 2.75% of `U`. Larger cartridges (2-4 MiB) are the measured cost driver, but no 2-4 MiB
  authorized workload is in the corpus. **SEG-031 (ADR 0080):** a sound hybrid (`D` plus bounded fallback islands plus closure) equals
  broad on every authorized title: each title has at least one uncovered site with no bound narrower than the whole program
  (interrupt-resumption register effects, unbalanced/computed RTS, pointer provenance, or an incomplete SEG-030 solve). Production
  stays broad AOT; selective admission is not established until those generic blockers are removed.
- **Z80:** not established. SMS broad AOT costs 23-28 s at 512 KiB after SEG-033. A Genesis Z80 image is 8 KiB and costs about 1 s.
  Selective Z80 admission would need new evidence: for example a materially larger Z80 code space, a compile/RSS budget violation on a
  supported host, or per-image counts well above the bound of 8.

## 5. Validation-gap inventory: execution-PC observation

| CPU / platform | complete retired-PC observation | first-entry witness | consequence |
| --- | --- | --- | --- |
| M68K / Genesis | yes (S10, ADR 0053) | yes | Falsifies any M68K discovery claim (`O - D`). |
| Z80 / SMS | yes (SEG-031-T006, ADR 0080): measurement builds only, keyed by (code-image identity, PC) | no | Falsifies any future SMS/Z80 discovery claim; 3 authorized titles observed with 0 escapes. |
| Z80 / Genesis | yes (SEG-031-T006, ADR 0080): inside the materialization pass, keyed by materialized image identity | no | A future static Genesis Z80 producer's executed footprint can be compared against the materialized images. |

Need for future validation:

- a Z80 observer is **not** needed while Z80 stays broad AOT, because no Z80 admission claim exists;
- it **is** a prerequisite for any future Z80 discovery or selective-admission claim, and for comparing a static Genesis Z80 producer's
  executed footprint against a materialized image;
- SEG-029's synthetic Z80 adapter is architectural validation over project-authored fixtures with known ground truth, so it does not
  need the observer.

The owner was recorded by SEG-027-T005 and ADR 0076; SEG-031-T006 delivered the Z80 observer (docs/testing/execution-coverage.md).

## 6. Reproduction (local, authorized images; aggregates only)

```sh
cmake --preset dev && cmake --build --preset dev
python3 tools/generated_code_scalability_report.py measure --segarecomp build/dev/apps/segarecomp/segarecomp \
  --rom games/<genesis-512k>.md --out-dir .cache/<run> --report .cache/<run>.json
python3 tools/sms_build_benchmark.py --cli build/dev/apps/segarecomp/segarecomp --root . \
  --image rom:games/<genesis-512k>.md --platform genesis --json .cache/<run>-genesis.json   # Z80 aggregates: <work>/out/status.json
python3 tools/sms_build_benchmark.py --cli build/dev/apps/segarecomp/segarecomp --root . \
  --image rom:games/sms/<sms-512k>.sms --json .cache/<run>-sms.json
```

Host: Apple clang 21, arm64, 12 CPUs, shared workstation (wall ±15%). Counts and sizes are deterministic; times and RSS vary with the
machine.

## Update after SEG-036 (ADR 0083, 2026-10-05)

The M68K broad-AOT entry representation changed (helper-backed entries are table rows; admission and the compiled-address set are
unchanged and digest-equal). For the 512 KiB reference title (re-measured, product `main` after SEG-035 vs SEG-036): generated C 220.2 ->
177.9 MB, compile CPU `-O0` 70.1 -> 57.3 s, executable `-O0` 64.9 -> 54.9 MB and `-O2` 33.7 -> 25.1 MB, dispatch-bound runtime +5.3% (min) / +5.7% (median).
1 MiB titles: executable `-O0` -18.5% to -25.1%. The rows above keep their original head label and are superseded by ADR 0083 for current
size/time figures. The Gen-3 activation decision (ADR 0076) is unchanged: this reduced cost, not the admitted set.
