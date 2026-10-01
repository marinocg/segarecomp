#!/usr/bin/env python3
"""SEG-009-T002: a 512 KiB synthetic SMS cartridge through the SMS generation route stays inside the ADR 0058 shape/budget.

usage: sms_emission_budget_test.py <sms_image_emitter> <z80_image_emitter> <product-root> [quick|512k]

  Two tiers (the 512 KiB emission is about 390 MiB of C and takes minutes, so it is not in the per-host `full` gate):
  * `quick` (default, label full): a 128 KiB ROM, emitted twice for byte-identical determinism, and compared with the
    generic emitter over the ADR 0058 reference shape for that size;
  * `512k` (label extended): the 512 KiB ROM emitted once, the ADR 0058 generated-C budget, and the byte-identity
    comparison with the generic emitter at the full reference shape.

  * emission is byte-identical across two runs of the SMS route;
  * the SMS route's ImageSet is exactly the ADR 0058 reference shape measured by `tools/z80_static_budget.py`
    (`banked512-random`-style map: invariant first 1 KiB + 32 banked 16 KiB images with the three slot windows): the
    emitted image files are byte-identical to those of the generic emitter run over `z80_static_budget.sms_spec`, so the
    SEG-008 measured cost (generated C, executable, compile time, lookup) applies unchanged;
  * the generated C size is within the 512 KiB generated-C budget of `z80_static_budget.BUDGETS_512K`;
  * SEG-033-T006 production build-shape invariants (structural, host-independent: no wall-clock threshold): the exact entry
    count is the image's start count, host owners hold at most 128 entries and at least 100 on average, the host function count (owners + shared effect bodies) stays an order of magnitude below the entry
    count, no translation unit exceeds the per-unit size bound and the total generated C stays inside the shape budget. A
    regression to one function per start (the defect SEG-033 fixed) fails every one of them.
"""
import hashlib
import pathlib
import random
import re
import shutil
import subprocess
import sys
import tempfile

SMS_EMITTER, Z80_EMITTER = sys.argv[1], sys.argv[2]
ROOT = pathlib.Path(sys.argv[3]).resolve()
MODE = sys.argv[4] if len(sys.argv) > 4 else "quick"
assert MODE in ("quick", "512k"), MODE
ROM_SIZE = 0x80000 if MODE == "512k" else 0x20000
RUN_NAMES = ("a",) if MODE == "512k" else ("a", "b")
sys.path.insert(0, str(ROOT / "tools"))
import sms_fixture_rom as builder  # noqa: E402
import z80_static_budget as budget  # noqa: E402

# SEG-033-T006 shape budgets per ROM size (measured 2026-10-01 on the seeded random image, which has the most distinct effect bodies:
# 128 KiB -> 132,096 entries, 1,032 owners, 9,043 shared bodies, 47 MiB, largest unit 2.7 MB; 512 KiB -> 525,312 / 4,104 / 23,525 / 186 MiB, largest 4.3 MB).
SHAPE_BUDGETS = {
    0x20000: {"entries": 1024 + 8 * 0x4000, "owners": 1100, "functions": 20000, "max_tu_mib": 4.0, "total_mib": 64.0},
    0x80000: {"entries": 1024 + 32 * 0x4000, "owners": 4300, "functions": 80000, "max_tu_mib": 8.0, "total_mib": 256.0},
}
EXTRA = {"sms_rom.c", "sms_cartridge.json", "sms.units"}  # SMS-route additions beyond the image emitter's files
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)


def digests(directory, skip=()):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(pathlib.Path(directory).iterdir())
            if p.is_file() and p.name not in skip}


def size_mib(directory):
    return sum(p.stat().st_size for p in pathlib.Path(directory).iterdir() if p.is_file()) / (1024 * 1024)


def check_shape(stdout, directory):
    """SEG-033-T006: structural build-shape invariants of the emitted image (no timing)."""
    shape = next((dict(item.split("=") for item in line.split()[1:]) for line in stdout.splitlines() if line.startswith("shape ")), None)
    check(shape is not None, "the emitter reports no shape line")
    if shape is None:
        return
    shape = {k: int(v) for k, v in shape.items()}
    budget = SHAPE_BUDGETS[ROM_SIZE]
    check(shape["entries"] == budget["entries"], "exact entry count changed: %d, expected %d (every start keeps its entry)" % (shape["entries"], budget["entries"]))
    check(shape["owners"] <= budget["owners"], "host owner count %d exceeds %d (owner grouping regressed)" % (shape["owners"], budget["owners"]))
    check(shape["max_group"] <= 128, "an owner holds %d entries (bound 128)" % shape["max_group"])
    check(shape["entries"] / max(1, shape["owners"]) >= 100, "owner grouping ratio below 100 entries per owner")
    functions = shape["owners"] + shape["shared_bodies"]
    check(functions <= budget["functions"], "host function count %d exceeds %d" % (functions, budget["functions"]))
    files = [p for p in pathlib.Path(directory).iterdir() if p.suffix == ".c"]
    largest = max(p.stat().st_size for p in files) / (1024 * 1024)
    check(largest <= budget["max_tu_mib"], "largest translation unit %.1f MiB exceeds %.1f MiB" % (largest, budget["max_tu_mib"]))
    check(size_mib(directory) <= budget["total_mib"], "generated C %.1f MiB exceeds the %.1f MiB shape budget" % (size_mib(directory), budget["total_mib"]))
    defined = sum(len(re.findall(r"^(?:static )?struct Z80OwnerRef z80_o_\w+\(struct Z80Runtime \*rt, uint16_t window_base\) \{$", p.read_text(), re.M))
                  for p in files)
    check(defined == shape["owners"], "the C defines %d owner functions, the emitter reported %d" % (defined, shape["owners"]))


def main():
    rng = random.Random(512)
    rom = bytearray(rng.randrange(256) for _ in range(ROM_SIZE))
    builder.write_header(rom, ROM_SIZE)
    rom = bytes(rom)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        (tmp / "big.sms").write_bytes(rom)
        runs = []
        for name in RUN_NAMES:
            r = subprocess.run([SMS_EMITTER, tmp / "big.sms", tmp / name, "sms", "--mapper", "sega"], capture_output=True, text=True, timeout=900)
            check(r.returncode == 0 and r.stdout.startswith("ok mapper=sega"), "SMS emission %s failed: %s" % (name, r.stdout[:300]))
            runs.append(r.stdout)
        if FAILED:
            return
        check_shape(runs[0], tmp / "a")
        if MODE == "quick":
            first, second = digests(tmp / "a"), digests(tmp / "b")
            check(first == second and runs[0] == runs[1], "SMS emission of a 128 KiB ROM is not byte-identical across two runs")
            shutil.rmtree(tmp / "b")
        else:
            check(size_mib(tmp / "a") <= budget.BUDGETS_512K["generated_c_mib"], "generated C exceeds the ADR 0058 512 KiB budget: %.0f MiB" % size_mib(tmp / "a"))
        banks = [rom[i:i + 0x4000] for i in range(0, len(rom), 0x4000)]
        (tmp / "ref.spec").write_text(budget.sms_spec(rom[:0x400], banks), encoding="utf-8")
        ref = subprocess.run([Z80_EMITTER, tmp / "ref.spec", tmp / "ref", "sms"], capture_output=True, text=True, timeout=900)
        check(ref.returncode == 0, "reference emission failed: " + ref.stdout[:300])
        if ref.returncode == 0:
            expected = digests(tmp / "ref", skip={"sms.units"})
            got = digests(tmp / "a", skip=EXTRA)
            check(expected == got, "the SMS route's image is not byte-identical to the ADR 0058 reference shape (%d vs %d files)" % (len(got), len(expected)))
            check(len(expected) > 10, "reference emission produced too few files")
    if FAILED:
        print("\n".join(FAILED[:10]))
        return 1
    print("sms %s emission: ok" % ("512 KiB" if MODE == "512k" else "128 KiB"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
