# Genesis sound mods and the audio API (developer preview)

Two ways to change a Genesis game's sound, both through the game's **own** sound hardware and driver. Nothing here is a new sound engine, a chip or driver abstraction or a tracker, and nothing knows any game's sound format ([audio architecture](../architecture/modding-audio-architecture.md), ADR [0323](../decisions/0323-genesis-sound-mod-gate-and-audio-api.md)).

1. **Original-hardware sound mods** change the game's sound data or code in the ROM.
2. **The audio API** lets a Luau script or a C11 (ABI 2) provider ask the game for sounds it already knows, only through mappings the mod declares.

Enhanced host audio (mod-authored digital audio played by the host) does not exist.

The runnable examples use a project-authored synthetic ROM with its own tiny Z80 driver: [`platforms/genesis/sdk/examples/sound`](../../platforms/genesis/sdk/examples/sound/README.md).

## 1. Original-hardware sound mods

| Change | Mechanism |
|---|---|
| replace a track's or an effect's data, instrument/voice data, DAC samples, a command mapping table | typed, expected-value-asserted [ROM edits](mod-projects.md#rom-edits-and-symbols) (`rom-edit-set`), or the same change as a `bps-image` |
| change which sound the game selects | a typed edit of the selecting instruction, or [injected guest code](mod-projects.md#guest-code-injection-header-repair-and-bps-adr-0292-adr-0293) (`guest-asm` in declared free space) |
| modify the Z80 driver code or its image | the same edits. The modified driver must pass the existing static materialization rules ([ADR 0073](../decisions/0073-genesis-build-time-z80-materialization-and-ram-backed-code.md), [0134](../decisions/0134-genesis-z80-structural-code-variants-no-go.md), [0135](../decisions/0135-static-first-z80-mutation-proof-feasibility.md)). It is compiled ahead of time or its sound CPU is isolated (which the gate rejects); it is **never** interpreted and no Z80 code is decoded at run time |

Every edit asserts the original bytes first: a mismatch is `E_EDIT_EXPECTED_MISMATCH` and nothing is written.
You have to find your own sites: the product ships no game's offsets, sound ids or formats.

### Verify it

```sh
segarecomp mod diff-run <project|package> --rom <original> --cc <compiler> --sdk-dir <platforms-dir> \
  --frames 64 --first <frame> --budget <instructions> --timeout <seconds> [--input <pad-script>] --audio [--aggregate]
```

It builds Vanilla and Modded programs with the unmodified product, runs both headless with the same input and compares frames, state and the canonical PCM digest (`pcm_sha256`: 44,100 Hz stereo s16, [ADR 0075](../decisions/0075-genesis-audio-mixer-pcm-artifact-and-viewer-presentation.md); it covers the run from reset to the end of the window). `pcm_differs` says whether your change was audible in the window you chose: choose `--first` and `--input` so that your sound plays there.
`--audio` adds the sound gate and an `audio` block to the report. The modded program is run twice.
The gate judges two things: what the build observed (a bounded run with **no pad input** and no mod command) and what **the compared runs** did (their own sound-fault state). A path neither of them executes is **not verified**: give `--input`, `--first` and `--frames` that make every sound you changed actually play, and run the command once per such scenario.

| Refusal | Meaning |
|---|---|
| `E_AUDIO_STATIC_MATERIALIZATION` | the modified sound code or image cannot be compiled statically: the modded program's sound is `degraded` (for example `structural_code_mismatch`), its build failed while producing the Z80 code (see the build log), or its sound CPU was isolated by a code fault during a compared run |
| `E_AUDIO_RUNTIME_FAULT` | the modded program's sound faulted during a compared run for another reason (the mixer, a native replacement) |
| `E_RUN_INCOMPLETE` | the program stopped before the requested frames. On Genesis a Z80 access or fetch the product cannot serve (an unsupported address, for example) stops the **whole program**, so a driver change can end here. Do not shorten the window to get past it: that only leaves the failing path unexecuted and unverified |
| `E_AUDIO_MOD_FAULT` | the mod itself faulted during the run, so its sound was not verified |
| `E_AUDIO_BASELINE` | the original program's sound is not working in this product (at build time or during the compared run), so there is nothing to verify a sound mod against |
| `E_AUDIO_IMPLEMENTATION_CHANGED` | the modded program selects another sound implementation than the original (their PCM is not comparable) |
| `E_AUDIO_NONDETERMINISTIC` | two identical runs of the modded program produced different PCM |
| `E_RUN_FAILED` | the evidence is missing or malformed (a build's sound status or implementation identifier, or a run's sound, mixer or native-replacement fault state or PCM digest): nothing was verified |

**An ordinary build does not refuse a driver it cannot compile**: it keeps the game running with the sound CPU isolated. It reports `genesis_audio: degraded` only when its own observation reaches the fault; a fault on a later path shows only while that path runs. Run the gate, with scenarios that play your sounds, before you publish a sound mod.

### Checking against your own commercial image (local only)

CI uses the synthetic fixture only. To check a sound mod of a game you legally own, locally:

1. Keep the image under the ignored `games/` directory (or anywhere outside the repository) and write your project **outside the repository**: it names your image's digest and your own sites.
2. First prove the baseline with a change that no sound path reads (for example one asserted byte of a header name): `mod diff-run <project> --rom <image> --audio --aggregate --first <a frame where the game plays sound> --frames 64 --budget <enough to reach it> --timeout <seconds> --cc <cc> --sdk-dir platforms --work <a directory outside the repository>`. Expect `ok`, `vanilla_status` and `modded_status` equal and not `degraded`, `repeat_identical: true`, `pcm_differs: false`.
3. Then make your sound edit and run the same command. Expect `ok` and `pcm_differs: true`.
4. Record only aggregates (the classes, statuses and booleans above). Never publish offsets, bytes, digests of game audio, PCM, the work directory or the generated programs.

## 2. The audio API

A mod declares **where** the game takes sound commands and **what** the values mean. Names resolve only through these declarations:

```toml
[[audio.mapping]]
id = "sfx"                 # the effect channel; "music" is the music channel
kind = "command"
space = "m68k.work_ram"    # the game's own mailbox in work RAM
offset = 32
width = 1

[[audio.mapping.value]]
name = "drum"
value = 4

[[audio.mapping]]
id = "accepted"            # a location to observe
kind = "event"
space = "m68k.work_ram"
offset = 34
width = 1

[[audio.mapping.value]]
name = "drum"
value = 4
```

Declare `audio.command.send` for commands (and `audio.event.read` for events), `state.mod` for save states and rewind.

| Luau | C11 (`features->audio`, `abi.minor = 4`) | Effect |
|---|---|---|
| `sega.audio.play_sfx(name)` | `audio->play_sfx(name)` | writes the value `name` of mapping `sfx` |
| `sega.audio.play_music(name)` | `audio->play_music(name)` | writes the value `name` of mapping `music` |
| `sega.audio.stop_music()` | `audio->stop_music()` | writes the value named `stop` of mapping `music` |
| `sega.audio.event(mapping)` (`string?`) | `audio->read_event(mapping, out)` (`""` = none) | the first declared name whose value the location holds now |

* Commands run in `frame-begin`, `post-input` and `frame-end` (C11 hooks also `hook-entry`/`hook-exit`); events in `frame-end` and `render` (an exact-frame C11 provider's `render` observes the previous-onset copy and needs `frame.work_ram` as well; before that copy exists it sees no event).
* A command is an ordinary staged write: it is committed when the callback returns without a fault, takes no guest time and is part of replay and save states. It only works if the game reads that mailbox.
* The mailbox holds one value and there is no queue or acknowledgement. Two commands to the same channel **within one successful callback** leave the last value. Commands from **different callbacks** (for example `post-input` and `frame-end`, or two hooks) are committed separately at each callback's end, and the guest runs between some of them: its driver may consume the first before the second is written, or a later one may overwrite it first. When and how often a command is consumed is the game's own driver's behaviour.
* If the game has no work-RAM command mailbox, add one first with a guest-code mod. Without a verified mapping there is **no** capability: a build that **requires** it is refused (`E_AUDIO_TARGET`); an **optional** one is simply not granted (mappings the API cannot serve are ignored), and the build status, the program and `MOD_LOADED` do not list it. A C11 provider checks `sega_mod_is_granted`; a Luau script that reaches for an absent binding faults with cause `capability`. The played program's HUD-only route (a package that requires nothing beyond `host.log` and `overlay.rgba`) serves no audio, so an optional audio capability is not granted there; the headless build of the same package grants it. There is no volume call.
* A wrong name or a call in a wrong phase faults only that mod (`E_MOD_FAULT cause=audio-name|audio-target|phase|capability|range|budget`); nothing of that callback is committed and the game continues unmodified. `mod check` catches literal names before that.
* Command and event mappings apply to every declared revision of the package; if the mailbox moves between revisions, ship one package per revision.

## Limits

Genesis only (Master System and Sega CD bindings are later work). Verified on the synthetic fixture: no claim is made about any commercial game's sound data, mailbox or driver. The headless comparison window is at most 64 frames.
