# ADR 0260: SEG-116 (M20) - a 68000 write to the Z80 area while the Z80 runs and the bus is not requested

## Status

Accepted (SEG-116). Extends ADR 0258/0259 (Sega CD main-side Z80). The Genesis product is unchanged (ADR 0072).

## Context

An interactive Sonic CD run ended after the fourth-zone boss in the typed stop `main_device_unsupported`: a 68000 byte write into the Z80 sound-RAM window made by code running from
Word RAM. At the stop the main-side Z80 was modelled and healthy, `/RESET` was released (one release more than assertions) and BUSREQ was released (equal assert/release counts), so the
68000 had not requested the bus while the Z80 ran. The Genesis route (`genesis_z80_area_access`) answers every Z80-area access without a grant with
`GENESIS_DIAG_68K_Z80_AREA_WITHOUT_BUS`, and the Sega CD main route maps anything it does not classify as the Z80's own outcome to `main_device_unsupported`.

The recorded references (genesis-z80-audio-contract section 3): Genesis Plus GX maps the area to open bus and ignores writes without the grant; ARES gates the area on the grant. The software
works on both, so the write has no architectural effect.

## Decision

1. With the Sega CD main-side Z80 attached (`m->z80 != NULL`), a 68000 write (any width) to `$A00000-$A0FFFF` while `/RESET` is released **and** BUSREQ is not requested is discarded and
   counted (`z80.main_ignored_writes`, also `ignored_writes`). It does not synchronise or disturb the Z80, never reaches the device state and is never a stop.
2. Everything else is unchanged: a write while `/RESET` is asserted or while a request is pending but not granted keeps the typed stop (no hardware evidence for a reference
   outcome, no workload); reads keep the SEG-114 rule (served as the sound RAM stands); a program without the Z80 keeps the legacy rule; the Genesis product keeps its fail-closed contract.
3. No value is fabricated, no write is performed behind the Z80's back, and nothing is title-specific.

## Evidence

Fixture case 20 (`tools/sega_cd_bios_hw_fixture.py`, `tests/sega_cd_main_z80_test.py`; project-authored bytes): a byte and a word write without a request are discarded (the Z80's own marker
survives, the word target is untouched), exactly two writes are counted, the Z80 stays healthy, a write under a bus hold (control) lands, and the digest and result are identical for
another scheduler partition. Cases 12-19 and the Sega CD BIOS suites are unchanged.

## Limits

The interactive Sonic CD session cannot be replayed (no input log), so the authentic evidence is a bounded regression run of the rebuilt program plus the classified cause above; whether the
session now passes the boss transition and what comes next is established by playing it.
