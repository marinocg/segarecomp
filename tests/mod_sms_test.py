#!/usr/bin/env python3
"""SEG-147-T001 + SEG-147-T002 (ADR 0324): Master System target identity (digest + declared mapper), typed edits with (bank, offset) diagnostics, the `TMR SEGA` header checksum repair, deterministic
BPS and variant identity, and the Z80 injection rules: bank-window placement, cross-bank reference refusal and the Z80 owner's static admission. Synthetic images only; the expected checksum and edited
bytes are computed here independently.
usage: mod_sms_test.py <segarecomp-cli>"""
import struct
import sys

from mod_test_support import *  # noqa: F401,F403

cli = Cli(sys.argv[1])
tmp = temp()


def checksum(img, limit):
    return (sum(img[:0x7FF0]) + sum(img[0x8000:limit])) & 0xFFFF if limit > 0x8000 else sum(img[:min(limit, 0x7FF0)]) & 0xFFFF


def rom(size, code, header=True, fill=0xFF):
    img = bytearray([fill]) * size
    for offset, data in code.items():
        img[offset:offset + len(data)] = data
    if header:
        img[0x7FF0:0x8000] = b"TMR SEGA" + bytes(7) + bytes([0x40 | {0x8000: 0xC, 0x10000: 0xE, 0x20000: 0xF}[size]])
        struct.pack_into("<H", img, 0x7FFA, checksum(img, size))
    return bytes(img)


# bank 0 holds a 16-bit table at 0x200; bank 7 (128 KiB) holds `ret` at its start and a routine `helper` at +0x10
BANKED = rom(0x20000, {0x200: bytes([0x34, 0x12]), 0x1C000: b"\xC9", 0x1C010: bytes([0xC6, 3, 0xC9])})
_n = [0]


def project(asm=None, image=BANKED, mapper="sega", edits=None, extra="", symbols=()):
    _n[0] += 1
    d = tmp / f"p{_n[0]}"
    text = (f'[mod]\nid = "sms-mod"\nversion = "1.0.0"\nname = "SMS Mod"\nlicense = "CC0-1.0"\n\n[target]\nplatform = "master-system"\n\n'
            f'[[target.revision]]\nlabel = "rev-a"\nsha256 = "{sha(image)}"\nmapper = "{mapper}"\n\n')
    if asm is not None:
        text += '[[source]]\nkind = "guest-asm"\npath = "asm/main.asm"\ncpu = "z80"\n\n'
        write(d / "asm" / "main.asm", asm)
    if edits is not None:
        text += '[[source]]\nkind = "rom-edit-set"\npath = "edits/main.edits"\n\n'
        write(d / "edits" / "main.edits", edits)
    for name, address in symbols:
        text += f'[[symbol]]\nname = "{name}"\nrevision = "rev-a"\naddress = "{address}"\nprovenance = "verified"\n\n'
    write(d / "mod.toml", text + extra)
    return d


def ok(what, *args):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: expected ok, got rc={rc} {doc} {p.stderr[-200:]}")
    return doc or {}


def refused(what, code, *args, says=None):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 1 and doc and doc.get("ok") is False and doc.get("class") == code, f"{what}: expected {code}, got rc={rc} {doc and (doc.get('class'), doc.get('error'))}")
    if says is not None and doc:
        cli.check(says in doc.get("error", ""), f"{what}: the diagnostic says {says!r}: {doc.get('error')}")


def apply(d, image, name="out.bin"):
    out = tmp / name
    if out.exists():
        out.unlink()
    rc, doc, p = cli.run("mod", "apply", d, "--rom", write(tmp / "in.bin", image), "--out", out)
    return rc, doc or {}, (out.read_bytes() if out.exists() else None)


def apply_refused(what, code, d, image, says=None):
    rc, doc, out = apply(d, image, "refused.bin")
    cli.check(rc == 1 and doc.get("class") == code and out is None, f"{what}: expected {code} and no output, got rc={rc} {doc.get('class'), doc.get('error')}")
    if says is not None:
        cli.check(says in doc.get("error", ""), f"{what}: the diagnostic says {says!r}: {doc.get('error')}")


# ------------------------------------------------------------------ T001: cartridge / mapper configuration
EDIT = "rev-a u16 0x00000200 0x1234 0xbeef\n"
for what, mapper, size in [("rom_only beyond 32 KiB", "rom_only", 0x10000), ("sega of 48 KiB", "sega", 0xC000), ("sega below 32 KiB", "sega", 0x4000), ("sega beyond 512 KiB", "sega", 0x100000)]:
    img = bytes(bytearray(size))
    apply_refused(what, "E_MAPPER_UNSUPPORTED", project(image=img, mapper=mapper, edits="rev-a u8 0x10 0x00 0x01\n"), img)
    d = project(image=img, mapper=mapper, edits="rev-a u8 0x10 0x00 0x01\n")
    pkg = tmp / f"cfg-{mapper}-{size}.segapatch"
    ok(what + ": the project itself is well formed", "mod", "pack", d, "--out", pkg)
    refused(what + ": install against that image", "E_MAPPER_UNSUPPORTED", "mod", "install", pkg, "--rom", write(tmp / "cfg.bin", img), "--library", tmp / "lib-cfg")
refused("an unverified mapper family", "E_FIELD_VALUE", "mod", "check", project(mapper="codemasters", edits=EDIT))

# typed edit: little-endian by default, asserted, (bank, offset) in diagnostics, and the header checksum of a verifying ROM repaired
dedit = project(edits=EDIT + "rev-a bytes 0x0001c013 ffff 0102\n")
rc, doc, out = apply(dedit, BANKED)
want = bytearray(BANKED)
want[0x200:0x202] = struct.pack("<H", 0xBEEF)
want[0x1C013:0x1C015] = bytes([1, 2])
struct.pack_into("<H", want, 0x7FFA, checksum(want, len(want)))
cli.check(rc == 0 and out == bytes(want) and doc["header_repaired"] is True and doc["warnings"] == [], f"typed edits apply and the verifying header is repaired: {doc}")
cli.check(checksum(want, len(want)) != checksum(BANKED, len(BANKED)), "fixture: the edit changes the checksum")
bad = project(edits="rev-a u8 0x0001c010 0x00 0x01\n")
apply_refused("a mismatched original names bank and bank offset", "E_EDIT_EXPECTED_MISMATCH", bad, BANKED, says="bank 7, bank offset 0x0010")
apply_refused("an edit beyond the image names its bank", "E_EDIT_RANGE", project(edits="rev-a u8 0x00020000 0x00 0x01\n"), BANKED, says="bank 8, bank offset 0x0000")
apply_refused("another image", "E_IMAGE_MISMATCH", dedit, rom(0x20000, {0x200: b"\x00\x00"}))

# header repair policy (ADR 0324): explicit, opt-out, never-verifying, missing header, collisions, the other platform's policy
rc, doc, out = apply(project(edits=EDIT, extra='[build]\nheader_repair = "none"\n'), BANKED)
cli.check(rc == 0 and out[0x7FFA:0x7FFC] == BANKED[0x7FFA:0x7FFC] and doc["header_repaired"] is False and doc["warnings"] == ["W_CHECKSUM_NOT_REPAIRED"], f"opting out warns and keeps the stale checksum: {doc}")
stale = bytearray(BANKED)
stale[0x7FFA] ^= 1
stale = bytes(stale)
rc, doc, out = apply(project(image=stale, edits=EDIT), stale)
cli.check(rc == 0 and doc["header_repaired"] is False and out[0x7FFA:0x7FFC] == stale[0x7FFA:0x7FFC] and doc["warnings"] == [], f"a header that never verified is left alone: {doc}")
rc, doc, out = apply(project(image=stale, edits=EDIT, extra='[build]\nheader_repair = "sms-checksum"\n'), stale)
cli.check(rc == 0 and doc["header_repaired"] is True and struct.unpack_from("<H", out, 0x7FFA)[0] == checksum(out, len(out)), f"explicit repair writes the checksum: {doc}")
headerless = rom(0x8000, {0x200: bytes([0x34, 0x12])}, header=False)
rc, doc, out = apply(project(image=headerless, mapper="rom_only", edits=EDIT), headerless)
cli.check(rc == 0 and doc["header_repaired"] is False and out[0x7FF0:] == headerless[0x7FF0:], f"a headerless image is never given a header: {doc}")
apply_refused("explicit repair without a header", "E_HEADER_UNSUPPORTED", project(image=headerless, mapper="rom_only", edits=EDIT, extra='[build]\nheader_repair = "sms-checksum"\n'), headerless)
code_d = bytearray(BANKED)
code_d[0x7FFF] = 0x4D   # size code 0xD (48 KiB) defines no accepted range
code_d = bytes(code_d)
apply_refused("explicit repair with an undefined size code", "E_HEADER_UNSUPPORTED", project(image=code_d, edits=EDIT, extra='[build]\nheader_repair = "sms-checksum"\n'), code_d)
sig = project(edits="rev-a bytes 0x00007ff0 544d5220 00000000\n")
rc, doc, out = apply(sig, BANKED)
cli.check(rc == 0 and doc["header_repaired"] is False and doc["warnings"] == ["W_CHECKSUM_NOT_REPAIRED"], f"a mod that destroys the header gets a warning, not a guessed checksum: {doc}")
coll = project(edits="rev-a u16 0x00007ffa 0x%04x 0x0000\n" % struct.unpack_from("<H", BANKED, 0x7FFA)[0])
apply_refused("authoring the checksum the repair rewrites", "E_HEADER_FIELD_COLLISION", coll, BANKED)
refused("the Genesis repair on Master System", "E_HEADER_UNSUPPORTED", "mod", "check", project(edits=EDIT, extra='[build]\nheader_repair = "genesis-checksum"\n'))

# BPS, package identity and variant identity: the patched variant differs from Vanilla; the original is never written
original_copy = write(tmp / "vanilla.sms", BANKED)
pkg = tmp / "edit.segapatch"
packed = ok("pack with image verification", "mod", "pack", dedit, "--out", pkg, "--rom", original_copy)
cli.check(packed.get("verified_images", [{}])[0].get("target_sha256") == sha(bytes(want)), f"pack verifies the target: {packed}")
bps = tmp / "edit.bps"
doc = ok("bps", "mod", "bps", pkg, "--rom", original_copy, "--out", bps)
cli.check(doc.get("source_sha256") == sha(BANKED) and doc.get("target_sha256") == sha(bytes(want)), f"bps digests: {doc}")
ok("verify-bps", "mod", "verify-bps", pkg, "--rom", original_copy, "--bps", bps)
first = bps.read_bytes()
ok("bps again", "mod", "bps", pkg, "--rom", original_copy, "--out", bps)
cli.check(bps.read_bytes() == first, "the BPS is deterministic")
installed = ok("install", "mod", "install", pkg, "--rom", original_copy, "--library", tmp / "lib")
variant = ok("variant", "mod", "variant", "--library", tmp / "lib", "--identity", packed["package_identity"], "--rom", original_copy)
parts = variant.get("components", {})
cli.check(installed.get("effective_image") == sha(bytes(want)) and parts.get("original_image") == sha(BANKED) and parts.get("effective_image") == sha(bytes(want)), f"variant identity: {installed} {variant}")
other = tmp / "other.segapatch"
op = ok("a second variant packs", "mod", "pack", project(edits="rev-a u16 0x00000200 0x1234 0xcafe\n"), "--out", other)
ok("install the second", "mod", "install", other, "--rom", original_copy, "--library", tmp / "lib")
v2 = ok("variant of the second", "mod", "variant", "--library", tmp / "lib", "--identity", op["package_identity"], "--rom", original_copy)
cli.check(v2.get("variant_key") != variant.get("variant_key") and v2.get("components", {}).get("effective_image") != parts.get("effective_image"), "another edit is another variant")
cli.check(original_copy.read_bytes() == BANKED, "the original image is never written")
# the mapper declaration is part of the package identity
r32 = rom(0x8000, {0x200: bytes([0x34, 0x12])})
ids = [ok("pack " + m, "mod", "pack", project(image=r32, mapper=m, edits=EDIT), "--out", tmp / (m + ".segapatch"))["package_identity"] for m in ("sega", "rom_only")]
cli.check(ids[0] != ids[1], "the declared mapper is part of the package identity")

# ------------------------------------------------------------------ T002: bank-window placement on the sega mapper
F = "free * 0x300 0x100 0xff\nfree * 0x4100 0x100 0xff\nfree * 0x18100 0x100 0xff\nfree * 0x1c100 0x100 0xff\n"
HELPER = (("helper", "0x0001c010"), ("elsewhere", "0x00018100"), ("low", "0x00000200"))


def guest(asm, **kw):
    return project(F + asm, symbols=HELPER, **kw)


def accept(what, asm, **kw):
    ok(what, "mod", "check", guest(asm, **kw))


def reject(what, code, asm, says=None, **kw):
    refused(what, code, "mod", "check", guest(asm, **kw), says=says)


accept("a banked block through slot 2", "org * 0x1c100 at 0x8100\n  ret\n")
accept("a banked block through slot 1", "org * 0x1c100 at 0x4100\n  ret\n")
accept("a fixed block", "org * 0x300\n  ret\n")
accept("a bank 1 block at its reset window needs no `at`", "org * 0x4100\n  ret\n")
reject("above 0xBFFF without `at`", "E_GUEST_ADDRESS", "org * 0x1c100\n  ret\n")
reject("an address outside the block's bank position", "E_GUEST_ADDRESS", "org * 0x1c100 at 0x8200\n  ret\n")
reject("an address in work RAM", "E_GUEST_ADDRESS", "org * 0x1c100 at 0xc100\n  ret\n")
reject("a banked block assembled for the fixed 1 KiB", "E_GUEST_ADDRESS", "org * 0x4100 at 0x0100\n  ret\n")
reject("the fixed 1 KiB runs only at its offset", "E_GUEST_ADDRESS", "org * 0x300 at 0x4300\n  ret\n")
refused("a block across a bank boundary", "E_GUEST_BANK_BOUNDARY", "mod", "check", project("free * 0x7ffe 0x4 0xff\norg * 0x7fff\n  nop\n  ret\n"), says="bank 1")
refused("a block across the fixed 1 KiB", "E_GUEST_BANK_BOUNDARY", "mod", "check", project("free * 0x3ff 0x4 0xff\norg * 0x3ff\n  nop\n  ret\n"))
refused("beyond the largest Sega-mapper cartridge", "E_MAPPER_UNSUPPORTED", "mod", "check", project("free * 0x80000 0x10 0xff\norg * 0x80000 at 0x8000\n  ret\n"))

# references between locations: only where the mapping is guaranteed
TWO = "org * 0x1c100 at 0x8100\nentry:\n  {op}\n  ret\norg * {off} at {at}\nother:\n  ret\n"
accept("same bank, same window", TWO.format(op="call other", off="0x1c180", at="0x8180"))
accept("banked code calls the fixed 1 KiB", TWO.format(op="call other", off="0x300", at="0x300"))
for what, op, off, at in [("out-of-bank call", "call other", "0x18100", "0x8100"), ("out-of-bank jp", "jp other", "0x18100", "0x8100"), ("out-of-bank conditional call", "call nz,other", "0x4100", "0x4100"),
                          ("out-of-bank data pointer", "ld hl,other", "0x18100", "0x8100"), ("same bank through another window", "call other", "0x1c180", "0x4180")]:
    reject(what, "E_GUEST_BANK_REFERENCE", TWO.format(op=op, off=off, at=at))
reject("an out-of-bank relative branch", "E_GUEST_BANK_REFERENCE", "org * 0x1c100 at 0x8100\n  jr other\norg * 0x18100 at 0x8100\nother:\n  ret\n")
reject("fixed code cannot assume a banked window", "E_GUEST_BANK_REFERENCE", "org * 0x300\n  jp other\norg * 0x1c100 at 0x8100\nother:\n  ret\n")
accept("a same-bank @symbol", "org * 0x1c100 at 0x8100\n  call @helper\n  ret\n")
accept("a fixed-area @symbol", "org * 0x1c100 at 0x8100\n  call @low\n  ret\n")
reject("an @symbol of another bank", "E_GUEST_BANK_REFERENCE", "org * 0x1c100 at 0x8100\n  call @elsewhere\n  ret\n", says="bank 6")
reject("a banked @symbol from fixed code", "E_GUEST_BANK_REFERENCE", "org * 0x300\n  call @helper\n  ret\n")
reject("overlapping banked blocks", "E_GUEST_COLLISION", "org * 0x1c100 at 0x8100\n  nop\n  ret\norg * 0x1c101 at 0x8101\n  ret\n")
reject("an edit inside a banked free range", "E_GUEST_COLLISION", "org * 0x1c100 at 0x8100\n  ret\n", edits="rev-a u8 0x0001c150 0xff 0x00\n")
reject("a forbidden instruction names its bank", "E_GUEST_FORBIDDEN_INSTRUCTION", "org * 0x1c100 at 0x8100\n  halt\n", says="bank 7, bank offset 0x0100")

# ------------------------------------------------------------------ T002: static admission by the Z80 owner's decoder
B = "org * 0x1c100 at 0x8100\n"
accept("every admitted straight-line and control form",
       B + "entry:\n  push bc\n  pop bc\n  add hl,de\n  ex de,hl\n  rlca\n  rrca\n  cpl\n  scf\n  ccf\n  ld a,(0xc000)\n  ld (0xc001),a\n  inc (hl)\n  call z,sub\n  call sub\n  ret nz\n  jr c,done\n  jp nz,entry\ndone:\n  ret\nsub:\n  djnz sub\n  jp entry\n")
# every straight-line form the assembler can emit is admitted (a form the CPU projection cannot describe must be listed by the owner, never silently refused or accepted)
r8 = ["b", "c", "d", "e", "h", "l", "(hl)", "a"]
forms = [f"ld {a},{b}" for a in r8 for b in r8 if not (a == b == "(hl)")] + [f"ld {r},7" for r in r8] + [f"{op} {r}" for op in ("inc", "dec") for r in r8 + ["bc", "de", "hl", "sp"]]
forms += [f"{op} {src}" for op in ("add a,", "adc a,", "sub ", "sbc a,", "and ", "xor ", "or ", "cp ") for src in r8 + ["9"]]
forms = [f.replace(", ", ",") for f in forms]
forms += [f"ld {rr},0x1234" for rr in ("bc", "de", "hl", "sp")] + [f"add hl,{rr}" for rr in ("bc", "de", "hl", "sp")] + [f"{op} {rr}" for op in ("push", "pop") for rr in ("bc", "de", "hl", "af")]
forms += ["ld a,(bc)", "ld a,(de)", "ld (bc),a", "ld (de),a", "ld a,(0xc000)", "ld (0xc000),a", "ld hl,(0xc000)", "ld (0xc000),hl", "ld sp,hl", "ex de,hl", "nop", "scf", "ccf", "cpl", "rlca", "rrca"]
accept(f"all {len(forms)} straight-line forms", B + "".join(f"  {f}\n" for f in forms) + "  ret\n")
for cc in ("nz", "z", "nc", "c", "po", "pe", "p", "m"):
    accept(f"conditional forms {cc}", B + f"s:\n  call {cc},t\n  ret {cc}\n  jp {cc},s\n" + (f"  jr {cc},s\n" if cc in ("nz", "z", "nc", "c") else "") + "  ret\nt:\n  ret\n")
for what, asm in [("an indirect jump", "  jp (hl)\n"), ("code that runs off the end", "  nop\n"), ("a call at the end of the block", "s:\n  ret\n  call s\n"),
                  ("a conditional return at the end", "  ret z\n"), ("a conditional jump at the end", "s:\n  jp z,s\n"), ("padding in the fall-through path", "  nop\n  even\n  ret\n")]:
    reject(what, "E_GUEST_NOT_ADMITTED", B + asm, says="bank 7")
reject("a control target in the middle of an injected instruction", "E_GUEST_NOT_ADMITTED", B + "  jp @mid\ns:\n  ld bc,0x1234\n  ret\n".replace("@mid", "@helper+0xf4"))
reject("an @symbol into a block assembled for another window", "E_GUEST_NOT_ADMITTED", B + "  call @helper+0x170\n  ret\norg * 0x1c180 at 0x4180\n  ret\n")
accept("an @symbol onto an injected instruction of the same window", B + "  call @helper+0x170\n  ret\norg * 0x1c180 at 0x8180\n  ret\n")
d = guest(B + "  jp (hl)\n")
refused("pack refuses what is not admitted", "E_GUEST_NOT_ADMITTED", "mod", "pack", d, "--out", tmp / "never.segapatch")
cli.check(not (tmp / "never.segapatch").exists(), "nothing is written")
rc, doc, p = cli.run("mod", "inspect", "--json", d)
cli.check(rc == 1 and doc and doc.get("checked") is False and doc.get("class") == "E_GUEST_NOT_ADMITTED", f"inspect reports the unadmitted project: {doc}")
apply_refused("apply refuses what is not admitted", "E_GUEST_NOT_ADMITTED", d, BANKED)

# a free range is asserted, and the output is deterministic
apply_refused("a free range that is not free", "E_GUEST_RANGE_NOT_FREE", project("free * 0x1c000 0x20 0xff\norg * 0x1c000 at 0x8000\n  ret\n"), BANKED)
inj = guest(B + "  ld a,1\n  ld (0xc000),a\n  call @helper\n  ret\n")
outs = [apply(inj, BANKED, f"det{i}.bin")[2] for i in range(3)]
cli.check(outs[0] is not None and outs[0] == outs[1] == outs[2], "three applications produce identical bytes")
cli.check(outs[0] is not None and outs[0][0x1C100:0x1C109] == bytes([0x3E, 1, 0x32, 0x00, 0xC0, 0xCD, 0x10, 0x80, 0xC9]), "the @symbol is translated into the slot 2 window")

cli.done("mod_sms_test")
