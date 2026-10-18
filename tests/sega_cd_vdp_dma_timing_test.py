#!/usr/bin/env python3
"""SEG-065-T002 (ADR 0147): the 68K->VDP DMA duration and the main processor's wait in the BIOS-image profile.

  * tests/tools/sega_cd_vdp_dma_table.c: control-port writes through the runtime's bus ABI at chosen frame positions against
    durations derived by hand from the documented slot model (blank and active windows, H32/H40, VRAM/CRAM/VSRAM, display off, a
    mid-line start, a frame-crossing transfer, length 0), the retire rule (the arming instruction completes after the transfer, once)
    and a VRAM fill (no wait).

usage: sega_cd_vdp_dma_timing_test.py <segarecomp> <c-compiler> <source-root>
"""
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402


def main():
    with tempfile.TemporaryDirectory(prefix="scd-vdp-dma-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(SEGARECOMP, CC, tmp)
        table = tmp / ("vdp_dma_table" + common.EXE)
        subprocess.run([CC, *common.CFLAGS, "-I", str(common.SCD_RUNTIME), "-I", str(common.GENESIS_RUNTIME),
                        str(ROOT / "tests" / "tools" / "sega_cd_vdp_dma_table.c"), *[str(o) for o in builder.runtime_objects()],
                        "-o", str(table)], check=True)
        out = subprocess.run([str(table)], capture_output=True, text=True, timeout=120)
        print(out.stdout)
        rows = [line for line in out.stdout.splitlines() if line.startswith(("ok", "FAIL"))]
        ok = out.returncode == 0 and len(rows) >= 12 and not any(r.startswith("FAIL") for r in rows)
        print(("ok    " if ok else "FAIL  ") + "VDP DMA timing table: %d rows" % len(rows))
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
