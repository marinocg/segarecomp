# ADR 0114: SEG-050 final decision (T006): alias admission and materialized-image ML

- Status: decision (checkpoint C6, subject to the independent review recorded in section 4). Two SEPARATE conclusions; neither depends on the other.
- Evidence: ADR 0110 (inventory, 19-image corpus screening, baselines), 0111 (alias admission O1), 0112 (frozen pre-registration + erratum), 0113 (results, T005 closure). Delivery: one product PR (#96), checkpoints C1 `ae37825`, C2 `54adae4`, C3 `97006c4` (+ erratum `cb88bc0`), C4 `194b014`, C5 `ab9dc3e`, C6 this commit.

## A. Alias / RAM-thunk selective admission — `ALIAS_ADMISSION_IMPROVED` (narrowly)

Generic correction safely implemented: an ADR 0049 copy-alias identity belongs to the proposed region when its byte-identity source is in it (O1). It uses only the existing descriptor and byte-identity source; no region is widened, no reachability or discovery pass was added, the structural closure, the validator (every alias identity a mandatory root) and the runtime guards are unchanged, and builds without aliases are unchanged. Measured: Golden Axe (432-byte copy) now builds as `ml_region_gen2` instead of falling back: -49 % wall, -54 % CPU, -58.8 % generated C versus Compatibility, behaviourally identical executables, deterministic, strict-C11 clean. Limits stated: one image demonstrates the end-to-end gain; **Flicky still falls back** (14 observed RAM `JMP` thunks that the copied code calls are outside the region). O2 was rejected (widens R, no complete-build benefit); an unregistered exploratory arm that treats mandatory alias/thunk identities as members of K0 admitted all Flicky emissions (52.2 % of the universe) and is recorded as a **follow-up needing its own pre-registration**, not adopted.

## B. Materialized-image ML — stopped: proof/truth/economics insufficient (`CORPUS_BLOCKER`)

Of the four outcomes (reuse frozen M3/B35 | narrowly justified separate model | retain broad materialized-image AOT | stop): **stop, with broad materialized-image AOT (plus ADR 0111 alias integration) retained.** Reasons, each independently sufficient:
1. Corpus: no M68K decompression producer exists; 2 copy-only A lineages of 19 screened images, exact truth for one (Flicky, a model-fitted title) => 0 blind qualifying lineages against the pre-registered 3 (E1) / 14 (E2). No label was manufactured.
2. Structure: the unchanged validator admits no proper subset of an alias image, so ML selection inside a materialized image has no admission path (R2 + K1 + E1 jointly unsatisfiable for alias images, stated before results, ADR 0112 erratum 1).
3. Economics: by the pre-registered rule materialized images are not an important compile-cost problem across lineages (Flicky 26.8 % of identities / 69 % of C; Golden Axe 0.09 %), and the admissible savings are already obtained by ADR 0111.
Exploratory, known-corpus only (no gate weight): frozen M3/B35 on Flicky's copy transfers exactly (identical selection to direct-ROM scoring; image-boundary effect on edge cells only) and its ranking is perfect, but B35 is density-limited (39.3 % of cells hold code vs a 35.2 % budget, 332 of 4,602 starts outside R). That is a code-density/budget limit, not a transfer failure and not a pass; any different selection unit would be a separate pre-registered hypothesis on independent lineages. T005 closed NOT_JUSTIFIED: no production ML code.

## C. Milestone outcome and follow-ups (as new records, not silent scope)

Outcome of the four: **stopped — proof/truth/economics insufficient** for B; **improved** for A. Follow-ups: (1) data acquisition: operator-provided local images that demonstrably copy code to work RAM plus exact source rebuilds (>= 3 lineages for E1; 8 training + 3 held-out disjoint lineages for E2) — only then re-open B; (2) a separately pre-registered evaluation of making mandatory alias/RAM-thunk identities unconditional members of K0 (would let thunk-bearing builds such as Flicky be admitted); neither is started here. SEG-051/052/053 are untouched and independent. No default or recommendation change: Compatibility remains default; gen2 remains an experimental opt-in (ADR 0109).

## D. Reviewer attack results (independent T006 review: PASS-WITH-FIXES, no blocker; fixes applied here)

Scope statements added on review: (i) **No source rebuilds were performed in SEG-050.** Source-backed truth was only REUSED from the pre-existing SEG-048/049 private artifacts (five lineages, digests re-verified by ROM hash); no public disassembly was cloned, rebuilt or downloaded, and no additional ROM was acquired. Of the screening candidates named by the operator only Golden Axe and Flicky are locally available and A-class; Golden Axe II, Shining Force II and the Streets of Rage disassembly data are missing, so those corpus branches are stopped (they would need the operator's authorized inputs). (ii) O1 lives in the shared prune, so it also changes **v1** Optimized outcomes for alias-bearing images (Golden Axe v1 now admits 24.9 % instead of falling back); v1 artifacts, weights and identities are untouched and the change is benign. (iii) Git timestamps of C3, C3b and C4 are minutes apart, so git alone cannot prove that the erratum preceded the first truth run; that attestation rests on the ADR text (only synthetic random bytes were run before the re-freeze) and on identical thresholds in C3 and C3b. (iv) Exact descriptor lengths and aggregate counts appear as coarse commercial-derived aggregates; no address, offset, byte, instruction word, descriptor address or truth list is committed.

Attacks tried and results:
1. Coincidental source overlap admitting a non-copy alias: impossible; the rule applies only to ADR 0049 copy-alias identities whose `alias_source_address` comes from the validated, byte-identity-guarded descriptor; an unrelated selected span does not admit the copy (tested).
2. Alias admitted with bytes different from its source: static proof and runtime byte-identity guard untouched; descriptor-modification / missing-descriptor behaviour covered by the existing generated tests.
3. Partial copy: rejected as `materialized_image_not_admitted` (tested); admission needs the complete source selected.
4. RAM thunks: no cartridge source, stay keyed by RAM address, fail closed (tested).
5. Validator weakening: only the new `in_region` predicate and rejection-class line changed; closure and mandatory-root rule intact; the unchanged validator accepts exactly K in every case.
6. Alias absolute edge escaping K: rejected as `pruned`, admitted once the target is selected (tested).
7. ROM-only regression: no aliases == plain region rule; plan text deterministic; `alias_set_mismatch` retained.
8. Double counting: none; T002 savings credited to no ML claim; no ML code exists.
9. Leakage / address dependence: the E1 image arm has no address, base, length or lineage input; boundary effect measured (edge cells only); lineages stated; known-corpus excluded from gates; SEG-053 corpus untouched.
10. False full-recall claim: none; recall is reported "no" with the density/budget attribution and the ban on reusing it as a T005/E2 rationale.
11. Selection vs structural-prune confusion: the unchanged-validator rejection of proper subsets was stated before results and exercised by test; K1 not relaxed.
12. Post-hoc threshold change: none; thresholds identical in C3 and C3b; classification by frozen precedence.
13. Extra infrastructure / unused code: offline research scripts only; no registry, runtime path, model change or second validator.
