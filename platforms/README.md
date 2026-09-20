# Platforms

Machine/platform products compose reusable libraries under `libs/` (CPU semantics, devices, media,
recompiler core, C11 codegen) into a specific console. Platform-specific runtime, machine wiring, and
frontends live here; CPU and device code is never duplicated under a platform.

Milestones: SEG-007 (Genesis title-screen
checkpoint, completed), SEG-008 (Z80 semantic path),
SEG-009 (Master System),
SEG-010 (Game Gear),
SEG-011 (Genesis continuation: first in-game frame),
SEG-018 (workspace productization, current).

| Platform | Code today | Current capability | Planned | Reuse |
| --- | --- | --- | --- | --- |
| [Genesis / Mega Drive](genesis/README.md) | yes | Generated-native MC68000; Genesis machine/runtime/video; controller I/O, VDP, YM2612, PSG and Z80-bus device behavior as currently implemented; Sonic 1 title-screen checkpoint (SEG-007) | Generated-native Z80 CPU semantic path (SEG-008); first in-game frame (SEG-011) | `libs/cpu/m68k`, `libs/device/sega/genesis` |
| [Master System](master-system/README.md) | no | none | SEG-009 | future Z80 CPU + SMS device libraries |
| [Game Gear](game-gear/README.md) | no | none | SEG-010 (Master System family specialization) | shares Master System libraries |
| [Sega CD](sega-cd/README.md) | no | none | future / not yet refined | `libs/cpu/m68k`, `libs/device/sega/genesis` |
| [32X](32x/README.md) | no | none | future / not yet refined | Genesis libraries + future SH-2 library |
| [Saturn](saturn/README.md) | no | none | future / not yet refined (after SH-2 foundations) | future SH-2 library |

Generated-native Z80 execution is **not** implemented yet; it lands with SEG-008. Only platforms with
real code appear in the CMake graph; documentation-only platforms have no source skeletons or placeholder
libraries.
