# ADR 0133: SEG-063-T002 frozen M3 transfer to Sega CD modules — evidence unavailable; static traversal retained

- Status: Accepted. **Outcome: `EVIDENCE_UNAVAILABLE`.** The pre-registered benchmark was not run: no lawful source-backed Sega CD
  M68K corpus with independent instruction-start labels is available to the project. This is not evidence for or against
  transfer. No model, budget or admission rule changes; the frozen Genesis gen2 M3 (+B35) is not applied to Sega CD images.
- Related: ADR 0131 (ML reuse boundary and evidence gate), ADR 0103 (model lifecycle), ADR 0124 (evidence-unavailable precedent),
  ADR 0132. Follow-up run on a user-supplied disc: ADR 0140 (SEG-063-T003).

## Pre-registration (fixed before any result)

| item | registered value |
|---|---|
| question | does the frozen gen2 M3 at B35, applied only inside an already-established executable image (IP, SP or admitted module), keep acceptable recall and reduce compiled size/time against static traversal from declared roots? |
| model identity | the frozen gen2 M3 of ADR 0100/0109, B35 of ADR 0101; no retraining, no new network |
| corpus | source-backed Sega CD M68K modules whose instruction starts come from an assembler listing of public source the operator may lawfully build (candidates named in refinement: a public Sonic CD disassembly, homebrew SDK sources), with the built bytes matching the user's disc extent digest |
| labels | independent source-backed instruction starts only; never the model's or the recompiler's own output; never committed when reconstructable |
| holdouts and leakage | per-title holdout; no title or shared engine that contributed to M3 training; engine sharing between modules of one title recorded |
| metrics | recall of labelled instruction starts at B35; false admissions that change behaviour (must be 0); compiled C size and emit/compile time versus static traversal |
| closed outcomes | `TRANSFER_ADEQUATE`, `TRANSFER_INADEQUATE_RETRAIN_JUSTIFIED`, `EVIDENCE_UNAVAILABLE` |
| retraining gate | only if the gate fails **and** a compile-size/time problem was measured; then a design is specified (scratch vs fine-tune, frozen M3 baseline), never run here |

## Evidence

| input | status |
|---|---|
| source-backed Sega CD corpus and labels | **absent**: no disc image, no BIOS, no licensed source build is present in this environment; none may be downloaded or reconstructed |
| compile-cost signal (the precondition for the benchmark to matter) | measured on the synthetic module disc: admitting two 192-byte modules at two bases each (4 candidates, 24 instructions) adds 30 KB of generated C (168 KB vs 138 KB) and about 3.5 ms of emission (17 ms vs 14 ms median of 5); compile time of the translation unit is unchanged within noise (about 0.55 s at -O1). No size or time problem is observed |
| how module code is found | static traversal from list-declared roots inside the admitted image (ADR 0132), not broad AOT: the regime in which M3 would prune candidates does not arise for admitted modules today |

## Decision

`EVIDENCE_UNAVAILABLE`. Static traversal from declared roots remains the only Sega CD discovery; deterministic admission and
live-byte verification remain the correctness authority (ADR 0131 point 1: the model never decides which images exist or run).
No retraining design is specified, because neither precondition holds (no failed gate, no measured cost problem).

## Prerequisites for a valid run

A user-supplied disc and a lawful source build whose bytes reproduce a module extent digest; labels extracted from its listing by
an independent tool; the registered holdouts; then the comparison above on that module image, run where the operator authorises
it, with only normalized, non-reconstructable results recorded.
