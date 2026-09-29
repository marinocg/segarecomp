#!/usr/bin/env python3
"""SEG-008-T001: compile and run the Z80 oracle adapter smoke against the pinned local checkout.
Skips cleanly (exit 0) unless SEGARECOMP_Z80_ORACLE_CHECKOUT names a directory holding the pinned
redcode_Z80 and redcode_Zeta clones; a wrong or dirty pin is a hard failure (Musashi convention)."""
import os
import pathlib
import subprocess
import sys
import tempfile

CHECKOUT_ENV = "SEGARECOMP_Z80_ORACLE_CHECKOUT"
PINS = {
    "redcode_Z80": "6bb4166317108b8d1a4b5934df15761089bdea9e",
    "redcode_Zeta": "93ba5ab967eef00f074d21bb760fe9dc48afd2d3",
}
DEFINES = ["-DZ80_STATIC", "-DZ80_WITH_EXECUTE", "-DZ80_WITH_Q", "-DZ80_WITH_FULL_IM0", "-DZ80_WITH_SPECIAL_RESET",
           "-DZ80_WITH_UNOFFICIAL_RETI", "-DZ80_WITH_ZILOG_NMOS_LD_A_IR_BUG"]
SOURCE = pathlib.Path(__file__).resolve().parent / "z80_oracle" / "adapter_smoke.c"


def checkout():
    configured = os.environ.get(CHECKOUT_ENV)
    if not configured:
        return None
    root = pathlib.Path(configured)
    if not (root / "redcode_Z80" / "sources" / "Z80.c").is_file():
        return None
    for name, pin in PINS.items():
        head = subprocess.run(["git", "-C", str(root / name), "rev-parse", "HEAD"], text=True, capture_output=True)
        if head.returncode != 0 or head.stdout.strip() != pin:
            raise AssertionError("%s checkout is not the pinned revision %s" % (name, pin))
        dirty = subprocess.run(["git", "-C", str(root / name), "diff", "--quiet", "HEAD", "--"])
        assert dirty.returncode == 0, "%s pinned checkout has local modifications" % name
    return root


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else "cc"
    root = checkout()
    if root is None:
        print("skipped: pinned Z80 oracle checkout unavailable (%s unset)" % CHECKOUT_ENV)
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = pathlib.Path(tmp) / "adapter_smoke"
        cmd = [compiler, "-std=c11", "-Wall", "-Wextra", *DEFINES,
               "-I%s" % (root / "redcode_Z80" / "API"), "-I%s" % (root / "redcode_Zeta" / "API"),
               str(SOURCE), str(root / "redcode_Z80" / "sources" / "Z80.c"), "-o", str(exe)]
        subprocess.run(cmd, check=True)
        run = subprocess.run([str(exe)], text=True, capture_output=True, timeout=30)
        sys.stdout.write(run.stdout.splitlines()[-1] + "\n" if run.stdout else "")
        return run.returncode


if __name__ == "__main__":
    sys.exit(main())
