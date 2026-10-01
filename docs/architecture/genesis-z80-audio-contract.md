# Genesis Z80/audio integration contract (SEG-032-T001)

Audience: every SEG-032 implementer (T002-T011). This document freezes the observable behaviour of the Genesis Z80
sound subsystem, its materialization boundary and its audio artifact, with a citation for every hardware fact. Later
tasks implement it; they do not re-decide it. A fact that is still open is listed in section 13 with a bound, an owner
and a resolution method. It is never guessed in code: until the owner resolves it, the behaviour that depends on it
stops with a typed outcome.

Decisions: ADR 0072 (Z80 platform integration: map, bus, clocks, scheduling, interrupts), ADR 0073 (build-time
materialization and RAM-backed code identity), ADR 0074 (YM2612 selection, oracle, toolchain). CPU-owned semantics
stay with `docs/architecture/z80-cpu-contract.md`; the immutable-image rules of ADR 0058 and the owner structure of
ADR 0071 are unchanged for the Master System. The Master System contract
(`docs/architecture/master-system-machine-contract.md`) owns the platform-neutral PSG device it shares.

Scope: NTSC Genesis/Mega Drive, one console profile (the existing runtime's), one Z80 (`z80_run` generated native),
one YM2612, one PSG. PAL, Sega CD, 32X, Game Gear and Saturn are out of scope.

## 0. References

| key | source | locator / pin |
| --- | --- | --- |
| GTO1 | Sega, *Genesis Technical Overview* v1.00 (1991) | pp. 10, 76 §4 (Z80 bus), 77, 91 (Internet Archive PDF, cited by the SEG-007 compat policies) |
| MCD1 | Charles MacDonald, *Sega Genesis hardware notes* v0.8 | <https://gendev.spritesmind.net/mirrors/cmd/gen-hw.txt>; cited by link only |
| GPGX | Genesis Plus GX (ekeeke), `core/genesis.c`, `core/mem68k.c`, `core/memz80.c`, `core/system.c`, `core/vdp_ctrl.c` | `939ce4f045f981f89965f24780cef045cc5e52d7`; non-commercial licence, test-only |
| ARES | ares `md` (`apu/`, `cpu/io.cpp`, `vdp/main.cpp`, `opn2/`) | `4cb8d92b441557cb6bcaf133c4cbc7f6819b1122`; ISC, test-only |
| NUKED | Nuked-OPN2 (nukeykt), YM2612 | `335747d78cb0abbc3b55b004e62dad9763140115`; LGPL-2.1; test-only, never linked |
| YMFM | ymfm (Aaron Giles) `src/ymfm_opn.*`, `ymfm_fm.*` | `81aec25ccbb98f4873a255f7551ac4dadac59b4a`; BSD-3-Clause |
| SMOKE | `tests/genesis_z80_oracle_smoke_test.py` | black-box run of the project fixture `bus_reset_probe` through the GPGX libretro core |

Pinned checkouts live in the ignored product `.tools/genesis-oracles/` (SEGARECOMP_GENESIS_ORACLE_CHECKOUT); the test
skips cleanly without them and fails on a wrong or modified pin. GPGX and ares are the same trees as the SMS oracles. The
existing SEG-007 compat policy documents (bus arbitration, Z80 RAM window, PSG port, YM2612 status port) are a fifth,
non-authoritative source: they record what a no-Z80 runtime did, and section 12 retires them.

"Two sources" below means a primary document or an independent code reading (GPGX and ARES are independent
implementations) plus the SMOKE run. A row marked **U#** is open (section 13).

## 1. Clocks (NTSC)

| quantity | value | source |
| --- | --- | --- |
| master clock | 53,693,175 Hz | existing runtime constant `GENESIS_NTSC_MASTER_TICKS_PER_SECOND`; MCD1 |
| M68K | master / 7 | runtime `GENESIS_M68K_CYCLE_MASTER_TICKS`; GPGX `m68k.cycles*7`; ARES |
| Z80 | master / 15 = 3,579,545 Hz exactly | GPGX `Z80.cycles` in multiples of 15 (`gen_zbusreq_w`); ARES `system.frequency() / 15.0` |
| YM2612 clock | master / 7 | ARES `OPN2` thread `frequency / 7.0` |
| YM2612 native sample | one every 144 YM clocks = 1,008 master ticks (53,267.04 Hz) | ARES `stream->setFrequency(frequency/7.0/144.0)`; YMFM `sample_rate(clock) = clock / 144` |
| PSG tick | Z80 clock / 16 = 240 master ticks | SEG-009-T007 device (`divider = 16`); PSG is clocked by the Z80 clock |
| scanline | 3,420 master ticks; VBlank onset at line 224 of 262 | runtime constants; GPGX `MCYCLES_PER_LINE` |

All time is integer master ticks of the existing `GenesisInterruptScheduler.master_ticks`. Z80 time is
`state.cycles x 15`.

## 2. Z80 memory map (Z80 view)

Frozen against GPGX `memz80.c` and ARES `apu/bus.cpp` (two independent codings that agree); MCD1 as primary note.

| Z80 address | behaviour |
| --- | --- |
| `$0000-$1FFF` | sound RAM, 8 KiB |
| `$2000-$3FFF` | mirror of `$0000-$1FFF` (`address & $1FFF`) |
| `$4000-$5FFF` | YM2612: `address & 3` selects part I address / data, part II address / data; reads return the status byte; mirrors repeat every 4 bytes |
| `$6000-$60FF` | bank register, write-only; each write shifts bit 0 into bit 8 of a 9-bit register (`bank = (data&1) << 8 \| bank >> 1`), so nine writes select the 32 KiB bank (`address bits 23-15`); power-on value 0 |
| `$6100-$7EFF` | unused: reads return `$FF`, writes ignored by the references. **Here: typed stop `z80_view_unmapped_access`** (hardware behaviour is open-bus; no supported software needs it) |
| `$7F00-$7FFF` | window onto `$C00000 + (address & $FF)`: PSG at `$7F11` (odd `$7F11/13/15/17`), VDP ports otherwise. **Here:** PSG write supported (T006); every other access is the typed stop `z80_view_unmapped_access` until a supported workload needs it (U4) |
| `$8000-$FFFF` | banked 68K view: 68K address `bank << 15 \| (address & $7FFF)` |

Banked targets (68K address, 24-bit): cartridge ROM (`$000000-$3FFFFF`; read-only, served from the already embedded owned
cartridge region) and work RAM (`$E00000-$FFFFFF`, 64 KiB mirrored). **Supported: ROM reads and work-RAM writes.** Every other
target is the typed stop `z80_bank_target_unsupported`: a work-RAM *read* (GPGX serves it, ARES states "the APU can write to CPU RAM,
but it cannot read from CPU RAM": the two references disagree, so it is open fact U3), any Z80 access to its own RAM through the
window (`$A00000-$A0FFFF`), reads past the end of the embedded image, and everything else (hardware: open bus / contention).

Z80 access to the 68K bus costs ~3 extra Z80 cycles (GPGX `z80_request_68k_bus_access`: `3 * 15` master ticks; ARES
`step(3)`): modelled as **+3 Z80 cycles per `$7F00`/`$8000` window access, added to Z80 time** (the second source of the
agreement). The matching 68K stall (ARES `stolenMcycles += 68`; GPGX: none) is **not modelled** (the references disagree;
no supported software depends on it).

I/O ports (`IN`/`OUT`): unused on the Genesis (ARES `in` returns `$FF`, `out` ignored). Typed stop
`z80_view_unmapped_access` is not used for them: `IN` returns `$FF`, `OUT` is ignored (both references).

## 3. M68K view of the Z80 area

Reachable only while the 68K is granted the Z80 bus (section 4); otherwise the access is the typed stop
`genesis_68k_z80_area_without_bus` (hardware: contention / open bus; GPGX maps the area to open bus and ignores writes).

With the grant (GPGX `mem68k.c z80_read_byte/z80_write_byte`, ARES `cpu` bus):
- `$A00000-$A03FFF`: the same sound RAM and mirror as the Z80 view (`address & $1FFF`); byte access. A word write stores
  the high byte only; a word read returns the byte duplicated in both halves (existing T103 policy, matches GPGX
  `z80_write_word`/`z80_read_word`).
- `$A04000-$A04003` (mirrored through `$A05FFF` by `address & 3`): the same YM2612 instance as the Z80.
- `$A06000-$A060FF`: the bank register (write-only, same shift register).
- `$A07F00-$A07FFF`: VDP-through-Z80 window, a bus lock-up on hardware (GPGX `m68k_lockup`): typed stop.
- `$A08000-$A0FFFF`: undefined: typed stop `genesis_68k_z80_area_without_bus` is not used; the access is
  `z80_view_unmapped_access` (GPGX aliases it to the low half; hardware behaviour is not documented).

## 4. BUSREQ / RESET (`$A11100` / `$A11200`)

Registers are 68K word/byte registers; only D8 of the word (bit 0 of the even byte) is architectural (GTO1 p.76 §4;
existing policy documents). Writes use the existing byte-lane rules of the SEG-007 policies; this task pins only the
semantics:

1. **Power-on.** The Z80 is held in reset (`/RESET` asserted), BUSREQ is not requested, the bank register is 0, Z80 RAM is
   zero filled (hardware: undefined; **project decision**: the build and the runtime use zero; software that depends on
   uninitialized Z80 RAM is outside the supported set). Sources: GPGX `zstate = 0` at reset, ARES `resLine = 0`.
2. **Reads.** `$A11100` D8 = 0 iff BUSREQ is asserted **and** `/RESET` is released ("bus granted"); otherwise 1. It is 1
   while only BUSREQ is asserted with `/RESET` low (GPGX `mem68k.c`: "Check if bus has been requested and is not
   reseted"; ARES `busgrantedCPU() = resLine & busreqLatch`; SMOKE row RA). `$A11200` read: 0 / open (existing policy).
3. **BUSREQ assert.** If the Z80 is runnable it executes through its current instruction and stops at the first
   instruction boundary whose Z80 time is at or after the request time (GPGX `z80_run(cycles)` at the request); the grant
   is then visible. If `/RESET` is low the request is recorded but not acknowledged until `/RESET` is released.
4. **BUSREQ release.** The Z80 resumes from its exact state at `ceil(request_release_time / 15) * 15` master ticks (GPGX
   `Z80.cycles = ((cycles + 14) / 15) * 15`); time spent held is not made up.
5. **RESET assert.** The Z80 stops (it first runs to the assert time if runnable), the YM2612 is reset (GPGX
   `fm_reset` on assert and release; ARES on release), an ongoing grant is dropped. Z80 RAM is not cleared.
6. **RESET release.** The Z80 is reset to the architectural reset state of `z80_reset` (PC 0, `AF = SP = $FFFF`, I = R = 0,
   IM 0, interrupts disabled; ARES `Z80::reset`, GPGX `z80_reset`), pending INT cleared, the YM2612 reset, the bank register
   is **not** changed (GPGX keeps `zbank`), and the Z80 is **pristine** until it executes its first instruction.
7. **Runnable.** The Z80 executes only while `/RESET` is released and BUSREQ is not asserted (GPGX `zstate == 1`; ARES
   `main()` stall condition `!resLine || busreqLatch`).
8. **Reset while BUSREQ is asserted.** Asserting then releasing `/RESET` with BUSREQ held resets the Z80 and YM2612 but does
   not run it; the grant returns when `/RESET` is released (GPGX `zstate == 3`; SMOKE row R7).
9. The 68K never reads Z80 RAM written by the Z80 with a stale view: every 68K access to Z80-domain state first
   synchronizes the Z80 to the access time (ADR 0072).

## 5. Epoch: the transition to a Z80-runnable state after a reset/upload epoch

An **image epoch** begins at the *runnable transition*: the instant at which rule 7 becomes true while the Z80 is
**pristine** (rule 6: reset since its last instruction). Any order of the 68K operations produces it:

- `BUSREQ=1, RESET release, upload, RESET assert, BUSREQ=0, RESET release` (the sequence of GTO1 p.91 and of the SMOKE
  fixture): the transition is the final RESET release (BUSREQ already 0);
- `BUSREQ=1, RESET release, upload, BUSREQ=0` with the Z80 still pristine from the earlier release: the transition is the
  BUSREQ cancel;
- a plain bus hold and release of a Z80 that has already executed instructions is **not** an epoch (it resumes);
- uploads with the bus held but no reset are not epochs; a later difference between compiled and live instruction bytes is
  the guard's concern (§6).

The materialized snapshot is the complete 8,192-byte sound RAM at that instant. The predicate is observable from the
`/RESET`, BUSREQ and `pristine` state alone: no cartridge scan, no title knowledge, no 68K code analysis.

## 6. RAM-backed code rule

Before each generated instruction executes, its static instruction bytes (1-4) must equal the live RAM bytes at the
(mirrored, wrapping) fetch addresses. A difference stops the Z80 with the typed outcome `z80_code_mismatch`, `state.pc` at
the instruction start, no state change. The Z80 code window is `$0000-$3FFF` (two mirrors of the 8 KiB); an instruction
whose fetch leaves the window is a generation-time typed stub (`mutable_code`). No self-modifying code, no mutable
immediates or displacements and no runtime decoding of replacement bytes are supported.

## 7. Content hash and activation signature

Two separate quantities are derived at the runnable transition:

- **Content hash** `H = SHA-256(tag "segarecomp.genesis.z80.image.v1", u32le window_length = 0x4000, u8 mirror_count = 2,
  the 8,192 snapshot bytes)`. It names the compiled image, orders the registry by first activation and makes builds
  reproducible. It is a build artifact, never selection authority at runtime. (Measured on the authorized workload: it
  varies with unrelated carry-over data and with the power-on fill, which is why it cannot select.)
- **Activation signature S1\*** (proved by SEG-032-T002, ADR 0073), the runtime selection key: `SHA-256(tag
  "segarecomp.genesis.z80.signature.v1.extents", then for every maximal run of Z80-RAM bytes the 68K wrote during the
  *hold window*: u16le offset, u16le length, the run's bytes at the transition)`, runs ordered by offset and merged when
  adjacent. The **hold window** is the time since power-on or since the previous runnable transition (an epoch *or* a plain
  resume), i.e. while the Z80 was not executing. Consequently it contains the uploaded image and anything else the 68K
  wrote in the same hold, and it excludes: 68K command/mailbox writes made in earlier holds (before the Z80 last ran),
  every Z80-written byte, and the power-on fill. A write that stores the byte already present still counts as written.
  **Empty hold window** (a plain Z80 restart: reset pulse and release with no 68K write to Z80 RAM): the signature is the one
  bound by the previous epoch (the Z80 restarts the code already in RAM); at the first epoch an empty window means the
  zero-filled RAM, which is itself materialized as an image. (Observed on an authorized workload: a restart epoch with
  zero written bytes occurs between two uploads.)
  The matching rule: the registered image whose signature equals the computed one is bound. Two epochs with the same
  signature are the same image *by definition*, even when their snapshots differ in carry-over data; if the differing bytes
  were executed code, the RAM-backed guard stops the Z80 with `z80_code_mismatch` (in the materialization pass: the build
  fails with that typed outcome, the signature being under-determined for the workload).
- **Rejected definitions (T002 negative controls):** the whole 8 KiB RAM (varies with carry-over and fill: five images
  where three suffice); "writes since the last /RESET assertion" (the documented upload sequence asserts `/RESET` after
  the upload, so the set is empty and every image collides); clearing the window only at an epoch (earlier holds' command
  writes leak into later signatures); the instruction-footprint alternative S2 (bytes reachable from the hardware entry
  points by static flow) is not adopted: it needs a control-flow analysis whose under-approximation (indirect jumps) would
  select an image whose live bytes differ in code reached only indirectly, turning a selection question into a guard
  failure. It remains the documented fallback if a supported workload ever uploads in a way S1\* cannot see (for example
  an upload performed by the Z80 itself, which is outside the supported set).
- **Known limit:** the signature is input-independent only if the 68K's writes during the final hold are. Anything the
  68K stores in the same hold as the upload is part of the image's identity by design.

## 8. Interrupts

- **INT.** Asserted at the start of the VBlank onset line (224) and cleared at the end of that line (3,420 master ticks,
  228 Z80 cycles) (GPGX `system.c` "Z80 interrupt is cleared at the end of the line"; ARES `vdp/main.cpp vedge()`: cleared at every edge,
  set at the VBlank edge). The line is level, not latched: a Z80 that is not runnable at that time misses the pulse; a reset
  clears it.
- **Acknowledge byte** `$FF` (GPGX `z80_irq_callback` returns -1): IM1 ignores it (RST 38h); IM2 vector low byte `$FF`; IM0
  `$FF` = RST 38h (admissible by ADR 0056).
- **NMI** is not connected.

## 9. PSG and YM2612 access

- PSG: 68K byte write at `$C00011/13/15/17` (existing region), Z80 write at `$7F11/13/15/17`; both write one
  `Sn76489` instance; reads are not supported (lock-up on hardware: typed stop). Reset: power-on only (neither reference
  resets the PSG on Z80 `/RESET`). Clock: Z80 clock, timestamps `master_ticks / 15`. Variant: the library default (Sega 16-bit LFSR,
  taps `$0009`).
- YM2612: one instance reached by the Z80 (`$4000-$5FFF`) and by the 68K while it holds the bus (`$A04000-$A04003`);
  status read at any of the four addresses (ymfm: busy bit 7, timer B bit 1, timer A bit 0). Reset on `/RESET` assert and
  release. Timers are driven in master ticks (§11, ADR 0074).
- **68K port semantics (confirmed by T007).** BYTE read and BYTE write only, at all four of `$A04000-$A04003`; every port reads the same
  status byte; WORD and LONG accesses fail closed (typed stop), as does any access while the Z80 does not yield the bus.
- **Device provenance and tolerance (T007).** Production is vendored ymfm (BSD-3-Clause, pinned, hash-manifested). The independent oracle is
  Nuked-OPN2: streams must agree after gain and integer-lag alignment within NRMSE <= 0.12 (DAC <= 0.05). Busy (192 input clocks) and timers
  are host-owned in master ticks (1 YM input clock = 7 master ticks).
- **Build.** The C++ core is linked without a C++ runtime (`cxx_runtime_shim.c`); the packaged Zig keeps only the libc++/libc++abi headers.
- **Device time.** Each device keeps a monotonic clock. A write from the other CPU whose timestamp is earlier than the
  device's clock (the Z80 may overshoot an M68K sync point by less than one instruction) is applied at the device clock
  (no reordering, no rewinding). One rule for both devices.

## 10. Scheduling and ordering (summary; ADR 0072 has the full rule)

The M68K retirement hook advances `master_ticks`; afterwards the Z80 is synchronized to it (instruction-boundary stop,
overshoot < 1 instruction, never moves backwards). Before any 68K access to Z80-domain state (Z80 RAM, `$A11100`, `$A11200`,
YM2612, PSG, bank register) the Z80 is synchronized to the 68K's current time. Same-tick order: the 68K access happens
before the Z80 instruction that begins at the same tick. Intermediate synchronization never changes the instruction
sequence, device timestamps or final digests (sync-cadence invariance).

## 11. Audio artifact

| item | frozen value |
| --- | --- |
| output sample rate | 44,100 Hz |
| channels | 2, interleaved L R |
| sample | signed 16-bit little-endian |
| window of output sample k | master ticks `[ceil(k x 53,693,175 / 44,100), ceil((k+1) x 53,693,175 / 44,100))` |
| decimation | integer box filter: the mean (truncating division) of every native sample whose **start** lies in the window, per source |
| YM2612 source | ymfm native stereo samples, one per 1,008 master ticks, signed 16-bit range |
| PSG source | `Sn76489` ticks (240 master ticks): mean level scaled to `0 .. 8192` as `mean x 8192 / 131,068` (unipolar, equal in L and R); no DC removal |
| mix | `clip16(ym + psg)` saturating at -32768/+32767; the number of clipped samples is counted |
| empty window | a window containing no native sample of a source repeats the last decimated value of that source |
| silence | the YM2612 idle output is the chip's DAC discontinuity offset, not zero (measured on both Nuked-OPN2 and ymfm by `genesis_ym2612_oracle_smoke_test`), so a silent run is a *constant* stream, not zeros; "non-silent" means the stream takes at least 2 distinct values with a non-trivial variation (T009 fixes the metric) |
| digest | SHA-256 over the canonical little-endian sample bytes (L then R per frame) followed by `u64le frame_count`; independent of chunk, frame or synchronization boundaries |

The gains are project decisions (not hardware claims); T009 may change them only with a cited reference and an ADR note.
The presentation (SDL3) layer follows ADR 0070's queue/overrun/underrun/mute policy and never feeds anything back.

## 12. Retirement list (owner task in brackets)

| obsolete behaviour | where | retired by |
| --- | --- | --- |
| BUSREQ immediate grant ("policy (a)") and reset-only latch ("policy (c)") | `genesis_z80_bus_access`, `docs/architecture/genesis-z80-bus-arbitration-compatibility-policy.md`, `genesis_startup_runtime_z80_bus_test.py` | T005 |
| "no Z80 exists" in the Z80 RAM window policy; flat RAM without a Z80 view | `genesis_z80_ram_window_access`, `genesis-z80-ram-window-compatibility-policy.md`, `genesis_startup_runtime_z80_ram_test.py` | T004/T005 |
| PSG latch model `GenesisPsgState`, `genesis_psg_access` | runtime.c/h, `genesis-psg-sn76489-port-write-compatibility-policy.md`, `genesis_startup_runtime_psg_test.py` | T006 |
| YM2612 status-port-only policy | `genesis_ym2612_access`, `genesis-ym2612-status-port-byte-read-compatibility-policy.md`, `genesis_startup_runtime_ym2612_test.py` | T007 |
| device-state digests of `z80_bus`/`psg` and the matching independent oracle | `genesis_checkpoint_digest`, `tests/oracle/genesis/checkpoint_oracle.c`, `tools/genesis_device_divergence.py` | T005/T006/T007 |
| fail-closed lanes `UNSUPPORTED_DEVICE_REGION_{Z80_BUS,Z80_RAM,PSG,YM2612}` for supported accesses | `GenesisDiagnostic` | T005-T007 (kept only for unsupported accesses) |

No fake/real dual path remains after a task retires its row; a transitional seam is allowed only between T004 and T006/T007
(Z80 view reaches the compat YM2612/PSG until replaced) and is removed by the owning task.

## 13. Open facts (bounded, owned)

| id | fact | bound / decision until resolved | owner |
| --- | --- | --- | --- |
| U1 | exact YM2612 busy duration and timer reload alignment (ymfm leaves busy and timers to the host interface: `ymfm_set_busy_end`, `ymfm_set_timer`) | RESOLVED by T007 within the documented tolerance: the host owns the clock, both are implemented from master ticks and compared to NUKED; a write during busy is lost on the real chip (Nuked), so the Z80 drivers poll | T007 |
| U2 | the activation signature | RESOLVED by T002: S1\* (hold-window written extents), §7 | T002 |
| U3 | Z80 read of work RAM through the bank window (ARES forbids, GPGX allows) | typed stop `z80_bank_target_unsupported` for RAM reads; RAM **writes** admitted; reconsider only with a supported workload that needs it | T004/T010 |
| U4 | Z80 access to VDP ports through `$7F00` | typed stop except PSG writes | T006/T010 |
| U5 | audible mix ratio of PSG to FM on hardware | §11 constants; not claimed to be hardware accurate | T009 |
| U6 | PAL timing | out of scope, NTSC constants only | later milestone |

## 14. Forbidden in production configuration

Any of: a title or game id, a ROM hash selecting behaviour, a Z80 driver address or range, a decompressor name, a PCM or
sample range list, a manually produced Z80 RAM dump, recorded runtime coverage used as discovery authority, a per-game
image list. Image content hashes and signatures are allowed only as generated build artifacts derived from the input
ROM during the build. The identifier scan (`tests/genesis_z80_forbidden_identifiers_test.py`, T008) reads the
word list in `tests/fixtures/genesis-z80-forbidden-identifiers.txt` and covers the Z80/audio/materialization production surface
(runtime, machine, YM2612 device, Z80 codegen/CPU libraries, the build command, packaging); the existing per-ROM 68K compat hint files are
not part of it and must never describe the sound subsystem (the scan checks that too).

## 15. Fixture plan

`tools/genesis_z80_fixture_rom.py` (T001 introduces the builder, the manifest and the `bus_reset_probe`/`bus_reset_control`
fixtures; later tasks add): raw upload, M68K-loop "decompressed" upload, multi-epoch with dirty carry-over data, same Z80 PC
under two images, banked ROM reads, bank changes, unsupported bank target, YM2612/PSG writes from both CPUs, DAC streams, Z80
interrupt, BUSREQ while executing, RESET while executing, illegal 68K Z80-area access, mutated executable bytes. All
project-authored; none commercial.

## 16. Build-time materialization pipeline (T008)

`segarecomp build` for Genesis produces the final static program in one invocation (ADR 0073, T008 record):

1. Stable objects are compiled once: the generated M68K units, the runtime, the Z80 machine, the shared PSG, the vendored ymfm YM2612 with the
   C++-runtime shim (the C++ driver is `<cc> c++` derived from the C driver, or `--cxx`/`--cxx-arg`), and the sound attach point.
2. Fixed point over a registry that starts empty: emit the registry and the Z80 C in-process, compile only units with a new content hash, link the
   *pass program* (the same objects plus `genesis_materialize_hook.c`), run it from reset with no input under the frozen bounds: 600 virtual
   frames, 100 M dispatches, 120 s wall. An image epoch whose hold-window signature (section 7) is unregistered makes the program write the
   snapshot and stop (`z80_unknown_image`); the build registers it (images ordered by first activation) and repeats. The loop ends at the
   first run with no unknown epoch; one confirming run must reproduce the outcome class, frame count and ordered epoch identities.
3. The final program links the same objects with the production hook; every bound and nondeterminism violation is a typed build failure
   (`z80_image_bound_exceeded`, `materialization_budget_exhausted`, `materialization_nondeterministic`, `z80_image_compile_failed`,
   `materialization_no_convergence`, `z80_code_mismatch`, `z80_execution_unsupported`, `materialization_pass_failed`) with no executable.
4. `status.json` records the outcome, image/epoch/run counts, frames reached, unit counts, generated/object/executable bytes and stage times.
   `--keep-work 1` retains `obj/` and the emitted Z80 C for falsification tooling.

Epochs after the observation window, and epochs that depend on input, surface at run time as `z80_unknown_image`. Z80 code that modifies
its own bytes stops the pass with `z80_code_mismatch` (section 6): a workload with such a driver does not build until a contract amendment
defines how mutable operand bytes are modelled.
