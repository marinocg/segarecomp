"""Shared content-keyed object cache for the SMS tests' C compilations.

`compile_object(cmd)` behaves like `subprocess.run(cmd, capture_output=True, text=True)` for a `cc <flags> -c src -o obj`
invocation. The key is sha256(compiler identity, flags without -o/-c/-I, `cc -E` output with line markers stripped), so
identical translation units (padding shards, the same fixture rebuilt by several tests, runtime sources) are compiled once
per cache directory, across tests and concurrent processes. Include paths are excluded from the key because the preprocessed
text already captures every included byte; this lets temp directories differ. Failed compiles are never cached, and any cache
problem falls back to a plain compile (the compile always writes the requested object directly; publishing is best effort).

Env: SEGARECOMP_TEST_OBJ_CACHE (cache directory), SEGARECOMP_TEST_OBJ_CACHE_STATS=1 (one-line hit/miss summary at exit).
"""
import atexit
import functools
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import uuid

MAX_OBJECT_BYTES = 64 * 1024 * 1024
_LINEMARKER = re.compile(rb"^#\s*(line\s+)?\d+\b.*$", re.M)
_lock = threading.Lock()
_stats = {"hit": 0, "miss": 0, "bypass": 0}


def cache_dir():
    env = os.environ.get("SEGARECOMP_TEST_OBJ_CACHE")
    if env:
        return env
    try:
        user = str(os.getuid())
    except AttributeError:
        user = re.sub(r"\W", "_", os.environ.get("USERNAME", "user"))
    return os.path.join(tempfile.gettempdir(), "segarecomp-sms-test-objcache-" + user)


def _run(cmd, timeout):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout)


@functools.lru_cache(maxsize=None)
def _compiler_identity(cc):
    try:
        v = subprocess.run([cc, "--version"], capture_output=True, text=True, timeout=60)
        return cc + "\n" + v.stdout + v.stderr
    except (OSError, subprocess.SubprocessError):
        return None


def _split(cmd):
    """Returns (cc, key_flags, preprocess_flags, src, obj) or None when `cmd` is not a plain single-source -c compile."""
    args = [str(c) for c in cmd]
    cc, rest = args[0], args[1:]
    src = obj = None
    key_flags, pp_flags = [], []
    i = 0
    while i < len(rest):
        a = rest[i]
        if a == "-c" and i + 1 < len(rest) and src is None:
            src = rest[i + 1]
            i += 2
        elif a == "-o" and i + 1 < len(rest) and obj is None:
            obj = rest[i + 1]
            i += 2
        elif a in ("-I", "-isystem", "-include", "-iquote") and i + 1 < len(rest):
            pp_flags += [a, rest[i + 1]]
            if a == "-include":
                return None  # forced include path: not worth reasoning about
            i += 2
        elif a.startswith("-I") or a.startswith("-isystem") or a.startswith("-iquote"):
            pp_flags.append(a)
            i += 1
        else:
            key_flags.append(a)
            if a == "-D" and i + 1 < len(rest):
                key_flags.append(rest[i + 1])
                i += 1
            i += 1
    if src is None or obj is None:
        return None
    return cc, key_flags, pp_flags, src, obj


def _key(cc, key_flags, pp_flags, src, timeout):
    ident = _compiler_identity(cc)
    if ident is None:
        return None
    pp = subprocess.run([cc, *key_flags, *pp_flags, "-E", "-P", src], capture_output=True, timeout=timeout)
    if pp.returncode != 0:
        return None
    text = _LINEMARKER.sub(b"", pp.stdout)
    h = hashlib.sha256()
    for part in (ident.encode("utf-8", "replace"), "\0".join(key_flags).encode("utf-8", "replace"), b"-g-src:" +
                 (src.encode("utf-8", "replace") if "-g" in key_flags else b""), text):
        h.update(len(part).to_bytes(8, "little"))
        h.update(part)
    return h.hexdigest()


def _count(kind):
    with _lock:
        _stats[kind] += 1


def compile_object(cmd, timeout=600):
    parsed = None
    key = path = None
    try:
        parsed = _split(cmd)
        if parsed is not None:
            cc, key_flags, pp_flags, src, obj = parsed
            directory = cache_dir()
            os.makedirs(directory, exist_ok=True)
            key = _key(cc, key_flags, pp_flags, src, timeout)
            if key is not None:
                path = os.path.join(directory, key + ".o")
                if os.path.isfile(path):
                    shutil.copyfile(path, obj)
                    _count("hit")
                    return subprocess.CompletedProcess([str(c) for c in cmd], 0, "", "")
    except Exception:  # noqa: BLE001 - any cache problem means "compile normally"
        key = path = None
    result = _run(cmd, timeout)
    if path is None:
        _count("bypass")
        return result
    _count("miss")
    if result.returncode == 0:
        tmp = os.path.join(os.path.dirname(path), "%s.%d.%s.tmp" % (key, os.getpid(), uuid.uuid4().hex))
        try:
            if os.path.getsize(parsed[4]) <= MAX_OBJECT_BYTES:
                shutil.copyfile(parsed[4], tmp)
                os.replace(tmp, path)
        except Exception:  # noqa: BLE001
            pass
        finally:
            try:
                os.unlink(tmp)
            except OSError:
                pass
    return result


def stats():
    return dict(_stats)


def _report():
    if os.environ.get("SEGARECOMP_TEST_OBJ_CACHE_STATS") == "1":
        total = _stats["hit"] + _stats["miss"]
        print("objcache: hit=%d miss=%d bypass=%d hit_rate=%.1f%% (%s)" % (
            _stats["hit"], _stats["miss"], _stats["bypass"], 100.0 * _stats["hit"] / total if total else 0.0,
            os.path.basename(sys.argv[0])), file=sys.stderr)


atexit.register(_report)
