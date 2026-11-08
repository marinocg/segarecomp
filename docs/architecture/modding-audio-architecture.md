# Audio modding architecture and verified capability matrix

Status: accepted contract (SEG-145-T001, ADR [0286](../decisions/0286-audio-modding-architecture.md)). **Nothing here is implemented**; it records what already exists so no task re-implements hardware, and fixes the shared vocabulary.
Verified against this tree. Inputs: [gap inventory](modding-platform-gap-inventory.md) section 4, [schema 3](segapatch-schema-3.md), [ABI 2](native-sdk-abi-2.md), [phases](mod-execution-phases.md), [adapters](mod-platform-adapters.md).
Consumers: SEG-145-T002 (audio asset declarations), SEG-145-T008 (audio API), SEG-145-T009 (host-audio spike), SEG-145-T012/T013, SEG-145-T005 (CD-DA decision).

## 1. Two mechanisms

| | Guest-native modification | Enhanced host audio |
|---|---|---|
| What changes | the guest's own music/SFX data, instruments, samples, driver tables and command mappings | mod-authored digital audio played by the host |
| Hardware behavior | original hardware behavior (the console's sound chips run the modified guest data) | not hardware: an extra host output, optionally replacing or mixing |
| Guest effect | yes, through statically valid guest code and data (build-time edits, declared mappings) | **none**: guest determinism is independent of host audio timing |
| Presentation | the normal mixed audio output | explicit presentation (the user sees that enhanced audio is active) |
| Status | the supported path (SEG-145-T003/T004/T006/T007/T008) | optional, gated by the SEG-145-T009 spike; `audio.host.play` is unsupported until GO |

Rules common to both: no commercial audio in any package or repository; the guest never depends on host timing; a mod that fails is disabled ([phases](mod-execution-phases.md) section 6).

## 2. Verified capability matrix (this tree)

Classes: **A** already implemented (no task), **B** achievable via existing guest modification, **C** needs SDK support, **D** needs new platform integration, **U** unsupported or unverified.

| Capability | Genesis | Master System | Sega CD |
|---|---|---|---|
| Sound CPU (generated-native Z80) | **A**: static-first materialization; a self-modifying driver not statically provable is isolated, never interpreted (ADR [0073](../decisions/0073-genesis-build-time-z80-materialization-and-ram-backed-code.md), [0134](../decisions/0134-genesis-z80-structural-code-variants-no-go.md), [0135](../decisions/0135-static-first-z80-mutation-proof-feasibility.md); `tests/genesis_z80_static_first_build_test.py`) | **A**: generated-native Z80 ([ADR 0119](../decisions/0119-seg-057-t002-native-sms-z80-producer-and-experimental-optimized-mode.md), [ADR 0064](../decisions/0064-master-system-execution-architecture.md)) | **A**: main-side Z80 (SEG-114, [ADR 0258](../decisions/0258-seg-114-m18-sega-cd-main-side-z80.md); `tests/sega_cd_main_z80_test.py`); driver epochs and FM/PSG correctness SEG-115 (ADR [0259](../decisions/0259-seg-115-m19-sega-cd-z80-driver-epochs.md)) are **U** until reference-checked |
| FM synthesis | **A**: YM2612 through vendored ymfm ([ADR 0074](../decisions/0074-genesis-ym2612-selection-and-oracle.md); `tests/genesis_audio_ym_test.py`, `tests/genesis_ym2612_device_test.py`) | **U**: the YM2413 FM unit is not modelled (ports `$F0-$F2` absent: [machine contract](master-system-machine-contract.md) sections 5 and 6) | **A** through the Genesis devices; not reference-checked (**U** correctness) |
| PSG | **A**: `tests/genesis_audio_psg_test.py` | **A**: SN76489 at 44.1 kHz (machine contract section 10, [ADR 0070](../decisions/0070-master-system-viewer-audio-and-presentation-reuse.md); `platforms/master-system/headless/sms_audio.c`; `tests/sms_psg_native_test.py`, `tests/sms_psg_differential_test.py`) | **A** through the Genesis devices; not reference-checked |
| DAC / PCM | **A**: `tests/genesis_audio_pcm_test.py` | n/a | **A**: RF5C164 PCM ([ADR 0251](../decisions/0251-seg-107-m11-rf5c164-pcm-audio.md), SEG-107; `tests/sega_cd_pcm_native_test.py`, `tests/sega_cd_pcm_audio_native_test.py`) |
| CD-DA | n/a | n/a | **A**: SEG-104, ADR [0244](../decisions/0244-seg-104-t001-cdda-format-timing-and-authentic-baseline.md)-[0248](../decisions/0248-seg-104-m8-decision-cdda-playback.md); `tests/sega_cd_cdda_native_test.py` |
| Mixer, PCM artifact, delivery | **A**: [ADR 0075](../decisions/0075-genesis-audio-mixer-pcm-artifact-and-viewer-presentation.md); `tests/genesis_audio_mixer_test.py`, `tests/genesis_audio_artifact_test.py` | **A**: viewer audio (ADR 0070) | **A**: CD-DA, PCM and FM/PSG on one guest-time 44.1 kHz timeline |
| Save states / rewind | **A** (SEG-131, [ADR 0278](../decisions/0278-genesis-save-states-persistence-and-rewind.md)) | **A** (SEG-132, [ADR 0279](../decisions/0279-master-system-and-sega-cd-save-states-and-rewind.md)) | **A** (SEG-132; bound to disc and BIOS) |
| Music/SFX/instrument/sample data edits | **B**: ROM data edits (typed edits and BPS, M03 authoring); Z80 driver edits only if statically materializable | **B** once an SMS target exists (SEG-147-T001) | **B** for data-track file/extent edits once the disc model exists (SEG-148-T001/T002) |
| Triggering sounds from scripts/native code | **C**: declared mapping and `audio.command.send` (section 5; SEG-145-T008) | **C**: same through the adapter (SEG-145-T013) | **C**: same (SEG-145-T013) |
| Platform integration still missing | **D**: none for guest-native; optional host audio (SEG-145-T009/T010) | **D**: SMS patching (SEG-147) | **D**: disc source model and CD-DA replacement (SEG-148-T001/T002, SEG-145-T005/T006) |
| Unsupported / unverified | arbitrary driver mutation that is not statically provable | **U**: YM2413 (see section 7), universal drivers | **U**: schema 1/2 representation (16 MiB image cap, 32 MiB archive cap, Genesis-only SDK); FM/PSG correctness until SEG-115 |

Hardware that is marked **A** is never a task. No row marks existing hardware as work to do; the **C** and **D** rows add integration, not devices.

## 3. What is common and what stays console-specific

**Common (the only overlap):** the *declared mapping* (a mod-authored description of an observable memory location or command mailbox, never a game format reader), the *audio asset class* ([schema 3](segapatch-schema-3.md) section 4), the *audio capability names* (section 5) and the *event phases* (section 5).
**Console-specific, never abstracted:** FM/DAC (YM2612) versus PSG versus RF5C164 versus CD-DA; sound driver formats and command protocols; the sound CPU timing; mixer composition on each platform; how a track is stored on a disc. There is no universal tracker, chip abstraction, sound-driver emulator or asset decompiler.

## 4. Audio asset declaration (fixed here for SEG-133-T002 and SEG-135)

An audio asset is an `asset` payload with `asset_class` `audio` ([schema 3](segapatch-schema-3.md) section 4): `audio_format` `wav-pcm-s16le`, `sample_rate` 8000..48000, `channels` 1 or 2, `frames`, optional `loop_start`/`loop_end` within `frames`.
The product validates **declared** format, size and checksum; it never interprets a game's native audio format. Deterministic conversion (SEG-145-T002) maps a declared source asset to the platform's declared target format by a fixed algorithm and records the output digest.
No schema revision is needed for later audio tasks: the class, fields and the `mappings` key exist in schema 3.

## 5. Capability names, mappings and legal phases

| Capability (ABI 2 id) | Meaning | Legal phases | Mutation |
|---|---|---|---|
| `audio.event.read` (13) | read the declared sound events the guest raised (derived from declared `event` mappings over memory the mod may already read) | `frame-end`, `render` | read-only |
| `audio.command.send` (14) | write a declared sound command into the game's declared command mailbox (a declared mapping over a work-RAM space; the write follows [phases](mod-execution-phases.md) section 4) | `frame-begin`, `post-input`, `hook-entry`, `hook-exit`, `frame-end` | mutates guest work RAM (staged) |
| `audio.host.play` (15) | enhanced host audio playback | reserved; unsupported until SEG-145-T009 GO | output only, outside guest time |

**Declared mapping shape** (the content model of the schema 3 `mappings` array; SEG-145-T002 implements the parser). Each element is an object with a token `id` and a `kind`:

* `command`: `{ "id", "kind":"command", "space", "offset", "width" (1|2|4), "commands":[{"name","value"}] }` - the mailbox and the named values `audio.command.send` may write; `space` is a writable space from the [adapter tables](mod-platform-adapters.md); `value` fits `width`.
* `event`: `{ "id", "kind":"event", "space", "offset", "width", "events":[{"name","value"}] }` - the observed location and the values that raise a named event.
* `asset-binding`: `{ "id", "kind":"asset-binding", "asset" (a declared audio asset path), "target" (token naming a declared replacement target) }` - binds an asset to a target defined by the platform task (data edit for SMS/Genesis; CD-DA/PCM kinds for Sega CD).

Unknown kinds, unknown space names, a `command` space that is not mod-writable and values wider than `width` are rejected at `mod check` (`E_FIELD_VALUE`, `E_SPACE_READONLY`). Mappings describe observable locations only; they never decode a game's driver.

## 6. Trust and licensing

Audio assets and mappings are data (trust class BPS-only/data); `audio.command.send` is guest-affecting and requires a script or native payload and the matching approval ([Luau policy](luau-trust-policy.md) section 1). Packages MUST NOT embed commercial audio, discs or BIOS images, and product tests use synthetic generated audio only. Enhanced host audio (if it ever ships) is presented explicitly and never alters guest state.

## 7. Master System YM2413

The Master System YM2413 FM unit is **unsupported hardware, outside this SDK milestone**: it is not modelled by the runtime (ports `$F0-$F2` do not decode) and no task creates it. A separate capability may be defined if a verified use case appears. Audio mods on the Master System are limited to PSG and Z80 data/driver edits.

## 8. Review against schema 3 and ABI 2

Checked against [segapatch-schema-3.md](segapatch-schema-3.md) and [native-sdk-abi-2.md](native-sdk-abi-2.md) in this delivery: the `audio` asset class and platform-scoped payload table exist in schema 3 section 4; the reserved `mappings` key points here; ABI 2 capabilities 13-15 and their phases equal section 5 above;
the Sega CD CD-DA mechanism is chosen by SEG-145-T005 inside the disc envelope owned by SEG-148-T001 ([sega-cd-mod-sources.md](sega-cd-mod-sources.md)). No clarification beyond the text of those documents is required; no incompatible later schema is needed.
