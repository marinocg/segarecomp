#!/usr/bin/env python3
"""SEG-104 (ADR 0247): the CD-DA comparison tool on project-authored data - exact equality, a one-sample difference, an unequal tail and the
interval statistics. Usage: sega_cd_cdda_compare_test.py <project root>"""
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile

root = sys.argv[1]
tool = os.path.join(root, "tools", "sega_cd_cdda_compare.py")


def pcm(frames):
    return b"".join(struct.pack("<hh", l, r) for l, r in frames)


def run(ours, ref, extra_log=None):
    with tempfile.TemporaryDirectory() as d:
        paths = {k: os.path.join(d, k) for k in ("ours", "ref", "report", "reflog", "prodlog")}
        open(paths["ours"], "wb").write(ours)
        open(paths["ref"], "wb").write(ref)
        json.dump({"cdda": {"sectors": 2, "frames": len(ours) // 4, "first_frame": 10, "last_frame": 12,
                            "pcm_sha256": hashlib.sha256(ours).hexdigest()}}, open(paths["report"], "w"))
        open(paths["reflog"], "w").write("11 736 5 100 30 0\n12 737 5 101 30 736\n20 736 5 200 30 1473\n")
        open(paths["prodlog"], "w").write("start frame=10 tick=1 lba=100 track=2\nstop frame=13 tick=2 next_lba=102\nstart frame=19 tick=3 lba=200 track=2\n")
        p = subprocess.run([sys.executable, tool, "--pcm", paths["ours"], "--reference-pcm", paths["ref"], "--report", paths["report"],
                            "--reference-log", paths["reflog"], "--product-log", paths["prodlog"]], capture_output=True, text=True)
        return p.returncode, json.loads(p.stdout)


a = [(i - 600, 600 - i) for i in range(1200)]
rc, out = run(pcm(a), pcm(a))
assert rc == 0 and out["exact"] and out["common_prefix_frames"] == 1200, out
rc, out = run(pcm(a), pcm(a + [(1, 2)] * 300))
assert rc == 0 and out["exact"] and out["length_difference_frames"] == 300, out
b = list(a)
b[700] = (b[700][0] ^ 1, b[700][1])
rc, out = run(pcm(a), pcm(b))
assert rc == 1 and not out["exact"] and out["common_prefix_frames"] == 700, out
c = [(r, l) for l, r in a]  # channels swapped
rc, out = run(pcm(a), pcm(c))
assert rc == 1 and out["common_prefix_frames"] < 5, out
rc, out = run(pcm(a), pcm(a))
assert out["reference_runs"] == 2 and out["start_difference_frames"] == 1 and out["run_start_differences_frames"] == [1], out
assert "lba" not in json.dumps(out) and "track" not in json.dumps(out)
print("sega_cd_cdda_compare_test: all checks passed")
