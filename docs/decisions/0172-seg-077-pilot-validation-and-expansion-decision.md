# ADR 0172: SEG-077 independent pilot validation and expansion decision

- Status: accepted (binding class of the SEG-077 expansion gate)
- Task: SEG-077-T002, checkpoint C2 of the combined SEG-077 PR
- Record: `tools/segarecomp_genesis_evidence.t002.validation.json` (aggregate-only); protocol ADR 0171, plan `03ebc8f8...`

## Context

SEG-077-T001 piloted the frozen Genesis evidence protocol (ADR 0171) on Chuck Rock (USA). Its self-assessed class was `STOP`:

| | positives | basis |
|---|---|---|
| revision 4 | 3.25% | below the 5% floor |
| amendment A1 (the single amendment permitted) | 3.11% | below the 5% floor |

Soundness gates G1-G4 held in both. T002's job is an independent reproduction, by a session that is not T001's implementer, and the
binding expansion decision.

## Validation (independent; aggregate-only)

- **Identity.** Image size and SHA-256 equal the plan's pilot block. The plan digest, decoder CLI and container image IDs match the
  freeze.
- **Independent trace replays (attract and explore-s1).** The PC, DMA and read digests and the derived files are byte-identical to the
  retained replica A. There are 0 mismatches.
- **G5.** Re-labelling from the retained evidence gives identical classes, tokens, seeds, spans and relocations. G1 was rerun
  independently and is identical, with 0 wrong I/c/D bytes in all three synthetic scenarios.
- **Truth-rule spot checks.**
  - Every I start is an executed or static-reach start.
  - Every start's span equals the decoder length.
  - No D byte lies inside executed bytes or in a prefetch shadow.
  - RAM-executed PCs: 0, so DR is active.
  - Rejected spans stay U.
  - For a 1-in-50 sample of executed starts, MAME's executed-instruction text agrees with the CPU-owned decoder's family (90/90),
    control class (22/22) and size (58/58).
- **Metrics.**
  - Starts and bytes:
    - E 4,476 and R 2,060 starts, with 20,414 continuation bytes.
    - D 50,506 bytes: HW 512, DMA 18,080, DR 31,914.
    - U is 92.6% of the image.
  - Cells (of 8,192): 255 positive, 276 negative, 24 mixed, 270 hard-negative.
  - Tool disagreement:
    - E rejection rate 0.089%.
    - 0 of 1,216 Ghidra candidates fall in labelled data.
    - 18.3% of m68k-disasm walk instructions were not admitted, and all of them stayed U.
    - DMA reconstruction agrees 100% with the read tap.
  - Cost: 9.06 CPU-hours, or 28 positive and 31 negative cells per CPU-hour.
- **New structural patterns.**
  - 0 of 255 positive cells contain an instruction-start (family, control) trigram that is absent from the SEG-072 development images.
    Holding out each SEG-072 image in turn gives 0.6-4.1%.
  - Byte 4-gram novelty in negative cells (96.4%) is within the hold-one-out range (66.7-99.4%), so it is not distinctive.
- **Correction.** The T001 statement that the three A1 explore seeds reached an *identical* PC set was false. The counts were equal
  (4,345 each), but the sets differ slightly (intersection 4,316, union 4,379). ADR 0171 and the T001 results were corrected. The class
  is unaffected.

## Decision

- **Binding class: `STOP`.** G1-G5 all hold, so this is not `VALIDATION_FAILED`, but positive cells are 3.11%, below the frozen 5% floor.
  For the record: U1 fails, U2 holds, U3 fails, C1 holds.
- **Expansion.** The multi-game development acquisition (SEG-077-T003 and T004) is cancelled with this evidence.
- **Continuation.** SEG-077-T005 to T008 proceed on the corpus that actually qualified. The Chuck Rock labels are sound and reproducible,
  but small, and they add no new instruction structure.
- **Consequence for the final ADR.** Given this corpus, the final decision is expected to be data-limited. It is not pre-judged here.

## Consequences

- The research question (were SEG-076's results limited by data?) can no longer be answered by a 16-lineage expansion under this
  protocol.
- Scenario-bounded evidence at this yield (about 260 positive cells and 9 CPU-hours per title) does not scale to the intended corpus. A
  materially different authorized scenario source would be a new protocol: for example, operator-recorded human input movies, or
  operator-supplied save states.
