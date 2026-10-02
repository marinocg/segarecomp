#!/usr/bin/env python3
"""Hermetic test of packaging/trim-zig.sh (SEG-032-T007, ADR 0074).

A fake Zig lib tree is trimmed for each host family. The libc++/libc++abi HEADERS must survive (the vendored YM2612 core is
compiled with `zig c++`); everything else under lib/libcxx and lib/libcxxabi must be removed, as must libunwind.

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
            for kept in ("std/std.zig", "libcxx/include/cassert", "libcxx/include/__config", "libcxxabi/include/cxxabi.h"):
                if not (lib / kept).is_file():
                    failures.append("%s: %s must survive" % (family, kept))
            for gone in ("libcxx/src", "libcxx/test", "libcxx/modules", "libcxxabi/src", "libunwind", "docs"):
                if (lib / gone).exists():
                    failures.append("%s: %s must be removed" % (family, gone))
            for d in ("libcxx", "libcxxabi"):
                extra = sorted(p.name for p in (lib / d).iterdir() if p.name != "include")
                if extra:
                    failures.append("%s: %s has extra entries %s" % (family, d, extra))
    for f in failures:
        print("FAIL:", f)
    print("packaging_trim_zig_test:", "FAIL" if failures else "PASS")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
