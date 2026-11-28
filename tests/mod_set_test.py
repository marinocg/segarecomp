#!/usr/bin/env python3
"""SEG-141-T001 + SEG-141-T002 + SEG-141-T004 (ADR 0326): ordered mod sets through the product CLI. Declared dependencies / incompatibilities / version ranges (`mod.toml`, the canonical manifest, the
reader); verified composition of typed edits and injected code on the exact original (overlaps, BPS, hook sites, header policy, Master System bank diagnostics and joint Z80 admission); persisted,
versioned set definitions; composite variant identities with cache reuse and rebuild reasons, and a one-member set that IS the single mod. Composed bytes, checksums, BPS payloads and manifests are
computed here independently of the C++ code. Synthetic images and mods only.
usage: mod_set_test.py <segarecomp-cli> <platforms-dir> <C compiler> <tools/segapatch.py>"""
import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile

from mod_test_support import *  # noqa: F401,F403

cli = Cli(sys.argv[1])
PLATFORMS, CC = sys.argv[2], sys.argv[3]
_spec = importlib.util.spec_from_file_location("segapatch", sys.argv[4])
segapatch = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(segapatch)
tmp = temp()


# ------------------------------------------------------------------ synthetic images
def checksum(img):
    return sum(struct.unpack_from(">H", img, i)[0] for i in range(0x200, len(img), 2)) & 0xFFFF


def genesis_rom(size=0x2000):
    img = bytearray(((i * 7 + 3) & 0xFF) for i in range(size))
    img[0x100:0x110] = b"SEGA MEGA DRIVE "
    img[0x1000:0x1100] = b"\xff" * 0x100
    struct.pack_into(">I", img, 0x1A0, 0)
    struct.pack_into(">I", img, 0x1A4, size - 1)
    struct.pack_into(">H", img, 0x18E, checksum(img))
    return bytes(img)


ROM = genesis_rom()
OTHER = genesis_rom(0x4000)
ROM_PATH, OTHER_PATH = write(tmp / "game.bin", ROM), write(tmp / "other.bin", OTHER)
_n = [0]


def project(mod_id, *, version="1.0.0", image=ROM, platform="genesis", mapper=None, edits=None, asm=None, cpu="m68k", bps=None, symbols=(), extra="", luau=False, native=False, hooks=""):
    _n[0] += 1
    d = tmp / f"p{_n[0]}-{mod_id}"
    text = f'[mod]\nid = "{mod_id}"\nversion = "{version}"\nname = "{mod_id}"\nlicense = "CC0-1.0"\n' + ('author = "tests"\n' if native else "")
    text += f'\n[target]\nplatform = "{platform}"\n\n[[target.revision]]\nlabel = "rev-a"\nsha256 = "{sha(image)}"\n' + (f'mapper = "{mapper}"\n' if mapper else "")
    if edits is not None:
        text += '\n[[source]]\nkind = "rom-edit-set"\npath = "edits/main.edits"\n'
        write(d / "edits" / "main.edits", edits)
    if asm is not None:
        text += f'\n[[source]]\nkind = "guest-asm"\npath = "asm/main.asm"\ncpu = "{cpu}"\n'
        write(d / "asm" / "main.asm", asm)
    if bps is not None:
        target, patch = bps
        text += f'\n[[source]]\nkind = "bps-image"\npath = "game.bps"\ntarget_sha256 = "{sha(target)}"\n'
        write(d / "game.bps", patch)
    if luau:
        text += '\n[[source]]\nkind = "luau-source"\npath = "scripts/main.luau"\n\n[capabilities]\nrequired = ["host.log"]\n'
        write(d / "scripts" / "main.luau", f'function reset()\n  sega.log("{mod_id}")\nend\n')
    if native:
        s = tmp / f"native-template-{_n[0]}"
        rc, doc, p = cli.run("mod", "init", s, "--platform", platform, "--id", mod_id, "--template", "c11-provider")
        assert rc == 0, (doc, p.stderr)
        write(d / "src" / "provider.c", (s / "src" / "provider.c").read_bytes())
        text += '\n[[source]]\nkind = "native-c11"\npath = "src/provider.c"\n\n[abi]\nnative = 2\n'
        if hooks:
            text += '\n[capabilities]\nrequired = ["hook.function"]\n'
    for name, address in symbols:
        text += f'\n[[symbol]]\nname = "{name}"\nrevision = "rev-a"\naddress = "{address}"\nprovenance = "verified"\n'
    write(d / "mod.toml", text + hooks + extra)
    return d


def dep(mod_id, version=None, kind="dependency"):
    return f'\n[[{kind}]]\nid = "{mod_id}"\n' + (f'version = "{version}"\n' if version is not None else "")


def excl(mod_id, version=None):
    return dep(mod_id, version, "incompatibility")


# SEG-141-T006: the exact text of every composition and set-build diagnostic (class, message, every violation) is pinned in tests/data/mod_set_diagnostics.json. A deliberate wording change is made with
# SEGARECOMP_UPDATE_GOLDEN=1 and reviewed as a diff. Temporary paths and package digests are normalised.
GOLDEN = {}
GOLDEN_PATH = Path(__file__).resolve().parent / "data" / "mod_set_diagnostics.json"


def normalised(value):
    return json.loads(re.sub(r"[0-9a-f]{64}", "<digest>", json.dumps(value).replace(str(tmp), "<tmp>")))


def ok(what, *args):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: expected ok, got rc={rc} {doc} {p.stderr[-300:]}")
    return doc or {}


def refused(what, code, *args, says=(), mods=None):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 1 and doc and doc.get("ok") is False and doc.get("class") == code, f"{what}: expected {code}, got rc={rc} {doc and (doc.get('class'), doc.get('error'))} {p.stderr[-300:]}")
    doc = doc or {}
    if code.startswith(("E_SET_", "E_HEADER_FIELD", "E_EDIT_EXPECTED", "E_NOT_APPROVED")) or (list(args[:2]) == ["mod", "set"] and code in ("E_PACKAGE_CHANGED", "E_PLATFORM_UNKNOWN", "E_RANGE_DECLARATION", "E_MEMORY_RANGE", "E_HOOK_REVISION", "E_HOOK_COUNT", "E_HOOK_CALLBACK_UNSUPPORTED")):
        GOLDEN.setdefault(code, []).append(normalised({"case": what, "class": doc.get("class"), "error": doc.get("error"), "violations": doc.get("violations", [])}))
    for word in says:
        cli.check(word in doc.get("error", ""), f"{what}: the diagnostic says {word!r}: {doc.get('error')}")
    if mods is not None:
        first = (doc.get("violations") or [{}])[0]
        cli.check(sorted(first.get("mods", [])) == sorted(mods), f"{what}: the first violation names {mods}: {first}")
    return doc


def composed(what, members, rom=ROM_PATH, name="composed.bin"):
    out = tmp / name
    if out.exists():
        out.unlink()
    doc = ok(what, "mod", "set", "apply", *members, "--rom", rom, "--out", out)
    return doc, (out.read_bytes() if out.exists() else b"")


def rejected_composition(what, code, members, rom=ROM_PATH, **kw):
    out = tmp / "refused.bin"
    doc = refused(what, code, "mod", "set", "apply", *members, "--rom", rom, "--out", out, **kw)
    cli.check(not out.exists(), f"{what}: a refused composition writes nothing")
    return doc


def orig(offset, n):
    return "0x" + ROM[offset:offset + n].hex()


# ------------------------------------------------------------------ T002: disjoint edits and injected code compose; the bytes are computed independently
edit_a = project("edit-a", edits=f"rev-a u16 0x00000400 {orig(0x400, 2)} 0x1111\n")
edit_b = project("edit-b", edits=f"rev-a u16 0x00000402 {orig(0x402, 2)} 0x2222\n")   # ADJACENT to edit-a: touching ranges are disjoint
code_c = project("code-c", asm="free * 0x1000 0x100 0xff\norg * 0x1000\n  moveq #9,d1\n  rts\n")
want = bytearray(ROM)
want[0x400:0x404] = b"\x11\x11\x22\x22"
want[0x1000:0x1004] = bytes([0x72, 0x09, 0x4E, 0x75])
struct.pack_into(">H", want, 0x18E, checksum(want))
doc, out = composed("disjoint edits and code compose", [edit_a, edit_b, code_c])
cli.check(out == bytes(want) and doc.get("effective_sha256") == sha(bytes(want)) and doc.get("header_repaired") is True and doc.get("write_ranges") == 3,
          f"the composed image is byte-exact with ONE header repair over all changes: {doc}")
doc2, out2 = composed("the reverse order composes the same bytes", [code_c, edit_b, edit_a])
cli.check(out2 == out, "disjoint writes give the same bytes in any order")
checked = ok("check with --rom", "mod", "set", "check", edit_a, edit_b, code_c, "--rom", ROM_PATH)
cli.check(checked.get("composed") is True and checked.get("effective_sha256") == sha(bytes(want)), f"check composes: {checked}")
cli.check(ok("check without --rom checks the relations only", "mod", "set", "check", edit_a, edit_b).get("composed") is False, "no image, no composition")
single, single_out = composed("a one-member set", [edit_a])
ok("single apply", "mod", "apply", edit_a, "--rom", ROM_PATH, "--out", tmp / "single.bin")
cli.check(single_out == (tmp / "single.bin").read_bytes(), "a one-member set composes exactly the single mod's image")

# overlaps are refused naming BOTH mods; nothing is merged, even when both write the same bytes
over = project("edit-over", edits=f"rev-a bytes 0x00000401 {ROM[0x401:0x403].hex()} abcd\n")
rejected_composition("overlapping edits", "E_SET_EDIT_OVERLAP", [edit_a, over], says=('"edit-a"', '"edit-over"', "offset 0x401", "1 byte(s)"), mods=["edit-a", "edit-over"])
same = project("edit-same", edits=f"rev-a u16 0x00000400 {orig(0x400, 2)} 0x1111\n")
rejected_composition("identical writes are still an overlap", "E_SET_EDIT_OVERLAP", [edit_a, same], mods=["edit-a", "edit-same"])
into_code = project("edit-into-code", edits="rev-a u16 0x00001002 0xffff 0x0000\n")
rejected_composition("an edit over another mod's injected block", "E_SET_EDIT_OVERLAP", [into_code, code_c], says=('"edit-into-code"', '"code-c"'), mods=["edit-into-code", "code-c"])
two_code = project("code-d", asm="free * 0x1000 0x100 0xff\norg * 0x1002\n  rts\n")
rejected_composition("two injected blocks overlap", "E_SET_EDIT_OVERLAP", [code_c, two_code], mods=["code-c", "code-d"])
beside = project("code-e", asm="free * 0x1000 0x100 0xff\norg * 0x1010\n  rts\n")
composed("two mods share a free range with disjoint blocks", [code_c, beside])
doc = rejected_composition("every overlapping pair is listed", "E_SET_EDIT_OVERLAP", [edit_a, over, same])
cli.check([sorted(v["mods"]) for v in doc.get("violations", [])] == [["edit-a", "edit-over"], ["edit-a", "edit-same"], ["edit-over", "edit-same"]], f"all pairs: {doc.get('violations')}")

# no colliding pair is hidden behind another pair's many ranges
lines = [f"rev-a u8 0x{0x800 + 2 * k:08x} 0x{ROM[0x800 + 2 * k]:02x} 0x00" for k in range(40)]
last = f"rev-a u8 0x00000a00 0x{ROM[0xA00]:02x} 0x00"
many_a = project("many-a", edits="\n".join(lines + [last]) + "\n")
many_b = project("many-b", edits="\n".join(lines) + "\n")
many_c = project("many-c", edits=last + "\n")
doc = rejected_composition("many ranges", "E_SET_EDIT_OVERLAP", [many_a, many_b, many_c], says=("first of 40",))
cli.check([sorted(v["mods"]) for v in doc.get("violations", [])] == [["many-a", "many-b"], ["many-a", "many-c"]], f"both colliding pairs are listed: {doc.get('violations')}")

# Genesis sound-CPU code: disjoint cartridge bytes assembled for the same sound-RAM address collide; separate addresses compose
def z80_mod(mod_id, offset, address):
    return project(mod_id, cpu="z80", asm=f"free * 0x1000 0x100 0xff\norg * 0x{offset:x} at 0x{address:x}\n  ret\n")


rejected_composition("the same sound-RAM address", "E_SET_EDIT_OVERLAP", [z80_mod("snd-a", 0x1000, 0x1000), z80_mod("snd-b", 0x1020, 0x1000)], says=("z80.sound_ram address 0x1000", '"snd-a"', '"snd-b"'), mods=["snd-a", "snd-b"])
doc, _ = composed("separate sound-RAM addresses", [z80_mod("snd-a", 0x1000, 0x1000), z80_mod("snd-c", 0x1020, 0x1100)])
cli.check(doc.get("write_ranges") == 2, f"only image writes are counted: {doc}")

# expected values are asserted against the PRE-composition original: a mod relying on another's bytes is rejected, in either order
relies = project("relies-on-a", edits="rev-a u16 0x00000400 0x1111 0x3333\n")
for order in ([edit_a, relies], [relies, edit_a]):
    doc = rejected_composition("relying on another mod's bytes", "E_EDIT_EXPECTED_MISMATCH", order, says=('"relies-on-a"', "unmodified original"))
    cli.check(doc.get("violations", [{}])[0].get("mods") == ["relies-on-a"], f"the dependent mod is named: {doc.get('violations')}")
rejected_composition("a member that does not declare this image", "E_SET_SOURCE", [edit_a, project("other-image", image=OTHER, edits=f"rev-a u8 0x10 0x{OTHER[0x10]:02x} 0x00\n")], says=('"other-image"',), mods=["other-image"])

# BPS: conservative. It composes only with members that neither change the image nor hook it.
bps_target = bytearray(ROM)
bps_target[0x500] ^= 0xFF
bps_target = bytes(bps_target)
bps_a = project("bps-a", bps=(bps_target, segapatch.bps_create(ROM, bps_target)))
rejected_composition("BPS with typed edits (disjoint bytes are still refused)", "E_SET_BPS_CONFLICT", [bps_a, edit_a], says=('"bps-a"', '"edit-a"', "never merged"), mods=["bps-a", "edit-a"])
rejected_composition("typed edits then BPS", "E_SET_BPS_CONFLICT", [edit_a, bps_a], mods=["bps-a", "edit-a"])
other_target = bytearray(ROM)
other_target[0x600] ^= 0xFF
bps_b = project("bps-b", bps=(bytes(other_target), segapatch.bps_create(ROM, bytes(other_target))))
doc = rejected_composition("two BPS patches", "E_SET_BPS_CONFLICT", [bps_a, bps_b])
cli.check(len(doc.get("violations", [])) == 1, f"one violation per BPS pair: {doc.get('violations')}")
rejected_composition("BPS with injected code", "E_SET_BPS_CONFLICT", [code_c, bps_a], mods=["bps-a", "code-c"])
script = project("hud-script", luau=True)
doc, out = composed("BPS with a script-only mod", [bps_a, script])
cli.check(out == bps_target and doc.get("header_repaired") is False, f"the composed image is exactly the BPS target: {doc}")

# hook sites: a write over another member's hooked entry is refused; a write elsewhere is not
HOOK = '\n[[hook]]\nid = "watch"\nsymbol = "fn"\nmode = "before"\nreads = ["d0"]\n'
hooker = project("hooker", native=True, hooks=HOOK, symbols=(("fn", "0x00000600"),))
rejected_composition("a write over a hooked entry", "E_SET_HOOK_SITE", [project("edit-hook", edits=f"rev-a u8 0x00000601 0x{ROM[0x601]:02x} 0x00\n"), hooker], says=('"edit-hook"', '"hooker"'), mods=["edit-hook", "hooker"])
composed("a write next to a hooked entry", [project("edit-near", edits=f"rev-a u8 0x00000602 0x{ROM[0x602]:02x} 0x00\n"), hooker])
# exclusive ownership (SEG-141-T003, ADR 0327): two observers share a site; a replacement owns its function alone, in either order
observer = project("observer", native=True, hooks=HOOK, symbols=(("fn", "0x00000600"),))
doc = ok("two observing mods share a site", "mod", "set", "check", hooker, observer, "--rom", ROM_PATH)
cli.check(doc.get("composed") is True, f"two before hooks on one function compose: {doc}")
REPLACE = '\n[[hook]]\nid = "swap"\nsymbol = "fn"\nmode = "replace"\nreads = ["d0"]\nwrites = ["d0", "ccr"]\ncost = 24\n'
swapper = project("swapper", native=True, hooks=REPLACE, symbols=(("fn", "0x00000600"),))
other_swapper = project("swapper-two", native=True, hooks=REPLACE, symbols=(("fn", "0x00000600"),))
for members, names in (([swapper, other_swapper], ["swapper", "swapper-two"]), ([hooker, swapper], ["hooker", "swapper"]), ([swapper, hooker], ["swapper", "hooker"])):
    rejected_composition("an exclusive-hook conflict " + "/".join(names), "E_SET_HOOK_OWNER", members, says=(f'"{names[0]}"', f'"{names[1]}"', "exclusively"), mods=names)
elsewhere = project("swapper-else", native=True, hooks=REPLACE, symbols=(("fn", "0x00000700"),))
composed("replacements of different functions", [swapper, elsewhere])

rejected_composition("BPS with a hooking mod", "E_SET_BPS_CONFLICT", [bps_a, hooker], says=("hook sites",), mods=["bps-a", "hooker"])

# header policy: one image, one policy
opted = project("opt-out", edits=f"rev-a u8 0x00000700 0x{ROM[0x700]:02x} 0x00\n", extra='\n[build]\nheader_repair = "none"\n')
rejected_composition("opting out against a repairing mod", "E_SET_HEADER_POLICY", [opted, edit_a], says=('"opt-out"', '"edit-a"'), mods=["opt-out", "edit-a"])
doc, out = composed("opting out beside a mod that changes no bytes", [opted, script])
cli.check(out[0x18E:0x190] == ROM[0x18E:0x190] and doc.get("warnings") == ["W_CHECKSUM_NOT_REPAIRED"], f"the opt-out is kept and warned: {doc}")
strict = project("strict", edits=f"rev-a u8 0x00000702 0x{ROM[0x702]:02x} 0x00\n", extra='\n[build]\nheader_repair = "genesis-checksum"\n')
end_field = project("rom-end", edits=f"rev-a u32 0x000001a4 {orig(0x1A4, 4)} 0x00001ffd\n")
composed("the ROM-end field alone under the default policy", [end_field, script])
rejected_composition("another mod's write that the combined strict repair would discard", "E_HEADER_FIELD_COLLISION", [strict, end_field], says=('"rom-end"',), mods=["rom-end", "strict"])
cli.check(ROM_PATH.read_bytes() == ROM, "the original image is never modified")

# ------------------------------------------------------------------ Master System: bank-qualified ranges and joint Z80 admission
sms = bytearray([0xFF]) * 0x20000
sms[0x7FF0:0x8000] = b"TMR SEGA" + bytes(7) + bytes([0x4F])
sms[0x1C000] = 0xC9
struct.pack_into("<H", sms, 0x7FFA, sum(sms[i] for i in range(0x20000) if not 0x7FF0 <= i < 0x8000) & 0xFFFF)
SMS = bytes(sms)
SMS_PATH = write(tmp / "game.sms", SMS)


def sms_mod(mod_id, **kw):
    kw.setdefault("mapper", "sega")
    return project(mod_id, platform="master-system", image=SMS, cpu="z80", **kw)


s1 = sms_mod("sms-a", asm="free * 0x1c100 0x100 0xff\norg * 0x1c100 at 0x8100\n  ret\n")
s2 = sms_mod("sms-b", edits="rev-a u8 0x0001c100 0xff 0x00\n")
rejected_composition("a bank-qualified overlap", "E_SET_EDIT_OVERLAP", [s1, s2], rom=SMS_PATH, says=('"sms-a"', '"sms-b"', "bank 7, bank offset 0x0100"), mods=["sms-a", "sms-b"])
s3 = sms_mod("sms-c", edits="rev-a u8 0x00018100 0xff 0x00\n")
doc, out = composed("the same bank offset in another bank is disjoint", [s1, s3], rom=SMS_PATH)
want = bytearray(SMS)
want[0x1C100], want[0x18100] = 0xC9, 0x00
struct.pack_into("<H", want, 0x7FFA, sum(want[i] for i in range(0x20000) if not 0x7FF0 <= i < 0x8000) & 0xFFFF)
cli.check(out == bytes(want), "the composed Master System image and its checksum are byte-exact")
# sms-d calls an address through a verified symbol that is existing cartridge code for it, but free-range PADDING of sms-e: each alone is admitted, together they are not
caller = sms_mod("sms-d", asm="free * 0x300 0x40 0xff\norg * 0x300\n  call @pad\n  ret\n", symbols=(("pad", "0x00000380"),))
owner = sms_mod("sms-e", asm="free * 0x370 0x40 0xff\norg * 0x370\n  ret\n")
ok("caller alone", "mod", "apply", caller, "--rom", SMS_PATH, "--out", tmp / "d.bin")
ok("owner alone", "mod", "apply", owner, "--rom", SMS_PATH, "--out", tmp / "e.bin")
rejected_composition("joint admission", "E_SET_GUEST_ADMISSION", [caller, owner], rom=SMS_PATH, says=('"sms-d"', '"sms-e"', "padding"), mods=["sms-d", "sms-e"])
r32 = bytes(bytearray([0xFF]) * 0x8000)
r32_path = write(tmp / "r32.sms", r32)
m1 = project("map-a", platform="master-system", image=r32, mapper="sega", edits="rev-a u8 0x10 0xff 0x00\n")
m2 = project("map-b", platform="master-system", image=r32, mapper="rom_only", edits="rev-a u8 0x20 0xff 0x00\n")
rejected_composition("different mapper declarations", "E_SET_SOURCE", [m1, m2], rom=r32_path, mods=["map-a", "map-b"])
refused("mods of two platforms", "E_SET_PLATFORM", "mod", "set", "check", edit_a, s1, says=("master-system", "genesis"))

# ------------------------------------------------------------------ T004: declared requirements, exclusions and version ranges
base = project("base-lib", version="1.4.0", luau=True)
needs = project("needs-base", luau=True, extra=dep("base-lib", ">=1.2.0 <2.0.0"))
ok("satisfied", "mod", "set", "check", base, needs)
ok("the position of the required mod does not matter", "mod", "set", "check", needs, base)
ok("any version", "mod", "set", "check", base, project("needs-any", luau=True, extra=dep("base-lib")))
refused("missing", "E_SET_MISSING_DEPENDENCY", "mod", "set", "check", needs, says=('"needs-base"', '"base-lib"', ">=1.2.0 <2.0.0"), mods=["needs-base", "base-lib"])
refused("version mismatch", "E_SET_VERSION_MISMATCH", "mod", "set", "check", project("base-lib", version="2.0.0", luau=True), needs, says=("2.0.0",), mods=["needs-base", "base-lib"])
foe = project("foe", luau=True, extra=excl("base-lib", "<=1.4.0"))
refused("incompatible", "E_SET_INCOMPATIBLE", "mod", "set", "check", base, foe, says=('"foe"', '"base-lib"'), mods=["foe", "base-lib"])
ok("an exclusion outside its range", "mod", "set", "check", project("base-lib", version="1.5.0", luau=True), foe)
refused("the same mod twice", "E_SET_DUPLICATE_MOD", "mod", "set", "check", base, project("base-lib", version="1.5.0", luau=True), says=("1.4.0", "1.5.0"))
many = project("many", luau=True, extra=dep("absent-one") + dep("base-lib", ">9.0.0") + excl("foe"))
doc = refused("every violated rule is listed", "E_SET_MISSING_DEPENDENCY", "mod", "set", "check", many, base, foe)
got = [(v["class"], v["mods"]) for v in doc.get("violations", [])]
cli.check(got == [("E_SET_MISSING_DEPENDENCY", ["many", "absent-one"]), ("E_SET_VERSION_MISMATCH", ["many", "base-lib"]), ("E_SET_INCOMPATIBLE", ["many", "foe"]), ("E_SET_INCOMPATIBLE", ["foe", "base-lib"])], f"all violations in order: {got}")
rc, again, _ = cli.run("mod", "set", "check", many, base, foe)
cli.check(again == doc, "verification is deterministic")
doc = rejected_composition("relations and edit conflicts are reported together", "E_SET_MISSING_DEPENDENCY", [project("lonely", edits=f"rev-a u16 0x00000400 {orig(0x400, 2)} 0x4444\n", extra=dep("nobody")), edit_a])
cli.check([v["class"] for v in doc.get("violations", [])] == ["E_SET_MISSING_DEPENDENCY", "E_SET_EDIT_OVERLAP"], f"both: {doc.get('violations')}")
refused("17 members", "E_SET_SIZE", "mod", "set", "check", *([base] * 17))

# the manifest keys: canonical bytes equal the independent builder's; the declaration rules fail closed in mod.toml and in the reader
inspected = ok("inspect shows relations", "mod", "inspect", needs)
cli.check(inspected.get("dependencies") == [{"id": "base-lib", "version": ">=1.2.0 <2.0.0"}] and "incompatibilities" not in inspected and "dependencies" not in ok("inspect without", "mod", "inspect", base), f"{inspected}")
lua = luau()
manifest = base_manifest(payloads=[lua], dependencies=[{"id": "aaa", "version": "=1.0.0"}, {"id": "bbb"}], incompatibilities=[{"id": "ccc", "version": "<1.0.0"}])
rel = project("canon", luau=False, extra=dep("bbb") + dep("aaa", "=1.0.0") + excl("ccc", "<1.0.0"))
text = (rel / "mod.toml").read_text().replace('id = "canon"\nversion = "1.0.0"\nname = "canon"\nlicense = "CC0-1.0"', 'id = "example-mod"\nversion = "1.0.0"\nname = "Example"').replace(sha(ROM), SRC_SHA)
write(rel / "mod.toml", text + '\n[[source]]\nkind = "luau-source"\npath = "scripts/main.luau"\n')
write(rel / "scripts" / "main.luau", b"-- hello")
ok("pack canonical", "mod", "pack", rel, "--out", tmp / "canon.segapatch")
cli.check(zipfile.ZipFile(tmp / "canon.segapatch").read("manifest.json") == canon(manifest), "the packed relations are the canonical bytes of the independent builder (sorted by id, omitted when empty)")


def reader(what, code, **extra):
    path = write(tmp / "reader.segapatch", build(base_manifest(payloads=[lua], **extra), [lua]))
    refused("reader: " + what, code, "mod", "inspect", path)


reader("an empty list", "E_RELATION", dependencies=[])
reader("unsorted", "E_RELATION", dependencies=[{"id": "bbb"}, {"id": "aaa"}])
reader("repeated", "E_RELATION", incompatibilities=[{"id": "aaa"}, {"id": "aaa", "version": "=1.0.0"}])
reader("itself", "E_RELATION", dependencies=[{"id": "example-mod"}])
reader("both required and excluded", "E_RELATION", dependencies=[{"id": "aaa"}], incompatibilities=[{"id": "aaa"}])
reader("a bad id", "E_RELATION", dependencies=[{"id": "Aaa"}])
reader("17 entries", "E_RELATION", dependencies=[{"id": f"m{i:02d}"} for i in range(17)])
reader("an unknown field", "E_FIELD_UNKNOWN", dependencies=[{"id": "aaa", "optional": "1"}])
reader("the schema 1/2 spelling", "E_FIELD_UNKNOWN", requires=["aaa"])
for bad in ["", "1.0.0", "^1.0.0", "~1.0.0", "1.x", ">= 1.0.0", ">=1.0", ">=01.0.0", ">=1.0.0  <2.0.0", ">=1.0.0,<2.0.0", ">=1.0.0 || <0.1.0", ">=1.0.0 <2.0.0 >0.1.0 <3.0.0 >0.2.0", "==1.0.0", ">=1.0.0 "]:
    reader(f"the range {bad!r}", "E_VERSION_RANGE", dependencies=[{"id": "aaa", "version": bad}])
for what, extra, code in [("self", dep("self-mod"), "E_RELATION"), ("empty range", dep("aaa", ""), "E_VERSION_RANGE"), ("bad range", excl("aaa", "latest"), "E_VERSION_RANGE"),
                          ("repeated", dep("aaa") + dep("aaa", "=1.0.0"), "E_RELATION"), ("unknown key", '\n[[dependency]]\nid = "aaa"\nurl = "x"\n', "E_FIELD_UNKNOWN")]:
    refused("mod.toml " + what, code, "mod", "check", project("self-mod", luau=True, extra=extra))

# ------------------------------------------------------------------ T001: persisted definitions, composite identity, cache reuse and rebuild reasons
LIB = tmp / "library"


def install(d, approve=True):
    pkg = tmp / (d.name + ".segapatch")
    packed = ok(f"pack {d.name}", "mod", "pack", d, "--out", pkg)
    ident = packed.get("package_identity", "")
    args = ["mod", "install", pkg, "--rom", ROM_PATH, "--library", LIB] + (["--approve", ident] if approve else [])
    ok(f"install {d.name}", *args)
    return ident


I_EDIT, I_CODE, I_LUAU = install(edit_a), install(code_c), install(script)


def save(name, *identities):
    args = ["mod", "set", "save", "--library", LIB, "--name", name]
    for i in identities:
        args += ["--identity", i]
    return ok(f"save {name}", *args)


def variant(name, **kw):
    args = ["mod", "set", "variant", "--library", LIB, "--name", name, "--rom", ROM_PATH]
    for k, v in kw.items():
        args += [f"--{k.replace('_', '-')}", v]
    return ok(f"variant {name}", *args)


def single_variant(identity):
    return ok("single variant", "mod", "variant", "--library", LIB, "--identity", identity, "--rom", ROM_PATH)


def archive_identity(path):
    return sha(zipfile.ZipFile(path).read("manifest.json"))


# one member: EXACTLY the single mod's identity and cache entry
alone = single_variant(I_LUAU)
save("solo", I_LUAU)
solo = variant("solo")
cli.check(solo.get("cache") == "hit" and solo.get("variant_key") == alone.get("variant_key") and solo.get("components") == alone.get("components") and "set_format" not in solo.get("components", {}),
          f"a one-member set reproduces the single-mod identity and reuses its build: {solo} {alone}")

save("trio", I_EDIT, I_CODE, I_LUAU)
cli.check((LIB / "mod-sets" / "trio.modset").read_text() == f"segarecomp-modset=1\nplatform=genesis\nmember={I_EDIT}\nmember={I_CODE}\nmember={I_LUAU}\n", "the persisted definition is versioned, ordered and canonical")
shown = ok("show", "mod", "set", "show", "--library", LIB, "--name", "trio")
cli.check(shown.get("format") == 1 and [m["package_identity"] for m in shown.get("members", [])] == [I_EDIT, I_CODE, I_LUAU], f"{shown}")
first = variant("trio")
parts = first.get("components", {})
cli.check(first.get("cache") == "miss" and first.get("reasons") == ["no_previous_build"], f"first build: {first}")
cli.check(list(parts)[:7] == ["set_format", "platform", "source_kind", "source_identity", "adapter_capabilities", "membership", "effective_image"] and list(parts)[7:] == ["member.0", "member.1", "member.2"],
          f"component order: {list(parts)}")
cli.check(parts.get("set_format") == "1" and parts.get("platform") == "genesis" and parts.get("source_kind") == "cartridge-image" and parts.get("source_identity") == f"image={sha(ROM)};mapper=none", f"{parts}")
cli.check(parts.get("member.2") == f"{I_LUAU}:{alone.get('variant_key')}", "each member is bound with its own single-mod key (which covers its compiled Luau)")
cli.check(parts.get("membership") == sha("".join(i + "\n" for i in sorted([I_EDIT, I_CODE, I_LUAU])).encode()), "membership is order independent")
cli.check(first.get("variant_key") == sha("".join(f"{k}={v}\n" for k, v in parts.items()).encode()), "the key is the digest of the ordered components")
_, both = composed("the same members", [edit_a, code_c, script])
cli.check(parts.get("effective_image") == sha(both), "the identity binds the composed image")
second = variant("trio")
cli.check(second.get("cache") == "hit" and second.get("reasons") == [] and second.get("variant_key") == first.get("variant_key"), f"identical set reuses the build: {second}")
save("trio-copy", I_EDIT, I_CODE, I_LUAU)
cli.check(variant("trio-copy").get("cache") == "hit", "the name is not part of the identity: an identical set under another name reuses the build")

save("trio", I_LUAU, I_EDIT, I_CODE)
reordered = variant("trio")
cli.check(reordered.get("cache") == "miss" and reordered.get("variant_key") != first.get("variant_key") and reordered.get("reasons") == ["member.0", "member.1", "member.2"],
          f"order changes the identity and the reasons name the positions (not membership, not the image): {reordered}")
save("trio", I_EDIT, I_CODE)
fewer = variant("trio")
cli.check(fewer.get("cache") == "miss" and fewer.get("variant_key") not in (first.get("variant_key"), reordered.get("variant_key")) and "membership" in fewer.get("reasons", []), f"membership changes the identity: {fewer}")
save("trio", I_EDIT, I_CODE, I_LUAU)
back = variant("trio")
cli.check(back.get("cache") == "hit" and back.get("variant_key") == first.get("variant_key"), f"going back reuses the earlier build: {back}")

# what a record holds IS the identity it is filed under: each cached archive, composite or single, hashes to its member identity and equals the installed bytes
for ident in (I_EDIT, I_CODE, I_LUAU):
    cached = LIB / "cache" / "variants" / first["variant_key"] / "members" / f"{ident}.segapatch"
    cli.check(archive_identity(cached) == ident and cached.read_bytes() == (LIB / "mods" / f"{ident}.segapatch").read_bytes(), f"the cached member is the verified package: {ident}")
cached = LIB / "cache" / "variants" / alone["variant_key"] / "package.segapatch"
cli.check(archive_identity(cached) == I_LUAU and cached.read_bytes() == (LIB / "mods" / f"{I_LUAU}.segapatch").read_bytes(), "the single-mod record holds the verified package too")

# a tampered entry is a miss and is rebuilt, never reused
entry = LIB / "cache" / "variants" / first["variant_key"]
for victim in [entry / f"luau-{I_LUAU}.bin", entry / "members" / f"{I_EDIT}.segapatch", entry / "set.modset", entry / "variant.txt"]:
    good = victim.read_bytes()
    victim.write_bytes(good + b"x")
    rebuilt = variant("trio")
    cli.check(rebuilt.get("cache") == "miss" and victim.read_bytes() == good, f"a modified {victim.name} is rebuilt: {rebuilt}")

# ONE member, same integrity rule: a cached archive that is damaged, truncated, empty, missing or another VALID package is a miss that restores the verified bytes. Both routes share the one key and entry,
# with and without compiled Luau (the compiled-output check alone never covered the archive).
save("solo-data", I_EDIT)
for ident, set_name in [(I_EDIT, "solo-data"), (I_LUAU, "solo")]:
    base_record = single_variant(ident)
    cached = LIB / "cache" / "variants" / base_record["variant_key"] / "package.segapatch"
    good = (LIB / "mods" / f"{ident}.segapatch").read_bytes()
    another = (LIB / "mods" / f"{I_CODE}.segapatch").read_bytes()
    entries = sorted(p.name for p in (LIB / "cache" / "variants").iterdir())
    damages = [("a flipped byte of the same size", lambda: cached.write_bytes(good[:40] + bytes([good[40] ^ 1]) + good[41:])), ("appended bytes", lambda: cached.write_bytes(good + b"x")),
               ("truncated", lambda: cached.write_bytes(good[:-1])), ("empty", lambda: cached.write_bytes(b"")), ("missing", lambda: cached.unlink()), ("another valid package", lambda: cached.write_bytes(another))]
    for route, run_variant in [("mod variant", lambda: single_variant(ident)), ("one-member set", lambda: variant(set_name))]:
        before_hit = run_variant()
        cli.check(before_hit.get("cache") == "hit" and cached.read_bytes() == good, f"{route}: a valid entry is a hit: {before_hit}")
        for what, damage in damages:
            damage()
            rebuilt = run_variant()
            cli.check(rebuilt.get("cache") == "miss" and cached.is_file() and cached.read_bytes() == good, f"{route}, {what}: a miss that restores the verified package: {rebuilt}")
            cli.check(rebuilt.get("variant_key") == base_record["variant_key"] and rebuilt.get("components") == base_record["components"], f"{route}, {what}: the single-mod identity is unchanged")
            again = run_variant()
            cli.check(again.get("cache") == "hit", f"{route}, {what}: the restored entry is a hit: {again}")
    cli.check(sorted(p.name for p in (LIB / "cache" / "variants").iterdir()) == entries, "no other cache entry appears")

# a native member needs its own approval, and its compile-only probe runs; a failed build publishes nothing
I_NATIVE = install(project("gameplay-native", native=True), approve=False)
save("with-native", I_LUAU, I_NATIVE, I_EDIT)
refused("a native member without approval", "E_NOT_APPROVED", "mod", "set", "variant", "--library", LIB, "--name", "with-native", "--rom", ROM_PATH, says=("gameplay-native",))
ok("approve", "mod", "install", tmp / [p for p in tmp.glob("*-gameplay-native.segapatch")][0].name, "--rom", ROM_PATH, "--library", LIB, "--approve", I_NATIVE)
before = sorted(p.name for p in (LIB / "cache" / "variants").iterdir())
refused("a native member without a compiler", "E_COMPILE_FAILED", "mod", "set", "variant", "--library", LIB, "--name", "with-native", "--rom", ROM_PATH)
cli.check(sorted(p.name for p in (LIB / "cache" / "variants").iterdir()) == before, "a failed build publishes nothing")
mixed = variant("with-native", cc=CC, sdk_dir=PLATFORMS)
cli.check(mixed.get("cache") == "miss" and variant("with-native", cc=CC, sdk_dir=PLATFORMS).get("cache") == "hit", f"Luau + native + typed edit has a stable composite identity: {mixed}")

# refusals on definitions
I_NEEDS = install(needs)
refused("saving a set with a violated rule", "E_SET_MISSING_DEPENDENCY", "mod", "set", "save", "--library", LIB, "--name", "broken", "--identity", I_NEEDS)
cli.check(not (LIB / "mod-sets" / "broken.modset").exists(), "a refused save writes nothing")
refused("a single-mod variant never ignores a requirement", "E_SET_MISSING_DEPENDENCY", "mod", "variant", "--library", LIB, "--identity", I_NEEDS, "--rom", ROM_PATH)
refused("nor does the build route", "E_SET_MISSING_DEPENDENCY", "mod", "set", "save", "--library", LIB, "--name", "broken", "--identity", I_NEEDS, "--identity", I_EDIT)
built = tmp / "build-out"
run = cli.run("build", "--rom", ROM_PATH, "--output", built, "--cc", CC, "--runtime-dir", Path(PLATFORMS) / "genesis", "--luau-package", tmp / (needs.name + ".segapatch"), "--luau-identity", I_NEEDS)
status = (built / "status.json").read_text() if (built / "status.json").exists() else ""
cli.check(run[0] != 0 and "E_SET_MISSING_DEPENDENCY" in status, f"`segarecomp build` refuses a mod whose requirement it cannot honour: rc={run[0]} {status[-300:]}")
# every route that RUNS a mod alone refuses an unmet requirement, including the data-only one (built from its effective image by a plain build) and the Luau fixture session
data_needs = project("needs-data", edits=f"rev-a u8 0x00000700 0x{ROM[0x700]:02x} 0x00\n", extra=dep("base-lib"))
(data_needs / "tests.toml").write_text('[[scenario]]\nname = "one"\nframes = 1\nbudget = 1000\ntimeout = 30\n')
run_args = ["--rom", ROM_PATH, "--cc", CC, "--sdk-dir", PLATFORMS]
for what, target in [("data-only", data_needs), ("Luau", needs)]:
    refused(f"diff-run of a {what} mod with a requirement", "E_SET_MISSING_DEPENDENCY", "mod", "diff-run", target, *run_args, "--frames", "1", "--budget", "1000", "--timeout", "30")
refused("mod test of a data-only mod with a requirement", "E_SET_MISSING_DEPENDENCY", "mod", "test", data_needs, *run_args)
refused("a fixture session of a mod with a requirement", "E_SET_MISSING_DEPENDENCY", "mod", "session", needs, "--rom", ROM_PATH, "--frames", "1")
refused("an unknown identity", "E_NOT_INSTALLED", "mod", "set", "save", "--library", LIB, "--name", "x", "--identity", "0" * 64)
refused("a package twice", "E_SET_DUPLICATE_MOD", "mod", "set", "save", "--library", LIB, "--name", "x", "--identity", I_EDIT, "--identity", I_EDIT)
refused("a name with a path", "E_FIELD_VALUE", "mod", "set", "save", "--library", LIB, "--name", "../x", "--identity", I_EDIT)
refused("an unknown set", "E_SET_UNKNOWN", "mod", "set", "show", "--library", LIB, "--name", "nothing")
for what, body, code in [("a newer format", f"segarecomp-modset=2\nplatform=genesis\nmember={I_EDIT}\n", "E_SET_VERSION"), ("not a definition", "hello\n", "E_SET_SYNTAX"),
                         ("no member", "segarecomp-modset=1\nplatform=genesis\n", "E_SET_SIZE"), ("an unknown key", f"segarecomp-modset=1\nplatform=genesis\nmember={I_EDIT}\nextra=1\n", "E_SET_SYNTAX"),
                         ("no final newline", f"segarecomp-modset=1\nplatform=genesis\nmember={I_EDIT}", "E_SET_SYNTAX"), ("a repeated member", f"segarecomp-modset=1\nplatform=genesis\nmember={I_EDIT}\nmember={I_EDIT}\n", "E_SET_DUPLICATE_MOD"),
                         ("an unknown platform", f"segarecomp-modset=1\nplatform=saturn\nmember={I_EDIT}\n", "E_PLATFORM_UNKNOWN"), ("another platform than its members", f"segarecomp-modset=1\nplatform=master-system\nmember={I_EDIT}\n", "E_SET_PLATFORM")]:
    write(LIB / "mod-sets" / "hand.modset", body)
    refused("a stored definition: " + what, code, "mod", "set", "show", "--library", LIB, "--name", "hand")
records = sorted(str(p.relative_to(LIB)) for p in (LIB / "cache").rglob("*"))
(LIB / "mods" / f"{I_CODE}.segapatch").write_bytes((LIB / "mods" / f"{I_EDIT}.segapatch").read_bytes())
refused("a replaced installed package", "E_PACKAGE_CHANGED", "mod", "set", "variant", "--library", LIB, "--name", "trio", "--rom", ROM_PATH)
refused("a replaced installed package, single route", "E_PACKAGE_CHANGED", "mod", "variant", "--library", LIB, "--identity", I_CODE, "--rom", ROM_PATH)
cli.check(sorted(str(p.relative_to(LIB)) for p in (LIB / "cache").rglob("*")) == records, "a replaced package publishes nothing")
cli.check(cli.run("mod", "set")[0] == 2 and cli.run("mod", "set", "check")[0] == 2 and cli.run("mod", "set", "apply", edit_a, "--rom", ROM_PATH)[0] == 2, "usage errors exit 2")
refused("the output may not be an input", "E_OUTPUT_IS_INPUT", "mod", "set", "apply", edit_a, "--rom", ROM_PATH, "--out", ROM_PATH)

# ------------------------------------------------------------------ outputs never touch an input: a member PROJECT FOLDER protects its manifest and every source
OUT = tmp / "guard"
OUT.mkdir()
ga = project("guard-a", edits=f"rev-a u8 0x00000400 0x{ROM[0x400]:02x} 0x01\n")
(ga / "edits" / "keep.partial").write_text("an author's file with the staging suffix\n")
gb = project("guard-b", asm="free * 0x1000 0x100 0xff\norg * 0x1000\n  rts\n")
(gb / "scripts").mkdir()
(gb / "scripts" / "notes.luau").write_text("-- not declared, still the author's\n")
gp = OUT / "guard-pkg.segapatch"
ok("pack a package member", "mod", "pack", edit_b, "--out", gp)


def snapshot():
    files = [p for d in (ga, gb) for p in sorted(d.rglob("*"))] + [ROM_PATH, gp]
    return {str(p): (p.read_bytes() if p.is_file() else None) for p in files}


before = snapshot()


def guarded(what, out_path, cwd=None):
    for route in (("mod", "set", "apply", ga, gb, gp), ("mod", "apply", ga), ("mod", "bps", ga)):
        if os.path.normpath(str(out_path)).startswith(str(gb) + os.sep) and route[1] != "set":
            continue   # guard-b is an input of the set route only
        rc, doc, p = cli.run(*route, "--rom", ROM_PATH, "--out", out_path, cwd=cwd)
        cli.check(rc == 1 and doc and doc.get("class") == "E_OUTPUT_IS_INPUT", f"{what} ({route[1]}): expected E_OUTPUT_IS_INPUT, got rc={rc} {doc} {p.stderr[-200:]}")
        cli.check(snapshot() == before, f"{what} ({route[1]}): every input is byte-identical and nothing was added")


guarded("a member manifest", ga / "mod.toml")
guarded("the second member's manifest", gb / "mod.toml")
guarded("a declared edit source", ga / "edits" / "main.edits")
guarded("a declared assembly source", gb / "asm" / "main.asm")
guarded("an undeclared project file", gb / "scripts" / "notes.luau")
guarded("a new file inside a member", ga / "composed.bin")
guarded("a new file in a new subfolder of a member", ga / "build" / "composed.bin")
guarded("the staging path would remove a project file", ga / "edits" / "keep")
guarded("through ..", gb / ".." / ga.name / "mod.toml")
guarded("relative from inside the project", "mod.toml", cwd=ga)
guarded("the original ROM", ROM_PATH)
rc, doc, _ = cli.run("mod", "set", "apply", ga, gp, "--rom", ROM_PATH, "--out", gp)
cli.check(rc == 1 and doc.get("class") == "E_OUTPUT_IS_INPUT" and snapshot() == before, f"a package-file member: {doc}")
# the staging file `<out>.partial` is the member folder itself
staged = project("guard-stage", edits=f"rev-a u8 0x00000401 0x{ROM[0x401]:02x} 0x01\n")
staged = staged.rename(tmp / "stage-target.partial")
rc, doc, _ = cli.run("mod", "set", "apply", staged, "--rom", ROM_PATH, "--out", tmp / "stage-target")
cli.check(rc == 1 and doc.get("class") == "E_OUTPUT_IS_INPUT" and (staged / "mod.toml").is_file(), f"the staging path is a member folder: {doc}")
# links from outside the project (a host without them skips only that case)
for what, make, name in [("a symlinked folder", lambda at: at.symlink_to(ga, target_is_directory=True), "alias-dir"),
                         ("a symlink to a source", lambda at: at.symlink_to(ga / "mod.toml"), "alias-file"),
                         ("a hard link to a source", lambda at: os.link(ga / "mod.toml", at), "hard-file"),
                         ("a symlinked staging path", lambda at: at.symlink_to(ga / "mod.toml"), "viastage.partial"),
                         ("a hard-linked staging path", lambda at: os.link(ga / "mod.toml", at), "viahard.partial")]:
    link = OUT / name
    try:
        make(link)
    except OSError:
        print(f"mod_set_test: {what} is not available on this host")
        continue
    target = link / "mod.toml" if name == "alias-dir" else OUT / name[:-len(".partial")] if name.endswith(".partial") else link
    guarded(what, target)
    link.unlink()
# an unrelated output still works, including a sibling that shares the member's name as a prefix, and an existing unrelated hard-linked file is replaced
for good in [OUT / "composed.bin", ga.parent / (ga.name + "-composed.bin")]:
    ok(f"an unrelated output {good.name}", "mod", "set", "apply", ga, gb, gp, "--rom", ROM_PATH, "--out", good)
    cli.check(good.is_file() and good.stat().st_size == len(ROM), "the composed image is written")
(OUT / "other").write_bytes(b"x")
try:
    os.link(OUT / "other", OUT / "other-link")
    ok("an unrelated hard-linked output", "mod", "set", "apply", ga, gb, "--rom", ROM_PATH, "--out", OUT / "other-link")
    cli.check((OUT / "other").read_bytes() == b"x", "the other name keeps its bytes")
except OSError:
    pass
cli.check(snapshot() == before, "the successful runs changed no input")

# ------------------------------------------------------------------ SEG-141-T006: `mod set build` refusals that happen before any compiler runs, and the saved-definition classes through it
def build_refused(what, code, members, *approvals, rom=ROM_PATH, **kw):
    out = tmp / ("setbuild-" + str(len(GOLDEN.get(code, []))) + code)
    args = ["mod", "set", "build", *members, "--rom", rom, "--out", out, "--cc", CC, "--sdk-dir", PLATFORMS]
    for a in approvals:
        args += ["--approve", a]
    refused(what, code, *args, **kw)
    cli.check(not (out / "game").exists() and not (out / "game.exe").exists(), f"{what}: no executable")


def identity_of(d):
    return ok("check " + d.name, "mod", "check", d)["package_identity"]


def drawing(mod_id):
    d = project(mod_id, luau=True)
    toml = d / "mod.toml"
    write(toml, toml.read_text().replace('required = ["host.log"]', 'required = ["host.log", "overlay.rgba"]').encode())
    return d


native_one = project("needs-approval", native=True)
build_refused("a native member without approval", "E_NOT_APPROVED", [edit_a, native_one])
build_refused("two drawing mods", "E_SET_PRESENTATION_CONFLICT", [drawing("draw-one"), drawing("draw-two")])
sound = project("sound-mod", luau=True, extra='\n[[audio.mapping]]\nid = "now"\nkind = "event"\nspace = "m68k.work_ram"\noffset = 112\nwidth = 1\n\n[[audio.mapping.value]]\nname = "idle"\nvalue = 0\n')
write(sound / "mod.toml", (sound / "mod.toml").read_text().replace('required = ["host.log"]', 'required = ["audio.event.read", "host.log"]').encode())
build_refused("the audio API in a set", "E_SET_AUDIO_UNSUPPORTED", [edit_a, sound])
plain_hook = project("hook-plain", native=True, hooks=HOOK, symbols=(("fn", "0x00000600"),))
anchored = project("hook-anchored", native=True, hooks=HOOK + 'anchor = "other"\n', symbols=(("fn", "0x00000600"), ("other", "0x00000700")))
build_refused("hooked functions declared with different anchors", "E_SET_HOOK_ANCHOR", [plain_hook, anchored], identity_of(plain_hook), identity_of(anchored))
build_refused("a composition refusal reads the same from build", "E_SET_BPS_CONFLICT", [bps_a, edit_a])
geometry = project("wide-data", edits=f"rev-a u8 0x00000710 0x{ROM[0x710]:02x} 0x00\n", extra='\n[geometry]\naspect_max = "16:9"\n')
build_refused("a data-only geometry claim is refused, never dropped", "E_SET_GEOMETRY_UNSUPPORTED", [geometry, code_c])


def two_revisions(d, extra):
    """The project with a second declared revision `rev-b` and `extra` appended."""
    toml = d / "mod.toml"
    write(toml, (toml.read_text() + f'\n[[target.revision]]\nlabel = "rev-b"\nsha256 = "{sha(OTHER)}"\n' + extra).encode())
    return d


def with_caps(d, caps):
    toml = d / "mod.toml"
    write(toml, toml.read_text().replace('required = ["host.log"]', f"required = [{caps}]").encode())
    return d


script_hook = project("script-hook", luau=True, extra=HOOK, symbols=(("fn", "0x00000600"),))
build_refused("a script hook has no callback", "E_HOOK_CALLBACK_UNSUPPORTED", [edit_a, with_caps(script_hook, '"hook.function", "host.log"')])
elsewhere_range = two_revisions(with_caps(project("range-elsewhere", luau=True), '"host.log", "memory.write.m68k.work_ram"'),
                                '\n[[memory]]\nrevision = "rev-b"\nspace = "m68k.work_ram"\noffset = 16\nlength = 2\naccess = "write"\n')
build_refused("a memory capability without a range for this revision", "E_RANGE_DECLARATION", [edit_a, elsewhere_range])
rom_reader = project("rom-reader", native=True)
write(rom_reader / "mod.toml", ((rom_reader / "mod.toml").read_text() + '\n[capabilities]\nrequired = ["memory.read.m68k.rom"]\n\n[[memory]]\nrevision = "rev-a"\nspace = "m68k.rom"\noffset = 8176\nlength = 64\naccess = "read"\n').encode())
build_refused("a ROM range beyond the composed image", "E_MEMORY_RANGE", [edit_a, rom_reader], identity_of(rom_reader))
other_hook = two_revisions(project("hook-elsewhere", native=True, hooks=HOOK.replace('id = "watch"', 'id = "watch"\nrevision = "rev-b"'), symbols=(("fn", "0x00000600"),)),
                           '\n[[symbol]]\nname = "fn"\nrevision = "rev-b"\naddress = "0x00000600"\nprovenance = "verified"\n')
build_refused("hooks that apply to no function of this revision", "E_HOOK_REVISION", [edit_a, other_hook], identity_of(other_hook))


def many_hooks(mod_id, base):
    names = [(f"fn{i}", f"0x{base + 4 * i:08x}") for i in range(40)]
    return project(mod_id, native=True, hooks="".join(f'\n[[hook]]\nid = "{mod_id}-{i:02d}"\nsymbol = "fn{i}"\nmode = "before"\nreads = ["d0"]\n' for i in range(40)), symbols=names)


crowd = [many_hooks("crowd-a", 0x400), many_hooks("crowd-b", 0x600)]
build_refused("more than 64 hooked functions across a set", "E_HOOK_COUNT", crowd, identity_of(crowd[0]), identity_of(crowd[1]))


def direct_build_refused(what, code, package, identity):
    """The two member-verification refusals only `segarecomp build --set-package` can reach (mod set build pins what it just read)."""
    out = tmp / ("direct-" + code)
    p = subprocess.run([sys.argv[1], "build", "--rom", str(ROM_PATH), "--output", str(out), "--cc", CC, "--runtime-dir", str(Path(PLATFORMS) / "genesis"), "--set-package", str(package), "--set-identity", identity],
                       text=True, capture_output=True, timeout=600)
    status = json.loads((out / "status.json").read_text()) if (out / "status.json").is_file() else {}
    cli.check(p.returncode != 0 and status.get("diagnostic") == code and not (out / "game").exists(), f"{what}: expected {code}, got {status}")
    GOLDEN.setdefault(code + " (build)", []).append(normalised({"case": what, "diagnostic": status.get("diagnostic"), "message": status.get("message")}))


packed_edit = tmp / "direct-member.segapatch"
ok("pack the direct member", "mod", "pack", edit_a, "--out", packed_edit)
direct_build_refused("a member that is not the pinned package", "E_PACKAGE_CHANGED", packed_edit, "0" * 64)
direct_build_refused("a member that cannot be read", "E_ARCHIVE_ENTRY", tmp / "no-such.segapatch", "0" * 64)

# two mods' hook ids that cannot be told apart even with the member suffix
def hook_text(hook_id, symbol):
    return f'\n[[hook]]\nid = "{hook_id}"\nsymbol = "{symbol}"\nmode = "before"\nreads = ["d0"]\n'


taken = project("ids-taken", native=True, hooks=hook_text("x", "fn-a") + hook_text("x-m1", "fn-b"), symbols=(("fn-a", "0x00000600"), ("fn-b", "0x00000700")))
clashing = project("ids-clash", native=True, hooks=hook_text("x", "fn-c"), symbols=(("fn-c", "0x00000800"),))
build_refused("hook ids that cannot be made unique", "E_SET_HOOK_SITE_ID", [taken, clashing], identity_of(taken), identity_of(clashing))

# a set-mod folder the build did not create is never deleted; the refusal's class and message are pinned
occupied = tmp / ("setbuild-" + str(len(GOLDEN.get("E_SET_ARTIFACT_WRITE", []))) + "E_SET_ARTIFACT_WRITE")
write(occupied / "set-mod" / "keep.txt", b"user data")
build_refused("a user folder where the build stages", "E_SET_ARTIFACT_WRITE", [edit_a, project("runs", luau=True)])
cli.check((occupied / "set-mod" / "keep.txt").read_bytes() == b"user data" and sorted(p.name for p in (occupied / "set-mod").iterdir()) == ["keep.txt"], "the user's set-mod folder is untouched")

# ------------------------------------------------------------------ no output of a package build may be an input: the composed image <out>/input/effective.rom first of all (it is removed and rewritten)
GUARD = tmp / "guard"
guard_members = [edit_a, edit_b]
member_packages = []
for i, d in enumerate(guard_members):
    package = GUARD / f"member-{i}.segapatch"
    package.parent.mkdir(parents=True, exist_ok=True)
    ok("pack a guard member", "mod", "pack", d, "--out", package)
    member_packages.append(package)
member_ids = [identity_of(d) for d in guard_members]


def tree(root):
    return {str(p.relative_to(root)): (os.readlink(p) if p.is_symlink() else p.read_bytes() if p.is_file() else "dir") for p in sorted(root.rglob("*"))}


def direct_args(out, rom, packages):
    argv = [sys.argv[1], "build", "--rom", str(rom), "--output", str(out), "--cc", CC, "--runtime-dir", str(Path(PLATFORMS) / "genesis")]
    for package, identity in zip(packages, member_ids):
        argv += ["--set-package", str(package), "--set-identity", identity]
    return argv


def guarded(what, area, out, rom, packages=None, cwd=None):
    """Both routes refuse with E_OUTPUT_IS_INPUT and change nothing under `area` (inputs, links and the output folder)."""
    before = tree(area)
    p = subprocess.run(direct_args(out, rom, packages or member_packages), text=True, capture_output=True, timeout=600, cwd=cwd)
    cli.check(p.returncode == 1 and "class=E_OUTPUT_IS_INPUT" in p.stdout and tree(area) == before, f"{what} (build --set-package): rc={p.returncode} {p.stdout[-300:]} changed={sorted(set(tree(area).items()) ^ set(before.items()))[:4] if tree(area) != before else []}")
    rc, doc, _ = cli.run("mod", "set", "build", *(packages or member_packages), "--rom", rom, "--out", out, "--cc", CC, "--sdk-dir", PLATFORMS, cwd=cwd)
    cli.check(rc == 1 and doc and doc.get("class") == "E_OUTPUT_IS_INPUT" and tree(area) == before, f"{what} (mod set build): rc={rc} {doc}")


def area(name):
    d = GUARD / name
    (d / "out" / "input").mkdir(parents=True)
    return d


a = area("direct")
original = write(a / "out" / "input" / "effective.rom", ROM)
guarded("the original at the generated image path", a, a / "out", original)
guarded("an equivalent spelling of the same path", a, a / "out" / "input" / ".." , original)
guarded("relative paths", a, Path("out"), Path("out/./input/../input/effective.rom"), cwd=a)
a = area("status-file")
status_rom = write(a / "out" / "status.json", ROM)
guarded("the original at another fixed build output", a, a / "out", status_rom)
a = area("generated-tree")
tree_rom = write(a / "out" / "obj" / "game.bin", ROM)
guarded("the original inside a folder the build replaces", a, a / "out", tree_rom)
a = area("member-at-image")
shutil.copy(member_packages[0], a / "out" / "input" / "effective.rom")
image = write(a / "game.bin", ROM)
guarded("a member package at the generated image path", a, a / "out", image, [a / "out" / "input" / "effective.rom", member_packages[1]])
try:
    a = area("symlink-destination")
    image = write(a / "game.bin", ROM)
    (a / "out" / "input" / "effective.rom").symlink_to(image)
    guarded("a link at the generated image path pointing to the original", a, a / "out", image)
    a = area("symlink-original")
    real = write(a / "out" / "input" / "effective.rom", ROM)
    (a / "game.bin").symlink_to(real)
    guarded("the original named through a link to the generated image path", a, a / "out", a / "game.bin")
    a = GUARD / "symlink-folder"
    (a / "out").mkdir(parents=True)
    real = write(a / "mine" / "effective.rom", ROM)
    (a / "out" / "input").symlink_to(a / "mine", target_is_directory=True)
    guarded("a linked input folder that holds the original", a, a / "out", real)
    a = area("symlink-log")
    image = write(a / "game.bin", ROM)
    (a / "out" / "build.log").symlink_to(image)
    guarded("a link at the build log pointing to the original", a, a / "out", image)
except OSError:
    print("mod_set_test: symbolic links are not available here; link collisions not exercised")
try:
    a = area("hardlink")
    image = write(a / "game.bin", ROM)
    os.link(image, a / "out" / "input" / "effective.rom")
    guarded("a hard link of the original at the generated image path", a, a / "out", image)
except OSError:
    print("mod_set_test: hard links are not available here")
# with performance reporting on, the build also writes and removes <out>/compiler-version.tmp (the compiler's version text), so that path is protected like every other fixed output
def profiled_refused(what, root, argv):
    before = tree(root)
    p = subprocess.run(argv + ["--performance-report", str(root / "report.json")], text=True, capture_output=True, timeout=600)
    cli.check(p.returncode == 1 and "class=E_OUTPUT_IS_INPUT" in p.stdout and tree(root) == before, f"{what}: rc={p.returncode} {p.stdout[-300:]} changed={tree(root) != before}")


a = area("version-file")
version_rom = write(a / "out" / "compiler-version.tmp", ROM)
profiled_refused("a profiled set build with the original at compiler-version.tmp", a, direct_args(a / "out", version_rom, member_packages))
cli.check(version_rom.is_file() and version_rom.read_bytes() == ROM, "the original's bytes are unchanged")
guarded("the original at compiler-version.tmp", a, a / "out", version_rom)
a = area("version-member")
shutil.copy(member_packages[0], a / "out" / "compiler-version.tmp")
image = write(a / "game.bin", ROM)
profiled_refused("a profiled set build with a member package at compiler-version.tmp", a, direct_args(a / "out", image, [a / "out" / "compiler-version.tmp", member_packages[1]]))
cli.check((a / "out" / "compiler-version.tmp").is_file() and (a / "out" / "compiler-version.tmp").read_bytes() == member_packages[0].read_bytes(), "the member package's bytes are unchanged")

# a single-mod package route shares the same generated image path and the same guard
a = area("single-route")
single = write(a / "out" / "input" / "effective.rom", ROM)
before = tree(a)
p = subprocess.run([sys.argv[1], "build", "--rom", str(single), "--output", str(a / "out"), "--cc", CC, "--runtime-dir", str(Path(PLATFORMS) / "genesis"), "--data-package", str(member_packages[0]), "--data-identity", member_ids[0]],
                   text=True, capture_output=True, timeout=600)
cli.check(p.returncode == 1 and "class=E_OUTPUT_IS_INPUT" in p.stdout and tree(a) == before, f"the single data-package route refuses too: {p.returncode} {p.stdout[-300:]}")

if os.environ.get("SEGARECOMP_UPDATE_GOLDEN") == "1":
    write(GOLDEN_PATH, (json.dumps(GOLDEN, indent=1, sort_keys=True) + "\n").encode())
golden = json.loads(GOLDEN_PATH.read_text()) if GOLDEN_PATH.is_file() else {}
cli.check(sorted(golden) == sorted(GOLDEN), f"every diagnostic class has a golden record: golden only {sorted(set(golden) - set(GOLDEN))}, new {sorted(set(GOLDEN) - set(golden))}")
for code in sorted(set(golden) & set(GOLDEN)):
    for want, got in zip(golden[code], GOLDEN[code]):
        cli.check(want == got, f"golden diagnostic {code} / {want.get('case')}: {got} != {want}")
    cli.check(len(golden[code]) == len(GOLDEN[code]), f"golden case count of {code}")

cli.done("mod_set_test")
