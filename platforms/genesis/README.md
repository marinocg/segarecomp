# Genesis / Mega Drive platform

Working platform: composes `libs/cpu/m68k`, `libs/device/sega/genesis`, `libs/recompiler`, and
`libs/codegen/c11` into generated-native Genesis programs.

- `machine/` — address-space, frontend, and startup composition (`segarecomp::machine_genesis`).
- `runtime/` — standalone strict-C11 runtime linked by generated programs (`segarecomp::runtime_genesis`).
- `viewer/` — optional SDL3 frame presenter (`SEGARECOMP_ENABLE_SDL3_VIEWER`).
- `compat/` — committed per-ROM compatibility metadata (no commercial data).

**Current status.** Generated-native MC68000 **and Z80** (the Genesis sound CPU, ADR 0073/0074 and the Z80 contract), Genesis machine/runtime/video, YM2612/PSG audio, controller I/O (two **three-button** pads), the SDL3 viewer and
the launcher library. Commercial games are playable; see the [compatibility ledger](../../docs/product/compatibility-ledger.md) for which games, with what evidence.
Limits: execution is 60 Hz (PAL selects only the version register), no six-button pad, no viewer input recording, compatibility incomplete.

*Main only (after v0.7.0):* widescreen Level 2/3, the public native patch SDK (`sdk/segarecomp_genesis_patch.h`) and `.segapatch` Vanilla/Patched variants ([overview](../../docs/product/widescreen-and-patches.md)),
and the opt-in static-first Z80 image production `--z80-static 1` ([ADR 0277](../../docs/decisions/0277-seg-128-genesis-z80-static-first-image-production.md); not a guarantee for any title).

Historical: the Sonic 1 title-screen checkpoint (SEG-007) was the first milestone; it is not the current capability.
See [../README.md](../README.md).
