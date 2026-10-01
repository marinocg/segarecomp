# ADR 0073: Build-Time Z80 Image Materialization and RAM-Backed Code Identity

- Status: Accepted (SEG-032-T001); amended by SEG-032-T002 (activation signature S1\*, measured results below); T008 freezes the bounds.
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
   `materialization_nondeterministic`, `z80_image_compile_failed`, `z80_signature_collision`.
6. **Runtime selection.** At each runnable transition while pristine the runtime computes the signature from live state and
   looks it up in the fixed compiled registry; unknown -> `z80_unknown_image` (no compilation, no decoding, no learning).
7. **RAM-backed code rule.** After activation mutable data is free. Before each generated instruction its 1-4 static bytes
   must equal the live RAM bytes (`z80_code_mismatch` otherwise), enforced in the single emitted entry prologue
   (`emit_entry`). The SEG-033 owner structure is unchanged: group-internal `goto` and direct binding both land on the next
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
