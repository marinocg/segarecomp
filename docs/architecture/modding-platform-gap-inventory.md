# Modding platform gap inventory (Genesis, Master System, Sega CD)

Status: accepted baseline for the modding-platform contracts (SEG-133-T001). Every row cites a file or test that exists in this tree; a reviewer can
spot-check any limit by opening the named file. The document contains no ROM-derived data and refers to nothing outside this repository.
It designs nothing: schema 3, ABI 2, execution phases, the Luau policy and the adapter vocabulary are the follow-on documents
([adapters](mod-platform-adapters.md), [schema 3](segapatch-schema-3.md), [phases](mod-execution-phases.md), [ABI 2](native-sdk-abi-2.md),
[Luau](luau-trust-policy.md), [audio](modding-audio-architecture.md), [Sega CD sources](sega-cd-mod-sources.md)).

Classification vocabulary used throughout:

* **exists** - implemented and tested; later tasks must reuse it and must not re-implement it.
* **extend narrowly** - a seam exists; the later task adds a bounded capability to that seam.
* **missing** - no implementation; the later task creates it.
* **n/a** - not applicable to the platform by design.

## 1. Existing implementation inventory

### 1.1 Patch packages (`.segapatch`)

| Item | Class | Where | Evidence | Hard limits later contracts must respect |
|---|---|---|---|---|
| Schema 1/2 parse, verify, apply | exists | `libs/media/src/patch_package.cpp`, `libs/media/include/segarecomp/patch.hpp` | `tests/segapatch_package_test.py`, `tests/segapatch_native_package_test.py` | `kMaxPackageBytes` 32 MiB archive, `kMaxManifestBytes` 16 KiB, `kMaxPatchBytes` 16 MiB, `kMaxImageBytes` 16 MiB, `kMaxArchiveEntries` 8, `kMaxProviderSourceBytes` 256 KiB |
| Closed manifest (unknown/duplicate keys refused) | exists | `patch_package.cpp` (`required`/`optional` key lists, closed JSON subset) | same tests | one integer, ASCII strings only |
| Platform field | exists, Genesis-only | `patch_package.cpp` (`info.platform != "genesis"` is refused) | same tests | SMS and Sega CD packages cannot be expressed |
| Payload types | exists | `rom` (BPS, schema 1/2) and `native` (schema 2 only) | same tests | closed list: any other payload type is refused; exactly one native source (`provider.c`) |
| Package identity | exists | SHA-256 of the manifest bytes (`PackageInfo::identity`) | same tests | manifest names every payload digest, so identity covers all inputs |
| BPS apply | exists | `libs/media/src/patch_bps.cpp` (`apply_bps`) | `tests/segapatch_package_test.py` | verifies sizes, three CRC-32 values, copy offsets and stream end; output <= `max_output` |
| CLI `patch inspect|verify|apply` | exists | `apps/segarecomp/patch_command.cpp` | `tests/segapatch_package_test.py` | one JSON object on stdout |
| Presentation defaults, `widescreen_width_max` | exists | `PackageInfo` | `tests/segapatch_package_test.py` | closed value sets |
| Schema 3, mod project, multi-source, assets | missing | - | - | SEG-133-T002 designs, SEG-135 implements |

### 1.2 Native SDK and provider build

| Item | Class | Where | Evidence | Limits |
|---|---|---|---|---|
| Native SDK ABI 1 header | exists | `platforms/genesis/sdk/segarecomp_genesis_patch.h` (`GENESIS_NATIVE_COMPAT_ABI` 1) | `tests/genesis_patch_provider_build_test.py` | one provider per package; table `GenesisNativeCompat` with `prepare`, `scenery_word`, `hud`; read-only views |
| Capabilities | exists | `GENESIS_CAP_FRAME_WORK_RAM` 1, `FRAME_VDP` 2, `SCENERY_WORDS` 4, `OVERLAY_RGBA` 8 (mirrored by `kCap*` in `patch.hpp`) | `tests/segapatch_native_package_test.py` | four bits; no write, hook, input or audio capability |
| Fixed sizes | exists | work RAM 65536, VRAM 65536, 24 registers, CRAM 128, VSRAM 80, 224 lines, overlay at most 128 x 256 RGBA | header constants | ABI 1 structure layouts are fixed |
| Host sees snapshot copies | exists | header comments; `docs/architecture/genesis-save-states-persistence-and-rewind.md` section 3.5 | `tests/genesis_native_compat_viewer_sdl_test.c` | provider cannot affect guest execution (this is why state features need no provider state today) |
| Provider build and link into the Patched program | exists | `apps/segarecomp/build_command.hpp` (`--patch-package`, `--patch-identity`), `apps/segarecomp/build_command.cpp` | `tests/genesis_patch_provider_build_test.py` (Linux only in CI) | Genesis viewer builds only; compiles against the public SDK header only |
| Non-Genesis SDK | missing | - | - | the SDK header and the build option are Genesis-only |
| Trust model | exists | launcher approval of the exact package identity | `apps/segarecomp-launcher/launcher_patch_gui_test.py` | native code is trusted, not sandboxed (SDK header comment, `docs/product/native-patch-sdk.md`) |

### 1.3 Build identities, variants and launcher

| Item | Class | Where | Evidence |
|---|---|---|---|
| Vanilla and Patched are separate builds; the patch identity is an input of the Patched build | exists | `docs/decisions/0266-seg-122-portable-patch-packages-and-game-variants.md`, `apps/segarecomp/main.cpp`, `apps/segarecomp/build_command.cpp` | `tests/genesis_patch_provider_build_test.py` |
| Launcher patch library, install, approval, removal, variant selection | exists | `apps/segarecomp-launcher/patch_library.hpp`, `patch_library.cpp` | `apps/segarecomp-launcher/launcher_patch_gui_test.py` |
| Approval never transfers between identities | exists | `patch_library.hpp` (`trust_native`) | same |
| Catalog, remote install, signatures, update/rollback | missing | - | scheduled in SEG-143 |
| Set of several mods (composition) | missing | single package per variant | scheduled in SEG-141 |

### 1.4 Rendering, widescreen and presentation

| Item | Class | Where | Evidence | Limits |
|---|---|---|---|---|
| Frame extension producer (Genesis) | exists | `platforms/genesis/runtime/frame_extension.h` | `tests/genesis_extension_test.cpp`, `tests/genesis_viewer_extension_sdl_test.c` | side extension per side bounded by `SV_EXT_MAX_SIDE` 256 source pixels (`libs/viewer/include/sv_layout.h`) |
| Layout geometry | exists | `libs/viewer/include/sv_layout.h` (`sv_layout_extension_side`, `sv_layout_visible_side`, `sv_layout_extended_dest`) | `libs/viewer/test` | extension targets a 16:9 display; the 256-pixel side bound caps wider targets (SEG-139 audits and lifts it) |
| Width setting (4:3 to 16:9) | exists | `docs/product/widescreen-and-patches.md` | `tests/genesis_viewer_test.cpp` | slider range ends at 16:9 |
| Scenery lookup, HUD overlay from a native provider | exists | ABI 1 `scenery_word`, `hud` | `tests/genesis_native_compat_viewer_sdl_test.c` | one fixed overlay at most 128 x 256; no authored sprites |
| Master System extension | exists, partial | `tests/sms_extension_test.cpp`, `tests/sms_viewer_extension_sdl_test.c` | the tests | Mode 4 name table is 32 columns and wraps, so extension repeats scenery (`docs/product/widescreen-and-patches.md`) |
| Sega CD extension | exists, partial | `tests/sega_cd_viewer_extension_sdl_test.c` | the test | always Partial: no VRAM write journal on the Sega CD presentation path |
| Authored sprites, layered overlays, wider than 16:9 | missing | - | scheduled in SEG-139/140 |

### 1.5 Save states, rewind, replay, capture, profiling, tooling, CI

| Item | Genesis | Master System | Sega CD | Evidence |
|---|---|---|---|---|
| Whole-machine save states and rewind | exists (SEG-131, ADR 0278) | exists (SEG-132, ADR 0279) | exists (SEG-132; bound to disc and BIOS identity) | `docs/architecture/genesis-save-states-persistence-and-rewind.md`, `docs/architecture/sms-sega-cd-save-states-inventory.md`, `docs/product/save-states.md`; `tests/genesis_snapshot_test.py`, `tests/genesis_rewind_test.py`, `tests/sms_snapshot_test.py`, `tests/sega_cd_snapshot_test.py` |
| Container limits | total file <= 4 MiB; rewind budget 96 MiB, snapshot every 500 ms of guest time (Genesis contract sections 4.3 and 7) | same container rules (inherited) | same | contract above |
| Mod-owned state in a state file | missing | missing | missing | the container has a fixed section list; SEG-138-T006 adds a versioned section |
| Input scripts / headless pad scripts | extend narrowly (no viewer recording, `docs/product/compatibility-ledger.json`) | exists (`tests/sms_pad_native_test.py`) | exists (`tests/sega_cd_input_test.py`) | tests named |
| Frame capture | exists | not provided (render checks: `tests/sms_render_native_test.py`) | exists | `platforms/genesis/viewer/frame_capture.h`, `tests/genesis_frame_capture_bridge_test.py`, `tests/sega_cd_frame_capture_test.py` |
| Deterministic replay as a user feature | missing (ledger limitation) | missing | missing | SEG-138-T007 and SEG-144-T004 |

| Tooling | Class | Where | Evidence |
|---|---|---|---|
| Profiling | exists | `tools/perf_report.py`, `docs/decisions/0273-performance-profiling-and-observability.md` | `tests/perf_report_test.py`, `tests/perf_integration_test.py` |
| Ghidra analysis | exists | `tools/ghidra.py`, `tools/ghidra/`, `docs/testing/ghidra-analysis.md` | local-input only; not a CI dependency |
| Product-truth gate | exists | `tools/check_product_truth.py`, `docs/product/compatibility-ledger.json` | `tests/product_truth_test.py` |
| CI | exists | `.github/workflows/ci.yml` (6-shard CTest on Linux, macOS, Windows; SDL3 viewer jobs), `.github/workflows/release.yml` | the workflows |
| Cross-OS provider build | limited | provider build test is Linux-only (`docs/product/native-patch-sdk.md` "Platform coverage") | SEG-144-T001 |

## 2. Per-platform rows

Class per cell; the cited evidence is in the section named in the last column.

| Facet | Genesis | Master System | Sega CD | Evidence |
|---|---|---|---|---|
| Patch format (schema 1/2 packages) | exists (platform `genesis`) | missing (platform refused) | missing (16 MiB image cap, one image, cartridge model) | 1.1 |
| SDK / native provider | exists (ABI 1) | missing | missing | 1.2 |
| Runtime access to guest state | exists, read-only snapshot (work RAM, VDP) | missing (headless/viewer state is internal) | missing | 1.2; `platforms/master-system/runtime/sms_memory.h`, `platforms/sega-cd/runtime/scd_runtime.h` |
| Hooks (function interception) | missing | missing | missing | generated C is the only execution form: `docs/architecture/pipeline.md`; feasibility spikes SEG-138-T003, SEG-147-T003, SEG-148-T004 |
| Graphics | exists (producer, composer, HUD) | exists, partial | exists, partial | 1.4 |
| Audio | exists: YM2612 (ymfm), PSG, DAC, mixer (`docs/architecture/genesis-z80-audio-contract.md`, ADR 0074, 0075; `tests/genesis_audio_*`) | PSG only; YM2413 not modelled (`docs/architecture/master-system-machine-contract.md` section 10, ADR 0070, `platforms/master-system/headless/sms_audio.c`) | CD-DA, RF5C164, main-side Z80 with YM2612/PSG (`docs/architecture/sega-cd-machine-contract.md` sections 22, 23; `tests/sega_cd_cdda_native_test.py`, `tests/sega_cd_pcm_native_test.py`); FM/PSG output not reference-checked (SEG-115 open) | audio baseline, section 4 |
| Save states / rewind | exists | exists | exists | 1.5 |
| Tooling (inspection, diff-run) | partial (CLI patch commands; headless pad scripts) | partial | partial | 1.5 |
| Identity of the source image | cartridge image digest | cartridge image digest plus declared mapper (`master-system-machine-contract.md` section 4.1; mapper is never inferred) | disc identity: ordered CUE FILEs (size + SHA-256), track table, layout (`sega-cd-machine-contract.md` section 9a; ADR 0129) | - |

## 3. Roadmap capability classification (M02-M11)

| Milestone | Capability | Class | Owning subsystem / file |
|---|---|---|---|
| M02 SEG-135 | schema 3 parsing and strict validation | missing | `libs/media/src/patch_package.cpp` (new parser beside schema 1/2) |
| M02 | `mod.toml` project model, target-revision declarations | missing | new CLI module in `apps/segarecomp` |
| M02 | multi-file C11, Luau sources, declared assets | extend narrowly | existing package reader (1 source, 8 entries) |
| M02 | mod CLI `init/check/inspect/build/pack` | extend narrowly | `apps/segarecomp/patch_command.cpp` (`inspect`, `verify`, `apply` exist) |
| M02 | variants, cache reuse, build diagnostics | extend narrowly | existing Vanilla/Patched build (`build_command.cpp`) |
| M03 SEG-136 | typed ROM edits, symbol mapping | missing | new authoring module; BPS apply exists |
| M03 | assembler selection and code injection | missing | none |
| M03 | BPS construction | missing | only `apply_bps` exists |
| M03 | RAM/code inspection and Vanilla-vs-Modded workflow | extend narrowly | `tools/genesis_frontier_debug.py`, headless pad scripts |
| M04 SEG-137 | Luau VM, sandbox, trusted compiler | missing | nothing vendored |
| M04 | read-only frame/event/overlay API | extend narrowly | ABI 1 read-only frame views are the model |
| M05 SEG-138 | lifecycle phases | missing | generated runtime has frame and VBlank boundaries only |
| M05 | guest work-RAM writes | missing | ABI 1 is read-only by construction |
| M05 | function hooks | missing | generated C emitter (`libs/codegen/c11`), spike first |
| M05 | mod state in save states/rewind | extend narrowly | container section list (Genesis contract section 4.1/10) |
| M06 SEG-139 | geometry through 32:9 | extend narrowly | `libs/viewer/include/sv_layout.h`, `platforms/genesis/runtime/frame_extension.h` |
| M07 SEG-140 | authored sprites, pattern decoding, layered overlays | extend narrowly | existing VDP decoding is reused; one fixed overlay today |
| M08 SEG-141 | mod sets, composite identity | missing | single package per variant today |
| M09 SEG-142 | Developer Workshop | missing | launcher exists; no developer-mode panels |
| M10 SEG-143 | catalog, provenance, update/rollback | missing | launcher library exists (`patch_library.*`) |
| M11 SEG-144 | cross-OS tests, fuzzing, determinism, beta gate | extend narrowly | CI exists; provider build test Linux-only |

## 4. Audio capability baseline (feeds SEG-145-T001)

Verified in this tree: Genesis has generated-native Z80 with static-first materialization (self-modifying drivers not statically provable are isolated, never interpreted;
ADR 0073, 0134, 0135), YM2612 through vendored ymfm (ADR 0074), PSG and DAC, mixer, PCM artifact and SDL delivery (ADR 0075).
Master System has generated-native Z80 and SN76489 PSG at 44.1 kHz; **the YM2413 FM unit is not modelled** (ports `$F0-$F2` absent). Sega CD has CD-DA, RF5C164 PCM and a
main-side Z80 with YM2612/PSG (FM/PSG output not reference-checked). Schema 1/2 cannot carry Sega CD music replacement: image cap 16 MiB, archive cap 32 MiB, Genesis-only SDK.
The full matrix and the two audio mechanisms are in [modding-audio-architecture.md](modding-audio-architecture.md).

## 5. Genesis-only assumptions in the native SDK today

1. `platform` must equal `genesis` (`patch_package.cpp`).
2. Header names, structures and constants are Genesis-prefixed; fixed sizes equal Genesis memories (64 KiB work RAM, 64 KiB VRAM, 128-byte CRAM, 80-byte VSRAM).
3. The provider build option exists only on Genesis viewer builds (`build_command.hpp`).
4. Work RAM is the only guest memory visible, and it is a copy (no space parameter exists; ABI 2 callers must name the space explicitly: [adapters](mod-platform-adapters.md)).
5. 16:9 assumption in extension geometry (`sv_layout.h`), 256-pixel side bound.
6. One provider per package, so there is no composition or ordering.

## 6. Hard limits recorded for later contracts

| Limit | Value | Source |
|---|---|---|
| Archive / manifest / BPS payload / image / provider source | 32 MiB / 16 KiB / 16 MiB / 16 MiB / 256 KiB | `patch.hpp` |
| Archive entries | 8 | `patch.hpp` |
| Side extension | 672 source pixels per side (legacy 4:3..16:9 clamp 256); ratios 4:3 through 32:9, rejected not clamped beyond | `sv_layout.h` (`SV_EXT_MAX_SIDE`, `SV_EXT_LEGACY_SIDE`, `sv_geom_plan`) |
| Overlay | 128 x 256 RGBA | SDK header |
| ABI 1 frame state | 64 KiB work RAM, 64 KiB VRAM, 224 lines | SDK header |
| Save-state file | 4 MiB; rewind 96 MiB | Genesis contract sections 4.3 and 7 |
| Packages per variant | 1 | `build_command.hpp` (single `--patch-package`) |
| Providers per package | 1 | `patch.hpp` |
| SMS ROM sizes | 32 to 512 KiB, mapper declared | `master-system-machine-contract.md` section 4.1 |
| Sega CD `INDEX 02` and later | unsupported (`cue_unsupported`) | `sega-cd-machine-contract.md` section 9a |

## 7. Platform classification of SEG-133..SEG-148 tasks

Classes: **neutral** (platform-neutral), **genesis-first** (Genesis reference, successors for the others), **successor** (needs SMS/Sega CD work in SEG-147/148/145),
**capability-declared** (available per declared capability), **gated** (unsupported until its feasibility gate says GO).

| Tasks | Class | Follow-up |
|---|---|---|
| SEG-133-T001..T005 | neutral (platform-aware vocabulary from SEG-146-T001) | none |
| SEG-133-T006, T007 | neutral, harness-delivered | none |
| SEG-134 (all) | neutral, public repository | per-platform content arrives with the platform examples |
| SEG-135-T001..T003 | neutral with `platform` and source-identity discriminator | `disc-image` resolved by SEG-148-T001 |
| SEG-135-T004..T006 | neutral | none |
| SEG-136-T001, T002, T004..T006 | genesis-first (M68K and Z80) | SMS: SEG-147-T001/T002; Sega CD: SEG-148-T002/T003 |
| SEG-136-T003, T007 | genesis-first research / tooling | assembler decision covers M68K, Z80, sub-M68K |
| SEG-137-T001..T007 | neutral VM, capability-declared per platform | SMS: SEG-147-T004; Sega CD: SEG-148-T006 |
| SEG-138-T001, T002, T004..T009 | genesis-first | SEG-147-T004/T005/T011; SEG-148-T005/T007/T012 |
| SEG-138-T003 | gated spike | SEG-147-T003, SEG-148-T004 |
| SEG-139 (all) | genesis-first | SEG-147-T009, SEG-148-T010 (evidence-gated) |
| SEG-140 (all) | genesis-first | SEG-147-T006, SEG-148-T009 |
| SEG-141-T001..T006 | genesis-first, one shared engine | SEG-141-T007 (SMS), SEG-141-T008 (Sega CD, post-beta; sets larger than 1 rejected until then) |
| SEG-142 (all) | one tool, capability-declared panels | SEG-147-T010, SEG-148-T011, SEG-145-T011 |
| SEG-143 (all) | neutral by source-identity kind | Sega CD matching: SEG-148-T001 |
| SEG-144 (all) | three-platform gate with support levels | SEG-146-T005 |
| SEG-145 (all) | capability-declared audio | SEG-145-T013 for SMS/Sega CD mapping |
| SEG-146 (all) | neutral vocabulary; adapters are existing runtimes | none |
| SEG-147 (all) | SMS successor | gated: T003/T008 |
| SEG-148 (all) | Sega CD successor | gated: T004/T008; post-beta: T013 |

## 8. Tasks that should shrink or be dropped because the capability exists

1. **SEG-135-T004 (mod CLI):** `patch inspect|verify|apply` already exists in `apps/segarecomp/patch_command.cpp`; extend that command family rather than add a second CLI entry.
2. **SEG-135-T005 (variants, cache):** Vanilla/Patched identities, cache keys and approval already exist (`build_command.cpp`, `patch_library.cpp`); the task is the identity extension for schema 3, not new build machinery.
3. **SEG-136-T006 (BPS):** apply and verification exist; only construction is new. No second BPS decoder.
4. **SEG-139-T001 audit:** the 256-pixel bound and 16:9 assumption are already catalogued here (section 6); the audit shrinks to measurement of buffer ownership.
5. **SEG-137-T004 read-only API:** ABI 1 frame views and `hud` overlay are the model; do not design a second read-only data model.
6. **SEG-138-T006 / SEG-147-T005 / SEG-148-T007 (state):** states and rewind exist on all three platforms; the tasks add a mod section, not a new codec.
7. **SEG-145 audio:** no hardware task exists for YM2612, PSG, DAC, RF5C164, CD-DA or the mixer; those are implemented. YM2413 stays out of scope.
8. **SEG-142 Workshop:** profiling (`tools/perf_report.py`), capture and the launcher already exist; panels consume them, no new profiling stack.
9. **SEG-144-T001:** CI already builds on Linux, macOS and Windows; only the provider/package build and Luau tests are Linux-only and need extension.
