#!/usr/bin/env python3
"""SEG-022-T012: opt-in content-addressed generated-object cache. Every key input (TU content, an
included header, compiler identity/version, target, flags) invalidates the entry; cold and warm
builds link behaviorally identical executables; corrupt/mismatched entries fail safe by recompiling;
the disabled cache is inert; LRU eviction bounds disk use. Project-authored C only; no ROM.
argv: [product root] [C compiler]."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1]
CC = sys.argv[2] if len(sys.argv) > 2 else (os.environ.get("CC") or shutil.which("cc") or "cc")
sys.path.insert(0, str(ROOT))
from tools import genesis_startup_bridge as bridge  # noqa: E402


def require(cond, msg):
    if not cond:
        raise RuntimeError(msg)


FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O0"]


def stats():
    return dict(bridge._object_cache_stats)


def build(t: pathlib.Path, jobs, name: str):
    """Compile jobs through the bridge path, link, run; return (rc, stdout, objects bytes, stats delta)."""
    before = stats()
    out = t / name
    out.mkdir()
    objects, failure = bridge.compile_objects(jobs, t, out)
    after = stats()
    delta = {k: after.get(k, 0) - before.get(k, 0) for k in set(after) | set(before)}
    if failure is not None:
        return None, failure, [], delta
    exe = out / ("prog.exe" if os.name == "nt" else "prog")
    subprocess.run([CC, "-o", str(exe)] + objects, check=True, cwd=t)
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    return run.returncode, run.stdout, [pathlib.Path(o).read_bytes() for o in objects], delta


def main():
    with tempfile.TemporaryDirectory() as tmp:
        t = pathlib.Path(tmp)
        (t / "inc").mkdir()
        hdr = t / "inc" / "unit.h"
        hdr.write_text("#define UNIT_VALUE 7\nint unit_value(void);\n")
        a = t / "a.c"
        a.write_text('#include <stdio.h>\n#include "unit.h"\nint main(void) { printf("%d\\n", unit_value()); return 0; }\n')
        b = t / "b.c"
        b.write_text('#include "unit.h"\nint unit_value(void) { return UNIT_VALUE; }\n')
        base = [CC] + FLAGS + ["-I", str(t / "inc")]
        jobs = [(base, a), (base, b)]

        # Disabled cache: inert (no stats, no directory).
        cache = t / "cache"
        bridge._object_cache_dir_override = None
        os.environ.pop(bridge.OBJECT_CACHE_ENV, None)
        rc, out0, objs0, d = build(t, jobs, "disabled")
        require(rc == 0 and out0 == "7\n" and not any(d.values()) and not cache.exists(), f"disabled {d}")

        # Cold then warm: warm reuses every object; executables behave identically; objects identical.
        bridge._object_cache_dir_override = cache
        rc, out1, objs1, d = build(t, jobs, "cold")
        require(rc == 0 and out1 == out0 and d.get("misses") == 2 and d.get("stores") == 2 and not d.get("hits"), f"cold {d}")
        rc, out2, objs2, d = build(t, jobs, "warm")
        require(rc == 0 and out2 == out1 and d.get("hits") == 2 and not d.get("misses"), f"warm {d}")
        require(objs2 == objs1, "warm objects differ from cold objects")

        # Each key input invalidates: TU content, included header content, flags/defines, compiler identity.
        b.write_text('#include "unit.h"\nint unit_value(void) { return UNIT_VALUE + 1; }\n')
        rc, out, _, d = build(t, jobs, "src")
        require(out == "8\n" and d.get("hits") == 1 and d.get("misses") == 1, f"source change {d}")
        hdr.write_text("#define UNIT_VALUE 9\nint unit_value(void);\n")
        rc, out, _, d = build(t, jobs, "hdr")
        require(out == "10\n" and d.get("misses") == 2 and not d.get("hits"), f"header change {d}")
        rc, out, _, d = build(t, [(base + ["-O1"], a), (base + ["-O1"], b)], "flags")
        require(out == "10\n" and d.get("misses") == 2 and not d.get("hits"), f"flag change {d}")
        rc, out, _, d = build(t, [(base, a), (base + ["-DUNIT_EXTRA=1"], b)], "define")
        require(out == "10\n" and d.get("hits") == 1 and d.get("misses") == 1, f"define change {d}")
        # A macro-only change (no token change in the compiled text) still invalidates: macros can
        # reach the object (-g3) or diagnostics (-Wunused-macros under -Werror).
        b.write_text('#include "unit.h"\n#define UNIT_UNUSED_MARK 1\nint unit_value(void) { return UNIT_VALUE + 1; }\n')
        rc, out, _, d = build(t, jobs, "macro")
        require(out == "10\n" and d.get("hits") == 1 and d.get("misses") == 1, f"macro-only change {d}")
        if os.name != "nt":  # POSIX shell compiler wrappers
            wrapper = t / "cc-wrapper.sh"
            wrapper.write_text('#!/bin/sh\nif [ "$1" = "--version" ]; then "%s" --version; echo "%s"; exit $?; fi\n'
                               'exec "%s" "$@"\n' % (CC, "wrapped-" + os.environ.get("CACHE_TEST_VER", "v1"), CC))
            wrapper.chmod(0o755)
            wjobs = [([str(wrapper)] + FLAGS + ["-I", str(t / "inc")], s) for s in (a, b)]
            rc, out, _, d = build(t, wjobs, "id1")
            require(out == "10\n" and d.get("misses") == 2, f"new compiler identity {d}")
            rc, out, _, d = build(t, wjobs, "id1b")
            require(d.get("hits") == 2, f"same wrapped identity {d}")
            wrapper.write_text(wrapper.read_text().replace("wrapped-v1", "wrapped-v2"))
            bridge._compiler_identity_memo.clear()
            rc, out, _, d = build(t, wjobs, "id2")
            require(out == "10\n" and d.get("misses") == 2 and not d.get("hits"), f"compiler version change {d}")

        # Target triple is part of the identity.
        ident = bridge.compiler_identity(CC, t)
        machine = subprocess.run([CC, "-dumpmachine"], capture_output=True).stdout
        require(ident is not None and machine.strip() and machine in ident, "target triple not in identity")

        # Corruption / mismatch fail safe: recompiled, correct result, entry repaired.
        entries = sorted(cache.glob("*/*.obj"))
        require(entries, "no entries")
        for mode in ("flip", "truncate", "garbage", "empty"):
            for e in cache.glob("*/*.obj"):
                blob = e.read_bytes()
                e.write_bytes({"flip": blob[:-1] + bytes([blob[-1] ^ 0xFF]), "truncate": blob[: len(blob) // 2],
                               "garbage": b"not an object", "empty": b""}[mode])
            rc, out, _, d = build(t, jobs, "corrupt-" + mode)
            require(rc == 0 and out == "10\n" and d.get("rejected") == 2 and d.get("misses") == 2 and not d.get("hits"),
                    f"corruption {mode} {d}")
            rc, out, _, d = build(t, jobs, "repaired-" + mode)
            require(out == "10\n" and d.get("hits") == 2, f"repair {mode} {d}")

        # Failed compile is never stored.
        bad = t / "bad.c"
        bad.write_text("int broken(void) { return }\n")
        rc, failure, _, d = build(t, [(base, bad)], "bad")
        require(rc is None and "bad.c" in failure and not d.get("stores"), f"failure stored {d}")

        # Uncacheable driver (cannot report identity) compiles normally without the cache.
        if os.name != "nt":
            fake = t / "noid.sh"
            fake.write_text('#!/bin/sh\ncase "$1" in --version|-dumpmachine) exit 1;; esac\nexec "%s" "$@"\n' % CC)
            fake.chmod(0o755)
            rc, out, _, d = build(t, [([str(fake)] + FLAGS + ["-I", str(t / "inc")], s) for s in (a, b)], "noid")
            require(out == "10\n" and d.get("uncacheable") == 2 and not d.get("stores"), f"uncacheable {d}")

        # Env configuration and LRU size bound.
        bridge._object_cache_dir_override = None
        os.environ[bridge.OBJECT_CACHE_ENV] = str(cache)
        os.environ[bridge.OBJECT_CACHE_MAX_BYTES_ENV] = "1"
        rc, out, _, d = build(t, jobs, "env")
        require(out == "10\n" and d.get("hits") == 2 and d.get("evicted", 0) >= 1, f"env/evict {d}")
        require(sum(p.stat().st_size for p in cache.glob("*/*.obj")) <= 1, "cache exceeds bound")

        # Stale temporary files from an interrupted store are evicted; fresh ones are left alone.
        os.environ[bridge.OBJECT_CACHE_MAX_BYTES_ENV] = str(1 << 30)
        build(t, jobs, "refill")
        shard = next(cache.glob("*/"))
        stale, fresh = shard / "x.obj.1.1.tmp", shard / "y.obj.2.2.tmp"
        stale.write_bytes(b"partial")
        fresh.write_bytes(b"partial")
        os.utime(stale, (0, 0))
        rc, out, _, d = build(t, jobs, "tmpclean")
        require(out == "10\n" and not stale.exists() and fresh.exists(), f"stale temp handling {d}")
        fresh.unlink()

        # Cache I/O failures never fail the build: a read-only cache that must evict (over cap) and a
        # corrupt entry that cannot be deleted both still build correctly by recompiling.
        if os.name != "nt" and hasattr(os, "geteuid") and os.geteuid() != 0:
            dirs = [cache] + [p for p in cache.glob("*/") if p.is_dir()]
            for e in cache.glob("*/*.obj"):
                e.write_bytes(b"junk")
            for p in dirs:
                p.chmod(0o555)
            try:
                os.environ[bridge.OBJECT_CACHE_MAX_BYTES_ENV] = "1"
                rc, out, _, d = build(t, jobs, "readonly")
                require(rc == 0 and out == "10\n" and d.get("misses") == 2 and d.get("rejected") == 2
                        and d.get("store_failed") == 2, f"read-only cache {d}")
            finally:
                for p in dirs:
                    p.chmod(0o755)
    print("ok")


main()
