# ADR 0201: SEG-088 - independent TAS evidence acquisition (Gunstar Heroes, Castlevania: Bloodlines, Contra: Hard Corps): result `INSUFFICIENT_NOVEL_EVIDENCE`

- Status: Accepted (SEG-088-T001, checkpoints A-D). Follows ADR 0200 (`STRUCTURAL_EVIDENCE_INSUFFICIENT`); none of ADRs 0190-0200 is amended and no frozen
  artefact is modified. Checkpoint E (conditional model pilot) was **not run** because the pre-registered gate failed.
- Privacy: aggregate only. ROMs, movies, traces, screenshots, class maps and per-address data stay in ignored local storage and the private harness
  (`private-corpus/seg088-tas-v1`, class maps only, role "exploratory diagnostic-only"). Pre-registration, evidence record and tools are harness-private.
- Scope: three authorised local Genesis images (identified by published SHA-1), five TASVideos `.gmv` routes replayed in the `oranguthang/gens_automation` Gens fork
  (Wine + Xvfb), 100-frame CPU-trace windows. Nothing here is independent qualification, a model comparison or a deployment decision.

## What was done

| step | result |
|---|---|
| ROM identity | all three images (plus the US Contra image) equal the published SHA-1 of the movie they pair with; plain big-endian images, no copier header |
| movie download | six official TASVideos endpoints, HTTP 200; each returned a zip with one `.gmv` (type detected, not trusted); valid `Gens Movie TEST` headers; no savestate requirement |
| emulator | `gens_automation` (pinned commit) built with Docker (`win-i386`), run under Wine 9 + Xvfb; command-line paths must be Windows paths without spaces |
| synchronisation | full-length replays at fixed screenshot cadence: Gunstar (#945), both Castlevania (#1220/#1221) and both Contra-J (#3844/#3867) progress through every stage to the final stage; **Contra US #97 (Gens 8) desynchronises (game over at about the fourth stage) and is excluded** |
| determinism | 20/20 repeated windows identical (normalised trace, executed-PC set, DMA set); repeatability is not used as proof of synchronisation |
| trace windows | 400 evidence windows (two pre-registered stages; the second triggered by the discovery rule), 6-16 % of each movie traced; 100 % of witnessed ROM starts accepted by the pinned decoder and the emulator's fall-through distances; 0 length disagreements |
| defect fixed | the unpatched `bintrace` DMA hook also logged VRAM fills/copies: 39.6 % of first-pass unique DMA events and 203 of 520 Contra extents were spurious; a one-line fork patch plus a bintrace-only re-run of all windows removed them (0 extents overlap executed bytes in all images) |

## Evidence classes (union of routes per ROM; two routes of one ROM are one image)

| image (lineage group) | unique ROM starts | positive cells | strong negative cells | model-A low-tail (logit < 0) positives |
|---|---|---|---|---|
| Gunstar Heroes (Treasure) | 25,355 | 1,513 | 223 | 1 |
| Castlevania: Bloodlines (Konami) | 27,359 | 1,422 | 226 | 3 |
| Contra: Hard Corps J (Konami; one lineage group with Castlevania) | 31,358 | 1,638 | 253 | 3 |
| total | 84,072 | 4,573 | 702 | **7** |

None of the three ROMs (SHA-256) or lineages occurs in any private corpus manifest. Route redundancy (never counted as lineage diversity): positive-cell Jaccard 0.70
(Castlevania routes) and 0.43 (Contra routes). Structural diversity against the 50-image development registry is high (236-344 control-flow signatures per game absent
from the registry's 5,476; thousands of new 3/4-gram forms). Dynamic evidence labels only executed bytes, so every positive cell contains unknown bytes: 89-92 % are `AMBIGUOUS_UNKNOWN`,
the remainder `PURE_CODE`; no mixed or data-dominated cell can be asserted from strong-tier partial observation. A PC not observed is not a negative; no model prediction is a label.

## Does it target the low-scoring tail? No

Frozen model A (SEG-076 recipe fitted on the eleven development lineages; bitwise reproduction of the stored cross-platform scores verified; new images scored only)
gives a negative logit to 7 of 4,573 new positive cells (0.15 %) against 205 of 14,186 (1.45 %) in the development corpus; logit < 4: 0.8 % against 3.7 %; first
percentile +4.3 to +4.5 against -1.69. The new positives are ordinary, easily ranked code.

## Pre-registered gate (thresholds frozen before any model score existed; amendments A1 and A2 recorded before any gate number)

N1 two qualified images: 3 (pass). N2 >= 1,500 positive cells: 4,573 (pass). **N3 >= 50 low-tail positives over >= 2 images: 7 (fail).** N4 >= 200 strong negative cells: 702
(pass). **N5 >= 10 low-tail cells in the ambiguity categories: 0 (fail).** **N6 evaluation-only group with >= 25 low-tail cells: 6 (fail).**

## Decision: `INSUFFICIENT_NOVEL_EVIDENCE`

No retraining, no B/C-v2 pilot and no calibration used the new images; nothing was promoted. The negative result is the finding: three new, qualified, structurally diverse
game lineages add 4,573 verified executed cells and 702 independent negatives but only 7 cells of the kind (low model-A score) that SEG-085-SEG-087 identified as binding.
Even complete movie coverage at the observed rate would yield roughly twenty such cells, below what a model comparison can resolve.

## Limitations

Coverage is 6-16 % of each movie (unexecuted bytes are unknown, never negative); synchronisation evidence is progression plus decoder/emulator agreement, not a frame-exact reference;
the Gens fork needed the DMA-mode patch (E-tier evidence does not depend on it); the two Contra-J routes and two Castlevania routes are single images; Contra US #97 could not be replayed.

## Recommended next action

Do not pursue more ordinary executed-code evidence for the low tail. The tail is a property of rare structures (data-interleaved code, indirect targets) that neither TAS execution nor the
existing structural witnesses reach. If work continues, pick one of: (1) source-backed or reconstruction-backed images that contain mixed/interleaved regions (the only labels that can
assert data inside a code cell); (2) a bounded targeted-execution experiment aimed at the specific low-score structure classes rather than gameplay traces. Both need a new pre-registration.
