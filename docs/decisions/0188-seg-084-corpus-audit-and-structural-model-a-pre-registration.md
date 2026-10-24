# ADR 0188: SEG-084 - M68K corpus audit, structural model-A reassessment protocol and label-preservation audit (checkpoint C1)

- Status: Accepted (SEG-084-T001, checkpoint C1). Follows ADRs 0181-0187; none is amended. SEG-083's frozen plan `228647dd...`, its decisions (EXPANSION_MIXED, ML_INCONCLUSIVE, MORE_DIVERSITY_NEEDED) and every historical metric are unchanged.
- Privacy: aggregate only. The audit opened no sealed, reserved or blind path (a path guard refuses them); the sealed cohorts are accounted for by entry counts in public manifests only.

## What the audit found (details: `docs/testing/m68k-ml-training-data-flow.md`, `tools/segarecomp_seg084_audit.inventory.json`)

1. **Every trained model uses one corpus**: the SEG-072 Genesis development registry (verified present and digest-clean: 50 images = 14 Genesis ROM images + 36 Sonic CD modules, 11 qualified
   lineages, 15,024 positive / 83,204 negative / 3,243 masked cells). Sonic CD modules are lineage D01, not independent engines. SEG-075's 36 cross-platform images (5 lineages) were transfer-experiment
   data (C2), never SEG-076 pilot training data (C0, Genesis only). **No persisted model consumed a SEG-081/083 label.**
2. **SEG-083's "model A" diagnostic was the SEG-072 M1-HGB** (`m1_features`, 64-byte block statistics; no seed, relocation or control-flow feature; `HGB_CONFIG`: 200 iterations, depth 4), **not** the SEG-076
   structural model A (`a_features`: 210 structural features incl. token chains, PC-relative targets, relocations and PROVEN reach; `A_CONFIG`: 300 iterations, depth 6). The two baselines (ER ~0.177 against ~0.105) are not comparable. ADR 0187's
   model-A statements are about M1 and stand as such.
3. **A representation defect for any TAS-trained A / C**: SEG-072 seeds come from the production CLI's label-free direct-flow report (`d.txt`, ~360-530 addresses per accepted ROM); the retained TAS samples carry only the reset vector. Feeding them unchanged
   would shift the seed / PROVEN-reach feature distribution of the training images. SEG-084 regenerates registry-style seeds for each TAS ROM (14 s per ROM, pinned CLI).
4. **A model-A refit reproduces the stored SEG-076 scores bit for bit** (verified on every fold at table time; the reproduction is a gate, not an assumption).

## Recoverability matrix (what is lost if every `.cache` and agent worktree directory vanished)

| Material | State today | Class |
|---|---|---|
| `seg081-v1`, `seg083-v1` TAS class / tier maps and provenance | private Git (harness PRs #100, #101) | restorable from the private harness; usable samples additionally need the authorized ROM and the pinned decoder (verified by restore and compare) |
| SEG-072 development class maps, seeds, token and image digests (50 images) | `.cache/seg072/data` and one local 308 MB handoff tar (also `.cache`) | **local-only and at risk** until T002; after T002: class maps and metadata in private Git; images from authorized local ROMs (7 commercial images), SGDK builds (7, reproducible from pinned public inputs) or the disc (36 Sonic CD modules); tokens from the pinned decoder |
| SEG-075 cross-platform development class maps, relocations, seeds (36 images) | `.cache/seg075` and a 3.6 GB local handoff (content-addressed store + toolchain image archive) | **local-only and at risk** until T002; images rebuild only if the toolchains / sources in the handoff survive (several toolchain images are floating-version: `environment_dependent`) |
| SEG-077 Chuck Rock T001 sample | `.cache/m68k-corpus` | local-only, exploratory (STOP); the static-first relabels are superseded (96 % wrong-IP defect) and are NOT eligible |
| Sealed / reserved material (SEG-072 7 entries, SEG-075 18, SEG-077 8 reserved titles) | opaque bindings in public manifests; contents under custodian-controlled `sealed` directories and in the handoffs | sealed and custodian-controlled; **never** placed in a training-visible snapshot; a separate encrypted or access-controlled backup is recommended (below), not built here |
| ROM / executable images, handoff CAS, toolchain images, traces, disassembly, token streams | `games/`, `.cache` | deliberately excluded from Git retention |

## Frozen assessment protocol

Plan `tools/segarecomp_seg084_a.plan.json`, digest `83b79e80d27f9986ecf4541e7ebba03f240718f26f9e11f465896b4ebc922481`, committed before any non-rehearsal fit. It binds the study tool, the audit and diagnostic tools, their tests, the unchanged SEG-075/076
modules and the SEG-083 results it reads. Summary (the plan is authoritative):

- **Model**: true SEG-076 model A (`a_features`, `A_CONFIG`), strict leave-one-lineage-out over the 11 qualified lineages (SEG-076's C0 folds). **Arms**: A0 (registry only), A81, A83, A_all (Sonic 2 excluded: same image as D01's `s2`), A_control,
  A_matched (TAS down-sampled to the control's cell counts), plus the explicitly versioned `seg084.a_weighted.v1` arms A_all_W / A_control_W (strong+weak, weights 1.0 / 0.25; not the original model A), A_term and A_noterm (The Terminator in and out), and three A0 refits on 90 % of the base cells as the noise reference.
- **Metrics**: the SEG-076 evaluation code unchanged (Genesis mean and worst ER, lineage-max ER, B35/B50 retention, misses, boundary strata); hard-negative and negative errors at the A0 operating point (recall 0.90); AP / AUC secondary; per-arm CPU. A0 must reproduce the stored SEG-076 scores or the result is EVIDENCE_INSUFFICIENT.
- **Noise rule**: a per-fold ER change counts only if it exceeds that fold's noise (largest |ER(sub) - ER(A0)| over three subsamples); the mean change must exceed the mean noise. Development folds are reused, lineage-max ER is outlier-driven: nothing here is confirmatory.
- **Readiness** (independent of SEG-083's verdict): `EVIDENCE_INSUFFICIENT` -> `MORE_DIVERSITY_NEEDED` -> `MODEL_OR_EVALUATION_LIMITATION` -> `NEURAL_PILOT_JUSTIFIED`, with **code diversity** (>= 5 independent useful lineages among the eight candidates and new strong positive cells >= 20 % of the base)
  and **negative-supervision diversity** (>= 3 independent qualified lineages with >= 300 strong hard-negative cells, whatever their code-yield class) judged separately; response (A_all improves ER beyond noise on >= 2 folds) and residual headroom (>= 2 folds still at ER >= 0.05).
- **Terminator negatives**: legitimacy against the tier definitions (strong D = header and DMA-source tiers only), non-triviality (ordinary-decode fraction of the hard cells against the registry's hard-negative share) and value (A_term vs A0, A_noterm vs A_all).
- **Shove It! and NHL '94** (existing artifacts, no replay): the progression-gate failure is decomposed into replay fidelity and continued discovery; the 204 rejected NHL '94 starts are classified into DISASSEMBLER_LIMITATION / INSTRUCTION_FORM_MISMATCH / ROM_MAPPING_ISSUE / UNSOUND_EXECUTION_LABELS / UNRESOLVED by fixed rules (CPU decoder ordinary and the emulator-observed successor equals pc + length for >= 95 %,
  and the independent disassembler printed raw data or nothing) with one narrow re-run of the pinned m68k-disasm image on the same executed PCs. No threshold is lowered, no label admitted on an unresolved class. A four-way separation of replay fidelity / continued discovery / label confidence / acquisition yield is proposed prospectively and applied only as a table.
- **Budget**: 8 CPU-hours; no hyperparameter search, no B / C / C-v2 training, no sealed data, no retracing, no new games.

## Pre-freeze rehearsal

Mechanics (preparation, seed regeneration, arm assembly, the `table` command, the A0 reproduction on the stored SEG-076 scores) were rehearsed on three development folds (D05, D09, D12) under draft plans, discarded, with no rule changed afterwards because of an outcome (the two refreezes fixed a table sub-fold option and one wrong test expectation).
The independent protocol review is recorded below before the first real fit.

@REVIEW@
