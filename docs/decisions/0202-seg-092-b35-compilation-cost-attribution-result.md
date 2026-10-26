# ADR 0202: SEG-092 - compilation-cost attribution of A+B35 versus M3+B35 and incumbent-preserving optimisation: `INCUMBENT_PREFERRED`

- Status: Accepted (research, SEG-092 T001-T004: checkpoints C1 `PLAUSIBLE_COST_CAUSES_IDENTIFIED`, C2 `GENERIC_CAUSE_IDENTIFIED` (limited), C3 `NO_MATERIAL_IMPROVEMENT`, final decision `INCUMBENT_PREFERRED`). Follows ADR 0157 (cost baselines), the model comparison ADRs 0165/0169/0170 and SEG-091's incumbent-preserving comparison.
  No ADR is amended. **No product change, no default change, no selection/prune/closure/validator/runtime change, no new budget, no retraining.** M3+B35 (`--immutable-rom-aot-ml-gen2-admission`) is unchanged and remains the incumbent.
- Privacy: aggregate only (ratios, counts, byte totals). ROM bytes, addresses, generated C, per-address and per-translation-unit data stay in the private harness; the pre-registration, tooling and per-image results are harness-private.
- Scope: development images only. Nothing here qualifies model A for deployment (it has no native implementation) and no result is independent evidence.

## Question

SEG-091 measured model A at the same B35 budget about 9 % cheaper in total cold-build CPU than the deployed M3+B35 route over eleven development images, without an explanation and under a non-randomised order with concurrent workers. Why?

## Method (pre-registered before measurement)

Product `0a530ba`, clang 18.1.3 `-O2 -std=c11 -Wall -Wextra -Werror -pedantic`, 4 compile jobs, no object cache, cold; SEG-091's exact selections and score snapshots; unchanged prune/closure/validator.
Nine images (six lineages): four expected large gains (Sonic 1, Sonic 2, Sonic & Knuckles, Shining Force II), one weak (Shining Force), one borderline (Phantasy Star II), three controls (Flicky, two SGDK).
Arms INC, A35_final, A35_oof (compiled) and an emit-only M3-through-plan arm whose generated C was byte-identical to INC's on every image and repetition. Three cold repetitions (Shining Force: two), arm order randomised per image/repetition, one serial worker on an idle-checked host,
per-translation-unit CPU from `wait4`. 13,430 CPU s spent of a 24,000 ceiling; no abort, failure or re-run. Generated C was digest-identical across repetitions for every arm.

## Results

1. **The advantage reproduces under randomised order on an idle serial host.** Pooled over the nine images: INC 1,757.9 CPU s, A35_final 1,608.4 (-8.5 %), A35_oof 1,640.0 (-6.7 %). A35_final/INC per image: Sonic 1 0.896, Sonic 2 0.870, Sonic & Knuckles 0.904,
   Shining Force II 0.918, Shining Force 0.920, Phantasy Star II 0.934, SGDK d09 0.978, SGDK d05 0.952 (inside its 5.6 % noise), **Flicky 1.068 (significantly worse)**. Every ratio is within 0.07 of SEG-091's. Lineage-mean 0.957 / worst 1.068 (SEG-091: 0.961 / 1.072),
   i.e. the frozen SEG-091 adoption gate would fail again for A35_final; **A35_oof does not reproduce SEG-091's held-out gate result** (SEG-091 0.940/1.042: met; here 0.969/1.095, Flicky 1.095: fails; oof ratios differ by more than 0.07 on four images). Image and lineage sets differ from SEG-091's (9/7 vs 11/8). The -8.5 % pooled figure is CPU-weighted (large builds dominate); the lineage mean is the more representative number. INC repetition spread is 0.6-2.3 % (d05 5.6 %). Absolute INC CPU is 9-20 % below SEG-091's (it ran concurrent workers).
2. **Equal admitted-identity counts do not mean equal cost.** The admitted identity totals differ by 0.1-2.0 % between arms (d05 +2.0 %, otherwise <= 0.94 %), but entry segments fall 0.5-10 % and generated C 1-9 % on the gaining images
   (Sonic 2: 44,246 -> 39,647 entry segments, 120.5 -> 109.2 MB; Shining Force II: 87,633 -> 80,207, 254.8 -> 234.5 MB). The reverse happens on Flicky (7,147 -> 7,609 segments, +6.7 % bytes).
3. **Decomposition of the claimed A35_final delta (-150.2 CPU s over the seven images with a claimed gain; -149.5 s over all nine):** generated-C volume of the `aot`, `shared` and `block` translation-unit families 74 % (56 % `aot`, 11 % `shared`, 7 % `block`);
   per-byte compile intensity 16 % (`aot` 0.5-3.7 % cheaper per byte on 8 of 9 images, Flicky 0.07 %; `block` on one image with two repetitions, weak); inference + plan + emit stage 9 % (native M3 inference inside INC's emit is 0.2-4.9 s per image; not like-for-like with research-Python A inference);
   link and all other families about 0 %. A35_oof gives the same ranking. Across the nine images log(CPU ratio) correlates 0.976 with log(generated-bytes ratio) (slope 1.16), in both signs.
4. **The difference is concentrated in the exchanged segment set (exploratory).** Between 5 % and 43 % of the admitted identities are exclusive to one arm; entry segments with equal head labels have identical byte length (0 B difference on eight images, 1.3 kB of 124 MB on one). A segment head is not an identity, and segmentation/closure differences are counted as exclusive, so this does not prove that emission of common identities is unchanged.
   INC's exclusive segments amortise fewer identities per entry segment (Sonic 2: 2.67 vs 3.70; Sonic 1 2.23 vs 2.74; Sonic & Knuckles 3.19 vs 4.71; Shining Force II 3.45 vs 4.17) and cost more generated bytes per identity (Sonic 2: 562 vs 397 B). On Flicky it is reversed (3.78 vs 2.62).
5. **Fragmentation is neither necessary nor sufficient.** Contiguous admitted cell runs fall 17-53 % on five gaining images but are unchanged on Shining Force I (-0.2 %) and nearly so on II (-2.5 %), which still gain 8 %; SGDK d09 halves its runs for a 2 % gain; Flicky's runs rise 9 % with its cost.
6. **No evidence for (post-hoc):** link (0.1-0.3 s), translation-unit count (equal on 6 of 9 images; +/-2 on the three controls, Flicky higher with its worse result), size concentration, precise-discovery block function counts (equal on every image), a difference at equal segment head labels.

## Checkpoint C1 decision (T001)

Frozen rule: benefit reproduced in sign on all four large images, and one component (generated-C volume of the exchanged identities, `aot` family) explains >= 25 % of the claimed delta in both A35_final and A35_oof -> **`PLAUSIBLE_COST_CAUSES_IDENTIFIED`**. The frozen clause "absent or small in negative controls" was not mechanically evaluated: Flicky shows the volume terms with opposite sign (+1.0 s aot, +0.8 s shared on 23.3 s), d05/d09 are small or inside noise; reading the reversal as consistent with a volume mechanism is a post-hoc interpretation. Volume is tautologically tied to compile CPU, so C1 identifies the carrier, not the cause of the lower volume. Ranked candidates for T002:
(H1) generated-C volume per admitted identity of the exchanged cells (about 74 %); (H2) per-byte compile intensity (about 16 %, weak); (H3) native M3 inference inside INC's emit versus cheaper A inference (about 9 %, tied to the model implementation); (H4) fragmentation (weak). 
The property of the exchanged cells that lowers their segment/byte volume (for example branch-target or block-leader density) was **not identified** by C1; T002 below.

## Checkpoint C2 (T002): `GENERIC_CAUSE_IDENTIFIED` (limited meaning)

- Per-cell generated-C cost (entry-segment bytes headed in the cell, from the broad emission) is context-free (99.99 % equal bytes at matched head labels; the table covers 83-91 % of heads) and reproduces the byte difference of the exchanged cells (pooled 0.92; >= 0.70 on 5 of 6 gaining images; opposite sign on Flicky).
- Causal intervention on Sonic 1, Sonic 2 and Flicky (random half-swaps of the exchanged cells at fixed B35 budget and seeds, three cold repetitions, randomised order): 55-64 % of A's CPU saving, and a same-direction penalty on Flicky.
- No usable cheap model-free structural proxy (H-C ambiguous: pooled leave-one-image-out R^2 0.31 passes, per-image fails; exchange-level reproduction poor): leave-one-image-out R^2 0.20-0.39 on gaining images (per-image mean 0.12, true pooled 0.31 against a 0.30 threshold whose "pooled" wording is ambiguous; the proxy reproduces only 0.25-0.58 of the table exchange on four images and <= 0 on two). An exploitable source exists only as an emitted-cost oracle (extra broad emission, +4.6 % pooled CPU on the large images, 6.3 % on one; the frozen "<= 5 % on the large images" clause was applied pooled - per image it would have failed).
- A's exclusive cells are deep in M3's ranking (median 64th percentile; 14-23 % within M3's top 45 %), so a rank-band reselection reaches only a small part of the effect. The rank-neutral null and the oracle upper bounds are weak/post-hoc.

## Checkpoint C3 (T003): `NO_MATERIAL_IMPROVEMENT`

Two pre-registered, opt-in, model-free candidates (cost-aware reselection inside +-5 % or +-10 % of cells around the B35 cutoff, exact budget and seeds, cost from the broad-emission table). CB5: no missed verified start, validator accepts everywhere, generated C -1.8 to -3.3 % on the large images, but screened net CPU lineage mean 0.978 (> 0.96; worst 1.016) after charging the oracle table, so no compile stage. CB10: 297 verified starts outside the admitted set on Sonic 2 and validator rejection of two SGDK images -> rejected without tuning.

## Final decision (T004): `INCUMBENT_PREFERRED`

M3+B35 is unchanged and remains the best supported approach. No model-free optimisation met the gates; A+B35's advantage reproduces on development data but its held-out arm fails SEG-091's frozen gate (0.969 / worst 1.095, Flicky +9.5 %), Flicky is significantly worse, and no eligible independent lineage exists, so `MODEL_A_NATIVE_FEASIBILITY_JUSTIFIED` is not met and the native-A assessment was not triggered. `INDEPENDENT_EVIDENCE_REQUIRED` is recorded as a secondary finding. No product task is proposed.

## Limits and non-claims

All analyses of the mechanism (hypothesis ranking, composition, band bounds) are post-hoc/exploratory except where marked pre-registered; T003's economic decision rests on a screen, not a compile. 

The hypothesis ranking and the composition analysis are post-hoc/exploratory. Pre-registered per-region bytes, contiguous-range statistics, duplicate bytes and rho are not reported. Idle host was checked by a 1-s busy-fraction (<10 %) rather than the pre-registered loadavg. Development images only (INC in-sample on five titles); Shining Force I with two repetitions; model-A inference is research Python and has no native implementation, so a deployable A would add inference cost not measured here; identity-level analysis uses entry-segment bytes as the generated-C proxy;
correlation across nine images with one reversed point; verified-start retention and runtime equivalence were not re-measured (SEG-091's stand; nothing here changes selection). No completeness claim, no adoption proposal, no change to M3+B35.
