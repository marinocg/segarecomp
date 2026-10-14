#!/usr/bin/env python3
"""Fictional launcher/viewer UI fixture (SEG-058): synthetic ROMs, procedural pixel-art covers and a real library built through the packaged
launcher, for UI screenshots and manual review. Stdlib only.

Every byte is project-authored: tiny synthetic Genesis cartridges (a 12-byte stop program behind a header), synthetic Master System cartridges
(deterministic filler behind a `TMR SEGA` header) and covers drawn here from gradients and rectangles. No commercial ROM, cover scan or screenshot
is read or reproduced, and nothing here is ever committed to the product tree (the output goes to a throw-away directory).

usage: launcher_ui_fixture.py --launcher <Segarecomp executable> --out <dir>
Prints the environment to run the GUI against the fixture:  SEGARECOMP_CACHE_DIR=<out>/cache  (the library lives in <out>/cache/library).
"""
import argparse
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import sys
import zlib

# Reset SSP, reset PC=8, MOVEQ #0,D0 then RESET: a deterministic unsupported-instruction stop (what the package smoke test builds).
STOP_IMAGE = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))

GENESIS = [("Fictional Quest", 11), ("Neon Harbor Demo", 23), ("Pixel Orchard", 37)]
SMS = [("Coastal Drift (Fictional)", 5), ("Summit Chase (Fictional)", 17)]
COVERED = (0, 3)  # indexes of the fixture games that get a fictional user-chosen cover; the rest show the launcher's title card
PALETTES = [((20, 30, 90), (255, 140, 60), (30, 140, 90)), ((10, 10, 40), (80, 200, 240), (200, 60, 120)), ((60, 20, 80), (250, 220, 90), (40, 160, 110)),
            ((12, 40, 70), (240, 120, 90), (90, 200, 160)), ((30, 50, 20), (200, 230, 120), (240, 180, 60))]


def genesis_rom(title: str, seed: int) -> bytes:
    image = bytearray(0x200)  # like the package smoke test's cartridge: the stop program, a header, zero fill (distinct titles give distinct digests)
    image[: len(STOP_IMAGE)] = STOP_IMAGE
    image[0x100:0x110] = b"SEGA GENESIS    "
    name = title.upper().encode("ascii")[:47].ljust(48)
    image[0x120:0x150] = name
    image[0x150:0x180] = name
    image[0x1F0:0x200] = b"U               "
    image[0x1E0] = seed & 0xFF  # a distinct byte in the (unused) checksum/I-O area
    return bytes(image)


def sms_rom(seed: int) -> bytes:
    image = bytearray((i * (31 + seed) + 7) & 0xFF for i in range(0x8000))
    image[0x7FF0:0x7FF8] = b"TMR SEGA"
    image[0x7FFF] = 0x4C
    return bytes(image)


def png(width: int, height: int, rgb: bytes) -> bytes:
    raw = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


class Canvas:
    """A tiny 96x128 RGB pixel canvas for the fictional covers (rectangles, circles and stepped gradients only)."""

    def __init__(self, w=96, h=128):
        self.w, self.h = w, h
        self.px = [[(0, 0, 0)] * w for _ in range(h)]

    def gradient(self, y0, y1, top, bottom, steps=10):
        for y in range(y0, y1):
            t = int((y - y0) / max(1, y1 - y0) * steps) / steps  # stepped: pixel art, not a smooth ramp
            colour = tuple(int(top[c] * (1 - t) + bottom[c] * t) for c in range(3))
            for x in range(self.w):
                self.px[y][x] = colour

    def rect(self, x0, y0, x1, y1, colour):
        for y in range(max(0, y0), min(self.h, y1)):
            for x in range(max(0, x0), min(self.w, x1)):
                self.px[y][x] = colour

    def disc(self, cx, cy, r, colour):
        for y in range(self.h):
            for x in range(self.w):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    self.px[y][x] = colour

    def png(self):
        big = bytearray()
        for y in range(self.h * 2):
            row = bytearray()
            for x in range(self.w * 2):
                row += bytes(self.px[y // 2][x // 2])
            big += row
        return png(self.w * 2, self.h * 2, bytes(big))


def cover(index: int) -> bytes:
    """A fictional 96x128 'box art', one distinct scene per index (peaks, night city, ruins, coast), upscaled 2x by the launcher. Original
    rectangles only: no game, character or trademark is depicted."""
    c = Canvas()
    kind = index % 4
    if kind == 0:  # snowy peaks at dusk
        c.gradient(0, 128, (26, 42, 112), (250, 150, 96))
        c.disc(70, 46, 11, (255, 226, 150))
        for k, (x0, top, width) in enumerate(((-10, 52, 70), (30, 38, 80), (62, 60, 50))):
            for y in range(top, 128):
                half = (y - top) * width // (128 - top) // 2 + 1
                mid = x0 + width // 2
                shade = (40 + k * 12, 52 + k * 10, 110 + k * 14)
                c.rect(mid - half, y, mid + half, y + 1, shade)
                if y < top + 12:
                    c.rect(mid - half // 2, y, mid + half // 2, y + 1, (232, 238, 255))
        c.rect(0, 108, 96, 128, (24, 70, 80))
    elif kind == 1:  # night skyline
        c.gradient(0, 128, (8, 10, 42), (62, 40, 120), 8)
        c.disc(24, 26, 8, (236, 240, 255))
        for sx, sy in ((50, 10), (68, 22), (82, 8), (40, 44), (88, 40)):
            c.rect(sx, sy, sx + 1, sy + 1, (230, 235, 255))
        x = 0
        seed = 7
        while x < 96:
            seed = (seed * 37 + 11) % 97
            width, height = 8 + seed % 9, 30 + seed % 44
            c.rect(x, 128 - height, x + width - 1, 128, (10 + seed % 14, 14 + seed % 12, 40 + seed % 20))
            for wy in range(128 - height + 4, 124, 6):
                for wx in range(x + 2, x + width - 3, 4):
                    if (wx * 7 + wy * 13 + seed) % 5 < 2:
                        c.rect(wx, wy, wx + 2, wy + 3, (250, 214, 110))
            x += width
        c.rect(0, 116, 96, 128, (14, 30, 90))
    elif kind == 2:  # ruins in a green valley with a waterfall
        c.gradient(0, 128, (44, 96, 120), (170, 214, 150), 8)
        for k in range(5):
            c.rect(6 + k * 20, 40 + (k % 2) * 8, 16 + k * 20, 100, (60, 54, 86))
            c.rect(4 + k * 20, 36 + (k % 2) * 8, 18 + k * 20, 42 + (k % 2) * 8, (84, 76, 116))
        c.rect(38, 60, 52, 128, (150, 210, 240))
        c.rect(40, 60, 42, 128, (230, 246, 255))
        for k in range(14):
            c.rect(k * 8 - 4, 96 + (k * 5) % 9, k * 8 + 9, 128, (24, 90 + (k % 3) * 14, 52))
    else:  # sun over the sea with palm silhouettes
        c.gradient(0, 70, (60, 30, 110), (255, 140, 80), 9)
        c.disc(48, 66, 18, (255, 222, 130))
        c.gradient(70, 128, (30, 70, 160), (10, 24, 80), 7)
        for k in range(7):
            c.rect(28 + k * 6 - (k % 2) * 4, 76 + k * 7, 68 - k * 6, 78 + k * 7, (255, 200, 120))
        c.rect(12, 56, 16, 128, (26, 22, 38))
        for dx, dy in ((-14, 4), (-8, -2), (0, -4), (8, -2), (14, 4)):
            c.rect(14 + dx - 6, 56 + dy, 14 + dx + 6, 59 + dy, (22, 70, 52))
    return c.png()


def run(cmd, env):
    result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=1800)
    return result.returncode, result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--launcher", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--no-build", action="store_true", help="import only (no builds)")
    args = parser.parse_args()
    out = pathlib.Path(args.out).resolve()
    roms = out / "roms"
    roms.mkdir(parents=True, exist_ok=True)
    cache = out / "cache"
    env = dict(os.environ, SEGARECOMP_CACHE_DIR=str(cache), SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    entries = []
    for i, (title, seed) in enumerate(GENESIS):
        path = roms / (title + ".md")
        path.write_bytes(genesis_rom(title, seed))
        entries.append((path, title, "genesis", i))
    for i, (title, seed) in enumerate(SMS):
        data = sms_rom(seed)
        path = roms / (title + ".sms")
        path.write_bytes(data)
        path.with_suffix(".mapper.json").write_text(json.dumps({"mapper": "sega", "sha256": hashlib.sha256(data).hexdigest()}))
        entries.append((path, title, "master-system", len(GENESIS) + i))
    # one more SMS cartridge with NO mapper declaration: it imports fine and must be declared before it can be built
    undeclared = roms / "Mystery Cart (Fictional).sms"
    undeclared.write_bytes(sms_rom(41))
    entries.append((undeclared, "Mystery Cart (Fictional)", "master-system", len(GENESIS) + len(SMS)))
    # a Genesis header with an odd reset program counter: the product refuses it, so the library shows a real failed build with its log
    broken = roms / "Broken Vector (Fictional).md"
    image = bytearray(genesis_rom("BROKEN VECTOR", 99))
    image[4:8] = bytes((0x00, 0x10, 0x00, 0x01))
    broken.write_bytes(bytes(image))
    failing = (broken, "Broken Vector (Fictional)", "genesis", len(GENESIS) + len(SMS) + 1)
    entries.append(failing)
    for path, title, platform, index in entries:
        rc, text = run([args.launcher, "--import", str(path)], env)
        if rc != 0:
            sys.exit(f"import failed for {title}: {text}")
    if not args.no_build:
        rc, text = run([args.launcher, "--build", str(broken)], env)
        print(f"build {failing[1]}: rc={rc} (a failure is expected)")
        if rc == 0:
            sys.exit("the deliberately broken cartridge built")
        for path, title, platform, index in entries[:-2]:
            rc, text = run([args.launcher, "--build", str(path)], env)
            print(f"build {title}: rc={rc}")
            if rc != 0:
                sys.exit(text)
        # Pixel Orchard is also built with the Optimized policy (a real build: the product decides the effective policy and any fallback)
        path = entries[2][0]
        rc, text = run([args.launcher, "--build", str(path), "--aot-policy", "optimized"], env)
        print(f"build {entries[2][1]} (optimized): rc={rc}")
        if rc != 0:
            sys.exit(text)
    if not args.no_build:
        # One headless start of the launcher adopts the finished builds into the library (build records, current build key), exactly as a user's
        # first start would; the per-game choices below are then made on top of real records.
        shot = out / "adopt.bmp"
        subprocess.run([args.launcher], env=dict(env, SEGARECOMP_SHOT=str(shot), SEGARECOMP_SHOT_PAGE="library", SEGARECOMP_SHOT_FRAMES="6",
                                                 SDL_RENDER_DRIVER="software"), capture_output=True, timeout=300)
        shot.unlink(missing_ok=True)
    lib = cache / "library"
    catalog = json.loads((lib / "library.json").read_text())
    now = int(__import__("time").time())
    (lib / "covers").mkdir(exist_ok=True)
    for game in catalog["games"]:
        for path, title, platform, index in entries:
            if hashlib.sha256(path.read_bytes()).hexdigest() == game["sha256"]:
                if index in COVERED:  # the others keep the launcher's title card (no cover chosen)
                    (lib / "covers" / (game["sha256"] + ".png")).write_bytes(cover(index))
                    game["cover"] = game["sha256"] + ".png"
                game["display_title"] = title
                game["last_played_at"] = now - 86400 * (index * 2 + 1) if index < 4 else 0
                game["favorite"] = index in (0, 3)
                game["last_built_at"] = now - 86400 * (index * 3 + 2) if index < len(GENESIS) + len(SMS) else 0
                if index in (1, 2):  # Neon Harbor: Optimized selected after a Compatibility build; Pixel Orchard: really built Optimized
                    game["aot_policy"] = "optimized"
                if index == 4 and not args.no_build:  # Summit Chase: kept as metadata only (its build folders deleted, as Storage's archive does)
                    import shutil
                    for build in game.get("builds", []):
                        shutil.rmtree(cache / "games" / build["key"], ignore_errors=True)
                    if game.get("current_build_key"):
                        shutil.rmtree(cache / "games" / game["current_build_key"], ignore_errors=True)
                    game["archived"] = True
    (lib / "library.json").write_text(json.dumps(catalog, indent=2))
    # Summit Chase's ROM was moved after it was archived: the library shows the missing-ROM recovery (Locate ROM) for a metadata-only game
    summit = entries[len(GENESIS) + 1][0]
    if summit.exists():
        summit.rename(summit.with_name(summit.stem + " (moved away).sms"))
    print(f"SEGARECOMP_CACHE_DIR={cache}")


if __name__ == "__main__":
    main()
