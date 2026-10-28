# ADR 0208: SEG-096-T003 - experimental frozen M3 + B35 selective admission for Sega CD

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
