"""Hermetic unit test for the shared SMS test object cache (tests/sms_cc_cache.py).

usage: sms_cc_cache_test.py <cc>
"""
import concurrent.futures
import os
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sms_cc_cache as cc_cache  # noqa: E402

CC = sys.argv[1]
failed = False


def check(cond, msg):
    global failed
    if not cond:
        failed = True
        print("FAIL: " + msg)


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def build(root, src_text, header_text="#define VALUE 1\n", flags=(), name="a b"):
    """Compiles a fresh tree under root/name (with a space in the path); returns (result, outcome, obj)."""
    d = root / name
    src = write(d / "unit.c", src_text)
    write(d / "inc" / "h.h", header_text)
    obj = d / "unit.o"
    before = cc_cache.stats()
    r = cc_cache.compile_object([CC, "-std=c11", "-O0", *flags, "-I", d / "inc", "-c", src, "-o", obj])
    after = cc_cache.stats()
    outcome = "hit" if after["hit"] > before["hit"] else "miss" if after["miss"] > before["miss"] else "bypass"
    return r, outcome, obj


SRC = '#include "h.h"\nint f(void) { return VALUE; }\n'

with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    os.environ["SEGARECOMP_TEST_OBJ_CACHE"] = str(tmp / "cache")

    r, o, obj1 = build(tmp / "one", SRC)
    check(r.returncode == 0 and obj1.is_file() and o == "miss", "first compile must miss and succeed (%s: %s)" % (o, r.stderr[:200]))
    first = obj1.read_bytes()
    r, o, obj2 = build(tmp / "two", SRC)
    check(o == "hit" and obj2.read_bytes() == first, "identical content from another directory must hit (%s)" % o)

    r, o, _ = build(tmp / "three", SRC, header_text="#define VALUE 2\n")
    check(o == "miss" and r.returncode == 0, "a changed header must miss (%s)" % o)
    r, o, _ = build(tmp / "four", SRC, flags=["-DVALUE_EXTRA=1"])
    check(o == "miss", "a changed define must miss (%s)" % o)
    r, o, _ = build(tmp / "five", SRC, flags=["-Wall"])
    check(o == "miss", "a changed flag must miss (%s)" % o)
    r, o, _ = build(tmp / "six", SRC.replace("int f(", "int g("))
    check(o == "miss", "a one-byte source change must miss (%s)" % o)
    r, o, _ = build(tmp / "seven", SRC)
    check(o == "hit", "the original content must still hit (%s)" % o)

    bad = '#include "h.h"\nint f(void) { return VALUE +; }\n'
    r, o, _ = build(tmp / "bad1", bad)
    check(r.returncode != 0 and o == "miss", "a failing compile reports failure (%s)" % o)
    r, o, _ = build(tmp / "bad2", bad)
    check(r.returncode != 0 and o == "miss", "a failed compile must never be cached (%s)" % o)

    def worker(n):
        return build(tmp / "conc", SRC + "int g%d(void) { return 0; }\n" % (n % 2), name="w%d" % n)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(worker, range(16)))
    check(all(r.returncode == 0 and obj.stat().st_size > 0 for r, _, obj in results), "concurrent writers must all succeed")
    sizes = {n % 2: results[n][2].read_bytes() for n in range(16)}
    check(all(results[n][2].read_bytes() == sizes[n % 2] for n in range(16)), "concurrent builds of one key must be identical")
    check(not [p for p in (tmp / "cache").iterdir() if p.name.endswith(".tmp")], "no temp files may remain in the cache")

    blocker = write(tmp / "blocker", "not a directory")
    os.environ["SEGARECOMP_TEST_OBJ_CACHE"] = str(blocker / "cache")
    r, o, obj = build(tmp / "unwritable", SRC)
    check(r.returncode == 0 and obj.is_file() and o == "bypass", "an unusable cache directory must fall back to a plain compile (%s)" % o)

print("sms cc cache: %s" % ("FAILED" if failed else "ok"))
sys.exit(1 if failed else 0)
