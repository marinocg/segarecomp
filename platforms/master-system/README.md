# Master System

Code lives in `machine/` (generation-time C++: cartridge ingestion, mapper identity, ImageSet, SMS generation route
`emit_cartridge`), `runtime/` (strict C11 compiled with generated programs: memory map, Sega mapper, typed error
surface, port decode, machine composition, deterministic scheduler, run API, state digest, scripted input) and `headless/`
(the generated program's `main`, artifacts, and the no-device wiring). See ADR 0065 and ADR 0066. The VDP state, port protocol, status register and frame/line interrupts
(`runtime/sms_vdp.{h,c}`, ADR 0067; `headless/sms_devices_vdp.c` attaches it and writes `vdp.trace`, `vram.bin`, `cram.bin`,
`vdp.json`) are independent of every other platform's VDP; rendering is T005. The PSG (`libs/device/sega/psg`, wired by `runtime/sms_psg.c`) produces the deterministic PCM stream and `audio.pcm`/`audio.sha256` headless artifacts; the controllers and I/O control (`runtime/sms_pad.c`, scripted `--input`, pause NMI, TH-latched H counter) are attached by the headless driver unless a device unit installs its own pad; the optional viewer (`viewer/`, ADR 0070: host-neutral core `sms_viewer.{h,c}`, SDL3 adapter `sms_viewer_sdl3.{h,c}` under `SEGARECOMP_ENABLE_SDL3_VIEWER`, viewer `main` `sms_viewer_main.c`) runs the same machine loop one frame at a time and records its input as a headless-replayable script.

**Purpose:** Static recompilation of Z80 + SMS VDP software.

**Profile:** Master System II (315-5246 VDP), NTSC, export region, documented post-BIOS state, declared mapper
identity. See [the machine contract](../../docs/architecture/master-system-machine-contract.md) and ADR 0061-0064.

**First checkpoint:** First deterministic frame of a project-authored fixture.

**Reuse:** SEG-008 Z80 CPU library, `codegen_c11_z80` and the Z80 runtime ABI; a platform-neutral PSG device
library `libs/device/sega/psg` (ADR 0063).

**Milestone:** SEG-009.
