#!/usr/bin/env python3
"""Run a research-tooling test on platforms without the POSIX-only `resource` module (Windows).

The SEG-080 BizHawk adapter imports `resource` at module level but uses it only when it launches BizHawk, which no offline test does. This launcher
injects an empty stand-in when the real module is missing, then runs the given test script unchanged (so frozen, digest-bound test files need no edit).
"""
import pathlib
import runpy
import sys
import types

try:
    import resource  # noqa: F401
except ImportError:
    sys.modules["resource"] = types.ModuleType("resource")

script = pathlib.Path(sys.argv[1]).resolve()
sys.argv = [str(script)] + sys.argv[2:]
sys.path.insert(0, str(script.parent))
runpy.run_path(str(script), run_name="__main__")
