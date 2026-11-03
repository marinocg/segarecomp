#!/usr/bin/env python3
"""SEG-124 (ADR 0267): patch package schema 2 with a source-bearing native payload (trusted native-source patch), through the product CLI seam (`segarecomp patch ...`),
cross-checked against the independent Python reference in tools/segapatch.py. Synthetic fixture image and the project-authored sample provider (docs/product/sdk-sample).
Every malformed or unsupported input must be refused with a readable error and no output file; schema 1 (BPS-only) stays byte-compatible.
usage: segapatch_native_package_test.py <segarecomp-cli> <tools/segapatch.py> <sample provider.c>"""
import hashlib
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

CLI = sys.argv[1]
spec = importlib.util.spec_from_file_location("segapatch", sys.argv[2])
sp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sp)
PROVIDER = Path(sys.argv[3]).read_bytes()
CAPS = ["frame.work_ram", "frame.vdp", "scenery.words", "overlay.rgba"]
failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL:", what)


def run(*args):
    p = subprocess.run([CLI, "patch", *map(str, args)], capture_output=True, text=True, timeout=60)
    try:
        doc = json.loads(p.stdout) if p.stdout.strip() else None
    except ValueError:
        doc = None
    return p.returncode, doc


def rom(n=0x10000, seed=7):
    out = bytearray()
    x = seed
    while len(out) < n:
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out += struct.pack("<I", x)
    return bytes(out[:n])


sha = lambda b: hashlib.sha256(b).hexdigest()
SRC = rom()
TGT = bytearray(SRC)
TGT[0x1000:0x1010] = b"PATCHED-WIDE-FIX"
TGT = bytes(TGT)
tmp = Path(tempfile.mkdtemp(prefix="segapatch-native-test-"))
(tmp / "src.bin").write_bytes(SRC)


def make(name, **kw):
    out = tmp / name
    base = dict(id_="synthetic-pkg", name="Synthetic", version="1.0.0", native_source=PROVIDER, native_caps=CAPS, author="Test Author", license_="MIT")
    base.update(kw)
    sp.create(SRC, base.pop("target", None), out, **base)
    return out


def rezip(src, name, edit):
    """Rewrites a package: edit(files: dict[name -> bytes]) -> files (or a list of (name, bytes))."""
    with zipfile.ZipFile(src) as z:
        files = {n: z.read(n) for n in z.namelist()}
    result = edit(files)
    out = tmp / name
    sp.write_zip(out, list(result.items()) if isinstance(result, dict) else result)
    return out


def with_manifest(src, name, fn):
    def edit(files):
        m = json.loads(files["manifest.json"])
        fn(m)
        files["manifest.json"] = (json.dumps(m, indent=2) + "\n").encode()
        return files
    return rezip(src, name, edit)


def refused(pkg, what, rom_bytes=SRC, package_valid=False):
    r = tmp / "rom.bin"
    r.write_bytes(rom_bytes)
    out = tmp / "refused.out"
    if out.exists():
        out.unlink()
    code, doc = run("apply", pkg, "--rom", r, "--out", out)
    check(code == 1 and doc and doc["ok"] is False and doc.get("error"), f"{what}: refused with a readable error (code={code} doc={doc})")
    check(not out.exists() and not Path(str(out) + ".partial").exists(), f"{what}: no output or partial file")
    check((run("inspect", pkg)[0] == 0) == package_valid, f"{what}: inspect {'accepts' if package_valid else 'refuses'} the package itself")


# ---- accepted shapes ------------------------------------------------------------------------------------------------------------
nat = make("native-only.segapatch")
code, doc = run("inspect", nat)
check(code == 0 and doc and doc["ok"] and doc["schema_version"] == 2 and doc["has_native"] and not doc["has_rom"] and doc["executable_code"] is True, f"native-only inspect ({doc})")
check(doc and doc["native_source_sha256"] == sha(PROVIDER) and doc["author"] == "Test Author" and doc["license"] == "MIT" and doc["native_abi"] == 1 and doc["native_capabilities"] == 15, "inspect reports digest, author, ABI, capabilities")
nat_id = doc["package_identity"]
out = tmp / "native.out"
code, doc = run("apply", nat, "--rom", tmp / "src.bin", "--out", out)
check(code == 0 and out.read_bytes() == SRC, "a native-only package leaves the image byte-identical")
code, doc = run("verify", nat, "--rom", tmp / "src.bin")
check(code == 0, "verify native-only")
ref = tmp / "ref.out"
check(sp.apply_package(nat, SRC) == SRC, "the independent reference reader agrees on the native-only package")

both = make("both.segapatch", target=TGT)
code, doc = run("inspect", both)
check(code == 0 and doc and doc["has_rom"] and doc["has_native"], "combined package inspect")
check(sp.apply_package(both, SRC) == TGT, "the independent reference reader agrees on the combined package")
out = tmp / "both.out"
code, doc = run("apply", both, "--rom", tmp / "src.bin", "--out", out)
check(code == 0 and out.read_bytes() == TGT, "combined package applies the BPS")
check(run("inspect", both)[1]["package_identity"] != nat_id, "BPS adds to the package identity")

# schema 1, BPS only: unchanged
bps = tmp / "bps.segapatch"
sp.create(SRC, TGT, bps, id_="bps-only", name="Bps", version="1.0.0")
code, doc = run("inspect", bps)
check(code == 0 and doc and doc["schema_version"] == 1 and not doc["has_native"] and doc["executable_code"] is False, "BPS-only schema 1 still works and carries no code")
code, doc = run("apply", bps, "--rom", tmp / "src.bin", "--out", tmp / "bps.out")
check(code == 0 and (tmp / "bps.out").read_bytes() == TGT, "BPS-only applies")
# deterministic packaging
check(make("again.segapatch").read_bytes() == nat.read_bytes(), "packaging is deterministic")
# different source -> different identity (source change invalidates)
changed = make("changed.segapatch", native_source=PROVIDER + b"\n/* v2 */\n")
check(run("inspect", changed)[1]["package_identity"] != nat_id, "a provider source change changes the package identity")
# a different version string
check(run("inspect", make("v2.segapatch", version="1.0.1"))[1]["package_identity"] != nat_id, "a version change changes the identity")

# ---- refusals ------------------------------------------------------------------------------------------------------------------
refused(nat, "wrong original image", rom_bytes=rom(0x10000, 9), package_valid=True)
refused(with_manifest(nat, "abi2.segapatch", lambda m: m["payloads"]["native"].__setitem__("abi", 2)), "ABI mismatch")
refused(with_manifest(nat, "abi0.segapatch", lambda m: m["payloads"]["native"].__setitem__("abi", 0)), "ABI zero")
refused(with_manifest(nat, "capx.segapatch", lambda m: m["payloads"]["native"].__setitem__("capabilities", ["frame.work_ram", "scenery.words", "memory.write"])), "unknown capability")
refused(with_manifest(nat, "capdup.segapatch", lambda m: m["payloads"]["native"].__setitem__("capabilities", ["scenery.words", "scenery.words"])), "repeated capability")
refused(with_manifest(nat, "capnone.segapatch", lambda m: m["payloads"]["native"].__setitem__("capabilities", ["frame.work_ram"])), "a provider that contributes nothing")
refused(with_manifest(nat, "capempty.segapatch", lambda m: m["payloads"]["native"].__setitem__("capabilities", [])), "no capabilities")
refused(with_manifest(nat, "badhash.segapatch", lambda m: m["payloads"]["native"].__setitem__("sha256", "0" * 64)), "invalid source hash")
refused(with_manifest(nat, "noexec.segapatch", lambda m: m["payloads"]["native"].pop("executable")), "missing executable-code declaration")
refused(with_manifest(nat, "execno.segapatch", lambda m: m["payloads"]["native"].__setitem__("executable", "no")), "false executable-code declaration")
refused(with_manifest(nat, "noauthor.segapatch", lambda m: m.pop("author")), "code without an author")
refused(with_manifest(nat, "fmt.segapatch", lambda m: m["payloads"]["native"].__setitem__("format", "wasm")), "unsupported payload format")
refused(with_manifest(nat, "file.segapatch", lambda m: m["payloads"]["native"].__setitem__("file", "../evil.c")), "unsafe source file name")
refused(with_manifest(nat, "file2.segapatch", lambda m: m["payloads"]["native"].__setitem__("file", "other.c")), "source file name other than provider.c")
refused(with_manifest(nat, "extra.segapatch", lambda m: m["payloads"]["native"].__setitem__("flags", "-O3")), "unknown field in the native payload (no compiler flags)")
refused(with_manifest(nat, "buildkey.segapatch", lambda m: m.__setitem__("build", {"cflags": "-fplugin=x"})), "unknown top-level build field")
refused(with_manifest(nat, "req.segapatch", lambda m: m.__setitem__("requires", ["genesis.sonic1.native-widescreen.v1"])), "a requires entry (no built-in game capability exists)")
refused(with_manifest(nat, "type.segapatch", lambda m: m["payloads"].__setitem__("binary", {"format": "elf", "file": "a.out", "sha256": "0" * 64})), "unsupported payload type (prebuilt binary)")
refused(rezip(nat, "missing.segapatch", lambda f: {k: v for k, v in f.items() if k != "provider.c"}), "missing provider source")
refused(rezip(nat, "undeclared.segapatch", lambda f: {**f, "build.sh": b"#!/bin/sh\nrm -rf /\n"}), "undeclared file (build script)")
refused(rezip(nat, "undeclared2.segapatch", lambda f: {**f, "extra.c": b"int x;"}), "undeclared extra source")
refused(rezip(nat, "traversal.segapatch", lambda f: [("manifest.json", f["manifest.json"]), ("provider.c", f["provider.c"]), ("../escape.c", b"x")]), "path traversal entry")
refused(rezip(nat, "abs.segapatch", lambda f: [("manifest.json", f["manifest.json"]), ("provider.c", f["provider.c"]), ("/etc/passwd", b"x")]), "absolute path entry")
refused(rezip(nat, "tampered.segapatch", lambda f: {**f, "provider.c": f["provider.c"] + b"\n"}), "source modified after packaging (digest mismatch)")
refused(rezip(nat, "empty.segapatch", lambda f: {**f, "provider.c": b""}), "empty source")
refused(rezip(nat, "nul.segapatch", lambda f: {**f, "provider.c": f["provider.c"] + b"\0"}), "source containing NUL")


def oversize(f):
    big = b"/* " + b"A" * (300 * 1024) + b" */\n"
    m = json.loads(f["manifest.json"])
    m["payloads"]["native"]["sha256"] = sha(big)
    return {"manifest.json": (json.dumps(m) + "\n").encode(), "provider.c": big}


refused(rezip(nat, "oversize.segapatch", oversize), "oversized source entry")
dup = tmp / "dup.segapatch"
with zipfile.ZipFile(dup, "w") as z:
    z.writestr("manifest.json", zipfile.ZipFile(nat).read("manifest.json"))
    z.writestr("provider.c", PROVIDER)
    z.writestr("provider.c", PROVIDER)
refused(dup, "duplicate entry")
refused(with_manifest(bps, "s1author.segapatch", lambda m: m.__setitem__("author", "x")), "author in a schema 1 package")
# schema 1 cannot carry native source
refused(with_manifest(nat, "s1native.segapatch", lambda m: m.__setitem__("schema_version", 1)), "native payload in a schema 1 package")
# the old schema-2 built-in-Sonic descriptor payload is explicitly refused (no migration of a built-in capability)
refused(with_manifest(nat, "olddesc.segapatch", lambda m: m["payloads"]["native"].update({"format": "descriptor", "file": "widescreen.json"})), "old descriptor-style schema 2 package")

if failures:
    print(f"segapatch_native_package_test: {len(failures)} failure(s)")
    sys.exit(1)
print("segapatch_native_package_test: ok")
