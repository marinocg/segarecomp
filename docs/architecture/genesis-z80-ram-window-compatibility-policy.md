# Genesis flat 68000-visible Z80 RAM-window compatibility policy (SEG-007-T103)

## Status and boundary

This is a bounded **project compatibility policy**, not an authoritative Genesis-hardware claim or a
verified Z80-bus/Z80-area access model. It is built exclusively through
[the persistent device-state and checkpoint-evidence contract](genesis-persistent-device-state-and-checkpoint-evidence-contract.md)
(SEG-007-T042) §§1.1–1.2, §2.1–§2.3, §3, §8, and §10: it realises the `GenesisZ80BusState.z80_ram`
field and the `GENESIS_Z80_RAM_BYTES` macro that contract reserved as named placeholders (§1.2, §2.4),
using only directly citable public hardware documentation and an explicit policy label for the one
behaviour that documentation does not pin (68000 word-width access to the 8-bit Z80 area). It makes no
new architecture decision of its own and builds directly on the
[68k-side Z80 bus-arbitration control-register compatibility policy](genesis-z80-bus-arbitration-compatibility-policy.md)
(SEG-007-T102): the window is gated on that policy's `bus_granted` latch.

It applies only to the flat 68000-visible Z80 program-RAM window — window base **`$A00000`**, size
**`GENESIS_Z80_RAM_BYTES` = 8192** bytes — reached through the runtime dispatch pair
`genesis_is_z80_ram_window_region` / `genesis_z80_ram_window_access` (`platforms/genesis/runtime/runtime.c`),
routed from `genesis_route_access` in the position immediately after the `genesis_is_z80_bus_region`
block and before the final `unmapped_data_access` fallthrough. It names no private target address
beyond the already-public `$A00000` window base documented here.

**No Z80 CPU emulation of any kind exists in this path or anywhere else in this runtime** — no Z80
core, no runtime step-through, no JIT, no instruction fetch, and no inspection of any Z80 or 68000
program byte. The only modeled effect is a flat byte-array read/write into
`GenesisZ80BusState.z80_ram`.

## Public sources

| ID | Source and locator | Facts used |
| --- | --- | --- |
| GTO1 | Sega Enterprises, *Genesis Technical Overview* v1.00 (1991), [Internet Archive PDF](https://ia801909.us.archive.org/24/items/Genesis_Technical_Overview_v1.00_1991_Sega_US/Genesis_Technical_Overview_v1.00_1991_Sega_US.pdf), [text derivative](https://ia801909.us.archive.org/24/items/Genesis_Technical_Overview_v1.00_1991_Sega_US/Genesis_Technical_Overview_v1.00_1991_Sega_US_djvu.txt), accessed 2026-08-28; **overview p. 2**, **68K memory map p. 7**, **p. 76 §4 "Z80 CONTROL"**, **p. 77** (Z80 area map / access), **p. 91** ("Z-80 OPERATION SEQUENCE"). | Facts 1–4 below. |
| MCD1 | Charles MacDonald, *Sega Genesis hardware notes* v0.8 (03/02/01), [`gen-hw.txt` SpritesMind mirror](https://gendev.spritesmind.net/mirrors/cmd/gen-hw.txt), accessed 2026-08-28; **§1 "68000 memory map"**, **§1.2 "Memory access quirks"**, **§2 "Sound hardware overview"**, **§2.1 "Z80 memory map"**, **§2.2 "RESET and BUSREQ registers"**. Public secondary technical source, not Sega primary documentation and not an independently qualified hardware oracle. | Corroborates facts 1–4; supplies the secondary word-write quirk note (fact 5). |

### Documented hardware facts

1. **Z80 RAM size is 8 KiB.** GTO1 overview p. 2 and 68K memory map p. 7 place an "8 KByte" Z80/sound
   RAM at `$A00000`. MCD1 §2 states "8k static RAM". `GENESIS_Z80_RAM_BYTES` is therefore `8192`.
2. **Window base and Z80-area range.** GTO1 p. 7 lists `$A00000` as the SOUND RAM base; GTO1 p. 77 and
   MCD1 §1 give the Z80 address space as `$A00000`–`$A0FFFF` viewed from the 68000.
3. **The 68000 must hold the Z80 bus to access the Z80 AREA.** GTO1 p. 76 §4: "(1) Write `$0100` in
   `$A11100` by using a WORD access. (2) Check to see that D8 of `$A11100` becomes 0. (3) Access to Z80
   AREA. (4) Write `$0000` in `$A11100` by using a WORD access." MCD1 §2.2: "The Z80 bus can only be
   accessed by the 68000 when the Z80 is running and the 68000 has the bus."
4. **BYTE access is documented.** GTO1 p. 77: "Access from 68000 by BYTE." GTO1 p. 91's Z-80 start-up
   sequence step 3 is "68K copies program into Z-80 S-RAM" — a plain byte-stream copy.
5. **Secondary word-write quirk (not modeled).** MCD1 §1.2: "When doing word-wide writes to Z80 RAM,
   only the MSB is written, and the LSB is ignored." This is a secondary-source report, covers writes
   only, and says nothing about word-read behaviour.
6. **Z80-RAM mirror (not modeled).** MCD1 §2.1 Z80 memory map: "`0000-1FFFh` : RAM" / "`2000-3FFFh` :
   RAM (mirror)". MCD1 does not state whether this mirror is visible through the 68000 `$A00000`
   window, and GTO1 does not restate it.

### Project inference (not a raw hardware quote)

- **BYTE-only width policy.** Public documentation does not unambiguously pin the 68000's *word* and
  *long* access semantics against the 8-bit Z80 area: GTO1 documents BYTE access (fact 4); MCD1's
  word-write MSB-only note (fact 5) is a secondary-source, write-only report with unspecified read
  behaviour. Per the project charter ("reject ambiguity explicitly"), this runtime models **BYTE width only**
  and fails WORD, LONG, and any invalid width closed. The runtime frontier for this window is
  byte-only; a stronger, directly citable width fact may extend it under the evidence-replacement rule
  below.
- **Bus-grant gate.** Fact 3 is modeled as: `genesis_z80_ram_window_access` fails closed unless
  `GenesisZ80BusState.bus_granted` (the SEG-007-T102 immediate-grant latch) is set. The window is not a
  free RAM region; it is only reachable while the 68000 holds the Z80 bus.
- **Mirror exclusion.** `genesis_is_z80_ram_window_region` recognises exactly `[0x00A00000,
  0x00A00000 + GENESIS_Z80_RAM_BYTES)` and nothing else. Fact 6's mirror is only secondarily attested
  and its 68000-side visibility is unstated, so folding it in (with address folding before indexing)
  without direct evidence would be a guess; it stays fail-closed.

## Modeled behaviour

Persistent state is `GenesisZ80BusState.z80_ram[GENESIS_Z80_RAM_BYTES]` (a flat `uint8_t` array),
nested in `GenesisDeviceState.z80_bus`, zero-initialized per T042 §8.

`genesis_is_z80_ram_window_region(address)` recognises the single tight interval `0x00A00000 <=
address < 0x00A00000 + GENESIS_Z80_RAM_BYTES`, mirroring `genesis_is_z80_bus_region`'s single-interval
fail-closed-lane style, and is routed immediately after the `genesis_is_z80_bus_region` block.

`offset = address - 0x00A00000`, bounds-checked `offset < GENESIS_Z80_RAM_BYTES` (defensive; the
predicate already guarantees it).

| Access | Result |
| --- | --- |
| **WRITE BYTE, `bus_granted == 1`, in window** | `z80_ram[offset] = (uint8_t)(*value & 0xFF)`. The caller's `*value` is never mutated. |
| **READ BYTE, `bus_granted == 1`, in window** | `*value = z80_ram[offset]`. Side-effect-free. |
| **any access with `bus_granted == 0`** | **fail-closed** device access: `GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS` / `GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_RAM`. |
| **WORD / LONG / invalid width, in window** | **fail-closed** as above. |
| everything else in the region matched by the predicate | there is nothing else — the predicate is exactly the 8 KiB window. |
| an address outside the window (including the mirror address `$A02000`) | unchanged: the pre-existing `GENESIS_STOP_UNSUPPORTED_MEMORY_REGION` / `GENESIS_DIAG_UNMAPPED_DATA_ACCESS` result. |

Every validation check (bus grant, then width, then offset bound) precedes every state mutation, so a
rejected access is atomic: it modifies neither `*value` nor any `GenesisRuntime` field, honouring
`genesis_route_access`'s "on failure neither `*value` nor the runtime is modified" contract (T042 §3).

## The policy statements

1. **BYTE width only.** WORD and LONG access to the window fail closed; the public sources do not
   unambiguously pin 68000 word-width semantics against the 8-bit Z80 area, and the ambiguity is
   rejected rather than guessed.
2. **Bus-grant gate.** The window is reachable only while `bus_granted` is set (fact 3); otherwise it
   fails closed.
3. **Flat, un-mirrored 8 KiB.** The window is exactly `GENESIS_Z80_RAM_BYTES` bytes of flat storage
   with no mirror folding.
4. **No Z80 semantics.** The stored bytes are never fetched, decoded, executed, or interpreted; the
   window is a byte-stream copy target only.

## Modeled behaviour table (state transitions)

| Precondition | Access | `z80_ram` effect | `*value` effect |
| --- | --- | --- | --- |
| `bus_granted == 1` | BYTE WRITE `$A00000+k`, `k < 8192` | `z80_ram[k] <- value & 0xFF` | none |
| `bus_granted == 1` | BYTE READ `$A00000+k`, `k < 8192` | none | `value <- z80_ram[k]` |
| any | WORD/LONG/invalid width in window | none | none (fail closed) |
| `bus_granted == 0` | any width/direction in window | none | none (fail closed) |

## Intentional deviations from the T042 §1.2 field list

1. **`z80_ram[GENESIS_Z80_RAM_BYTES]` is now added** — this is the field T042 §1.2 reserved and
   SEG-007-T102 deliberately deferred; `GENESIS_Z80_RAM_BYTES` is defined here with its cited size
   (fact 1), realising the named placeholder T042 §2.4 left open.
2. **`busreq_status_read_count` is still NOT added.** It remains unused under SEG-007-T102's
   immediate-grant policy (there is no "after N reads" progression to count). A later task adds it when
   it first needs it, per T042 §1.1's extension discipline.
3. **Power-on Z80-reset / RAM-content state is not modeled.** `z80_ram` is zero-initialized per T042
   §8; no documented power-on RAM pattern is asserted.

## What still fails closed / out of scope

- **WORD and LONG width** to the window (policy statement 1).
- **The Z80-RAM mirror region** (`$A02000`–`$A03FFF`) and every other address in `$A00000`–`$A0FFFF`
  above the 8 KiB window.
- **The YM2612 (`$A04000`+), the `$A06000` bank-address register, and the PSG (`$A07F11`)** — Z80
  sound-chip / bank registers, entirely out of scope.
- **Z80 execution** of the copied bytes: no core, decode, interpreter, JIT, or instruction fetch.
- **DMA visibility** of `z80_ram` (VDP DMA from the Z80 area) and any handshake-buffer / interrupt
  semantics.
- **The `bus_granted == 0` open-bus value**: a rejected access returns no value at all (fail closed),
  not a modeled open-bus pattern.

## Evidence replacement rule

Only specifically scoped stronger evidence may replace or refine the BYTE-only width policy, the
bus-grant gate, the mirror exclusion, or the fail-closed boundary: it must directly cover this window
and the exact affected CPU-visible behaviour, record its provenance and limits, and receive
independent validation through a separately evidenced change. Absent that, this remains a labelled
project compatibility policy and the 68000 word-width semantics against the Z80 area remain
unresolved. Replacement must not silently upgrade this policy into an authoritative hardware claim.

## Privacy and non-goals

This policy records no commercial-ROM address, byte, opcode, disassembly, trace, local path, or
derived fixture. It adds no decoder, CPU, emitter, or rendering behaviour. It makes no VDP, rendering,
interactive-input, audio, Z80-execution, or title-screen-completion claim.
