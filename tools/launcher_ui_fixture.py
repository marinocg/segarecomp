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


def cover(index: int) -> bytes:
    """A fictional 96x128 'box art' (pixel-stepped sky gradient, a sun and two hills), upscaled 2x by the launcher."""
    w, h = 96, 128
    sky, sun, hill = PALETTES[index % len(PALETTES)]
    px = bytearray(w * h * 3)
    for y in range(h):
        for x in range(w):
            t = y / h
            band = int(t * 8) / 8  # stepped gradient: pixel art, not a smooth ramp
            colour = [int(sky[c] * (1 - band) + sun[c] * band * 0.6) for c in range(3)]
            if (x - 62) ** 2 + (y - 40) ** 2 < 14 ** 2:
                colour = list(sun)
            if y > 84 + int(10 * ((x // 8) % 3 == 0)) - (x // 6) % 5:
                colour = [int(hill[c] * (0.65 + 0.35 * ((x // 12 + y // 10) % 2))) for c in range(3)]
            if y > 112:
                colour = [int(c * 0.55) for c in colour]
            px[(y * w + x) * 3:(y * w + x) * 3 + 3] = bytes(colour)
    big = bytearray()
    for y in range(h * 2):
        row = bytearray()
        for x in range(w * 2):
            row += px[((y // 2) * w + (x // 2)) * 3:((y // 2) * w + (x // 2)) * 3 + 3]
        big += row
    return png(w * 2, h * 2, bytes(big))


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
    for path, title, platform, index in entries:
        rc, text = run([args.launcher, "--import", str(path)], env)
        if rc != 0:
            sys.exit(f"import failed for {title}: {text}")
    if not args.no_build:
        for path, title, platform, index in entries[:-1]:
            rc, text = run([args.launcher, "--build", str(path)], env)
            print(f"build {title}: rc={rc}")
            if rc != 0:
                sys.exit(text)
    lib = cache / "library"
    catalog = json.loads((lib / "library.json").read_text())
    now = int(__import__("time").time())
    (lib / "covers").mkdir(exist_ok=True)
    for game in catalog["games"]:
        for path, title, platform, index in entries:
            if hashlib.sha256(path.read_bytes()).hexdigest() == game["sha256"]:
                (lib / "covers" / (game["sha256"] + ".png")).write_bytes(cover(index))
                game["cover"] = game["sha256"] + ".png"
                game["display_title"] = title
                game["last_played_at"] = now - 86400 * (index * 2 + 1) if index < 4 else 0
                game["favorite"] = index in (0, 3)
    (lib / "library.json").write_text(json.dumps(catalog, indent=2))
    print(f"SEGARECOMP_CACHE_DIR={cache}")


if __name__ == "__main__":
    main()
