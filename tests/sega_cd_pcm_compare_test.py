#!/usr/bin/env python3
"""SEG-102: regression for tools/sega_cd_pcm_compare.py on synthetic logs - a matching prefix passes; an extra product operation, a declared-length
mismatch (truncation) and a diverging operation fail.  usage: sega_cd_pcm_compare_test.py <source-root>"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[1]).resolve()
TOOL = ROOT / "tools" / "sega_cd_pcm_compare.py"
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def ref_line(kind, off, val, abs_cycles):
    return "0 0 %s %X %X cyc 0 pcmcyc 0 st 00 en 1 a0 0 abs %d" % (kind, off, val, abs_cycles)


UNIT = 2148241  # product ticks per sub cycle; the reference counts four cycles per sub cycle
REF = [ref_line("Z", 0, 0, 0), ref_line("W", 8, 0xFE, 4000), ref_line("R", 0x10, 3, 400000), ref_line("W", 7, 0x80, 400100), ref_line("R", 0x11, 0, 800000)]
PRODUCT = ["Z 0 0", "W 0 %d 8 FE 0" % (1000 * UNIT), "R 0 %d 20 1 3 0 5" % (100000 * UNIT), "W 0 %d 7 80 5" % (100025 * UNIT),
           "R 0 %d 22 1 0 0 5" % (200000 * UNIT)]


def run(ref, product, *extra):
    with tempfile.TemporaryDirectory() as tmp:
        r, p = pathlib.Path(tmp, "ref"), pathlib.Path(tmp, "prod")
        r.write_text("\n".join(ref) + "\n")
        p.write_text("\n".join(product) + "\n")
        return subprocess.run([sys.executable, str(TOOL), "--reference", str(r), "--trace", str(p), *extra], capture_output=True, text=True).returncode


check(run(REF, PRODUCT) == 0, "identical operation sequences pass")
check(run(REF + [ref_line("W", 7, 0x81, 900000)], PRODUCT) == 0, "a product trace that is a prefix of a longer reference passes")
check(run(REF, PRODUCT + ["W 0 %d 7 81 5" % (300000 * UNIT)]) != 0, "an extra product operation beyond the reference fails")
check(run(REF + [ref_line("W", 7, 0x81, 900000)], PRODUCT[:-1], "--expect-product-operations", "5") != 0, "a truncated product trace fails against the declared length")
check(run(REF, PRODUCT[:3] + ["W 0 %d 7 81 5" % (100025 * UNIT)] + PRODUCT[4:]) != 0, "a diverging written value fails")
if failures:
    sys.exit(1)
print("sega_cd_pcm_compare_test: OK")
