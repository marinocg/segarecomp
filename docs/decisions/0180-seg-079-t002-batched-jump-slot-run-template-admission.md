# ADR 0180: SEG-079-T002 - batched `JMP (xxx).L` slot-run operand-template admission

- Status: Accepted (SEG-079-T002). Amends the template record of ADR 0142 decisions 3, 7 and 8 (and the operand-template row of the Sega CD machine contract). The
  static epoch producer of ADR 0179 is unchanged.
- Privacy: the authentic measurement below is aggregate only (counts, rounds, seconds); no address, table base, slot count of a specific table or
  byte of a commercial image appears.

## Context

ADR 0179 left 14 of the 15 recipe-assisted rounds of the authentic Checkpoint H build as operand-template rounds: ADR 0142 decision 3 admits one
record per build pass because a pass ends at its first fail-closed stop, and the sub and main initialisers build tables of `JMP (xxx).L` slots
(6-byte stride) by stores, so each slot is a separate frontier. Nothing static derives those slots (ADR 0179), but the **live bytes of the stopped
pass already contain the whole table**: when the pass stops at the first slot, the neighbouring slots have been written by the same initialiser.

## Decisions

1. **A template record may be a bounded slot run.** A frontier capture of a stop at a `JMP (xxx).L` slot (opcode word `$4EF9`, even operand)
   additionally reports the contiguous run of live slots (6-byte stride, each wholly RAM of the stopped processor, opcode `$4EF9`, even
   operand; at most 128 slots) that contains the stopped PC. The build driver trims the run so it holds no already admitted slot and admits it as ONE
   `template-run:<pass identity>:<cpu>:<first slot>:<slots>:<6 x slots live bytes>` record. The authority stays `bounded_build_time_materialization`,
   the verification `structural`, there is still exactly one admission per pass and the pass-identity chain (ADR 0142 decision 3) is unchanged
   (the run record is part of the program identity text as `templaterun`, a single record keeps its previous text, so every existing program is
   byte-identical). A run of one slot is an ordinary `template` record; `.W` slots, other opcodes and slots beyond a gap end a run and are admitted
   by their own later rounds exactly as before.
2. **Why over-admitting slots is sound.** The emitter expands a run into exactly the per-slot templates a sequence of single records would have
   produced: each slot has its own opcode-word guard (the instruction is compiled only for execution when the live opcode word is still `$4EF9`),
   reads its operand live, and may only transfer to a destination that is a compiled entry of the processor (else `entry_not_compiled`, the
   existing negative). An admitted slot grants no execution authority beyond the compiled destinations that the operand guard already allows for any
   admitted template; a slot the program never executes costs only generated code. A run that does not re-validate slot by slot (not `$4EF9`, odd
   operand, hole, outside the processor's RAM, past the end of RAM, over 128 slots, a byte count other than 6 x slots, overlap or duplicate of an
   admitted slot, broken provenance) is refused whole (`template_invalid` or a typed `admit` syntax error); the driver never falls back silently
   (`template_run_invalid`). The captured bytes are build-pass observations; the run-time guards, not the capture, remain the verification.
3. **Bounds.** ADR 0142 decision 8's 64 templates become 64 template *admissions* (rounds) and 256 template *slots* per program, 128 slots per run
   (the measured tables hold 30 and 48 slots, ADR 0179). The round bound 1 + 4 + 64 is unchanged. `--no-template-runs` restores one template per
   round for comparison.
4. **Not changed.** The shape set (`JMP (xxx).W/.L`), the per-instruction verification, timing, the Checkpoint H gate and frames, the epoch
   producers, no interpreter/JIT/runtime decoding, and no committed commercial-derived template list or schema (ADR 0179).

## Evidence

Synthetic (`sega_cd_template_run_test`, project-authored slot tables in `tools/sega_cd_slot_table_fixture.py`: a 24-slot contiguous table, a
second table with a `JMP (xxx).W` slot and a gap in front of its last slot): 29 conventional rounds -> 5 batched rounds, the same 28 admitted
slots, identical observable run reports and build-pass outcomes, deterministic admissions; typed run-time stops unchanged for a rewritten opcode
(`code_bytes_mismatch`), an uncompiled and an odd destination (`entry_not_compiled`); emitter refusals for a hole, a mixed opcode, a `JSR`, an odd
operand, outside RAM, an odd base, past the end of RAM, over the bound, a length mismatch, a single-slot run record, a broken pass identity and an
overlap; exactly 128 slots admitted at the bound. `sega_cd_bios_profile_test` and `sega_cd_epoch_recipe_build_test` (all their negatives) unchanged.

Authentic Checkpoint H configuration (same inputs, flags, input script, backup RAM and recipe as the ADR 0179 recipe-assisted build; one build,
cold object cache, 8 jobs, 16-core host; aggregates only). The conventional and recipe-assisted columns are the recorded ADR 0179 measurements.

| | conventional | recipe-assisted (ADR 0179) | recipe-assisted + slot runs |
| --- | ---: | ---: | ---: |
| generation rounds (+ confirming run) | 16 | 15 | **3** |
| operand-template admissions / slots admitted | 14 / 14 | 14 / 14 | **2 / 78** |
| compiled instruction functions (final) | 395,207 | 395,207 | 395,271 (+64 slot functions) |
| generated C | 399,251,233 B | 399,251,233 B | 399,308,405 B |
| wall time | 1,035 s | 920 s | **523 s (-43 % vs recipe-assisted, -49 % vs conventional)** |
| compile / emit / build-pass seconds | 747 / 211 / 63 | 645 / 200 / 60 | 451 / 40 / 18 |
| units rebuilt / reused (sum over rounds) | 131 / 1,425 | 125 / 1,344 | 101 / 192 |
| peak compiler RSS / peak emitter RSS | 856 MB / 1,229 MB | 855 MB / 1,228 MB | 856 MB / 1,228 MB |
| final outcome (both CPUs' registers and trace digests, all RAM/VRAM digests, result block, device log, BIOS-profile counters, ticks) | - | - | **identical to the recipe-assisted build** |

The two runs cover the two tables that ADR 0179 measured; the 14 slots the program actually jumps through are among the 78 admitted, the other 64 are
inert generated functions (guards never satisfied-and-reached at run time; no new execution authority). Single runs; no variance estimate. The
projection in the task ("about three rounds") held: one recipe round, one main run, one sub run, plus the confirming run. The remaining cost is
dominated by compiling the two admitted sub/main windows, not by discovery rounds. Checkpoint H gates and frames were not re-run (nothing about
timing changed); the outcome-digest equality above is the equivalence evidence, as in ADR 0179.

## Consequences

A future table of `JMP (xxx).L` slots costs one round instead of one per slot; a table interleaved with other opcodes still costs one round per
run. `bios_profile_identity` text for a run differs from that of the equivalent single records (a different provenance chain, as it must);
programs built without runs are byte-identical to before. Residual risks: the run is a build-pass observation of RAM contents (unchanged class of
trust: every slot is re-guarded at run time); a table longer than 128 slots takes several runs.
