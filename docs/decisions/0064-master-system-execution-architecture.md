# ADR 0064: Master System Execution Architecture, Artifacts, Audio Pipeline and Fixture Builder

- Status: Accepted
- Date: 2026-09-30
- Task: SEG-009-T001
- Related: ADR 0006 (build-time embedded cartridge data), ADR 0050 (+ amendment: a finite budget for
  automation), ADR 0058 (image identity and dispatch), ADR 0060 (Z80 generated runtime ABI), ADR 0061-0063,
  `docs/architecture/master-system-machine-contract.md` §10-§13.

## Decision

### 1. Generation time vs run time

- **Generation time (C++, `platforms/master-system/machine`):**
  - profile selection;
  - ROM validation (size, header class through `libs/media`);
  - mapper identity check (ADR 0061);
  - the mapper contract table -> `ImageSet` (ADR 0058 reference shape);
  - `emit_image_set`.

  All `SMS_ERROR_*` classes that can be decided statically (undeclared/unsupported mapper, ROM size, profile) are
  raised here, before any C is written.
- **Run time (C11, `platforms/master-system/runtime` + `libs/device/sega/psg`):** compiled from sources together
  with the generated code per game, as the Genesis runtime is (ADR 0050). It contains:
  - memory map and mapper;
  - port decode, VDP, PSG wiring, controllers;
  - the scheduler, run API, artifacts and the headless `main`.

### 2. ROM data at run time

Operand reads of mapped ROM (tables, `LDIR` from ROM) are served from ROM bank arrays **embedded at build time**
as `static const uint8_t` data in the generated program. This is the Genesis convention (ADR 0006,
`GenesisOwnedCartridgeRegion`: "never a pointer into the original ROM file"). The executable never opens or
hash-verifies the input at start, so a built game is self-contained and deterministic. The embedded bytes are
checked against the recorded input SHA-256 at generation time.

- Size is at most 512 KiB. That is negligible next to the ADR 0058 generated-code budgets (a 512 KiB map
  measured 340-368 MiB of C).
- Code is never fetched from these arrays. It is the compiled immutable image (integration contract §3).

### 3. Run API (C11)

```c
typedef enum { SMS_STOP_CYCLE, SMS_STOP_FRAME, SMS_STOP_HALT_IDLE, SMS_STOP_CYCLE_BUDGET,
               SMS_STOP_Z80_ERROR, SMS_STOP_PLATFORM_ERROR } SmsStopKind;
SmsStop sms_run_until_cycle(SmsMachine *m, uint64_t t_state);   /* resumable unless *_ERROR */
SmsStop sms_run_until_frame(SmsMachine *m, uint64_t frame);     /* stops at T >= frame * 59,736 */
```

- The scheduler calls `z80_run(rt, deadline)` with `deadline = min(next device event, requested cycle stop)`. Both
  are absolute T-states, which is the only deadline type the Z80 ABI has. The requested cycle stop is the smallest of
  the `run_until` target and the end of the cycle or frame budget.
- Memory and I/O accesses are ordered at their instruction-start T-state (contract §2, open fact U11).
- `deadline`, `halted` and `prefix_lock` are resumable and advance time. While halted, time advances to the next
  event; an interrupt wakes the CPU per the Z80 contract.
- A fail-closed `Z80Outcome` or an `SMS_ERROR_*` stops the machine **permanently**. The stop reports PC, image
  identity, T-state, class and offending value, and nothing resumes it.
- `SMS_STOP_HALT_IDLE` is a diagnostic: the CPU is halted with interrupts disabled and no NMI source configured.

### 4. Headless driver (generated `main`)

```
<game> [--cycle-budget <T>] [--frames <F>] [--input <script>] [--artifacts <dir>] [--frame-hashes]
```

- `--cycle-budget <T>` stops at the first instruction boundary with `cycles >= T` (overshoot at most one instruction).
  `--frames <F>` stops at `T >= F x 59,736`. Both map directly onto the absolute T-state deadline of `z80_run`.
- Automation always passes a finite `--cycle-budget` and/or `--frames`. This is the SMS form of the ADR 0050
  amendment's rule that automated callers must bound every run. The consumer default (neither option) runs until the
  guest stops, as the Genesis `main` does.
- There is **no `--instruction-budget`** for SMS. The Genesis budget comes from the runner's dispatch allowance.
  The Z80 ABI has no instruction counter, and statically direct-bound owners execute several instructions without
  returning to the dispatcher, so an instruction count cannot be turned into a deadline. A per-owner counter in the
  generated Z80 path would be complexity added only to mimic a CLI name. If instruction-count budgeting is ever
  genuinely needed, it is a deliberate, separate decision. Tooling that enforces a finite budget (the launcher, and
  the harness rule for agents) must accept `--cycle-budget`/`--frames` for SMS executables (T003/T010).
- Exit status:
  - 0: the `--frames` target was reached (`SMS_STOP_FRAME`), or the guest stopped normally with no bound given;
  - 2: the `--cycle-budget` was reached first, or it was the only bound (`SMS_STOP_CYCLE_BUDGET`). When both bounds
    fall on the same instruction boundary, the frame target wins (exit 0);
  - 3: fail-closed Z80 outcome;
  - 4: `SMS_ERROR_*`;
  - 64: usage.

### 5. Artifacts (written to `--artifacts`, all deterministic)

| artifact | format |
| --- | --- |
| `status.json` | stop kind, frames completed, T-state, error class/PC/identity when stopped (no instruction count: the Z80 ABI has none) |
| `state.sha256` | SHA-256 over the canonical machine-state serialization: Z80 state (ABI field order), RAM, mapper registers and cartridge RAM, VDP registers/VRAM/CRAM/latch/buffer/flags/counters, PSG registers/counters/LFSR, I/O control |
| `frames/<n>.bin` + `frames.sha256` | framebuffer artifact per frame: active area, 1 byte per pixel = 6-bit CRAM colour, row-major (contract §9.9); one SHA-256 line per frame |
| `audio.pcm` + `audio.sha256` | s16le mono 44,100 Hz PCM for the run, plus SHA-256 per frame range `[f0,f1)` |
| `irq.trace` | text lines `<T> <frame|line|pause> <asserted|accepted|deasserted>` |
| `vdp.trace` | text lines `<T> <reg <n> <value>|status <value>|vram <sha256-of-writes-since-last-line>|cram <index> <value>>` |
| `mapper.trace` | text lines `<T> <register> <value>` |

### 6. Audio pipeline

The PSG device steps in chip ticks (16 T). Each PSG write is applied at its `io_out` `cycles` value, the
instruction-start T-state (open fact U11), after the device catches up. Then comes the integer box-filter decimation to 44,100 Hz and s16le mapping of contract §10.

- The result depends only on guest T-states, never on host slicing. A test splits the same run at arbitrary cycle
  boundaries and compares the PCM digest.
- The ring buffer handed to consumers holds whole output samples with their frame range.

### 7. Viewer and headless share one guest loop

The viewer (T009) calls the same `sms_run_until_frame`. It only paces presentation at the rational frame period
(59,736 x 11 / 39,375,000 s), presents the frame artifact through the palette mapping, feeds the audio ring to an
SDL3 stream, and samples host keys **at frame boundaries** into the contract's scripted-input structure.

- Host time never enters guest state. Audio underrun or overrun is handled on the host side only.
- Recording the input stream and replaying it headless reproduces the frame, state and PCM digests.

### 8. Scripted input

Contract §11 format (`<frame> <p1> <p2> <pause>`, frame-stamped, events applied at the start of the frame). A pause
edge is a `-` -> `P` transition. Frame stamping is chosen over cycle stamping because the viewer can only sample at
frame boundaries and equivalence requires both producers to share one format.

### 9. Fixture builder

A stdlib-only Python builder, `tools/sms_fixture_rom.py`.
- It has an embedded two-pass assembler for the documented Z80 forms that fixtures use; unknown forms are hard
  errors.
- `tests/sms_fixture_rom_test.py` cross-checks every emitted instruction against the SEG-008 legal-form dataset.
- The builder writes the SMS Power! header and checksum, per-bank markers, and the mapper declaration manifest (a
  mapper identity declaration source).
- Generated ROM bytes are **regenerated at test time and never committed**. The committed
  `tests/fixtures/sms-fixture-roms.json` records each fixture's size, declared mapper and SHA-256, and `--check`
  proves byte-for-byte reproducibility on every host.
- A pinned third-party assembler was rejected: it would be a CI dependency with its own licence and version drift,
  for no capability the tests need.

## Consequences

- T003 implements §3-§5 and §8 (driver side). T004/T005 feed `vdp.trace` and the frame artifact. T006 consumes
  §8. T007 implements §6. T009 implements §7. T010 composes the build.
- Every artifact is hermetic and byte-stable, so T012's coverage ratchet and T013's completion gate can compare
  digests without re-running references in CI.
