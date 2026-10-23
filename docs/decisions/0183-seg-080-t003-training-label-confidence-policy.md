# ADR 0183: SEG-080-T003 - versioned training-label confidence policy and a minimal model-A diagnostic (verdict: INCONCLUSIVE)

- Status: Accepted (SEG-080-T003). Preserves ADR 0181 (`REPLAY_INVALID`) and ADR 0182 (`LABELS_UNSAFE`) and their frozen decisions; this is a new
  training-label layer, not a revised Gate B. Authoritative generated-native code admission is unchanged and fail-closed; nothing here touches it.
- Privacy: aggregate only. Existing private Sonic 2 TAS / control traces and label directories were reused; nothing was re-traced.
- Disclosure: Sonic 2 is already in SEG-072; the Sonic lineage was excluded from the base training pool, so Sonic 2 enters only through the policy
  labels. The diagnostic is not blind generalisation evidence.

## Policy `seg080.label_policy.v1` (frozen before evaluation; plan digest `3503f0f2...363b`, one pre-evaluation amendment)

| tier | meaning | role |
|---|---|---|
| E | executed instruction (I/c) | strong code |
| R | direct static reach from E | weak code |
| H, M | hardware header; DMA-source extent | strong data (independently supported) |
| W | ordinary observed data read | weak, **nonexclusive** data: never vetoes a code label or code cell |
| U | unknown / masked | permitted; never a negative |

Variants: `strong` = E + H + M; `weak` = strong + R + W. Cell labels use the unchanged SEG-072 rule; cell sample weight 1.0 when the label rests on
strong tiers only, 0.25 otherwise. Source truth is read only by the evaluators. The plan was reviewed independently before any evaluation; the review
forced a crash fix, minimum-support and sensitivity conditions on the recommendation rules (PASS/FAIL/INCONCLUSIVE), a `combine` step, a code-side versus
data-side error split in the diagnostic, and token/ROM identity assertions.

## Measurements (TAS = full-movie trace; control = seed-1; seed-2 control as sensitivity)

- **Strong tiers are error-free against the truth** in all three traces: E 121,652 bytes (TAS), H 512, M 122,080, with 0 wrong, 0 code-as-data and
  0 data-as-code. R (18,276 bytes): 0 wrong. **All errors sit in the weak data tier W**: 3 code-as-data bytes of 532,697 (TAS), 1 of 186,049 (control),
  0 of 74,007 (seed-2 control); no weak code error.
- **Coverage lost when weak labels are excluded (TAS):** labelled bytes -69.3 %, positive cells -4.1 %, negative cells -80.1 % (control: -59 %, -4.4 %,
  -67 %). The loss is almost entirely data supervision from ordinary reads.
- **Recovered cells, TAS minus control (cells with a wrong label excluded):**

| variant | extra correct code-positive | extra mixed | extra negative | extra hard-negative | cells with a wrong label (TAS / control) |
|---|---|---|---|---|---|
| strong | 758 | 0 | 399 | 392 | 0 / 0 |
| weak | 761 | 201 | 2,895 | 2,366 | 2 / 1 |

  The strong variant keeps 99.6 % of the extra code cells with no wrong cell, but yields no mixed cells (mixed cells need weak data inside a code cell) and
  few negatives. Seed-2 control: strong 906 pos / 688 neg, weak 903 / 233 mixed / 3,955.
- **Training-label noise:** no negative cell of either variant contains truth code (the 3 wrong bytes sit in positive cells).

## Model-A diagnostic (M1 HistGradientBoosting; leave-one-lineage-out over the 9 qualified non-Sonic lineages)

| arm | pooled AP | pooled AUC | FN rate (positive cells) | FP rate (negative) | FP rate (hard negative) |
|---|---|---|---|---|---|
| B0 base only | 0.98783 | 0.99843 | 0.1036 | 0.0012 | 0.0015 |
| S_tas strong | 0.99058 | 0.99882 | 0.0903 | 0.0016 | 0.0019 |
| W_tas strong+weak | 0.99041 | 0.99878 | 0.1023 | 0.0016 | 0.0019 |
| S_ctl strong | 0.99067 | 0.99874 | 0.1028 | 0.0016 | 0.0019 |
| W_ctl strong+weak | 0.99049 | 0.99879 | 0.1072 | 0.0016 | 0.0018 |
| T Sonic 2 truth (reference) | 0.99221 | 0.99899 | 0.1571 | 0.0015 | 0.0017 |

(Thresholds per fold at B0 recall 0.90; pooled AP mixes classifiers and is descriptive.) Every Sonic 2 arm lifts pooled AP by about 0.003, the control
arms as much as the TAS arms, mostly in two folds; strong and strong+weak are indistinguishable, and the complete-truth reference lifts it by only 0.0044.
The base pool already saturates the metric (AP about 0.99), so this diagnostic cannot rank label policies. The in-image check on Sonic 2 cells left
unlabelled by each arm is non-blind and not interpretable as generalisation (AP 0.99 in all arms).

## Recommendation rule (frozen) and verdict

- A (clean strong tier, all three traces, >= 1000 E bytes): **PASS**. B (strong keeps >= 50 % of the weak variant's extra code cells; weak count >= 50):
  **PASS**. C (no harm, requires reference-arm sensitivity AP(T) - AP(B0) >= 0.005): **INCONCLUSIVE** (0.0044).
- Frozen combine verdict: **INCONCLUSIVE**.

## Recommendation (judgement beyond the frozen verdict)

The label pipeline is useful as a **supervision source**: its strong tier is error-free here, it keeps nearly all recovered code cells, and the
weak read-derived data tier carries every observed error and can be down-weighted or dropped. A single TAS pilot from a different game engine is
justified **on those grounds**, provided it uses the strong-only policy as the primary label set and judges success on label yield and tier cleanliness
(rules A and B) against its own equal-duration control. It is **not** justified by a model-A gain: this diagnostic could not detect one, so no claim about
improving models A/B/C follows. Mixed cells and hard negatives require the weak tier and will remain weakly supervised. No 16-game acquisition and no
retraining follow from this ADR.

## Evidence index

`tools/segarecomp_genesis_label_policy.t003.results.json`, the plan and digest beside it, `tests/segarecomp_genesis_label_policy_test.py`.
