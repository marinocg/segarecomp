#!/usr/bin/env python3
"""SEG-135-T001: the `.segapatch` schema 3 reader through the product CLI (`segarecomp mod inspect <package>`), cross-checked against an independent Python builder.
Every ADR 0281 rejection class has a negative test, every worked example and test vector has a positive test, schema 1/2 packages keep their pinned identities, and
archive bounds (traversal, duplicates, oversize, zip-bomb ratio) are enforced. Synthetic data only.
usage: mod_package_test.py <segarecomp-cli> <tools/segapatch.py>"""
import copy
import hashlib
import importlib.util
import io
import json
import struct
import sys
import zipfile

from mod_test_support import *  # noqa: F401,F403

cli = Cli(sys.argv[1])
spec = importlib.util.spec_from_file_location("segapatch", sys.argv[2])
sp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sp)
tmp = temp()


def inspect(zip_bytes, name="p.segapatch"):
    path = write(tmp / name, zip_bytes)
    return cli.run("mod", "inspect", path)[:2]


def accept(what, zip_bytes):
    rc, doc = inspect(zip_bytes)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: accepted ({doc})")
    return doc


def reject(what, zip_bytes, code):
    rc, doc = inspect(zip_bytes)
    cli.check(rc == 1 and doc and doc.get("ok") is False and doc.get("class") == code, f"{what}: expected {code}, got {doc and doc.get('class')} ({doc and doc.get('error')})")


def good(platform="genesis", **extra):
    lp = luau()
    return base_manifest(platform, [lp], **extra), [lp]


# ---------------------------------------------------------------- positive: worked examples and vectors
nat = native()
lau = luau()
# Vector A: content digest (schema 3 section 12) -- luau + native payloads, genesis; the same text with master-system differs.
m = base_manifest("genesis", [lau, nat], head={"author": "A"}, abi={"native": 2})
m_order = {k: m[k] for k in ("format", "schema_version", "id", "version", "name", "platform", "author", "source", "abi", "payloads")}
doc = accept("vector A (genesis)", build(m_order, [lau, nat]))
cli.check(doc["content_digest"] == "15668889b22e0bcc650c597871b0819dc5a35ac3cabbe063acaf43dc0e03f0ef", "vector A content digest is pinned")
cli.check(doc["package_identity"] == sha(canon(m_order)), "package identity = SHA-256 of the exact manifest bytes")
cli.check(doc["trust_class"] == "native" and doc["abi_native"] == 2, "a native payload makes the package native (abi 2)")
sms = copy.deepcopy(m_order)
sms["platform"] = "master-system"
sms["source"]["revisions"][0]["mapper"] = "sega"
doc = accept("vector A (master-system)", build(sms, [lau, nat]))
cli.check(doc["content_digest"] == "25fdf0085aa085f112923452bb836f1a7c600c71cc3628a6d1a4cfb614c51d6f", "platform changes the content digest (pinned)")

# Vector B: the syntactic identity vector is rejected E_TRUST_UNDECLARED (native payload without author/abi).
vector_b = (b'{"format":"segarecomp-patch","schema_version":3,"id":"example-mod","version":"1.0.0","name":"Example","platform":"genesis","source":{"kind":"cartridge-image","revisions":'
            b'[{"label":"rev-a","sha256":"a86d60429e75e5f792f5dcc5436ade92d21d19f9979a5114b11e437a13b7d1dd"}]},"capabilities":["host.log"],"payloads":[{"kind":"luau-source","kind_version":1,'
            b'"path":"scripts/main.luau","size":8,"sha256":"a49bb5fe7c01bb8575e59200abef7b4c873292154b8434616942764a0d76763c"},{"kind":"native-c11","kind_version":1,"path":"src/main.c","size":7,'
            b'"sha256":"7c725f30854a46033dd94f728ac6b08caf10845993cd3ed48e40079cdb0a76a6"}]}')
cli.check(sha(vector_b) == "1c2ad1354fe66a47ee00336af9b82375859025b8552c22187e24bcf1e24aec30", "vector B bytes hash to the documented identity")
reject("vector B", build(None, [lau, nat], raw_manifest=vector_b), "E_TRUST_UNDECLARED")

# Worked example: schema 1 patch re-authored as schema 3 (BPS only), genesis.
S = bytes(range(256)) * 16
T = bytearray(S)
T[10:14] = b"\xde\xad\xbe\xef"
bps = sp.bps_create(S, bytes(T))
bm = {"format": "segarecomp-patch", "schema_version": 3, "id": "x", "version": "1.0.0", "name": "X", "platform": "genesis",
      "source": {"kind": "cartridge-image", "revisions": [{"label": "rev-a", "sha256": sha(S)}]},
      "payloads": [payload("bps-image", "game.bps", bps, target_sha256=sha(bytes(T)))]}
doc = accept("schema 3 BPS-only", build(bm, [(None, ("game.bps", bps))]))
cli.check(doc["trust_class"] == "data", "a BPS-only package is data")
cli.check(doc["package_identity"] == sha(canon(bm)), "BPS-only identity")
# determinism: the same input yields the identical report; reserved kinds do not change how existing kinds parse
cli.check(inspect(build(bm, [(None, ("game.bps", bps))]))[1] == doc, "two reads of the same package give the identical report")

# Worked example: multi-revision, native + capability + state, optional capabilities, presentation defaults, empty reserved keys
caps = base_manifest("genesis", [nat], head={"author": "A"}, abi={"native": 2}, capabilities=["frame.vdp", "host.log", "state.mod"], capabilities_optional=["input.read"], state_schema=1,
                     hooks=[], geometry={"scenes": []}, presentation_defaults={"widescreen": "original", "side_style": "raw"})
caps["source"]["revisions"].append({"label": "rev-b", "sha256": sha(b"other-image")})
order = ["format", "schema_version", "id", "version", "name", "platform", "author", "source", "capabilities", "capabilities_optional", "abi", "state_schema", "hooks", "geometry", "presentation_defaults", "payloads"]
caps = {k: caps[k] for k in order if k in caps}
caps["capabilities"] = ["frame.vdp", "hook.function", "host.log", "state.mod"]   # hook.function declared: `hooks` then has its capability (unsupported on the platform is a check-time concern)
accept("capabilities + reserved empty keys", build(caps, [nat]))

# ---------------------------------------------------------------- schema 1/2 regression: pinned identities (byte-for-byte unaffected)
def rom(n=0x4000, seed=7):
    out = bytearray()
    x = seed
    while len(out) < n:
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out += struct.pack("<I", x)
    return bytes(out[:n])


s_src = rom()
s_tgt = bytearray(s_src)
s_tgt[0x100:0x108] = b"PATCHED!"
p1 = tmp / "golden1.segapatch"
sp.create(s_src, bytes(s_tgt), p1, id_="golden-one", name="Golden One", version="1.0.0")
rc, d, _ = cli.run("patch", "inspect", p1)
cli.check(rc == 0 and d["schema_version"] == 1 and d["package_identity"] == "cbbbb4e40a7a18c3ea997d324a5c16d9aebd5a61afa250c444cf1e699d62fe2a", f"schema 1 golden identity is pinned ({d})")
p2 = tmp / "golden2.segapatch"
sp.create(s_src, None, p2, id_="golden-two", name="Golden Two", version="1.0.0", native_source=b"int provider_marker;\n", native_caps=["frame.work_ram", "scenery.words"], author="Tester")
rc, d, _ = cli.run("patch", "inspect", p2)
cli.check(rc == 0 and d["schema_version"] == 2 and d["package_identity"] == "78086cb2cbe1f158a4b938769bd7bf7b82cea6b6f002bde59418eee5677184f7", f"schema 2 golden identity is pinned ({d})")
cli.check(d["native_abi"] == 1 and d["native_capabilities"] == 5, "schema 2 stays ABI 1 with its capability mask")
# a schema 1/2 reader meets schema 3 and says so; a schema 3 reader meets schema 1/2 and refuses it
rc, d, _ = cli.run("patch", "inspect", write(tmp / "s3.segapatch", build(bm, [(None, ("game.bps", bps))])))
cli.check(rc == 1 and d["ok"] is False, f"the schema 1/2 reader refuses schema 3 ({d})")
rc, d, _ = cli.run("mod", "inspect", p1)
cli.check(rc == 1 and d["ok"] is False and d["class"].startswith("E_"), f"the schema 3 reader does not accept a schema 1 package ({d})")

# ---------------------------------------------------------------- negative: every rejection class
def mutate(fn, platform="genesis", payloads=None, **extra):
    manifest, files = good(platform, **extra)
    if payloads is not None:
        files = payloads
        manifest["payloads"] = [p for p, _ in payloads]
    fn(manifest)
    return build(manifest, files)


reject("duplicate key", build(None, [lau], raw_manifest=canon(good()[0]).replace(b'"id":"example-mod"', b'"id":"example-mod","id":"x"', 1)), "E_MANIFEST_SYNTAX")
reject("insignificant whitespace", build(None, [lau], raw_manifest=json.dumps(good()[0], indent=1).encode()), "E_MANIFEST_SYNTAX")
reject("not json", build(None, [lau], raw_manifest=b"{not json"), "E_MANIFEST_SYNTAX")
reject("oversized manifest", build(None, [lau], raw_manifest=b"x" * 70000), "E_MANIFEST_SYNTAX")
reject("boolean value", build(None, [lau], raw_manifest=canon(good()[0]).replace(b'"version":"1.0.0"', b'"version":true')), "E_MANIFEST_SYNTAX")
reject("escape other than quote and backslash", build(None, [lau], raw_manifest=canon(good()[0]).replace(b'"Example"', b'"Exa\\nmple"')), "E_MANIFEST_SYNTAX")
reject("schema_version 2", mutate(lambda m: m.__setitem__("schema_version", 2)), "E_SCHEMA_VERSION")
reject("requires in schema 3", mutate(lambda m: m.__setitem__("requires", [])), "E_FIELD_UNKNOWN")
reject("widescreen_width_max in schema 3", mutate(lambda m: m.__setitem__("widescreen_width_max", 10)), "E_FIELD_UNKNOWN")
reject("unknown top-level field", mutate(lambda m: m.__setitem__("shell", "x")), "E_FIELD_UNKNOWN")
reject("missing field", mutate(lambda m: m.pop("name")), "E_FIELD_MISSING")
reject("bad id", mutate(lambda m: m.__setitem__("id", "Bad_ID")), "E_FIELD_VALUE")
reject("bad version", mutate(lambda m: m.__setitem__("version", "1.0")), "E_FIELD_VALUE")
reject("leading-zero version", mutate(lambda m: m.__setitem__("version", "01.0.0")), "E_FIELD_VALUE")
reject("platform saturn", mutate(lambda m: m.__setitem__("platform", "saturn")), "E_PLATFORM_UNKNOWN")
reject("platform case variation", mutate(lambda m: m.__setitem__("platform", "Genesis")), "E_PLATFORM_UNKNOWN")
reject("disc-image on genesis", mutate(lambda m: m.__setitem__("source", {"kind": "disc-image", "disc_identity": "0" * 64})), "E_SOURCE_PLATFORM")
reject("cartridge-image on sega-cd", mutate(lambda m: None, platform="sega-cd"), "E_SOURCE_PLATFORM")
reject("disc-image on sega-cd", mutate(lambda m: m.__setitem__("source", {"kind": "disc-image", "disc_identity": "0" * 64}), platform="sega-cd"), "E_SOURCE_KIND_RESERVED")
reject("unknown source kind", mutate(lambda m: m.__setitem__("source", {"kind": "tape", "revisions": []})), "E_FIELD_VALUE")
reject("source with zero revisions", mutate(lambda m: m["source"].__setitem__("revisions", [])), "E_SOURCE_DIGEST")
reject("duplicate revision digest", mutate(lambda m: m["source"]["revisions"].append({"label": "rev-b", "sha256": SRC_SHA})), "E_SOURCE_DIGEST")
reject("duplicate revision label", mutate(lambda m: m["source"]["revisions"].append({"label": "rev-a", "sha256": sha(b"z")})), "E_SOURCE_DIGEST")
reject("17 revisions", mutate(lambda m: m["source"].__setitem__("revisions", [{"label": f"r{i}", "sha256": sha(bytes([i]))} for i in range(17)])), "E_SOURCE_DIGEST")
reject("revision digest not hex", mutate(lambda m: m["source"]["revisions"][0].__setitem__("sha256", "Z" * 64)), "E_SOURCE_DIGEST")
reject("mapper on genesis", mutate(lambda m: m["source"]["revisions"][0].__setitem__("mapper", "sega")), "E_FIELD_UNKNOWN")
reject("master-system without mapper", mutate(lambda m: m["source"]["revisions"][0].pop("mapper"), platform="master-system"), "E_FIELD_MISSING")
reject("master-system with inferred mapper", mutate(lambda m: m["source"]["revisions"][0].__setitem__("mapper", "auto"), platform="master-system"), "E_FIELD_VALUE")
reject("bios object on a cartridge source", mutate(lambda m: m.__setitem__("bios", {"requirement": "class", "class": "us"})), "E_FIELD_UNKNOWN")
reject("unsorted capabilities", mutate(lambda m: m.__setitem__("capabilities", ["host.log", "frame.vdp"])), "E_FIELD_VALUE")
reject("duplicate capabilities", mutate(lambda m: m.__setitem__("capabilities", ["host.log", "host.log"])), "E_FIELD_VALUE")
reject("unknown capability", mutate(lambda m: m.__setitem__("capabilities", ["net.connect"])), "E_CAPABILITY_UNKNOWN")
reject("unknown memory space capability", mutate(lambda m: m.__setitem__("capabilities", ["memory.read.vram"])), "E_CAPABILITY_UNKNOWN")
reject("optional repeats required", mutate(lambda m: m.update(capabilities=["host.log"], capabilities_optional=["host.log"])), "E_FIELD_VALUE")
reject("capability on a data-only package", mutate(lambda m: m.__setitem__("capabilities", ["host.log"]), payloads=[(payload("bps-image", "g.bps", bps, target_sha256=sha(b"t")), ("g.bps", bps))]), "E_CAPABILITY_INCONSISTENT")
reject("scenery.words needs native", mutate(lambda m: m.__setitem__("capabilities", ["scenery.words"])), "E_CAPABILITY_INCONSISTENT")
reject("hooks without capability", mutate(lambda m: m.__setitem__("hooks", [])), "E_CAPABILITY_INCONSISTENT")
reject("hooks entry is not defined", mutate(lambda m: m.update(capabilities=["hook.function"], hooks=[{"site": "x"}])), "E_FIELD_UNKNOWN")
reject("geometry content is not defined", mutate(lambda m: m.__setitem__("geometry", {"scenes": [{"id": "x"}]})), "E_FIELD_UNKNOWN")
reject("geometry other key", mutate(lambda m: m.__setitem__("geometry", {"x": 1})), "E_FIELD_UNKNOWN")
reject("state.mod without state_schema", mutate(lambda m: m.__setitem__("capabilities", ["state.mod"])), "E_FIELD_MISSING")
reject("state_schema without state.mod", mutate(lambda m: m.__setitem__("state_schema", 1)), "E_FIELD_UNKNOWN")
reject("limits above policy (memory)", mutate(lambda m: m.__setitem__("limits", {"luau_memory_bytes": 4194305})), "E_LIMIT_EXCEEDS_POLICY")
reject("limits above policy (budget)", mutate(lambda m: m.__setitem__("limits", {"luau_budget_units": 100001})), "E_LIMIT_EXCEEDS_POLICY")
reject("unknown limit key", mutate(lambda m: m.__setitem__("limits", {"native_memory": 1})), "E_FIELD_UNKNOWN")
accept("limits at policy", mutate(lambda m: m.__setitem__("limits", {"luau_memory_bytes": 4194304, "luau_budget_units": 100000})))
reject("unknown presentation default", mutate(lambda m: m.__setitem__("presentation_defaults", {"widescreen": "ultra"})), "E_FIELD_VALUE")

n_good = native()
reject("native without author/abi", build(None, [lau, nat], raw_manifest=vector_b), "E_TRUST_UNDECLARED")
reject("native with abi 1", mutate(lambda m: m.update(author="A", abi={"native": 1}), payloads=[n_good]), "E_FIELD_VALUE")
reject("native with abi 3", mutate(lambda m: m.update(author="A", abi={"native": 3}), payloads=[n_good]), "E_ABI_UNSUPPORTED")
reject("abi without native", mutate(lambda m: m.__setitem__("abi", {"native": 2})), "E_FIELD_UNKNOWN")
reject("native kind_version 2", mutate(lambda m: (m["payloads"][0].__setitem__("kind_version", 2), m.update(author="A", abi={"native": 2})), payloads=[n_good]), "E_PAYLOAD_VERSION_UNKNOWN")
for kind in ("disc-extent-edit", "disc-track-replace", "guest-asm"):
    d = b"reserved"
    reject(f"reserved kind {kind}", mutate(lambda m: None, payloads=[(payload(kind, "x/r.bin", d), ("x/r.bin", d))]), "E_PAYLOAD_KIND_RESERVED")
reject("reserved kind with an unknown version", mutate(lambda m: None, payloads=[(dict(payload("disc-extent-edit", "x/r.bin", b"r"), kind_version=9), ("x/r.bin", b"r"))]), "E_PAYLOAD_VERSION_UNKNOWN")
reject("unknown kind", mutate(lambda m: None, payloads=[(payload("firmware", "x/f.bin", b"r"), ("x/f.bin", b"r"))]), "E_PAYLOAD_KIND_UNKNOWN")
reject("bios payload", mutate(lambda m: None, payloads=[(payload("bios", "bios.bin", b"r"), ("bios.bin", b"r"))]), "E_PAYLOAD_KIND_UNKNOWN")
reject("unknown payload field", mutate(lambda m: m["payloads"][0].__setitem__("flags", "-O3")), "E_FIELD_UNKNOWN")
reject("payload sha not hex", mutate(lambda m: m["payloads"][0].__setitem__("sha256", "xyz")), "E_FIELD_VALUE")
reject("payload digest mismatch", mutate(lambda m: m["payloads"][0].__setitem__("sha256", "0" * 64)), "E_PAYLOAD_DIGEST")
reject("payload size mismatch", mutate(lambda m: m["payloads"][0].__setitem__("size", 99)), "E_ARCHIVE_SIZE")
reject("payload path traversal", mutate(lambda m: m["payloads"][0].__setitem__("path", "../x.luau")), "E_ARCHIVE_PATH")
reject("payload absolute path", mutate(lambda m: m["payloads"][0].__setitem__("path", "/x.luau")), "E_ARCHIVE_PATH")
reject("payload upper-case path", mutate(lambda m: m["payloads"][0].__setitem__("path", "Scripts/Main.luau")), "E_ARCHIVE_PATH")
reject("payload deep path", mutate(lambda m: m["payloads"][0].__setitem__("path", "a/b/c/d.luau")), "E_ARCHIVE_PATH")
reject("payload dot segment", mutate(lambda m: m["payloads"][0].__setitem__("path", ".hidden/x.luau")), "E_ARCHIVE_PATH")
reject("payload backslash path", mutate(lambda m: m["payloads"][0].__setitem__("path", "a\\x.luau")), "E_ARCHIVE_PATH")
reject("payload wrong extension", mutate(lambda m: m["payloads"][0].__setitem__("path", "scripts/main.txt")), "E_FIELD_VALUE")
reject("payload role", mutate(lambda m: m["payloads"][0].__setitem__("role", "provider")), "E_FIELD_VALUE")
la = luau("a.luau", b"-- a")
reject("duplicate payload path", mutate(lambda m: m["payloads"].append(dict(m["payloads"][0])), payloads=[la]), "E_PAYLOAD_DUPLICATE")
import warnings
with warnings.catch_warnings():
    warnings.simplefilter("ignore")   # zipfile warns about the duplicate name it is asked to write
    reject("payload named manifest.json (two manifest.json entries)", mutate(lambda m: None, payloads=[(payload("asset", "manifest.json", b"x", asset_class="data", format="raw"), ("manifest.json", b"x"))]), "E_ARCHIVE_ENTRY")
bp = (payload("bps-image", "g.bps", bps, target_sha256=sha(b"t")), ("g.bps", bps))
gc = (payload("guest-code", "code.bin", b"\x4e\x71", cpu="m68k"), ("code.bin", b"\x4e\x71"))
reject("bps-image + guest-code", mutate(lambda m: None, payloads=[bp, gc]), "E_PAYLOAD_COMBINATION")
bp2 = (payload("bps-image", "h.bps", bps, target_sha256=sha(b"t")), ("h.bps", bps))
reject("two bps-image", mutate(lambda m: None, payloads=[bp, bp2]), "E_PAYLOAD_COMBINATION")
accept("rom-edit-set + guest-code", mutate(lambda m: None, payloads=[(payload("rom-edit-set", "e.bin", b"edits", edits=3), ("e.bin", b"edits")), gc]))
reject("m68k guest-code on master-system", mutate(lambda m: None, platform="master-system", payloads=[gc]), "E_PAYLOAD_PLATFORM")
reject("bps-image on sega-cd (source is judged first)", mutate(lambda m: None, platform="sega-cd", payloads=[bp]), "E_SOURCE_PLATFORM")
reject("bad guest-code cpu", mutate(lambda m: None, payloads=[(payload("guest-code", "code.bin", b"\x00", cpu="sh2"), ("code.bin", b"\x00"))]), "E_FIELD_VALUE")
reject("luau bytecode", mutate(lambda m: None, payloads=[luau("b.luau", b"\x03\x06binary\x00")]), "E_LUAU_BYTECODE")

# archive-level rules
good_m, good_f = good()
extra = build(good_m, good_f, extra_entries=[("notes.txt", b"unnamed")])
reject("entry the manifest does not name", extra, "E_ARCHIVE_ENTRY")
reject("named entry absent", build(good_m, []), "E_ARCHIVE_ENTRY")
reject("entry with traversal name", build(good_m, good_f, extra_entries=[("../evil", b"x")]), "E_ARCHIVE_PATH")
reject("entry with upper-case name", build(good_m, good_f, extra_entries=[("Evil.txt", b"x")]), "E_ARCHIVE_PATH")
reject("case-folded duplicate entries", make_zip([("manifest.json", canon(good_m)), ("scripts/main.luau", b"-- hello"), ("scripts/MAIN.luau", b"-- hello")]), "E_ARCHIVE_PATH")
reject("directory entry", make_zip([("manifest.json", canon(good_m)), ("scripts/", b""), ("scripts/main.luau", b"-- hello")]), "E_ARCHIVE_PATH")
raw = build(good_m, good_f)
reject("trailing data", raw + b"\x00", "E_ARCHIVE_ENTRY")
reject("not a zip", b"PK\x00\x00 definitely not a zip archive at all, but long enough", "E_ARCHIVE_ENTRY")
reject("no manifest", make_zip([("scripts/main.luau", b"-- hello")]), "E_ARCHIVE_ENTRY")
# duplicate archive entries (zipfile warns on a duplicate name but writes it)
dm, df = good()
buf = io.BytesIO()
with zipfile.ZipFile(buf, "w") as z:   # zipfile warns on a duplicate name but writes it
    import warnings
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        for name, data in (("manifest.json", canon(dm)), ("scripts/main.luau", b"-- hello"), ("scripts/main.luau", b"-- hello")):
            zi = zipfile.ZipInfo(name, FIXED_TIME)
            zi.external_attr = 0o100644 << 16
            z.writestr(zi, data)
reject("duplicate archive entries", buf.getvalue(), "E_ARCHIVE_ENTRY")
# oversize entry: a stored 17 MiB asset
big = b"\x01" * ((16 << 20) + 1)
bigp = (payload("asset", "big.bin", big, asset_class="data", format="raw"), ("big.bin", big))
reject("oversized entry", mutate(lambda m: None, payloads=[bigp]), "E_ARCHIVE_SIZE")
# zip bomb: 6 MiB of zeros deflates to ~6 KiB (ratio ~1000)
zeros = b"\x00" * (6 << 20)
bomb = (payload("asset", "zero.bin", zeros, asset_class="data", format="raw"), ("zero.bin", zeros))
bm2, bf2 = good()
bm2["payloads"].append(bomb[0])
reject("zip-bomb ratio", build(bm2, bf2 + [bomb], deflate=("zero.bin",)), "E_ARCHIVE_SIZE")
# archive over 32 MiB: three incompressible 11 MiB stored assets
import os
chunks = [os.urandom(11 << 20) for _ in range(3)]
heavy = [(payload("asset", f"a{i}.bin", c, asset_class="data", format="raw"), (f"a{i}.bin", c)) for i, c in enumerate(chunks)]
reject("archive larger than 32 MiB", mutate(lambda m: None, payloads=heavy), "E_ARCHIVE_SIZE")
# per-kind caps
many = [native(f"src/f{i:02d}.c", b"int x;\n") for i in range(65)]
reject("65 native files", mutate(lambda m: m.update(author="A", abi={"native": 2}), payloads=many), "E_PAYLOAD_CAP")
accept("64 native files", mutate(lambda m: m.update(author="A", abi={"native": 2}), payloads=many[:64]))
fat = native("src/fat.c", b"/" * ((256 << 10) + 1))
reject("native file over 256 KiB", mutate(lambda m: m.update(author="A", abi={"native": 2}), payloads=[fat]), "E_PAYLOAD_CAP")
many_assets = [(payload("asset", f"a/{i:03d}.bin", bytes([i % 256]) * 3, asset_class="data", format="raw"), (f"a/{i:03d}.bin", bytes([i % 256]) * 3)) for i in range(129)]
reject("129 assets", mutate(lambda m: None, payloads=many_assets), "E_PAYLOAD_CAP")
pl = lambda n: [(payload("asset", f"a/{i:03d}.bin", b"x" + bytes([i]), asset_class="data", format="raw"), (f"a/{i:03d}.bin", b"x" + bytes([i]))) for i in range(n)]
reject("256 payloads (257 entries)", mutate(lambda m: None, payloads=pl(256)), "E_ARCHIVE_ENTRY")
reject("too many rom edits", mutate(lambda m: None, payloads=[(payload("rom-edit-set", "e.bin", b"e", edits=4097), ("e.bin", b"e"))]), "E_PAYLOAD_CAP")

# the maximum payload count: 255 payloads + manifest.json = 256 entries (128 assets, 64 Luau files, 63 guest-code files)
mix = pl(128) + [luau(f"s/{i:02d}.luau", b"-- " + bytes([48 + i % 10])) for i in range(64)] + [(payload("guest-code", f"g/{i:02d}.bin", bytes([i]), cpu="z80"), (f"g/{i:02d}.bin", bytes([i]))) for i in range(63)]
accept("255 payloads (256 entries)", mutate(lambda m: None, payloads=mix))

# canonical key order is what the packer emits; a reader accepts the manifest it re-serialises (round trip through the C++ packer is tested in mod_project_test.py)
cli.done("mod_package_test")
