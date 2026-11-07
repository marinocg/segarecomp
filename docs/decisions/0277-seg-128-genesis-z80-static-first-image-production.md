# ADR 0277: Genesis Z80 static-first executable-image production

Status: accepted (opt-in; default enablement is a separate, measured decision, see Consequences). Builds on ADR 0073 (Genesis Z80
materialization and image identity), 0077 (executable-image producer boundary), 0089/0135 (Sonic 1 driver; static extraction feasibility),
0134 (structural mutation NO-GO, untouched), 0255/0256 (execution-observed reconstruction; SEG-112 static producer, reference only), 0259 (Sega CD
epochs; untouched) and 0276 (SEG-127: stable Z80 unit layout, the baseline this is measured against). Baseline: product `main` 5547e8d.
Architecture, audit and listing: `docs/architecture/genesis-z80-static-first.md`. Measurements: `docs/testing/genesis-z80-static-first-measurements.md`.

## Context

After SEG-127 the superseded-object cost of the Z80 fixed point is solved (27.5 s -> 0.2 s on Sonic 1), but the *passes* remain: for N images the
build runs N discovery programs, each followed by a Z80 emit/compile/link generation, plus a confirming run. Most of that is determined by
immutable cartridge bytes. Goal: for fully provable cases one Z80 compilation generation; unchanged behaviour for everything else.

## Decisions

1. **A small producer, not a framework.** `produce_static_epochs(rom)` returns the runtime's own `Epoch` records (8 KiB RAM + written-extent
   bitmap) in activation order; `preload_registry` adds them to the existing `Registry`. No new image IR, no new dispatcher, no change to the
   emitter, the generated code, the activation signature or the runtime selection.
2. **Epoch semantics are replayed, not re-invented.** BUSREQ/RESET latches, the hold window, word/long store byte rules, mirrors and the
   epoch rule are copied rule for rule from `runtime.c` (`genesis_z80_bus_write`, `genesis_z80_epoch_note_ram_write`, `genesis_z80_area_access`).
3. **Only recognised mechanisms write the model.** Constant stores, a counted constant `MOVE.B (As)+,(Ad)+ ; DBRA` ROM copy, and a call of a
   recognised Kosinski routine. Everything else that could touch the window while the 68K owns the bus is a typed refusal. Equal bytes are never
   proof of a causal upload; a data scan is not used.
4. **Compressed uploads: Kosinski only, because it is what the evidence shows.** Sonic 1's driver stream is Kosinski (ADR 0089/0135; the loader
   at the call site is a ROM routine whose code is shared, byte for byte, by 9 of the 64 local images). Recognition is the SHA-256 of the routine's
   position-independent code from its entry to its first `RTS` (two forms: a project-authored *reference* routine, documented and used by the
   fixtures, and the *library* form, digest only - no title code is stored). The decode uses the shared public decoder extracted from Sega CD
   (`libs/core/.../kosinski.hpp`; Sega CD behaviour unchanged). The destination may be the sound RAM or a constant work-RAM buffer later consumed by a counted copy into the sound RAM (the two-stage
   loader of the Sega-family drivers, e.g. Streets of Rage); the buffer is discarded by any store that may touch it. A different routine is
   `decompression_unproven`, not a guess. This is a
   *format-and-loader-contract* recognition (the digest identifies an algorithm used by many titles, not a title); it is deliberately a closed,
   two-entry catalogue, not a signature scanner, and adding a form requires evidence of use as a Z80 loader.
5. **The prediction is verified by the existing machinery, and a contradiction is never absorbed.** The first fixed point runs with the preloaded
   registry. The confirming run must activate exactly the registered images in registered order and (sound-fault rule) no image after an isolated
   sound CPU; otherwise the static registry is discarded and the unchanged fixed point runs (`z80.static.fallback` names the reason). The
   production is therefore never the *only* route, `--z80-static 0|1` selects it, and a wrong prediction costs work, never correctness.
6. **Provenance is explicit.** Static images are `static_proof` / `genesis.z80_static_producer` with evidence `derivation=static_reconstruction`
   and `proof=rom_stores|rom_copy|kosinski`; they never claim `runnable_epoch_snapshot`. `registries_equal` stays strict; the new
   `compare_registries` names the one legitimate difference (equivalent modulo carry-over) explicitly.
7. **Completeness is the verifier's claim, not the walk's.** `complete` means no Z80-affecting construct was refused up to the walk's horizon.
   Whether a later epoch exists is established by the verification run: an unknown later epoch is discovered by the unchanged fixed point
   (outcome `partial` in practice, one extra generation), never silently ignored.

## Soundness argument

* A produced epoch is the exact state the runtime would produce *if* the walked path executes: the model replays the runtime's rules, and every byte
  of the window enters through a mechanism whose source is constant ROM and whose destination is a constant RAM range.
* Paths the walk cannot decide while the window is open are refused rather than forked (a fork would create infeasible paths whose epochs the
  program never produces). Outside an open window, forks are explored and traces are reconciled by the common-prefix rule.
* Wrong or unreachable predictions cannot reach the output: an epoch with a different content has a different activation signature, so the
  runtime reports it as unknown to the preloaded registry and the fixed point registers it; a predicted image that never activates, a different
  order, or an image after a sound fault is detected by the build and discarded.
* What is *not* proved and is therefore delegated to the existing guard: Z80-written carry-over bytes (the structural RAM-backed guard compares the
  executed code bytes at run time, as for observed images), asynchronous interrupt effects (any resulting difference is an activation difference,
  detected by the verifier).

## Falsification actually run (project-authored fixtures; real consumer route)

For 19 fixtures `--z80-static 1` is compared with `--z80-static 0`: generated Z80 C and registry, linked program, bounded-run sound/execution
summary and audio status are **byte-/value-identical in all 19**, including the fallback cases (never-activated image, region-gated upload,
image after sound fault) and the refused shapes (unknown count/source/destination pointer, beyond-RAM raw and decompressed output, unrecognised
decompressor, valid stream into work RAM, computed upload, structural mutation). Fully provable fixtures take exactly one Z80 compilation
generation, one discovery run, one verification run, zero reused units. The Kosinski fixtures execute the reference routine in the legacy build and
compare it to the producer's decode of the same stream: equal. Repeated builds with 1/4/8 jobs are byte-identical.

## Consequences

* New code: `z80_static_producer.{hpp,cpp}` (Genesis machine), `kosinski.hpp` (core, shared with Sega CD), `Registry` derivation/proof fields and
  `compare_registries`, `MaterializationSummary::activation_order/activation_epoch`, the `--z80-static` option, the `genesis-z80-static-report`
  CLI (sanitized aggregates), `status.json` `z80.static` and `z80.prepare_generations/verification_runs`.
* **Not changed:** the Z80 instruction set, the structural mutation policy (ADR 0134), Sega CD semantics and pipeline, the emitter, the runtime,
  the registry ABI and the generated code. Remaining M68K alias-preparation rounds are independent of this feature (they re-run the same fixed
  point on the same registry and are neither reduced nor increased by it).
* **Default enablement** is not claimed by this ADR: it is decided from the measurements recorded in `docs/testing/genesis-z80-static-first-measurements.md`.
* Limits are constants (no silent expansion): 1,500,000 steps, 400,000 nodes, 64 contexts, call depth 8, 16 epochs, 96 exact visits per node,
  48 individually executed non-constant stores per window. Titles that exceed them get `state_bound`/`step_bound` and a `partial`/`resource_bound`
  result, with the unchanged fixed point finishing the work.

## Rejected

A generalized executable-image IR; an M68K emulator or abstract interpreter (the walk knows exact constants or nothing); a universal compression
scanner or a runtime decompressor; Saxman/Nemesis without evidence of use as a Z80 loader; porting SEG-112's transform idioms (no Genesis evidence);
reading Z80 RAM written by the Z80 as input; trusting a successful decode as authority; weakening equality to make a comparison pass.
