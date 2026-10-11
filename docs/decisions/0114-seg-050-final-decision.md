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

## D. Reviewer attack results

(recorded below by the independent T006 review)
