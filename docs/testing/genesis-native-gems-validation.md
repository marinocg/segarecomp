# Native GEMS replacement - validation (ADR 0291)

Commercial images are used locally and ephemerally (`docs/testing/commercial-games.md`): nothing below is a byte, address, trace, asset or disassembly; it is counts,
classes and pass / fail. CI never depends on them. The committed tests use synthetic data only (see section 5).

## 1. Identity and version

The four authorized local images - Cool Spot (primary), Aladdin, Earthworm Jim, Jurassic Park - upload a Z80 image with the **same activation signature**. The
signature covers the whole 8 KiB written extent, so the four games carry one and the same driver binary. Version 2.5: the address map, the interrupt / FIFO / DAC-routine
layout, the frequency tables, the sub-beat default and the position of the patch buffers match the published 2.5 driver's; the inline (not factored) delay / duration decoding
excludes 2.8, the stack initialisation and the compressed-sample hook exclude 2.0. No byte-exact original binary was available, so the evidence tier is
`address_map_and_tables` and the version is not claimed beyond it. Each game's *resources* (patch, sequence and sample tables) differ and are read from its own cartridge.

## 2. AOT-versus-replacement decision (Cool Spot, current `main`)

| Variant | Result |
| --- | --- |
| Default build | `genesis_audio = degraded / structural_code_mismatch`, Z80 isolated at virtual frame 4 |
| `--z80-alternatives 1` (ADR 0259 mechanism, compiled alternatives) | Z80 isolated at frame 109: the alternatives covered the first mutation and not the next (a value the driver loads from RAM). Still degraded. |
| Static analysis of the image | 498 stores, 409 with unknown destination, 173 with unmodelled value, 11 exact candidates: not closable without RAM data-flow analysis (out of scope, ADR 0134 / 0291) |
| `--sound-driver gems` | Native replacement serves the image; digital sound effects play (section 3) |

## 3. Reference comparison (the commands are replayed; the chip accesses are compared)

Reference: the third-party GEMSPlay C port of the 2.5 driver (offline, not vendored, never linked into the product; its author's one non-original line - a sub-beat reset at
every sequence start, marked "not in actual code" - is removed from a scratch copy). `tools/gems_reference_harness.c` runs it on the guest clock with the commands the native
driver consumed (`SEGARECOMP_AUDIO_WRITE_LOG` `C` lines) and logs every YM2612 / PSG access; `tools/gems_reference_compare.py` compares the two logs: **exact** on
operation order, port and value, **bounded skew** on time.

| Game (local image) | Native start-up run (600-frame window) | Operations compared | Sequence identical | DAC data writes | Max skew |
| --- | --- | --- | --- | --- | --- |
| Cool Spot | 17 commands, 7 sequences; stops at an unsupported sample | 78,918 | **yes** | 19,617 | 0.09 ms |
| Aladdin | 11 commands, 3 sequences; stops at an FM patch | 32,212 | **yes** | 7,936 | 0.12 ms |
| Jurassic Park | 6 commands, 2 sequences; stops at an unsupported sequence event | 32,976 | **yes** | 8,192 | 2.4 ms (a bus hold across a VBlank, not modelled by the harness) |
| Earthworm Jim | 6 commands, 1 sequence; stops at an unsupported sample before any DAC byte | 256 | **yes** | 0 | 0 |

The compared stream includes the boot silencing, every stop's FM / PSG clipping, every note-on setup (Timer A reload, DAC on, channel pan), every Timer A-paced DAC byte and the
driver's tail-cut shutdown. The DAC bytes themselves are also the cartridge's sample bytes (unit test, synthetic data). Known, accepted differences: the reference calls its
refill routine inside its DAC routine ("not in actual code") where the real driver calls it from the main loop - the native driver does the latter, and the logs agree on every
operation compared; the native driver stops writing at its first unsupported operation and the reference does not, so the comparison range is the native log.
Exact PCM identity with another implementation is not claimed: the bounded time skew above would shift individual samples.

## 4. Real-game behaviour (Cool Spot)

* Genuine 68000 sound commands are delivered through the FIFO; the replacement consumes them (read pointer advances), resolves its resources from the cartridge and drives the existing YM2612 / PSG.
* PCM (headless `SOUND_SUMMARY`): the original-path build is silent (no sample change); the replacement build is non-silent with 20,603 sample changes; the digest is identical across repeated runs and
  across a different runner slice cadence (`SEGARECOMP_PERF_SLICE`). The game continues through the whole tested interaction (budget-bound run, no stop).
* The first operation outside the verified subset (a later sample with an unsupported property) is reported by the build and by the run as `unsupported_sample`; the replacement is then silent. No acknowledgement
  or chip write is invented for it.

## 5. Negative and robustness cases

Committed (`tests/gems_native_test.c`, `tests/genesis_native_gems_build_test.py`, launcher tests): unverified identity (a changed signature; the fixtures' own drivers) is refused at build time with a typed failure and no
executable; unsupported command, FM patch, unsupported sequence event, unmapped resource, invalid track count, invalid FIFO pointer - each an explicit fault that leaves no further chip write and no
acknowledgement; `/RESET` during playback restarts the driver from a clean state; a long bus hold loses the interrupt edges inside it; sync-cadence invariance (one call vs. 777-tick slices: identical accesses);
repeated builds produce an identical record; malformed options are rejected. Unexpected structural mutation of the *original* path is the unchanged ADR 0134 fault isolation (`genesis_z80_build_pipeline_smc_test`).

## 6. Reproduce locally

```
segarecomp build --rom <game>.md --output out --cc cc --runtime-dir platforms/genesis --sound-driver gems
SEGARECOMP_AUDIO_WRITE_LOG=native.log out/game --instruction-budget 12000000
python3 tools/gems_reference_compare.py native.log --emit-script cmds.txt 2> boot.txt
cc -std=gnu11 -w -D__cdecl= -include string.h -I<patched GEMSPlay> -o harness tools/gems_reference_harness.c <patched GEMSPlay>/GemsPlay.c
./harness <game>.md cmds.txt 600000000 ref.log "$(awk '{print $2}' boot.txt)"
python3 tools/gems_reference_compare.py native.log ref.log
```
