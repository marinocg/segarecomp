#!/usr/bin/env python3
"""SEG-065-T003 (ADR 0146, ADR 0147): frame capture at a vertical-blank onset.

  * tests/tools/sega_cd_frame_capture_table.c: a project-authored VDP state driven through the machine's device events; the canonical
    frame encoding, per-line sampling (a mid-frame CRAM change), the VDP state file, the refusals (shadow/highlight, palette select)
    and an incomplete frame, against hand-derived values;
  * the program options: a capture request outside the BIOS-image profile, a capture onset without a directory (or the reverse) and
    onset < 2 are usage errors (exit 2).

usage: sega_cd_frame_capture_test.py <segarecomp> <c-compiler> <source-root>
"""
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def main():
    with tempfile.TemporaryDirectory(prefix="scd-capture-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(SEGARECOMP, CC, tmp)
        table = tmp / ("frame_capture_table" + common.EXE)
        subprocess.run([CC, *common.CFLAGS, "-I", str(common.SCD_RUNTIME), "-I", str(common.GENESIS_RUNTIME),
                        str(ROOT / "tests" / "tools" / "sega_cd_frame_capture_table.c"), *[str(o) for o in builder.runtime_objects()],
                        "-o", str(table)], check=True)
        scratch = tmp / "captures"
        scratch.mkdir()
        out = subprocess.run([str(table), str(scratch)], capture_output=True, text=True, timeout=120)
        print(out.stdout)
        rows = [line for line in out.stdout.splitlines() if line.startswith(("ok", "FAIL"))]
        check(out.returncode == 0 and len(rows) >= 9 and not any(r.startswith("FAIL") for r in rows), "frame capture table: %d rows" % len(rows))

        disc, exe, _, _ = builder.program("subside", "subside")
        for label, extra in (("synthetic profile", ["--capture-onset", "5", "--capture-dir", str(scratch)]),
                             ("onset without a directory", ["--capture-onset", "5"]),
                             ("directory without an onset", ["--capture-dir", str(scratch)]),
                             ("onset 1", ["--capture-onset", "1", "--capture-dir", str(scratch)])):
            code, _ = common.run(exe, disc, 6000000, *extra)
            check(code == 2, "usage error (exit 2): %s, got %d" % (label, code))
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
