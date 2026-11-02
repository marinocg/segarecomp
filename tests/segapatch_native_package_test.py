#!/usr/bin/env python3
"""SEG-124-T001 (ADR 0267): patch package schema 2 (native-only, combined BPS + native) through the product CLI seam (`segarecomp patch ...`), cross-checked against the independent
Python reference in tools/segapatch.py. Synthetic fixture image and the project-owned fixture capability (visible only with SEGARECOMP_ALLOW_FIXTURE_CAPABILITIES=1; never buildable).
Every malformed or unsupported input must be refused with a readable error and no output file.
usage: segapatch_native_package_test.py <segarecomp-cli> <tools/segapatch.py>"""
import hashlib
import importlib.util
import json
import os
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
CAP = "genesis.fixture.native-widescreen.v1"
SONIC = "genesis.sonic1.native-widescreen.v1"
failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL:", what)


def run(*args, fixtures=True):
    env = dict(os.environ)
    env.pop("SEGARECOMP_ALLOW_FIXTURE_CAPABILITIES", None)
    if fixtures:
        env["SEGARECOMP_ALLOW_FIXTURE_CAPABILITIES"] = "1"
    p = subprocess.run([CLI, "patch", *map(str, args)], capture_output=True, text=True, timeout=60, env=env)
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
TGT = bytes(TGT)
sha = lambda b: hashlib.sha256(b).hexdigest()
tmp = Path(tempfile.mkdtemp(prefix="segapatch-native-test-"))
(tmp / "src.bin").write_bytes(SRC)


def refused(pkg, what, rom_bytes=SRC, fixtures=True, package_valid=False):
    r = tmp / "rom.bin"
    r.write_bytes(rom_bytes)
    out = tmp / "refused.out"
    if out.exists():
        out.unlink()
    code, doc = run("apply", pkg, "--rom", r, "--out", out, fixtures=fixtures)
    check(code == 1 and doc and doc["ok"] is False and doc.get("error"), f"{what}: refused with a readable error (code={code} doc={doc})")
    check(not out.exists() and not Path(str(out) + ".partial").exists(), f"{what}: no output or partial file")
    check(run("verify", pkg, "--rom", r, fixtures=fixtures)[0] == 1, f"{what}: verify refuses")
    check((run("inspect", pkg, fixtures=fixtures)[0] == 0) == package_valid, f"{what}: inspect {'accepts' if package_valid else 'refuses'} the package itself")


def desc(**kw):
    d = {"format": "segarecomp-native-descriptor", "schema_version": 1, "capability": CAP, "source_sha256": sha(SRC), "min_implementation": 1, "components": ["hud", "scenery"]}
    for k, v in kw.items():
        if v is None:
            d.pop(k, None)
        else:
            d[k] = v
    return json.dumps(d).encode()


def manifest(native=None, rom_payload=None, **kw):
    payloads = {}
    files = []
    if rom_payload is not None:
        payloads["rom"] = {"format": "bps", "file": "game.bps", "sha256": sha(rom_payload), "target_sha256": sha(TGT)}
        files.append(("game.bps", rom_payload))
    if native is not None:
        payloads["native"] = {"format": "descriptor", "file": "widescreen.json", "sha256": sha(native)}
        files.append(("widescreen.json", native))
    m = {"format": "segarecomp-patch", "schema_version": 2, "id": "synthetic-native", "version": "1.0.0", "name": "Synthetic Native", "platform": "genesis", "purpose": "widescreen",
         "source_sha256": sha(SRC), "payloads": payloads, "requires": [CAP]}
    for k, v in kw.items():
        if v is None:
            m.pop(k, None)
        else:
            m[k] = v
    return m, files


def pack(m, files, name="mod.segapatch"):
    p = tmp / name
    sp.write_zip(p, [("manifest.json", json.dumps(m).encode()), *files])
    return p


# ---- native-only: image unchanged, distinct identity ----
nat = tmp / "native-only.segapatch"
sp.create(SRC, None, nat, id_="synthetic-native", name="Synthetic Native", version="1.0.0", native_capability=CAP, native_components=["hud", "scenery"], width_max=100)
nat2 = tmp / "native-only2.segapatch"
sp.create(SRC, None, nat2, id_="synthetic-native", name="Synthetic Native", version="1.0.0", native_capability=CAP, native_components=["hud", "scenery"], width_max=100)
check(nat.read_bytes() == nat2.read_bytes(), "native-only authoring is deterministic")
code, doc = run("inspect", nat)
check(code == 0 and doc["ok"] and doc["schema_version"] == 2 and doc["has_native"] and not doc["has_rom"] and doc["native_capability"] == CAP and doc["native_components"] == 3
      and doc["native_implementation"] == 1 and doc["native_width_max"] == 100 and doc["widescreen_width_max"] == 100 and doc["requires"] == [CAP], f"native-only inspect {doc}")
check(doc["target_sha256"] == "" and doc["payload_sha256"] == "" and len(doc["native_descriptor_sha256"]) == 64, "native-only has no BPS digests, has the descriptor digest")
nat_id = doc["package_identity"]
code, doc = run("verify", nat, "--rom", tmp / "src.bin")
check(code == 0 and doc["ok"] and doc["rom_sha256"] == sha(SRC), f"native-only verify {doc}")
out = tmp / "native.out"
code, doc = run("apply", nat, "--rom", tmp / "src.bin", "--out", out)
check(code == 0 and out.read_bytes() == SRC, "native-only apply leaves the original bytes unchanged")
check(sp.apply_package(nat, SRC) == SRC, "the independent Python reference agrees (native-only)")
refused(nat, "native-only with another image", rom_bytes=SRC[:-1], package_valid=True)
refused(nat, "native-only with a one-bit different image", rom_bytes=bytes([SRC[0] ^ 1]) + SRC[1:], package_valid=True)
refused(nat, "native-only without the fixture switch (capability unknown to this release)", fixtures=False)

# ---- combined: BPS + native ----
both = tmp / "combined.segapatch"
sp.create(SRC, TGT, both, id_="synthetic-native", name="Synthetic Native", version="1.0.0", native_capability=CAP, native_components=["hud", "scenery"], width_max=100)
code, doc = run("inspect", both)
check(code == 0 and doc["has_native"] and doc["has_rom"] and doc["target_sha256"] == sha(TGT) and len(doc["payload_sha256"]) == 64, f"combined inspect {doc}")
both_id = doc["package_identity"]
out = tmp / "combined.out"
code, doc = run("apply", both, "--rom", tmp / "src.bin", "--out", out)
check(code == 0 and out.read_bytes() == TGT and doc["target_sha256"] == sha(TGT), "combined apply produces the patched image")
check(sp.apply_package(both, SRC) == TGT, "the independent Python reference agrees (combined)")
# BPS-only still works as schema 1 and as a schema 2 manifest without a native payload
bps_only = tmp / "bps-only.segapatch"
sp.create(SRC, TGT, bps_only, id_="synthetic-native", name="Synthetic Native", version="1.0.0")
code, doc = run("inspect", bps_only)
check(code == 0 and doc["schema_version"] == 1 and doc["has_rom"] and not doc["has_native"] and doc["native_capability"] == "", f"BPS-only inspect {doc}")
bps_id = doc["package_identity"]
check(len({nat_id, both_id, bps_id}) == 3, "native-only, combined and BPS-only packages have three distinct identities")
bps_payload = zipfile.ZipFile(bps_only).read("game.bps")
m, files = manifest(rom_payload=bps_payload, requires=None)
m["payloads"]["rom"]["target_sha256"] = sha(TGT)
v2_rom_only = pack(m, files, "v2-rom-only.segapatch")
code, doc = run("apply", v2_rom_only, "--rom", tmp / "src.bin", "--out", tmp / "v2.out")
check(code == 0 and (tmp / "v2.out").read_bytes() == TGT, "a schema 2 package with only a rom payload applies")
# the descriptor content is part of the identity
hud_only = tmp / "hud-only.segapatch"
sp.create(SRC, None, hud_only, id_="synthetic-native", name="Synthetic Native", version="1.0.0", native_capability=CAP, native_components=["hud"], width_max=100)
code, doc = run("inspect", hud_only)
check(code == 0 and doc["native_components"] == 1 and doc["package_identity"] != nat_id, "a different component set is a different package identity")
w60 = tmp / "w60.segapatch"
sp.create(SRC, None, w60, id_="synthetic-native", name="Synthetic Native", version="1.0.0", native_capability=CAP, native_components=["hud", "scenery"], width_max=60)
check(run("inspect", w60)[1]["package_identity"] != nat_id, "a different validated width is a different package identity")

# ---- schema 1 stays closed ----
m, files = manifest(native=desc())
m["schema_version"] = 1
refused(pack(m, files), "a schema 1 manifest naming a native payload")
m, files = manifest(native=desc(), schema_version=3)
refused(pack(m, files), "unsupported schema version 3")
m, files = manifest(native=desc(), schema_version="2")
refused(pack(m, files), "schema version as a string")

# ---- descriptor refusals ----
for what, d in (
    ("unknown capability", desc(capability="genesis.other.native-widescreen.v1")),
    ("a real capability bound to another image", desc(capability=SONIC)),
    ("descriptor for another source image", desc(source_sha256="11" * 32)),
    ("min_implementation newer than this release", desc(min_implementation=2)),
    ("min_implementation zero", desc(min_implementation=0)),
    ("min_implementation not a number", desc(min_implementation="1")),
    ("empty components", desc(components=[])),
    ("unknown component", desc(components=["hud", "audio"])),
    ("repeated component", desc(components=["hud", "hud"])),
    ("components not a list", desc(components="hud")),
    ("too many components", desc(components=["hud", "scenery", "hud"])),
    ("width_max above 100", desc(width_max=101)),
    ("width_max negative text", desc(width_max="100")),
    ("unknown field", desc(address_map={"x": 1})),
    ("missing capability", desc(capability=None)),
    ("missing components", desc(components=None)),
    ("wrong format", desc(format="other")),
    ("wrong descriptor schema", desc(schema_version=2)),
    ("malformed JSON", b'{"format": '),
    ("not an object", b"[1,2,3]"),
    ("expression-like value", desc(capability="1+1")),
):
    m, files = manifest(native=d)
    refused(pack(m, files), "descriptor: " + what)
m, files = manifest(native=desc(width_max=40), widescreen_width_max=60)
refused(pack(m, files), "manifest and descriptor disagree about the validated width")
m, files = manifest(native=b" " * 5000 + desc())
refused(pack(m, files), "oversized descriptor")
dup = desc()
m, files = manifest(native=dup)
m["payloads"]["native"]["sha256"] = "22" * 32
refused(pack(m, files), "descriptor digest mismatch")
m, files = manifest(native=desc(), requires=None)
refused(pack(m, files), "native payload without the requires entry")
m, files = manifest(native=desc(), requires=[CAP, CAP])
refused(pack(m, files), "requires repeats the capability")
m, files = manifest(native=desc(), requires=[SONIC])
refused(pack(m, files), "requires names another capability than the descriptor")
m, files = manifest(native=desc(), requires=["frame-hooks"])
refused(pack(m, files), "requires an unknown capability")
m, files = manifest(rom_payload=bps_payload)
refused(pack(m, files), "requires without a native payload")
m, files = manifest(native=desc())
m["payloads"]["native"]["format"] = "script"
refused(pack(m, files), "native payload format is not a descriptor")
m, files = manifest(native=desc())
m["payloads"]["native"]["file"] = "game.bps"
refused(pack(m, files), "native file name collides")
m, files = manifest(native=desc())
m["payloads"]["native"]["file"] = "manifest.json"
refused(pack(m, files), "native file named manifest.json")
m, files = manifest(native=desc())
m["payloads"]["native"]["extra"] = 1
refused(pack(m, files), "native payload with an unknown key")
m, files = manifest(native=desc())
m["payloads"]["plugin"] = {"format": "so", "file": "x.so", "sha256": "00" * 32}
refused(pack(m, files + [("x.so", b"\x7fELF")]), "an executable payload type")
m, files = manifest()
refused(pack(m, files), "no payload at all")
m, files = manifest(native=desc())
refused(pack(m, files + [("extra.txt", b"x")]), "unexpected extra entry")
m, files = manifest(native=desc())
p = tmp / "missing.segapatch"
sp.write_zip(p, [("manifest.json", json.dumps(m).encode())])
refused(p, "missing descriptor entry")

# ---- release-visible capability list is exactly the audited one (the fixture is hidden) ----
m, files = manifest(native=desc())
fx = pack(m, files, "fixture.segapatch")
refused(fx, "a fixture capability without the test switch", fixtures=False)

print("failures:", len(failures))
sys.exit(1 if failures else 0)
