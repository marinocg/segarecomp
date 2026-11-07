# Genesis Z80 static-first image production

Status: implemented (ADR 0277), opt-in (`segarecomp build --z80-static 1`), Genesis only. The build-time materialization fixed point of ADR 0073
is unchanged and is both the verifier and the fallback.

## What it does

`segarecomp build` of a Genesis image normally discovers each Z80 executable image by running the generated program until an unknown
activation signature appears (run, capture the 8 KiB snapshot, compile the image, relink, rerun; then one confirming run). With
`--z80-static 1` the build first asks a small producer (`platforms/genesis/machine/src/z80_static_producer.cpp`) to reconstruct the images whose
causal upload is provable from the immutable cartridge bytes alone. The produced epochs enter the **same** `Registry` before the first Z80
emission; the existing emitter compiles them in one generation; the existing verification run must then observe exactly the produced activation
signatures in the produced order. Anything the producer cannot prove is left to the unchanged fixed point; any disagreement discards the static
registry and the build runs the unchanged fixed point.

```
cartridge bytes ──> bounded M68K walk ──> ordered Epoch records ──> Registry (preload, derivation=static_reconstruction)
                                                                      │
                  existing: emit_registry ──> one Z80 compile generation ──> pass program ──> verification run
                                                                      │
        activation signatures == registry, same order?  yes: keep   no: discard, run the legacy fixed point
```

## Feasibility audit (recorded with the implementation; falsifiable)

1. **Which parts of the upload are statically recoverable?** The producer needs, per epoch, the BUSREQ/RESET writes with constant values, and
   every byte stored into the window by a constant-valued store, a counted ROM copy or a recognised decompressor call. On the 64 locally held
   images (aggregates only, `docs/testing/genesis-z80-static-first-measurements.md`): the Sega-standard boot stub (a `MOVEM` table load from
   the ROM followed by a 38-byte `MOVE.B (An)+,(Am)+ / DBRA` copy) is recovered in every title that uses it; complete multi-segment drivers
   assembled from several ROM extents are recovered where each segment is a constant counted copy; Sonic 1's driver is recovered through its
   Kosinski call.
2. **SEG-112 versus Genesis-specific.** SEG-112 (ADR 0256) analyses Sega CD *sub CPU* boot images and models CD interface state; it has no Genesis
   Z80 bus/RAM semantics (BUSREQ/RESET latches, the hold window bitmap, the mirror, the word/long store byte rule). It is reference for the
   method only (decode/lift/effects API, exact-constant propagation, counted-loop idiom, refusing instead of guessing). The Genesis producer is
   its own file; nothing was moved into a generic library. Genesis-specific: the epoch tracker rules, the I/O power-on reads, BUSACK reads.
3. **Boot stub and full driver before execution?** Yes where the walk reaches them with constants: measured per title; the unprovable ones are
   typed refusals (below).
4. **The exact M68K-written extent bitmap of S1\*?** Yes, by replaying the runtime's own rules (`genesis_z80_bus_write`,
   `genesis_z80_epoch_note_ram_write`): a BYTE store marks the byte, a WORD store keeps and marks only its high byte, a LONG store is two WORD
   stores (bytes +0 and +2), mirror offsets wrap `& 0x1FFF`, the window is cleared at every runnable transition, an epoch is the first runnable
   transition since the last `/RESET` release. The registry then computes the content hash and the signature exactly as for an observed epoch.
5. **Compressed formats in qualified workloads.** Only Kosinski (Sonic 1's driver, ADR 0135/0089; the same routine form is present in 9 of the 64
   local images, used for graphics as well). No other compressed Z80 upload is evidenced; Saxman and others are not implemented.
6. **Reusable decoder.** The Sega CD Kosinski decoder was extracted unchanged into `libs/core/include/segarecomp/core/kosinski.hpp` (header only);
   `epoch_recipe.cpp` calls it (same error taxonomy, same bounds). No toolchain was imported.
7. **What still depends on execution or dynamic inputs.** Z80 writes to its own RAM between epochs (carry-over bytes), asynchronous interrupts
   (an interrupt handler that toggles BUSREQ during an upload), work-RAM-driven control flow (a game mode that selects the driver), the console
   region (the version register is unknown to the walk), controller input, structural Z80 self-modification, and *exhaustiveness* (whether a
   later epoch exists beyond the walk's horizon).

*Decoding is not authority.* A successful decode proves only that a byte string exists. The producer additionally requires that (a) the routine
called is a recognised decompressor form, (b) the stream address is a constant inside the immutable ROM image, (c) the destination is a constant
inside the 8 KiB RAM while the 68K provably owns the bus, (d) the result is stored through the runtime's byte-store rule, and (e) the write is
followed by the runnable transition that defines an activation epoch. A valid stream expanded into work RAM contributes nothing.

## The walk

A deterministic exploration of the 68000 program from the reset vector over `(pc, Z80 context)` nodes, using only the CPU-owned decode, lift,
control-successor and effect-footprint APIs (`libs/cpu/m68k`). It is **not** an emulator or an abstract interpreter: register values are exact
constants or unknown, flags are tracked only where a constant decides a branch, memory is read only from the immutable ROM (and two modelled
device reads), and anything else is unknown.

* **Z80 context** (merged only when equal): the 8 KiB RAM model and the hold-window bitmap, the BUSREQ / `/RESET` / pristine latches, the shadow
  call stack, the epochs produced so far. Power-on state per the contract: RAM zero, `/RESET` asserted, no request, pristine.
* **Registers** are explored exactly per node (a counted loop with constant operands unrolls) until the node has seen 96 distinct states; then it
  widens (the differing constants become unknown).
* **Mechanisms that may write the window** (everything else that could is a refusal, see below): constant-valued `MOVE`/`CLR` stores; the counted
  copy `MOVE.B (As)+,(Ad)+ ; DBRA Dn` with constant `As` (ROM), `Ad` (RAM) and `Dn`; and a `BSR`/`JSR` whose target is a **recognised Kosinski
  routine** with constant `A0` (ROM stream) and `A1` (RAM destination). Recognition is the SHA-256 of the routine's code bytes from its entry to
  its first `RTS`, position independent by construction (short branches, no absolute operand). Two forms are recognised: the project-authored
  *reference* routine (listing below; the fixture builder assembles it) and the *library* form shared by qualified titles (digest only; no code
  from any title is stored). The Kosinski decode itself is the shared public decoder. The destination is the sound RAM, **or a constant work-RAM
  range** ("staging", the loader shape of the Sega-family drivers: expand into a work-RAM buffer, then a counted copy into the sound RAM): the
  decoded bytes are remembered as the context's *staged buffer*, a function of immutable ROM bytes only, and a later counted copy whose source
  lies wholly inside that buffer is an admitted source (proof class `kosinski`). Any store into the buffer, any store through an unresolvable
  pointer, any block store and any call of an unrecognised routine discards the buffer first (`Walker::touch`), so a clobbered buffer is
  `source_not_immutable`, never a guess.
* **Epoch list.** The walk records every runnable-transition epoch; plain restarts (empty hold window) of a bound image are epochs of the list and
  re-bind in the registry exactly like observed ones.
* **Soundness rules inside an open window** (68K owns the bus): a branch or `DBcc` with a non-constant outcome, a store through an unknown pointer,
  a store of an unknown value, a call of an unrecognised routine, or more than 48 individually executed non-constant stores are refusals that end
  that path. A non-constant store inside a backward-branch span the walk observed invalidates its epoch (`loop_unproven`).
* **Multiple paths.** Each context's epoch trace is collected; the claimed list is the longest trace on which every other trace agrees. A
  divergence is `ambiguous_epoch_order` and only the common prefix is claimed.

### Typed result

`complete` (non-empty ordered list, no refused Z80-affecting construct), `partial` (a proven list plus at least one refusal or a bound),
`none` (the walk finished and proved nothing), `unsupported` (unreadable entry or ambiguity with nothing agreed), `resource_bound` (a bound ended
the walk before any epoch). Refusal classes: `bus_write_value_unknown`, `z80_store_value_unknown`, `z80_store_unknown_address`,
`z80_store_form_unsupported`, `length_unknown`, `source_not_immutable`, `destination_out_of_ram`, `loop_unproven`, `decompression_unproven`,
`decode_failed`, `window_control_unknown`, `ambiguous_epoch_order`, `image_bound`, `call_depth`, `state_bound`, `step_bound`, `entry_unreadable`.
`complete` is *relative to the walk's horizon* (dynamic control flow, unresolved returns, the bounds). The producer never claims there is no later
epoch; the verification run does.

Bounds are constants (no options): 1,500,000 steps, 400,000 nodes, 64 contexts, call depth 8, 16 epochs, 96 exact visits per node. They were
fixed before the measurements, the context bound was raised once from 24 to 64 after Sonic 1 needed 22; no bound was changed afterwards.

## Verification and fallback (`apps/segarecomp/build_command.cpp`)

The static registry is a *prediction*. After the first fixed point the build requires: the confirmed run activated exactly the registry's images
(no registered image unactivated, none unregistered), in the registry's ordinal order (first activation, restarts of a bound image excluded), and
no registered image is first activated after an isolated sound CPU's fault epoch. Otherwise (and when the pass fails for any reason other than a
compile failure or an exhausted budget) the build logs `z80 static CONTRADICTED ... (<class>)`, replaces the registry with an empty one and runs the
unchanged fixed point; `status.json` records `z80.static.fallback`. A genuinely nondeterministic input still ends in the typed
`materialization_nondeterministic` failure of the legacy path. Later M68K alias-preparation rounds are unchanged: they re-run the fixed point on
the same registry, so the registry is re-validated against every modified executable path.

## Provenance and identity

* Content hash, activation signature, ordinals, `executable_images()` and the generated code are the existing ones. `Image::derivation` is
  `static_reconstruction` for produced images: authority `static_proof`, producer `genesis.z80_static_producer`, evidence `derivation =
  static_reconstruction` and `proof = rom_stores | rom_copy | kosinski`; verification stays the RAM-backed `structural` guard. Observed images keep
  `bounded_build_time_materialization` / `runnable_epoch_snapshot`.
* `registries_equal` is unchanged and strict. `compare_registries` adds the explicit relation *equivalent modulo carry-over*: same count, ordinals
  and signatures (so equal extents' content), snapshots differing only outside the written extents because the Z80 may write its own RAM
  between epochs and the signature deliberately excludes those bytes. Executed code outside the extents is guarded at run time by the structural
  RAM-backed guard (`z80_code_mismatch`), exactly as for an observed image.
* A mixed registry (static prefix, discovered tail) has ordinals in registration order, which equals activation order only when the verification
  run says so; otherwise the whole static registry is discarded.

## Reference Kosinski routine (project-authored, recognised form "reference")

A0 = stream, A1 = destination; clobbers D0-D6/A2; returns by `RTS`; 210 bytes; position independent; one `RTS`; no subroutine. The descriptor
register D1 is refilled eagerly when its 16th bit is consumed (the stream layout of the format). Assembled with `m68k-linux-gnu-as -m68000`:

```
REFILL:   moveq #0,d1; move.b (a0)+,d1; moveq #0,d6; move.b (a0)+,d6; lsl.w #8,d6; or.w d6,d1; moveq #16,d2
GETBIT:   moveq #0,d0; lsr.w #1,d1; addx.w d0,d0; subq.w #1,d2; bne.s 1f; REFILL; 1:
kos:      REFILL
next:     GETBIT; tst.w d0; bne literal
          GETBIT; tst.w d0; beq inline
          moveq #0,d3; move.b (a0)+,d3          ; lo
          moveq #0,d4; move.b (a0)+,d4          ; hi
          move.w d4,d5; andi.w #$F8,d5; lsl.w #5,d5; or.w d3,d5; subi.w #$2000,d5   ; displacement
          andi.w #7,d4; bne short_count
          move.b (a0)+,d4; beq done             ; extra: 0 = end
          cmpi.w #1,d4; beq next                ; 1 = no copy
          bra copy                              ; copy extra+1 bytes (counter = extra)
short_count: addq.w #1,d4; bra copy             ; copy count+2 bytes
inline:   GETBIT; move.w d0,d4; add.w d4,d4; GETBIT; add.w d0,d4; addq.w #1,d4
          moveq #-1,d5; move.b (a0)+,d5          ; displacement = byte - 256
copy:     lea (a1,d5.w),a2
loop:     move.b (a2)+,(a1)+; dbra d4,loop; bra next
literal:  move.b (a0)+,(a1)+; bra next
done:     rts
```

## Unsupported (explicit)

Saxman/Nemesis/other formats; a decompressor whose code is not a recognised form (typed `decompression_unproven`); word/long element copies
into the window; computed (arithmetic-transformed) uploads (SEG-112's transform idioms are not ported); table-driven or pointer-chased loaders;
uploads performed from an interrupt handler or by the Z80 itself; drivers selected by work-RAM state or console region; images over the 8 KiB RAM;
more than 16 epochs / 8 images. None of these is an error: the unchanged fixed point handles them. The structural Z80 mutation policy
(ADR 0134 NO-GO) is untouched: a one-shot initial driver does not imply support for self-modifying code.
