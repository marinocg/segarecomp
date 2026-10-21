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

### Evidence tiers (frozen)

| Label | Tier | Evidence | Never from |
|---|---|---|---|
| I/c | E executed | PC observed in an independent emulator trace (MAME 0.276, `:maincpu`), inside the image. Two independent decoders (m68k-disasm, CPU-owned token decoder) agree on the length. | odd PCs, decode disagreement, overlapping executed spans (all typed and masked) |
| I/c | R static reach | Direct Bcc/BRA/BSR/DBcc/JMP/JSR (abs.w, abs.l, d16(PC)) targets, and fall-through of non-terminating, non-call instructions, from accepted code. Calls and traps continue only when their return site was executed. The same two-decoder agreement is required, with no overlap. | m68k-disasm's own walk (it decodes inline data after calls; observed on the fixture) |
| D | HW_HEADER | 0x000-0x1FF vector table and cartridge header | - |
| D | DMA | Observed 68K-bus VDP DMA source extents, reconstructed from the control-port write stream with documented register semantics. Extents overlapping accepted code are masked whole. | fill/copy DMA, RAM sources |
| D | DR | Observed CPU data reads by an accepted executed reader (or RAM code) that never reads accepted code outside its own fetch window. Bytes in the 4-byte prefetch shadow after executed code are excluded, as are copy origins of RAM-executed code (widened to the copier's contiguous read extent). DR is disabled for the whole image when any non-trampoline RAM run has no ROM origin. | checksum or copy loops, prefetch |
| veto | Ghidra | An ADR-0025 code-entry candidate inside data unlabels the next 10 bytes | Ghidra never creates labels |
| U | - | everything else | - |

Excluded as label evidence:
- decoder agreement alone, which shows decode validity, not intent;
- non-execution in bounded scenarios;
- byte-exact reassembly, since `dc.b`/`incbin` reproduce anything;
- model scores and agent judgement;
- ROM bytes of RAM-executed code, because no copy relation is validated (they stay U).

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

Principal gates (soundness):

- **G1** 0 wrong I, c or D bytes on the committed synthetic cartridge against its GNU as listing truth.
- **G2** replica determinism.
- **G3** mapping: 0 mismatched words, and no executed cartridge-space PC beyond the image.
- **G4** executed-PC rejections at most 2% of the executed ROM PCs.
- **G5** independent re-labelling reproduces the classes digest.

Usefulness:

- **U1** positive cells at least 10% of cells.
- **U2** all-D negative cells at least 2%.
- **U3** at least 50 mixed (I and D) positive cells.

Cost:

- **C1** at most 10 CPU-hours per title.

Classes:

- `PROCEED`: G1-G5, U1-U3 and C1 all hold.
- `PARTIAL`: G1-G5 hold and positives are at least 5%, but a usefulness or cost criterion fails.
- `STOP`: any of G1-G4 fails after one bounded correction cycle, or positives are below 5%.

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

Synthetic proof (G1 precondition), fixture under the frozen tool:
- 0 wrong I, c or D bytes;
- 58 of 65 true instruction starts and 580 of 604 data bytes recalled;
- every abstention is a designed hazard: the RAM-copied routine, the dead routine, the unused jump-table case, the unraised exception
  handler, inline arguments in the prefetch shadow, and the copy source.

The checksum loop over executed and unexecuted code was rejected as a code reader.

## Consequences

- Labels are partial by construction. T005 must score partially labelled images only on labelled cells. Masked cells still occupy rank
  positions in the SEG-072 full-recall fraction, so they must be excluded or treated separately.
- The evidence is bounded by the scenarios. Code that is not reached stays unknown, so a title's positive coverage measures scenario
  reach, not ROM composition.
- A title that executes RAM code with no ROM origin loses DR evidence entirely. It falls back to HW and DMA data evidence.

## Pilot result

Recorded in the Evidence of SEG-077-T001 and checkpointed in ADR 0172 after the run (aggregate-only).
