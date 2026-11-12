#!/usr/bin/env python3
"""Local demonstration (ADR 0291, docs/testing/genesis-native-gems-validation.md section 8): save a state while the native GEMS driver plays a real game, restore it, and
require the continuation to be identical to the uninterrupted one. Needs a legally held image; prints counts and verdicts only.

  genesis_state_demo.py --rom <image> --work DIR [--segarecomp PATH] [--cc cc] [--pre N] [--span N]
"""
import argparse
import glob
import hashlib
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rom", required=True)
    p.add_argument("--work", required=True)
    p.add_argument("--segarecomp", default=str(ROOT / "build" / "dev" / "apps" / "segarecomp" / "segarecomp"))
    p.add_argument("--cc", default="cc")
    p.add_argument("--pre", default="6000000", help="guest dispatches before the save")
    p.add_argument("--span", default="3000000", help="guest dispatches compared after it")
    a = p.parse_args()
    work = pathlib.Path(a.work)
    out = work / "out"
    if not (out / "obj").is_dir():
        work.mkdir(parents=True, exist_ok=True)
        subprocess.run([a.segarecomp, "build", "--rom", a.rom, "--output", str(out), "--cc", a.cc, "--runtime-dir", str(ROOT / "platforms" / "genesis"), "--keep-work", "1"],
                       check=True, stdout=subprocess.DEVNULL)
    rt = ROOT / "platforms" / "genesis" / "runtime"
    incs = ["-I" + str(d) for d in [rt, ROOT / "platforms/genesis/viewer", ROOT / "libs/viewer/include", ROOT / "libs/codegen/c11/include",
                                    ROOT / "libs/device/sega/psg/include", ROOT / "libs/device/sega/ym2612/include"]]
    objects = []
    for tool, src, extra in (("demo-hook", ROOT / "tools" / "genesis_state_demo_hook.c", []), ("demo-snapshot", rt / "genesis_snapshot.c", ["-DSEGARECOMP_GENESIS_SOUND"])):
        obj = work / (tool + ".o")
        subprocess.run([a.cc, "-std=c11", "-O1", *incs, *extra, "-c", str(src), "-o", str(obj)], check=True)
        objects.append(str(obj))
    # obj/ also holds the objects of earlier materialization passes: compile the FINAL Z80 units (generated-z80) here and leave every obj/z80-*.o out
    z80 = out / "generated-z80"
    for unit in (z80 / "genesis_z80.units").read_text().split():
        obj = work / ("demo-z80-" + pathlib.Path(unit).stem + ".o")
        subprocess.run([a.cc, "-std=c11", "-O1", "-w", *incs, "-I" + str(z80), "-c", str(z80 / unit), "-o", str(obj)], check=True)
        objects.append(str(obj))
    keep = [o for o in sorted(glob.glob(str(out / "obj" / "*.o"))) if not pathlib.Path(o).name.startswith(("hook-", "z80-"))]
    exe = work / "state_demo"
    subprocess.run([a.cc, "-o", str(exe), *keep, *objects, "-lm", "-lstdc++"], check=True)
    sha = hashlib.sha256(pathlib.Path(a.rom).read_bytes()).hexdigest()
    env = {"SEGARECOMP_DEMO_PRE": a.pre, "SEGARECOMP_DEMO_SPAN": a.span, "SEGARECOMP_DEMO_SOURCE_SHA": sha, "PATH": "/usr/bin:/bin"}
    done = subprocess.run([str(exe)], env=env, text=True, capture_output=True, timeout=3600)
    print("\n".join(l for l in done.stdout.splitlines() if l.startswith("STATE_DEMO")))
    return 0 if "continuation_identical=1 third_identical=1" in done.stdout else 1


if __name__ == "__main__":
    sys.exit(main())
