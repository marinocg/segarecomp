# ADR 0179: SEG-079 - Sega CD static image reconstruction and one-shot compilation: result

- Status: Accepted (SEG-079-T001, checkpoints C2-C7). Executes the plan frozen in ADR 0178; deviations are listed in "Deviations".
- Primary outcome: **`PARTIAL_STATIC_RECONSTRUCTION_SUPPORTED`**.
- Privacy: no BIOS, disc or snapshot byte, hash, offset, destination, table base, template address, trace or disassembly appears here.
  Commercial inputs and every private intermediate stayed in the ignored `.cache/` of the base checkout; counts and classes only.

## The five separate decisions

| question | answer |
| --- | --- |
| Is the sub-BIOS epoch image fully reconstructable, without executing the BIOS, from the user's own input files? | **Yes** (tested BIOS, all qualified configurations): 131,072 of 131,072 bytes, identical length and SHA-256, from the BIOS image alone and a three-step recipe. |
| Are all mutable `JMP` templates structurally admissible? | **Attributable, not statically derivable.** 14 of 14 have a writer PC in immutable BIOS code (2) or in code of the reconstructed epoch image (12); 0 depend on runtime-dependent code; 0 are unattributed. **0 of 14 are derived before execution** by anything in this repository. |
| Is a safe distributable metadata schema possible? | **The epoch recipe is schema-safe but not publishable under the current policy; recommendation: do not commit it, regenerate it locally.** No template schema (no derivation to base it on). |
| Is cold one-shot compilation feasible? | **Not with what exists, under the stop rule.** The templates need execution-derived evidence (or a successor derivation / batched slot-run admission); a program emitted from the recipe alone stops `entry_not_compiled` at the first main thunk. |
| Is an opt-in implementation justified? | **Partly, and it is implemented:** the static epoch producer (small). The batched slot-run template admission that would remove ~14 of 15 rounds is a successor (authority-contract change). |

## Phase A: provenance of the materialized 128 KiB epoch image (C2)

Writer evidence: a generated-native run of the authentic main BIOS to the epoch with a new local diagnostic (`--ram-write-log`, outside
the program identity) shows that **every** PRG-RAM write before the release comes from exactly three writer instructions: a word-wide
clear of all 512 KiB, and the literal and copy writes of one routine. The routine is a public-format LZ decoder (Kosinski) called three
times with constant arguments from three static call sites of the immutable BIOS image.

| class | regions | bytes | % of image | independently supported | unresolved dynamic behaviour |
| --- | ---: | ---: | ---: | --- | --- |
| 1 verbatim copy | 0 | 0 | 0 | n/a | no |
| 2 reordered / segmented copy | 0 | 0 | 0 | n/a | no |
| 3 relocated / patched copy | 0 | 0 | 0 | n/a | no |
| 4 deterministically transformed (Kosinski) | 3 | 69,608 | 53.107 | **yes** (independent decoder reproduces each stream; read sequence identical) | one path condition, below |
| 5 initialised / generated (zero fill) | 2 | 61,464 | 46.893 | **yes** (constant-zero clear stream owns every byte) | no |
| 6 unknown provenance | 0 | 0 | 0 | n/a | no |

- *Equality is not provenance.* A source search alone finds 3,396 bytes (2.6 %) of long exact runs against the BIOS; **none** is a copy:
  they are the decoder's literal runs. A search-only method would have mislabelled them class 1. No byte was classified by equality.
- *Independent support for class 4.* (1) A one-off private observation (no committed tool or test; an independent reviewer reproduced it with a separately written decoder): the BIOS routine's complete sequence of 50,581 source reads equals, address for address, the
  access sequence of the independent Python decoder; (2) its output equals the image byte for byte (the provenance tool reuses the recipe tool's decoder, so its 'independent reproduction' is independent of the BIOS code, not of the recipe tool); (3) the C++ decoder in the emitter
  (a third implementation) produces the same image inside the real build (the run-time epoch check passes).
- *Executable vs data.* Of the sub's executed instruction starts in a bounded run, 3,078 lie in class 4 regions and 9 in class 5 regions
  (the runtime-built jump-slot table lives in a zero-filled gap). The reconstruction target is the entire image, not only code.
- *Unresolved dynamic behaviour.* The BIOS reaches the decode calls only when a work-RAM flag (derived from a boot-time device check) is
  clear; all ten privately available snapshots, taken under different cartridge, START-timing and disc configurations, are
  byte-identical and consistent with that path. The static scan cannot know the flag; the run-time whole-image epoch check does.
- *Other BIOS images.* Of 31 distinct 128 KiB BIOS images in the authorized local pack, the structural scan yields a three-step candidate for
  27 (each reconstructs without error) and fails closed on 4 (ambiguous / overlapping call sites). Only the BIOS used throughout this
  project is verified against an independent snapshot; the others are unverified candidates.

## Phase B: reconstruction prototype (C3)

`tools/sega_cd_epoch_recipe.py` (`reconstruct`, `verify`, `spec`, `scan`, `derive`): zero-initialised image plus ordered Kosinski steps,
no other operation. On the real BIOS: **identical length, bytes and SHA-256** to the independently materialized snapshot; identical across
3 repeated runs (0.05 s, 18 MB) and equal to all ten qualified-configuration snapshots. `scan` (a structural call-shape scan of the BIOS
image: no execution, no lifting; 0.08 s) produces the **same** recipe as `derive` (from the writer log), so the recipe is regenerable from
the user's BIOS file alone. Committed tests use project-authored inputs only (an encoder written here, hand-derived vectors, synthetic
BIOS-like images).

## Phase C: the mutable `JMP` templates (C4)

The Checkpoint H configuration admits 14 templates (2 main, 12 sub; the authentic ADR 0144 configuration admitted 12, a subset: 2 main,
10 sub). `tools/sega_cd_template_provenance.py` attributes each opcode-word store to its writer:

| writer class | count (14) | count (12-subset) |
| --- | ---: | ---: |
| static writer (writer PC in immutable BIOS code) - main | 2 | 2 |
| reconstructed writer (code inside the reconstructed epoch image) - sub | 12 | 10 |
| runtime writer (RAM code no immutable input or reconstruction explains) | 0 | 0 |
| unattributed | 0 | 0 |
| **derived before any execution by an implemented analysis** | **0** | **0** |

All 14 are members of two bulk jump-slot tables (30 slots main, 48 slots sub) built by loops in immutable or reconstructed code, and
re-installed with the same opcode by vector-setting routines in the same classes; the tables are complete (all slots written) at the frontier stop at which the
recorded run ends. The classes S/E of ADR 0178 are therefore met by *attribution*; they are **not** met by *derivation*, because
deriving the table (base, stride, count) without running guest code needs a concrete evaluator of the initialiser routines, which the
stop rule excludes ("arbitrary guest execution", "symbolic execution"). A previously observed PC is not authority; here it would also be
the wrong unit, since each template is one of a 30- or 48-slot run.

## Phase D: schema and distributability (C5)

`docs/architecture/sega-cd-epoch-recipe.md`: schema `segarecomp.sega_cd.epoch_recipe.v1` (fields, validation, a synthetic example
validated by a test). Gate against the five questions: no reconstructable content by itself; each step an independent structural fact;
three records per BIOS but an address inventory of firmware across variants; **`docs/testing/commercial-games.md` would need a separately
approved amendment** (its only carve-out is the Genesis scalar `logical_table_descriptor`); and the recipe **can be regenerated locally**.
Recommendation: keep real recipes private (ignored cache), regenerate with `scan`; do not widen the policy. No template schema is proposed.

## Phase E: the opt-in static producer and the measured comparison (C6)

Implemented (small): `epoch_recipe.cpp` (independent C++ decoder) and `--admit epoch-recipe:<length>:<source>@<dest>[,...]`; the image
is admitted with authority `static_proof` (ADR 0076: static reasoning over immutable input, no guest execution) and producer evidence
`epoch.recipe`; the admission chain records `epochrecipe <canonical recipe>` and **no pass identity is fabricated**; a pass-observed
admission after it must still carry the true identity of the program before it. The program is unchanged in what it enforces: at the sub
runnable epoch it compares live PRG RAM with the image (length, SHA-256) and stops `materialized_image_unverified` before the sub retires
an instruction; per-instruction byte guards, template guards, typed stops and program identity are untouched. The driver
(`--epoch-recipe`) fails closed (`epoch_recipe_unverified`) when the first epoch's live image is not the reconstructed one, including the
case where a build pass would otherwise silently capture a second epoch.

Authentic Checkpoint H configuration, identical inputs, one build each, cold object cache, 8 jobs (16-core host):

| | conventional | recipe-assisted |
| --- | ---: | ---: |
| generation rounds (+ confirming run) | 16 | **15** |
| compiled instruction functions (final) | 395,207 | 395,207 |
| generated C | 399,251,233 B | 399,251,233 B |
| wall time | 1,035 s | **920 s (-11 %)** |
| compile / emit / build-pass seconds | 747 / 211 / 63 | 645 / 200 / 60 |
| units rebuilt / reused (sum over rounds) | 131 / 1,425 | 125 / 1,344 |
| peak compiler RSS / peak emitter RSS | 856 MB / 1,229 MB | 855 MB / 1,228 MB |
| epoch image equal to the independent reference | yes | yes (reconstructed) |
| final outcome (both CPUs' registers and trace digests, all RAM/VRAM digests, result block, device log, ticks) | - | **identical** |
| program identity | differs (the provenance differs, as it must) | |

The saving is one discovery round: the sub translation units are compiled once instead of twice, and one emit/pass cycle disappears.
Rounds 2-15 (the 14 template rounds, ~50 % of the build) are untouched. Single runs; no variance estimate. Synthetic baselines
(`sega_cd_epoch_recipe_build_test`, fixture BIOS carrying a project-authored stream): 5 -> 4 rounds, same three templates, same digests,
COMSTAT and both processors' state; every fixture negative (uncompiled/odd targets, rewritten opcode, resident-code mutation, stale
return, incomplete/corrupt image) stops identically on the recipe-assisted program. SEG-079 leaves the accepted timing contract and the Checkpoint H result unchanged: ADR 0177 records Checkpoint H as achieved
(`TIME_ALIGNED`, identical pre-registered frames), and nothing about its timing conventions, gate or frames was touched. What SEG-079 measured is narrower: the
generated-native **outcome-digest equivalence** of the conventional and recipe-assisted programs (both CPUs' registers and trace
digests, all RAM/VRAM digests, result block, device log, ticks). It did **not** rerun the frame-by-frame Checkpoint H comparison against the
independent reference. Sonic CD Checkpoint G has no separate build here: the Checkpoint H
build contains its executable images.

## Adversarial matrix (task section 9)

Synthetic, in CI: valid single and multiple source ranges; source mismatch and stale BIOS bytes; wrong BIOS image/size/SHA; wrong length,
destination, overlapping or out-of-bounds steps; truncated or unterminated streams, back-reference before the stream, token bound;
patched (decodable but different) stream; non-canonical/duplicate/extra/missing fields, wrong schema version, boolean/negative numbers;
fabricated pass identity (epoch and template) refused; missing producer; unverified epoch (typed stop, sub retired nothing); deterministic
emission (byte-identical C); the fixture's template-shape, live-operand, uncompiled-target and overlap negatives replayed on the
recipe-assisted program. Provenance: equality without a copying writer is class 6; unwritten non-zero bytes are class 6; sanitized output
contains no offsets.

## Deviations from ADR 0178

1. ADR 0178 defined template class S as "writer and destination derivable from immutable instructions by a bounded analysis" and E as "present in
   the reconstructed epoch image" or "written by a copy loop of a verified reconstructed image". Under those **literal** definitions the sub
   slot tables (built at run time in a zero-filled gap, so not present in the image) are not E, and no template is S by an implemented
   analysis: the literal-rule outcome is S = 0, E = 0, R/U = 14 (they depend on running the initialiser code). This result nevertheless reports the
   finer writer-attribution classes above (writer PC in immutable or reconstructed code), which is a relabelling, not the plan's rule; the
   decisions rest on the literal outcome and on the added criterion "derived before any guest execution" (the plan permits only stricter rules).
2. The task statement says twelve templates (two main, ten sub), the figure of the ADR 0144 configuration; the Checkpoint H build admits 14.
   Both are reported.
3. Phase E was implemented for the epoch producer only, as the evidence supports nothing more.

## Correction of a premise carried from ADR 0178

ADR 0178 (frozen, preserved unaltered as historical pre-registration) says Checkpoint H "remains `TIME_ALIGNED`/strict-equivalence failing as
recorded". That reflected the task statement's outdated premise (ADR 0176's *not achieved* result). ADR 0177 superseded it: Checkpoint H is
achieved. ADR 0178's sentence had no bearing on any rule, measurement or decision; the correct statement is the one in Phase E above.

## Limitations

One BIOS image is verified against an independent snapshot; the writer log and the template attribution come from one recorded execution
path; the decoder's equivalence to the BIOS routine rests on the read-sequence and output equality for the three observed streams (the
routine itself is not re-implemented from its instructions); timing is a single run per arm; the structural scan is a firmware call-shape
heuristic in local tooling (not compiler core) and is only a candidate generator.

## Next steps (successor)

1. **Batched slot-run template admission** (largest saving; authority change): a frontier capture records the contiguous run of live
   `JMP` slots containing the stopped PC (bounded), admitted as one record; each slot keeps its opcode guard, live operand and
   compiled-destination check. Projected rounds: epoch recipe + 2 run frontiers + final instead of 15 (projection, not measured).
2. Alternative considered and not taken: admitting the template shape at every even RAM address (no evidence needed) - it changes the
   meaning of ADR 0142 invariant 7 (unknown executable bytes fail closed) and adds ~290 K functions.
3. A cold one-shot needs a derivation of the slot runs without execution; there is none, and building one is outside this spike's rules.
