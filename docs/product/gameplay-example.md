# Gameplay example: a Luau mod that changes the game (Genesis, synthetic)

[`platforms/genesis/sdk/examples/gameplay`](../../platforms/genesis/sdk/examples/gameplay/README.md) is the redistributable example of a mod that changes **gameplay**, not only bytes, and draws a **HUD**. It uses only the official, restricted Luau API ([Luau scripting](luau-scripting.md)); the whole developer workflow runs in CI against a fabricated image (no commercial input): `tests/mod_gameplay_example_test.py` and `tests/mod_diff_run_test.py`. Decisions and limits: [ADR 0300](../decisions/0300-scheduled-mods-in-built-programs-diff-run-test-and-gameplay-example.md).

**The fixture.** `fixture_rom.py` writes a tiny runnable Genesis program (not a game): 90 one-frame turns; each turn it reads controller 1, adds 1 to the *score* word at work RAM `0xFF0010` while button B is held and 1 to the *position* word at `0xFF0012` while Right is held.

**The mod.** `scripts/main.luau`: in the `post_input` phase, while button C is held, add 10 to the score through `sega.mem.read` / `sega.mem.write` (big-endian 16-bit, work RAM); `render` draws a panel with how many bonuses were granted; `save()` / `load()` keep that counter in save states. `mod.toml` declares the capabilities (`memory.read/write.m68k.work_ram`, `input.read`, `overlay.rgba`, `host.log`, `state.mod`), the state schema and the **one** range the mod may touch:

```toml
[[memory]]
revision = "fixture"      # the exact revision this range is valid for
space = "m68k.work_ram"
offset = 16               # 0xFF0010
length = 2
access = "read-write"
```

A capability alone never authorizes an offset: the scheduler denies every access outside a declared range of the exact image being run, and a build with a memory capability but no range for that image is refused (`E_RANGE_DECLARATION`).

## The workflow

```sh
python3 fixture_rom.py fixture.rom
segarecomp mod init demo --platform genesis --id demo --template luau --rom-sha256 <sha256>   # a starting point for your own mod
segarecomp mod check   gameplay
segarecomp mod test    gameplay --rom fixture.rom --cc cc --sdk-dir <platforms-dir>              # the project's tests.toml scenarios
segarecomp mod diff-run gameplay --rom fixture.rom --cc cc --sdk-dir <platforms-dir> --frames 30 --budget 4000000 --timeout 120 \
                        --input "10:1:0x20,20:1:0x00" --ram 16:4                                  # Vanilla versus Modded
segarecomp mod pack    gameplay --out gameplay-example.segapatch
segarecomp mod install gameplay-example.segapatch --rom fixture.rom --library lib                # a Luau-only package: consent, not native approval
```

`diff-run` answers "what did my mod change?": the first frame at which the picture or a watched RAM region differs from Vanilla, the changed bytes, how many frames diverge and the mod's own summary. For the example it reports frame 11 (button C is keyed at frame 10), guest byte `0xFF0011` Vanilla `0` and Modded `10`. `--input` is a frame-keyed pad script `<frame>:<player 1|2>:<mask>[,...]` (masks: Up `0x01`, Down `0x02`, Left `0x04`, Right `0x08`, B `0x10`, C `0x20`, A `0x40`, Start `0x80`); `--aggregate` removes every byte value from the report. **Every run is bounded and explicit**: `--budget` (instructions), `--timeout` (seconds, each run is killed at it) and `--frames` (at most 64) are required; the build has a fixed 900 second limit.

## `tests.toml`

A project file that is never packed (and so never part of the package identity). One `[[scenario]]` per test: `name`, `frames`, `budget`, `timeout` (all required), `input`, `ram = ["<offset>:<length>", ...]`, `checkpoint = <frame>` (a save/restore round trip at that frame must be lossless) and expectations: `expect_ram_frames` (one lower-case hex string per frame, the watched regions concatenated), `expect_frames` (frame digests), `expect_frame`, `expect_state` (SHA-256 of the work RAM at the end), `expect_pcm`. A failing scenario reports its first divergence (`field`, `frame`, `expected`, `actual`) and the command exits 1 (`E_TEST_FAILED`); the report also prints the observed values, so a new expectation can be pinned. The example's per-frame RAM expectations are derived by hand from the two rules, not copied from a run.

## What it proves, and what it does not

CI checks, in a real generated-native program of the fixture: the pinned deterministic package (identity, content digest, bytes, bytecode digest) and its install class; the hand-derived gameplay effect and the independently derived final state; **replay** (the same recorded input reproduces frame digests, RAM, witness and HUD hash); **save/restore** (a state saved mid-run and loaded into a *fresh* program ends identically; corrupted and foreign states are refused); the HUD overlay request; failure isolation (an out-of-range write faults only the mod and the guest ends in the Vanilla state) and every refusal class.

It does **not** show the mod in the played viewer window yet: the SDL3 viewer program keeps the HUD-only seam, so a package that needs memory or input capabilities is refused there (`E_LIVE_CAPABILITY`). The HUD is hashed and listed in the run summary but not composited into the captured VDP frames. Function hooks and native ABI 2 providers are not linked into built programs. See ADR 0300.
