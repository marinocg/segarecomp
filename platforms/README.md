# Platforms

Machine/platform products compose reusable libraries under `libs/` (CPU semantics, devices, media,
recompiler core, C11 codegen) into a specific console. Platform-specific runtime, machine wiring, and
frontends live here; CPU and device code is never duplicated under a platform.

**Current status** (the authoritative per-game and per-feature record is the [compatibility ledger](../docs/product/compatibility-ledger.md); the latest published release is v0.7.0 and items marked *main only* are not in it).

| Platform | Code today | Current capability | Planned | Reuse |
| --- | --- | --- | --- | --- |
| [Genesis / Mega Drive](genesis/README.md) | yes | Generated-native MC68000 **and Z80**; Genesis machine/runtime/video; controller I/O, VDP, YM2612, PSG and Z80-bus device behavior; SDL3 viewer; launcher library; widescreen Level 2/3 and `.segapatch` variants (*main only*); opt-in static-first Z80 image production in the CLI (*main only*) | wider title compatibility; six-button pads; 50 Hz PAL | `libs/cpu/m68k`, `libs/cpu/z80`, `libs/device/sega/genesis` |
| [Master System](master-system/README.md) | yes | Generated-native Z80; SMS machine/runtime, VDP, PSG, controllers and Pause; SDL3 viewer and input recording; launcher library; widescreen Level 2/3 (always Partial, *main only*) | wider title compatibility | `libs/cpu/z80`, `libs/device/sega/psg` |
| [Game Gear](game-gear/README.md) | no | none | Master System family specialization (SEG-010) | shares Master System libraries |
| [Sega CD](sega-cd/README.md) (experimental) | yes | Authentic user-supplied BIOS, two generated-native MC68000 processors and a main-side Z80, Gate Array, Word RAM, CDD/CDC, CD-DA, RF5C164 PCM, FM/PSG, persistent internal backup RAM, viewer, launcher build/import, recording and replay; synthetic post-BIOS profile (SEG-062) for tests; widescreen Level 2/3 (always Partial, *main only*). Contract in docs/architecture | complete CUE-to-play compilation (not claimed), sound-level validation | `libs/cpu/m68k`, `libs/device/sega/genesis` |
| [32X](32x/README.md) | no | none | future / not yet refined | Genesis libraries + future SH-2 library |
| [Saturn](saturn/README.md) | no | none | future / not yet refined (after SH-2 foundations) | future SH-2 library |

Historical milestones: SEG-007 (Genesis title-screen checkpoint), SEG-008 (Z80 semantic path), SEG-009 (Master System), SEG-010 (Game Gear, not started), SEG-011 (Genesis first in-game frame).
Only platforms with real code appear in the CMake graph; documentation-only platforms have no source skeletons or placeholder libraries.
