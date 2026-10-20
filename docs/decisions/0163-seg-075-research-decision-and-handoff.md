# ADR 0163: SEG-075 research decision, economics and verified handoff

- Status: Accepted. **Outcome: `DATA_INSUFFICIENT`** (pre-registered rule 1 of ADR 0161, applied by `segarecomp_xplat_decide.outcome`). No
  production model, default, admission or runtime behavior changes.
- Task: SEG-075-T006, the final task of the combined SEG-075 product PR (checkpoints C6/C7).
- Basis:
  - ADR 0159 (T001 sources);
  - ADR 0160 (T002 corpus and handoff);
  - ADR 0161 (T003 pre-registration, plan `fde026ee`, T005 addendum `e1854647…`);
  - ADR 0162 (T004 `EVIDENCE_INSUFFICIENT`).

## 1. Decision

- T004's pre-registered noise rule fired (tau = 0.103 > 0.05). Outcome rule 1 maps this to **`DATA_INSUFFICIENT`**; no other outcome
  class is reachable.
- **Candidate:** the T006 freeze needs the ordering's best configuration, and selection is empty when T004 is insufficient, so nothing was
  frozen.
- **One-shot evaluation: not run.** The sealed reserved cohort (neogeosdk, space-invaders; 18 samples) was never opened.
  `ONE_SHOT_STATUS.json`, private and bound in the handoff, records `opened: false` together with the sealed binding digest. The cohort
  therefore remains fresh, independent evidence for a future study under a new pre-registration.
- **Economics:** no candidate means no comparison through SEG-074's exact-code benchmark.
  - The pre-registered economic measure, the complete budget, is B50 for every configuration in every condition, including the C0 M3-R
    comparator (ADR 0162). No configuration offered a smaller complete budget even descriptively, so there is no economic signal to
    pursue with the current data.
  - ADR 0157's finding stands unchanged: region-level ranking has headroom on commercial images.
- **T005 (models A/B/C):** not admitted by its pre-registered gate. The implementations are reviewed, bound and preserved, but not
  trained.

## 2. Answers to the milestone's questions (within the data's limits)

1. **Does cross-platform training help Genesis-style recognition?** Unresolved.
   - The measured C2 effects are smaller than the variation between two training seeds of the same model: M3-R Genesis mean ER 0.146 to
     0.122, against a 0.036 seed difference.
   - The data shows neither consistent help nor consistent harm. C2 worst-lineage ER was lower for every model, and C2 added no B50
     misses.
2. **Do cross-platform programs need cross-platform training?** Mostly no, for the qualified programs.
   - Genesis-only models, including the zero-shot production M3, already rank these Amiga and Atari programs almost perfectly
     (AUROC ≥ 0.9996; ER 0 on the assembler-written Amiga programs).
   - Compiler-generated firmware (EmuTOS) is the only cross-platform lineage with headroom.
3. **Does a lightweight structural model, a local Transformer or a graph-guided Transformer add value?** Not tested. The pre-registered
   gates did not admit them on this evidence.

## 3. What a successor needs to change (ordered by expected value)

1. **Training-seed noise first.** Pre-register a multi-seed M3-R protocol (several seeds per fit, averaged scores, the noise floor taken
   from several seed pairs) before testing any data or architecture change on SEG-072's Genesis development set. D01 (the Sonic family)
   moves broadly with the seed.
2. **Harder, more diverse source-backed data rather than more of the same.**
   - Examples: C-compiled 68000 games (Amiga SAS/C or vbcc, Atari Pure C/Lattice), and programs with jump tables and inline data in code.
   - Also new open-source Genesis homebrew with source-backed truth.
   - The qualified assembler programs are already easy.
   - The full "missing kinds of programs" list is in ADR 0161 section 3.
3. **Then re-run T004 unchanged, and T005 as gated.** The corpus (ADR 0160), the frozen design (ADR 0161), the T004 runner, the T005
   models and every fitted model are preserved, so a successor only adds data or seeds and re-freezes.

## 4. Verified private handoff (C7)

`tools/segarecomp_xplat_final.py verify` is the successor's single check. Run it in a fresh process from any export of the product at the
PR head; no task worktree is needed. It:

1. **Physically verifies** the final store manifest: every content-addressed object is rehashed, and the manifest is bound to the
   committed receipt.
2. **Reproduces a diagnostic.** It restores the needed objects into an empty temporary directory and re-scores a cross-platform
   development representative from those copies only, using the preserved C2-all M3-R weights and the unchanged SEG-072 scorer. The result
   must equal the stored scores bit for bit; there is no rebuild and no retraining.
3. **Re-derives the decision** from the committed results with the bound rules.
4. **Confirms the one-shot status record.**

It prints `ARTIFACT_HANDOFF_PASS` or `ARTIFACT_HANDOFF_INCOMPLETE`. The result of the final run, with the task worktree made unavailable,
is recorded in the receipt `tools/segarecomp_xplat_handoff.c7_receipt.json` and in section 5.

Contents of the final store (`<HARNESS_ROOT>/.cache/seg075-handoff`, manifest `c7-final-handoff`):

- T001: pinned source checkouts, both reproducibility runs of every recipe, records, toolchain inputs, Dockerfiles, helpers, and the
  archived toolchain images.
- T002: truth runs (listings, kept assembly, relist proofs), the canonical corpus, the decoder binary, the C2 review oracle scripts, and
  the Sonic CD module restore script.
- T003: the frozen snapshot, with development samples and the sealed binding.
- T004: every fit record, fold score, all-development model weights, `reproduce.json` and the farm logs.
- T006: the environment report (research venv package set, toolchain image ids, product and harness SHAs, plan and addendum digests)
  and the one-shot status.

External inputs that are not copied, by policy: the operator's commercial Genesis images and Sonic CD disc under `games/`. The SEG-072
bundle verifies them by digest.

## 5. Final verification result

**`ARTIFACT_HANDOFF_PASS`** (`tools/segarecomp_xplat_handoff.c7_receipt.json`).

- **Manifest:** `c7-final-handoff` `1e8ba1e62f045eb762c2993f09f57849469ff000b8b379d6ebbafe5d449292cd`.
- **Physical verification:** 32,154 files and 4,640,136,029 bytes rehashed, 0 problems.
- **Run context:** from a `git archive` export of the product at the C6 head, `python3 -I`, cwd `/`, minimal environment, with the task
  worktree moved away (unavailable) for the duration.
- **Diagnostic:** a 131-cell cross-platform representative, re-scored from 9 restored copies, is bit-identical to the stored scores.
- **Decision:** re-derived as `EVIDENCE_INSUFFICIENT` -> `DATA_INSUFFICIENT`.
- **One-shot status:** present, `opened: false`.

Artifacts:

- **Missing because never produced:** T005 A/B/C weights and predictions (not admitted); one-shot predictions (not run).
- **External, not copied:** the operator's commercial images and Sonic CD disc under `games/`, verified by digest by the SEG-072 bundle.
- **Rebuildable, not bundled:** the research virtualenv.
- **Unrecoverable dependencies:** none.
