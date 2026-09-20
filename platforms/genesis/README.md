# Genesis / Mega Drive platform

Working platform: composes `libs/cpu/m68k`, `libs/device/sega/genesis`, `libs/recompiler`, and
`libs/codegen/c11` into generated-native Genesis programs.

- `machine/` — address-space, frontend, and startup composition (`segarecomp::machine_genesis`).
- `runtime/` — standalone strict-C11 runtime linked by generated programs (`segarecomp::runtime_genesis`).
- `viewer/` — optional SDL3 frame presenter (`SEGARECOMP_ENABLE_SDL3_VIEWER`).
- `compat/` — committed per-ROM compatibility metadata (no commercial data).

Current: generated-native MC68000 plus Genesis machine/runtime/video and the implemented device behavior; Sonic 1 title-screen checkpoint completed in SEG-007.
Planned: generated-native Z80 (SEG-008); first in-game frame (SEG-011).
See [../README.md](../README.md).
