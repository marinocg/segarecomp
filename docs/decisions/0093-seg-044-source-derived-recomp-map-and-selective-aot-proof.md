# ADR 0093: SEG-044 Source-derived recomp map and first real selective-AOT proof

- Status: In progress (T001 section frozen; later sections are appended by T002..T006).
- Related, unchanged: ADR 0080 (hybrid selective admission), ADR 0089/0090/0091 (external facts and the real-title
  harvest), ADR 0092 (SEG-043 final result: interrupt-resumption premise is the sole remaining automatic blocker).

SEG-044 does **not** continue the automatic whole-program analysis line (SEG-038..043). It tests the *unchanged*
selective-AOT planner against an external completeness authority: an exact source project that rebuilds the pinned
ROM.

## SEG-044-T001 -- qualification of `s1disasm` as a source authority (path A: CONTINUE)

### Source identity and exact-rebuild gate

| | |
|---|---|
| Repository | `https://github.com/sonicretro/s1disasm` (public) |
| Exact revision | `7ebe4b3d0c182b2566026b6d3f423d33539558da` (a branch name is never the identity) |
| Assembler | the repository-bundled `build_tools/Linux-aarch64/asl` (Flamewing's modified Macroassembler AS, reports `AS V1.42 Beta [Bld 212]`) and bundled `p2bin` |
| Build driver | the repository's `build.lua`/`build_tools/lua/common.lua` steps, executed by a faithful Python port (no Lua interpreter exists in the image and a Lua download was refused by the operator's permission policy). The port is itself validated: unmodified source (`Revision = 1`) rebuilds a ROM whose MD5 equals the repository's own `chkbitperfect.lua` REV01 constant |
| Configuration | the repository's own documented revision switch `Revision = 0` in `sonic.asm` (the pinned image is the REV00 ROM; MD5 `1bc674be...` equals the `chkbitperfect.lua` REV00 constant). No source content was patched, no binary-diff correction layer exists |
| Build steps | WAV -> PCM/DPCM conversion, `asl -xx -n -q -A -L -U -E -i . sonic.asm`, `p2bin -p=FF -z=0,kosinski,Size_of_DAC_driver_guess,after`, header (end-of-ROM, checksum) fix |
| Result | SHA-256 `46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6` == the pinned ROM, **PASS twice**, each from a fresh clone with no generated output |

### What the assembler listing (`-L`) can mechanically distinguish

The listing (`sonic.lst`, never committed) is assembler output, not source parsing. Each emitting line carries
`source-line / address : emitted-bytes source-text`, macro invocations appear as `(MACRO)` followed by the *expanded*
lines (`listing purecode`), and continuation lines carry bytes beyond the first row. CPU mode switches
(`CPU Z80` / `CPU 68000`) and the `save`/`restore` stack that brackets them are visible in the source column.
Measured on the exact rebuild (aggregate only):

* 24,180 instruction-start lines assembled in 68000 mode, every mnemonic in the closed 68000 set (macro-expanded
  lines included, e.g. the `stopZ80`/`_move.b` family); **0** emitting lines with an unrecognized construct.
* Data: `dc.b/dc.w/dc.l/dw` rows (about 57 KB) and `binclude`d binary assets (about 377 KB, including the
  38-byte Z80 startup block and the 5,984-byte compressed Z80 DAC driver region that are Z80 machine code, not M68K).
* Accounting: every ROM byte is covered by an instruction row, a data row, a `binclude`/padding directive or an
  explicitly Z80-mode region. Uncovered ranges are explained one for one; the build is therefore exhaustive over the
  image, with no silent holes.
* Honest listing limit: for ~100 macro-expanded instructions the listing displays an operand byte that differs from
  the final ROM (`1+field` forms evaluated in an earlier pass). Address and length are right; byte equality with the
  ROM is therefore **not** claimed, and T002 must check the instruction *length* against segarecomp's decoder instead.

### Frozen trust contract (path A)

> **Source-derived facts are an external semantic-completeness authority for exactly the source constructs qualified
> here. They are not independently proven by segarecomp.**

Qualified construct class (everything else is *no authority / Unknown*, never a guessed classification):

* `C` = the set of addresses of listing rows that (a) were assembled while the effective CPU is `68000` (tracked
  through `CPU` and the `save`/`restore` stack; an unbalanced stack, an unknown CPU or a nested file that never
  restores it is a hard failure) and (b) whose mnemonic, after stripping label and the macro-force prefix `!`, is in
  the closed MC68000 mnemonic set. The claim: **every ROM-resident M68K PC that ever executes is an instruction start
  in `C`**.
* Rows in other classes (data directives, `binclude`, Z80-mode rows, `org`/padding) assert nothing about execution.
  Labels, comments and symbol names are never evidence.
* Any 68000-mode row that emits bytes with an unrecognized first token, an unparseable address, an address going
  backwards without a recognized `org`, an uncovered ROM range not explained above, or a listing whose source revision
  or ROM hash differs from the supplied ones, makes the whole extraction fail closed.

What segarecomp still verifies independently: ROM hash binding, evenness, mapping, decodability of every member,
bounds and duplicates. What it cannot verify: that no feasible M68K PC lies outside `C`. Known ways the claim can be
false, kept visible: execution of bytes classed as data (e.g. a deliberately mis-vectored interrupt), a branch into the
middle of an instruction, M68K code copied to RAM, and an unqualified assembler pass difference. The complete
execution-PC oracle (T005) and the mutation tests (T002, T006) are falsifiers; zero observed escapes does not prove
completeness and the final ADR repeats this.

### Why this is a global-universe (A), not per-site (B), authority

The mechanically derivable object is the whole instruction-start universe, not individual dispatch constructs: a
jump-table-only extractor would need per-macro semantic knowledge the listing does not give. `C` serves as a
conservative `contained` set (`PossibleTargets(site) subset of C`) for any site the internal ladder leaves
`whole_image`; it is used for `rte` as `contained`, never `exact`. It is applied only where the existing ladder yields
`whole_image` and never overrides a stronger internal result.

### Constraint discovered for T003

`parse_genesis_external_m68k_facts` is bounded at 1 MiB / 65,536 facts. A 24,180-entry set costs about 218 KB as
one v1 `contained` fact (9 bytes per entry), so repeating it once per residual site exceeds the file bound after
four sites (the SEG-043 Sonic 1 residual has six). The planner's island bound is *not* the obstacle: `total_entries`
counts distinct entries across sites (24,180 < 65,536). T003 therefore needs only the single smallest shared
source-container representation (one universe, sites reference it), not a planner change.
