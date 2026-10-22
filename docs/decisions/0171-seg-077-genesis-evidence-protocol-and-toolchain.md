# ADR 0171: SEG-077 Genesis commercial evidence protocol, toolchain selection and Chuck Rock pilot pre-registration

- Status: accepted (pre-registration; frozen before the first full Chuck Rock analysis)
- Task: SEG-077-T001, checkpoint C1 of the combined SEG-077 PR
- Plan: `tools/segarecomp_genesis_evidence.t001.plan.json` + `.digest` (binds the labeller, image recipe, synthetic fixture and the
  reused SEG-072/075 and Ghidra code by SHA-256)
- Tool: `tools/segarecomp_genesis_evidence.py`; image recipe `tools/segarecomp_genesis_evidence.Dockerfile`; fixture
  `tests/fixtures/genesis_evidence/synthetic_cart.s`; tests `tests/segarecomp_genesis_evidence_test.py`

## Context

SEG-077 asks whether the SEG-076 results (A preferred, descriptively; ADR 0170) were limited by the diversity, quantity or label quality
of training data. It adds authorized commercial Genesis titles as new development lineages, and retrains A, B and C on them.

Commercial titles have no source listing. Their labels must therefore come from evidence that is independent of the production
decoder's opinion, and every unresolved byte has to stay unknown. The existing sample layout (`segarecomp_ml_v3_train.Image`:
image, `classes.bin` I/c/D/U, decode tokens, seeds) and the unchanged cell rule (`segarecomp_ml_v3.cell_labels`: any I = positive,
all D = negative, otherwise masked) already support partial truth, so only a narrow adapter is needed. No new corpus representation is
introduced.

Privacy and retention follow ADR 0151, by explicit operator decision on 2026-10-21:
- images, labels, traces, scores and derived samples stay in the git-ignored `<HARNESS_ROOT>/.cache/m68k-corpus/`;
- only non-reconstructable aggregates, identities and digests are committed;
- no weights trained on private inputs are published.

## Decision

### Evidence tiers (frozen, revision 2)

Revision 1 (commit 96cec35) was blocked by the independent C1 review **before any full analysis**, on two defects:

1. **Rejected executed PCs could become data.** A refill read of a rejected executed target was credited to the branching instruction,
   and that instruction was not treated as a code reader.
2. **RAM-copy origins could be wrong.** They were matched from the end-of-run RAM, which can be cleared or reused, so a real copy source
   could become data.

Revision 2 fixed both and applied the review's notes. Its round-2 review was also blocked before any full analysis, on two holes in R:

- an executed instruction masked for overlap could be re-admitted as R through a direct branch;
- the one-round conflict withdrawal could admit a new R over the very observed data that caused the conflict.

Revision 3 makes R a single fixpoint:
- executed PCs that are not accepted are never R starts and their bytes are never covered by R;
- an R candidate that touches observed data is rejected at search time, its successors are never explored, and its span is blocked from
  both code and data.

Revision 4 (round-3 review, still before any full analysis) replaces an over-strict internal assertion (the rebuild may legitimately
admit an R that the first pass ordered out) with the real invariant: no code over observed data or executed bytes. Observed bytes inside
a conflicting candidate span stay data; only the rest of the span becomes U. Revisions 1-3 produced no commercial labels.

| Label | Tier | Evidence | Never from |
|---|---|---|---|
| I/c | E executed | PC observed in an independent emulator trace (MAME 0.276, `:maincpu`), inside the image. Two independent decoders (m68k-disasm, CPU-owned token decoder) agree on the length. | odd PCs, decode disagreement, overlapping executed spans (typed; masked from code and data) |
| I/c | R static reach | Direct Bcc/BRA/BSR/DBcc/JMP/JSR (abs.w, abs.l, d16(PC)) targets, and fall-through of non-terminating, non-call instructions, from accepted code. Calls and traps continue only when their return site was executed. The same two-decoder agreement is required, with no overlap. Executed PCs that are not accepted are never R starts and are never covered by R. An R candidate that touches observed data (HW, DMA or admitted DR bytes) is rejected at search time, and its successors are not explored. The non-observed bytes of its span become U. | m68k-disasm's own walk (it decodes inline data after calls; observed on the fixture) |
| D | HW_HEADER | 0x000-0x1FF vector table and cartridge header | - |
| D | DMA | Observed 68K-bus VDP DMA source extents, reconstructed from the control-port write stream with documented register semantics. Registers are unknown at power-on and after a state load (A1) until rewritten. | fill/copy DMA, RAM sources |
| D | DR | Observed data reads (program-space read tap, current-PC attribution) by an accepted executed reader or by RAM code. The reader must never read code bytes outside its own fetch window, where code bytes means accepted code or any executed PC's bytes. Bytes in the 4-byte prefetch shadow after executed code are excluded. Reads made over the 68000 bus by the Z80 or VDP count as 68000 data, so Z80 code is D for this corpus. DR is **disabled for the image** when any scenario executes code from work RAM, because no copy relation is inferred. | checksum and copy loops, prefetch and refill reads, RAM-copied code |
| block | executed bytes | No data byte may overlap an executed PC's bytes: the accepted span, or [pc, pc+10) for a rejected PC. An HW or DMA extent that touches code or executed bytes is masked whole, and a DR byte there is dropped. | - |
| veto | Ghidra | An ADR-0025 code-entry candidate inside data unlabels the next 10 bytes | Ghidra never creates labels |
| U | - | everything else | - |

Excluded as label evidence:
- decoder agreement alone, which shows decode validity, not intent;
- non-execution in bounded scenarios;
- byte-exact reassembly, since `dc.b`/`incbin` reproduce anything;
- model scores and agent judgement;
- any RAM-to-ROM copy inference.

**Disclosure.** The DR tier and the 480 s play length were chosen after a bounded 60 s Chuck Rock preflight. Only aggregate counts were
used: little DMA-sourced data, and the read-tap throughput. No rule was tuned on labels.

**Residual risks.**
- 68000 code that a non-code reader reads, and that never executes in the bounded scenarios or reaches RAM execution, can still become DR
  data. One example is a routine copied to RAM in a scenario where its RAM copy never runs.
- A title whose cartridge declares SRAM and is larger than 2 MiB is rejected (`SRAM_OVERLAPS_IMAGE`), because SRAM paging during play
  would not be visible to the stop-time mapping check.

### Scenarios, determinism and mapping

- **Scenarios.** Generic templates, never title-specific:
  - `attract`: no input, 180 s;
  - `play-s1`, `play-s2`, `play-s3`: a seeded deterministic input policy, 480 s each. Start/A pulses for the first 40 s, then 8-frame
    random actions (right 50%, left/up/down, A/B/C 30% each), with a double Start every 900 frames (pause toggles cancel).
- **Replays.** Every scenario runs twice in the offline container (`--network none`, 1 CPU, 2 GiB). Its PC set, DMA events and read
  set must be digest-identical across the two replays.
- **Trace capture.** The trace is streamed through a FIFO into a first-occurrence filter, and is never stored. The raw VDP and read logs
  are reduced to events and ranges, then deleted.
- **ROM mapping check.** At machine stop, the MAME program-space view of the cartridge is compared word by word with the image.

### Gates and classification (frozen; T002 owns the binding class)

Principal gates (soundness), computed by the tool:

- **G1** The `g1` command assembles the committed fixture, traces two scenarios, labels each and scores it against the GNU as listing
  truth. It requires 0 wrong I, c or D bytes in both scenarios: attract, where DR is active, and play, where holding pad C executes a RAM
  routine that is then cleared and DR must disable itself. It also requires that both rules were actually exercised.
- **G2** Replica determinism, computed and reported; a failing scenario is never silently dropped.
- **G3** Mapping: 0 mismatched words, and no executed cartridge-space PC beyond the image.
- **G4** The sum of executed-PC rejections divided by the distinct executed cartridge-space PCs is at most 0.02. Rejected bytes cost
  recall only, never soundness, because they are blocked from data.
- **G5** Independent re-labelling reproduces the classes digest.

Usefulness:

- **U1** positive cells at least 10% of cells.
- **U2** all-D negative cells at least 2%.
- **U3** at least 50 mixed (I and D) positive cells.

Cost:

- **C1** at most 10 CPU-hours per title. This is the sum of single-CPU container wall time (traces, Ghidra `-max-cpu 1`) plus label time.

Classes:

- `PROCEED`: G1-G5, U1-U3 and C1 all hold.
- `PARTIAL`: G1-G5 hold and positives are at least 5%, but a usefulness or cost criterion fails.
- `STOP`: any of G1-G5 fails, or positives are below 5%.

A rule change after the freeze requires a recorded amendment (an ADR), a re-freeze and a G1 rerun; the replaced plan's results are
reported. The plan binds the code, the fixture, the image IDs of the evidence, Ghidra and assembler images, and the decoder CLI digest.
A resumed trace is reused only when its extraction version, plan digest and image ID match.

Stop classes: `INPUT_REQUIRED`, `RETENTION_DECISION_REQUIRED`, `TOOLCHAIN_INFEASIBLE`, `BUDGET_EXHAUSTED`.

### Tool-choice gate (actual tests, 2026-10-21)

| Capability | Selected | Fallback | Proven by | Reason |
|---|---|---|---|---|
| Static candidate discovery | Existing Ghidra 12.1.2 image (`segarecomp/ghidra-mcp-headless:6.0.0`), unchanged ADR-0025 recipe/exporter, run offline in its image | none needed | The fixture yields 0 candidates (the recipe is seedless: no prologues, BinaryLoader without vectors). The fixture is a known limitation; Ghidra runs only as veto and disagreement on real titles. | Existing, pinned and isolated. Candidate evidence only. The wrapper's 0600 staging is unreadable by the container user on a root host, so the labeller runs the same image and arguments directly with a 0644 private copy. |
| Independent M68K decode | Oxore/m68k-disasm `bb6d839` (Unlicense; C++, CMake) | Capstone (not needed) | Traced and walk modes on the fixture. It found two defects, both handled: traced mode prints some traced locations as raw `.short` (the walk decode at the same PC is used), and walk decodes inline data after a JSR (R uses our own call rule) | 68000-only, base 0, at most 4 MiB, matching Genesis. Independent of the CPU-owned decoder |
| Dynamic execution tracing | MAME 0.276 (Debian `0.276+dfsg.1-1+deb13u1`, GPL-2.0+, external tool), driver `genesis`, CPU `:maincpu`, Lua autoboot plus debugger `trace` | BizHawk/BlastEm (not evaluated: MAME passed) | Fixture: deterministic double replay, ROM/RAM PC provenance (RAM routine at 0xFF0000 seen), DMA reconstruction equal to the 64 tile bytes, read tap confirms the DMA bytes. Chuck Rock bounded preflight (60 s): 3,596 frames, 0 mapping mismatches, scripted play reaches 2.7x more code than attract, the read tap leaves the PC and DMA digests unchanged and sees 100% of the reconstructed DMA bytes | `-debugger none` ignores `-debugscript`; Lua `manager.machine.debugger:command` arms the trace. ~0.24x realtime when tracing, ~0.13x with the read tap (hence 480 s play scenarios: ~7 CPU-hours per title) |
| Genesis segment recovery | none (sega2asm not selected) | sega2asm | n/a | No demonstrated gap. Compression decoders give no exclusive-data proof by themselves; DMA/DR already provide observed data |
| Reassembly | GNU as 2.42 (`segarecomp-xplat/tc-gnu:1`), fixture only | vasm/AS | Fixture builds, listing truth via the unchanged SEG-075 labeller with relocation masking | Commercial reassembly is not evidence and is not performed |
| Label export | narrow adapter to the SEG-072/075 sample layout | - | `Image`-compatible files; `segarecomp_ml_v3_data.audit` runs on every sample | No new representation |
| Corpus verification | existing manifest/handoff pattern (later checkpoints) | - | per-sample digests in `meta.json` | - |
| A/B/C training | SEG-076 framework (T005/T006) | narrow compatible update | reuse plan only in T001 | - |

The G1 result under the revision-2 freeze is recorded in the Evidence of SEG-077-T001.

## Consequences

- Labels are partial by construction. T005 must score partially labelled images only on labelled cells. Masked cells still occupy rank
  positions in the SEG-072 full-recall fraction, so they must be excluded or treated separately.
- The evidence is bounded by the scenarios. Code that is not reached stays unknown, so a title's positive coverage measures scenario
  reach, not ROM composition.
- A title that executes RAM code with no ROM origin loses DR evidence entirely. It falls back to HW and DMA data evidence.

## Amendment A1 (after the revision-4 pilot: explore scenarios and the DMA tightening that state loads require)

**Revision-4 pilot result, preserved and reported.**
- G1-G4 held: the G4 rejection rate was 0.0007, and all four scenarios were digest-identical across replicas.
- Positive cells were 266 of 8,192 (3.25%), with 344 negative and 24 mixed. The run took 8.81 CPU-hours.
- The self-assessed class was therefore `STOP` (positives below 5%).
- Marginal new ROM PCs per scenario, in plan order: attract 1,537, play-s1 3,839, play-s2 4, play-s3 220. The seeded random policy
  saturates early, so the bottleneck is scenario reach, not label soundness.

**Change.** The three random-play scenarios become `explore` scenarios with the same input policy, seeds, start window and frame
budget:
- Every 300 frames after the start window, the emulator state is archived if new code was reached (new read-tap reader PCs or new
  per-frame sampled PCs) or if the archive is empty.
- Otherwise, the next segment resumes from a seeded choice among the 8 most recent archived states.
- The scenario ends on its own frame budget, because machine time rewinds on a state load.

This is a generic coverage technique with no title knowledge.

State loads force one soundness tightening of the DMA rule. MAME restores the VDP registers on a load, but the control-port
reconstruction cannot see that. Registers are therefore treated as unknown at power-on and after every load, and a DMA is recorded only
when register 1 and registers 0x13-0x17 have been written since. In addition, a reconstructed DMA extent is kept only where the same run's read
tap saw those bytes read. This is a weak, drop-only filter, because the tap is the union of all readers. Both measures can only drop DMA
evidence, never add it.

The window between a load request and the actual load is closed in practice by MAME's load timing, not by the tap. MAME performs a
scheduled load right after the timeslice whose timers fired the frame notifier, so normally no 68000 instruction runs in between.

**Residual window (confirmation review, rated near zero).** If MAME defers the load by up to about one timeslice, both of these must also
happen inside that slice: the old timeline writes all six DMA registers, and the loaded state then issues a DMA command before the next
frame without rewriting them. Only then could a stale source be recorded. Hardening (dropping every DMA command between the two markers)
is deferred to any future plan revision.

G1 gains a 20 s explore scenario with real state loads, and the fixture repeats its DMA every VBlank. Because the fixture writes register 1
only at reset, G1 exercises the *dropping* of post-load DMA, not its re-recognition. Gates, thresholds and budgets are unchanged. The independent A1 review found the stale-register defect; the incomplete pilot traces started under the first A1 freeze were stopped and excluded from all results (they remain only in private storage).

**Bound.** This is a single amendment: if the pilot is still `STOP` under A1, T001 ends `STOP`. The explore mechanics, including
replica determinism, were checked on the synthetic fixture only; no Chuck Rock run of A1 preceded the freeze. G1 passed again under A1.

## Pilot result (aggregate-only; `tools/segarecomp_genesis_evidence.t001.results.json`)

| | revision 4 (random play) | amendment A1 (explore) |
|---|---|---|
| G1 / G2 / G3 / G4 | pass / pass / pass / pass (rate 0.0007) | pass / pass / pass / pass (rate 0.0009) |
| executed ROM PCs | 5,600 | 4,480 |
| E / R instruction starts | 5,596 / 1,394 | 4,476 / 2,060 |
| I / c / D / U bytes | 6,990 / 21,894 / 60,264 / 959,428 | 6,536 / 20,414 / 50,506 / 971,120 |
| DR bytes admitted | 34,248 | 31,914 |
| positive / negative / mixed / masked cells (of 8,192) | 266 / 344 / 24 / 7,582 | 255 / 276 / 24 / 7,661 |
| positive fraction (U1 needs 10%, STOP below 5%) | 3.25% | 3.11% |
| CPU-hours (C1 cap 10) | 8.81 | 9.06 |
| marginal new ROM PCs per scenario | 1,537 / 3,839 / 4 / 220 | 1,537 / 2,909 / 11 / 23 |
| self-assessed class | STOP | **STOP** (final for T001: single amendment) |

Further results:
- Re-labelling from the retained evidence reproduces the classes and tokens digests exactly.
- Ghidra produced 1,216 code-entry candidates; 664 of them fall on accepted code starts. No Ghidra veto of data fired, because no
  candidate fell in labelled data.

**Diagnosis.** The limit is scenario reach, not label soundness.
- Under A1, the three explore seeds reached an identical ROM PC set.
- Under revision 4, the second and third random seeds added only 4 and 220 PCs.
- The generic seeded input policies therefore do not drive the title beyond its early states. Novelty archiving cannot create reach
  that the input policy lacks.

The labels that were produced are sound under every gate, but too few for the pre-registered usefulness floor.

**Implication for SEG-077.** At about 9 CPU-hours per title and about 260 positive cells per title, scenario-bounded evidence is far below
the yield the expansion needs. A materially different, authorized scenario source would be a new protocol, not a further amendment of
this one. Examples are operator-recorded human input movies, or title-agnostic save states supplied by the operator. SEG-077-T002 owns the
binding class and the expansion decision.
