# ADR 0210: SEG-098-T001 - authentic loader inventory and feasibility survey

- Status: Accepted. Pre-registration (written before the authentic run) and result are below. Related: ADR 0205 (reference-independent extent discovery), 0206 (static
  loader-operation scan), 0141 (SP entered through its module jump table), 0140 (real-title overlay inventory), 0142 (verified broad AOT).
- Privacy: this ADR contains classes and counts only. No address, LBA, offset, module name, hash, byte or disassembly of the authorized commercial title appears here or in
  the test; the survey tool computes overlaps in memory and prints aggregates (`tools/sega_cd_loader_survey.py`, checked by `tests/sega_cd_loader_survey_test.py`).

## Question (pre-registered)

How far does the M1 static loader scan reach on an authentic Sega CD disc, how does that compare with what an independent generated-native build route observes the
console loading, and which single missing capability would add the most source-authorized executable coverage? Result classes: `AUTHENTIC_SURVEY_COMPLETE` (all
metrics measured on authorized local inputs) or `AUTHENTIC_EVIDENCE_UNAVAILABLE` (synthetic structural analysis only; no invented title results).

Method fixed beforehand: (1) the unchanged scan on the disc; (2) the same scan with walk roots taken from the SP module jump table; (3) a clearly labelled survey-only
what-if that lets the walk continue after computed (`JSR (d8,An/PC,Xn)`) calls; (4) the existing `segarecomp sega-cd build` (Compatibility, no input script, formatted empty
backup RAM) as the independent delivery evidence; (5) relation of the observed sources to immutable disc structure (ISO9660 file starts, LBA constants in the boot images).
Observations are coverage evidence and falsification controls only; nothing here becomes admission input. No production behaviour changes in this task: the new scan options
are off by default and the build never passes them.

## Result (authorized local Sonic CD disc, Model 1 BIOS; aggregate)

| Measure | Value |
| --- | --- |
| Static, unchanged scan (descriptor entry) | 1 step, 0 CD operations, 0 modules, complete (nothing was walked) |
| Cause | the authentic SP descriptor's entry offset is 0 = the module header; the BIOS enters the SP through the header's module jump table (init, main, int2, user), which the scan did not read |
| Static, module-table roots | 4 roots, 38 steps, 0 CDD/CDC register-programming sites, 0 operations, 0 modules; 2 constant-function BIOS-service call sites; the main path ends at a computed PC-indexed call (an SP-internal service dispatcher) |
| Static, what-if: walk continues after computed calls | 36 states, 1,122 steps, still 0 CDD/CDC sites, 0 operations; 4 BIOS-service call sites (2 with a function number not constant) |
| Byte-pattern count in the SP image (not a decode) | 17 `MOVE.W #n,D0` + `JSR` to the BIOS service vector, 6 to the SP's own dispatcher |
| Observed build route | 17 rounds, 12 admitted extents, stop `pcm_unsupported` (the previous result, reproduced) |
| Observed by destination | 7 main Work RAM, 5 sub PRG RAM, 0 Word RAM; sizes: 1 under 2 KiB, 10 of 2-32 KiB, 1 of at least 32 KiB; about 158 KB; ~79,000 aligned instruction starts |
| Overlay alternatives needed | 4 destination groups with more than one alternative |
| Static sources / observed sources / overlap | 0 / 12 / 0 |
| ISO9660 | about 130 files, ~121 MB; 7 of 12 observed sources start exactly at a file's first block, 9 of 12 lie inside a file |
| LBA constants in the boot images | 0 of the 131 file start blocks occur as a 32-bit constant; 3 of 12 observed sources match a 16/32-bit pattern (an upper bound: 16-bit matches of small values are expected by chance) |
| Bounded-analysis exhaustion | none (states, steps, depth, operations, modules: no limit reached) |

Reading (a hypothesis consistent with the measurements, not a proof): on the authentic disc the scan found nothing for two stacked reasons. First, a defect of scope, not of principle: it never walked the SP's real entry points. Second,
even when it does, the SP does not program the CD hardware itself on the walked paths (a short prefix of the SP: 38 steps, 1,122 in the what-if; both stop at a computed dispatcher, so this says nothing about the rest of the SP): it requests CD service through the BIOS service vector with constant function numbers and
SP-internal table dispatch, and file selection is not by LBA constant (none of the file start blocks appears as a 32-bit big-endian constant in the two boot images; BCD/MSF encodings, constants in loaded code or data and computed addresses are not searched, so this is weak evidence), i.e. it goes through the ISO9660 directory
at run time. The loader arguments (which file, which destination) are produced by later, command- or table-driven code that a constant walk of the boot image cannot reach without
(a) recovering immutable PC-indexed dispatch tables, (b) modelling the documented BIOS CD/file service arguments, and very likely (c) the main program's requests (its IP is
also a static boot image). The first is bounded; the third is not established by this survey and is recorded as the open question for T003.

Synthetic structural analysis (hermetic): the project-authored `table_header` disc (an authentic-style SP header and jump table, descriptor entry 0) discovers nothing with the
descriptor entry and finds modules A, B and C (identical to the entry-at-start disc) with module-table roots. This is the M1 result carried across the authentic SP shape.

## Decision

`AUTHENTIC_SURVEY_COMPLETE`. Ranked missing capabilities (rank 1 is measured; ranks 2 and 3 are judgement, not quantified by the survey):

1. **SP module-jump-table roots (T002 prerequisite).** Without it nothing on an authentic disc is walked. Bounded, no new semantics (a header field and four word offsets, all
   validated inside the image, `MAIN` name check, fall back to the descriptor entry). Adopted by T002 for production.
2. **Walk-through computed calls and immutable PC-indexed dispatch tables.** Today a computed call ends the path (`computed_*`), dropping everything after a service dispatcher. The
   sound minimum is: a computed call is a foreign call that returns (registers and the CD shadow are havocked). Recovering the targets of an immutable word-offset table inside the
   image is the T003/T004 candidate; it needs an extent proof for the table (bounded run, every target inside the image and decodable) and is not claimed here.
3. **BIOS CD/file service wrappers (T003 first candidate).** The unit that actually carries load arguments on this disc. Its feasibility depends on whether the arguments
   (file identity, destination) are constants on some walked path; this survey could not establish that (the dispatch layer is in the way) and the main program's role is open.
   T003 pre-registers the rule and must report `UNRESOLVED` honestly if the arguments are command-supplied.

The seam for the production BIOS-image builder (T002) does not depend on how hard the authentic recognition is: a verified static source-to-mapping result is admitted as a
disc extent with the existing per-instruction byte-guarded, overlay-selected image machinery. T002 is therefore implemented on project-authored BIOS-image fixtures, and an
authentic benefit is not claimed until T003/T005 measure it.

## Tests and tools

- `segarecomp sega-cd discover-modules --module-table-roots [--computed-call-whatif]` (survey/opt-in; the report gains a `survey` object of counters).
- `tools/sega_cd_loader_survey.py` (aggregate-only; `--probe-source-constants` for the ISO9660/LBA-constant relation).
- `tests/sega_cd_loader_survey_test.py` (11 checks) and the `table_header` case of the `unvisited` fixture.
