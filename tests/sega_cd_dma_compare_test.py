#!/usr/bin/env python3
"""SEG-103-T004: regression for tools/sega_cd_dma_compare.py on synthetic logs - matching transfers pass; a different source, a different length and
an unequal arm/end count fail; durations are reported, not required.  usage: sega_cd_dma_compare_test.py <source-root>"""
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[1]).resolve()
TOOL = ROOT / "tools" / "sega_cd_dma_compare.py"
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def product(rows):
    out = []
    for frame, src, length, dur in rows:
        out += ["%d dma_src %X" % (frame, src), "%d dma_len %X" % (frame, length), "%d dma_dur %X" % (frame, dur)]
    return "\n".join(out) + "\n"


def reference(rows, ends=None):
    arm = ["%d %02X %04X %d 21 0100 %d" % (f, r23, src, length, cyc) for f, r23, src, length, cyc, _ in rows]
    end = ["E %d %d" % (f, cyc + dur) for f, _, _, _, cyc, dur in rows]
    lines = []
    for a, e in zip(arm, end):
        lines += [a, e]
    return "\n".join(lines) + "\n"


def run(prod, ref):
    with tempfile.TemporaryDirectory() as t:
        t = pathlib.Path(t)
        (t / "p.txt").write_text(prod)
        (t / "r.dma").write_text(ref)
        done = subprocess.run([sys.executable, str(TOOL), str(t / "p.txt"), str(t / "r.dma")], capture_output=True, text=True)
        return done.returncode, done.stdout, done.stderr


REF_ROWS = [(5, 0x10, 0x0011, 2399, 1000, 8000), (6, 0x10, 0x0971, 2399, 2000, 7000), (7, 0x7F, 0x8000, 100, 3000, 120)]
PROD = [(1, 0x100011, 2399, 8000), (2, 0x100971, 2399, 7000), (3, 0x7F8000, 100, 121)]
code, out, _ = run(product(PROD), reference(REF_ROWS))
data = json.loads(out)
check(code == 0 and data["all"]["compared"] == 3 and data["all"]["source_equal"] == 3 and data["all"]["length_equal_after_delay"] == 3, "matching transfers pass")
check(data["page"]["compared"] == 2 and data["page"]["duration_equal"] == 2 and data["all"]["duration_equal"] == 2, "durations counted: the 1-clock difference is reported, not hidden")
bad = list(PROD)
bad[1] = (2, 0x100972, 2399, 7000)
code, out, _ = run(product(bad), reference(REF_ROWS))
check(code == 1 and json.loads(out)["all"]["first_mismatch_index"] == 1, "a different source fails at its index")
bad = list(PROD)
bad[0] = (1, 0x100011, 2398, 8000)
code, out, _ = run(product(bad), reference(REF_ROWS))
check(code == 1, "a different length fails")
code, out, err = run(product(PROD), reference(REF_ROWS) + "E 9 9\n")
check(code != 0 and "differ" in err, "unequal arm/end counts in the reference are refused")
print("%d failures" % len(failures))
sys.exit(1 if failures else 0)
