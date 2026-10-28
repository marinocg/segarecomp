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

Review notes: (1) `sega-cd build` now ends the build at the frontier `uncompiled_entry_in_admitted_extent` instead of admitting the same extent again; this also applies to Compatibility
(it never occurred there because an admitted extent is compiled), the emission itself being byte-identical. (2) The request, not the outcome, enters the program identity; the admitted
set also depends on binary32 scores, which are bit-identical across IEEE-754 hosts under the pinned numeric contract (ADR 0102) - the same assumption the Genesis policy makes. (3) The
BIOS-image selective path runs a second, broad emission for the comparison counts (about twice the emission cost); a failure of that comparison emission does not discard a valid selective result.

## Follow-up (SEG-097 M1 follow-up): the experimental policy scores only game modules

**Why.** The first result (above, "arm H" below) was too aggressive: on every synthetic route the program stopped at uncompiled *boot IP/SP* code while the actual game modules kept every
executed start, and on the BIOS-image fixture the build stopped before it could discover the executable disc extent because BIOS-derived code had been cut. ADR 0140 separately showed
incomplete M3 recall inside real game modules. The experiment therefore asks a narrower question: with firmware, BIOS-derived images and boot code under full broad AOT, does M3 on the
independently admitted game modules alone keep routes complete?

**Decision (contract `selective_admission.v2`, policy identity changed).** Eligibility is read from explicit executable-image provenance, never from an address range, a name, a file
extension or a title:

| image | role / provenance | policy |
|---|---|---|
| BIOS image | `bios_firmware` (BIOS-image profile) | `broad_required` |
| BIOS copy images, sub-BIOS epoch images (materialized or recipe) | `bios_derived_copy`, `bios_derived_epoch` | `broad_required` |
| boot IP / SP (synthetic profile) | producer `sega_cd.boot_ip` / `sega_cd.boot_sp` | `broad_required` |
| discovered disc extent inside the disc's system area (first 32 KiB: header, IP, SP) | `boot_area_extent` | `broad_required` |
| user-declared disc image | `manual_disc_image` (role not established) | `broad_required` |
| disc-loaded module (module list) | producer `sega_cd.disc_module` | `m3_selected` |
| discovered disc extent outside the boot area | `disc_extent` | `m3_selected` |
| any other / unknown producer | `unknown_provenance` or its producer | `broad_required` |
| runtime-generated BIOS jump-slot templates | not images | unchanged, never scored |

M3 still only proposes: it discovers no image, extends no range, decides no mapping and weakens no byte guard. The model, B35 and the native evaluator are unchanged. With no eligible image
the build is broad and says `no_eligible_images`; a producer or validator failure still falls back to the whole broad build and marks the eligible images `broad_fallback`. Every image is
reported with `selection`, `selection_reason`, source size and entry counts; images that are not scored are counted as broad-admitted (`admitted_entries == broad_entries`), and the report
adds `m3_images`, `m3_broad_entries`, `m3_admitted_entries` for the optimized part only. The contract name moved to `...selective_admission.v2` and the policy identity carries
`scope=game_modules`: a program built under the previous eligibility can never be reused. The Compatibility policy identity text, report keys and every generated Compatibility output are unchanged (pinned in the test). The historical scope remains reachable for evidence only, as `--aot-scope all-images` (identity `scope=all_images`); it is not offered by the launcher, which uses the shipped scope.

**Results** (`tests/sega_cd_experimental_m3_test.py`, 52 checks; arms A = Compatibility, H = historical global scope, C = shipped game-module scope).

Synthetic three-route fixture (module list discovered by T001; independent labels = the fixture encoder's starts):

| | A | H (historical) | C (shipped) |
|---|---:|---:|---:|
| route default / B / C | complete | stops `entry_not_compiled` after 70 / 68 / 71 instructions (boot SP) | **complete, identical result words, retired counts and register state to A** |
| executed starts retained | 214/214 | 164/214 | **214/214** |
| IP / SP admitted instruction sets | reference | cut (23/28, 88/133 labels) | **equal to A exactly** (digests) |
| modules scored | - | 3 (+ IP, SP) | 3 genuinely scored (cells 16 each, 6 selected each), module recall 8/8, 8/8, 35/39 = A's |
| native bodies / C bytes (with source-relative bodies) | 249 / 241,201 | 164 / 161,040 | 214 / 214,462 (= source-relative Compatibility) |

The 3 modules are 28-byte routines: M3 keeps all their code, so the game-module-only policy saves **nothing** on this fixture beyond what ADR 0207 saves. It is a correctness
improvement (no regression against Compatibility), not a size gain.

BIOS-image fixture (`direct`, end-to-end `sega-cd build`):

| | A | H | C |
|---|---:|---:|---:|
| rounds / result / extents discovered | 3 / `idle` / 1 | 2 / `stopped` `entry_not_compiled` / 0 | **3 / `idle` / 1** (reaches and discovers the extent; runs to the same end) |
| entries admitted (all images) | 10,439 | 3,072 of 8,574 | 9,280 of 10,439 (the one scored extent: 706 of 1,865) |
| generated C bytes | 9,350,968 | 2,629,952 | 8,143,726 |
| build CPU s / peak KiB | 18.5 / 374,228 | 6.4 / 282,408 | 16.9 / 373,852 |

Eligibility negatives and positives on the same fixture: the BIOS image is `broad_required`/`bios_firmware`; with nothing but firmware admitted the build is broad (`no_eligible_images`); a
user-declared disc image stays `broad_required` (`manual_disc_image`); a discovered extent inside the boot area stays `broad_required` (`boot_area_extent`); a discovered extent outside it is
scored (`m3_selected`, `disc_extent`).

Authorized local commercial title (Sonic CD, the launcher route: BIOS image and disc only, no input, no reference; one run per arm; **aggregates only, nothing derived from the disc or BIOS is kept
in the repository**; the full Compatibility build reproduces ADR 0205's route A: 17 rounds, 12 extents, ending at the documented PCM boundary):

| | A Compatibility | H historical global | C game-module scope |
|---|---:|---:|---:|
| rounds / extents discovered | 17 / 12 | 9 / 5 | **9 / 5** |
| end of the build's route | `pcm_unsupported` (the documented audio boundary) | `code_bytes_mismatch` at the main boot-area base, frontier `no_progress` | `code_bytes_mismatch`, same place, frontier `no_progress` |
| image classes scored | - | firmware 1, BIOS epoch 1, boot-area extents 3, disc extents 2 | **disc extents 2 only** |
| entries admitted per class (broad -> admitted) | all | firmware 61,869 -> 22,329; epoch 64,628 -> 22,493; boot-area 30,707 -> 10,806; disc 11,154 -> 3,945 | firmware, epoch, boot-area unchanged; **disc 11,154 -> 3,945** |
| instruction functions in the final emission | 185,289 | 59,588 | 153,304 |
| generated C bytes | 183,880,647 | 57,504,834 | 146,086,169 |
| build CPU s / peak MiB | 844 / 1,130 | 238 / 550 | 462 / 623 |

Where C stops: the instruction that no admitted variant matches lies in one of the two disc *game-module* extents (a small module loaded by the main processor; roughly a third of its instruction
entries were admitted): the missing code belongs to a **game module**, not to boot code or firmware. This is the incomplete-recall limitation of ADR 0140,
now observed in a build rather than a benchmark; no code was admitted heuristically to get past it (the frontier extent was already admitted, so the resolver correctly reports `no_progress`).
The savings of C and H against A are NOT like-for-like: both builds end 8 rounds earlier, with 5 extents instead of 12. Independent source-backed labels (ADR 0140 needs the public
disassembly built locally) were not rebuilt in this task: recall against independent labels is measured only on the synthetic fixture.

## Decision (follow-up): `PARTIAL`

- Synthetic routes: `GO` - all three complete with Compatibility's exact results, boot/IP/SP sets equal, modules genuinely scored; but no size gain (modules are tiny).
- BIOS-image fixture: `GO` for reach - keeping BIOS-derived code broad lets the build discover and compile the disc extent and finish like Compatibility; saving 13 % C / 9 % CPU.
- Real title: no gain that can be claimed. The shipped scope removes the boot-code stop of the historical scope (both stop at the same game-module instruction, so on this title the
  boot-code policy was not the binding limit) and still ends 8 rounds before Compatibility because a game module's required instruction was not selected. 
- The policy stays experimental and off by default; M3 is not promoted. **Recommendation:** no further M3 work before M2. Any retry needs the independent module labels and a recall
  fix (budget or seeding by evidence), which is a research question, not a launcher feature; the loader-coverage work of M2 has a clearer benefit than a 21 % build-size reduction that
  costs the route.
