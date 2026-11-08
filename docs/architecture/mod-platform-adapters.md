# Mod platform adapters and address-space ownership

Status: accepted contract (SEG-146-T001, ADR [0280](../decisions/0280-mod-platform-adapter-and-address-space-contract.md)). Consumers: SEG-133-T002 (schema 3), SEG-133-T003 (ABI 2),
SEG-133-T004 (phases), SEG-146-T002/T003/T004, SEG-147, SEG-148. Baseline facts: [gap inventory](modding-platform-gap-inventory.md).
This document defines **vocabulary and ownership only**. It introduces no bus, CPU or device interface.

## 1. What an adapter is

An adapter is the pair *(an existing platform runtime, a capability table)*. The runtimes already exist and are not changed by this contract:
Genesis `platforms/genesis/runtime/runtime.h`, Master System `platforms/master-system/runtime/sms_memory.h`, Sega CD `platforms/sega-cd/runtime/scd_runtime.h`.
The capability table lists which capabilities, spaces and phases the platform supports and at which support level. Nothing else is shared.

## 2. The shared vocabulary (the complete list)

| Term | Values | Rule |
|---|---|---|
| Platform identity | `genesis`, `master-system`, `sega-cd` | exact string; mandatory; never guessed from an image |
| Source-identity kind | `cartridge-image`, `disc-image` | Genesis and Master System use `cartridge-image`; Sega CD uses `disc-image`. A reader must reject a kind its platform does not use |
| Capability name | lowercase `domain.name`, ASCII `[a-z0-9._]`, at most 48 bytes | ABI 1 names (`frame.work_ram`, `frame.vdp`, `scenery.words`, `overlay.rgba`) are kept unchanged |
| Space name | `<owner>.<region>` from the tables below | always passed explicitly by the caller; there is no numeric space id and no default space |
| Phase name | `reset`, `frame-begin`, `input`, `post-input`, `hook-entry`, `hook-exit`, `frame-end`, `render` | defined with legal operations in [mod-execution-phases.md](mod-execution-phases.md) |
| Support level | `supported-and-tested`, `supported-with-limitations`, `experimental`, `unsupported` | per platform and capability; tracked in the support matrix (SEG-146-T005) |

Capability domains: `frame.*` (read-only presentation views), `scenery.*`, `overlay.*`, `memory.read.<space>`, `memory.write.<space>`, `input.*`, `hook.function`, `audio.*`,
`host.log`, `state.mod`. A capability name is shared across platforms **only when its semantics are identical**; otherwise it is a platform-specific name
(for example `frame.vdp` is Genesis and Sega CD (shared Genesis VDP), while a Master System VDP view would be `frame.sms_vdp`). A package that declares a capability its platform lacks is rejected at `mod check` with a stable diagnostic (`E_CAPABILITY_UNSUPPORTED`).

## 3. Address spaces

Columns: **Owner** (the single party allowed to mutate the space), **Phases** (when a mod may access it; phase names per T004), **R/W/H** (read, write, hook-site eligibility),
**Endian/width** (guest byte order and legal access widths), **Rule** (who may touch it when). "Mod write" is granted **only** to declared work-RAM spaces, bounded, at declared boundaries (T004).

### 3.1 Genesis (contract sources: `platforms/genesis/runtime/runtime.h`, `docs/architecture/genesis-z80-audio-contract.md`, `docs/architecture/genesis-z80-ram-window-compatibility-policy.md`, `docs/architecture/genesis-raster-and-display-modes.md`)

| Space | Owner | Phases | R/W/H | Endian / width | Rule |
|---|---|---|---|---|---|
| `m68k.rom` (cartridge ROM) | image | none at runtime | read: build time only; write: never; hook: M68K sites resolved at build time against the exact digest | big-endian; 8/16/32 | ROM is immutable at runtime; typed edits are build-time (M03) |
| `m68k.work_ram` (64 KiB) | M68K | read in every phase (in `render` only through the `frame.work_ram` copy); write only in `frame-begin`, `post-input`, `hook-exit`, `frame-end` (T004) | R yes; W yes if `memory.write.m68k.work_ram`; H n/a | big-endian; 8/16/32, 16/32 aligned | the only mod-writable Genesis space |
| `z80.ram` (8 KiB window at `$A00000`) | Z80 / bus arbiter | read in `frame-begin`, `frame-end` only while the Z80 is not running a conflicting access | R limited; W **no**; H no | byte-addressed window | static-first Z80 materialization (ADR 0073, 0135) is invalidated by writes, so no mod write |
| `vdp.vram`, `vdp.cram`, `vdp.vsram` | VDP | `render` read-only (via `frame.vdp` copies) | R yes (copy); W no | VRAM bytes; CRAM/VSRAM words | presentation only; never mutated by a mod |
| Z80 bus / I/O ports, YM2612, PSG | devices | none | no | - | not accessible; audio is governed by [modding-audio-architecture.md](modding-audio-architecture.md) |

### 3.2 Master System (contract: `docs/architecture/master-system-machine-contract.md` sections 3, 4, 5)

| Space | Owner | Phases | R/W/H | Endian / width | Rule |
|---|---|---|---|---|---|
| `z80.rom` (mapper-resolved) | image | none at runtime | build time only; hook sites resolved by verified (bank, offset) per source identity | little-endian; 8/16 | the mapper family is declared, never inferred (section 4.1); ROM is immutable |
| `z80.work_ram` (8 KiB at `$C000`, mirror at `$E000`) | Z80 | read in every phase (`render`: copy only); write as Genesis `m68k.work_ram` | R yes; W yes if `memory.write.z80.work_ram`; H n/a | little-endian; 8/16 | the mirror is one space; a mod addresses the 8 KiB once |
| `z80.cart_ram` (32 KiB, slot 2 when `$FFFC` bit 3 set) | Z80 | read in the phases above | R yes; W **no** this milestone | 8 | persistent saves: writes would alter game saves |
| `vdp.vram`, `vdp.cram` | VDP | `render` read-only | R copy; W no | - | presentation only |
| mapper registers `$FFFC-$FFFF` | mapper | none | no | - | write-through RAM mirror; not a mod surface |

Bank-aware resolution: a mod never decodes the mapper at runtime. Packages target a verified (ROM-image offset) site; the bank register is part of the source identity
(mapper declaration) and of the build, not of a runtime lookup.

### 3.3 Sega CD (contract: `docs/architecture/sega-cd-machine-contract.md` sections 3, 4, 6, 9a, 15)

| Space | Owner | Phases | R/W/H | Endian / width | Rule |
|---|---|---|---|---|---|
| `main.work_ram` (64 KiB, repeats through `$E00000-$FFFFFF`) | main M68K | as Genesis `m68k.work_ram` | R yes; W yes if `memory.write.main.work_ram`; H main-M68K sites only | big-endian; 8/16/32 | the only mod-writable Sega CD space |
| `main.word_ram` (2M mode `$200000-$23FFFF`; 1M mode `$200000-$21FFFF` bank) | current owner (`RET`/`DMNA`, section 6) | read only while main is the owner at that phase boundary | R conditional; W no | big-endian | non-owner access completes nowhere (open bus / ignored): a mod access when not owner is a **diagnosed denial**, not a stall |
| `sub.prg_ram` (512 KiB; 128 KiB main window) | sub M68K, main via bank window | read-only when the main window is open | R conditional; W no; H sub sites unsupported until SEG-148-T013 | big-endian | write protect and window gating per contract section 3/4 |
| `sub.word_ram` | current owner | not accessible | no | - | owner rules as `main.word_ram` |
| `sub.backup_ram` (8 KiB, odd byte lane) | sub | none | no | byte | persistent saves |
| `pcm.ram`, CD subsystem registers, Gate Array | devices | none | no | - | not a mod surface; CD-DA/PCM governed by the audio contract |
| `z80.ram` | main-side Z80 | as Genesis `z80.ram` | R limited; W no | byte | shared Genesis Z80 path |
| `vdp.*` | VDP | `render` read-only | R copy | - | as Genesis |
| `disc` | image | build time | build-time edits only (layout-preserving) | sector-based | [sega-cd-mod-sources.md](sega-cd-mod-sources.md) |

Module and overlay state: Sega CD code is disc-loaded (contract section 15). A hook or edit site is resolved **by verified symbol or site per source identity** at build time; the
runtime never decodes which overlay is resident.

## 4. What is deliberately not shared

Bus semantics (arbitration, DTACK, open bus), VDP models, timing and scheduling, mapper behaviour, Word RAM ownership, audio devices, CPU instruction sets and the
generated-code shape stay platform-owned. There is **no** universal bus, CPU, device or emulator interface, no common `read(addr)` across platforms and no numeric address
that means the same thing on two platforms. The only shared artifacts are the vocabulary in section 2 and the shape of the tables in section 3.

**Review gate for any future shared interface.** A proposal to share anything beyond section 2 must (1) name two platforms that already implement the same behaviour in
current code, (2) show a test that passes unchanged on both, (3) show it removes more code than it adds, and (4) be accepted as an ADR by independent review. Absent all four it is rejected.

## 5. Rules every consumer must follow

1. A caller names the space on every access; an unnamed or unknown space is `E_SPACE_UNKNOWN`.
2. Reads and writes are bounds-checked against the space size and width/alignment rules above; violations are `E_ACCESS_RANGE` / `E_ACCESS_ALIGNMENT`.
3. A write to a space not marked mod-writable is `E_SPACE_READONLY`, at build time when statically visible, otherwise a diagnosed failure that disables the mod ([phases](mod-execution-phases.md) section 5).
4. A space owned by another party in the current phase is `E_SPACE_NOT_OWNER`.
5. Resolution of sites is build-time, per source identity digest. No runtime decoding, no heuristics, no fallback guess.
6. Diagnostics are stable identifiers; their text may change.

## 6. Tests T004 and the later conformance suite must run per platform

| Platform | Required tests |
|---|---|
| Genesis | `tests/genesis_snapshot_test.py`, `tests/genesis_rewind_test.py`, `tests/genesis_states_test.py`, `tests/genesis_extension_test.cpp`, `tests/genesis_patch_provider_build_test.py` |
| Master System | `tests/sms_snapshot_test.py`, `tests/sms_bank_crossing_test.py`, `tests/sms_machine_scheduler_test.py`, `tests/sms_extension_test.cpp` |
| Sega CD | `tests/sega_cd_snapshot_test.py`, `tests/sega_cd_state_convergence_test.py`, `tests/sega_cd_dual_cpu_test.py`, `tests/sega_cd_main_z80_test.py`, `tests/sega_cd_viewer_states_test.py` |

## 7. Testable statements

* Each space name in section 3 appears exactly once per platform and is unique across platforms only by its `<owner>.<region>` form.
* No space outside the work-RAM rows is writable; a conformance scenario per platform asserts `E_SPACE_READONLY` for each other space.
* `main.word_ram` access while main is not the owner yields `E_SPACE_NOT_OWNER`.
