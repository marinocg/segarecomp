#!/usr/bin/env python3
"""CTest wrapper: runs tools/architecture_baseline.py in verify mode against
the committed manifest (tests/fixtures/architecture-baseline-manifest.json).

This is SEG-014-T001's regression anchor. Later SEG-014 structural-move tasks
must run this same verifier rather than relying only on green tests after
large file moves.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main() -> int:
    if len(sys.argv) != 3:
        return 2
    segarecomp, compiler = (pathlib.Path(value).resolve() for value in sys.argv[1:])
    tool = ROOT / "tools" / "architecture_baseline.py"
    result = subprocess.run(
        [sys.executable, str(tool), "--segarecomp", str(segarecomp), "--cc", str(compiler), "--mode", "verify"],
        text=True, capture_output=True, cwd=ROOT)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        return 1
    sys.stdout.write(result.stdout)
    print("architecture baseline verifier: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
