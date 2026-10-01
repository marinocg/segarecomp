#!/usr/bin/env python3
"""SEG-009-T011: authorized local Master System images through the real `segarecomp build` route (skips without games/sms).

usage: sms_local_images_test.py <segarecomp-cli> <cc> <product-root>

Images are found under <product-root>/games/sms (git-ignored, never committed). Only ROM SHA-256 identities and aggregate
results are asserted; no ROM bytes, framebuffers, addresses or traces are stored. The mapper family of each image was
established once by a human from the public SMS Power! mapper/cartridge documentation (not from bytes) and is recorded here
per SHA-256; an image with an unknown identity is skipped, never guessed.
"""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

CLI, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
# sha256 -> (declared mapper, min distinct frame images, min mapper writes); source: SMS Power! Development/Mappers
IDENTITIES = {
    "6667e133818e36a214c81003e543f2c4c3ab5fa018ca313e3ac222a5a6e361c5": ("sega", 10, 100),
    "6ad738965ece231427ee046b9905cfee470d5c01220afdd934da4673e4a2458b": ("sega", 3, 100),
    "402b1c89b120a23c6057caf3d014f0e3074732264353c4a0e725d245b522e561": ("sega", 100, 100),
}
FRAMES = 300
failed = []


def check(cond, message):
    if not cond:
        failed.append(message)
        print("FAIL:", message)


def run(exe, art, *extra):
    art.mkdir(exist_ok=True)
    r = subprocess.run([str(exe), "--frames", str(FRAMES), "--cycle-budget", "2000000000", "--artifacts", str(art), *extra],
                       capture_output=True, text=True, timeout=900)
    return r.returncode, json.loads((art / "status.json").read_text()), (art / "state.sha256").read_text().strip(), art


def main():
    images = sorted((ROOT / "games" / "sms").glob("*.sms")) if (ROOT / "games" / "sms").is_dir() else []
    if not images:
        print("skipped: no local images under games/sms")
        return 0
    used = 0
    with tempfile.TemporaryDirectory(prefix="sms-local-") as tmpname:
        tmp = pathlib.Path(tmpname)
        for n, rom in enumerate(images):
            digest = hashlib.sha256(rom.read_bytes()).hexdigest()
            if digest not in IDENTITIES:
                print("skipped image %d: mapper identity not established" % n)
                continue
            mapper, min_distinct, min_writes = IDENTITIES[digest]
            out = tmp / ("b%d" % n)
            r = subprocess.run([CLI, "build", "--rom", str(rom), "--output", str(out), "--cc", CC, "--optimize", "0",
                               "--runtime-dir", str(ROOT / "platforms" / "master-system"), "--mapper", mapper],
                               capture_output=True, text=True, timeout=1800)
            check(r.returncode == 0, "%s: build failed" % digest[:12])
            if r.returncode != 0:
                continue
            used += 1
            rc, st, d1, art = run(out / ("game.exe" if sys.platform == "win32" else "game"), tmp / ("r1_%d" % n))
            check(rc == 0 and st["stop"] == "frame" and st["sms_error"] == "SMS_OK" and st["frames_completed"] == FRAMES,
                  "%s: did not sustain %d frames: %s" % (digest[:12], FRAMES, st))
            frames = [l.split()[3] for l in (art / "frames.txt").read_text().splitlines()]
            check(len(frames) == FRAMES and len(set(frames)) >= min_distinct, "%s: too few distinct frame images" % digest[:12])
            irq = [l.split() for l in (art / "irq.trace").read_text().splitlines()]
            check(sum(1 for e in irq if e[1:3] == ["frame", "accepted"]) >= FRAMES // 2, "%s: frame interrupts not accepted" % digest[:12])
            check(len((art / "mapper.trace").read_text().splitlines()) >= min_writes, "%s: no bank switching" % digest[:12])
            pcm = (art / "audio.pcm").read_bytes()
            check(any(pcm), "%s: PCM silent" % digest[:12])
            _, _, d2, _ = run(out / ("game.exe" if sys.platform == "win32" else "game"), tmp / ("r2_%d" % n))
            check(d1 == d2, "%s: two runs differ" % digest[:12])
            _, _, d3, _ = run(out / ("game.exe" if sys.platform == "win32" else "game"), tmp / ("r3_%d" % n), "--slice-cycles", "997")
            check(d1 == d3, "%s: slice split changes the state digest" % digest[:12])
    print("sms local images: %d used, %d failures" % (used, len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
