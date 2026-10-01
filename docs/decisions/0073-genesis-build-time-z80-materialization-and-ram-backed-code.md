# ADR 0073: Build-Time Z80 Image Materialization and RAM-Backed Code Identity

- Status: Accepted (SEG-032-T001); T002 amends the activation-signature section with the measured proof; T008 freezes the bounds.
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
   - *Activation signature*: the runtime selection key, defined to ignore unrelated mutable and carry-over RAM. Candidates S1
     (68K-written extents since the last reset assertion) and S2 (instruction-footprint bytes) are frozen; T002 must prove one
     with: fill-pattern perturbations, scribbled carry-over data, a multi-epoch dirty-data falsifier (epoch 2 inherits epoch 1's
     driver state; epoch 3 repeats epoch 1's code over different carry-over), a different-code/identical-carry-over collision
     control and footprint-byte sensitivity controls. Failure to prove either is NO-GO (or an operator decision), not a silent
     redefinition.
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
