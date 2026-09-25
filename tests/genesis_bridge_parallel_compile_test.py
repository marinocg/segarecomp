#!/usr/bin/env python3
"""SEG-022-T004: bounded parallel generated-TU compilation: job-ordered objects, bounded concurrency,
clean abort with the failing unit's diagnostics. Uses a project-authored fake compiler; no ROM."""
import os
import pathlib
import stat
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import genesis_startup_bridge as bridge  # noqa: E402


def require(cond, msg):
    if not cond:
        raise RuntimeError(msg)


FAKE = """#!/bin/sh
# args: ... -c -o OUT SRC
while [ $# -gt 0 ]; do case "$1" in -o) out="$2"; shift;; *) src="$1";; esac; shift; done
d=$(dirname "$out")
mkdir -p "$d/run"; echo $$ > "$d/run/$$"
n=$(ls "$d/run" | wc -l); echo $n >> "$d/peak"
sleep 0.2
case "$src" in *bad*) echo "error in $src" >&2; rm -f "$d/run/$$"; exit 1;; esac
cat "$src" > "$out"; rm -f "$d/run/$$"
"""


def main():
    with tempfile.TemporaryDirectory() as t:
        t = pathlib.Path(t)
        cc = t / "fakecc"
        cc.write_text(FAKE)
        cc.chmod(cc.stat().st_mode | stat.S_IEXEC)
        srcs = []
        for i in range(8):
            p = t / f"u{i}.c"
            p.write_text(f"unit{i}\n")
            srcs.append(p)
        for jobs in (1, 3):
            bridge._compile_jobs_override = jobs
            out = t / f"o{jobs}"
            out.mkdir()
            objects, failure = bridge.compile_objects([([str(cc)], s) for s in srcs], t, out)
            require(failure is None, failure)
            require([pathlib.Path(o).read_text() for o in objects] == [f"unit{i}\n" for i in range(8)], "order")
            peak = max(int(x) for x in (out / "peak").read_text().split())
            require(peak <= jobs, f"peak {peak} > {jobs}")
            if jobs == 3:
                require(peak > 1, "no concurrency")
        bad = t / "bad.c"
        bad.write_text("x")
        out = t / "of"
        out.mkdir()
        bridge._compile_jobs_override = 3
        objects, failure = bridge.compile_objects([([str(cc)], s) for s in srcs[:2] + [bad] + srcs[2:]], t, out)
        require(objects == [] and "error in" in failure and "bad.c" in failure, "failure diagnostics")
        os.environ["SEGARECOMP_COMPILE_JOBS"] = "2"
        bridge._compile_jobs_override = None
        require(bridge.default_compile_jobs() == 2, "env")
    print("ok")


main()
