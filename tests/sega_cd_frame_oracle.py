"""SEG-062-T005: independent expected frame for the `cdframe` fixture.

Computes, from the two data sectors read by the independent disc reader (sega_cd_disc_oracle.py), what the VDP must show once the
main program has copied them (contract-documented layout of the fixture data: 16 CRAM words, 16 tiles, then 28 rows x 40 plane-A
nametable entries) with the register set the program writes (H40, 64x32 planes, plane A at $C000, plane B at $E000, no scroll, no
window, empty sprite table, backdrop colour 0; plane B's nametable stays zero, so it shows tile 0 everywhere). It implements only the Mode-5 rules that configuration exercises (Sega, Genesis
Technical Overview; Plutiedev VDP pages): pattern rows of 4 bytes with the left pixel in the high nibble, nametable entry bits
(tile 10-0, H-flip 11, V-flip 12, palette 14-13, priority 15), plane A over plane B, colour 0 transparent, CRAM ----BBB-GGG-RRR-
with each 3-bit channel scaled by 255/7. It shares no code with platforms/genesis/runtime/vdp_render.c.
"""
import hashlib
import struct

WIDTH, HEIGHT = 320, 224


def layout(data):
    palette = [struct.unpack_from(">H", data, 2 * i)[0] for i in range(16)]
    tiles = data[32:32 + 16 * 32]
    names = [struct.unpack_from(">H", data, 32 + 512 + 2 * i)[0] for i in range(28 * 40)]
    return palette, tiles, names


def scale(v):
    return v * 255 // 7


def rgb(word):
    return bytes((scale((word >> 1) & 7), scale((word >> 5) & 7), scale((word >> 9) & 7)))


def pixel(tiles, tile, px, py):
    byte = tiles[tile * 32 + py * 4 + px // 2]
    return (byte >> 4) if px % 2 == 0 else (byte & 0x0F)


def expected_ppm_pixels(data):
    palette, tiles, names = layout(data)
    out = bytearray()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            entry = names[(y // 8) * 40 + (x // 8)]
            tile = entry & 0x7FF
            px, py = x % 8, y % 8
            if entry & 0x0800:
                px = 7 - px
            if entry & 0x1000:
                py = 7 - py
            index = pixel(tiles, tile, px, py)
            if index:
                colour = ((entry >> 13) & 3) * 16 + index
            else:
                # Plane B: every entry of its (zero) nametable is tile 0, palette 0, no flip, no scroll; then the backdrop (colour 0).
                colour = pixel(tiles, 0, x % 8, y % 8)
            out += rgb(palette[colour] if colour < 16 else 0)
    return bytes(out)


def expected_vram_sha256(data):
    palette, tiles, names = layout(data)
    vram = bytearray(0x10000)
    vram[0:len(tiles)] = tiles
    for row in range(28):
        for col in range(40):
            struct.pack_into(">H", vram, 0xC000 + row * 128 + col * 2, names[row * 40 + col])
    return hashlib.sha256(bytes(vram)).hexdigest()


def expected_cram_hex(data):
    palette, _, _ = layout(data)
    cram = bytearray(128)
    for i, word in enumerate(palette):
        struct.pack_into(">H", cram, 2 * i, word)
    return bytes(cram).hex()


def read_ppm(path):
    blob = open(path, "rb").read()
    parts = blob.split(b"\n", 3)
    assert parts[0] == b"P6", parts[0]
    width, height = (int(v) for v in parts[1].split())
    assert int(parts[2]) == 255
    return width, height, parts[3]
