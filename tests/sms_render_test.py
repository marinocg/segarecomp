#!/usr/bin/env python3
"""SEG-009-T005: state-injection differential of the SMS renderer against the independent model, with mutation controls.

usage: sms_render_test.py <sms_render_dump> <product-root>

Generated scenarios (seeded, plus directed ones) set VRAM/CRAM/registers and per-line register events, render one frame through
the C renderer (tests/tools/sms_render_dump.c) and compare framebuffer bytes and the overflow/collision flags with
tests/sms_render_model.py (written from the machine contract, not from the C code). Coverage of every T005 capability row is
asserted from the scenarios, and every deliberately wrong model rule (sms_render_model.MUTATIONS) must disagree with the C
renderer on at least one scenario.
"""
import hashlib
import pathlib
import random
import struct
import subprocess
import sys
import tempfile

EXE, ROOT = sys.argv[1], pathlib.Path(sys.argv[2]).resolve()
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sms_render_model as model  # noqa: E402

FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)


class Scenario:
    def __init__(self, name, vram, cram, regs, events=()):
        self.name, self.vram, self.cram, self.regs, self.events = name, bytes(vram), bytes(cram), list(regs), list(events)

    def pack(self):
        out = self.vram + self.cram + bytes(self.regs[:11]) + struct.pack("<I", len(self.events))
        for line, reg, value in sorted(self.events, key=lambda e: e[0]):
            out += struct.pack("<HBB", line, reg, value)
        return out

    def regs_at(self, line):
        regs = list(self.regs)
        for ln, reg, value in sorted(self.events, key=lambda e: e[0]):
            if ln <= line:
                regs[reg] = value
        return regs


def base_regs(r0=0x06, r1=0xC0, r2=0xFF, r5=0xFF, r6=0xFB, r7=0x00, r8=0, r9=0):
    return [r0, r1, r2, 0xFF, 0xFF, r5, r6, r7, r8, r9, 0xFF]


def run_c(scn, tmp):
    src, dst = tmp / "s.bin", tmp / "r.bin"
    src.write_bytes(scn.pack())
    r = subprocess.run([EXE, str(src), str(dst)], capture_output=True, timeout=60)
    if r.returncode != 0:
        return None
    data = dst.read_bytes()
    height, overflow, collision = struct.unpack("<HBB", data[:4])
    pixels = data[4:]
    rows = [list(pixels[i * 256:(i + 1) * 256]) for i in range(height)]
    return height, rows, bool(overflow), bool(collision)


def put_tile(vram, index, rows):
    """rows: 8 lists of 8 colour indices."""
    for r, cols in enumerate(rows):
        planes = [0, 0, 0, 0]
        for c, v in enumerate(cols):
            for p in range(4):
                planes[p] |= ((v >> p) & 1) << (7 - c)
        for p in range(4):
            vram[index * 32 + r * 4 + p] = planes[p]


def random_scenario(seed):
    rng = random.Random(seed)
    vram = bytearray(0x4000)
    tall = rng.random() < 0.25
    r2 = rng.choice([0x0E, 0x0C, 0x0A, 0x08, 0x00]) if not tall else rng.choice([0x0C, 0x08, 0x04, 0x00])
    r5 = rng.choice([0x7E, 0x6E, 0x7C, 0x3E]) | 1
    r0 = 0x06 | rng.choice([0, 0x80]) | rng.choice([0, 0x40]) | rng.choice([0, 0x20]) | rng.choice([0, 8])
    r1 = 0x80 | (0x40 if rng.random() < 0.93 else 0) | (0x10 if tall else 0) | rng.choice([0, 2]) | rng.choice([0, 1, 0, 0])
    regs = base_regs(r0, r1, r2, r5, rng.choice([0xFB, 0xFF]), rng.randrange(16) | 0xF0 & 0, rng.choice([0, rng.randrange(256)]),
                     rng.choice([0, rng.randrange(256)]))
    # pattern memory: sparse tiles (colour 0 is common), a region of dense ones
    for t in range(0, 96):
        density = rng.choice([0.2, 0.5, 0.9])
        put_tile(vram, t, [[rng.randrange(16) if rng.random() < density else 0 for _ in range(8)] for _ in range(8)])
    name_base = (r2 & 0x0C) * 0x400 + 0x700 if tall else (r2 & 0x0E) * 0x400
    for i in range(32 * 32):
        entry = rng.randrange(96) | rng.choice([0, 0x200]) << 0 | rng.choice([0, 0x400]) | rng.choice([0, 0x800]) | rng.choice([0, 0x1000])
        if rng.random() < 0.3:
            entry |= 0x100  # pattern 256+: upper patterns are (mostly) zero memory
        a = (name_base + i * 2) % 0x4000
        vram[a], vram[a + 1] = entry & 255, entry >> 8
    sat = (r5 & 0x7E) << 7
    n = rng.choice([0, 3, 12, 20, 40, 64])
    crowd = rng.random() < 0.5
    base_y = rng.randrange(0, 160)
    for i in range(n):
        y = (base_y + rng.randrange(0, 12)) if crowd and rng.random() < 0.8 else rng.choice([rng.randrange(0, 224), rng.randrange(0xE8, 0x100)])
        if y == 0xD0 and rng.random() < 0.8:
            y = 0xCF
        vram[sat + i] = y
        vram[sat + 0x80 + 2 * i] = rng.randrange(0, 256) if rng.random() < 0.6 else rng.randrange(40, 120)
        vram[sat + 0x81 + 2 * i] = rng.randrange(100)
    if n < 64:
        vram[sat + n] = 0xD0 if rng.random() < 0.8 else rng.randrange(0, 224)
    cram = bytes(rng.randrange(64) for _ in range(32))
    events = []
    for _ in range(rng.choice([0, 0, 2, 5, 10])):
        line = rng.randrange(0, 200)
        reg = rng.choice([0, 8, 8, 9, 7, 1])
        if reg == 0:
            value = 0x06 | rng.choice([0, 0x80, 0x40, 0x20, 8, 0xC0, 0xE8])
        elif reg == 1:
            value = (regs[1] & ~3) | rng.choice([0, 1, 2, 3])  # keeps the mode bits, changes sprite size/zoom
        else:
            value = rng.randrange(256) if reg != 7 else rng.randrange(16)
        events.append((line, reg, value))
    return Scenario("random_%d" % seed, vram, cram, regs, events)


def directed():
    out = []
    palette = bytes(range(0, 32))
    # sprites over 8 per line: overflow and the 9th sprite is not drawn
    for count, expect in ((8, False), (9, True)):
        vram = bytearray(0x4000)
        put_tile(vram, 1, [[5] * 8 for _ in range(8)])
        sat = 0x3F00
        for i in range(count):
            vram[sat + i] = 19
            vram[sat + 0x80 + 2 * i], vram[sat + 0x81 + 2 * i] = i * 9, 1
        vram[sat + count] = 0xD0
        out.append((Scenario("overflow_%d" % count, vram, palette, base_regs(0x06, 0xC0, 0xFF, 0xFF), []), "overflow", expect))
    # terminator: sprites after Y=$D0 invisible in 192 mode, visible in 224 mode
    for tall in (False, True):
        vram = bytearray(0x4000)
        put_tile(vram, 1, [[7] * 8 for _ in range(8)])
        sat = 0x3F00
        vram[sat] = 0xD0
        vram[sat + 1] = 30
        vram[sat + 0x82], vram[sat + 0x83] = 50, 1
        r1 = 0xD0 if tall else 0xC0
        out.append((Scenario("terminator_%s" % ("224" if tall else "192"), vram, palette, base_regs(0x06, r1, 0x0C if tall else 0xFF, 0xFF), []), "terminator", tall))
    # collision
    for overlap in (False, True):
        vram = bytearray(0x4000)
        put_tile(vram, 1, [[3] * 8 for _ in range(8)])
        sat = 0x3F00
        for i, x in enumerate((20, 28 if not overlap else 24)):
            vram[sat + i] = 40
            vram[sat + 0x80 + 2 * i], vram[sat + 0x81 + 2 * i] = x, 1
        vram[sat + 2] = 0xD0
        out.append((Scenario("collision_%d" % overlap, vram, palette, base_regs(0x06, 0xC0, 0xFF, 0xFF), []), "collision", overlap))
    # F4 / U12: the sprite row offset wraps modulo 256, so a sprite whose top lies near the bottom of the 256-line space
    # reappears at the top. Tile row r is painted with colour r + 1, so the pattern row drawn on each line is visible.
    for tall in (False, True):
        for y, top_rows in ((0xFA, (5, 6, 7)), (0xFF, (0, 1, 2))):
            vram = bytearray(0x4000)
            put_tile(vram, 1, [[r + 1] * 8 for r in range(8)])
            sat = 0x3F00
            vram[sat] = y
            vram[sat + 0x80], vram[sat + 0x81] = 40, 1
            vram[sat + 1] = 0xE0  # second entry far below the screen (and not the terminator) so nothing else is drawn
            vram[sat + 0x82], vram[sat + 0x83] = 200, 1
            r1 = 0xD0 if tall else 0xC0
            scn = Scenario("y_wrap_%s_%02X" % ("224" if tall else "192", y), vram, palette, base_regs(0x06, r1, 0x0C if tall else 0xFF, 0xFF), [])
            out.append((scn, "y_wrap", top_rows))
    return [o[0] for o in out], out


def compare(scn, tmp, mutation=None):
    c = run_c(scn, tmp)
    m = model.render(scn.vram, scn.cram, scn.regs_at, mutation)
    return c, m, c == m


def main():
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="sms_render_"))
    scenarios = [random_scenario(seed) for seed in range(300)]
    dscn, dmeta = directed()
    scenarios += dscn
    cov = {"overflow": 0, "collision": 0, "hscroll_lock": 0, "vscroll_lock": 0, "left_blank": 0, "tall": 0, "sprites_8x16": 0,
           "zoom": 0, "shift": 0, "events": 0, "blank_display": 0}
    results = {}
    for scn in scenarios:
        c, m, same = compare(scn, tmp)
        results[scn.name] = (c, m)
        check(c is not None, "%s: C renderer failed" % scn.name)
        if c is None:
            continue
        if not same:
            diff = next((i for i, (a, b) in enumerate(zip(c[1], m[1])) if a != b), None)
            check(False, "%s: differs from the model (height %s/%s, flags %s/%s, first differing row %s)" %
                  (scn.name, c[0], m[0], c[2:], m[2:], diff))
        r = scn.regs
        cov["overflow"] += bool(c[2]); cov["collision"] += bool(c[3])
        cov["hscroll_lock"] += bool(r[0] & 0x40); cov["vscroll_lock"] += bool(r[0] & 0x80); cov["left_blank"] += bool(r[0] & 0x20)
        cov["tall"] += c[0] == 224; cov["sprites_8x16"] += bool(r[1] & 2); cov["zoom"] += bool(r[1] & 1); cov["shift"] += bool(r[0] & 8)
        cov["events"] += bool(scn.events); cov["blank_display"] += not r[1] & 0x40
    for name, value in cov.items():
        check(value >= 5, "coverage: scenario family %s exercised only %d times" % (name, value))
    # directed expectations come from the contract, not from either implementation
    for scn, kind, expect in dmeta:
        c = results[scn.name][0]
        if c is None:
            continue
        if kind == "overflow":
            check(c[2] == expect, "%s: overflow flag %s, expected %s" % (scn.name, c[2], expect))
        elif kind == "collision":
            check(c[3] == expect, "%s: collision flag %s, expected %s" % (scn.name, c[3], expect))
        elif kind == "terminator":
            # sprite 1 (Y 0xD0's successor) draws colour 7 at x=50 only when the list was not terminated
            cram_color = scn.cram[16 + 7] & 63
            drawn = any(row[50] == cram_color for row in c[1])
            check(drawn == expect, "%s: terminated-list sprite drawn=%s, expected %s" % (scn.name, drawn, expect))
        elif kind == "y_wrap":
            # the wrapped top rows of the sprite appear on lines 0..2 (x = 40), the line below them is empty
            want = [scn.cram[16 + r + 1] & 63 for r in expect]
            got = [c[1][line][40] for line in range(3)]
            check(got == want, "%s: wrapped sprite rows %s, expected %s" % (scn.name, got, want))
            check(c[1][8][40] == c[1][20][40] and c[1][8][40] not in want, "%s: the line below the wrapped sprite is not empty" % scn.name)
    # mutation controls: each wrong rule must be detected by at least one scenario
    for mutation in model.MUTATIONS:
        detected = False
        for scn in scenarios:
            c = results[scn.name][0]
            if c is not None and model.render(scn.vram, scn.cram, scn.regs_at, mutation) != c:
                detected = True
                break
        check(detected, "mutation %s was not detected by any scenario" % mutation)
    # determinism: the same scenario twice gives identical framebuffer hashes
    for scn in scenarios[:10]:
        a, b = run_c(scn, tmp), run_c(scn, tmp)
        check(a == b, "%s: non-deterministic render" % scn.name)
    digest = hashlib.sha256(b"".join(bytes(v) for s in scenarios for v in results[s.name][0][1] if results[s.name][0])).hexdigest()
    if FAILED:
        print("\n".join(FAILED[:40]))
        return 1
    print("ok: %d scenarios match the independent model; coverage %s; %d mutants detected; corpus sha256 %s" %
          (len(scenarios), cov, len(model.MUTATIONS), digest[:16]))
    return 0


sys.exit(main())
