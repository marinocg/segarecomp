#!/usr/bin/env python3
"""Generates libs/viewer/test/sv_gallery_scene.c: the FICTIONAL picture the viewer gallery (libs/viewer/test/sv_gallery.c) shows behind its menus.

Test/review fixture only: it is compiled into the sv_gallery review tool and nothing else (never into the viewer library, a game or a package), and it is
never drawn over a real game picture. The art is original to this project (drawn by this script: no game, no ROM, no screenshot, no third-party sprite):
a lakeside landscape with clouds, distant mountains, a cliff and waterfall, trees, a tiled ground, a floating platform with gems, a small round robot and
a minimal HUD, in a deliberately limited palette with crisp nearest-neighbour pixels. It is laid out per picture size so the Genesis (320x224) and Master
System (256x192, 256x224) arrangements keep their focal content.

usage: gen_gallery_scene.py --out libs/viewer/test/sv_gallery_scene.c [--png-dir DIR]   (PNG previews are optional, for review)
"""
import argparse
import pathlib
import random

from PIL import Image

SIZES = [(320, 224), (256, 192), (256, 224)]

PAL = {
    # sky, light to the horizon
    "s0": (36, 86, 196), "s1": (52, 112, 216), "s2": (74, 140, 230), "s3": (104, 168, 240), "s4": (146, 198, 246), "s5": (190, 224, 250),
    # clouds
    "c0": (255, 255, 255), "c1": (214, 232, 252), "c2": (160, 190, 232),
    # far mountains and snow
    "m0": (78, 92, 168), "m1": (104, 122, 196), "m2": (140, 158, 220), "sn": (232, 240, 255),
    # cliff stone
    "r0": (64, 58, 92), "r1": (96, 88, 128), "r2": (132, 124, 164),
    # water and waterfall
    "w0": (24, 80, 168), "w1": (40, 112, 204), "w2": (88, 164, 236), "w3": (200, 236, 255),
    # greens (hills, trees, grass)
    "g0": (22, 84, 58), "g1": (34, 120, 66), "g2": (60, 160, 74), "g3": (112, 204, 88), "g4": (176, 232, 110),
    # ground
    "d0": (78, 44, 30), "d1": (120, 68, 36), "d2": (166, 100, 46), "d3": (214, 146, 72),
    # wood, flowers, gems, robot, HUD
    "t0": (92, 60, 36), "t1": (140, 94, 52),
    "fr": (238, 84, 84), "fy": (255, 214, 72), "fp": (255, 150, 200),
    "e0": (150, 40, 150), "e1": (232, 90, 210), "e2": (255, 196, 244),
    "k0": (20, 22, 40), "k1": (60, 200, 196), "k2": (150, 240, 228), "ko": (255, 150, 60),
    "h0": (255, 255, 255), "hr": (240, 70, 90),
}
NAMES = list(PAL)
INDEX = {n: i for i, n in enumerate(NAMES)}

# ---- hand-drawn sprites ('.' is transparent) ----
SPRITE_KEYS = {
    "K": "k0", "c": "k1", "C": "k2", "o": "ko", "W": "h0", "w": "c1", "g": "c2",
    "e": "e0", "E": "e1", "F": "e2", "L": "g1", "l": "g3", "D": "g0", "t": "t0", "T": "t1",
    "r": "fr", "y": "fy", "p": "fp", "h": "hr", "x": "k0",
}

ROBOT = [  # a small round robot (16 x 20), original
    "......oo........",
    ".......o........",
    ".....KKKKKK.....",
    "...KKccccccKK...",
    "..KccCCCCCcccK..",
    "..KcCWWKKWWCcK..",
    ".KccCWKKKKWCccK.",
    ".KccCWWKKWWCccK.",
    ".KcccccccccccccK",
    ".KcccKKKKKKcccK.",
    ".KccccccccccccK.",
    "..KccccccccccK..",
    "...KKccccccKK...",
    "..KcK.KKKK.KcK..",
    "..KK..KccK..KK..",
    "......KccK......",
    ".....KccccK.....",
    ".....KKKKKK.....",
]
CLOUD = [
    ".........wwww...........",
    "......wwWWWWww..........",
    "....wwWWWWWWWWw..wwww...",
    "...wWWWWWWWWWWWwwWWWWw..",
    "..wWWWWWWWWWWWWWWWWWWWw.",
    ".wWWWWWWWWWWWWWWWWWWWWWw",
    "gwwwwwwwwwwwwwwwwwwwwwwg",
    ".gggggggggggggggggggggg.",
]
GEM = [
    "..eee...",
    ".eFFEe..",
    "eFEEEEe.",
    "eEEEEEe.",
    ".eEEEe..",
    "..eEe...",
    "...e....",
]
HEART = [
    ".hh.hh.",
    "hWhhhhh",
    "hhhhhhh",
    ".hhhhh.",
    "..hhh..",
    "...h...",
]
PALM_LEAVES = [
    "...........DDDD.............",
    "......DDDDLLllLLDD...DDD....",
    "...DDLLllllLLLLllLDDDLLLD...",
    ".DDLLllLLLLDDDLLLLllLLllLLD.",
    "DLLlLLDD...DLLtLDDLLLLDDLLLD",
    "DLlLD.....DLLLTtLD...DDLLlLD",
    "DLLD.....DLlD.Tt.DLD....DLLD",
    "DLD.....DLlD..Tt..DLD....DLD",
    ".D.....DLLD...Tt...DLD....D.",
    "......DLD.....Tt....DD......",
    "......DD......Tt............",
]
FLOWER = {
    "r": [".r.", "ryr", ".r.", ".L."],
    "y": [".y.", "yry", ".y.", ".L."],
    "p": [".p.", "pyp", ".p.", ".L."],
}
DIGITS = {  # 3x5
    "0": ["###", "#.#", "#.#", "#.#", "###"], "1": [".#.", "##.", ".#.", ".#.", "###"], "2": ["###", "..#", "###", "#..", "###"],
    "3": ["###", "..#", ".##", "..#", "###"], "4": ["#.#", "#.#", "###", "..#", "..#"], "5": ["###", "#..", "###", "..#", "###"],
    "6": ["###", "#..", "###", "#.#", "###"], "7": ["###", "..#", ".#.", ".#.", ".#."], "8": ["###", "#.#", "###", "#.#", "###"],
    "9": ["###", "#.#", "###", "..#", "###"], "x": ["...", "#.#", ".#.", "#.#", "..."],
}


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[0] * w for _ in range(h)]

    def put(self, x, y, name):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = INDEX[name]

    def rect(self, x0, y0, x1, y1, name):
        for y in range(max(0, y0), min(self.h, y1)):
            row = self.px[y]
            for x in range(max(0, x0), min(self.w, x1)):
                row[x] = INDEX[name]

    def sprite(self, rows, x0, y0, keys=SPRITE_KEYS):
        for dy, row in enumerate(rows):
            for dx, ch in enumerate(row):
                if ch != ".":
                    self.put(x0 + dx, y0 + dy, keys[ch])

    def text(self, s, x, y, name, shadow="k0"):
        for i, ch in enumerate(s):
            g = DIGITS[ch]
            for dy, row in enumerate(g):
                for dx, c in enumerate(row):
                    if c == "#":
                        self.put(x + i * 4 + dx + 1, y + dy + 1, shadow)
            for dy, row in enumerate(g):
                for dx, c in enumerate(row):
                    if c == "#":
                        self.put(x + i * 4 + dx, y + dy, name)

    def image(self):
        img = Image.new("RGB", (self.w, self.h))
        img.putdata([PAL[NAMES[i]] for row in self.px for i in row])
        return img


def render(w, h):
    rnd = random.Random(1987)  # deterministic
    c = Canvas(w, h)
    horizon = h * 50 // 100          # far shore / lake top
    lake_bottom = h * 67 // 100
    ground = h * 79 // 100           # top of the foreground grass

    # sky: bands with a two-row checker dither between them
    bands = ["s0", "s1", "s2", "s3", "s4", "s5"]
    band_h = horizon / len(bands)
    for y in range(horizon):
        k = int(y / band_h)
        frac = y - k * band_h
        for x in range(w):
            name = bands[min(k, 5)]
            if k < 5 and frac > band_h - 3 and (x + y) % 2 == 0:
                name = bands[k + 1]
            c.put(x, y, name)

    # clouds (relative positions)
    for fx, fy in [(0.10, 0.10), (0.56, 0.06), (0.80, 0.20), (0.33, 0.24)]:
        c.sprite(CLOUD, int(w * fx), int(h * fy) + 14)

    # far mountains: piecewise ridges with a lit left face and snow on the tallest
    peaks = [(-0.05, 0.40), (0.10, 0.26), (0.24, 0.38), (0.38, 0.22), (0.52, 0.36), (0.66, 0.30), (0.82, 0.40), (1.05, 0.33)]
    pts = [(int(px * w), int(py * h)) for px, py in peaks]
    for x in range(w):
        for i in range(len(pts) - 1):
            (x0, y0), (x1, y1) = pts[i], pts[i + 1]
            if x0 <= x <= x1:
                t = (x - x0) / max(1, x1 - x0)
                top = int(y0 + (y1 - y0) * t)
                rising = y1 < y0
                for y in range(top, horizon):
                    depth = y - top
                    name = "m1" if (rising and depth < 40) else "m0"
                    if depth < 2:
                        name = "m2"
                    if top < h * 0.29 and depth < (h * 0.29 - top) * 0.9 + 2:
                        name = "sn" if (rising or depth < 3 or (x + y) % 3) else "m2"
                    c.put(x, y, name)
                break

    # lake: bands, shimmer streaks and a light rim at the far shore
    for y in range(horizon, lake_bottom):
        t = (y - horizon) / max(1, lake_bottom - horizon)
        name = "w1" if t < 0.35 else ("w0" if t > 0.75 else ("w1" if (y // 2) % 2 else "w0"))
        c.rect(0, y, w, y + 1, name)
    c.rect(0, horizon, w, horizon + 1, "w2")
    for _ in range(w // 6):
        x = rnd.randrange(0, w)
        y = rnd.randrange(horizon + 2, lake_bottom - 1)
        ln = rnd.choice([3, 4, 6, 8])
        c.rect(x, y, x + ln, y + 1, rnd.choice(["w2", "w2", "w3"]))

    # cliff with a waterfall on the right
    cl_x0, cl_x1 = int(w * 0.70), w
    cl_top = int(h * 0.36)
    for x in range(cl_x0, cl_x1):
        top = cl_top + max(0, (cl_x0 + 10 - x)) * 2
        for y in range(top, ground):
            stripe = ((x // 6) + (y // 9)) % 3
            name = ["r0", "r1", "r1"][stripe]
            if x % 6 == 0 or y % 9 == 0:
                name = "r0"
            if (x % 6 == 1) and stripe != 0:
                name = "r2"
            if x - cl_x0 < 5:
                name = "r0" if (x + y) % 2 else "r1"
            if (y - cl_top) % 28 in (0, 1) and y > cl_top + 10:
                name = "g2" if (y - cl_top) % 28 == 0 else "g0"
            c.put(x, y, name)
        c.rect(x, top - 3, x + 1, top, "g2")
        c.rect(x, top - 4, x + 1, top - 3, "g3" if x % 3 else "g4")
    fall_x0, fall_x1 = int(w * 0.80), int(w * 0.80) + 14
    for y in range(cl_top - 1, ground - 2):
        for x in range(fall_x0, fall_x1):
            name = ["w2", "w3", "w2", "w1"][(x + (y // 3)) % 4]
            if x in (fall_x0, fall_x1 - 1):
                name = "w1"
            c.put(x, y, name)
    c.rect(fall_x0 - 10, ground - 6, fall_x1 + 10, ground - 1, "w1")  # the pool at the foot of the cliff
    c.rect(fall_x0 - 10, ground - 6, fall_x1 + 10, ground - 5, "w2")
    for i in range(-9, 23):  # foam where the fall lands
        for y in range(ground - 9, ground - 4):
            if rnd.random() < 0.7:
                c.put(fall_x0 + i, y, rnd.choice(["w3", "c0"]))

    # mid hills with a tree line on the left
    for x in range(0, int(w * 0.72)):
        top = int(h * 0.58 + 7 * (1 - abs(((x / (w * 0.36)) % 2) - 1)))
        for y in range(top, ground):
            name = "g1" if (y - top) < 3 else "g0"
            if (x + y) % 4 == 0 and (y - top) < 6:
                name = "g2"
            c.put(x, y, name)
    for i in range(0, int(w * 0.68), 9):  # rounded tree crowns
        cx, cy = i + rnd.randrange(0, 4), int(h * 0.58) + rnd.randrange(-6, 2)
        r = rnd.choice([5, 6, 7])
        for y in range(cy - r, cy + r):
            for x in range(cx - r, cx + r):
                d = (x - cx) ** 2 + (y - cy) ** 2
                if d <= r * r:
                    name = "g2" if (x - cx) + (y - cy) < -r // 2 else "g1"
                    if d > (r - 1) ** 2 and (x + y) % 2:
                        name = "g0"
                    c.put(x, y, name)

    # a rolling meadow between the tree line and the foreground (lighter, striped)
    meadow_top = int(h * 0.66)
    for x in range(0, int(w * 0.72)):
        top = meadow_top + int(4 * (1 + __import__("math").sin(x / (w * 0.09))))
        for y in range(top, ground):
            stripe = ((x + 2 * (y - top)) // 6) % 2
            name = "g3" if stripe else "g2"
            if y - top < 1:
                name = "g4"
            elif y - top < 3 and x % 2:
                name = "g3"
            c.put(x, y, name)
        if rnd.random() < 0.10:
            c.put(x, top + rnd.randrange(3, max(4, ground - top - 2)), rnd.choice(["fy", "c0", "fp"]))

    # foreground ground: grass lip with tufts, then dirt tiles with highlights
    for x in range(w):
        lip = ground + (1 if (x // 5) % 3 == 0 else 0)
        c.rect(x, ground - 2, x + 1, lip + 4, "g2")
        c.put(x, ground - 2, "g4" if x % 4 else "g3")
        if x % 7 in (2, 3):
            c.put(x, ground - 3, "g3")
        c.rect(x, lip + 4, x + 1, lip + 6, "g1")
    for y in range(ground + 6, h):
        depth = y - ground - 6
        for x in range(w):
            wave = int(2 * __import__("math").sin(x / 9.0 + depth / 7.0))
            layer = (depth + wave) // 7
            name = ["d2", "d1", "d2", "d1", "d0"][layer % 5] if depth > 2 else "d1"
            c.put(x, y, name)
    for _ in range(w * (h - ground) // 200):  # pebbles: a rounded stone with a lit top
        x, y = rnd.randrange(0, w), rnd.randrange(ground + 9, h - 2)
        rw = rnd.choice([3, 4, 5])
        c.rect(x, y, x + rw, y + 2, "r1")
        c.rect(x + 1, y, x + rw - 1, y + 1, "r2")
        c.rect(x, y + 2, x + rw, y + 3, "r0")
    for _ in range(w // 10):  # small roots under the grass
        x = rnd.randrange(0, w)
        for k in range(rnd.randrange(3, 7)):
            c.put(x + (k % 2), ground + 6 + k, "d0")

    # floating platform with gems
    pw = 48
    px0, py0 = int(w * 0.40), int(h * 0.50)
    c.rect(px0, py0, px0 + pw, py0 + 3, "g3")
    c.rect(px0, py0, px0 + pw, py0 + 1, "g4")
    c.rect(px0, py0 + 3, px0 + pw, py0 + 12, "d2")
    for tx in range(px0, px0 + pw, 12):
        c.rect(tx, py0 + 3, tx + 1, py0 + 12, "d0")
        c.rect(tx + 1, py0 + 4, tx + 11, py0 + 5, "d3")
    c.rect(px0, py0 + 11, px0 + pw, py0 + 12, "d0")
    for i in range(3):
        c.sprite(GEM, px0 + 6 + i * 14, py0 - 12)

    # palm tree on the left, bushes, flowers, the robot and a crate
    trunk_x = int(w * 0.10)
    for y in range(ground - 46, ground):  # a gently bent trunk
        bend = (ground - y) // 12
        c.rect(trunk_x + bend, y, trunk_x + bend + 4, y + 1, "t1" if y % 4 else "t0")
        c.put(trunk_x + bend, y, "t0")
    c.sprite(PALM_LEAVES, trunk_x - 11, ground - 54)
    for bx in (int(w * 0.22), int(w * 0.60)):
        for y in range(ground - 9, ground):
            for x in range(bx - 11, bx + 11):
                if (x - bx) ** 2 / 121 + (y - ground) ** 2 / 81 <= 1:
                    c.put(x, y, "g3" if y < ground - 6 and (x + y) % 3 else ("g2" if (x + y) % 5 else "g1"))
    for i, fx in enumerate(range(int(w * 0.16), int(w * 0.66), 17)):
        c.sprite(FLOWER["ryp"[i % 3]], fx, ground - 5)
    c.sprite(ROBOT, int(w * 0.34), ground - 18)
    cx0 = int(w * 0.52)
    c.rect(cx0, ground - 14, cx0 + 14, ground, "t1")
    c.rect(cx0, ground - 14, cx0 + 14, ground - 13, "d3")
    c.rect(cx0, ground - 1, cx0 + 14, ground, "t0")
    for k in range(14):
        c.put(cx0 + k, ground - 14 + k, "t0")
        c.put(cx0 + 13 - k, ground - 14 + k, "t0")

    # HUD: gem counter on the left, hearts on the right (generic, no text)
    c.sprite(GEM, 8, 6)
    c.text("x027", 18, 7, "h0")
    for i in range(3):
        c.sprite(HEART, w - 10 - (i + 1) * 9, 6)
    return c


def rle(c):
    out = []
    flat = [i for row in c.px for i in row]
    i = 0
    while i < len(flat):
        j = i
        while j < len(flat) and flat[j] == flat[i] and j - i < 255:
            j += 1
        out += [j - i, flat[i]]
        i = j
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--png-dir")
    a = ap.parse_args()
    lines = [
        "/* GENERATED by tools/gen_gallery_scene.py - do not edit. Review-fixture art for sv_gallery ONLY (never linked into the viewer, a game or a package).",
        " * An original, fictional pixel-art landscape (no game, ROM, screenshot or third-party sprite); run-length encoded palette indices. */",
        '#include "sv_gallery_scene.h"', "",
        f"static const unsigned char kPalette[{len(NAMES)}][3] = {{",
    ]
    lines += ["  {%d, %d, %d}," % PAL[n] for n in NAMES]
    lines += ["};", ""]
    table = []
    for w, h in SIZES:
        c = render(w, h)
        if a.png_dir:
            pathlib.Path(a.png_dir).mkdir(parents=True, exist_ok=True)
            c.image().save(pathlib.Path(a.png_dir) / f"scene_{w}x{h}.png")
        data = rle(c)
        lines.append(f"static const unsigned char kRle{w}x{h}[{len(data)}] = {{")
        for k in range(0, len(data), 32):
            lines.append("  " + ",".join(str(v) for v in data[k:k + 32]) + ",")
        lines += ["};", ""]
        table.append((w, h, len(data)))
    lines.append("int sv_gallery_scene(unsigned char *rgb, int w, int h) {")
    lines.append("  const unsigned char *rle = 0;")
    lines.append("  int n = 0, i, k, pos = 0;")
    for w, h, n in table:
        lines.append(f"  if (w == {w} && h == {h}) {{ rle = kRle{w}x{h}; n = {n}; }}")
    lines += [
        "  if (rle == 0) return 0;",
        "  for (i = 0; i + 1 < n; i += 2)",
        "    for (k = 0; k < rle[i] && pos < w * h; ++k, ++pos) {",
        "      rgb[pos * 3 + 0] = kPalette[rle[i + 1]][0];",
        "      rgb[pos * 3 + 1] = kPalette[rle[i + 1]][1];",
        "      rgb[pos * 3 + 2] = kPalette[rle[i + 1]][2];",
        "    }",
        "  return pos == w * h;",
        "}",
    ]
    pathlib.Path(a.out).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
