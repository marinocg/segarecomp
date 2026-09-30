# ADR 0063: Master System Placement, Reuse Boundary and PSG Device Placement

- Status: Accepted
- Date: 2026-09-30
- Task: SEG-009-T001
- Related: ADR 0059 (Z80 placement), ADR 0061, ADR 0064, `docs/architecture/master-system-machine-contract.md`,
  `docs/architecture/z80-master-system-integration-contract.md`, `docs/architecture/genesis-psg-sn76489-port-write-compatibility-policy.md`.

## Context

SEG-009 adds the second machine. The milestone requires an explicit audit of what is reused unchanged, what may
become shared because SMS is a genuine second consumer, and what must stay SMS-specific. The audit below was made
against product `main` at `97d56fa` (2026-09-30).

## Audit on current `main`

### Reused unchanged

| component | evidence on `main` | SMS use |
| --- | --- | --- |
| `libs/media` header classification (`inspect_rom`, `Platform::master_system`, `TMR SEGA` at `$1FF0/$3FF0/$7FF0`, region nibble -> `sms_export`/`sms_japan`, size code) and `sha256` | `libs/media/src/rom.cpp` lines 18-19 and 155-172; `tests/sms_fixture_rom_test.py` runs `segarecomp inspect` on both fixture ROMs and gets `HDR_RECOGNIZED_SMS` / `sms_export`, with no mapper named | platform/profile recognition and input identity. A missing, conflicting or Game Gear header requires explicit selection (T002/T010) |
| `libs/cpu/z80` (decode, forms, provenance) | SEG-008 completion (261 forms, 11 stages) | CPU semantics; nothing SMS-specific enters it |
| `codegen_c11_z80::emit_image_set`, `ImageSet`, `ImageKind` | `libs/codegen/c11/include/segarecomp/codegen/c11/z80.hpp` lines 27, 46, 90 | generation route (T002); the ADR 0058 SMS reference shape |
| `z80_runtime.h` ABI (`Z80Host`, outcomes, `int_line`/`nmi_pending`, deadlines) | integration contract §3-§9 | run-time wiring (T003) |
| Z80 conformance/budget test pattern (`--emitter`, `--cc`, CTest functions) | `tools/z80_*.py`, `tests/z80_*` | fixture build harness for tests (T003) |
| ADR 0050 launcher/toolchain and generated `main` budget semantics | ADR 0050 amendment | headless driver and launcher (T003/T010) |

### Extraction candidates (only with a real second consumer; decided by the named task)

| candidate | on `main` | second-consumer argument | decision owner and rule |
| --- | --- | --- | --- |
| C SHA-256 for artifacts | private `genesis_sha256_*` in `platforms/genesis/runtime/runtime.c` (around line 3087) | SMS needs framebuffer, PCM and state digests in C11 at run time (ADR 0064) | T003. Extract to a platform-neutral C11 source (proposed `libs/media/c/sha256.{h,c}`) only if the Genesis runtime keeps its behaviour and tests unchanged; otherwise SMS carries its own copy and records why |
| SDL3 presentation helper and rational pacer | `platforms/genesis/viewer/viewer.h` (`GenesisViewerHost`, `GenesisPacer` with a rational NTSC period), `viewer_sdl3.c` (88 lines, video only) | the SMS viewer needs the same window/texture/present/key/pacer pieces with a different rational period (59,736 T at 315/88 MHz) | T009. Extract only if both viewers get smaller and the Genesis viewer's behaviour and tests are unchanged. The framebuffer format, CRAM decoding and runtime hooks stay platform-specific |
| audio output (SDL3 stream) | none: no audio exists anywhere in the product | SMS is the first consumer; SEG-032 would be the second | T009. Keep the API PCM-frames-plus-rate and platform-neutral, placed where T009's viewer decision puts SMS presentation code |
| build routing | `apps/segarecomp/build_command.cpp` is Genesis-only (`analyze_genesis_reset_image`, line 238) | SMS is the second platform route | T010. Add a platform route and `--mapper`/manifest, not a second build system |
| scheduler pieces | Genesis scheduling is interleaved with 68K-specific dispatch | unclear; SMS needs only a small explicit line/event loop | T003. Extract only if both become simpler; otherwise record why not |

### SMS-specific (never shared)

Mapper and ImageSet declaration; address map; port decode; memory control; VDP (registers, ports, status, interrupts,
counters, renderer, palette); VDP timing; interrupt wiring; controller protocol and I/O control; pause NMI; clock
ratios; reset and post-BIOS state; SMS headless driver and artifacts. Everything Z80-owned stays Z80-owned
(integration contract §10).

## PSG reuse decision

**Evidence.**
- SMS Power! (*SN76489*) lists the SMS 1, SMS 2, Genesis/Mega Drive and Game Gear PSGs as the same Sega integrated
  variant: a 16-bit LFSR, white-noise taps bits 0 and 3, the same latch/data protocol and the same /16 internal
  divider.
- MacDonald (VDP §1) states every VDP revision integrates the SN76489 and calls it "identical to the stand-alone
  version". That conflicts with SMS Power! on the LFSR (15-bit taps `$0003` discrete vs 16-bit taps `$0009` Sega); the
  more specific SMS Power! measurement is followed, which is why LFSR width/taps are the variant parameter. The Game Gear adds only a stereo
  register.
- Genesis Plus GX drives both machines with one `psg.c` "integrated" model, parameterised only by clock and
  panning.
- The chip clock is the Z80 clock on both the SMS and the Genesis Z80 side (3.58 MHz NTSC).
- The only real differences are wiring (port decode, clock source) and the Game Gear stereo register.

**Decision.**
- Place a **platform-neutral PSG device library `libs/device/sega/psg`** in plain C11. It has no SMS or Genesis
  include and no `segarecomp::device_genesis` dependency.
- It is parameterised only where public evidence shows a variant difference: LFSR width/taps (discrete SN76489 vs
  Sega integrated) and the optional stereo mask (Game Gear, unused by SMS).
- It exposes chip-tick stepping, timestamped writes and the channel output/attenuation state the reference
  differential observes.
- The SMS runtime supplies the clock, the port wiring and the reset. The sample-generation pipeline (decimation to
  44,100 Hz s16le) belongs to the SMS runtime/audio layer, not to the device (ADR 0064).
- No universal audio framework is designed.
- `GenesisPsgState` (a latch-only policy state; the Genesis runtime synthesizes no audio) is **not** reused or
  changed. Migrating Genesis to the shared device is a **SEG-032** decision.
- A dependency test (T007) proves the library builds and tests without any SMS/Genesis target and that the SMS
  runtime depends on it, never the reverse.

## Placement of new SMS code

- `platforms/master-system/machine`: generation-time C++. Profile, ROM validation, mapper identity, the mapper
  contract table's ImageSet side and the emission entry point. Library `segarecomp::machine_master_system`, T002.
- `platforms/master-system/runtime`: C11 compiled with the generated code (ADR 0050). Memory map/mapper run-time
  side, port decode, scheduler, VDP, controllers, run API, headless driver, artifacts. Library
  `segarecomp::runtime_master_system`, T002-T007.
- `platforms/master-system/viewer`: SDL3 viewer and audio output (T009), subject to the extraction rule above.
- `libs/device/sega/psg`: the PSG device (T007).
- `tools/sms_*.py` and `tests/sms_*`: capability list, fixture builder, reference adapters (T001), later
  differentials.

The directory name follows the existing documentation-only `platforms/master-system/`. CMake target names use
`master_system`, as `Platform::master_system` does.

## Consequences

The mapper contract table is one data definition consumed by both the generation-time ImageSet and the run-time
C11 mapper (T002). The device library boundary is enforced by a dependency test. No SMS work changes `libs/cpu/z80`,
`codegen_c11_z80` or any Genesis target except through an explicit extraction that leaves Genesis behaviour and tests
unchanged.
