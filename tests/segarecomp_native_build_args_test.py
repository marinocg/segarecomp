#!/usr/bin/env python3
"""SEG-124 (ADR 0267): `segarecomp build --native-*` accepts exactly the audited capability selection and refuses everything else as a usage error (exit 2) before any work:
unknown capability, another implementation version, components the capability lacks or none, a source image other than the one it is verified for, a missing or malformed descriptor digest,
partial option sets, a headless (no viewer) build and a fixture capability (never buildable, even with the test switch). A valid selection is accepted by the parser (the build then
fails on the missing ROM with exit 1: nothing is compiled). usage: segarecomp_native_build_args_test.py <segarecomp-cli>"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

CLI = sys.argv[1]
CAP = "genesis.sonic1.native-widescreen.v1"
SRC = "46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6"
DESC = "9608f439f2538b3fb4afaa7f9b567be1de3b0612e5522a11e2e8e307e536fa62"
failures = []
tmp = Path(tempfile.mkdtemp(prefix="native-build-args-"))


def run(extra, viewer=True, env_extra=None):
    cmd = [CLI, "build", "--rom", str(tmp / "missing.bin"), "--output", str(tmp / "out"), "--cc", "cc", "--runtime-dir", str(tmp / "rt")]
    if viewer:
        cmd += ["--sdl3-include", str(tmp / "inc"), "--sdl3-lib", str(tmp / "lib")]
    cmd += extra
    env = dict(os.environ)
    env.update(env_extra or {})
    return subprocess.run(cmd, text=True, capture_output=True, env=env, timeout=60).returncode


def native(cap=CAP, impl="1", comps="3", src=SRC, desc=DESC):
    out = ["--native-capability", cap, "--native-implementation", impl, "--native-components", comps, "--native-source-sha256", src, "--native-descriptor-sha256", desc]
    return out


def expect(code, extra, what, **kw):
    got = run(extra, **kw)
    if got != code:
        failures.append(f"{what}: exit {got}, expected {code}")


expect(1, native(), "a valid selection is accepted (then the missing ROM fails the build)")
expect(1, native(comps="1"), "HUD only is accepted")
expect(1, native(comps="2"), "scenery only is accepted")
expect(1, [], "no native option at all is the unchanged build")
expect(2, native(cap="genesis.other.native-widescreen.v1"), "unknown capability")
expect(2, native(cap=""), "empty capability id with other options")
expect(2, native(impl="2"), "another implementation version")
expect(2, native(impl="0"), "implementation zero")
expect(2, native(comps="0"), "no component")
expect(2, native(comps="4"), "unknown component bit")
expect(2, native(comps="7"), "a component the capability does not have")
expect(2, native(src="11" * 32), "a source image the capability is not verified for")
expect(2, native(src=""), "missing source digest")
expect(2, native(desc=""), "missing descriptor digest")
expect(2, native(desc="Z" * 64), "malformed descriptor digest")
expect(2, native(desc=DESC[:-1]), "short descriptor digest")
expect(2, native(desc=DESC.upper()), "upper-case descriptor digest")
expect(2, ["--native-implementation", "1"], "options without a capability")
expect(2, ["--native-descriptor-sha256", DESC], "a descriptor digest without a capability")
expect(2, native(), "a headless (no viewer) build cannot link a presentation provider", viewer=False)
expect(2, native(cap="genesis.fixture.native-widescreen.v1", src="3004c4367a68819c54484ef6050f4c6dcd27cb474bc71d0656bad1f0e606b342"), "a fixture capability is never buildable")
expect(2, native(cap="genesis.fixture.native-widescreen.v1", src="3004c4367a68819c54484ef6050f4c6dcd27cb474bc71d0656bad1f0e606b342"), "a fixture capability is never buildable, even with the switch",
       env_extra={"SEGARECOMP_ALLOW_FIXTURE_CAPABILITIES": "1"})
expect(2, native() + ["--platform", "master-system"], "another platform")
if failures:
    print("\n".join("FAIL: " + f for f in failures))
    sys.exit(1)
print("segarecomp_native_build_args_test: ok")
