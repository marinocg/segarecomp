# ADR 0323: Genesis original-hardware sound mods (the verification gate) and the Luau / C11 audio API

- Status: accepted (SEG-145-T003 and SEG-145-T008, combined delivery F16). Contracts: [modding-audio-architecture.md](../architecture/modding-audio-architecture.md) section 10, [native-sdk-abi-2.md](../architecture/native-sdk-abi-2.md). User page: [Genesis sound mods](../product/genesis-sound-mods.md).

## Decision 1: original-hardware sound mods need no new tool, only a gate

Track, instrument, SFX, sample, command-table and selection changes are ordinary typed edits, BPS images or injected guest code (ADR 0290, 0292, 0293) of the exact declared image; the sound chips and the generated-native Z80 are unchanged.
What was missing is a verdict: a modified Z80 driver that the product cannot compile statically (ADR 0073 / 0134 / 0135) does not fail an ordinary build, it isolates the sound CPU and reports `genesis_audio: degraded`.
`segarecomp mod diff-run --audio` builds Vanilla and Modded programs with the unmodified product and refuses, with stable classes: `E_AUDIO_BASELINE` (Vanilla's sound is not `supported`/`native`, so nothing can be verified against it),
`E_AUDIO_STATIC_MATERIALIZATION` (the modded program's sound is degraded, or its build fails in the Z80 materialization or sound-driver stage), `E_AUDIO_IMPLEMENTATION_CHANGED` (the build selects another sound implementation, so PCM is not comparable)
and `E_AUDIO_NONDETERMINISTIC` (two identical modded runs give different PCM). Ordinary builds are unchanged: they never interpret the driver, and keep reporting the degraded state.

Rejected: failing every modded build whose audio is degraded (a build alone has no Vanilla baseline, so a title whose original sound is already degraded would reject every unrelated mod); parsing or validating any game's sound format (title knowledge).

## Decision 2: a three-call command API plus one level read, resolved only by declared names

* `play_sfx(name)` is a value of the command mapping with id `sfx`; `play_music(name)` of the mapping `music`; `stop_music()` the value named `stop` of `music`. `event(mapping)` returns the first declared name of an event mapping whose value the observed location holds now, or nothing. Luau: `sega.audio.play_sfx/play_music/stop_music/event`. C11: `SegaModAudioApi` (platform-neutral, in `segarecomp_mod.h`) reached through `SegaGenesisFeatures::audio`, ABI minor 4. Both go through the same scheduler services, so phase, capability, mapping and name rules and the fault causes are identical.
* A command is an ordinary **staged work-RAM write** of the declared value into the declared mailbox (phases section 4): committed with the callback, discarded on a fault or a declined replacement, counted in the write budget and the replay witness. It needs no `[[memory]]` range (the mapping is the declared location) and grants no other access. The mailbox holds one value: the SDK never queues, retries, acknowledges or knows what the game does with it.
* `event` is a level read, not a queue or an interrupt. In `render` it sees a copy of the declared locations only, taken after `frame-end` (`audio.event.read` grants no work-RAM read).
* **No volume call.** No platform or mapping exposes a verified volume control; the capability is absent rather than guessed. **No generic `send(mapping, name)`**: the two channels cover the verified shape without inventing channel semantics; a later channel is an additive decision.
* **No verified interface, no capability.** A scheduled build refuses (`E_AUDIO_TARGET`) a required `audio.command.send` without an `sfx` or `music` command mapping, a required `audio.event.read` without an event mapping, a command mapping with another id, a mapping outside `m68k.work_ram`, or more than 128 declared values. An optional audio capability without a mapping is simply not granted. The scheduler repeats the checks at attach (`E_AUDIO_MAPPING` for a malformed row). `mod check` additionally rejects literal Luau names that no mapping declares (`E_AUDIO_TARGET`, `E_AUDIO_NAME`); computed names are checked at run time.
* Run-time causes (the `E_MOD_FAULT cause=` field): `capability`, `phase`, `audio-target`, `audio-name`, `range` (a NULL argument), `budget`. `SegaModStatus` gains `SEGA_MOD_E_AUDIO_TARGET` (9) and `SEGA_MOD_E_AUDIO_NAME` (10).
* Command and event mappings decide which guest bytes a mod writes and observes, so they enter the package content digest and the save-state compatibility digest (both only when declared: other digests are unchanged).

## Consequences

Command and event mappings have no per-revision field: a package with several revisions must have the mailbox at the same place in each (otherwise ship one package per revision). Genesis Z80 RAM is not mod-writable, so a game whose only command interface is Z80 RAM needs guest code (a 68000 shim reading a work-RAM mailbox) first. Master System and Sega CD bindings are SEG-145-T013 over the same types.
