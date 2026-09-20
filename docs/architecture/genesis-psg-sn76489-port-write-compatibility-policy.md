# Genesis PSG (SN76489) audio-port write compatibility policy (SEG-007-T109)

## Status and boundary

This is a bounded **project compatibility policy**, not an authoritative Genesis / SN76489
hardware claim and not a verified audio model. It is built exclusively through
[the persistent device-state and checkpoint-evidence contract](genesis-persistent-device-state-and-checkpoint-evidence-contract.md)
(SEG-007-T042) §§1.1, 3, 8, and 10: it adds one new genuinely stateful subsystem
(`GenesisDeviceState.psg`) under §1.1's extension discipline, using only directly citable
public documentation and an explicit policy label for the behaviour that documentation does
not pin. It makes no new architecture decision of its own and follows the
[68k-side Z80 bus-arbitration control-register policy](genesis-z80-bus-arbitration-compatibility-policy.md)
(SEG-007-T102) and the
[flat Z80 RAM-window policy](genesis-z80-ram-window-compatibility-policy.md) (SEG-007-T103)
as its precedent shape: a single tight address interval, one small persistent state struct,
every validation before any mutation, and a labelled policy for the one ambiguity.

It applies only to the one co-located PSG audio port at the odd byte address **`$C00011`**,
reached through the runtime dispatch pair `genesis_is_psg_region` / `genesis_psg_access`
(`platforms/genesis/runtime/runtime.c`), routed from `genesis_route_access` in the position
**immediately before** the `genesis_is_vdp_region` block (the PSG address is inside that
interval). It names no private target address beyond the already-public `$C00011`.

**No audio synthesis of any kind exists in this path or anywhere else in this runtime** — no
tone or noise oscillator, no frequency divider, no attenuation ramp, no mixing, no sample
output, no PSG ready/busy line, no YM2612 / FM, and no Z80 view of the chip. The only
modelled effect is updating a small register-latch struct. No target instruction is fetched,
decoded, or inspected.

## Public sources

| ID | Source and locator | Facts used |
| --- | --- | --- |
| GTO1 | Sega Enterprises, *Genesis Technical Overview* v1.00 (1991), [Internet Archive PDF](https://ia801909.us.archive.org/24/items/Genesis_Technical_Overview_v1.00_1991_Sega_US/Genesis_Technical_Overview_v1.00_1991_Sega_US.pdf), [text derivative](https://ia801909.us.archive.org/24/items/Genesis_Technical_Overview_v1.00_1991_Sega_US/Genesis_Technical_Overview_v1.00_1991_Sega_US_djvu.txt), accessed 2026-08-29; **p. 10 "VDP AREA" address diagram** ("PSG 76489" at `$C00011`). | The 68000-side PSG port address. |
| SMSPOWER | SMS Power, "Development / SN76489", <https://www.smspower.org/Development/SN76489>, accessed 2026-08-29. | The full SN76489 write-command byte format (LATCH byte `%1cctdddd`, DATA byte `%0-DDDDDD`), the 4-bit attenuation range (0 loudest, 15 silent), and the 3-bit noise register layout (bit 2 feedback mode, bits 1-0 shift rate). Well-established secondary development reference, not Sega primary documentation. |
| PLUTIE-PSG | Plutiedev, "Programming the PSG", <https://plutiedev.com/psg>, accessed 2026-08-29. | Independent corroboration of the port address ("68000: at `$C00011`"), of write-only single-byte / two-byte access, of the volume command (`$90 \| channel<<5 \| attenuation`), of the two-byte tone command (`$80 \| channel<<5 \| (freq & 0x0F)` then `freq >> 4`), and of the documented noise command set `$E0`-`$E7`. |
| MCD1 | Charles MacDonald, *Sega Genesis hardware notes* v0.8, [`gen-hw.txt` SpritesMind mirror](https://gendev.spritesmind.net/mirrors/cmd/gen-hw.txt), accessed 2026-08-29. | The PSG is write-only ("Reading the PSG addresses will cause the machine to lock up"); the secondarily-attested odd mirror addresses (`$C00013` / `$C00015` / `$C00017`) and the secondary word-write / even-address quirk notes (neither modelled). Public secondary technical source, not Sega primary documentation. |

The port address is triangulated across GTO1 (primary) and PLUTIE-PSG. The command-byte
format is triangulated across SMSPOWER and PLUTIE-PSG (with MCD1 corroborating write-only
direction).

### Documented facts

1. **Port address.** The PSG (SN76489-compatible) audio port is at `$C00011` viewed from the
   68000 (GTO1 p. 10; PLUTIE-PSG).
2. **Direction.** The port is **write-only**. A read is not a defined operation (MCD1:
   reading the PSG addresses locks the machine).
3. **LATCH byte — bit 7 set — `%1cctdddd`.** `cc` (bits 6-5) selects one of four channels
   (0-2 tone, 3 noise); `t` (bit 4) selects the register: `1` = volume/attenuation, `0` =
   tone/noise. `dddd` (bits 3-0) is the 4-bit data: for a tone channel it is the low 4 bits
   of that channel's 10-bit period; for volume it is the 4-bit attenuation; for the noise
   channel it is the (3-bit) noise control (SMSPOWER; PLUTIE-PSG).
4. **DATA byte — bit 7 clear — `%0-DDDDDD`.** `DDDDDD` (bits 5-0) updates the register the
   last LATCH byte selected: for a tone channel it supplies the upper 6 bits of the 10-bit
   period; for volume/noise it supplies the low bits of the 4-bit attenuation / 3-bit noise
   control (SMSPOWER).
5. **Attenuation.** 4-bit, `0` = maximum output, `15` = silence (SMSPOWER).
6. **Noise register (channel 3, `t = 0`).** 3-bit: bit 2 = feedback mode (`0` periodic /
   `1` white), bits 1-0 = shift rate. The documented latch command set is `$E0`-`$E7`
   (SMSPOWER; PLUTIE-PSG).

### Project inference / policy (not a raw hardware quote)

- **`$C00011` only; odd mirrors excluded.** Only the primary-documented `$C00011` is
  recognised. MCD1's `$C00013` / `$C00015` / `$C00017` mirrors are only secondarily attested,
  exactly like the SEG-007-T103 Z80-RAM mirror, so they stay fail-closed rather than be
  folded in without direct evidence.
- **BYTE width only.** A WORD or LONG access to the odd address `$C00011` is already rejected
  by `genesis_route_access`'s odd-effective-address guard before this lane runs; the width
  check inside `genesis_psg_access` is defensive. MCD1's secondary "word write, data in LSB"
  and "even-address byte write has no effect" quirks are not modelled — the same
  ambiguity-rejection stance SEG-007-T103 took for 68000 word-width access to the 8-bit Z80
  area.
- **DATA byte before any LATCH byte fails closed.** The SN76489's power-on latched register
  is genuinely undefined across sources; `latch_valid` gates a DATA byte until a LATCH byte
  has been seen, and the runtime rejects the ambiguity rather than guess (the project charter,
  "reject ambiguity explicitly"). T042 §8 zero-initialisation gives `latch_valid = 0`.
- **Noise register is 3 bits; a channel-3 tone/noise LATCH byte with data bit 3 set fails
  closed.** No documented SN76489 command produces this encoding (the documented noise LATCH
  set is `$E0`-`$E7`), and the cited sources do not state its hardware disposition, so the
  runtime rejects it (fail-closed / honest stop) rather than silently pick "ignore" or
  "latch". This is a conservative, replaceable policy: it can only ever convert a would-be
  silent no-op into an explicit stop, never mis-model a documented command. Every other 8-bit
  command value is a defined SN76489 command (a LATCH byte with a fully-valid 4-bit data
  field for a tone channel or the volume register, or a DATA byte).

## Modelled behaviour

Persistent state is `GenesisPsgState` (`platforms/genesis/runtime/runtime.h`), nested in
`GenesisDeviceState.psg`, zero-initialised per T042 §8:

```c
typedef struct GenesisPsgState {
  uint8_t  latched_channel;   /* 0..3 */
  uint8_t  latched_volume;    /* 1 = volume register latched, 0 = tone/noise */
  uint8_t  latch_valid;       /* 1 once any LATCH byte has been seen since reset */
  uint16_t tone_period[3];    /* 10-bit period for tone channels 0..2 */
  uint8_t  attenuation[4];    /* 4-bit attenuation for channels 0..3 */
  uint8_t  noise_control;     /* 3-bit noise register for channel 3 */
} GenesisPsgState;
```

`genesis_is_psg_region(address)` recognises exactly `address == 0x00C00011` and nothing else.
It is routed from `genesis_route_access` immediately before the `genesis_is_vdp_region` block.

| Access | Result |
| --- | --- |
| **WRITE BYTE `$C00011`, LATCH byte (bit 7 set), valid encoding** | `latched_channel` / `latched_volume` updated, `latch_valid = 1`; then, per the selected register: `attenuation[cc] = dddd` (volume), or `tone_period[cc] = (tone_period[cc] & 0x3F0) \| dddd` (tone), or `noise_control = dddd & 0x07` (channel 3 tone/noise). The caller's value is never mutated. |
| **WRITE BYTE `$C00011`, DATA byte (bit 7 clear), `latch_valid == 1`** | per the last-latched register: `attenuation[ch] = DDDDDD & 0x0F` (volume), or `tone_period[ch] = (tone_period[ch] & 0x000F) \| (DDDDDD << 4)` (tone), or `noise_control = DDDDDD & 0x07` (channel 3). The caller's value is never mutated. |
| **WRITE BYTE `$C00011`, DATA byte, `latch_valid == 0`** | **fail-closed** device access: `GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS` / `GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_PSG`. |
| **WRITE BYTE `$C00011`, channel 3 tone/noise LATCH byte with data bit 3 set** | **fail-closed** as above (reserved encoding). |
| **any READ of `$C00011`** (BYTE) | **fail-closed** as above (write-only port). |
| **WORD / LONG access to `$C00011`** | rejected earlier by `genesis_route_access`'s odd-effective-address guard: `GENESIS_STOP_UNSUPPORTED_MEMORY_REGION` / `GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS`. The defensive width check in `genesis_psg_access` would otherwise fail it closed with the PSG diagnostic. |
| **any other address** (including `$C00010`, `$C00012`, the `$C00013`+ odd mirrors, `$C00020`) | unchanged: the pre-existing co-located VDP-lane fail-close (`GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP`) inside the `$C00000..$C0001F` window, or the generic `GENESIS_DIAG_UNMAPPED_DATA_ACCESS` outside it. |

Every validation check (address, direction, width, then command-byte encoding) precedes every
state mutation, so a rejected access is atomic: it modifies neither `*value` nor any
`GenesisRuntime` field, honouring `genesis_route_access`'s "on failure neither `*value` nor
the runtime is modified" contract (T042 §3).

## The policy statements

1. **`$C00011` only.** The odd mirror addresses are only secondarily attested and stay
   fail-closed.
2. **BYTE width only; write-only.** WORD/LONG and every read fail closed. The secondary
   word-write / even-address quirks are not modelled.
3. **DATA byte requires a prior LATCH byte.** Otherwise fail closed.
4. **Noise register is 3 bits.** A channel-3 tone/noise LATCH byte with data bit 3 set fails
   closed.
5. **Latch bookkeeping only — no audio.** The stored fields are never used to synthesise,
   time, mix, or emit anything.

## Intentional deviations / non-additions

1. **No device-step counter, no interrupt, no DMA, no status field.** The PSG port exposes no
   readable state to the 68000 in this model, so none is added (T042 §1.1 "add abstractions
   only when a current target exercises them").
2. **No `interrupt` nested struct.** Still SEG-007-T047's own responsibility to add.
3. **Power-on latched register is not modelled as a non-zero initial value.** `latch_valid`
   is zero-initialised per T042 §8; a DATA byte before any LATCH fails closed.

## What still fails closed / out of scope

The odd mirror addresses (`$C00013`+), every even address in the VDP window, WORD/LONG PSG
access, every PSG-port read, the reserved noise encoding, the Z80-side PSG address (`$7F11`),
the YM2612 (`$A04000`+), all audio synthesis / timing / mixing / output, and the PSG
ready/busy line.

## Evidence replacement rule

Only specifically scoped stronger evidence may replace or refine the `$C00011`-only interval,
the BYTE-only / write-only policy, the DATA-byte-requires-latch rule, the noise-bit-3
rejection, or the fail-closed boundary: it must directly cover this port and the exact
affected CPU-visible behaviour, record its provenance and limits, and receive independent
validation through a separately evidenced change. Absent that, this remains a labelled
project compatibility policy. Replacement must not silently upgrade this policy into an
authoritative hardware claim.

## Privacy and non-goals

This policy records no commercial-ROM address, byte, opcode, disassembly, trace, local path,
or derived fixture. It adds no decoder, CPU, emitter, or rendering behaviour. It makes no
VDP, rendering, interactive-input, audio-output, Z80-execution, or title-screen-completion
claim.
