# Genesis 68k-side Z80 bus-arbitration control-register compatibility policy (SEG-007-T102)

## Status and boundary

This is a bounded **project compatibility policy**, not an authoritative Genesis-hardware claim or a
verified bus-arbitration/bus-timing model. It is built exclusively through
[the persistent device-state and checkpoint-evidence contract](genesis-persistent-device-state-and-checkpoint-evidence-contract.md)
(SEG-007-T042) §§1.1–1.2, §2.1–§2.3, §3, §4.1/§4.4, §8, and §10: it fills the `GenesisDeviceState.z80_bus`
seam that contract reserved for SEG-007-T043, using only directly citable public hardware documentation
and an explicit policy label for the one behaviour that documentation does not pin. It makes no new
architecture decision of its own.

It applies only to the two 68k-side Z80 bus-arbitration control registers — **BUSREQ (`$A11100`)** and
**RESET (`$A11200`)** — reached through the runtime dispatch pair `genesis_is_z80_bus_region` /
`genesis_z80_bus_access` (`platforms/genesis/runtime/runtime.c`), routed from `genesis_route_access` in the same
position `genesis_is_device` / `genesis_is_vdp_region` already occupy. It neither names nor derives any
private target address beyond the already-public `$A11100` / `$A11200` documented here.

**No Z80 CPU emulation of any kind exists in this path or anywhere else in this runtime** — no Z80
core, no runtime step-through, no JIT, no instruction fetch, and no inspection of any Z80 or 68000
program byte. The only modeled effect is latching three booleans and computing a deterministic
read-back from them.

## Public sources

| ID | Source and locator | Facts used |
| --- | --- | --- |
| GTO1 | Sega Enterprises, *Genesis Technical Overview* v1.00 (1991), [Internet Archive PDF](https://ia801909.us.archive.org/24/items/Genesis_Technical_Overview_v1.00_1991_Sega_US/Genesis_Technical_Overview_v1.00_1991_Sega_US.pdf), [text derivative](https://ia801909.us.archive.org/24/items/Genesis_Technical_Overview_v1.00_1991_Sega_US/Genesis_Technical_Overview_v1.00_1991_Sega_US_djvu.txt), accessed 2026-08-28; **p. 76 §4 "Z80 CONTROL"** and **p. 91** ("Z-80 OPERATION SEQUENCE" / "RESET Z-80"). Register-index table p. 76 entries "Z80 BUSREQ 76", "Z80 RESET 76". | See the itemised facts below. |

### Documented hardware facts (GTO1 p. 76 §4, p. 91)

1. **BUSREQ register address** is `$A11100`. **RESET register address** is `$A11200`.
2. **`$A11100` D8 (W):** `0` = "BUSREQ CANCEL", `1` = "BUSREQ REQUEST".
3. **`$A11100` D8 (R):** `0` = "CPU FUNCTION STOP / ACCESSIBLE" (the 68000 has the Z80 bus), `1` =
   "FUNCTIONING" (the Z80 is still running). The documented acquire sequence is: "(1) Write `$0100` in
   `$A11100` by using a WORD access. (2) Check to see that D8 of `$A11100` becomes 0. (3) Access to Z80
   AREA. (4) Write `$0000` in `$A11100` by using a WORD access."
4. **`$A11200` D8 (W):** `0` = "RESET REQUEST" (assert the Z80 `/RESET` line), `1` = "RESET CANCEL"
   (release it). GTO1 p. 91 restates this as "RESET ON: DATA `0H` (Word) → `$A11200`" and "RESET OFF:
   DATA `100H` (Word) → `$A11200`".
5. **BYTE access is documented for both registers:** "Access to `$A11100` can also be based on BYTE" and
   "Access to `$A11200` can also be based on BYTE."
6. "At the time of POWER ON RESET, the 68000 has access to the Z80 bus." "The Z80 is automatically reset
   during the MEGA DRIVE hardware's POWER ON RESET sequence."
7. GTO1 p. 91 Z-80 start-up shape: `(1) BUS REQ ON  (2) BUS RESET OFF  (3) 68K copies program into Z-80
   S-RAM  (4) BUS RESET ON  (5) BUS REQ OFF  (6) BUS RESET OFF`.

### Project inference (not a raw hardware quote)

- **BYTE bit lane.** GTO1 documents the control/status bit as **D8** and documents BYTE access as
  permitted, but does not restate the bit position for a BYTE transfer. On the MC68000, a BYTE access to
  an **even** address drives data lines D15–D8, so the documented **D8** bit is **bit 0** of the byte
  transferred at exactly `$A11100` / `$A11200`. This runtime therefore treats the BYTE request/reset/
  BUSACK bit as **D0 of the byte written or returned at the exact even register address**, and treats a
  BYTE access to the odd half (`$A11101` / `$A11201`) as not one of the documented registers
  (fail-closed). This is a standard MC68000 byte-lane fact applied to the documented D8 bit, labelled
  inference because GTO1 does not print it.

## Modeled behaviour

Persistent state is `GenesisZ80BusState { uint8_t bus_requested; uint8_t bus_granted; uint8_t
reset_asserted; }` (booleans, 0/1), nested in `GenesisDeviceState.z80_bus`, zero-initialized per T042
§8.

`genesis_is_z80_bus_region(address)` recognises the single tight interval `0x00A11100 <= address <
0x00A11300`, mirroring `genesis_is_vdp_region`'s single-interval fail-closed-lane style, and is routed
before `genesis_route_access`'s final `unmapped_data_access` fallthrough.

Request bit for a WORD access is **D8** (mask `0x0100`); for a BYTE access it is **D0** (mask `0x0001`).

| Access | Result |
| --- | --- |
| **WRITE WORD/BYTE `$A11100`** | request bit set ⇒ `bus_requested = 1` **and** `bus_granted = 1`; request bit clear ⇒ `bus_requested = 0` **and** `bus_granted = 0`. The caller's value is never mutated. |
| **READ WORD/BYTE `$A11100`** | returns a value whose BUSACK bit (D8 word / D0 byte) is `0` when `bus_granted`, `1` otherwise; **every other bit is `0`**. Side-effect-free. |
| **WRITE WORD/BYTE `$A11200`** | bit set ⇒ `reset_asserted = 0` (RESET CANCEL / released); bit clear ⇒ `reset_asserted = 1` (RESET REQUEST / asserted). The caller's value is never mutated. |
| everything else in the region | **fail-closed** as a device access: `GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS` / `GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_Z80_BUS`. This includes: any **LONG** access to either register; a **READ** of `$A11200` (write-only); a BYTE access to the odd half of either register address; and any other sub-address in `[0x00A11100, 0x00A11300)`. |
| an address outside the region | unchanged: the pre-existing `GENESIS_STOP_UNSUPPORTED_MEMORY_REGION` / `GENESIS_DIAG_UNMAPPED_DATA_ACCESS` result. |

Every validation check (width, exact address, direction) precedes every state mutation, so a rejected
access is atomic: it modifies neither `*value` nor any `GenesisRuntime` field, honouring
`genesis_route_access`'s "on failure neither `*value` nor the runtime is modified" contract (T042 §3).

## The one policy statement: immediate deterministic grant

GTO1 documents the *acquire protocol* (write BUSREQ, then poll D8 until it reads 0) but not *how long*
the poll takes or under what condition D8 clears — that is real hardware arbitration timing this project
does not model and this document does not claim.

**Policy:** a BUSREQ request is granted **deterministically and immediately** — `bus_granted` tracks
`bus_requested` with no delay and **no device-step count** — because there is no Z80 core executing to
contend for the bus. Consequently the first `$A11100` read following a `$0100`/`$01` BUSREQ write
already reports BUSACK granted (D8/D0 = 0), and the documented poll loop (GTO1 p. 76 step 2) exits on
its first iteration.

This is within the T042 contract's allowed envelope: §4.1 permits "a state transition caused by a
documented `BUSREQ`/`RESET` … access itself", and §4.4's worked "Allowed" example permits granting
BUSACK after the *N*-th documented access to the BUSREQ status address "regardless of which instruction
performed it". Immediate grant is the strongest form of that envelope (*N* = 0 — the grant is part of
the BUSREQ write itself), and it involves no CPU-PC, opcode, or polling-loop-shape recognition, which
§4.2 forbids. This runtime therefore does **not** add the `busreq_status_read_count` device-step counter
T042 §1.2 reserved: with immediate grant there is no "after *N* reads" progression to count.

Real open-bus / MC68000 prefetch fill of the unused read-back bits is **not** modeled; every non-BUSACK
bit reads back `0` (deterministic).

## Intentional deviations from the T042 §1.2 field list

1. **No `z80_ram[GENESIS_Z80_RAM_BYTES]` field and no Z80-RAM-window macro.** The 68k↔Z80 RAM window
   (`$A00000`–`$A0FFFF`) is SEG-007-T043 C4 scope and outside this task's bounded bus-arbitration family.
   Adding it now would violate this project's "add abstractions only when a current target exercises
   them" rule (the project charter, Scope Discipline). A later task adds it following T042 §1.1's extension
   discipline.
2. **No `busreq_status_read_count` device-step counter** — unused under the immediate-grant policy
   above.
3. **Power-on Z80-reset state is not modeled as an initial value.** GTO1 p. 76 notes the Z80 is reset
   during the console's power-on-reset sequence, but `reset_asserted` is zero-initialized (not asserted)
   per T042 §8, because no Z80 core exists and the canonical startup route drives `$A11200` explicitly
   before it would matter. This is a documented deviation, not a silent one.

## What still fails closed / out of scope

The Z80 RAM window (`$A00000`–`$A0FFFF`), the Z80 sound-chip (YM2612 / PSG) registers, TMSS /
`$A14000` system-control, the `$A11000` "MEMORY MODE" (D-RAM/ROM) register, and every address that is
not exactly `$A11100` / `$A11200` remain fail-closed or out of scope. No Z80 execution, memory image,
handshake-buffer contents, FM-clear timing (GTO1 p. 91's "26 ms"), or interrupt behaviour is modeled.

## Evidence replacement rule

Only specifically scoped stronger evidence may replace or refine the immediate-grant policy, the
deterministic read-back fill, the BYTE bit-lane inference, or the fail-closed boundary: it must directly
cover the same registers and the exact affected CPU-visible behaviour, record its provenance and limits,
and receive independent validation through a separately evidenced change. Absent that, this remains a
labelled project compatibility policy and the underlying arbitration timing remains unresolved.
Replacement must not silently upgrade this policy into an authoritative hardware claim.

## Privacy and non-goals

This policy records no commercial-ROM address, byte, opcode, disassembly, trace, local path, or derived
fixture. It adds no decoder, CPU, emitter, or rendering behaviour. It makes no VDP, rendering,
interactive-input, audio, Z80-execution, or title-screen-completion claim.
