#!/usr/bin/env python3
"""Hermetic test of packaging/trim-zig.sh (SEG-032-T007, ADR 0074).

A fake Zig lib tree is trimmed for each host family. libc++, libc++abi and libunwind must survive WHOLE, sources included: a game with a mod links C++ with exceptions and RTTI,
and Zig builds those libraries from their sources at link time (SEG-146: "sub-compilation of libunwind failed ... FileNotFound" when the trim removed them). Documentation is still removed.

usage: packaging_trim_zig_test.py <project-source-dir>
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile


def main():
    root = pathlib.Path(sys.argv[1])
    script = root / "packaging" / "trim-zig.sh"
    if shutil.which("bash") is None:
        print("SKIP: no bash")
        return 0
    if sys.platform == "win32":
        # On a Windows host `bash` may resolve to the WSL launcher and `find` to find.exe, so a pass or fail here would not
        # describe the script. The release workflow runs trim-zig.sh under Git-bash on the Windows package-smoke job.
        print("SKIP: trim-zig.sh is exercised by the Windows package smoke job (Git-bash), not by a bare Windows PATH")
        return 0
    failures = []
    for family in ("linux", "macos", "windows"):
        with tempfile.TemporaryDirectory() as tmp:
            zig = pathlib.Path(tmp)
            files = ["std/std.zig", "libcxx/include/cassert", "libcxx/include/__config", "libcxx/src/iostream.cpp",
                     "libcxx/test/t.cpp", "libcxx/modules/std.cppm", "libcxxabi/include/cxxabi.h", "libcxxabi/src/abort.cpp",
                     "libunwind/src/u.c", "docs/x.md", "libc/include/generic-glibc/stdio.h"]
            for rel in files:
                path = zig / "lib" / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("x\n")
            run = subprocess.run(["bash", str(script), str(zig), family], capture_output=True, text=True, timeout=120)
            if run.returncode != 0:
                failures.append("%s: exit %d: %s" % (family, run.returncode, run.stderr))
                continue
            lib = zig / "lib"
            for kept in ("std/std.zig", "libcxx/include/cassert", "libcxx/include/__config", "libcxx/src/iostream.cpp", "libcxxabi/include/cxxabi.h", "libcxxabi/src/abort.cpp", "libunwind/src/u.c"):
                if not (lib / kept).is_file():
                    failures.append("%s: %s must survive" % (family, kept))
            for gone in ("docs",):
                if (lib / gone).exists():
                    failures.append("%s: %s must be removed" % (family, gone))
    for f in failures:
        print("FAIL:", f)
    print("packaging_trim_zig_test:", "FAIL" if failures else "PASS")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
