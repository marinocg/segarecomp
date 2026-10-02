# ADR 0073: Build-Time Z80 Image Materialization and RAM-Backed Code Identity

- Status: Accepted (SEG-032-T001); amended by SEG-032-T002 (activation signature S1\*, measured results below); T008 freezes the bounds and implements the pipeline (record below).
- Date: 2026-10-01
- Related: ADR 0058 (broad immutable-image AOT; "SEG-032-style materialization"), ADR 0071 (owners), ADR 0072,
  `docs/architecture/genesis-z80-audio-contract.md` §5-7.

## Context

On the Genesis the Z80 program is data the 68K builds in sound RAM at run time (copied, decompressed, patched, replaced).
Broad AOT needs the bytes at build time; the user must supply nothing. SEG-029/030 (static M68K analysis) are not
prerequisites and are not reproduced here. SEG-031 (admission policy over known images) is not a materialization producer.

## Decision

1. **Producer: bounded generated-native build-time materialization.** Inside one `segarecomp build`, the machine is run from
   deterministic reset (generated-native M68K and Z80 only; no input; finite instruction budget and wall timeout on every
   execution of a generated program). Only generic hardware events are observed: `/RESET`, BUSREQ and writes to Z80 RAM.
2. **Epoch.** The *runnable transition after a reset/upload epoch* (contract §5): `/RESET` released and BUSREQ not asserted
   while the Z80 is pristine. The snapshot is the full 8 KiB RAM.
3. **Two keys, never confused.**
   - *Content hash* (contract §7): names a compiled image and makes the registry reproducible. Build artifact.
   - *Activation signature S1\**: the runtime selection key, defined over the 68K-written extents of the **hold window**
     (since power-on or the previous runnable transition, epoch or plain resume). The T001 candidate "writes since the last
     `/RESET` assertion" was disproved by the first real run (the documented upload sequence asserts `/RESET` after the
     upload, so the set was empty) and replaced; the instruction-footprint alternative S2 is the documented fallback only.
     T002 proof (`tests/genesis_z80_epoch_signature_test.py` on the real runtime router, plus the authorized workload):
     identical signatures for the same code over different carry-over data and different power-on fills, different
     signatures for different code and for a one-byte code change, an independent model of the observer state machine,
     and three negative controls (whole-RAM identity, writes-since-assertion, clearing only at an epoch).
4. **Fixed point.** `run -> unknown signature at a runnable transition -> write the snapshot and stop -> derive content hash and
   signature -> emit and compile only that image with the existing broad Z80 AOT -> regenerate the registry -> relink -> restart
   from reset`. It ends when one complete run of the observation window ends with no unknown epoch; one confirming run must
   reproduce the ordered list exactly. Images are ordered by first activation.
5. **Bounds are constants, not options** (provisional image bound 8, ceiling 16; observation window in virtual frames,
   instruction budget, wall timeout, `iterations <= bound + 1` measured and frozen by T002/T008). Exhaustion is a typed build
   failure with no executable: `z80_image_bound_exceeded`, `materialization_budget_exhausted`,
   `materialization_nondeterministic`, `z80_image_compile_failed`. A snapshot that differs in executed code from the image its signature selected surfaces as the guard's `z80_code_mismatch` (a typed build failure in the pass).
6. **Runtime selection.** At each runnable transition while pristine the runtime computes the signature from live state and
   looks it up in the fixed compiled registry; unknown -> `z80_unknown_image` (no compilation, no decoding, no learning).
7. **RAM-backed code rule (amended by T012; T003 originally required exact equality of all 1-4 bytes).** After activation mutable
   data is free. Before each generated instruction the live RAM bytes are snapshotted once; every *statically defining* byte (prefixes,
   the opcode, the final DDCB/FDCB opcode and every byte encoding a register, condition, bit or operation selection) must equal the
   compiled byte (`z80_code_mismatch` otherwise), while the descriptor displacement and immediate payload bytes are live operands read
   from the entry snapshot. Enforced in the single emitted entry (`emit_entry`): boundary acceptance, then snapshot + structural guard,
   then instruction-begin bookkeeping, then the effect. The SEG-033 owner structure is unchanged: group-internal `goto` and direct binding both land on the next
   entry's prologue, so the guard covers every instruction boundary; T003 proves it by reading emitted code and by mutation
   tests and falls back to forced dispatcher return where any path skips a prologue. Immutable images (SMS) emit exactly as before.
8. **Future producer swap.** A later static producer (SEG-029/030) replaces only step 1-4; the content hash, signature,
   registry, guard, runtime selection and devices are unchanged. Local names say "materialization", not "Gen-3".

## Rejected alternatives

- Scanning the cartridge for a driver / per-title extraction (title knowledge, fragile).
- Whole-RAM hash as the runtime key (mutable carry-over data would make selection input-dependent).
- Treating a code mismatch as a new epoch mid-run (not a hardware-defined event; unsupported by design).
- Runtime compilation, JIT or interpreting a replacement opcode.
- Coverage-driven "run N seconds and compile what appeared": the pass only resolves hardware handoffs and has hard bounds.

## Consequences

- Epochs that occur after the observation window or depend on input surface as `z80_unknown_image` at run time (documented unsupported).
- Evidence records only counts and classes; image hashes are local artifacts.

## T002 decision record (2026-10-01): GO

Generic epoch observer in the Genesis runtime (`GenesisZ80EpochObserver`, optional, non-semantic) plus the probe seam
`platforms/genesis/viewer/z80_epoch_probe_main_hook.c` (mirrors the execution-coverage seam; bridge option `--z80-epoch-probe`).
Authorized-workload aggregates (zero input, quick profile, 600 virtual frames, power-on fill 0):

| workload (all six authorized local images) | epochs | restart epochs (empty hold window) | images (signature classes) | notes |
| --- | --- | --- | --- | --- |
| Sonic 1 | 3 | 0 | 2 | epoch 1 a 38-byte stub upload at frame 0; epochs 2 and 3 a 7,110-byte image at frames 59 and 348 that differ in exactly one byte outside the uploaded extent (carry-over data): content hash 3 classes, signature 2 |
| Sonic 2 | 2 | 0 | 2 | 38-byte stub, then a 4,872-byte image at frame 113 |
| Golden Axe | 3 | 0 | 2 | stub, then two 8,192-byte uploads at frames 46 and 48; then an unrelated M68K stop (`known_but_unemitted_target`) at frame 54 |
| Streets of Rage | 4 | 1 | 2 | stub, a restart at frame 53, a 7,883-byte image at frames 60 and 74 |
| Cool Spot | 3 | 1 | 2 | stub, a restart at frame 2, an 8,192-byte upload at frame 4 |
| OutRun | 5 | 3 | 2 | stub, a 5,050-byte image at frame 80, three restarts (frames 80, 262, 597) |

Every image set is a tiny boot stub plus at most one driver image: **maximum 2 images per workload**, 2-5 epochs. All six reach their first epoch within the first frame
and their driver image within 113 frames. Restart epochs (reset pulse with no 68K write to Z80 RAM) are common (3 of 6 workloads), which is why an empty hold window re-binds
the previously bound image (contract §7) instead of creating an image.

Caveats recorded honestly:
- The 600-frame window is long enough for the boot-time handoffs of both workloads; epoch 3 of Sonic 1 appears in this seam, where
  **no Z80 executes**: the 68K reads Z80 RAM mailboxes the real driver would write, so with another power-on fill (0xFF) the 68K
  takes a different path and epoch 3 does not occur within the window (dispatch count 2.3 M vs 6.3 M). Later epochs are therefore a
  lower bound and must be re-derived with the executing Z80 (T008); epochs 1 and 2 are fill-independent.
- Broad AOT of one snapshot (the existing `z80_image_emitter`, debug build, 16 KiB two-mirror window): 10 translation units,
  3.2-4.1 MB of C, 0.8-1.1 MB of objects, 0.2 s emit and 0.6-1.0 s strict-C11 `-O2` compile wall on 8 jobs per image; three repeated
  derivations are byte-identical. The cost of one more image is therefore about a second; the image bound is driven by correctness,
  not by build time.
- Proposed constants (frozen by T008 after the executing-Z80 re-run): image bound 8 (observed maximum 2 images, 5 epochs; rule: at least 2x the
  maximum observed, at most 16), observation window 600 virtual frames (the last epoch observed anywhere is at frame 597, a restart; Sonic 1's driver reload at 348), per-run instruction budget 400,000,000 retired dispatches
  (the observed runs used 2.3-6.3 M), per-run wall timeout 120 s, loop iterations at most bound + 1.

## T003 implementation record (2026-10-01)

- `CodeImage::live_bytes` (banked, one window) makes `emit_entry` emit `z80_code_guard(rt, pc, n, b0..b3)` right after the unchanged
  prologue. Because a banked image never has direct binding or in-group chaining (`Plan::successor` exists only for invariant images)
  every instruction returns to the dispatcher and runs prologue + guard: **no RAM-backed mode change to the SEG-033 chaining was
  needed**; the audit result of the refinement (the guard goes in the single `emit_entry` point) is confirmed, and the structural test
  asserts no `goto z80_e_` / `Z80_OWNER_NEXT` in a live image. Cost: a dispatcher round trip per instruction (about 100-300 ns per
  ADR 0058's measurement), roughly 3-10 million Z80 instructions per second, against about 1 million per second needed.
- Exhaustive evidence in `tests/z80_live_guard_test.py` (every byte of every instruction of a straight-line program covering 1-4 byte
  instructions, loop, block-repeat, interrupt-entry and missing-matcher cases, equivalence with the immutable reference for owner
  groups 1 and 128, and a golden-digest regression of immutable emission).
- `libs`-independent registry: `platforms/genesis/machine` `segarecomp_machine_genesis_z80` (content hash, signature, deterministic
  registry with the image bound, RAM-backed `ImageSet`, the generated `*_registry.c` with `genesis_z80_image_for_signature`).
  Two epochs with the same signature are the same image even when their snapshots differ; a code difference there is the guard's
  `z80_code_mismatch`, so no separate collision failure exists.

## T008 implementation record (2026-10-01)

**Pipeline (one `segarecomp build` invocation, Genesis route).** analyze, generate (the existing M68K emit route), compile the stable set once
(M68K units, runtime, Z80 machine, shared PSG, vendored ymfm YM2612 + its C++-runtime shim, the sound attach point), then the fixed point
(`platforms/genesis/machine` `z80_materialization.[hpp|cpp]`, pure platform logic over a `PassRunner`): emit the registry in-process
(`emit_registry`), compile only Z80 units whose content hash is new, link the pass program, run it headless from reset with no input; an
unknown activation signature stops the program with `z80_unknown_image` after writing the snapshot, the build derives identity, registers the
image and repeats; a run that ends with no unknown epoch is the candidate and one further run of the same program must reproduce the
outcome class, the frame count and the ordered epoch identities exactly. Then the final program is linked with the **same** generated
units, registry and runtime objects and the production hook (`genesis_sound_hook.c`; the viewer hook attaches the same sound set).
The pass differs from the final program only by its hook object (`genesis_materialize_hook.c`): there is no second configuration to drift.
The pass observes `/RESET`, BUSREQ and Z80-RAM-write epochs only (the T002 observer's `on_epoch` seam) and never decodes a Z80 byte;
the image code runs generated-native with the T003 guard, the Z80 machine and the devices attached, so epochs that depend on the
executing Z80 (the T002 caveat) are now observed.

**Bounds frozen (constants, not options; `z80_materialization.hpp`, `z80_images.hpp`).** image bound 8 (the authorized workloads show at most
2 images: 4x; ceiling 16), observation window 600 virtual frames (the last epoch observed anywhere in T002 was frame 597; the window must
not be shortened), per-run instruction budget **100,000,000** dispatches (re-frozen down from the provisional 400 M: the 600-frame runs
retire 2-20 M dispatches and the only way to exhaust the budget is a guest that never advances virtual time, which 100 M ends in seconds),
per-run wall timeout 120 s, discovery runs at most bound + 1 (the completing run included), plus the confirming run.

**Typed build failures, no executable, `status.json` records outcome and counts only.** `z80_image_bound_exceeded`,
`materialization_budget_exhausted` (instruction budget or wall timeout), `materialization_nondeterministic` (the confirming run differs;
also a run that does not reproduce the previous run's epochs), `z80_image_compile_failed`, `materialization_no_convergence` (defensive: unreachable
while each unknown image either registers or fails), `z80_code_mismatch` (a registered image's compiled bytes differ from the live RAM:
the signature under-determines the workload, or the Z80 modified its own code), `z80_execution_unsupported` (any other typed Z80 stop in the pass),
`materialization_pass_failed` (no usable report, or the pass and the build disagree on a signature). A non-Z80 guest stop inside the window
ends the observation there (the final program stops at the same frontier); the frames reached are recorded.

**Corrections found by running real workloads.** (1) The RAM-backed emitter treated a decoded start longer than four bytes (data that decodes
as redundant prefixes, or an instruction running past the 8 KiB RAM) as a failure of the whole image; it is now the same typed never-run stub
(`z80_mutable_code`) as an endless prefix run (`libs/codegen/c11/src/z80.cpp`; regression in `genesis_z80_images_test`). One authorized workload
failed `z80_image_compile_failed` before this and converges after. (2) A guest that never advances virtual time (the degenerate BRA-to-self
stub the older build tests used) can never finish the window: it is now the typed `materialization_budget_exhausted`; the build test was
updated (its loop fixture is a real spin loop) and keeps the stuck stub as the typed-failure case.

**Workload results (authorized local images, aggregates only; default `-O2`, cold, one machine).** Of six images, four converge:
2 images each, 3-5 epochs, one confirming run; three reach the full 600-frame window and one stops earlier on an unrelated M68K frontier
(frame 54, recorded as `guest_stop`). Two fail closed with `z80_code_mismatch` at the driver's first frames: a local-only instrumented
rerun (not committed) shows the mismatching code byte is a byte the Z80 itself wrote (self-modifying driver code), the case the contract
(section 6) declares unsupported; that is the successor frontier. Build cost on the converging images: the materialization stage is 11-18 s of a
60-115 s build (3 discovery runs, 1 confirming run, 31 Z80 units compiled over the iterations for 19 in the final emission, 5-9 s of Z80
compile wall, 10.2-10.7 MB of generated Z80 C, 3.8-4.1 MB of Z80 objects, final executable 34-51 MB); the vendored ymfm objects add a few seconds
once per build. Unit reuse across iterations is by content hash but adding an image renames and reshards every unit (0 reused), so the
Z80 compile cost is the sum over iterations; a per-image emission would make it linear (not needed at the measured bound).

**Falsification evidence** (`tests/genesis_z80_materialization_test.cpp` scripted runners; `tests/genesis_z80_build_pipeline_test.py`, the
real route on project-authored ROMs): exact image counts for multi-epoch (2 images from 4 epochs incl. a plain restart and a repeated
signature), raw vs computed ("decompressed") upload of the same bytes = one image, byte-identical Z80 C, registry and M68K C across repeats and
worker counts (1, 4, default), bound + 1 images and bound images, instruction-budget exhaustion, self-modifying Z80 code (`z80_code_mismatch`),
an epoch after the window (not materialized; typed `z80_unknown_image` at run time), a registry with the image removed (typed unknown image),
one image's emitted bytes mutated (`z80_code_mismatch`), the unmodified relink as the control, and a pipeline rebuild that restores it;
`tests/genesis_z80_forbidden_identifiers_test.py` scans the production surface against `tests/fixtures/genesis-z80-forbidden-identifiers.txt`.

## T012 implementation record (2026-10-02): shape-stable live operands

Evidence (Phase 1, sanitized aggregates) showed that the executed `z80_code_mismatch` stops of two authorized workloads were largely
operand patches of an unchanged instruction form (relative displacements, indexed displacements, immediates), with only a small residue of
opcode/length-shape toggles at a few PCs. T003's exact-byte rule was therefore too strict for a class that is not structural self-modifying
code. The rule is replaced, for RAM-backed images only, by the structural-guard / live-payload rule of decision 7.

- **Mechanism.** `CodeImage::live_bytes` entries compute a structural-byte mask from the CPU-owned `FormDescriptor` (`displacement_index`,
  `immediate_index`, `immediate_size`) shifted by `DecodedInstruction::extra_prefix_count`; ignored/superseded prefixes, effective prefixes,
  opcode bytes and register/condition/bit-selecting bits stay structural. The entry calls `z80_live_guard(rt, pc, length, mask, b0..b3)`: one
  host `code_fetch` of the 1-4 live bytes into runtime-private `rt->live_code` (non-architectural, no cycles or side effects), a masked compare,
  `Z80_ERROR_CODE_MISMATCH` before any effect on a difference or a missing callback. Lowering rows take every displacement/immediate from the
  `operand_*` expressions of `LowerContext` (`live_operands`): the literals as before for immutable images (emission is byte-identical, golden
  digests unchanged), `rt->live_code[...]` for RAM-backed images. No runtime decoder and no second decoder exist; shared effect bodies stay
  shareable because the live text is operand-independent. An instruction that writes its own operand uses its entry snapshot.
- **Prologue order fixed.** The T003 entry ran the merged `z80_owner_prologue` (which clears `int_deferral` and the LD A,I/R marker) before
  the guard, so a `code_mismatch` consumed boundary state. RAM-backed entries now run `z80_owner_boundary`, the guard, then
  `z80_owner_begin`; the merged prologue is unchanged for immutable owners.
- **Typed limitation kept.** A RAM-backed start whose logical length (extra prefixes included) exceeds four bytes remains a `mutable_code` stub; the
  host fetch and the mask are 4 bytes wide. Structural self-modifying code (any change to a structurally defining byte) stays unsupported
  and fail-closed: no multi-form-per-PC dispatch, no per-write images, no all-opcode compilation.
- **Evidence.** `tests/z80_live_operand_test.py`: every legal canonical form with a displacement or immediate field (106 forms, 147 cases,
  base/ED/DD/FD/DDCB/FDCB) executed over six payload values and two register/flag initialisations equals an immutable reference compiled
  directly from the mutated bytes (state, memory digest, port-access digest, cycles; shared and unshared emission); every single-bit
  mutation of a structural byte stops at entry with no effect; loop, DJNZ and LDIR re-entry, self-written operands, patched and mutated
  interrupt handlers, rejected-instruction boundary state after an EI deferral and after LD A,I, a host without the callback, and the absence
  of decoder symbols. `tests/z80_live_guard_test.py` now distinguishes structural from payload bytes.

**T012 real-software rerun (authorized local images, sanitized aggregates; `segarecomp build` at `-O2`, `--jobs 4`, executed with
`--instruction-budget 100000000`).** Sonic 1, Streets of Rage and OutRun do not regress: each still converges (2 images, 600-frame window,
3-5 epochs), runs to the instruction budget with a non-silent audio stream, and the audio digest is identical across two runs. Sonic 2, which stopped with
`z80_code_mismatch` before, now converges too (2 images, 2 epochs, 600-frame window); its audio is non-silent and its digest and linked executable are
identical across two independent builds and two runs each. Cool Spot still stops with `z80_code_mismatch` in the materialization pass, now at a very early
driver frame (single-digit frame count, 3 epochs seen): by construction of the guard the stop is a structural form change only, because a displacement or
immediate difference is no longer a mismatch; it is the residual class (opcode/length-shape toggles of a patched instruction) that this ADR keeps
unsupported pending an operator decision. **>4-byte audit:** none of the five workloads executed a RAM-backed start of more than four logical bytes
(no `z80_mutable_code` outcome in any converging run; the fifth stopped on the structural mismatch before any such start), so the typed limitation is kept
without a workload that needs it.

## Sound-CPU fault isolation amendment (SEG-032, supersedes the "fail the whole build on `z80_code_mismatch`" consequence)

`segarecomp build` is a single user-visible invocation with an internal bounded generated-native materialization phase that DOES execute generated-native
guest code; there is no manual dump/export. The final executable contains only AOT code: it performs no materialization, learns no image, decodes no opcode, and
has no JIT or interpreter. Later SEG-028/029/030 may provide a static image producer; SEG-031 does not solve materialization.

A structural Z80 code mutation no longer fails the whole build or stops the whole machine. It latches a permanent sound-CPU fault (contract section 18): the Z80
neither executes nor writes devices, BUSREQ/RESET/bank/M68K device accesses keep their documented behaviour, a reset or re-upload does not clear the fault, and the
build reports `genesis_audio = degraded` / `z80_audio_outcome = structural_code_mismatch`. All other typed failures (unknown image, bound exceeded, budget, nondeterminism,
compile failure, bank target, unmapped view access) stay whole-build fail-closed. Product status of such a title: game execution supported through the observed route;
Genesis audio degraded; never fully supported audio.
