# ADR 0185: SEG-081-T001 - three-game TAS acquisition and cross-engine validation (decision: CROSS_ENGINE_ACQUISITION_SUPPORTED; model A: ML_INCONCLUSIVE)

- Status: Accepted (SEG-081-T001, checkpoint C4). Result of the protocol pre-registered in ADR 0184 (plan digest `a51546ad...2845`, committed before the first commercial
  trace; no post-freeze amendment was needed). ADRs 0181-0184 are unchanged.
- Privacy: aggregate only (counts, rates, digests, hours). Results: `tools/segarecomp_genesis_tasbatch.{vm2,bnr,ewj2,model_a,batch}.results.json`.

## Decision

**`CROSS_ENGINE_ACQUISITION_SUPPORTED`**: all three games completed verified replay, passed the strong-code usefulness criterion and showed structural novelty beyond the
SEG-072 registry and their own controls, with no evidence conflict and 0 open review blockers. **Model A: `ML_INCONCLUSIVE`** (separate; it does not change the batch decision).
Recommended next action: a staged acquisition campaign of a small number of further, engine-diverse TAS titles (preferably not engine-adjacent to the development corpus),
with the strong-only policy, the same frozen gates, and a corpus-isolation check per title. This ADR does not authorise the campaign.

## Per game (TAS vs seed-1 control, equal frames, same ROM/core/sync/tracing/labelling)

| | Vectorman 2 | Batman & Robin | Earthworm Jim 2 |
|---|---|---|---|
| frames (full movie) | 28,632 | 130,772 | 71,699 |
| class | QUALIFIED_USEFUL | QUALIFIED_USEFUL | QUALIFIED_USEFUL |
| executed ROM PCs TAS / control | 37,764 / 10,592 | 26,589 / 7,403 | 19,619 / 6,854 |
| strong code-positive 128-B cells TAS / control | 1,647 / 486 | 1,040 / 344 | 829 / 327 |
| extra strong cells vs control (rule: >=100, target 500) | 1,178 (+242 %) | 696 (+202 %) | 506 (+155 %) |
| extra vs seed-2 control | 1,172 | 697 | 513 |
| extra indirect targets / edges | 2,110 / 2,255 | 2,187 / 5,960 | 240 / 295 |
| novel structural signatures not in registry nor control | 95 | 145 | 129 |
| CPU seconds per extra cell (TAS run) | 0.36 | 2.66 | 2.02 |
| ledger CPU-hours (cap 8) | 0.51 | 2.24 | 1.21 |

Total 3.96 of 24 CPU-hours. Gates: identity, full-movie playback FINISHED at N, six distinct (RAM, VRAM) checkpoint states, new code in all five movie-time fifths, playback and
traced digests identical (no perturbation), two traced TAS runs identical, ROM mapping 0 mismatches, accepted-E fraction >= 0.9999, native CDL consistent, E code inside DMA sources 0.
Earthworm Jim 2 (authored on BizHawk 2.6.3) replayed on 2.9.1 with no compatibility investigation. Model A: pooled AP B0 0.99786, C 0.99778, T 0.99781, TW 0.99765, matched-volume
0.99772; max |delta| 5e-5 << 0.005 so the frozen rule returns `ML_INCONCLUSIVE` (a saturated baseline: no evidence of gain or harm); cross-game and held-out containment <= 0.008, no game withheld.

## Independent review

Plan review and delta review before the freeze (6 blockers fixed). Final review reproduced, with independent code: the plan/bound digests and commit order, the strong-cell counts
(exact, incl. set-difference), ledger hours and run digests, identities, strong/weak tier provenance (strong = E+H+M only), an independent structural-novelty count (same sign and
ordering, about 2.5x larger with finer signatures), isolation and model-A leakage checks, and the privacy scan; the synthetic CDL fixture re-ran exactly. 0 open blockers.

## Limits and disclosures (read before relying on this)

1. The equal-duration random control reaches only 26-36 % of the TAS's PCs, mostly title/menu code, so "extra cells" are measured against a low-coverage control; relative gains are
   large partly for that reason. The thresholds are feasibility thresholds, not a claim about label perfection or ML generalisation.
2. No source truth exists for these titles. Correctness is internal consistency (decode audit, mapping, two-decoder agreement, provenance), not a measured error rate.
3. The native CDL data flag is blanket: E bytes also read as data are 275 / 62 / 28; they stay code and never gate.
4. Executing code from RAM (9 PCs in Vectorman 2 and Batman & Robin) makes the SEG-077 label rule drop all admitted data reads, so the weak tier W is empty for Vectorman 2 and for the
   Batman & Robin TAS; weak-variant data comparisons and the TW arm are not like-for-like for those two. The strong-only usefulness rule is unaffected.
5. Vectorman 2 and Earthworm Jim 2 are sequels engine-adjacent to listed SEG-077 development candidates (not identical, not in any manifest); Batman & Robin has no such adjacency.
   "Cross-engine" is therefore weaker for those two.
6. Structural novelty is mostly control-flow target patterns; novel instruction forms are rare. Counts depend on the signature definition.
7. Two-player movies (Vectorman 2, Earthworm Jim 2) versus a control that moves player 1 only. The control is a synthetic `.bk2` using the TAS movie's own sync settings (SEG-080 used
   BizHawk defaults). There is no original-emulator end-state oracle: playback FINISHED plus progression cannot rule out a late desynchronisation (new code appears in every fifth).
8. Vectorman 2 and Batman & Robin are a single movie each, one control policy, one window; no claim is made about other engines.
