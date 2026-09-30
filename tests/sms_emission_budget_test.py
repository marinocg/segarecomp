#!/usr/bin/env python3
"""SEG-009-T002: a 512 KiB synthetic SMS cartridge through the SMS generation route stays inside the ADR 0058 shape/budget.

usage: sms_emission_budget_test.py <sms_image_emitter> <z80_image_emitter> <product-root>

  * emission is byte-identical across two runs of the SMS route;
  * the SMS route's ImageSet is exactly the ADR 0058 reference shape measured by `tools/z80_static_budget.py`
    (`banked512-random`-style map: invariant first 1 KiB + 32 banked 16 KiB images with the three slot windows): the
    emitted image files are byte-identical to those of the generic emitter run over `z80_static_budget.sms_spec`, so the
    SEG-008 measured cost (generated C, executable, compile time, lookup) applies unchanged;
  * the generated C size is within the 512 KiB generated-C budget of `z80_static_budget.BUDGETS_512K`.
"""
import hashlib
import pathlib
import random
import shutil
import subprocess
import sys
import tempfile

SMS_EMITTER, Z80_EMITTER = sys.argv[1], sys.argv[2]
ROOT = pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
import sms_fixture_rom as builder  # noqa: E402
import z80_static_budget as budget  # noqa: E402

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


def main():
    rng = random.Random(512)
    rom = bytearray(rng.randrange(256) for _ in range(0x80000))
    builder.write_header(rom, 0x80000)
    rom = bytes(rom)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        (tmp / "big.sms").write_bytes(rom)
        runs = []
        for name in ("a", "b"):
            r = subprocess.run([SMS_EMITTER, tmp / "big.sms", tmp / name, "sms", "--mapper", "sega"], capture_output=True, text=True, timeout=900)
            check(r.returncode == 0 and r.stdout.startswith("ok mapper=sega"), "SMS emission %s failed: %s" % (name, r.stdout[:300]))
            runs.append(r.stdout)
        if FAILED:
            return
        first, second = digests(tmp / "a"), digests(tmp / "b")
        check(first == second and runs[0] == runs[1], "SMS emission of a 512 KiB ROM is not byte-identical across two runs")
        check(size_mib(tmp / "a") <= budget.BUDGETS_512K["generated_c_mib"], "generated C exceeds the ADR 0058 512 KiB budget: %.0f MiB" % size_mib(tmp / "a"))
        shutil.rmtree(tmp / "b")
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
    print("sms 512 KiB emission: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
