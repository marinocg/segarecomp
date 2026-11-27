#!/usr/bin/env python3
"""SEG-147-T007 (ADR 0324): the Master System asset-replacement example. The committed mod-owned adapter turns the declared text assets into typed edits; the packaged variant builds through the
generated-native route and renders the expected frame; the unmodified cartridge renders the original one. The expected frames are computed HERE from the asset text and the fixture's declared screen
layout (not with the adapter's encoders and not by the C renderer). Out-of-range assets, a mismatched original and a stale edit file fail closed and write nothing. Synthetic data only; bounded runs.
usage: mod_sms_assets_test.py <segarecomp-cli> <c-compiler> <product-root>"""
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from mod_test_support import Cli, sha, temp, write  # noqa: E402

sys.dont_write_bytecode = True
exe, compiler, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
example = root / "platforms" / "master-system" / "sdk" / "examples" / "asset-replace"
sys.path.insert(0, str(example))
import fixture_rom as fixture  # noqa: E402

cli = Cli(exe)
tmp = temp()
image = fixture.build()
rom = write(tmp / "demo.sms", image)
assert fixture.build() == image


def adapter(project, rom_path, *flags):
    return subprocess.run([sys.executable, "-B", str(example / "encode_assets.py"), str(rom_path), "--project", str(project), *flags], text=True, capture_output=True, timeout=120)


r = adapter(example, rom, "--check")
assert r.returncode == 0, r.stderr   # the committed edit file is exactly what the committed assets produce

package = tmp / "assets.segapatch"
rc, packed, p = cli.run("mod", "pack", example, "--out", package, "--rom", rom)
assert rc == 0 and packed["ok"] and packed["payloads"]["assets"] == 4 and packed["trust_class"] == "data", (rc, packed, p.stderr)


# ---- the independent expectation: a 256x192 frame of CRAM bytes ------------------------------------------------------------------------------------------
def entries(path):
    return [line for line in path.read_text(encoding="ascii").splitlines() if line.strip() and not line.startswith("#")]


def expected_frame(modded):
    palette = [(i * 5) & 0x3F for i in range(16)]
    original = fixture.original_tiles()
    tiles = [[[sum(((original[n * 32 + y * 4 + plane] >> (7 - x)) & 1) << plane for plane in range(4)) for x in range(8)] for y in range(8)] for n in range(40)]
    room = [[27 + (x + y) % 4 for x in range(8)] for y in range(4)]
    text = "VANILLA ROOM"
    if modded:
        for line in entries(example / "assets" / "palette.txt"):
            i, red, green, blue = map(int, line.split())
            palette[i] = red + 4 * green + 16 * blue
        rows = []
        for line in entries(example / "assets" / "tiles.txt"):
            if line.startswith("tile "):
                rows = tiles[int(line.split()[1])] = []
            else:
                rows.append([int(c, 16) for c in line])
        room = [[int(v) for v in line.split()] for line in entries(example / "assets" / "room.txt")]
        text = entries(example / "assets" / "text.txt")[0]
    names = [[0] * 32 for _ in range(24)]
    for y in range(4):
        names[4 + y][4:12] = room[y]
    names[10][8:24] = [" ABCDEFGHIJKLMNOPQRSTUVWXYZ".index(c) for c in text.ljust(16)]
    return bytes(palette[tiles[names[y // 8][x // 8]][y % 8][x % 8]] for y in range(192) for x in range(256))


def render(name, *extra):
    out = tmp / name
    r = subprocess.run([exe, "build", "--rom", str(rom), "--output", str(out), "--cc", compiler, "--optimize", "0", "--runtime-dir", str(root / "platforms" / "master-system"),
                        "--cc-arg", "-Wno-misleading-indentation", *map(str, extra)], text=True, capture_output=True, timeout=900)
    assert r.returncode == 0, (r.stdout[-2000:], r.stderr[-2000:])
    artifacts = out / "artifacts"
    artifacts.mkdir()
    r = subprocess.run([str(out / ("game.exe" if sys.platform == "win32" else "game")), "--frames", "20", "--artifacts", str(artifacts)], text=True, capture_output=True, timeout=120)
    assert r.returncode == 0, (r.stdout, r.stderr)
    return json.loads((artifacts / "frame.json").read_text()), (artifacts / "framebuffer.bin").read_bytes()


frames = {}
for name, modded, extra in [("vanilla", False, ("--mapper", "rom_only")), ("modded", True, ("--data-package", package, "--data-identity", packed["package_identity"]))]:
    meta, pixels = render(name, *extra)
    want = expected_frame(modded)
    differing = sum(a != b for a, b in zip(pixels, want))
    assert (meta["width"], meta["height"]) == (256, 192) and pixels == want and meta["sha256"] == hashlib.sha256(want).hexdigest(), (name, meta, len(pixels), differing)
    frames[name] = meta["sha256"]
assert frames["vanilla"] != frames["modded"] and rom.read_bytes() == image


# ---- fail closed: ranges, sizes, the original, and nothing written -----------------------------------------------------------------------------------------
def copy_project(name):
    d = tmp / name
    shutil.copytree(example, d, ignore=shutil.ignore_patterns("__pycache__"))
    return d


for n, (asset, content, says) in enumerate([
        ("palette.txt", "0 0 0 4\n", "levels 0..3"), ("palette.txt", "16 0 0 0\n", "out of range"), ("palette.txt", "1 0 0 0\n1 1 1 1\n", "repeated"),
        ("tiles.txt", "tile 40\n" + "00000000\n" * 8, "out of range"), ("tiles.txt", "tile 1\n" + "00000000\n" * 7, "not 8"), ("tiles.txt", "tile 1\n" + "0000000g\n" * 8, "hexadecimal"),
        ("tiles.txt", "00000000\n", "after a `tile` line"), ("room.txt", ("40 " * 7 + "40\n") * 4, "0..39"), ("room.txt", ("1 " * 7 + "1\n") * 5, "4 rows"),
        ("text.txt", "ABCDEFGHIJKLMNOPQ\n", "at most 16"), ("text.txt", "lower case\n", "at most 16"), ("text.txt", "A\nB\n", "one line")]):
    d = copy_project(f"bad{n}")
    (d / "edits" / "main.edits").write_text("untouched\n")
    write(d / "assets" / asset, content)
    r = adapter(d, rom)
    assert r.returncode == 1 and says in r.stderr and (d / "edits" / "main.edits").read_text() == "untouched\n", (asset, says, r.returncode, r.stderr)

d = copy_project("stale")
write(d / "assets" / "text.txt", "CHANGED\n")
r = adapter(d, rom, "--check")
assert r.returncode == 1 and "stale" in r.stderr, r.stderr

mismatched = bytearray(image)
mismatched[fixture.TILES + 40] ^= 0xFF
mismatched = bytes(mismatched)
other = write(tmp / "other.sms", mismatched)
r = adapter(example, other)
assert r.returncode == 1 and "not a revision" in r.stderr, r.stderr
d = copy_project("same-label-other-bytes")   # even a mod that declares the other image is refused: the region no longer holds the bytes the adapter understands
write(d / "mod.toml", (d / "mod.toml").read_text().replace(sha(image), sha(mismatched)))
before = (d / "edits" / "main.edits").read_bytes()
r = adapter(d, other)
assert r.returncode == 1 and "original bytes of the region" in r.stderr and (d / "edits" / "main.edits").read_bytes() == before, r.stderr
rc, doc, _ = cli.run("mod", "apply", package, "--rom", other, "--out", tmp / "never.sms")
assert rc == 1 and doc["class"] == "E_IMAGE_MISMATCH" and not (tmp / "never.sms").exists(), (rc, doc)
rc, doc, _ = cli.run("mod", "apply", d, "--rom", other, "--out", tmp / "never.sms")   # the stale expected values are asserted again by the build
assert rc == 1 and doc["class"] == "E_EDIT_EXPECTED_MISMATCH" and not (tmp / "never.sms").exists(), (rc, doc)
print("mod_sms_assets_test: ok", frames)
