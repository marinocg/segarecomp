#!/usr/bin/env python3
"""Regression for the SEG-083 `ctl_a = all-controller primary` CI flake: ZIP member timestamps came from the wall clock. Deterministic (no sleeping): the clock is MOVED, not waited for."""
import io
import pathlib
import subprocess
import sys
import tempfile
import textwrap
import time
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SHIM = ROOT / "tests" / "run_with_resource_shim.py"
FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)


def build(offset: float) -> bytes:
    """Same members and bytes; only the generation clock differs."""
    real, pinned = time.time, zipfile.time
    zipfile.time = time                      # this test may itself be launched through the shim, which pins zipfile's clock: use the real clock for the unpinned demonstration
    time.time = lambda: real() + offset
    try:
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("Input Log.txt", b"|..|\n" * 50)
            z.writestr("Header.txt", b"h")
        return buf.getvalue()
    finally:
        time.time = real
        zipfile.time = pinned


def test_root_cause_is_zip_metadata_only():
    a, b = build(0), build(10)
    check(a != b, "unpinned: identical content differs at the binary level when the clock moves")
    za, zb = zipfile.ZipFile(io.BytesIO(a)), zipfile.ZipFile(io.BytesIO(b))
    check(za.namelist() == zb.namelist() and all(za.read(n) == zb.read(n) for n in za.namelist()), "names, order and decompressed contents are identical")
    check([i.date_time for i in za.infolist()] != [i.date_time for i in zb.infolist()], "only the member timestamps differ")


def test_shim_pins_the_clock_for_the_real_generator():
    script = textwrap.dedent(f"""
        import sys, time, types
        try:
            import resource
        except ImportError:
            sys.modules['resource'] = types.ModuleType('resource')      # Windows: the SEG-080 adapter imports it at module level but never uses it offline
        sys.path.insert(0, {str(ROOT / 'tools')!r}); sys.path.insert(0, {str(ROOT / 'tests')!r})
        import segarecomp_genesis_tasbatch5 as t5
        from segarecomp_genesis_tasbatch_test import make_bk2
        src, _ = make_bk2(2, False, 10)
        real = time.time
        a = t5.control_movie_all(src, 3000, 1)
        time.time = lambda: real() + 10
        b = t5.control_movie_all(src, 3000, 1)
        c = t5.control_movie_all(src, 3000, 2)
        print(int(a == b), int(a != c))
    """)
    with tempfile.TemporaryDirectory() as t:
        f = pathlib.Path(t) / "probe.py"
        f.write_text(script)
        out = subprocess.run([sys.executable, str(SHIM), str(f)], capture_output=True, text=True, timeout=300)
    check(out.stdout.split() == ["1", "1"], f"pinned: the same controller streams give byte-identical archives and a different seed still differs: {out.stdout!r} {out.stderr[-300:]!r}")
    plain = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True, timeout=300)
    check(plain.stdout.split()[:1] == ["0"], f"without the pin the fixture reproduces the flake: {plain.stdout!r} {plain.stderr[-300:]!r}")


def main() -> int:
    for fn in [v for k, v in sorted(globals().items()) if k.startswith("test_")]:
        fn()
    if FAILURES:
        print("\n".join(FAILURES))
        return 1
    print("segarecomp_zip_clock_pin_test: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
