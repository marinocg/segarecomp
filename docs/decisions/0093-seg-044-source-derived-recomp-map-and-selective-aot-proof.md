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

## SEG-044-T002 -- deterministic producer (implemented)

`tools/segarecomp_source_map_extract.py` turns the assembler listing into `segarecomp.m68k_source_universe.v1`
(ROM SHA-256, producer, exact source revision, source configuration token, sorted unique even entries, count, `end`). It
fails closed with a stable content-free class on: unknown construct with emitted bytes, unknown CPU, unbalanced
`save`/`restore`, bytes inside a macro definition, odd/outside-ROM/overlapping rows, opcode or data bytes that differ from the
ROM, an unexplained uncovered ROM range, count/size exhaustion, ROM-hash or source-revision mismatch. Only the source project's own
`org *-1` / `dc.b` operand-fixup idiom may overlap an instruction *operand* byte (never an opcode word or data). Skipped
conditional regions do not appear in the listing and macro-definition bodies carry no bytes, so neither can create authority.
Real run (aggregate): two clean rebuilds give byte-identical universes; 24,180 entries; instruction bytes 90,150; `dc` data bytes
57,001; other bytes (binary assets, Z80 blocks, padding) 377,239; 102 operand overlays. Cross-check with segarecomp's own
decoder: all 24,180 entries decode and every decoded length equals the listing length (0 mismatches, 0 undecodable) -- two
independent toolchains agree on the instruction stream. 20 hermetic synthetic tests (hash/revision mismatch, duplicate, odd,
unmapped, opcode mismatch, malformed listing, continuation gap, unknown construct, unknown CPU, unbalanced modes, macro bodies and
skipped regions, macro-expanded instruction, overlay idiom only, reordering, omission mutation, size/count exhaustion, CLI
determinism).

## SEG-044-T003 -- integration with the unchanged planner (implemented)

The shared universe enters through the existing external-input seam, nothing else: `GenesisExternalM68kFacts::source_universe`,
`parse_genesis_source_m68k_universe`, `--source-m68k-universe`. In `genesis_hybrid_container` the single centralized
`apply_external_fact` consults it after any per-site fact and only for a site the internal ladder left `whole_image`, as a
`points_to_region` contained set. Every member is re-verified (even, mapped, decodable, within the entry bound) or the
whole universe is discarded. `plan_genesis_hybrid_admission`, `validate_genesis_hybrid_round`, the fixed point, bounds, universe
fingerprint and production seam are untouched; an absent universe reproduces today's behavior exactly. Tests: universe resolves an
Unknown site; closed superset safe; omission of a feasible PC is *not* consumer-detectable (documented trust limit); never overrides
an internally exact site or a per-site fact; unmapped/undecodable member or over-bound discards the universe; validator requires
the matching universe; 13 parser rejection classes.

## SEG-044-T004 -- Sonic 1 under the UNCHANGED planner (result: H = U, `broad_analysis_incomplete`)

| configuration | sites contained | outcome | rounds | H | H/U |
|---|---|---|---|---|---|
| baseline, no external input (reproduces SEG-042) | 0 of 13 | `broad_whole_image` | 1 | 246,293 | 1.000000 |
| source universe only | 13 of 13 (all `points_to_region`, 24,180 entries) | `broad_analysis_incomplete` | 2 | 246,293 | 1.000000 |
| SEG-043 facts (7 contained, regenerated) + source universe on the 6 residual sites | 13 of 13 (7 per-site, 6 source) | `broad_analysis_incomplete` | 2 | 246,293 | 1.000000 |
| same, RTE sites given a 1-entry placeholder (uncredited ablation) | 13 of 13 | `broad_analysis_incomplete` | 2 | 246,293 | 1.000000 |
| same, PC-index sites given a 1-entry placeholder (uncredited ablation) | 13 of 13 | `broad_analysis_incomplete` | 2 | 246,293 | 1.000000 |

`U` 246,293, `D` 1,276, `|C|` 24,180 (`C/U` 0.0982). **The source authority resolved the structural obstacle completely**: every
one of the 13 baseline `whole_image` sites (and in particular the 6 SEG-043 residuals -- 4 PC-relative table dispatches and 2
`rte`) obtained a re-verified container, and round 1 converged. Round 2, which analyses the island entries as opaque-state roots,
returns `broad_analysis_incomplete`: the SEG-030 memory-domain fixed point does not converge within `m68k_memory_round_bound`
(16) once a 24k-entry island of unknown-state code is rooted (reason `iteration_bound`; about 663k solver iterations and 286k program
points when it stopped; 2.2 GB peak RSS; 423 s wall on a Release build). It is a capacity/precision limit of the unchanged
analysis, not a trust failure, and the planner correctly answers broad. A raise of the solver caps (x64, uncredited scratch
build) gave the identical stop, so the solver iteration/point caps are not the binding limit; a scratch raise of the memory round
bound to 256 had not finished after 10 minutes and was abandoned. Both single-class ablations stop identically, so neither
PC-relative table dispatch nor `rte` alone can be contained by a code-universe-sized island under this analysis. The planner was
not modified to obtain a result.

## SEG-044-T005 -- economics and correctness of the source authority itself (DIAGNOSTIC plan, not a planner result)

Because the unchanged planner cannot consume a `|C|`-sized island, the *source authority* was evaluated without the planner:
`tools/segarecomp_source_universe_plan.py` writes the admission plan `H = C ∩ U` (diagnostic, never a production route, never
credited as the SEG-031 planner's result), which then goes through the unchanged production seam
(`segarecomp build --admission-plan`). The production validator independently accepted it (fingerprint, ROM hash, structural closure over
fixed successors, call continuations and machine roots): no static control edge leaves `C` into a broad identity.

| | broad | source-direct plan (diagnostic) | change |
|---|---|---|---|
| admitted identities | 246,293 | 24,180 | -90.18% |
| generated C | 177.9 MB | 25.8 MB | -85.49% |
| compile CPU (`segarecomp build`; fresh-process run, the in-process comparison tool run gave 488.9 / 106.3 s) | 490.6 s | 106.3 s | -78.3% |
| build wall | 135.2 s | 31.9 s | -76.4% |
| compiler peak RSS | 531 MiB | 232 MiB | -56% |
| executable | 39.3 MB | 7.7 MB | -80.3% |
| 3,000-frame run (bridge `run_wall_seconds`, 2 runs each) | 2.66, 2.72 s | 2.71, 2.65 s | within noise |

Execution-PC oracle (broad generated-native program, complete instrumentation but one no-input workload: 23,200 frames, explicit instruction budget; it observes 43.5% of `C`): 10,512 distinct
observed PCs, all inside `C` and inside `U`: **0 escapes**. Broad and selective produce identical final-state and execution-coverage digests (3,000 frames, both repetitions); the
frame-stream digest is the empty-input digest in both because the coverage route does not render, so it carries no evidence. Mutations: omitting any of 7 observed (executed) PCs from `C` was
rejected by the production validator (`fixed_successor_not_admitted` / `machine_root_not_admitted`) *and* is an oracle escape;
adding closed supersets (extra `RTS` words in data) is accepted with 0 escapes; adding arbitrary decodable data PCs is rejected
(not closed under fixed successors). These meet ADR 0080 decision 11's per-title thresholds (generated C -30%, compile CPU -25%,
runtime <= +15%) *for the diagnostic plan only*; zero oracle escapes proves nothing about unobserved paths.

Note on the aggregate counters: `external_facts_applied` counts every site that used an external input including the source
universe (`source_universe_applied` is the universe-only subset).

## SEG-044-T006 -- independent adversarial review (PASS WITH FINDINGS) and hardening

An independent reviewer (no edit rights) rebuilt REV01 and REV00 in fresh clones with the Python port (REV01 MD5 equals the
repository constant; REV00 SHA-256 equals the pinned digest, ROM byte-identical to ours), re-parsed the 10 MB listing with its own
row accounting (52,955 byte-emitting rows, identical; byte accounting reconciles), decoded all 24,180 members with segarecomp's
decoder (0 undecodable; every fixed successor, branch, call and stacked continuation that lands in the ROM is itself in `C`; no
fixed edge reaches data or mid-instruction; all 15 exception-vector targets in `C`), parsed all 203 PC-relative index tables from
the listing (827 entries, all in `C`), and found no planner logic altered. Findings and disposition:

* Extractor gap masking (minor): a zero-width directive row (`even`, `org`, ...) at the address of a deleted instruction row could
  explain the resulting hole. **Fixed**: asset directives (`binclude`, `incbin`, Z80 `save` blocks) explain a segment; pad
  directives explain it only when the ROM bytes are one uniform `00`/`FF` fill; gaps are split at every inner directive row.
* Extractor nested blocks (minor): an inner `while`/`rept`/`irp` `... endm` ended macro-definition mode early. **Fixed**: block
  depth is counted (`endm`/`endr`/`endw`). The real universe is byte-identical after both fixes; two regression tests added.
* Documented trust limit, quantified: relabelling a real instruction row as `dc.w` passes the extractor, and 799 of the 24,180
  members have no fixed or stacked in-edge, so only the runtime oracle (or the external authority) could notice their omission.
* Wording corrections (frame-stream digest, "complete" oracle, figure provenance): applied above.
* The production validator checks structural closure only (fixed successors, call continuations, roots); dynamic containment is the
  planner's job, so "validator accepted" is structural evidence, not a completeness proof (stated in T005).
* The diagnostic plan builder is judged acceptable as a labelled diagnostic (it emits no `alias` lines, prints a banner, is not wired into
  CI or defaults); a hand-written plan is already possible because the seam validates structure only.
* Fast-gate result: the focus suites (`analysis_hybrid_plan`, extractor, plan builder, harvest) passed 4/4; the fast preset showed
  two failures not attributable to this diff -- `segarecomp_build_command_test` (generated `hook-gen.c` rejected by gcc 14 for
  implicit `fdopen`; files untouched here, base behavior unconfirmed locally) and `m68k_conformance_harness_test` (killed by load,
  passes alone in 83 s). GitHub CI is the arbiter. Harness defect noted: `tools/agent_exec.py` crashes on a missing `container` import.
* Process: the work is one combined branch/PR (`task/seg-044-t001`, PR #83) bound to T001..T006 by operator instruction; per-child
  branch metadata follows the SEG-043 precedent of the schema-mandated names.

## Final answers

1. **Exact ROM from the selected source?** Yes: s1disasm `7ebe4b3d0c182b2566026b6d3f423d33539558da`, its bundled AS/p2bin, REV00 via the
   repository's own `Revision = 0` switch, SHA-256 equals the pinned ROM, twice from fresh clones (plus the reviewer's third).
   The `build.lua` driver was executed through a validated Python port (no Lua interpreter available).
2. **Accepted authority constructs:** only 68000-mode assembler-listing rows with a closed MC68000 mnemonic and emitted bytes
   (macro-expanded rows included); everything else (data, assets, Z80, padding) asserts nothing.
3. **Global executable universe `C` (path A)** was defensible and mechanically derived: 24,180 instruction starts.
4. **SEG-043 residual Sonic 1 sites source-resolved:** 6 of 6 (4 PC-relative table dispatches, 2 `rte`), all as `contained`
   (0 exact); 13 of 13 baseline sites.
5. **Did the unchanged SEG-031 planner reach `H < U`?** **No.** `broad_analysis_incomplete`.
6. **U 246,293; C 24,180 (C/U 0.0982); planner H 246,293 (H/U 1.000000, reduction 0).** Diagnostic source-direct H 24,180
   (0.0982; reduction 222,113, 90.18%) -- not a planner result.
7. **Runtime-PC escapes (diagnostic plan):** 0 of 10,512 observed PCs (23,200-frame no-input workload; 43.5% of `C`); not a proof.
8. **Behaviour identical to broad:** final-state and coverage digests identical over 3,000 frames, two repetitions each.
9. **Economics (diagnostic plan):** generated C 177.9 -> 25.8 MB (-85.49%); compile CPU 490.6 -> 106.3 s (-78.3%); compiler peak
   RSS 531 -> 232 MiB; executable 39.3 -> 7.7 MB; build wall 135 -> 32 s; runtime unchanged.
10. **ADR 0080 per-title thresholds:** met by the diagnostic plan; not creditable because the planner route yields H = U.
11. **Broad AOT remains the production default.** (Needs >= 2 complete-oracle titles *and* a planner-credited or newly-decided
    admission route.)
12. **Retain?** The deterministic fail-closed producer and the inert shared-universe input are small, tested and worth keeping as
    the experiment's reproducible basis. The finding that matters is negative for the planner route and positive for the authority.
13. **Generic recomp-map format/registry?** No. The evidence does not support a registry or multi-game framework.

## Classification

**SOURCE AUTHORITY PARTIAL (planner cannot consume it): H = U under the unchanged SEG-031 planner.** The exact-ROM gate passed and
the source universe resolved every residual site structurally, but the unchanged analysis stack cannot take a code-universe-sized
island (memory-domain fixed point does not converge), so the credited result stays `broad_analysis_incomplete`, H/U = 1.000000.
Separately and uncredited, the source authority itself survives the available falsifiers (decoder agreement 24,180/24,180, static
control-flow closure, 0 oracle escapes, rejected/accepted mutations) and, used directly, would admit 9.82% of `U` with large
generated-C and compile savings. This distinguishes the two statements in the milestone brief: selective-AOT *machinery* (emitter, seam,
validator, runtime) works on a real title; what fails is the planner's ability to carry the completeness claim, not the claim's plausibility.

## Successor decision

No successor is registered by this milestone. If the operator wants to pursue the value, the one bounded and evidence-supported next
step is a **decision milestone, not an implementation**: (a) an ADR deciding whether a source-authority admission route that
bypasses the analysis (plan = authority universe, validated by the production closure check and the execution-PC oracle) is an
acceptable trust boundary at all, and (b) if yes, a second source-backed title (Sonic 2 via its public disassembly) to meet ADR 0080's
two-title rule. Making the SEG-030 analysis consume a 24k-entry island is the SEG-038..043 line this milestone deliberately did not
reopen. Merge of PR #83 remains an operator decision; broad stays the default.
