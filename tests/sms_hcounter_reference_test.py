#!/usr/bin/env python3
"""SEG-009-T006 (U3): the SMS H counter value table against both pinned machine references (ADR 0062).

usage: sms_hcounter_reference_test.py <cc> <product-root>

The platform's `sms_vdp_hcounter_value` (228 T offsets in a line) is dumped by a tiny generated C program and compared with
the two references' own tables, parsed from their pinned sources (never vendored):
  * GPGX (`cycle2hc32`, indexed by master cycles, T x 15): identical at every T offset (the platform origin);
  * Gearsystem (`kVdpHCounter`, per T): the same 171-count sequence shifted by exactly 20 T, with each value equal or one
    count (2 pixels) ahead: the U2 in-line origin tolerance, classified and bounded, not silently accepted.
Skips cleanly (exit 0) unless SEGARECOMP_SMS_ORACLE_CHECKOUT holds the pinned checkouts.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

CC = sys.argv[1]
ROOT = pathlib.Path(sys.argv[2]).resolve()
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "sms_oracle"))
import pins  # noqa: E402

NAMES = ["drhelius_Gearsystem", "ekeeke_Genesis-Plus-GX"]
RUNTIME = ROOT / "platforms" / "master-system" / "runtime"
DUMP = """#include <stdio.h>
#include "sms_vdp.h"
int main(void) { unsigned t; for (t = 0; t < 228u; ++t) printf("%u\\n", (unsigned)sms_vdp_hcounter_value(t)); return 0; }
"""


def table(text, name, length):
    body = re.search(name + r"\[[^\]]*\]\s*=\s*\{(.*?)\};", text, re.S).group(1)
    values = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    assert len(values) == length, (name, len(values))
    return values


def main():
    root = pins.checkout(NAMES)
    if root is None:
        print("skipped: " + pins.skip_reason(NAMES))
        return 0
    gs = table((root / NAMES[0] / "src" / "Video.h").read_text(), "kVdpHCounter", 228)
    gp = table((root / NAMES[1] / "core" / "hvc.h").read_text(), "cycle2hc32", 3420)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        (tmp / "dump.c").write_text(DUMP)
        build = subprocess.run([CC, "-std=c11", "-I", str(RUNTIME), "-I", str(ROOT / "libs/codegen/c11/include"), "-I",
                                str(ROOT / "libs/device/sega/psg/include"), str(tmp / "dump.c"), str(RUNTIME / "sms_vdp.c"),
                                str(RUNTIME / "sms_memory.c"), str(RUNTIME / "sms_sha256.c"),
                                "-o", str(tmp / "dump")], capture_output=True, text=True)
        if build.returncode != 0:
            print("FAIL: dump build: " + build.stderr[:800])
            return 1
        out = subprocess.run([str(tmp / "dump")], capture_output=True, text=True, timeout=60)
    device = [int(x) for x in out.stdout.split()]
    failures = []
    if device != [gp[t * 15] for t in range(228)]:
        failures.append("device table differs from Genesis Plus GX cycle2hc32")
    sequence = list(range(0x94)) + list(range(0xE9, 0x100))
    index = {v: i for i, v in enumerate(sequence)}
    deltas = {(index[gs[t]] - index[device[(t + 20) % 228]] + 85) % 171 - 85 for t in range(228)}
    if not deltas <= {0, 1}:
        failures.append("Gearsystem table is not the device sequence shifted by 20 T within one count: %s" % sorted(deltas))
    for f in failures:
        print("FAIL:", f)
    if not failures:
        print("sms_hcounter_reference_test: ok")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
