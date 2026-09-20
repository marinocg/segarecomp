# Genesis YM2612 PART-I status-port BYTE-read compatibility policy (SEG-007-T171)

## Status and boundary

This is a bounded **project compatibility policy**, not an authoritative Genesis / YM2612
hardware claim and not a verified FM-synthesis model. It is the device's *first* owner in this
project: `platforms/genesis/runtime/runtime.c`'s own PSG documentation previously stated "NO YM2612/FM" is
modelled, and `m68k_route_genesis_device_access` (`platforms/genesis/machine/src/address_space.cpp`) had no
YM2612 arm before this task. It follows the
[PSG (SN76489) audio-port write compatibility policy](genesis-psg-sn76489-port-write-compatibility-policy.md)
(SEG-007-T109) and the
[SEG-007-T111 CTRL3 BYTE-read compatibility policy](genesis-controller-io-startup-read-compatibility-policy.md)
as its direct structural template: a single tight, publicly documented port address; a
deterministic, explicitly labelled fixed policy value for CPU-visible content this project does
not model; every validation before any mutation.

It applies to five runtime-confirmed shapes, discovered across one task's own bounded
same-subsystem absorption loop (the project charter Advancement Discipline), each reached immediately after
implementing the previous one: a **BYTE READ** of the YM2612 PART-I address/status port
(**`$A04000`**, the task's original frontier); a **BYTE WRITE** of that same port (register-select
latch); a **BYTE WRITE** of the PART-I data port (**`$A04001`**, register-data write); a **BYTE
WRITE** of the PART-II address port (**`$A04002`**, register-select latch for the PART-II register
bank); and a **BYTE WRITE** of the PART-II data port (**`$A04003`**, register-data write for the
PART-II register bank -- the fifth and final pass of this task's own absorption loop, completing
the register-select/register-data write-latch protocol symmetrically across both PART-I and
PART-II). All five are reached through the runtime dispatch pair `genesis_is_ym2612_region` /
`genesis_ym2612_access` (`platforms/genesis/runtime/runtime.c`), routed from `genesis_route_access`. It names
no private target address beyond the already-public `$A04000`-`$A04003`.

**No FM synthesis of any kind exists in this path or anywhere else in this runtime** — no
channel/operator/LFO state, no timer A/B modelling, no busy-flag timing, no mixing, no sample
output, and no Z80 view of the chip. The only modelled effects are returning a fixed status byte and
accepting-and-discarding a write. No target instruction is fetched, decoded, or inspected.

## Public sources

| ID | Source and locator | Facts used |
| --- | --- | --- |
| GTO1 | Sega Enterprises, *Genesis Technical Overview* v1.00 (1991), p. 10 "Z80 AREA" address diagram. | The YM2612 register window's base address (`$A04000`-`$A04003`) viewed from the 68000. |
| PLUTIE-YM | Plutiedev, "Programming the YM2612", <https://plutiedev.com/ym2612>, accessed 2026-09-04. | The four-port window layout (PART-I address `$A04000`, PART-I data `$A04001`, PART-II address `$A04002`, PART-II data `$A04003`); the documented status-byte read of the address/status port: bit 7 = "Busy" (writing FM data), bit 0 = "Timer A overflow", remaining bits unused. |

The window base address is triangulated across GTO1 (primary) and PLUTIE-YM. The status-port
byte layout is sourced from PLUTIE-YM (a well-established secondary development reference for the
YM2612, not Sega primary documentation).

### Documented facts

1. **Window address.** The YM2612 register window occupies `$A04000`-`$A04003` viewed from the
   68000 (GTO1 p. 10; PLUTIE-YM).
2. **Port layout.** `$A04000`/`$A04002` are the PART-I/PART-II address (register-select) ports;
   `$A04001`/`$A04003` are the PART-I/PART-II data ports (PLUTIE-YM).
3. **Status-byte read.** A read of the address/status port returns bit 7 = Busy, bit 0 = Timer A
   overflow; the remaining bits are documented unused (PLUTIE-YM). Neither source documents the
   PART-II address port's own returned bit pattern, any write-latch protocol for a data port, or
   any timer-timing semantics precisely enough to model deterministically without guessing.

### Still unresolved

- The concrete Busy-flag timing (how long a register write keeps Busy asserted) and Timer A/B
  overflow timing: neither source documents this precisely enough to model deterministically, and
  this project performs no audio-timing/DSP modelling at all.
- PART-II status-port behaviour: not runtime-confirmed by this task, so not implemented (see
  Non-goals).

## Selected deterministic policy

Five accepted selectors, decided separately below. Each has width BYTE (never WORD, never LONG);
each applies only within `m68k_route_genesis_device_access` / `genesis_route_access` — the same
existing routed-device gate the PSG/Z80 precedents already occupy — never as an
instruction-specific emitter case.

**(a) READ — PART-I status byte value.** This project models no FM register-write latency and no
timer state, so neither the Busy flag nor the Timer A overflow flag can ever become
deterministically set without inventing timing behaviour this project does not implement. This
section therefore selects the one value consistent with "no pending write, no timer overflow" —
the same "no special bits set" simplification the SEG-007-T020/T038/T079/T111 controller-I/O
policies already apply to their own unmodelled register content:

```text
YM2612 PART-I status ($A04000, BYTE READ) = 0x00
```

labelled **Project compatibility policy** — a deliberate deterministic choice, not an asserted
real YM2612 Busy/Timer-A hardware value at any particular point in program execution.
Side-effect-free: neither source documents a status-port read side effect, so this read mutates no
device/bus state, matching the PSG/controller-I/O precedents' identical choice.

**(b), (c), (d), (e) WRITE — register-select/register-data latch (frontier passes 2-5, same
task).** PLUTIE-YM documents that a BYTE write to an address port (PART-I `$A04000` or PART-II
`$A04002`) latches an 8-bit register-select value governing which register the *next* write to the
corresponding data port affects, that a BYTE write to a data port (PART-I `$A04001` or PART-II
`$A04003`) writes that register's data byte, and that none of these writes carries a documented
side effect beyond the register-write protocol itself. This project implements no FM
register/channel/operator model of any kind, so it has no register-select or register-data state
to store and no register-specific behaviour to apply once any such write is later "consulted"
(nothing consults it: no FM register/channel semantics are implemented by this task). Rather than
add unread latch/data state for any of the four ports, this section accepts a BYTE write of *any*
8-bit value to any of them — the documented protocol places no restriction on which value may be
selected or written — as a pure no-op, identically for all four:

```text
YM2612 PART-I address-port write  ($A04000, BYTE WRITE, any value) = accepted, no state change
YM2612 PART-I data-port write     ($A04001, BYTE WRITE, any value) = accepted, no state change
YM2612 PART-II address-port write ($A04002, BYTE WRITE, any value) = accepted, no state change
YM2612 PART-II data-port write    ($A04003, BYTE WRITE, any value) = accepted, no state change
```

labelled **Project compatibility policy** — an explicit, deliberate simplification (accept without
storing), not a claim that this project models any register-select latch, register-data value, or
resulting FM behaviour. Side-effect-free: none of these writes mutates any device/bus state. Each
of (b), (c), (d), (e) was implemented only after its own runtime confirmation, one same-task
frontier pass at a time (the project charter Advancement Discipline / this task's own Scope item 6): (b) was
reached immediately after implementing (a); (c) immediately after (b); (d) immediately after (c);
(e) immediately after (d). No port/direction/width beyond these five was ever speculatively
pre-implemented ahead of its own confirmation. (e) is this task's fifth same-task frontier pass
(within the milestone's own six-same-task-frontier-iteration advancement bound, the project charter), and the
resulting shape set is already a symmetric, complete register-select/register-data write-latch pair
across both PART-I and PART-II -- a natural stopping shape for this absorption loop regardless of
the remaining iteration budget.

## Typed result boundary

On success, the future device result has these typed fields:

| Field | Required value/property |
| --- | --- |
| `value` (READ only) | One 8-bit BYTE value, `0x00` under this policy. |
| `category` (on failure only) | `unsupported_device_region_ym2612`. |

On failure, no successful value exists; no device/bus state is mutated (T042 SS3 /
`genesis_route_access`'s own "on failure neither it nor the runtime is modified" contract).

## Fail-closed boundary and ownership seam

This policy adds **exactly these five selectors**; every other YM2612-window access shape stays
fail-closed with `unsupported_device_region_ym2612` until it is itself runtime-confirmed and
implemented by a later bounded task:

- A WORD or LONG access at any offset in the window.
- A BYTE read of any of the three write-only ports (`$A04001`, `$A04002`, `$A04003`).

No value, timing, register-select/register-data state, or fallback behaviour is guessed for any of
them.

```text
statically lifted CPU memory operation
  -> Genesis bus/address resolver (m68k_route_genesis_device_access)
  -> YM2612 device semantics (genesis_ym2612_access)
  -> typed read result, accepted no-op write, or fail-closed result
```

The CPU operation supplies typed address, width, direction, and source provenance. The
bus/address resolver recognizes and routes the bounded shape without deriving provenance from
host storage. The device supplies the fixed BYTE read result, accepts the write, or returns the
retained failure.

## Evidence replacement rule

Only specifically scoped stronger evidence may replace or refine any of this policy's status-byte
value, write-acceptance rule, side-effect rule, or failure boundary: it must directly cover the
exact functional selector and the exact affected CPU-visible behaviour, record its provenance and
limits, and receive independent validation. It must not silently upgrade any policy value into an
authoritative hardware/timing claim, and it must not silently extend to the PART-II status-port
READ without its own runtime confirmation and its own separately reasoned policy value. A later
task implementing real FM register/channel semantics must add its own register-select/register-data
state for every port; this policy deliberately omits it because no consumer exists yet.

## Privacy and non-goals

This policy records no commercial-ROM address, byte, opcode, disassembly, trace, local path, or
derived fixture beyond the addresses `$A04000`-`$A04003` (already public). It does not model FM
synthesis, channel/operator/LFO state, timer A/B semantics, audio output, any actual
register-select latch or register-data value, the PART-II status port, VDP, rendering, interactive
input, or Z80 behaviour. It adds no decoder, CPU, emitter, fixture, or validation behaviour beyond
the five selectors this section defines.
