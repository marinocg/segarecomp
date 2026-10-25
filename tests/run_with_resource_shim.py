#!/usr/bin/env python3
"""Run a research-tooling test on platforms without the POSIX-only `resource` module (Windows).

The SEG-080 BizHawk adapter imports `resource` at module level but uses it only when it launches BizHawk, which no offline test does. This launcher
injects an empty stand-in when the real module is missing, then runs the given test script unchanged (so frozen, digest-bound test files need no edit).
"""
import pathlib
import runpy
import sys
import time
import types

PINNED_ZIP_EPOCH = time.mktime((2000, 1, 1, 0, 0, 0, 0, 0, -1))


class _PinnedClock:
    """`time` as seen by `zipfile` only: `time.time()` is constant, everything else is delegated."""

    def __getattr__(self, name):
        return getattr(time, name)

    @staticmethod
    def time():
        return PINNED_ZIP_EPOCH


def pin_zip_clock():
    """`ZipFile.writestr(name, data)` stamps each member with the current time (2-second resolution), so two generations of the SAME movie content can differ at the binary level
    (SEG-083 `ctl_a = all-controller primary` CI flake). Pinning the clock seen by `zipfile` makes archive bytes a function of content alone. Test-launcher only; production code is untouched."""
    import zipfile
    zipfile.time = _PinnedClock()


def pins_zip_clock(script: pathlib.Path, environ) -> bool:
    """Scope: only the SEG-081/083 TAS batch tests (they compare independently generated .bk2 archives) and anything that opts in with SEGARECOMP_PIN_ZIP_CLOCK=1."""
    return script.name.startswith("segarecomp_genesis_tasbatch") or environ.get("SEGARECOMP_PIN_ZIP_CLOCK") == "1"


def main():
    if len(sys.argv) > 1 and pins_zip_clock(pathlib.Path(sys.argv[1]), __import__("os").environ):
        pin_zip_clock()
    _main()


def _main():
    try:
        import resource  # noqa: F401
    except ImportError:
        sys.modules["resource"] = types.ModuleType("resource")

    script = pathlib.Path(sys.argv[1]).resolve()
    sys.argv = [str(script)] + sys.argv[2:]
    sys.path.insert(0, str(script.parent))
    runpy.run_path(str(script), run_name="__main__")


if __name__ == "__main__":
    main()
