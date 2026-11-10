# Native GEMS replacement - validation (ADR 0291)

Commercial images are used locally and ephemerally (`docs/testing/commercial-games.md`): nothing below is a byte, address, trace, asset or disassembly; it is counts,
classes and pass / fail. CI never depends on them. The committed tests use synthetic data only (see section 6).

## 1. Identity and version

The four authorized local images - Cool Spot (primary), Aladdin, Earthworm Jim, Jurassic Park - upload a Z80 image with the **same activation signature**. The
signature covers the whole 8 KiB written extent, so the four games carry one and the same driver binary. Version 2.5: the address map, the interrupt / FIFO / DAC-routine
layout, the frequency tables, the sub-beat default and the position of the patch buffers match the published 2.5 driver's; the inline (not factored) delay / duration decoding
excludes 2.8, the stack initialisation and the compressed-sample hook exclude 2.0. No byte-exact original binary was available, so the evidence tier is
`address_map_and_tables` and the version is not claimed beyond it. Each game's *resources* (patch, envelope, sequence and sample tables) differ and are read from its own cartridge.

## 2. AOT-versus-replacement decision (Cool Spot)

| Variant | Result |
| --- | --- |
| Default build | `genesis_audio = degraded / structural_code_mismatch`, Z80 isolated at virtual frame 4, silent |
| `--z80-alternatives 1` (ADR 0259 mechanism, compiled alternatives) | Z80 isolated at frame 109: the alternatives covered the first mutation and not the next (a value the driver loads from RAM). Still degraded. |
| Static analysis of the image | 498 stores, 409 with unknown destination, 173 with unmodelled value, 11 exact candidates: not closable without RAM data-flow analysis (out of scope, ADR 0134 / 0291) |
| `--sound-driver gems` | The native replacement serves the image for the whole run (section 3) |

## 3. Reference comparison

Reference: the third-party GEMSPlay C port of the 2.5 driver (offline, not vendored, never linked into the product). Three places where the port deviates from the 2.5 assembly are
corrected in a scratch copy (a sub-beat reset at every sequence start that its author marks "not in actual code", the SFX flag of envelopes, the target of the mute event); the
assembly is the authority. `tools/gems_reference_harness.c` runs it on the guest clock, driven by exactly what the native run saw: the commands the driver consumed and the windows in
which the 68000 held the Z80 bus (work due in a hold waits for its end; a VBlank edge lost in a hold stays lost). `tools/gems_reference_check.py` runs every life of the native driver
(a `/RESET` starts the next one) and compares the two chip-access streams: **exact** on operation order, port and value, bounded skew on time.

Each game was played headless for the same instruction budget (about 100 s of game time), from power-on, with no input script:

| Game (local image) | Commands | Sequences started | Bus holds | Chip operations compared | DAC data writes | Identical | Skew |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Cool Spot | 352 | 70 | 11,675 | 2,117,208 | 504,454 | **yes** | 0 |
| Aladdin | 224 | 105 | 9,763 | 589,044 | 129,862 | **yes** | at most 1,046 ticks (20 µs) |
| Earthworm Jim | 464 | 226 | 16,146 | 1,446,677 | 336,161 | **yes** | 0 |
| Jurassic Park | 47 + 6 (two lives of the driver) | 21 + 2 started | 13,266 + 2,593 | 371,783 + 136,405 | 86,142 + 32,879 | **yes**, both lives | 0 |

In every run the driver ended bound, with no fault. The compared stream is everything the replacement writes to the YM2612 and the PSG: boot, stops and clipping, FM patch loads and
key on / off, frequency updates and pitch envelopes, PSG tone / noise and their envelopes, DAC set-up and every Timer A-paced DAC byte, looped / slow / compressed sample handling, and
voice allocation under contention. (Jurassic Park also streams some PCM from the 68000 itself; those accesses are not the driver's and are excluded.) The compared operations total about
4.66 million. Two things are *not* claimed: exact PCM identity with another implementation (the comparison is of chip accesses, which determine the PCM given the same chip model) and
cycle-accuracy against a real Z80 (events are placed on exact chip / interrupt times, not on the real driver's polling phase).

How the differences found on the way were resolved (each was a defect in the replacement or in the comparison, not a tolerance): a note-to-sample mapping slip for notes below C4 (found by
Earthworm Jim, which uses them: the original maps note `n < 48` to sample `n + 48`; fixed, with a unit test); Timer A is free-running on the chip (a write that clears the flag does not move the
next overflow) so the DAC pump takes the overflow time from the chip itself; at fast tempos a tick carries several sub-beats and each is a pass of its own; and four harness-model corrections
(free-running timer, chip clock granularity, bus holds, tie order).

## 4. Real-game behaviour

* Genuine 68000 sound commands are delivered through the FIFO; the replacement consumes them (read pointer advances), resolves its resources from the cartridge and drives the existing YM2612 / PSG.
* PCM (headless `SOUND_SUMMARY`, Cool Spot, same instruction budget): the original-path build is silent (no sample change); the replacement build is non-silent with 2,584,123 sample changes; the digest
  is identical across repeated runs and across a different runner slice cadence (`SEGARECOMP_PERF_SLICE`). The run ends by budget, not by a stop.
* The build's own 600-frame start-up run (recorded in `status.json`) met no fault in any of the four games.

## 5. Negative and robustness cases

Committed (`tests/gems_native_test.c`, `tests/genesis_native_gems_build_test.py`, launcher tests): unverified identity (a changed signature; the fixtures' own drivers) is refused at build time with a typed failure and no
executable; an unknown command id is consumed and ignored like the original; a sequence that runs off the cartridge, a resource outside it, a track count above 16, a FIFO pointer outside the ring and a
control-block number above 15 are each an explicit fault that leaves no further chip write and no fabricated acknowledgement; `/RESET` during playback restarts the driver from a clean state; a long bus hold
loses the interrupt edges inside it; sync-cadence invariance (one call vs. 777-tick slices: identical accesses); repeated builds produce an identical record; malformed options are rejected. Unexpected
structural mutation of the *original* path is the unchanged ADR 0134 fault isolation (`genesis_z80_build_pipeline_smc_test`).

## 6. Committed tests (synthetic data only)

`gems_native_tests`: boot; get-pointers / start / stop; the DAC FIFO's hand-derived sample counts (one-shot with the original's tail cut, low-note sample mapping, looped sample); FM note register map and
frequency; PSG tone note and loops; tempo including the multi-beat accumulator; mailbox store; the faults above. `genesis_native_gems_build_test`: the build record and the refusal of an unverified identity.
Launcher: the record reader, wording and cache identity (`launcher_policy_test`), the Details page through the real window (`launcher_audio_gui_test`).

## 7. Reproduce locally

```
segarecomp build --rom <game>.md --output out --cc cc --runtime-dir platforms/genesis --sound-driver gems
SEGARECOMP_AUDIO_WRITE_LOG=native.log out/game --instruction-budget 40000000
# a copy of GEMSPlay with the three scratch corrections, then:
cc -std=gnu11 -w -D__cdecl= -include string.h -I<GEMSPlay copy> -o harness tools/gems_reference_harness.c <GEMSPlay copy>/GemsPlay.c
python3 tools/gems_reference_check.py --rom <game>.md --native-log native.log --harness ./harness
```
