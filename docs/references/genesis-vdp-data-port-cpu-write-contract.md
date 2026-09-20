# Genesis VDP bounded CPU DATA-port write contract

## Scope

This record supplies the public-source basis for SEG-007-T108's bounded runtime
capability: the plain (non-armed-fill, non-DMA) MC68000 CPU write to the Genesis
VDP DATA port (`$C00000`) that the canonical Sonic startup route reaches. It
covers only what that route exercises:

- a `LONG` (32-bit) and `WORD` (16-bit) CPU write to `$C00000`;
- whose transfer target was selected by a completed non-DMA two-word
  address-set command as **CRAM WRITE** (`CD5..CD0 = 000011 = 0x03`) or
  **VSRAM WRITE** (`CD5..CD0 = 000101 = 0x05`);
- storing big-endian halfwords into the VDP-owned CRAM / VSRAM byte storage and
  advancing the current VDP address by the register-15 auto-increment value.

It is **not** a general VDP data-port model. Out of scope and left fail-closed:
DATA-port reads; VRAM CPU data-port writes; CRAM / VSRAM / VRAM read paths;
generic control-port ownership or a generic data-port state machine; the
armed VRAM-fill DATA-port WORD path (SEG-007-T098 / SEG-007-T101, unchanged);
DMA to CRAM / VSRAM; VDP FIFO / timing / status-bit behavior; and the PSG
(`SN76489`) audio port that shares the `$C00000..$C0001F` address window.

## Public sources

| ID | Source and locator | Fact used |
| --- | --- | --- |
| GTO1 | Sega Enterprises, *Genesis Technical Overview* v1.00 (1991), [Internet Archive PDF](https://ia801909.us.archive.org/24/items/Genesis_Technical_Overview_v1.00_1991_Sega_US/Genesis_Technical_Overview_v1.00_1991_Sega_US.pdf), [text derivative](https://ia801909.us.archive.org/24/items/Genesis_Technical_Overview_v1.00_1991_Sega_US/Genesis_Technical_Overview_v1.00_1991_Sega_US_djvu.txt); accessed 2026-08-29. p. 20 ("WRITE2: ADDRESS SET" two-word command diagram and the `CD5..CD0` access-mode table; "Long word access is equivalent to two word accesses, with D31-D16 written first"); p. 27 (identical `CD5..CD0` access-mode table); pp. 28-33 (VRAM/CRAM/VSRAM access examples, each a two-word address-set followed by word data-port transfers); p. 28 ("VRAM address is increased by the value of REGISTER # 15"); p. 2 ("64 x 9-bits of CRAM (Color RAM)"); p. 12 ("64 9-bit wide color registers"; "40 10bit words inside the VDP chip"); p. 10 ("VDP AREA" address diagram: DATA `$C00000`, CONTROL `$C00004`, HV COUNTER `$C00008`, PSG 76489 `$C00011`). | The two-word address-set command layout (unchanged from SEG-007-T091); the `CD5..CD0` code table including CRAM WRITE `0x03` and VSRAM WRITE `0x05`; a long-word data-port access equals two sequential word accesses with the upper halfword first; the current VDP address auto-increments by register 15 after each data-port access; CRAM is 64 color entries and VSRAM is 40 entries, each entry one 16-bit access word. |
| PLUTIE-CMD | Plutiedev, "VDP command reference", <https://plutiedev.com/vdp-commands>; accessed 2026-08-29. | Independent corroboration of the `CD` command-code table (CRAM write command `$C0000000` decodes to `CD5..CD0 = 0x03`; VSRAM write `$40000010` decodes to `0x05`) and of the address split across the two control-port halves; "after every word written the address is automatically incremented by the autoincrement amount". |
| PLUTIE-PAL | Plutiedev, "Tiles and palettes", <https://plutiedev.com/tiles-and-palettes>; accessed 2026-08-29. | Independent corroboration of CRAM capacity: four palettes of sixteen colors (64 entries), stored as BGR words. |
| COPETTI-MD | Rodrigo Copetti, "Mega Drive / Genesis Architecture", <https://www.copetti.org/writings/consoles/mega-drive-genesis/>; accessed 2026-08-29. | Independent corroboration of the byte sizes: "128 B CRAM (Colour RAM): Stores four palette entries with 16 colours each"; "80 B VSRAM (Vertical Scroll RAM)". |
| CMAC-VDP | Charles MacDonald, "Sega Genesis VDP documentation" (genvdp), widely mirrored (e.g. <https://segaretro.org/images/a/a5/Sega_Genesis_VDP_documentation_%28genvdp%29.txt>); accessed 2026-09-09. | The control port uses one internal write-pending flip-flop shared with the data port; the first control word writes the low bits of the address/code registers immediately; any data-port access clears the write-pending flip-flop, so a data-port access between the two control words breaks the pending command. |
| PLUTIE-ADDR | Plutiedev, "The address register", <https://plutiedev.com/vdp-registers#reg-address>/<https://plutiedev.com/vdp-commands>; accessed 2026-09-09. | Independent corroboration that the address/code is written in two halves, low half (A13-A0, CD1-CD0) first, and completed by the second half (A15-A14, CD5-CD2). |
| GPGX-VDP | Genesis Plus GX, `core/vdp_ctrl.c` `vdp_ctrl_w` / `vdp_data_w` / `vdp_68k_data_w` (mature clean-room Mode-5 reference), <https://github.com/ekeeke/Genesis-Plus-GX>; accessed 2026-09-09. | Convergent mature implementation: `addr`/`code` are running registers updated across the two control halves (`pending` gates second half); `vdp_data_w` sets `pending = 0`, so a data-port access mid-command cancels the pending second word and the transfer then uses the merged `addr`/`code`. |

The `CD5..CD0` code table and the CRAM entry format are each triangulated across
two independent sources (GTO1 + PLUTIE-CMD for the codes; GTO1 + PLUTIE-PAL for
the CRAM format). The CRAM (128 B) and VSRAM (80 B) byte sizes are triangulated
across GTO1 (primary) and COPETTI-MD.

## Implemented typed behavior

Through the existing `genesis_route_access` -> `genesis_vdp_access` dispatch and
the SEG-007-T042 persistent `GenesisVdpState` owner:

1. When `genesis_vdp_control_port_write_word` accepts a non-DMA two-word
   address-set command, it now also records the accepted `CD5..CD0` code in
   `GenesisVdpState.data_port_transfer_code` and sets
   `data_port_transfer_code_valid`. Both are zero-initialised.

2. `genesis_vdp_access` handles a CPU write to `$C00000` when **no DMA is
   armed** (`dma.phase == GENESIS_VDP_DMA_IDLE`) and a documented WRITE code is
   selected:
   - `data_port_transfer_code == 0x03` targets `GenesisVdpState.cram`
     (`GENESIS_VDP_CRAM_BYTES == 128`).
   - `data_port_transfer_code == 0x05` targets `GenesisVdpState.vsram`
     (`GENESIS_VDP_VSRAM_BYTES == 80`).
   - A `LONG` write is decomposed into two sequential 16-bit sub-writes,
     D31-D16 first (GTO1 p. 20), mirroring the existing `$C00004` LONG-write
     decomposition.
   - Each 16-bit sub-write stores a big-endian halfword at
     `addressed_pointer mod target_size` (high byte first) and then advances
     `addressed_pointer` by `auto_increment_value`, modulo `target_size`.
   - Each 16-bit sub-write is fully validated before it mutates anything.

## Fail-closed boundary

Every excluded neighbour fails closed (returns 0, mutating no `GenesisVdpState`
field) **before any mutation**, preserving
`GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP`:

- wrong direction: any DATA-port read;
- wrong port/address: any address other than `$C00000` for this path;
- no transfer code selected (`data_port_transfer_code_valid == 0`);
- a READ code selected (`0x00` VRAM READ, `0x04` VSRAM READ, `0x08` CRAM READ);
- a DMA code / any armed DMA (`dma.phase != IDLE`), including an armed
  memory-to-VRAM or armed VRAM-fill engine;
- an odd current VDP address for a **CRAM / VSRAM** target (VRAM applies the
  documented odd-address byte exchange instead, SEG-007-T191);
- the second sub-write of a `LONG` write failing after the first succeeded
  (partial-completion policy below).

The armed VRAM-fill DATA-port WORD path (SEG-007-T098 / SEG-007-T101) is
unchanged: when `dma.phase == BUSY && dma.kind == VRAM_FILL`, a `WORD` write to
`$C00000` still routes to `genesis_vdp_data_port_fill_write` and any other width
still fails closed.

## SEG-007-T191 extension: generalized data-port WRITE surface

SEG-007-T191 consumes the SEG-007-T190 one-shot runtime-selected frontier:
generated-native Sonic execution reached its **first VDP data-port region
access** -- a `LONG`, direction = write, `diagnostic_category =
unsupported_device_region_vdp` -- which the classification below identifies as
a **VRAM WRITE** (`CD5..CD0 = 0x01`) data-port write that the SEG-007-T108
path failed closed. This extension widens the plain CPU data-port **write**
model (still not a read path, still not a DMA path) to the full surface that
frontier implies:

- **VRAM WRITE target (`0x01`).** Routed to `GenesisVdpState.vram`
  (`GENESIS_VDP_VRAM_BYTES == 65536`) through the existing
  `genesis_vdp_write_target_buffer` mapping (which already resolved `0x01`
  for the DMA engine). Even current address: big-endian halfword store
  (D15-D8 first). Auto-increment by register #15 after each access, modulo
  64 KiB (GTO1 p. 28: "VRAM address is increased by the value of REGISTER #
  15, independent data size").
- **VRAM odd-address byte exchange.** GTO1 (pp. 20 / 27-33): "VRAM address
  A0 is used in the calculation of the address increment, but is ignored
  during address decoding" and "high and low bytes are exchanged if A0 = 1".
  When the current VRAM address is odd, the even byte pair is written with
  the two data bytes swapped (`vram[A & ~1] = D7..D0`, `vram[(A & ~1) + 1] =
  D15..D8`) and the pointer still advances by register #15 from the odd
  base. CRAM / VSRAM at an odd address remain fail-closed (GTO1 documents
  the exchange only for VRAM).
- **BYTE access width.** GTO1 (pp. 20 / 27-33): "When you do byte writing,
  data is D7 ~ D0, and may be written to $C00000 or $C00001." Modeled as a
  WORD write whose data byte is mirrored into both halves (`(b << 8) | b`).
  This is a **replaceable project compatibility policy** matching convergent
  mature-emulator behavior (e.g. Genesis Plus GX routes an 8-bit data-port
  write as `vdp_data_w((data << 8) | (data & 0xff))`), not a verified
  hardware observation of the byte-write data path.
- **LONG width** remains two sequential 16-bit sub-writes, D31-D16 first
  (GTO1 p. 20), with the same documented non-atomic partial-completion
  policy the `$C00004` LONG write uses.
- **Coupled control-port latch (cancel-and-consume, not fail-closed).** The
  VDP write-pending flip-flop is a single bit shared between the CONTROL and
  DATA ports. The two-word CONTROL command is not buffered whole: the FIRST
  word already updates the low half of the internal address/code registers
  (`A13-A0`, `CD1-CD0`) the instant it is written; only `A15-A14` / `CD5-CD2`
  wait for the second word. A 68000 DATA-port access performed while only the
  first half has been written **breaks/cancels** the pending second-word
  sequence: hardware clears the shared write-pending flip-flop on any
  data-port access, so the abandoned second half is dropped and the next
  CONTROL-port word is interpreted as a fresh first half. The runtime models
  this exactly: `genesis_vdp_control_port_write_word` applies the first
  word's `A13-A0` over the retained `A15-A14` (and, when a code is already
  selected, its `CD1-CD0` over the retained `CD5-CD2`); a DATA-port write
  then clears `control_port_awaiting_second_word` / `control_port_first_word`
  unconditionally (even when the write is itself rejected below because no
  transfer code has ever been selected -- `data_port_transfer_code_valid ==
  0` -- which remains the one genuine fail-closed sub-case) and performs the
  transfer against the resulting merged address/code state. Sources: Genesis
  Plus GX `vdp.c` (`vdp_ctrl_w` maintains `addr`/`code` across the two halves
  and both `vdp_data_w` and the control path clear `pending`); BlastEm VDP
  command-word handling; Charles MacDonald, "Sega Genesis VDP documentation"
  (shared write-pending flag, first-word partial address latch); plutiedev
  "VDP command reference" / "The address register" (address split across the
  two CONTROL halves, low half first). No independent hardware observation of
  the exact byte-ordering disposition at a wrap across the merged pointer is
  claimed; the modulo-target-size wrap policy below still governs that.

No new `GenesisVdpState` field or ownership is introduced: this is a purely
additive widening within the SEG-007-T042 / ADR-0012 persistent-device-state
fields (`addressed_pointer`, `auto_increment_value`,
`data_port_transfer_code[_valid]`, `control_port_first_word`,
`control_port_awaiting_second_word`, `vram/cram/vsram`), so no new ADR is
recorded. The two-halves address/code latch and the shared write-pending
flip-flop cancel-on-data-access are the documented behavior of the same
two-word CONTROL command those existing fields already model (SEG-007-T091 /
T042 SS1.3), now completed rather than approximated -- a bounded additive
detail within the existing semantic owner, not a new state machine.

Still out of scope and fail-closed: all DATA-port reads; CRAM / VSRAM / VRAM
read paths; a generic control-port state machine; the VDP DMA engine (the
executed frontier did not couple the data-port write to DMA); FIFO / timing /
status-bit behavior. A DATA-port write with no transfer code ever selected
(`data_port_transfer_code_valid == 0`) is still rejected -- but it now still
clears any half-written CONTROL command latch first, matching the shared
write-pending flip-flop.

## Project compatibility policy (replaceable, not verified hardware)

The following are deliberate deterministic project choices within the
SEG-007-T042 architecture, explicitly **not** claims about real Genesis VDP
hardware behavior. Stronger reproducible hardware evidence may replace them
through a separately evidenced change.

- **Address wrap.** The current VDP address wraps modulo the selected target's
  documented byte size (128 for CRAM, 80 for VSRAM). GTO1 documents the 16-bit
  address register but not the CRAM / VSRAM wrap disposition (byte ordering at
  the crossing, final pointer value, partial-commit).
- **LONG non-atomic partial completion.** A `LONG` data-port write is two
  sequential bus transactions (GTO1 p. 20). If the second sub-write is rejected,
  the first sub-write's already-committed target byte and pointer advance are
  left in place and the overall access reports failure. This is the same narrow,
  documented exception to `genesis_route_access`'s general "on failure neither
  it nor the runtime is modified" contract that the `$C00004` LONG write already
  uses, justified identically by GTO1's two-sequential-transaction model.
- **Odd address rejected.** Only an even current VDP address is accepted; the
  reached startup clear loops use auto-increment 2 from an even base. An odd
  address fails closed rather than guess the odd-address halfword byte ordering,
  mirroring the VRAM-fill path's identical rationale.
- **No FIFO / timing / status model.** These writes complete immediately with no
  FIFO occupancy, no DMA-busy interaction, and no status-bit effect.
