"""Shared helpers of the modding-platform tests (SEG-135, SEG-146, SEG-145): an INDEPENDENT Python builder of schema 3 manifests/archives and a thin CLI runner.
The builder shares no code with libs/media/src/mod_*.cpp, so the tests cross-check the C++ reader and packer."""
import copy
import hashlib
import json
import subprocess
import tempfile
import zipfile
from pathlib import Path

SRC_IMAGE = b"synthetic-source-image-A"
SRC_SHA = hashlib.sha256(SRC_IMAGE).hexdigest()   # a86d6042... (schema 3 section 12)
FIXED_TIME = (1980, 1, 1, 0, 0, 0)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def canon(manifest):
    """Canonical manifest bytes: compact separators, insertion order (schema 3 section 2)."""
    return json.dumps(manifest, separators=(",", ":")).encode("ascii")


def payload(kind, path, data, **fields):
    entry = {"kind": kind, "kind_version": 1, "path": path, "size": len(data), "sha256": sha(data)}
    entry.update(fields)
    return entry


def luau(path="scripts/main.luau", data=b"-- hello"):
    return payload("luau-source", path, data, role="script"), (path, data)


def native(path="src/main.c", data=b"int x;\n"):
    return payload("native-c11", path, data, role="provider"), (path, data)


def base_manifest(platform="genesis", payloads=None, **extra):
    """A valid schema 3 manifest skeleton (key order = canonical order)."""
    revision = {"label": "rev-a", "sha256": SRC_SHA}
    if platform == "master-system":
        revision["mapper"] = "sega"
    manifest = {"format": "segarecomp-patch", "schema_version": 3, "id": "example-mod", "version": "1.0.0", "name": "Example", "platform": platform}
    manifest.update(extra.pop("head", {}))
    manifest["source"] = {"kind": "cartridge-image", "revisions": [revision]}
    manifest.update(extra)
    manifest["payloads"] = [p for p, _ in (payloads or [])]
    return manifest


def make_zip(entries, *, deflate=(), names_raw=False):
    """entries: list of (name, bytes). Returns zip bytes with fixed times; `deflate` lists names stored with DEFLATE."""
    import io
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name, data in entries:
            info = zipfile.ZipInfo(name, FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED if name in deflate else zipfile.ZIP_STORED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            z.writestr(info, data)
    return buf.getvalue()


def build(manifest, payloads, *, raw_manifest=None, extra_entries=(), deflate=()):
    entries = [("manifest.json", raw_manifest if raw_manifest is not None else canon(manifest))]
    entries += [e for _, e in payloads]
    entries += list(extra_entries)
    return make_zip(entries, deflate=deflate)


class Cli:
    def __init__(self, exe):
        self.exe = str(Path(exe).resolve())
        self.failures = []

    def run(self, *args, cwd=None):
        p = subprocess.run([self.exe, *map(str, args)], capture_output=True, text=True, timeout=300, cwd=cwd)
        try:
            doc = json.loads(p.stdout) if p.stdout.strip() else None
        except ValueError:
            doc = None
        return p.returncode, doc, p

    def check(self, cond, what):
        if not cond:
            self.failures.append(what)
            print("FAIL:", what)

    def done(self, name):
        if self.failures:
            raise SystemExit(f"{name}: {len(self.failures)} failure(s)")
        print(f"{name}: ok")


def write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode("ascii"))
    return path


def temp():
    return Path(tempfile.mkdtemp(prefix="segarecomp-mod-test-"))
