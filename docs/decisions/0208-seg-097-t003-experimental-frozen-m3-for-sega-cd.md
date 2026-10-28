# ADR 0208: SEG-097-T003 - experimental frozen M3 + B35 selective admission for Sega CD

- Status: Pre-registration accepted (committed before the comparison of section "Pre-registered comparison" was run on the final implementation).
  The result and decision are appended under "Result" and "Decision".
- Related: ADR 0100/0101/0102 (frozen gen2 M3, native evaluator), 0103 (admission authority and lifecycle), 0109/0137 (Genesis Optimized and its default),
  0131 (ML reuse boundary), 0132 (module lists), 0140 (the failed complete-recall gate on Sonic CD modules), 0142 (verified broad AOT), 0206/0207.
- Privacy: only project-authored fixtures are used here. Nothing about a commercial disc or BIOS is recorded.

## Acknowledged prior finding

ADR 0140 measured the frozen M3 at B35 inside seed-union regions of 36 Sonic CD modules and found **306 independently labelled instruction starts outside the
selected regions in 3 modules**: M3 has no demonstrated complete recall for Sega CD, so it is not a correctness authority and not a default. Nothing in this
task changes that. The task builds the *consumer path* ADR 0140 recorded as missing ("no Sega CD R-admission path exists"), as an explicit experiment, and
measures what it does.

## Decision

1. **Authority is untouched.** The proposal may only *remove* entries from what the broad emission would compile. It cannot create an image, a mapping, an
   overlay candidate, an executable range, a root or an entry; it is applied after the image set (IP/SP, the digest-checked module list, extents,
   epoch/template admissions) is fixed, and an inventory comparison (images, mappings, roots, producers) is part of the tests. The per-instruction byte guard,
   the whole-range overlay liveness, the typed stops and the unchanged dispatch are the only execution authority. An omitted entry is simply not in the
   dispatch: reaching it is the existing typed `entry_not_compiled` / `image_entry_not_compiled` stop.
2. **Scoring unit = the image's immutable source bytes.** One proposal per image (IP, SP, module, BIOS, copy, disc extent, epoch image), whatever the number of
   mappings; the cells are 128-byte cells of the image's own bytes from its offset 0; no address, base or mapping is a model input. Admitted entries are
   source-relative offsets and are shared by every mapping (and by every shared position-independent body, ADR 0207).
3. **Producer = the unchanged native frozen gen2 M3 + B35** (`propose_genesis_ml2_executable_regions`: CPU-owned decode tokens, binary32 CNN, rank selection,
   `R = selected cells U seed cells`). No retraining, no change of B35, weights or Genesis identities. It lives in `apps/segarecomp/sega_cd_aot.cpp`; the Sega CD
   machine library knows only a callback (`selective_admission.hpp`) and depends on no other machine and no model.
4. **Seeds (certain code) = declared roots**: the boot entry of IP/SP, a module's declared entry roots, the BIOS image's reset entry. Their cells are always in R.
5. **Validation before use** (shared by both emitters, `validate_region_proposal`): the producer must succeed; ranges are non-empty, even, ascending, disjoint,
   inside the image; every seed is inside R; after application every declared root that the broad emission compiled is still compiled; something remains
   admitted. **Any** failure - producer (`model_identity`, `rom_size`, `token_range`, `non_finite_score`, `empty_proposal`), validator (`validator_rejected:<detail>`)
   or an emission error - emits the **broad** program in the same call and reports the exact reason and the offending image. The report says `effective: broad,
   fallback: true`; an experimental result is never claimed for a broad build.
6. **Policy vocabulary** (the product's, as for Master System's experimental policy): `--aot-policy compatibility|optimized [--aot-model gen2]` on
   `segarecomp sega-cd emit` and `sega-cd build`. **Compatibility is the default and stays byte-identical to before**; for Sega CD `optimized` is
   EXPERIMENTAL (reports carry `experimental: true`) and only the frozen gen2 model is accepted. It is available for both emission profiles: the
   synthetic post-BIOS profile (broad = statically reached instructions) and the BIOS-image profile used by the launcher (broad = every aligned start of every
   admitted image). `status.json` records the `aot_policy` object of the final emission.
7. **Identities stay separate.** Model, rank plan, weights and token contract are the Genesis constants; the *policy identity*
   (`segarecomp.sega_cd.aot_policy_identity.v1|contract|requested|family|producer|model|schema|tokens|rank|weights`) is ROM-independent and enters the program
   identity **only when the experimental request is made** (not when it falls back: the admitted set is a pure function of the inputs and this identity, and a
   build that was not asked to be experimental can never alias one that was). The admitted-set digest `k_sha256` (the sorted source-relative offsets per image) is
   reported separately; source-image, mapping and module-list identities are unchanged. Position-independent emission has its own emitter identity (ADR 0207).
8. **Launcher**: an explicit, off-by-default "Optimized (experimental)" choice for a Sega CD game with a plain warning (it may omit code and can stop at an
   uncompiled entry), the effective policy shown after the build, and a one-click Compatibility rebuild when an experimental build ends at an uncompiled entry.
   The launcher knows no model, threshold or compatibility rule; it passes the product's option and reads the product's report.
9. **Out of scope**: a default change, a Genesis change, runtime ML, a new validator that weakens admission, choosing a budget from results.

## Pre-registered comparison (fixed before the final run)

Subjects (project-authored, aggregate figures only): **S1** the `unvisited` disc (case `pi`) with the module list DISCOVERED by T001, synthetic profile; module C is
mapped at 2 bases. **S2** the BIOS-image profile fixture `direct` (a 16 KiB project-authored BIOS-like image, one disc extent). Arms: **A** the per-mapping broad emission
(ADR 0132 reference); **B** broad with position-independent bodies (ADR 0207); **C** experimental M3 + B35 with position-independent bodies.

| item | registered |
|---|---|
| inventory | the image/mapping/root/producer inventory of A, B and C is identical (gate) |
| admitted offsets | distinct source-relative instruction offsets per image, per arm; C is a subset of A (gate: C never adds) |
| independent labels (S1 only) | the encoder's own instruction starts of the fixture (IP, SP, A, B, C), recorded by the fixture builder independently of any decoder; recall = labels admitted / labels; **G_recall: 100 % on every image** |
| executed-start retention | instruction starts executed on the three input routes under arm A, and how many arm C retains |
| cost | native compiled instruction bodies, generated C bytes, compile CPU and peak memory (`cc -std=c11 -O1`, one process), object size |
| runs | the three routes (default/A, B, C) per arm: result words and typed stop; C may stop at an uncompiled entry, never anywhere else |
| determinism | two emissions of each arm are byte-identical, `k_sha256` equal |
| model identity | the reported model/rank/weights equal the identities `aot-policy-identity --aot-policy optimized --aot-model gen2` prints for Genesis |
| fallback | each producer/validator failure is covered by the fake-producer test (`sega_cd_selective_admission_test`): broad text, exact reason |

Registered expectation, stated before the run: G_recall will **not** hold on the fixtures (the B35 budget selects 35 % of the cells of a 2 KiB module sector and of
the small IP/SP, ADR 0140), C will stop at an uncompiled entry on routes that arm A completes, and the saving on S2 will be large because broad AOT of a
mostly-data BIOS image compiles every aligned start. Outcomes: `EXPERIMENTAL_PATH_IMPLEMENTED` iff every gate marked (gate) holds and the identity,
fallback and launcher criteria hold; `NOT_PROMOTED` is recorded regardless unless G_recall holds on an independent held-out set, which this task cannot provide;
the milestone does not claim M3 is safe for general Sega CD gameplay.

(Chronology, for the record: the seam, the emitters and a first CLI run on `unvisited/pi` and on S2 were written and run exploratorily before this section was
committed; they showed the stop at an uncompiled entry and the S2 reduction qualitatively. The table, the labels and the figures below were produced after.)

## Result

Run: `tests/sega_cd_experimental_m3_test.py` (38 checks, real native model, CI test `sega_cd_experimental_m3_test`), `tests/sega_cd_selective_admission_test.cpp` (fake
producer), `tests/sega_cd_unvisited_discovery_test.py`, `apps/segarecomp-launcher/launcher_sega_cd_test.cpp` (policy rows) and
`launcher_sega_cd_flow_test.py` (an experimental build through the launcher). Figures are from one local run on project-authored fixtures (GCC, Linux, `-O1`);
CPU/memory figures are noisy at this scale.

**S1** - the `unvisited` disc with the module list discovered by T001 (synthetic profile; independent labels = the fixture encoder's own instruction starts):

| | A per-mapping broad | B broad, position-independent bodies | C experimental M3 + B35, position-independent bodies |
|---|---:|---:|---:|
| image / mapping / root / producer inventory | reference | identical | identical |
| distinct source-relative offsets admitted | 214 | 214 | 164 (a subset of A in every image) |
| native instruction bodies | 249 | 214 | 164 |
| generated C bytes | 241,201 | 214,462 | 161,040 |
| compile CPU s / peak KiB / object bytes | 0.34 / 48,144 / 126,976 | 0.31 / 46,940 / 121,704 | 0.20 / 40,792 / 95,168 |
| label recall IP / SP / A / B / C | 28/28, 133/133, 8/8, 8/8, 35/39 | same as A | 23/28, 88/133, 8/8, 8/8, 35/39 |
| executed starts retained (3 routes, 214 starts under A) | 214 | 214 | 164 |
| routes (default / B / C) | complete: 787,225 / 263,087 / 525,215 instructions, correct markers | identical to A | all three stop with the typed `entry_not_compiled` after 70 / 68 / 71 instructions |
| two emissions identical, `k_sha256` equal | yes | yes | yes |

The 4 labelled starts of module C that arm A also misses are dead instructions after an unconditional branch / indirect jump: the encoder labels them, the
static traversal rightly does not reach them. Module C (two mappings) is scored **once** (one policy-report row) and its admitted offsets are identical for both
mappings. Arms A and B run every route identically (the ADR 0207 result); B compiles 14 % fewer bodies than A (214 against 249, the ADR 0207 sharing of module C's two mappings) and C a further 23 % fewer than B (164).

**S2** - the BIOS-image profile fixture `direct` (a 16 KiB BIOS-like image and one disc extent), the launcher's own build route (`segarecomp sega-cd build`):

| | A Compatibility | C experimental |
|---|---:|---:|
| status / rounds / result | ok / 3 / `idle` (runs to its end) | ok / 2 / `stopped`, typed `entry_not_compiled`, frontier `no_delivery_evidence` |
| aligned starts compiled | 8,574 (everything the CPU owner lowers) | 3,072 (of 8,574; 35 % B35 + seed cells) |
| generated C bytes | 9,350,968 | 2,629,952 |
| whole build CPU s / peak KiB | 18.6 / 374,428 | 6.4 / 282,576 |
| discovered extents | 1 | 0 (the program stops before the loader's data path is reached) |
| program identity | differs (the request enters it) | |

Fallbacks (producer failures `model_identity`, `rom_size`, `non_finite_score`, an empty proposal, and validator rejections of odd/empty/outside/unordered
ranges and of a seed outside the proposal) are covered with a fake producer: each gives the broad text, the exact reason, the offending image, no admitted-set
claim and the unchanged identity of the request. The real model never failed on the fixtures, so no fallback was observed with it.

## Decision

- **`EXPERIMENTAL_PATH_IMPLEMENTED`**: every registered gate holds. The frozen gen2 M3 + B35 is selectable explicitly for Sega CD (CLI `--aot-policy optimized
  [--aot-model gen2]` for `emit` and `build`; the launcher's off-by-default "Optimized (experimental)" choice and a Compatibility rebuild), runs natively with the
  frozen identities, reports a truthful effective policy, never expands image authority, preserves the byte guards and the typed stops, scores each image once, and
  leaves Genesis policies, weights and identities untouched (the Genesis `aot-policy-identity` output is pinned in the test). No runtime ML and no runtime code
  generation exist.
- **`NOT_PROMOTED`**: the complete-recall gate fails, as ADR 0140 predicted and as registered: M3 + B35 omitted 50 labelled starts of the small IP (158 bytes) and SP (716 bytes) and every
  route of the fixture ends at an uncompiled entry that the broad build compiles. On the BIOS-image fixture it cuts build CPU by about two thirds while the program
  stops at its first omitted entry. This is a build-size saving that costs correctness of the route, on a fixture where there is no size problem to solve (ADR 0140:
  no compile-size/time problem exists for the Sega CD module programs). It is **not** a default, not a correctness authority and not a claim that M3 is safe for
  general Sega CD gameplay. Compatibility remains the default and the fallback; the Compatibility emission is byte-identical to before.
- Limits: two small project-authored subjects; the BIOS-image profile's per-image scoring covers BIOS, copy, disc-extent and epoch images the same way, but no
  commercial disc was run (no aggregate figure exists for one); a model-proposed region is never a completeness certificate.
