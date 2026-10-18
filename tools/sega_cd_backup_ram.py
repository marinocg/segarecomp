#!/usr/bin/env python3
"""SEG-065-T002 (ADR 0147): author or check an 8 KiB Sega CD internal backup RAM image (the `--backup-ram` machine input).

  sega_cd_backup_ram.py format OUT     write a formatted, empty image
  sega_cd_backup_ram.py check FILE     exit 0 if FILE is a formatted, empty image, 1 otherwise

A formatted, empty image is all zero except its final 64 bytes, the public backup-RAM format trailer: the volume name field
(eleven '_' bytes, then 00 00 00 00 40), the number of free 64-byte blocks repeated four times as 16-bit big-endian words
(`8192 / 64 - 3` = 125 for an empty internal RAM), eight zero bytes, then the format signatures "SEGA_CD_ROM" 00 01 00 00 00 and
"RAM_CARTRIDGE___". It is the state a formatted, empty internal backup RAM holds and contains no software-derived byte. The
layout is the one the independent Genesis Plus GX writes when it formats an absent internal backup RAM (`libretro.c` /
`sdl/sdl1/main.c` `brm_format`, size fields `sizeof(bram) / 64 - 3`); the local reference comparison of ADR 0146 uses exactly it.
"""
import pathlib
import sys

SIZE = 0x2000
BLOCK = 64


def formatted_empty(size=SIZE):
    free = size // BLOCK - 3
    trailer = bytearray(b"_" * 11 + b"\x00\x00\x00\x00\x40")
    trailer += free.to_bytes(2, "big") * 4 + bytes(8)
    trailer += b"SEGA_CD_ROM\x00\x01\x00\x00\x00"
    trailer += b"RAM_CARTRIDGE___"
    assert len(trailer) == BLOCK
    return bytes(size - BLOCK) + bytes(trailer)


def main(argv):
    if len(argv) != 3 or argv[1] not in ("format", "check"):
        print(__doc__.strip().splitlines()[2:4], file=sys.stderr)
        return 2
    path = pathlib.Path(argv[2])
    if argv[1] == "format":
        path.write_bytes(formatted_empty())
        return 0
    try:
        data = path.read_bytes()
    except OSError as error:
        print("unreadable: %s" % error, file=sys.stderr)
        return 1
    return 0 if data == formatted_empty() else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
