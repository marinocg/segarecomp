# ADR 0062: Master System Independent References, Smoke Results and Comparison Policy

- Status: Accepted
- Date: 2026-09-30
- Task: SEG-009-T001
- Related: ADR 0057 (Z80 oracle policy, followed here), ADR 0061, `docs/architecture/master-system-machine-contract.md`
  (§15), `tests/sms_oracle_smoke_test.py`, `tests/sms_psg_oracle_smoke_test.py`.

## Question

Which public emulators serve as pinned, test-only, independent references for each SEG-009 subsystem? How are they
obtained, driven and compared? What happens where they disagree with each other or with the public documents?

## Candidates (evaluated at exact commits, 2026-09-30)

| candidate | commit | licence (test-only local use) | headless drive | injection / observation | determinism | independence |
| --- | --- | --- | --- | --- | --- | --- |
| **Gearsystem** (drhelius) | `704a92ebb702febc4c9c1dafc056339c09c4d5c4` | GPL-3.0: local test build, never distributed or linked into production | libretro core (`platforms/libretro`, builds in ~12 s); C++ classes `Video`, `Memory`, `Processor` also directly drivable | ROM program + pad/pause input; system RAM, framebuffer; `Video::WriteControl/WriteData/Tick/GetVRAM/GetCRAM/GetRegisters` for state injection | two runs byte-identical | independent author; PSG is Blargg's Sms_Apu (below) |
| **Genesis Plus GX** (ekeeke) | `939ce4f045f981f89965f24780cef045cc5e52d7` | non-commercial licence: redistribution may not be sold or used commercially; local non-distributed test use is permitted | libretro core (`Makefile.libretro`, ~10 s); explicit `master system II` hardware option | ROM program + input; system RAM; framebuffer; cycle-level VDP/PSG models with hardware-test notes | two runs byte-identical | derived from Charles MacDonald's SMS Plus (same author as the primary documents), independent of Gearsystem |
| **ares** (ares team) | `4cb8d92b441557cb6bcaf133c4cbc7f6819b1122` | ISC | the `ms` machine needs ares' scheduler/node system and has no libretro core; the SN76489 component (`ares/component/audio/sn76489`, 80 lines) builds standalone with the real nall headers | component: `write(byte)`, `clock()` per chip tick, full register/LFSR state | deterministic | independent; its SMS machine also documents post-BIOS values (`$C000 = $AB`, `$C700 = $9B`, SP `$FFFD`) |
| **Blargg Sms_Apu 0.1.4** (Shay Green) | the copy inside the Gearsystem pin | LGPL-2.1-or-later | standalone C++ (`Sms_Apu`, `Blip_Buffer`) | `write_data(time, byte)`, `run_until`; oscillator state (private; observed by the adapter) | deterministic | independent of ares and GPGX; *not* independent of Gearsystem's PSG |
| **MAME** `sms` / `sega315_5124` / `sn76496` | `8b80cfd15d79ff2a9e60681ecaedb1f6222f59e1` (sparse) | BSD-3-Clause per file | only inside MAME's device framework; no practical headless state injection without building MAME | none without the framework | n/a | independent; used as documentary corroboration only |
| **Mesen2** (SourMesen) | `b9fa69ddc6d0a331fb103fdb5eef6904305703c2` | GPL-3.0 | SMS core (`Core/SMS`) is bound to the Mesen emulator/UI/interop layers; no libretro core | none without the interop layer | n/a | independent; not a finalist |
| SMS Power! documents (MacDonald, Maxim) | pages read 2026-09-30 | citation only | n/a | n/a | n/a | primary public evidence; expectations are authored from them |

### Known-issues audit

- **Gearsystem.** 139 issues, 5 open. The only open SMS item is #199, a feature request (System E). Relevant
  closed issues: #33 (scanline rendering timing off by one line, fixed); #56 (sprite zoom); #100 and #143 (SG-1000
  noise and chip revisions); #125/#122 (YM2413, outside the profile). Source reading: IM0 is handled as `RST 38h`;
  IM2 reads `(I << 8) | $FF`; the interrupt line is a level. It has no separate SMS 1/SMS 2 VDP switch. Our checks
  exercise only behaviour documented as common to both, or as SMS 2.
- **Genesis Plus GX.** 493 issues, 23 open; none open is an SMS VDP/PSG defect (#514 concerns CD audio filtering).
  Closed SMS items include #398 (open-slot behaviour), #474 (Mark III noise in *Ys (J)*), #273 (NTSC crash in two
  titles), #406 (palette), #347, #659. Its `psg.c` records hardware verification on integrated PSGs: tone period 0
  behaves as period 1; the tone 2 attenuation register is latched at power-on (315-5313A/315-5660).
- **ares.** More than 200 open issues across all systems. SMS-relevant: #2269 (start button with run-ahead,
  frontend only). Closed: #1257, #1258, #828, #450, #400, #829, #403, #546. **New finding (T001):** the SN76489
  component ignores a data byte after an attenuation latch. SMS Power! states this byte "is NOT ignored" (the
  *Alex Kidd* sustained-tone case). There is no upstream issue.
- **Blargg Sms_Apu.** No tracker (inside Gearsystem). Source reading shows the three deviations below.
- **MAME, Mesen2.** Not finalists; MAME audited from source headers only; Mesen2 has its GitHub tracker disabled.

### Independent adversarial smoke checks (authored from MacDonald and SMS Power!, not from any reference's suites)

**Whole machine** (`tests/sms_oracle_smoke_test.py`). One project-authored 128 KiB fixture ROM (`oracle_smoke`,
declared mapper `sega`) runs black-box through each finalist's libretro core. That gives 47 checks per finalist:
- port decode and mirrors, `$00-$3F` reads, nationalization;
- VDP latch reset, first-byte low-address update, buffer loaded by writes, buffered read refill, address wrap, CRAM
  mirror;
- frame IRQ line `$C1` (polled and IM1), status clear, 12 line interrupts at V `$0F`/`$1F` for R10 = 15;
- IM2 via `$FF`, V counter jump `$DA -> $D5`;
- sprite overflow with nine transparent sprites, sprite collision;
- mapper reset values, masking, write-through, fixed first 1 KiB and slot 0 remap;
- pause NMI (hold then press gives exactly 2), UP bit;
- rendered tiles and the left-column blank;
- determinism (two identical runs), and a negative control (Japanese console must fail the nationalization
  checks).

| finalist | checks | pass |
| --- | --- | --- |
| Gearsystem | 47 | 47 |
| Genesis Plus GX | 47 | 47 |

Both finalists produce an identical result block. Status bits 4-0 read `%11111` on both. MacDonald documents
them as garbage, so `%11111` is adopted as a project convention only.

**PSG chip** (`tests/sms_psg_oracle_smoke_test.py`). 17 checks per reference: reset state, latch/data protocol
including the two SMS Power! data-byte cases, LFSR reset on noise writes, the white and "periodic" sequences for
the first 64/32 shifts, noise shift intervals for all four rates, tone periods 0/1/3 and the 2 dB attenuation
steps. Results: ares 14 pass + 3 masked; Blargg 14 pass + 3 masked; no unmasked failure.

### Disagreements and their resolution

| topic | observations | resolution (citation) |
| --- | --- | --- |
| data byte after an attenuation latch | Blargg updates; ares ignores | update: SMS Power! SN76489 ("The data byte is NOT ignored", Alex Kidd). ares deviation (mask) |
| noise output phase | GPGX and Blargg output bit 0 after the shift; ares outputs the bit shifted off (one shift later) | **open (U4, T007).** SMS Power!'s prose ("the bit that is shifted off ... is output to the mixer") supports ares; its reference implementation (`Output=ShiftRegister&1` after the shift) supports GPGX and Blargg. The model follows the implementation meanwhile; ares' phase is masked. Neither PSG reference in the smoke observes the chosen convention exactly (ares is masked on phase, Blargg on polarity); only GPGX, which is not in the PSG smoke, does |
| noise polarity | ares and GPGX: bit 1 = channel on; Blargg: bit 0 = on | bit 1 = on: SMS Power! mixer description ("the bit ... is output to the mixer", multiplied by the volume), GPGX. Blargg deviation (mask; SMS Power! "Output inversion") |
| tone period 0 | ares toggles every tick (= period 1); Blargg holds it static | **open (U10, T007).** SMS Power! states "If the register value is zero or one then the output is a constant value of +1"; GPGX ("zero value behaves the same as a value of 1 on integrated version"), ares and MAME toggle every tick. The model follows the references meanwhile, as a project decision that departs from SMS Power! and changes the PCM of sample-playback software. Blargg's static period 0 is masked |
| post-BIOS VDP registers | Gearsystem R0-R10 = `$36,$80,$FF,$FF,$FF,$FF,$FB,$00,$00,$00,$FF` (R1 `$A0` for some database titles); Genesis Plus GX writes R6 = `$FF` ("normally done by BOOT ROM"); ares seeds a RAM copy of R1 = `$9B` | **open (U9, T003/T011).** No public source; the contract uses the Gearsystem column as a project convention. Not exercised by the smoke |
| PSG latch before the first latch byte | GPGX: tone 2 attenuation (315-5313A/315-5660); ares: channel 0 tone | unresolved for the 315-5246: typed stop `SMS_ERROR_PSG_DATA_BEFORE_LATCH` (U5, T007) |
| post-BIOS SP and RAM | Gearsystem SP `$DFF0`, RAM `$00`; ares SP `$FFFD`, `$C000 = $AB`, `$C700 = $9B` | `$C000 = $AB` is documented (MacDonald §6); the rest is U1 (T011) |
| status bits 4-0 | both finalists `%11111` | MacDonald §4: "garbage values"; `%11111` is a project convention, not a hardware fact |
| in-line event offsets | Gearsystem and GPGX use different per-line cycle tables | U2 (T004): the event line is fixed; offsets are resolved only where both agree or a public source states them |

## Decision

1. **Primary references per subsystem.**
   - Memory map, mapper and ports, interrupts, VDP state/ports/status and raster/renderer: **Gearsystem
     `704a92e`**. Secondary: **Genesis Plus GX `939ce4f`** (configured as `master system II`, NTSC-U, BIOS
     disabled).
   - PSG chip: **ares SN76489 `4cb8d92`**, with a committed three-entry deviation mask (two behaviours). Secondary:
     **Blargg Sms_Apu 0.1.4** from the Gearsystem pin, with a three-entry mask.
   - Masks are exact: a masked check passes only when the reference deviates in the recorded way, and fails as
     "stale" when it starts agreeing.
2. **An oracle never defines hardware scope.** Where a reference cannot observe or disagrees with an in-scope
   behaviour, the contract follows the cited document, and the project-authored fixture corpus plus the second
   reference falsify it. Examples: in-line event timing (U2), H counter (U3), and PSG behaviours where each
   reference deviates somewhere. Exclusions come only from compatibility evidence (contract §1/§14).
3. **Public test-program corpus.** No third-party SMS test ROM is adopted in T001: no candidate's licence was
   verified as redistributable or test-usable here. The falsification corpus is the project-authored fixture set
   (ADR 0064), extended by later tasks. T012 may adopt public test programs after a recorded licence check.
4. **Acquisition.** Plain Git checkouts at the pinned hashes in the ignored product `.tools/sms-oracles/`
   (`drhelius_Gearsystem/`, `ekeeke_Genesis-Plus-GX/`, `ares-emulator_ares/`), named by
   `SEGARECOMP_SMS_ORACLE_CHECKOUT`. Nothing is vendored or committed.
   - The libretro cores are built from temporary copies; the ares/Blargg sources are compiled read-only into a
     temporary directory.
   - Unset or missing gives `skipped:` with exit 0. A wrong HEAD or modified tracked files is a hard failure.
   - References are never linked into production and never distributed.
5. **CI.** Hermetic tests only (M68K/Z80 policy): CI never fetches, builds or runs a reference. Reference-validated
   coverage is recorded in a committed validation manifest that later tasks regenerate locally with the pinned
   checkouts. T012 turns the capability list into the ratchet that consumes it.
6. **Comparison granularity.**
   - VDP ports/state: generated port-sequence differential on the result/state level, exact.
   - Renderer: state-injection single-frame differential. Our artifact is 6-bit CRAM values, the reference's is
     RGB, so they are compared through the colour-class bijection (each CRAM value maps to exactly one reference
     RGB and vice versa within the frame). Exact per pixel.
   - Mapper: write-sequence differential over (register state, address) classes, exact.
   - Interrupts/timing: IRQ trace per line, exact in line number. In-line offsets stay within the U2 bound until it
     is resolved.
   - PSG: register-write sequence -> per-chip-tick channel output bit and attenuation nibble, exact against ares
     with its mask.
   - PCM: no bit-exact reference comparison is possible, because the references synthesize band-limited output
     (Blip_Buffer/blip_buf) with their own filters. The PCM path is verified by our own exact reference
     implementation of the contract's decimation, plus a tolerance comparison of per-frame RMS level per channel
     against GPGX within +/-1 dB (justified by the filter differences).
   - Full fixture: frame hashes (bijection-normalized) exact against both machine references; PCM by the rule
     above.
7. **Adapters in the test tree:**
   - `tests/sms_oracle/libretro_host.c`: C11, a locally declared libretro subset, a phase/acknowledge handshake
     through RAM `$C0F5/$C0F6`;
   - `tests/sms_oracle/psg_observe_ares.cpp`, `tests/sms_oracle/psg_observe_blargg.cpp`, `tests/sms_oracle/pins.py`.

   They prove the injection/observation shape per selected reference: ROM program, pad and pause script, RAM
   results and framebuffer summaries for the machines; chip writes and per-tick state for the PSGs.

## Consequences

- T004/T005 build their generated-sequence and state-injection differentials against Gearsystem, with GPGX as
  falsification. T007 builds its chip-tick differential against ares under the committed mask.
- Any new disagreement is resolved against a public citation and recorded as a mask entry or as a contract
  correction, never absorbed silently.
