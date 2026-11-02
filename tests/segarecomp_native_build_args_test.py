#!/usr/bin/env python3
"""SEG-124 (ADR 0267): `segarecomp build --patch-package/--patch-identity` takes a REFERENCE to a package (never source text, compiler flags or capability strings): the two options travel
together, the identity is a lower-case SHA-256, the build needs the viewer (a presentation provider), and Master System is refused; everything else is a usage error (exit 2) before any
work. A valid reference is accepted by the parser (the build then fails on the missing ROM with exit 1: nothing is compiled). The old per-capability options no longer exist.
usage: segarecomp_native_build_args_test.py <segarecomp-cli>"""
import subprocess
import sys
import tempfile
from pathlib import Path

CLI = sys.argv[1]
ID = "ab" * 32
failures = []
tmp = Path(tempfile.mkdtemp(prefix="native-build-args-"))


def run(extra, viewer=True):
    cmd = [CLI, "build", "--rom", str(tmp / "missing.bin"), "--output", str(tmp / "out"), "--cc", "cc", "--runtime-dir", str(tmp / "rt")]
    if viewer:
        cmd += ["--sdl3-include", str(tmp / "inc"), "--sdl3-lib", str(tmp / "lib")]
    return subprocess.run(cmd + extra, text=True, capture_output=True, timeout=60).returncode


def expect(code, extra, what, **kw):
    got = run(extra, **kw)
    if got != code:
        failures.append(f"{what}: exit {got}, expected {code}")


ref = ["--patch-package", str(tmp / "p.segapatch"), "--patch-identity", ID]
expect(1, ref, "a valid reference is accepted (then the missing ROM fails the build)")
expect(1, [], "no patch option at all is the unchanged build")
expect(2, ["--patch-package", str(tmp / "p.segapatch")], "a package without the approved identity")
expect(2, ["--patch-identity", ID], "an identity without a package")
for bad in ("", "AB" * 32, "ab" * 31, "zz" * 32, "ab" * 33):
    expect(2, ["--patch-package", str(tmp / "p.segapatch"), "--patch-identity", bad], f"malformed identity {bad!r}")
expect(2, ref + ["--patch-package", str(tmp / "q.segapatch")], "a repeated package option")
expect(2, ref + ["--patch-identity", ID], "a repeated identity option")
expect(2, ref, "a headless (no viewer) build cannot link a presentation provider", viewer=False)
expect(2, ref + ["--platform", "master-system"], "another platform")
for old in ("--native-capability", "--native-implementation", "--native-components", "--native-source-sha256", "--native-descriptor-sha256"):
    expect(2, [old, "x"], f"the retired option {old}")
if failures:
    print("\n".join("FAIL: " + f for f in failures))
    sys.exit(1)
print("segarecomp_native_build_args_test: ok")
