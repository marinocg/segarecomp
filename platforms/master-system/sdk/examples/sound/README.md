# Master System original-hardware sound mod

A redistributable example (SEG-145-T004) over a **project-authored synthetic cartridge** with its own tiny PSG driver (`fixture_rom.py`). The driver's tables and layout are invented for this example: the
product assumes no common Master System song format or driver, and ships no game's offsets, sound ids or formats.

| Change | How |
| --- | --- |
| which note each step plays (command table) | a typed edit (`edits/main.edits`) |
| tone pitch, step volume, noise channel (the PSG's own parameter bytes) | typed edits, each asserting the exact original bytes |
| how the driver treats a volume | a routine injected by `asm/main.asm` into declared free space, and a typed edit that redirects the driver's call to it |

The injected routine is admitted by the Z80 owner's static decoder and compiled ahead of time with the cartridge. Port I/O is not admitted in injected code, so the cartridge's own driver stays the only
writer of the PSG; indirect jumps and code that runs off its block are refused too. Nothing is patched or decoded at run time.

```sh
python3 fixture_rom.py /tmp/sound.sms
segarecomp mod check .
segarecomp mod apply . --rom /tmp/sound.sms --out /tmp/sound-modded.sms
segarecomp mod test  . --rom /tmp/sound.sms --cc "$CC" --sdk-dir "$SDK"     # compares the PCM digest
segarecomp mod pack  . --out /tmp/sound.segapatch --rom /tmp/sound.sms
```

The headless program writes `audio.pcm` (s16le mono, 44,100 Hz) and `audio.sha256` with `--artifacts <dir>`; run it with `--frames <n>` as the bound. Compare the Vanilla and modded digests.

**The YM2413 FM unit is unsupported hardware**: it is not modelled and ports `$F0-$F2` do not behave as an FM unit, so a game's FM probe finds none and uses its PSG path
([audio architecture](../../../../../docs/architecture/modding-audio-architecture.md) section 7). There is no PCM sample channel adapter and no audio-command mapping on this platform yet.
