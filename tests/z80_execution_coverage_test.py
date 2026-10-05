#!/usr/bin/env python3
"""SEG-031 (ADR 0080): the Z80 execution-PC observer on the project-authored Master System `machine_e2e` fixture (hermetic).

usage: z80_execution_coverage_test.py <segarecomp-cli> <cc> <product-root>

1. A measurement build (`--cc-arg -DSEGARECOMP_Z80_EXECUTION_COVERAGE`) of the fixture runs a fixed frame count twice with
   `--execution-coverage` and once without it: the machine state digest is identical in all three runs (zero semantic effect), the
   private coverage list and the sanitized summary are identical in both observed runs (deterministic), every observed PC has a
   code-image identity, and the list is canonical ((identity, PC) strictly ascending).
2. The default build (no macro) of the same fixture rejects `--execution-coverage` (usage, exit 64) and reaches the same state digest:
   ordinary programs never call the observer.
"""
import json
import pathlib
import re
import subprocess
import sys
import tempfile

CLI, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sms_e2e_common as c  # noqa: E402
from sms_e2e_common import builder  # noqa: E402

FRAMES = "30"


def build(rom, out, *extra):
    result = subprocess.run([CLI, "build", "--rom", str(rom), "--output", str(out), "--cc", CC, "--optimize", "0", "--mapper", "sega",
                             "--runtime-dir", str(ROOT / "platforms" / "master-system"), "--cc-arg", "-Wno-misleading-indentation",
                             *extra], text=True, capture_output=True, timeout=900)
    assert result.returncode == 0, (result.stdout + result.stderr)[-1500:]
    return out / ("game.exe" if sys.platform == "win32" else "game")


def run(exe, *extra):
    result = subprocess.run([str(exe), "--frames", FRAMES, *extra], text=True, capture_output=True, timeout=600)
    match = re.search(r"^stop (\S+) cycles \d+ frames \d+ digest ([0-9a-f]{64})", result.stdout, re.M)
    return result, match


def main():
    with tempfile.TemporaryDirectory(prefix="z80-coverage-") as directory:
        tmp = pathlib.Path(directory)
        builder.main(["--out", str(tmp / "roms"), "--fixture", c.FIXTURE])
        rom = tmp / "roms" / (c.FIXTURE + ".sms")
        measured = build(rom, tmp / "measured", "--cc-arg", "-DSEGARECOMP_Z80_EXECUTION_COVERAGE")
        digests, lists, summaries = [], [], []
        for index, observed in enumerate((True, True, False)):
            cov = tmp / f"cov{index}"
            cov.mkdir()
            result, match = run(measured, *(["--execution-coverage", str(cov)] if observed else []))
            assert match is not None and "z80_outcome" not in result.stdout, (index, result.stdout, result.stderr)
            digests.append(match.group(2))
            if observed:
                line = next(l for l in result.stderr.splitlines() if l.startswith("Z80_COVERAGE_SUMMARY "))
                summaries.append(json.loads(line.split(" ", 1)[1]))
                lists.append((cov / "z80-coverage.txt").read_text())
        assert len(set(digests)) == 1, f"the observer changed the machine state: {digests}"
        assert lists[0] == lists[1] and summaries[0] == summaries[1], "the observed runs differ (nondeterministic observer)"
        summary = summaries[0]
        assert summary["images"] >= 1 and summary["distinct_pcs"] > 0 and summary["retirements"] >= summary["distinct_pcs"], summary
        assert summary["unknown_identity"] == 0 and summary["identity_overflow"] == 0, summary
        entries = [tuple(int(field, 16) for field in line.split()) for line in lists[0].splitlines()]
        assert entries == sorted(set(entries)) and len(entries) == summary["distinct_pcs"], "the private list is not canonical"
        assert all(0 <= pc <= 0xFFFF for _, pc in entries)
        assert sorted(summary) == ["coverage_digest", "distinct_pcs", "identity_overflow", "images", "retirements",
                                   "unknown_identity"], "the summary carries only counts and the digest"

        default = build(rom, tmp / "default")
        rejected = subprocess.run([str(default), "--frames", FRAMES, "--execution-coverage", str(tmp)], text=True, capture_output=True,
                                  timeout=600)
        assert rejected.returncode == 64, ("the default program accepted --execution-coverage", rejected.returncode)
        _, match = run(default)
        assert match is not None and match.group(2) == digests[0], "the default program diverges from the measurement build"
    print("z80_execution_coverage_test: OK", json.dumps({"images": summary["images"], "distinct_pcs": summary["distinct_pcs"],
                                                         "retirements": summary["retirements"]}))


if __name__ == "__main__":
    main()
