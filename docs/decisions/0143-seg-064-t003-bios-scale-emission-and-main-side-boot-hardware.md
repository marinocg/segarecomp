# ADR 0143: SEG-064-T003 - BIOS-scale emission and main-side boot hardware of the BIOS-image profile; first G-A evidence

- Status: Accepted (SEG-064-T003). Builds on ADR 0142 (whose decisions 1-8 and gate G-M result are unchanged) and ADR 0141 (its
  historical `NO_GO` under the original ADR 0130 criteria is unchanged).
- Scope: the first G-A evidence of ADR 0142 - the sub runnable epoch reached and reproduced by generated-native execution of an
  authentic main BIOS. It does **not** claim authentic boot, application IP/SP handoff, sub-BIOS service execution, Sonic CD
  progress, SEG-064 completion or Checkpoint E.
- Privacy: no BIOS or disc byte, hash, internal address, routine inventory, trace or disassembly appears here; comparisons with the
  independent reference were made locally and are reported as normalized identity results and aggregates only.

## Context

ADR 0142 proved the mechanism (G-M) and measured that a single translation unit recompiled every round cannot carry an authentic BIOS
(65 K functions: 837 s / 4.3 GB per round; about 127 K expected). The main BIOS must also run, before it releases the sub, on main-side
hardware the profile did not model (ADR 0141 inventory: VDP ports and timing, Z80 bus request and write-only upload, I/O, Gate Array
including the H-INT vector register, the PRG-RAM window, the empty cartridge slot).

## Decisions

1. **Deterministic translation units.** `segarecomp sega-cd emit --bios ... --partition-window W` (W a power of two, 256 B-16 MiB)
   writes one translation unit per processor and non-empty PC window `[kW, (k+1)W)` - every compiled variant, operand template and
   live-byte selection chain whose PC lies in it, plus that window's PC switch - and a shared dispatch unit (entry tables, the
   compiled-entry lookups, now external, the per-processor window switch, disc tables, program). The CPU-owned lowering, exact PC
   dispatch, per-instruction guards, templates and image identities are unchanged; each function is defined in exactly one unit. A
   unit's text depends only on its own window, never on the program identity, so it is unchanged by an admission elsewhere. W = 0 is the
   single unit. Partitioning is not part of the program identity. Measured choice: **W = 8 KiB** (below).
2. **Content-addressed object reuse.** `tools/sega_cd_bios_build.py` compiles each unit (and each runtime source) to an object keyed by
   the SHA-256 of a cache version, the compiler's identity (`--version`, `-dumpmachine`, resolved path), the flags, the content of every
   file the compiler reports as a dependency (`-M`) of a probe unit including the runtime ABI header and of every runtime source -
   which reaches the CPU-owned exception/timing cores and the Genesis device contracts outside the runtime directories - and the
   unit's name and text; units compile in parallel from a private
   directory under their bare name (no host path enters an object) and link in the emitter's order. An unchanged unit is reused; a
   changed unit, flag, compiler or header rebuilds what depends on it; the shared unit rebuilds when its tables change.
3. **One admission per pass (kept).** A pass ends at its first stop: an epoch capture or a RAM frontier is a fail-closed stop and nothing
   after it runs, so a second observation in the same pass would require executing past an unadmitted image. Batching would need
   evidence a stopped pass cannot produce; the round cost is instead bounded by decisions 1-2. Admission order, the identity chain and
   the confirming run are unchanged.
4. **Main-side Genesis devices (BIOS-image profile only).** The main processor's I/O ports, Z80 BUSREQ/RESET lines, Z80 RAM window, PSG
   and the whole VDP window are served by the existing Genesis runtime device host (no second VDP, no second I/O model). Before every
   access the host's master clock is set to the machine time (500,000 ticks per master clock), so the VDP status flags and HV counter
   are the Genesis runtime's projections of the executed schedule. Device facts the Mega-CD changes are applied here: the version
   register's DISK bit (bit 5) reads 0 (expansion unit attached; GTO1 I/O section, GPGX `io_reset`), a word read of an I/O register
   returns its byte on both lanes (GPGX `ctrl_io_read_word`), BUSACK's unused bits are open bus (GPGX `mem68k.c`).
5. **The Z80 is not executed.** The BIOS's Z80 work before the epoch is a write-only RAM upload plus bus/reset control; nothing the main
   processor observes before the epoch depends on Z80 execution. Once the Z80 has been runnable (reset released, bus not requested), any
   main read of the Z80 area is the typed stop `main_device_unsupported`. What a running Z80 would do through its 68000 bank window
   (work RAM, VDP/PSG writes) is not modelled and not detectable here: a recorded deviation (for the authentic epoch, main work RAM
   matched the reference in every byte).
6. **VDP DMA.** Fill is the Genesis device's VRAM fill. A 68K->VDP transfer is fed from the Sega CD memory: the BIOS image is the host's
   read-only region and main work RAM is mirrored into the host before the arming command; a source in Word RAM or the PRG-RAM window
   (whose Sega CD DMA timing differs) is the typed stop `vdp_dma_unsupported`; copy stays fail-closed in the device. Transfers complete
   at the arming write (no DMA duration: contract section 12).
7. **Main interrupts.** The vertical-blank onset is a machine device event every frame. Following GPGX, the VDP's vertical interrupt
   becomes pending at every onset and requests level 6 while register #1 IE0 is set; the acknowledge clears it (and status F). The main
   processor accepts it through the CPU-owned exception core with the BIOS image's vectors; the level-4 vector is `$FFFF` : the H-INT
   vector register `$A12006` (GPGX `scd_reset` writes `$FFFF` over the boot ROM's `$70` and `$72` words and `$A12006` owns `$72`; reads
   of `$70-$73` return them; the register starts at `$FFFF`). Writing register #0 with IE1 (the H-interrupt) set is the typed stop
   `interrupt_source_unsupported` at that write: GPGX latches the line counter on active lines whatever IE1 says, and the counter is
   not modelled. A control word `101x xxxx` outside a pending command (a register write for GPGX, a command word for the Genesis
   device host) is the typed stop `vdp_access_unsupported` (correction cycle 2).
8. **Open bus (GPGX model, exact cases only).** A main read nothing answers - the empty cartridge slot `$400000-$7FFFFF`, unused areas -
   returns the instruction-stream word at the processor's prefetch position, its byte selected by A0 (GPGX `m68k_read_bus_8/16`: the
   word at the CPU's PC after the extension words consumed before the operand read; for a long read both halves). The emitter states
   that position per generated instruction (`cpu->prefetch`, from the decoded shape): the address after the instruction whenever its
   operand reads follow all of its extension words; for a MOVE that reads memory before fetching its destination's extension words it
   is "not established" and the read stays the typed stop `open_bus_read`, as do sub reads and every synthetic-profile read. No runtime
   decoding is involved. The empty slot ignores writes.
9. **Exception entry into uncompiled RAM (ADR 0142 decision 7, generalized entry point).** In the BIOS-image profile an exception or
   interrupt whose vector names RAM without a compiled entry is entered; the dispatch then stops `entry_not_compiled` at that PC before
   anything there runs - the same frontier a direct jump produces - so a build pass can admit a `JMP (xxx).L` thunk there as an
   operand template. The synthetic profile keeps `exception_vector_not_compiled` at the vector.
10. Generator version `segarecomp.sega_cd.bios_profile.v2` (generated instruction functions now state their prefetch position).

## Result (SEG-064-T003)

**Scale (gcc -O1, strict C11, 16-core host, 8 parallel jobs).** Per-unit cost by window on the authentic main image: 4 KiB 1.5 MB /
6.8 s / 188 MB; **8 KiB 3.0 MB / 8.2 s / 361 MB**; 16 KiB 6.0 MB / 20.7 s / 485 MB; 32 KiB 9.6 MB / 38.8 s / 716 MB (super-linear past
8 KiB, so 8 KiB is the default). Authentic BIOS-image program at the epoch: **126,497 instruction functions** (main 61,869 over the
128 KiB image, sub 64,628 over the 128 KiB epoch image), **116 MB of C in 33 units** (32 window units of 3,376-4,096 functions,
at most 4.3 MB, plus the shared unit); emission 7-8 s and 356 MB peak. Cold fixed point: round 0 compiles 17 units in 24 s, round 1
rebuilds 17 (16 new sub units and the shared unit) and reuses 16 in 53 s; **2 rounds + confirming run, 98 s in total; peak
compiler resident set 376 MB per process**. A warm rebuild compiles nothing (21 s, emission and the passes); the executable is
byte-identical to an independent build with another job count. Synthetic proof (2,427 functions): about 1 s per incremental round.

**Main-side hardware reached and implemented**, each forced by the runtime-selected frontier of the generated main BIOS (in order: an
I/O control-port read, then VDP DMA enable; the rest followed in the same batch from the same evidence): I/O version/controller ports,
Z80 bus request/reset with BUSACK and the write-only upload, PSG writes, VDP ports with live status timing, VRAM fill with its DMA
status, vertical blank and V-INT acceptance, the H-INT vector register, the empty-cartridge open bus. Each has synthetic positive and
negative tests (`tests/sega_cd_bios_hardware_test.py`).

**First G-A evidence: the sub runnable epoch (authorized US Model 1 class BIOS, local only).**

| check | result |
| --- | --- |
| generated-native main BIOS reaches the sub reset release from its reset vectors | **yes**, in the first build pass, with no input |
| captured through the bounded fixed point (ADR 0142 decision 3) and admitted (`bounded_build_time_materialization`) | **yes**: one epoch admission, round 2 verifies it, the confirming run reproduces the outcome |
| captured image vs the independent reference's PRG RAM at the sub's first instruction (Genesis Plus GX oracle of SEG-064-T001) | **identical**: same extent (128 KiB by the snapshot rule, the rest of PRG RAM zero in both), all bytes equal, reset SSP/PC equal; also equal to the reference images of its two-cartridge, START-timing and synthetic-disc runs |
| execution path to the release | the set of main instruction addresses executed is identical to the reference's (308 = 308), and every per-address execution count is identical except one three-instruction VRAM-fill completion poll (reference: the device's fill duration; here: fills complete at once, decision 6) |
| main work RAM | identical in all 65,536 bytes at the same point of the main instruction stream |
| runtime verification before the sub retires an instruction | **pass**: the sub's reset exception is taken from the verified bytes; an admitted image with one flipped byte, or one 256 bytes short, stops `materialized_image_unverified` with the sub having retired nothing; a different BIOS image is refused before any instruction |
| determinism | the build-pass capture is identical (bytes, extent, tick, main instruction count, device-log digest) unpartitioned and under 8 seeded deadline partitions; the final program's report is identical over 3 repeats, 10 seeded partitions and 2 fixed slicings; repeated builds give the same admission and the same executable |
| no reference data in the runtime | the program carries only the admitted image's length and SHA-256 (ADR 0142 decision 4); PRG RAM is written only by the generated main BIOS through the modelled bus; the reference is an oracle, not a producer |

**Remaining earliest authentic frontier (diagnostic only, after the epoch):** the sub BIOS, running from the verified image, stops on a
sub-side Gate Array register this profile does not model (the stopwatch) a few instructions after the release; the main processor's
next step there is the BIOS's level-6 work-RAM thunk (an operand-template frontier of decision 9). Both are SEG-064 sub-side/boot work,
outside this task.

**Independent review (correction cycle 1).** The adversarial validator contested three points, each corrected in this PR and tested:
the object key missed headers included from outside the runtime directories (now the compiler's dependency set; a copied-tree test
edits such headers); the H-interrupt enable was only checked at the frame onset (now a stop at the enabling write, also inside a long
write); the H-INT default vector followed the image instead of GPGX's `$FFFF` (now `$FFFF` for `$70` and `$72`). Re-run after the
corrections: same authentic result (extent, bytes, zero tail), 15/15 identical determinism runs.

**Recorded deviations.** VDP DMA has no duration (fills and transfers complete at the arming write); a poll for completion therefore
iterates once - the only per-address count difference observed. The Z80 is never executed and its bank-window effects are not modelled (decision 5). The open-bus model is GPGX's
(the reference) and applies only where the prefetch position is exact (decision 8). The H-interrupt line counter is not modelled.

## Consequences

- G-A remains open: boot behaviour after the epoch, the sub-side hardware, the application handoff and the services after it still need
  independent evidence. Checkpoint E is not claimed.
- The synthetic post-BIOS profile and every SEG-062/SEG-063 output are unchanged (generated C byte-identical on all 34 fixture cases);
  the SEG-064-T002 proof passes unchanged, including its Musashi reference.
