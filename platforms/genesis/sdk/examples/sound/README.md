# Genesis sound-mod examples (synthetic fixture)

`fixture_rom.py` writes a tiny **project-authored** Genesis program (no game, no vendor code, no commercial sound data) with its own data-driven Z80 sound driver: PSG note sequences, an effect, DAC samples and an FM instrument, selected through a command table. The game logic asks for sounds through two work-RAM mailboxes. `layout()` returns the ROM offset of every editable item.

| Directory | What it shows |
|---|---|
| `data/` | original-hardware sound mods as asserted ROM edits: track data, effect sequence, DAC samples, FM instrument, command table, selection |
| `luau/` | the audio API from Luau: effects on input, music change and stop, a read-only sound event |
| `native/` | the same behaviour as a C11 ABI 2 provider (`abi.minor = 4`) |

```sh
python3 fixture_rom.py /tmp/sound.md
segarecomp mod check luau
segarecomp mod diff-run luau --rom /tmp/sound.md --cc cc --sdk-dir <platforms-dir> --frames 64 --budget 20000000 --timeout 120 \
  --input "10:1:0x20,12:1:0x00,20:1:0x10,22:1:0x00,32:1:0x08,34:1:0x00" --ram 32:3 --audio
```

Documentation: [Genesis sound mods](../../../../../docs/product/genesis-sound-mods.md). Tests: `tests/mod_sound_workflow_test.py`, `tests/mod_audio_api_test.py`, `tests/genesis_mod_audio_test.cpp`.
