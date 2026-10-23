#!/usr/bin/env python3
"""SEG-071-T002 (ADR 0177): the timing conventions of the BIOS-image profile.

  * tests/tools/sega_cd_timing_table.c: the line-locked sub cycle and the 75 Hz drive grid (against hand-computed tick lines), the main bus
    refresh delay and the long-instruction rule, the Z80-window latency, the VDP FIFO and the VRAM-fill busy bit, the interrupt latency after
    IE0, the video phase and the autovector exception cycles, each against values derived by hand; the synthetic profile keeps its exact
    sub clock, exact 75 Hz grid and flat interrupt cost.

usage: sega_cd_timing_convention_test.py <segarecomp> <c-compiler> <source-root>
"""
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402


def main():
    with tempfile.TemporaryDirectory(prefix="scd-timing-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(SEGARECOMP, CC, tmp)
        table = tmp / ("timing_table" + common.EXE)
        subprocess.run([CC, *common.CFLAGS, "-I", str(common.SCD_RUNTIME), "-I", str(common.GENESIS_RUNTIME),
                        str(ROOT / "tests" / "tools" / "sega_cd_timing_table.c"), *[str(o) for o in builder.runtime_objects()],
                        "-o", str(table)], check=True)
        out = subprocess.run([str(table)], capture_output=True, text=True, timeout=120)
        print(out.stdout)
        rows = [line for line in out.stdout.splitlines() if line.startswith(("ok", "FAIL"))]
        ok = out.returncode == 0 and len(rows) >= 40 and not any(r.startswith("FAIL") for r in rows)
        print(("ok    " if ok else "FAIL  ") + "timing convention table: %d rows" % len(rows))
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
