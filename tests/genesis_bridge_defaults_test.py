#!/usr/bin/env python3
"""Focused proof for the bridge's first-run defaults (binary path, compiler selection)."""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import genesis_startup_bridge as b  # noqa: E402

root = pathlib.Path("/r")
assert b.default_segarecomp(root, "posix") == root / "build/dev/apps/segarecomp/segarecomp"
assert b.default_segarecomp(root, "nt").name == "segarecomp.exe"
path = {"cc": "/bin/cc", "clang": "/bin/clang", "gcc": "/bin/gcc"}
which = path.get
assert b.select_compiler("clang", "gcc", which) == "/bin/clang"   # --cc wins, resolved via PATH
assert b.select_compiler(None, "gcc", which) == "/bin/gcc"        # then $CC
assert b.select_compiler(None, None, which) == "/bin/cc"          # then PATH order
assert b.select_compiler(None, None, {"gcc": "/bin/gcc"}.get) == "/bin/gcc"
assert b.select_compiler(None, None, {}.get) is None              # missing -> caller errors
assert b.select_compiler("/x/custom-cc", None, which) == "/x/custom-cc"
sys.exit(0)
