# ADR 0192: SEG-085-T002 - independent historically representative evaluation cohort: qualification, exclusions and freeze (checkpoint C2)

- Status: Accepted (SEG-085-T002, checkpoint C2). Follows ADR 0190. Frozen before any policy was scored on a cohort image. Machine-readable record: `tools/segarecomp_seg085_cohort.manifest.json` (identities, digests, counts only).
- Classification of the cohort: **1 qualified independent historical lineage against the pre-registered minimum of 3 -> the milestone cannot reach a promotional outcome; `EVIDENCE_INSUFFICIENT` applies (ADR 0190 section 5).** Partial results are preserved; no SGDK example was added to fill the shortage.

## Procedure incident (disclosed)

The first candidate examined, ToeJam & Earl (REV 02), builds byte-identically from a public GNU-assembler reconstruction and passed every corroboration check. It is, however, one of the eight **SEG-077 reserved blind-cohort titles** (`B077-04`), and ADR 0190 excludes
reserved titles. The identity check against the reserved lists was run after, not before, its truth had been extracted and decoded. Consequences: (1) no policy was scored on it and no number from it appears anywhere; (2) every derived artifact (class map, registry
entry, token stream, work tree, coverage run) was deleted from the private cache; (3) a mechanical guard now refuses any image whose SHA-256 is a SEG-077 reserved title, a SEG-072 sealed image or a development image (`segarecomp_seg085_cohort.refuse_reserved`, enforced in
`gas-truth` and `corroborate`, tested), so the order of operations cannot recur; (4) the reserved title is neither qualified nor otherwise used. The reserved cohort's evaluative value is judged intact because no model, threshold or decision used the discarded data, but the
examination itself is recorded here.

## Qualified entry

| field | H01 Phantasy Star IV (English revision) |
|---|---|
| project / revision | `github.com/squidfeatures/ps4disasm` @ `8a658314850904c6888969f90177779845133a7e` (public disassembly, hand-written MC68000, Sega 1995) |
| era / toolchain evidence | commercial 1995 release; reconstruction assembled with the repository-bundled AS V1.42 Bld 89 (`asw.exe` SHA-256 `a782c3c7...`, byte-identical to the binary SEG-072 qualified for D04) under Wine 9.0; operator authorised the Wine execution |
| CPU / platform | MC68000, Sega Genesis (24-bit) |
| input | local authorized image, SHA-256 `511f35cc11f88316f8b8940e28ab298bd75a4da193672a80172884d6eb913b6a`, SHA-1 `bc7ff6d6a8408f38562bc610f24645cad6c42629`, 3,145,728 bytes |
| rebuild | `revision = 1`, default options: output byte-identical to the image (revisions 0 and 2 differ) |
| label method | `tools/segarecomp_source_map_extract.py` listing semantics, dialect `as`, unchanged: 102,494 instruction starts; non-instruction explained bytes D; Z80/other bytes U |
| tier | **T-B** (reconstructed code, byte-identical rebuild, independent boundary corroboration); not T-A (no original build) |
| corroboration (no model) | decoder agrees on all 102,494 labelled starts (0 odd, 0 non-ordinary, 0 length mismatches); 27,916 direct control-flow targets: 27,912 on a labelled start, 3 on labelled data, 1 inside an instruction (contradiction rate 1.4e-4) -> 5 cells (all bytes) and the 119 starts they touch are masked to U; final 102,375 starts, 648 U bytes |
| independence | 12-byte shingle containment of labelled code vs every development lineage: maximum 0.028 (D02; D04, the same project family, 0.0066 / 0.0014); threshold 0.30. Sealed lineages are never opened, so independence from them is not measured |
| known gaps | boundaries are the reconstruction authors' classification, corroborated by the checks above but not by an original build; code the authors left as data and nothing branches to is invisible; no execution record for the economics image (the startup-bridge broad build crashed clang on a single 3 MiB translation unit; the benchmark's sharded path is used instead) |
| licence / retention | public repository carries no ROM; derived class map retained only in the private harness (evaluation-only role), never in the product repository |

## Exclusions

Sealed, reserved and development identities (above guard); ToeJam & Earl, Ristar and Puyo Puyo (reserved SEG-077 titles); Sonic 3D Blast and the Streets of Rage trilogy (public projects are IDA databases or address-less IDA exports, not rebuildable: tier T-C at best, diagnostic only);
Beyond Oasis, Dr. Robotnik's Mean Bean Machine, Toy Story, Dragon Ball Z Buyuu Retsuden and NHL 96 (public sources exist but no authorized local image); the Sonic 3 / S&K family and the Phantasy Star / Shining Force / Flicky projects already in development. No period Atari ST or Amiga
source with a held image and a reproducible build beyond the SEG-075 development lineages was found within the bounded effort.

## Consequence

One lineage, one image. It is reported as a single independent historical data point, never as a cohort. The development lineages stay development: their nested leave-one-lineage-out results are supporting evidence, not independent evaluation.
