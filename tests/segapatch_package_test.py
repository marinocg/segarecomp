#!/usr/bin/env python3
"""SEG-122-T001: patch package v1 through the product CLI seam (`segarecomp patch ...`), cross-checked against the
independent Python reference in tools/segapatch.py. Every malformed input must be refused, with no output file left behind.
usage: segapatch_package_test.py <segarecomp-cli> <tools/segapatch.py>"""
import hashlib
import importlib.util
import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

CLI = sys.argv[1]
spec = importlib.util.spec_from_file_location("segapatch", sys.argv[2])
sp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sp)

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


SRC = rom()
TGT = bytearray(SRC)
TGT[0x1000:0x1010] = b"PATCHED-WIDE-FIX"
TGT[0x8000:0x8004] = b"\xde\xad\xbe\xef"
TGT = bytes(TGT) + b"EXTRA-TAIL"
sha = lambda b: hashlib.sha256(b).hexdigest()

tmp = Path(tempfile.mkdtemp(prefix="segapatch-test-"))


def expect_refused(pkg, rom_bytes, what, *, rom_name="rom.bin"):
    r = tmp / rom_name
    r.write_bytes(rom_bytes)
    out = tmp / "refused.out"
    if out.exists():
        out.unlink()
    code, doc = run("apply", pkg, "--rom", r, "--out", out)
    check(code == 1 and doc and doc["ok"] is False and doc.get("error"), f"{what}: refused with a readable error (code={code} doc={doc})")
    check(not out.exists() and not Path(str(out) + ".partial").exists(), f"{what}: no output or partial file")
    code, doc = run("verify", pkg, "--rom", r)
    check(code == 1, f"{what}: verify refuses")


# ---- valid package, determinism, cross-implementation ----
good = tmp / "good.segapatch"
sp.create(SRC, TGT, good, id_="synthetic-wide", name="Synthetic Widescreen", version="1.0.0")
good2 = tmp / "good2.segapatch"
sp.create(SRC, TGT, good2, id_="synthetic-wide", name="Synthetic Widescreen", version="1.0.0")
check(good.read_bytes() == good2.read_bytes(), "authoring is deterministic")
(tmp / "src.bin").write_bytes(SRC)
code, doc = run("inspect", good)
check(code == 0 and doc["ok"] and doc["source_sha256"] == sha(SRC) and doc["target_sha256"] == sha(TGT) and doc["platform"] == "genesis", f"inspect {doc}")
identity = doc["package_identity"]
code, doc = run("apply", good, "--rom", tmp / "src.bin", "--out", tmp / "out.bin")
check(code == 0 and doc["ok"] and doc["rom_sha256"] == sha(SRC), f"apply {doc}")
check((tmp / "out.bin").read_bytes() == TGT, "C++ output equals the intended target")
check(sp.apply_package(good, SRC) == TGT, "python reference output equals the intended target")
check(not (tmp / "out.bin.partial").exists(), "no partial file after success")
code, doc = run("verify", good, "--rom", tmp / "src.bin")
check(code == 0, "verify ok")
# a changed package changes the identity
sp.create(SRC, TGT, tmp / "other.segapatch", id_="synthetic-wide", name="Synthetic Widescreen", version="1.0.1")
check(run("inspect", tmp / "other.segapatch")[1]["package_identity"] != identity, "identity changes with the package")
# stored (no compression) archives are accepted too
sp.write_zip(tmp / "stored.segapatch", [("manifest.json", zipfile.ZipFile(good).read("manifest.json")), ("game.bps", zipfile.ZipFile(good).read("game.bps"))], method=zipfile.ZIP_STORED)
check(run("apply", tmp / "stored.segapatch", "--rom", tmp / "src.bin", "--out", tmp / "stored.out")[0] == 0, "stored entries accepted")

# ---- ROM identity ----
close = bytearray(SRC); close[0x123] ^= 1
expect_refused(good, bytes(close), "one-bit different ROM")
expect_refused(good, SRC[:-1], "truncated ROM")
expect_refused(good, SRC + b"\0", "extended ROM")

# ---- manifest ----
man = json.loads(zipfile.ZipFile(good).read("manifest.json"))
man = json.loads(json.dumps(man))
bps = zipfile.ZipFile(good).read("game.bps")


def pack(manifest_obj=None, payload=bps, manifest_raw=None, name="mod.segapatch", extra=()):
    raw = manifest_raw if manifest_raw is not None else json.dumps(manifest_obj).encode()
    p = tmp / name
    sp.write_zip(p, [("manifest.json", raw), ("game.bps", payload), *extra])
    return p


def mutated(**kw):
    m = json.loads(json.dumps(man))
    for k, v in kw.items():
        if v is None:
            m.pop(k)
        elif isinstance(v, dict) and k == "rom":
            m["payloads"]["rom"].update(v)
        else:
            m[k] = v
    return m


expect_refused(pack(mutated(schema_version=2)), SRC, "unsupported schema version")
expect_refused(pack(mutated(schema_version="1")), SRC, "schema version as string")
expect_refused(pack(mutated(format="other")), SRC, "wrong format")
expect_refused(pack(mutated(platform="saturn")), SRC, "unsupported platform")
expect_refused(pack(mutated(purpose="cheats")), SRC, "unsupported purpose")
expect_refused(pack(mutated(source_sha256="ab" * 31)), SRC, "short source digest")
expect_refused(pack(mutated(source_sha256=("AB" * 32))), SRC, "uppercase digest")
expect_refused(pack(mutated(id="Bad Id")), SRC, "invalid id")
expect_refused(pack(mutated(name="x" * 200)), SRC, "oversized name")
expect_refused(pack(mutated(extra_field="x")), SRC, "unknown manifest field")
expect_refused(pack(mutated(version=None)), SRC, "missing field")
expect_refused(pack(mutated(rom={"format": "ips"})), SRC, "unsupported payload format")
expect_refused(pack(mutated(rom={"file": "../game.bps"})), SRC, "traversal payload name")
expect_refused(pack(mutated(rom={"file": "manifest.json"})), SRC, "payload names the manifest")
expect_refused(pack(mutated(rom={"sha256": "00" * 32})), SRC, "payload digest mismatch")
expect_refused(pack(manifest_raw=b"{not json"), SRC, "invalid JSON")
dup = json.dumps(man).replace('"id"', '"id": "a", "id"', 1).encode()
expect_refused(pack(manifest_raw=dup), SRC, "duplicate manifest key")
expect_refused(pack(manifest_raw=b"\xef\xbb\xbf" + json.dumps(man).encode()), SRC, "BOM / non-ASCII manifest")
expect_refused(pack(mutated(requires=["frame-hooks"])), SRC, "unsupported required capability")
expect_refused(pack(mutated(requires="x")), SRC, "requires is not a list")
expect_refused(pack(mutated(widescreen_width_max=101)), SRC, "width range above 100")
expect_refused(pack(mutated(widescreen_width_max=-1)), SRC, "negative width range")
expect_refused(pack(mutated(payloads={"widescreen": {"file": "widescreen.json"}})), SRC, "native descriptor payload not supported by this release")
m_both = mutated(); m_both["payloads"]["widescreen"] = {"file": "widescreen.json"}
expect_refused(pack(m_both, extra=[("widescreen.json", b"{}")]), SRC, "unsupported payload type next to a valid rom payload")
expect_refused(pack(mutated(payloads={})), SRC, "no payload at all")
width_ok = tmp / "width.segapatch"
sp.create(SRC, TGT, width_ok, id_="synthetic-wide", name="Synthetic Widescreen", version="1.0.0", width_max=60)
code, doc = run("inspect", width_ok)
check(code == 0 and doc["widescreen_width_max"] == 60 and doc["requires"] == [], f"width range reported {doc}")
check(run("inspect", good)[1]["widescreen_width_max"] == -1, "unstated width range reported as -1")
# typed presentation defaults (closed names; never a free-form override language)
pd_ok = tmp / "pd.segapatch"
sp.create(SRC, TGT, pd_ok, id_="synthetic-wide", name="Synthetic Widescreen", version="1.0.0", presentation_defaults={"widescreen": "enhanced_gameplay", "side_style": "raw"})
code, doc = run("inspect", pd_ok)
check(code == 0 and doc["default_widescreen"] == "enhanced_gameplay" and doc["default_side_style"] == "raw", f"presentation defaults parsed {doc}")
check(run("apply", pd_ok, "--rom", tmp / "src.bin", "--out", tmp / "pd.out")[0] == 0 and (tmp / "pd.out").read_bytes() == TGT, "a package with defaults still applies")
check(run("inspect", good)[1]["default_widescreen"] == "" and run("inspect", good)[1]["default_side_style"] == "", "no defaults stated -> empty")
only_ws = tmp / "pd2.segapatch"
sp.create(SRC, TGT, only_ws, id_="synthetic-wide", name="Synthetic Widescreen", version="1.0.0", presentation_defaults={"widescreen": "extended_backgrounds"})
check(run("inspect", only_ws)[1]["default_widescreen"] == "extended_backgrounds" and run("inspect", only_ws)[1]["default_side_style"] == "", "one default alone is accepted")
check(run("inspect", pd_ok)[1]["package_identity"] != run("inspect", good)[1]["package_identity"], "the defaults are part of the package identity")
for what, value in (("unknown widescreen name", {"widescreen": "turbo"}), ("unknown side style", {"side_style": "sparkly"}), ("unknown key", {"widescreen": "raw", "width": 5}),
                    ("empty object", {}), ("not an object", "enhanced_gameplay"), ("number", {"widescreen": 3}), ("list", ["raw"]), ("wrong case", {"side_style": "Raw"}),
                    ("nested", {"widescreen": {"mode": "enhanced_gameplay"}}), ("widescreen name given as side style", {"side_style": "enhanced_gameplay"})):
    expect_refused(pack(mutated(presentation_defaults=value)), SRC, "presentation_defaults: " + what)
bad_target = mutated(rom={"target_sha256": "11" * 32})
expect_refused(pack(bad_target), SRC, "target digest mismatch")
corrupt = bytearray(bps); corrupt[len(corrupt) // 2] ^= 0xFF
m2 = mutated(rom={"sha256": sha(bytes(corrupt))})
expect_refused(pack(m2, bytes(corrupt)), SRC, "payload corrupted (BPS patch checksum)")
expect_refused(pack(mutated(), bytes(corrupt)), SRC, "payload corrupted (manifest digest)")

# ---- ZIP structure ----
def raw_zip(entries, *, symlink=False):
    p = tmp / "raw.segapatch"
    with zipfile.ZipFile(p, "w") as z:
        for name, data in entries:
            zi = zipfile.ZipInfo(name, sp.FIXED_TIME)
            zi.create_system = 3
            zi.external_attr = (0o120777 if symlink and name == "game.bps" else 0o100644) << 16
            z.writestr(zi, data)
    return p


mj = json.dumps(man).encode()
expect_refused(raw_zip([("manifest.json", mj), ("game.bps", bps), ("extra.txt", b"x")]), SRC, "unexpected extra entry")
expect_refused(raw_zip([("manifest.json", mj)]), SRC, "missing payload")
expect_refused(raw_zip([("../manifest.json", mj), ("game.bps", bps)]), SRC, "traversal entry name")
expect_refused(raw_zip([("/manifest.json", mj), ("game.bps", bps)]), SRC, "absolute entry name")
expect_refused(raw_zip([("C:\\manifest.json", mj), ("game.bps", bps)]), SRC, "drive-letter entry name")
expect_refused(raw_zip([("dir/manifest.json", mj), ("game.bps", bps)]), SRC, "nested entry name")
expect_refused(raw_zip([("manifest.json", mj), ("game.bps", bps)], symlink=True), SRC, "symlink entry")
import warnings
with warnings.catch_warnings():
    warnings.simplefilter("ignore")
    expect_refused(raw_zip([("manifest.json", mj), ("game.bps", bps), ("game.bps", bps)]), SRC, "duplicate entry")
junk = tmp / "junk.segapatch"; junk.write_bytes(b"not a zip at all" * 8)
expect_refused(junk, SRC, "not an archive")
empty = tmp / "empty.segapatch"; empty.write_bytes(b"")
expect_refused(empty, SRC, "empty file")
trunc = tmp / "trunc.segapatch"; trunc.write_bytes(good.read_bytes()[:-30])
expect_refused(trunc, SRC, "truncated archive")
flip = bytearray(good.read_bytes()); flip[60] ^= 0x55
(tmp / "flip.segapatch").write_bytes(flip)
expect_refused(tmp / "flip.segapatch", SRC, "bit flip in archive data")
trailing = tmp / "trail.segapatch"; trailing.write_bytes(good.read_bytes() + b"\0")
expect_refused(trailing, SRC, "trailing byte after archive")

# decompression bomb: tiny deflate stream that expands far past the declared size
def patched_usize(path, fake_usize):
    data = bytearray(path.read_bytes())
    # rewrite the uncompressed size in the central directory entry of game.bps and its local header
    out = bytearray(data)
    pos = 0
    while True:
        i = out.find(b"PK\x01\x02", pos)
        if i < 0:
            break
        nlen = struct.unpack_from("<H", out, i + 28)[0]
        name = bytes(out[i + 46:i + 46 + nlen])
        if name == b"game.bps":
            struct.pack_into("<I", out, i + 24, fake_usize)
        pos = i + 4
    return bytes(out)


big = bytes(5_000_000)  # compresses to a few KiB
bomb_payload = big
bomb = tmp / "bomb.segapatch"
sp.write_zip(bomb, [("manifest.json", mj), ("game.bps", bomb_payload)])
bomb.write_bytes(patched_usize(bomb, 1000))
expect_refused(bomb, SRC, "decompression bomb (expands past declared size)")
huge = tmp / "huge.segapatch"
sp.write_zip(huge, [("manifest.json", mj), ("game.bps", bytes(20 * 1024 * 1024))])
expect_refused(huge, SRC, "oversized payload entry")
bigman = tmp / "bigman.segapatch"
sp.write_zip(bigman, [("manifest.json", b" " * 40000 + mj), ("game.bps", bps)])
expect_refused(bigman, SRC, "oversized manifest entry")

# ---- BPS (hand assembled, all four actions; independent of the encoder) ----
def num(n):
    return sp._num(n)


def bps_file(source, target, commands, metadata=b"", fix=None):
    body = b"BPS1" + num(len(source)) + num(len(target)) + num(len(metadata)) + metadata + commands
    body += struct.pack("<II", zlib.crc32(source), zlib.crc32(target))
    body += struct.pack("<I", zlib.crc32(body))
    return body


def package_with(source, target, payload, *, name):
    m = {"format": "segarecomp-patch", "schema_version": 1, "id": "hand", "version": "1", "name": "hand", "platform": "genesis",
         "purpose": "widescreen", "source_sha256": sha(source),
         "payloads": {"rom": {"format": "bps", "file": "game.bps", "sha256": sha(payload), "target_sha256": sha(target)}}}
    return pack(m, payload, name=name)


hs = bytes(range(16))
# target: SourceRead 4 | TargetRead "ZZ" | SourceCopy 3 bytes from offset 10 | TargetCopy (overlap) 6 bytes from target offset 4
ht = hs[:4] + b"ZZ" + hs[10:13] + b""
tc = bytearray(ht)
# TargetCopy from output offset 4: reads "ZZ" + src[10:13] + (overlap) ... build expected byte-by-byte
for k in range(6):
    tc.append(tc[4 + k])
ht = bytes(tc)
cmds = (num(((4 - 1) << 2) | 0) + num(((2 - 1) << 2) | 1) + b"ZZ" +
        num(((3 - 1) << 2) | 2) + num((10 << 1) | 0) + num(((6 - 1) << 2) | 3) + num((4 << 1) | 0))
good_hand = bps_file(hs, ht, cmds)
r = package_with(hs, ht, good_hand, name="hand_good.segapatch")
(tmp / "hs.bin").write_bytes(hs)
code, doc = run("apply", r, "--rom", tmp / "hs.bin", "--out", tmp / "hand.out")
check(code == 0 and (tmp / "hand.out").read_bytes() == ht, f"hand-assembled BPS (all four actions) applies: {doc}")
check(sp.bps_apply(good_hand, hs) == ht, "python reference agrees on the hand-assembled BPS")

def refused_bps(source, target, payload, what):
    expect_refused(package_with(source, target, payload, name="hand_bad.segapatch"), source, what, rom_name="hs2.bin")

refused_bps(hs, ht, good_hand[:-6], "truncated BPS (checksum footer)")
refused_bps(hs, ht, bps_file(hs, ht, cmds[:-3]), "truncated BPS command stream")
refused_bps(hs, ht, bps_file(hs, ht, cmds + num(0)), "trailing BPS command")
badcrc = bytearray(good_hand); badcrc[-5] ^= 1
refused_bps(hs, ht, bytes(badcrc), "BPS target CRC wrong")
srccrc = bytearray(good_hand); srccrc[-12] ^= 1
srccrc[-4:] = struct.pack("<I", zlib.crc32(bytes(srccrc[:-4])))
refused_bps(hs, ht, bytes(srccrc), "BPS source CRC wrong (patch CRC repaired)")
tgtcrc = bytearray(good_hand); tgtcrc[-8] ^= 1
tgtcrc[-4:] = struct.pack("<I", zlib.crc32(bytes(tgtcrc[:-4])))
refused_bps(hs, ht, bytes(tgtcrc), "BPS target CRC wrong (patch CRC repaired)")
refused_bps(hs, ht, bps_file(hs[:8], ht, cmds), "BPS declared source size mismatch")
bad_copy = (num(((4 - 1) << 2) | 0) + num(((2 - 1) << 2) | 1) + b"ZZ" + num(((3 - 1) << 2) | 2) + num((14 << 1) | 0) + num(((6 - 1) << 2) | 3) + num((4 << 1) | 0))
refused_bps(hs, ht, bps_file(hs, ht, bad_copy), "BPS SourceCopy past the source")
neg_copy = (num(((4 - 1) << 2) | 0) + num(((3 - 1) << 2) | 2) + num((5 << 1) | 1) + num(((9 - 1) << 2) | 1) + b"123456789")
refused_bps(hs, bytes(16)[:16], bps_file(hs, bytes(16), neg_copy), "BPS negative copy offset before the source start")
fwd_target = num(((4 - 1) << 2) | 0) + num(((3 - 1) << 2) | 3) + num((9 << 1) | 0) + num(((9 - 1) << 2) | 1) + b"123456789"
refused_bps(hs, bytes(16), bps_file(hs, bytes(16), fwd_target), "BPS TargetCopy of output that does not exist yet")
over = num(((20 - 1) << 2) | 0)
refused_bps(hs, ht, bps_file(hs, ht, over), "BPS command past the target size")
refused_bps(hs, ht, b"IPS1" + good_hand[4:], "BPS magic wrong")
huge_target = b"BPS1" + num(len(hs)) + num(1 << 40) + num(0)
huge_target += struct.pack("<II", zlib.crc32(hs), 0)
huge_target += struct.pack("<I", zlib.crc32(huge_target))
refused_bps(hs, ht, huge_target, "BPS oversized target (allocation refused)")

# ---- bounded mutation fuzz: a flipped byte anywhere in a valid package is either refused or (never) accepted with a different image ----
import random
rng = random.Random(122)
data = good.read_bytes()
fuzz_ok = fuzz_refused = 0
for n in range(400):
    mutated_bytes = bytearray(data)
    for _ in range(rng.choice((1, 1, 2, 3))):
        mutated_bytes[rng.randrange(len(mutated_bytes))] ^= 1 << rng.randrange(8)
    if rng.random() < 0.1:
        del mutated_bytes[rng.randrange(len(mutated_bytes)):]
    fz = tmp / "fuzz.segapatch"
    fz.write_bytes(bytes(mutated_bytes))
    fout = tmp / "fuzz.out"
    fout.unlink(missing_ok=True)
    proc = subprocess.run([CLI, "patch", "apply", str(fz), "--rom", str(tmp / "src.bin"), "--out", str(fout)], capture_output=True, text=True, timeout=60)
    check(proc.returncode in (0, 1), f"fuzz {n}: the importer ended with a signal or an unexpected status {proc.returncode}")
    if proc.returncode == 0:
        fuzz_ok += 1
        check(fout.read_bytes() == TGT, f"fuzz {n}: an accepted mutated package must still produce exactly the intended image")
    else:
        fuzz_refused += 1
        check(not fout.exists(), f"fuzz {n}: a refused package left an output file")
check(fuzz_refused > 300, f"the mutations were mostly refused ({fuzz_refused} refused, {fuzz_ok} equivalent)")

# the original image is never an output
(tmp / "inplace.bin").write_bytes(SRC)
code, doc = run("apply", good, "--rom", tmp / "inplace.bin", "--out", tmp / "inplace.bin")
check(code == 1 and (tmp / "inplace.bin").read_bytes() == SRC, "apply refuses to overwrite the original image")

# ---- atomic behaviour: an existing destination is untouched by a failed attempt ----
keep = tmp / "keep.bin"; keep.write_bytes(b"previous")
code, doc = run("apply", junk, "--rom", tmp / "src.bin", "--out", keep)
check(code == 1 and keep.read_bytes() == b"previous", "failed apply leaves a previous output untouched")

print("PASS" if not failures else f"{len(failures)} failure(s)")
sys.exit(1 if failures else 0)
