#!/usr/bin/env python3
"""ADR-0014 (docs/decisions/0014-entry-rooted-discovery-prefix-boundary-for-a-
fragmented-ancestor.md) Binding scope item 3's strict-C11 compilation check.

`--emit-general-startup-runtime-c4-discovery-boundary-multi-frame` re-emits
the identical two-call-frame-deep discovery_prefix_boundary partial program
`general_startup_promotes_a_multi_frame_deep_discovery_boundary_with_orphaned_continuations`
(tests/m68k_pipeline_test.cpp) analyzes and asserts directly: two admitted
JSR call frames whose own callees never admit an RTS (so both continuations
are retained only through ADR-0014 Decision Sec1b's call-to-continuation
reachability relation), with the m68k_discovery_max_instructions ceiling
tripping inside the second callee. This test proves the resulting generated C
is deterministic across independent runs and strict-C11 compilable, per the
ADR's own binding-scope requirement -- it does not re-assert the discovery/
reachability structure itself (covered directly in the C++ suite above).
"""
import pathlib
import subprocess
import sys
import tempfile


def main():
    executable, compiler, root = sys.argv[1:]
    first = subprocess.run(
        [executable, "--emit-general-startup-runtime-c4-discovery-boundary-multi-frame"],
        text=True, capture_output=True)
    assert first.returncode == 0, first.stderr
    assert not first.stdout.startswith("/* translation rejected:"), first.stdout
    assert "GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY" in first.stdout
    second = subprocess.run(
        [executable, "--emit-general-startup-runtime-c4-discovery-boundary-multi-frame"],
        text=True, capture_output=True)
    assert second.returncode == 0, second.stderr
    assert first.stdout == second.stdout, "two independent runs must emit byte-identical generated C"
    with tempfile.TemporaryDirectory() as temp:
        path = pathlib.Path(temp)
        (path / "generated.c").write_text(first.stdout)
        standalone = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"),
             "-c", str(path / "generated.c"), "-o", str(path / "multi-frame-boundary.o")],
            text=True, capture_output=True)
        assert standalone.returncode == 0, standalone.stderr
    print("genesis startup runtime C4 discovery-prefix-boundary multi-frame: ok")


if __name__ == "__main__":
    main()
