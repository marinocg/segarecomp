#!/usr/bin/env python3
"""SEG-136-T001 + SEG-136-T002 (ADR 0290): typed ROM edits with expected-value assertions, per-revision resolution, overlap rejection, platform-correct endianness, and per-revision
symbols with address provenance, through the product CLI (`segarecomp mod check|pack|apply|inspect|install|import-hints`). The expected edited images are computed here independently
with `struct`/slicing, so the byte-level results are cross-checked on every host. Synthetic images only.
usage: mod_edits_test.py <segarecomp-cli>"""
import struct
import sys

from mod_test_support import *  # noqa: F401,F403

cli = Cli(sys.argv[1])
tmp = temp()

IMG_A = bytes((i * 7 + 3) & 0xFF for i in range(256))
IMG_B = bytes((i * 11 + 5) & 0xFF for i in range(256))
SHA_A, SHA_B = sha(IMG_A), sha(IMG_B)
IMG_A_PATH, IMG_B_PATH = write(tmp / "a.bin", IMG_A), write(tmp / "b.bin", IMG_B)


def hx(b):
    return b.hex()


def u(v, w):
    return "0x" + format(v, "0%dx" % (2 * w))


def toml(platform="genesis", symbols="", edits_decl="", extra=""):
    rev = lambda label, digest: f'[[target.revision]]\nlabel = "{label}"\nsha256 = "{digest}"\n' + ('mapper = "sega"\n' if platform == "master-system" else "")
    return (f'[mod]\nid = "edit-mod"\nversion = "1.0.0"\nname = "Edit Mod"\nlicense = "CC0-1.0"\n\n[target]\nplatform = "{platform}"\n\n' + rev("rev-a", SHA_A) + "\n" + rev("rev-b", SHA_B) +
            f'\n[[source]]\nkind = "rom-edit-set"\npath = "edits/main.edits"\n{edits_decl}\n' + symbols + extra)


def sym(name, revision, address, provenance):
    return f'\n[[symbol]]\nname = "{name}"\nrevision = "{revision}"\naddress = "{address}"\nprovenance = "{provenance}"\n'


_n = [0]


def project(edits, platform="genesis", **kw):
    _n[0] += 1
    d = tmp / f"p{_n[0]}"
    write(d / "mod.toml", toml(platform, **kw))
    write(d / "edits" / "main.edits", edits)
    return d


def ok(what, *args):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: expected ok, got rc={rc} {doc} {p.stderr[-200:]}")
    return doc


def refused(what, code, *args):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 1 and doc and doc.get("ok") is False and doc.get("class") == code, f"{what}: expected {code}, got rc={rc} {doc and (doc.get('class'), doc.get('error'))}")


def apply(d, image_path, name):
    out = tmp / name
    if out.exists():
        out.unlink()
    rc, doc, p = cli.run("mod", "apply", d, "--rom", image_path, "--out", out)
    return rc, doc, out


# ------------------------------------------------------------------ T001: matching assertions apply; byte-level results
want = bytearray(IMG_A)
lines = []
want[0x10:0x11] = b"\xaa"; lines.append(f"rev-a u8 0x10 {u(IMG_A[0x10], 1)} 0xaa")
want[0x20:0x22] = struct.pack(">H", 0x1234); lines.append(f"rev-a u16 0x20 {u(struct.unpack('>H', IMG_A[0x20:0x22])[0], 2)} 0x1234")      # genesis default: big-endian
want[0x30:0x34] = struct.pack(">I", 0xDEADBEEF); lines.append(f"rev-a u32 0x30 {u(struct.unpack('>I', IMG_A[0x30:0x34])[0], 4)} 0xdeadbeef")
want[0x40:0x45] = b"\x01\x02\x03\x04\x05"; lines.append(f"rev-a bytes 0x40 {hx(IMG_A[0x40:0x45])} 0102030405")
text = "# comment\n\n" + "\n".join(lines) + "\n"
d = project(text)
doc = ok("check", "mod", "check", d)
cli.check(doc.get("rom_edits") == 4, f"inspect-visible edit count: {doc.get('rom_edits')}")
rc, doc, out = apply(d, IMG_A_PATH, "ok.bin")
cli.check(rc == 0 and doc["edits_applied"] == 4 and out.read_bytes() == bytes(want), f"edits apply byte for byte: {doc}")
cli.check(doc["original_sha256"] == SHA_A and doc["effective_sha256"] == sha(bytes(want)) and doc["revision"] == "rev-a", f"revision and digests reported: {doc}")
rc2, _, out2 = apply(d, IMG_A_PATH, "ok2.bin")
cli.check(out2.read_bytes() == out.read_bytes(), "applying twice is identical (deterministic)")
cli.check(IMG_A_PATH.read_bytes() == IMG_A, "the original image is never modified")
# the packed package yields the same bytes as the project directory
pkg = tmp / "ok.segapatch"
ok("pack", "mod", "pack", d, "--out", pkg)
_, _, out3 = apply(pkg, IMG_A_PATH, "ok3.bin")
cli.check(out3.read_bytes() == bytes(want), "package route equals project route")

# ------------------------------------------------------------------ mismatch fails closed
bad = text.replace(u(IMG_A[0x10], 1), u((IMG_A[0x10] + 1) & 0xFF, 1), 1)   # first edit asserts a wrong original byte
d = project(bad)
ok("a wrong assertion is still a valid project", "mod", "check", d)
rc, doc, out = apply(d, IMG_A_PATH, "mm.bin")
cli.check(rc == 1 and doc["class"] == "E_EDIT_EXPECTED_MISMATCH" and not out.exists(), f"mismatch fails closed, writes nothing: {doc}")
cli.check("line" in doc["error"] and hx(IMG_A[0x10:0x11]) not in doc["error"].replace("0x", ""), "diagnostic names the line and does not echo image bytes")
d = project(text.replace(u(struct.unpack('>I', IMG_A[0x30:0x34])[0], 4), "0x00000000", 1))   # last-but-one edit wrong: nothing earlier is applied either
rc, doc, out = apply(d, IMG_A_PATH, "mm2.bin")
cli.check(rc == 1 and doc["class"] == "E_EDIT_EXPECTED_MISMATCH" and not out.exists(), "one bad assertion rejects the whole set")

# ------------------------------------------------------------------ mixed endianness
w = bytearray(IMG_A)
w[0x20:0x22] = struct.pack("<H", 0x1234); w[0x30:0x34] = struct.pack("<I", 0xCAFEF00D); w[0x38:0x3A] = struct.pack(">H", 0xBEEF); w[0x3C:0x40] = struct.pack(">I", 0x01020304)
mixed = (f"rev-a u16le 0x20 {u(struct.unpack('<H', IMG_A[0x20:0x22])[0], 2)} 0x1234\n"
         f"rev-a u32le 0x30 {u(struct.unpack('<I', IMG_A[0x30:0x34])[0], 4)} 0xcafef00d\n"
         f"rev-a u16be 0x38 {u(struct.unpack('>H', IMG_A[0x38:0x3A])[0], 2)} 0xbeef\n"
         f"rev-a u32be 0x3c {u(struct.unpack('>I', IMG_A[0x3C:0x40])[0], 4)} 0x01020304\n")
d = project(mixed)
rc, doc, out = apply(d, IMG_A_PATH, "mixed.bin")
cli.check(rc == 0 and out.read_bytes() == bytes(w), f"explicit endianness on genesis: {doc}")
# master-system: the platform default is little-endian; an explicit be suffix overrides it
sms = (f"rev-a u16 0x20 {u(struct.unpack('<H', IMG_A[0x20:0x22])[0], 2)} 0x1234\n"
       f"rev-a u32 0x30 {u(struct.unpack('<I', IMG_A[0x30:0x34])[0], 4)} 0xcafef00d\n"
       f"rev-a u16be 0x38 {u(struct.unpack('>H', IMG_A[0x38:0x3A])[0], 2)} 0xbeef\n")
d = project(sms, platform="master-system")
rc, doc, out = apply(d, IMG_A_PATH, "sms.bin")
w2 = bytearray(IMG_A)
w2[0x20:0x22] = struct.pack("<H", 0x1234); w2[0x30:0x34] = struct.pack("<I", 0xCAFEF00D); w2[0x38:0x3A] = struct.pack(">H", 0xBEEF)
cli.check(rc == 0 and out.read_bytes() == bytes(w2), f"master-system default is little-endian: {doc}")
# the same text means different bytes on the two platforms (and so a wrong default asserts wrongly)
d = project(f"rev-a u16 0x20 {u(struct.unpack('<H', IMG_A[0x20:0x22])[0], 2)} 0x1234\n")
rc, doc, out = apply(d, IMG_A_PATH, "wrongorder.bin")
cli.check(rc == 1 and doc["class"] == "E_EDIT_EXPECTED_MISMATCH", "genesis does not read a u16 little-endian")

# ------------------------------------------------------------------ overlap, adjacency, syntax and range
ov = lambda a, b: project(f"rev-a u16 {a[0]} {u(0,2)} 0x0001\nrev-a u16 {b[0]} {u(0,2)} 0x0002\n")
refused("overlapping edits", "E_EDIT_OVERLAP", "mod", "check", ov(("0x10",), ("0x11",)))
refused("identical site", "E_EDIT_OVERLAP", "mod", "check", ov(("0x10",), ("0x10",)))
ok("adjacent edits are not an overlap", "mod", "check", ov(("0x10",), ("0x12",)))
refused("a bytes edit contains another", "E_EDIT_OVERLAP", "mod", "check", project("rev-a bytes 0x10 00000000 01020304\nrev-a u8 0x11 0x00 0x01\n"))
refused("a wildcard edit overlaps a revision edit", "E_EDIT_OVERLAP", "mod", "check", project("* u16 0x10 0x0000 0x0001\nrev-b u8 0x11 0x00 0x01\n"))
ok("the same site in different revisions is not an overlap", "mod", "check", project("rev-a u16 0x10 0x0000 0x0001\nrev-b u16 0x10 0x0000 0x0002\n"))
for what, line, code in [
    ("unknown type", "rev-a u64 0x10 0x00 0x01", "E_EDIT_SYNTAX"),
    ("upper-case hex", "rev-a u8 0x10 0x00 0xAA", "E_EDIT_SYNTAX"),
    ("wrong digit count", "rev-a u16 0x10 0x0000 0x123", "E_EDIT_SYNTAX"),
    ("four fields", "rev-a u8 0x10 0x01", "E_EDIT_SYNTAX"),
    ("six fields", "rev-a u8 0x10 0x00 0x01 0x02", "E_EDIT_SYNTAX"),
    ("site without 0x", "rev-a u8 10 0x00 0x01", "E_EDIT_SYNTAX"),
    ("nine-digit site", "rev-a u8 0x123456789 0x00 0x01", "E_EDIT_SYNTAX"),
    ("odd bytes", "rev-a bytes 0x10 000 001", "E_EDIT_SYNTAX"),
    ("bytes length mismatch", "rev-a bytes 0x10 0000 000000", "E_EDIT_SYNTAX"),
    ("tab separator", "rev-a\tu8 0x10 0x00 0x01", "E_EDIT_SYNTAX"),
    ("carriage return", "rev-a u8 0x10 0x00 0x01\r", "E_EDIT_SYNTAX"),
    ("non-ascii", "rev-a u8 0x10 0x00 0x01 é", "E_EDIT_SYNTAX"),
    ("undeclared revision", "rev-z u8 0x10 0x00 0x01", "E_EDIT_REVISION"),
    ("beyond the largest image", "rev-a u8 0xffffffff 0x00 0x01", "E_EDIT_RANGE"),
]:
    d = project("placeholder\n")
    write(d / "edits" / "main.edits", (line + "\n").encode("utf-8"))
    refused(what, code, "mod", "check", d)
d = project(f"rev-a u32 0xfe {u(0,4)} 0x00000001\n")   # past the end of a 256-byte image: judged at apply time against the real image
ok("range is image-relative, so check passes", "mod", "check", d)
rc, doc, out = apply(d, IMG_A_PATH, "range.bin")
cli.check(rc == 1 and doc["class"] == "E_EDIT_RANGE" and not out.exists(), f"edit beyond the image end: {doc}")
d = project(f"rev-a u8 0xff {u(IMG_A[0xff], 1)} 0x00\n")   # the last byte is in range
rc, doc, out = apply(d, IMG_A_PATH, "last.bin")
cli.check(rc == 0 and out.read_bytes()[0xFF] == 0 and out.read_bytes()[:0xFF] == IMG_A[:0xFF], "the last image byte is editable")
# count: 4096 edits is the cap
many = "".join(f"* u8 0x{i:x} 0x00 0x01\n" for i in range(4097))
d = project(many)
rc, doc, _ = cli.run("mod", "check", d)
cli.check(rc == 1 and doc["class"] == "E_PAYLOAD_CAP", f"more than 4096 edits: {doc}")
# a stated count must agree
refused("declared count differs", "E_EDIT_COUNT", "mod", "check", project("rev-a u8 0x10 0x00 0x01\n", edits_decl="edits = 2\n"))
ok("declared count agrees", "mod", "check", project("rev-a u8 0x10 0x00 0x01\n", edits_decl="edits = 1\n"))

# ------------------------------------------------------------------ per-revision independence
ea = lambda off, img, new: f"rev-a u16 0x{off:x} {u(struct.unpack('>H', img[off:off+2])[0], 2)} {u(new, 2)}"
eb = lambda off, img, new: f"rev-b u16 0x{off:x} {u(struct.unpack('>H', img[off:off+2])[0], 2)} {u(new, 2)}"
ew = lambda off, img, new: f"* u8 0x{off:x} {u(img[off], 1)} {u(new, 1)}"
good = "\n".join([ea(0x10, IMG_A, 0x1111), eb(0x50, IMG_B, 0x2222)]) + "\n"
d = project(good)
rc, doc, out = apply(d, IMG_A_PATH, "ra.bin")
wa = bytearray(IMG_A); wa[0x10:0x12] = struct.pack(">H", 0x1111)
cli.check(rc == 0 and doc["revision"] == "rev-a" and doc["edits_applied"] == 1 and out.read_bytes() == bytes(wa), f"rev-a resolves only its own edits: {doc}")
rc, doc, out = apply(d, tmp / "b.bin", "rb.bin")
wb = bytearray(IMG_B); wb[0x50:0x52] = struct.pack(">H", 0x2222)
cli.check(rc == 0 and doc["revision"] == "rev-b" and out.read_bytes() == bytes(wb), f"rev-b resolves only its own edits: {doc}")
# a wrong assertion in rev-b does not affect rev-a (and vice versa)
d = project("\n".join([ea(0x10, IMG_A, 0x1111), eb(0x50, IMG_A, 0x2222)]) + "\n")   # rev-b asserts rev-a's bytes: wrong for B
rc, doc, out = apply(d, IMG_A_PATH, "ind-a.bin")
cli.check(rc == 0, f"rev-b's bad assertion leaves rev-a buildable: {doc}")
rc, doc, out = apply(d, tmp / "b.bin", "ind-b.bin")
cli.check(rc == 1 and doc["class"] == "E_EDIT_EXPECTED_MISMATCH" and not out.exists(), f"rev-b fails alone: {doc}")
# a wildcard edit is asserted against every revision it covers: both images carry different bytes at 0x01, so a shared expected value fits at most one
d = project(ew(0x01, IMG_A, 0x77) + "\n")
cli.check(IMG_A[1] != IMG_B[1], "fixture: the images differ at 0x01")
cli.check(apply(d, IMG_A_PATH, "w-a.bin")[0] == 0, "wildcard asserts against rev-a")
rc, doc, _ = apply(d, tmp / "b.bin", "w-b.bin")
cli.check(rc == 1 and doc["class"] == "E_EDIT_EXPECTED_MISMATCH", "wildcard asserts against rev-b too, and fails there")
# an image that is no declared revision is refused before any edit is looked at
write(tmp / "c.bin", b"\x00" * 256)
rc, doc, out = apply(project(good), tmp / "c.bin", "unk.bin")
cli.check(rc == 1 and doc["class"] == "E_IMAGE_MISMATCH" and not out.exists(), "an unknown image is refused")
# overlap is judged per revision: rev-b alone overlaps
refused("overlap in one revision only", "E_EDIT_OVERLAP", "mod", "check", project("rev-a u16 0x10 0x0000 0x0001\nrev-b u16 0x10 0x0000 0x0001\nrev-b u16 0x11 0x0000 0x0001\n"))

# ------------------------------------------------------------------ the effective image (install) uses the edits
lib = tmp / "lib"
d = project(good)
ok("pack for install", "mod", "pack", d, "--out", tmp / "inst.segapatch")
doc = ok("install applies and reports the edited image", "mod", "install", tmp / "inst.segapatch", "--rom", IMG_A_PATH, "--library", lib)
cli.check(doc["effective_image"] == sha(bytes(wa)), f"install effective image: {doc}")
d = project("rev-a u8 0x10 0x00 0x01\n" if IMG_A[0x10] != 0 else "rev-a u8 0x10 0x01 0x02\n")
ok("pack mismatching", "mod", "pack", d, "--out", tmp / "inst2.segapatch")
refused("install refuses an image the assertion does not hold for", "E_EDIT_EXPECTED_MISMATCH", "mod", "install", tmp / "inst2.segapatch", "--rom", IMG_A_PATH, "--library", lib)

# ------------------------------------------------------------------ package reader (independent builder)
edits = b"* u8 0x0 0x00 0x01\n"
man = lambda payloads, **kw: base_manifest(payloads=payloads, **kw)
pe = (payload("rom-edit-set", "e.edits", edits, edits=1), ("e.edits", edits))
rc, doc, _ = cli.run("mod", "inspect", write(tmp / "r1.segapatch", build(man([pe]), [pe])))
cli.check(rc == 0 and doc["rom_edits"] == 1, f"the reader accepts a valid edit set: {doc}")
pe2 = (payload("rom-edit-set", "e.edits", edits, edits=2), ("e.edits", edits))
refused("the reader checks the informational count", "E_EDIT_COUNT", "mod", "inspect", write(tmp / "r2.segapatch", build(man([pe2]), [pe2])))
bad = b"* u8 0x0 0x00 0x01\n* u8 0x0 0x00 0x02\n"
pe3 = (payload("rom-edit-set", "e.edits", bad, edits=2), ("e.edits", bad))
refused("the reader rejects overlap", "E_EDIT_OVERLAP", "mod", "inspect", write(tmp / "r3.segapatch", build(man([pe3]), [pe3])))

# ================================================================== T002: symbols, revisions and provenance
SYM_EDIT = "* u16 @title+0x2 {} 0x7777\n"
va, vb = 0x24, 0x44   # the same logical symbol sits at different addresses per revision
text_sym = f"rev-a u16 @title+0x2 {u(struct.unpack('>H', IMG_A[va+2:va+4])[0], 2)} 0x7777\nrev-b u16 @title+0x2 {u(struct.unpack('>H', IMG_B[vb+2:vb+4])[0], 2)} 0x7777\n"
syms = sym("title", "rev-a", "0x00000024", "verified") + sym("title", "rev-b", "0x00000044", "verified")
d = project(text_sym, symbols=syms)
doc = ok("verified symbols resolve per revision", "mod", "check", d)
cli.check(doc["symbols"] == [{"name": "title", "revision": "rev-a", "address": "0x00000024", "provenance": "verified"},
                             {"name": "title", "revision": "rev-b", "address": "0x00000044", "provenance": "verified"}], f"provenance is recorded and printed by check/inspect: {doc['symbols']}")
rc, doc, out = apply(d, IMG_A_PATH, "s-a.bin")
w = bytearray(IMG_A); w[va + 2:va + 4] = b"\x77\x77"
cli.check(rc == 0 and out.read_bytes() == bytes(w), f"rev-a resolves its address: {doc}")
rc, doc, out = apply(d, tmp / "b.bin", "s-b.bin")
w = bytearray(IMG_B); w[vb + 2:vb + 4] = b"\x77\x77"
cli.check(rc == 0 and out.read_bytes() == bytes(w), f"rev-b resolves its own address: {doc}")
# one wildcard edit, two addresses
wild = "* u8 @title " + "{}" + " 0x7f\n"
d = project(f"rev-a u8 @title {u(IMG_A[va],1)} 0x7f\nrev-b u8 @title {u(IMG_B[vb],1)} 0x7f\n", symbols=syms)
ok("same symbol, per-revision lines", "mod", "check", d)
# provenance survives the package round trip and is printed by inspect of the package
pk = tmp / "sym.segapatch"
ok("pack", "mod", "pack", d, "--out", pk)
rc, doc, _ = cli.run("mod", "inspect", pk)
cli.check(rc == 0 and [s["provenance"] for s in doc["symbols"]] == ["verified", "verified"], f"inspect <package> prints provenance: {doc.get('symbols')}")
ok("apply from the package", "mod", "apply", pk, "--rom", IMG_A_PATH, "--out", tmp / "from-pk.bin")
# a symbol without a verified entry for a revision fails the build
d = project(text_sym, symbols=sym("title", "rev-a", "0x00000024", "verified"))
refused("revision without an entry", "E_SYMBOL_UNKNOWN", "mod", "check", d)
refused("revision without an entry (build)", "E_SYMBOL_UNKNOWN", "mod", "build", d, "--out", tmp / "no.segapatch")
cli.check(not (tmp / "no.segapatch").exists(), "a failed build writes no package")
refused("a wildcard edit needs every revision", "E_SYMBOL_UNKNOWN", "mod", "check", project(SYM_EDIT.format("0x0000"), symbols=sym("title", "rev-a", "0x00000024", "verified")))
refused("an unknown symbol name", "E_SYMBOL_UNKNOWN", "mod", "check", project(text_sym, symbols=sym("other", "rev-a", "0x24", "verified")))
# hint-only and manual entries can never be patch sites
for prov in ("imported-hint", "manual"):
    s2 = sym("title", "rev-a", "0x00000024", "verified") + sym("title", "rev-b", "0x00000044", prov)
    refused(f"{prov} as a site", "E_SYMBOL_NOT_VERIFIED", "mod", "check", project(text_sym, symbols=s2))
# ... but a hint that no edit uses is just recorded
d = project("rev-a u8 0x10 0x00 0x01\n".replace("0x00 ", u(IMG_A[0x10], 1) + " "), symbols=sym("unused", "rev-a", "0x00000050", "imported-hint"))
doc = ok("an unused hint is recorded", "mod", "check", d)
cli.check(doc["symbols"][0]["provenance"] == "imported-hint", "hint provenance printed")
# invalid symbol entries
refused("duplicate (name, revision)", "E_SYMBOL_DUPLICATE", "mod", "check", project("# none\n", symbols=sym("title", "rev-a", "0x24", "verified") + sym("title", "rev-a", "0x28", "verified")))
refused("symbol of an undeclared revision", "E_SYMBOL_REVISION", "mod", "check", project("# none\n", symbols=sym("title", "rev-z", "0x24", "verified")))
refused("unknown provenance", "E_FIELD_VALUE", "mod", "check", project("# none\n", symbols=sym("title", "rev-a", "0x24", "guessed")))
refused("upper-case name", "E_FIELD_VALUE", "mod", "check", project("# none\n", symbols=sym("Title", "rev-a", "0x24", "verified")))
refused("name with spaces (no free text / disassembly)", "E_FIELD_VALUE", "mod", "check", project("# none\n", symbols=sym("move.w d0, d1", "rev-a", "0x24", "verified")))
refused("name too long", "E_FIELD_VALUE", "mod", "check", project("# none\n", symbols=sym("a" * 65, "rev-a", "0x24", "verified")))
refused("address not hex", "E_FIELD_VALUE", "mod", "check", project("# none\n", symbols=sym("title", "rev-a", "36", "verified")))
refused("address too long", "E_FIELD_VALUE", "mod", "check", project("# none\n", symbols=sym("title", "rev-a", "0x123456789", "verified")))
refused("symbol address plus addend beyond the image limit", "E_EDIT_RANGE", "mod", "check", project("rev-a u8 @title+0x10 0x00 0x01\n", symbols=sym("title", "rev-a", "0x00ffffff", "verified") + sym("title", "rev-b", "0x00000000", "verified")))
refused("symbol address beyond the largest image", "E_FIELD_VALUE", "mod", "check", project("# none\n", symbols=sym("title", "rev-a", "0x01000000", "verified")))
# identity: order in mod.toml does not matter, but provenance and address do
p1 = project(text_sym, symbols=syms)
p2 = project(text_sym, symbols=sym("title", "rev-b", "0x00000044", "verified") + sym("title", "rev-a", "0x00000024", "verified"))
i1 = ok("p1", "mod", "check", p1)["package_identity"]
i2 = ok("p2", "mod", "check", p2)["package_identity"]
cli.check(i1 == i2, "symbol order in mod.toml does not change the package identity")
# a different address for the same edit text changes the content digest (symbols decide where edits land)
c1 = ok("c1", "mod", "check", project("rev-a u8 0x10 " + u(IMG_A[0x10], 1) + " 0x01\n", symbols=sym("x", "rev-a", "0x10", "verified")))
c2 = ok("c2", "mod", "check", project("rev-a u8 0x10 " + u(IMG_A[0x10], 1) + " 0x01\n", symbols=sym("x", "rev-a", "0x11", "verified")))
cli.check(c1["content_digest"] != c2["content_digest"] and c1["package_identity"] != c2["package_identity"], "symbols are part of identity and content digest")
# the reader (independent builder): symbols validated, sorted, provenance enforced
pe = (payload("rom-edit-set", "e.edits", b"rev-a u8 @t 0x00 0x01\n", edits=1), ("e.edits", b"rev-a u8 @t 0x00 0x01\n"))
mk = lambda symbols: write(tmp / "s.segapatch", build(man([pe], symbols=symbols), [pe]))
S = lambda n, r, a, p: {"name": n, "revision": r, "address": a, "provenance": p}
rc, doc, _ = cli.run("mod", "inspect", mk([S("t", "rev-a", 16, "verified")]))
cli.check(rc == 0 and doc["symbols"][0]["address"] == "0x00000010", f"reader accepts a verified symbol: {doc}")
refused("reader: hint used as a site", "E_SYMBOL_NOT_VERIFIED", "mod", "inspect", mk([S("t", "rev-a", 16, "imported-hint")]))
refused("reader: no entry for the revision", "E_SYMBOL_UNKNOWN", "mod", "inspect", mk([S("z", "rev-a", 16, "verified")]))
refused("reader: unsorted symbols", "E_FIELD_VALUE", "mod", "inspect", mk([S("u", "rev-a", 1, "verified"), S("t", "rev-a", 1, "verified")]))
refused("reader: duplicate symbols", "E_SYMBOL_DUPLICATE", "mod", "inspect", mk([S("t", "rev-a", 1, "verified"), S("t", "rev-a", 2, "verified")]))
refused("reader: undeclared revision", "E_SYMBOL_REVISION", "mod", "inspect", mk([S("t", "rev-z", 1, "verified")]))
refused("reader: bad provenance", "E_FIELD_VALUE", "mod", "inspect", mk([S("t", "rev-a", 1, "trusted")]))

# ------------------------------------------------------------------ hint import: hints only, never verified
hints = write(tmp / "hints.csv", '"Name","Location","Type"\n"Main_Loop","0x00000200","Function"\n"FUN_00001234","$1234","Function"\n# a comment\nvblank_handler 512\n')
d = project("# none\n")
doc = ok("import hints", "mod", "import-hints", d, "--revision", "rev-a", "--from", hints)
cli.check(doc["imported"] == 3 and doc["provenance"] == "imported-hint", f"import report: {doc}")
doc = ok("the imported project checks", "mod", "check", d)
got = {s["name"]: (s["address"], s["provenance"]) for s in doc["symbols"]}
cli.check(got == {"main_loop": ("0x00000200", "imported-hint"), "fun_00001234": ("0x00001234", "imported-hint"), "vblank_handler": ("0x00000200", "imported-hint")}, f"imported entries are hints with folded names: {got}")
refused("re-import is a duplicate", "E_SYMBOL_DUPLICATE", "mod", "import-hints", d, "--revision", "rev-a", "--from", hints)
refused("unknown revision", "E_SYMBOL_REVISION", "mod", "import-hints", d, "--revision", "rev-z", "--from", hints)
refused("unparsable address", "E_SYMBOL_HINT", "mod", "import-hints", d, "--revision", "rev-b", "--from", write(tmp / "badh.txt", "name 0x200\nother 00ff\n"))
refused("a bad first row is not mistaken for a header", "E_SYMBOL_HINT", "mod", "import-hints", d, "--revision", "rev-b", "--from", write(tmp / "badh3.txt", "foo bar\nx 0x10\n"))
refused("missing hint file", "E_SYMBOL_HINT", "mod", "import-hints", d, "--revision", "rev-b", "--from", tmp / "nope.txt")
before = (d / "mod.toml").read_bytes()
refused("a failed import changes nothing", "E_SYMBOL_HINT", "mod", "import-hints", d, "--revision", "rev-b", "--from", write(tmp / "badh2.txt", "good 0x10\nbad\n"))
cli.check((d / "mod.toml").read_bytes() == before, "mod.toml is untouched by a failed import")
# an imported hint cannot be a site until the author promotes it to verified
d2 = project("rev-a u8 @vblank_handler 0x00 0x01\n")
ok("import into a project that uses the symbol", "mod", "import-hints", d2, "--revision", "rev-a", "--from", write(tmp / "h2.txt", "vblank_handler 0x10\n"))
refused("a hint cannot be a site", "E_SYMBOL_NOT_VERIFIED", "mod", "check", d2)
promoted = (d2 / "mod.toml").read_text().replace('provenance = "imported-hint"', 'provenance = "verified"')
write(d2 / "mod.toml", promoted)
write(d2 / "edits" / "main.edits", f"rev-a u8 @vblank_handler {u(IMG_A[0x10], 1)} 0x01\n")
ok("after promotion to verified", "mod", "check", d2)

# ------------------------------------------------------------------ init template
t = tmp / "tpl"
doc = ok("init rom-edit template", "mod", "init", t, "--platform", "genesis", "--id", "tpl", "--template", "rom-edit", "--rom-sha256", SHA_A)
cli.check("edits/main.edits" in doc["created"], f"template files: {doc}")
ok("the template checks", "mod", "check", t)

cli.done("mod_edits_test")
