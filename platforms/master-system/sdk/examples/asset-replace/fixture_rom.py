#!/usr/bin/env python3
"""The synthetic asset-demo cartridge of the Master System asset-replacement example (SEG-147-T007). Project-authored bytes only: a tiny program that shows a 16-colour palette, 40 tiles, an 8x4 room
table and a 16-character line of text. It is not a game and holds no third-party data. Its (invented) data format is described in rom_format.json.

usage: fixture_rom.py <output.sms>"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[5] / "tools"))
import sms_fixture_rom as builder  # noqa: E402

PALETTE, TILES, ROOM, TEXT = 0x1000, 0x1100, 0x1800, 0x1900
TILE_COUNT = 40
CHARSET = " ABCDEFGHIJKLMNOPQRSTUVWXYZ"   # text code = position = tile number


def planar(rows):
    """8 rows of 8 colour indices -> the 32-byte Mode 4 pattern (four planes per row, leftmost pixel in bit 7)."""
    return bytes(sum(((rows[y][x] >> plane) & 1) << (7 - x) for x in range(8)) for y in range(8) for plane in range(4))


def original_tiles():
    tiles = [planar([[0] * 8] * 8)]
    for n in range(1, 27):    # 'A'..'Z': a distinct placeholder glyph per letter (the letter number in binary, in colour 1)
        tiles.append(planar([[1 if 1 <= y <= 5 and 1 <= x <= 5 and (n >> (x - 1)) & 1 else 0 for x in range(8)] for y in range(8)]))
    for n in range(27, TILE_COUNT):   # scenery blocks: flat colour 2..14
        tiles.append(planar([[2 + (n - 27)] * 8 for _ in range(8)]))
    return b"".join(tiles)


def original_data():
    palette = bytes((i * 5) & 0x3F for i in range(16))
    room = bytes(27 + (x + y) % 4 for y in range(4) for x in range(8))
    text = bytes(CHARSET.index(c) for c in "VANILLA ROOM".ljust(16))
    return {PALETTE: palette, TILES: original_tiles(), ROOM: room, TEXT: text}


def source():
    def vram(address, code=1):
        return ["        ld a,0x%02X" % (address & 255), "        out (0xBF),a", "        ld a,0x%02X" % ((address >> 8) | (code << 6)), "        out (0xBF),a"]

    def reg(number, value):
        return ["        ld a,0x%02X" % value, "        out (0xBF),a", "        ld a,0x%02X" % (0x80 | number), "        out (0xBF),a"]

    lines = [".org 0x0000", "        di", "        im 1", "        ld sp,0xDFF0", "        jp main",
             ".org 0x0038", "        reti", ".org 0x0066", "        retn", ".org 0x0100", "main:"]
    for number, value in {0: 0x06, 1: 0x80, 2: 0x0E, 5: 0x7E, 6: 0x00, 7: 0x00, 8: 0x00, 9: 0x00, 10: 0xFF}.items():
        lines += reg(number, value)
    # palette -> CRAM 0..15
    lines += vram(0, 3) + ["        ld hl,0x%04X" % PALETTE, "        ld b,16", "        ld c,0xBE", "        otir"]
    # tiles -> VRAM 0 (five 256-byte runs)
    lines += vram(0) + ["        ld hl,0x%04X" % TILES, "        ld d,5", "        ld c,0xBE", "tiles:  ld b,0", "        otir", "        dec d", "        jr nz,tiles"]
    # clear the 32x28 name table
    lines += vram(0x3800) + ["        ld bc,0x0700", "clear:  xor a", "        out (0xBE),a", "        dec bc", "        ld a,b", "        or c", "        jr nz,clear"]
    # the room table at rows 4..7, columns 4..11; the text line at row 10, columns 8..23 (each name-table entry is the tile number then a zero attribute byte)
    for row in range(4):
        lines += vram(0x3800 + (4 + row) * 64 + 4 * 2) + ["        ld hl,0x%04X" % (ROOM + 8 * row), "        ld b,8", "room%d:  ld a,(hl)" % row, "        out (0xBE),a",
                                                           "        xor a", "        out (0xBE),a", "        inc hl", "        djnz room%d" % row]
    lines += vram(0x3800 + 10 * 64 + 8 * 2) + ["        ld hl,0x%04X" % TEXT, "        ld b,16", "text:   ld a,(hl)", "        out (0xBE),a", "        xor a", "        out (0xBE),a",
                                               "        inc hl", "        djnz text"]
    lines += vram(0x3F00) + ["        ld a,0xD0", "        out (0xBE),a"]   # no sprites
    lines += reg(1, 0xC0)   # display on (no interrupts)
    lines += ["idle:   jr idle"]
    return "\n".join(lines) + "\n"


def build():
    image, _ = builder.Assembler(source()).assemble()
    markers = {}
    for offset, blob in original_data().items():
        for i, byte in enumerate(blob):
            markers[offset + i] = byte
    return builder.build_rom(0x8000, image, markers)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    pathlib.Path(sys.argv[1]).write_bytes(build())
