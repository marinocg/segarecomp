#!/usr/bin/env python3
"""SEG-122-T011 (ADR 0271 check F1b, ADR 0272): a visible raster effect produced by H-interrupt handlers reached through a work-RAM `JMP` stub,
generated-native versus the pinned Genesis Plus GX (existing tests/genesis_oracle infrastructure).

Fixture: tests/genesis_hint_ram_jmp_fixture.py `build_raster_cycle`: handler A writes backdrop CRAM[0] red and retargets the stub operand to handler B, B writes green and
retargets back; H counter 79 gives two interrupts per frame. Steady state is three horizontal bands (boundaries near rows 80 and 160). Checked per machine, on the
last 10 of 40 captured frames: exactly two boundaries, each within 3 rows of 80 and 160, colours only red and green with the pattern x y x (both handlers ran, so the retarget
selected the right handler on alternate interrupts), and the boundaries of the two machines within 3 rows of each other. Control: with the H-interrupt disabled no boundary in either.
(The one-shot variant `build_raster` is not asserted: it enables the interrupt mid-frame, an edge where the generated scheduler and the reference differ; ADR 0272.)
The generated half always runs; the reference half is skipped loudly unless SEGARECOMP_GENESIS_ORACLE_CHECKOUT holds the pinned checkout, and
SEGARECOMP_REQUIRE_GENESIS_REFERENCE=1 turns that skip into a failure.
usage: genesis_hint_raster_reference_test.py <segarecomp cli> <cc> <repo root>
"""
import glob
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "genesis_oracle"))
sys.path.insert(0, str(HERE))
import pins  # noqa: E402
import genesis_hint_ram_jmp_fixture as fixture  # noqa: E402

NAMES = ["ekeeke_Genesis-Plus-GX"]
FRAMES = 40


def run(command, **kwargs):
    return subprocess.run(command, text=True, capture_output=True, **kwargs)


def classes(path):
    data = pathlib.Path(path).read_bytes().split(b"\n", 3)
    width, height = map(int, data[1].split())
    pixels, out = data[3], []
    for y in range(height):
        i = (y * width + width // 2) * 3
        r, g, b = pixels[i:i + 3]
        out.append("R" if r > 80 and r > g and r > b else "G" if g > 80 and g > r and g > b else "B" if b > 80 and b > r and b > g else ".")
    return out


def bands(frame):
    runs = []
    for c in frame:
        if runs and runs[-1][0] == c:
            runs[-1][1] += 1
        else:
            runs.append([c, 1])
    return runs


def ordered(directory):
    return sorted(glob.glob(str(directory / "frame-*.ppm")), key=lambda p: int(pathlib.Path(p).stem.split("-")[1]))


def native(cli, cc, root, work, name, rom):
    path = work / f"{name}.md"
    path.write_bytes(rom)
    out = work / f"nb_{name}"
    built = run([cli, "build", "--rom", str(path), "--output", str(out), "--cc", cc, "--runtime-dir", str(pathlib.Path(root) / "platforms/genesis"), "--platform", "genesis",
                 "--optimize", "1", "--jobs", "4"], timeout=900)
    assert built.returncode == 0, built.stdout[-2000:] + built.stderr[-2000:]
    frames = work / f"nf_{name}"
    frames.mkdir()
    env = {"PATH": os.environ["PATH"], "SEGARECOMP_CAPTURE_DIR": str(frames), "SEGARECOMP_CAPTURE_FIRST": "1", "SEGARECOMP_CAPTURE_COUNT": str(FRAMES),
           "SEGARECOMP_CAPTURE_STRIDE": "1", "SEGARECOMP_CAPTURE_SLICE": "1024"}
    done = run([str(out / "game"), "--instruction-budget", "80000000"], env=env, timeout=300)
    assert len(ordered(frames)) == FRAMES, (done.returncode, done.stderr[-500:])
    return [classes(p) for p in ordered(frames)]


def reference(host, core, work, name, rom):
    path = work / f"ref_{name}.md"
    path.write_bytes(rom)
    frames = work / f"rf_{name}"
    frames.mkdir()
    done = run([host, core, str(path), str(FRAMES), "--capture", str(frames), "1", "1", str(FRAMES), "genesis_plus_gx_system_hw=mega drive / genesis",
                "genesis_plus_gx_region_detect=ntsc-u", "genesis_plus_gx_bios=disabled"], timeout=300)
    assert done.returncode in (0, 4), done.stderr
    assert len(ordered(frames)) == FRAMES
    return [classes(p) for p in ordered(frames)]


def steady(frames):
    """Boundaries and colours of the last 10 frames; asserts the three-band pattern x y x with x, y in {R, G}, x != y."""
    result = set()
    for frame in frames[-10:]:
        b = bands(frame)
        assert len(b) == 3 and {c for c, _ in b} == {"R", "G"} and b[0][0] == b[2][0] != b[1][0], b
        result.add((b[0][0], b[0][1], b[0][1] + b[1][1]))
    assert len(result) == 1, result  # stable across the ten frames
    return result.pop()


def main():
    cli, cc, root = sys.argv[1:4]
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)
        on, off = fixture.build_raster_cycle(True), fixture.build_raster_cycle(False)
        got_on, got_off = native(cli, cc, root, work, "on", on), native(cli, cc, root, work, "off", off)
        n_colour, n1, n2 = steady(got_on)
        assert abs(n1 - 80) <= 3 and abs(n2 - 160) <= 3, (n1, n2)
        assert all(len(bands(f)) == 1 for f in got_off), "no H-interrupt: no boundary"
        print("generated-native: bands %s boundaries %d/%d; disabled control uniform" % (n_colour, n1, n2))
        checkout = pins.checkout(NAMES)
        if checkout is None:
            if os.environ.get("SEGARECOMP_REQUIRE_GENESIS_REFERENCE") == "1":
                print("FAIL: reference required but unavailable: " + pins.skip_reason(NAMES))
                return 1
            print("SKIPPED reference half (NOT executed): " + pins.skip_reason(NAMES))
            return 0
        host = work / "host"
        subprocess.run([cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-o", str(host), str(HERE / "genesis_oracle" / "libretro_host.c"), "-ldl"], check=True)
        tree = pins.private_copy(checkout, "ekeeke_Genesis-Plus-GX", work)
        subprocess.run(["make", "-j" + str(max(1, min(8, os.cpu_count() or 1))), "-f", "Makefile.libretro"], cwd=tree, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=900)
        core = [p for p in glob.glob(str(tree / "genesis_plus_gx_libretro.*")) if p.endswith((".so", ".dylib"))][0]
        r_on, r_off = reference(str(host), core, work, "on", on), reference(str(host), core, work, "off", off)
        r_colour, r1, r2 = steady(r_on)
        assert all(len(bands(f)) == 1 for f in r_off), "reference: no H-interrupt, no boundary"
        assert abs(r1 - 80) <= 3 and abs(r2 - 160) <= 3 and abs(r1 - n1) <= 3 and abs(r2 - n2) <= 3, (r1, r2, n1, n2)
        print("reference: bands %s boundaries %d/%d; generated %d/%d: agree within 3 rows" % (r_colour, r1, r2, n1, n2))
        print("independent raster differential: ok (reference half executed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
