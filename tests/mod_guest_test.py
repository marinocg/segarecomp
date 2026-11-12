#!/usr/bin/env python3
"""SEG-136-T004/T005/T006 (ADR 0292, ADR 0293): bounded guest-code injection with static validation, collision/range checks, Genesis header repair and deterministic BPS construction,
through the product CLI (`segarecomp mod check|pack|apply|bps|verify-bps`). The expected bytes are hand-assembled and the BPS applier / checksum are written independently here, so the C++
assembler, validator, repair and writer are cross-checked on every host. Synthetic images only.
usage: mod_guest_test.py <segarecomp-cli>"""
import random
import struct
import sys
import zlib

from mod_test_support import *  # noqa: F401,F403

cli = Cli(sys.argv[1])
tmp = temp()


# ------------------------------------------------------------------ images
def pattern(n, mul, add):
    return bytearray(((i * mul + add) & 0xFF) for i in range(n))


def genesis_rom(size=0x2000, mul=7, add=3, free=(0x1000, 0x100, 0xFF), valid_header=False):
    img = pattern(size, mul, add)
    img[0x100:0x110] = b"SEGA MEGA DRIVE "
    start, length, fill = free
    img[start:start + length] = bytes([fill]) * length
    struct.pack_into(">I", img, 0x1A0, 0)
    struct.pack_into(">I", img, 0x1A4, size - 1)
    if valid_header:
        struct.pack_into(">H", img, 0x18E, checksum(img))
    return bytes(img)


def checksum(img, end=None):
    end = len(img) if end is None else end
    return sum(struct.unpack_from(">H", img, i)[0] for i in range(0x200, end, 2)) & 0xFFFF


def sms_rom(size=0x4000, free=(0x3000, 0x80, 0x00)):
    img = pattern(size, 5, 9)
    img[free[0]:free[0] + free[1]] = bytes([free[2]]) * free[1]
    return bytes(img)


A, B = genesis_rom(), genesis_rom(mul=11, add=5)
SHA_A, SHA_B = sha(A), sha(B)
_n = [0]


def toml(platform, revs, sources, extra=""):
    out = f'[mod]\nid = "guest-mod"\nversion = "1.0.0"\nname = "Guest Mod"\nlicense = "CC0-1.0"\n\n[target]\nplatform = "{platform}"\n\n'
    for label, digest, mapper in revs:
        out += f'[[target.revision]]\nlabel = "{label}"\nsha256 = "{digest}"\n' + (f'mapper = "{mapper}"\n' if mapper else "") + "\n"
    for src in sources:
        out += "[[source]]\n" + "".join(f'{k} = "{v}"\n' for k, v in src.items()) + "\n"
    return out + extra


def project(asm, platform="genesis", cpu="m68k", revs=None, edits=None, extra="", mapper=None):
    _n[0] += 1
    d = tmp / f"p{_n[0]}"
    revs = revs or [("rev-a", SHA_A, mapper), ("rev-b", SHA_B, mapper)]
    sources = [{"kind": "guest-asm", "path": "asm/main.asm", "cpu": cpu}]
    write(d / "asm" / "main.asm", asm)
    if edits is not None:
        sources.append({"kind": "rom-edit-set", "path": "edits/main.edits"})
        write(d / "edits" / "main.edits", edits)
    write(d / "mod.toml", toml(platform, revs, sources, extra))
    return d


def ok(what, *args):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: expected ok, got rc={rc} {doc} {p.stderr[-200:]}")
    return doc


def refused(what, code, *args):
    rc, doc, p = cli.run(*args)
    cli.check(rc == 1 and doc and doc.get("ok") is False and doc.get("class") == code, f"{what}: expected {code}, got rc={rc} {doc and (doc.get('class'), doc.get('error'))}")


def apply(d, image, name="out.bin"):
    out = tmp / name
    if out.exists():
        out.unlink()
    rc, doc, p = cli.run("mod", "apply", d, "--rom", write(tmp / "in.bin", image), "--out", out)
    return rc, doc, (out.read_bytes() if out.exists() else None)


FREE = "free * 0x1000 0x100 0xff\n"

# ------------------------------------------------------------------ T004: a synthetic M68K routine is assembled byte-exactly (hand-assembled expectation)
ASM = FREE + """org * 0x1000
entry:
  moveq #9,d1
  move.l #0x12345678,d3
  move.w d1,0xff0010        ; absolute write inside work RAM
  lea table(pc),a0
  move.b (a0)+,d4
  addq.w #1,d5
  add.l d1,d2
  lsl.w #2,d3
  jsr @helper
  bne.s entry
  bra.w done
done:
  rts
data
table:
  dc.b 1,2,3,4
"""
SYM = '\n[[symbol]]\nname = "helper"\nrevision = "{rev}"\naddress = "{addr}"\nprovenance = "verified"\n'
syms = SYM.format(rev="rev-a", addr="0x00000400") + SYM.format(rev="rev-b", addr="0x00000800")
d = project(ASM, extra=syms)
words = [0x7209, 0x263C, 0x1234, 0x5678, 0x33C1, 0x00FF, 0x0010]
# lea table(pc),a0 at 0x100E: extension word at 0x1010, table at the end
code = b"".join(struct.pack(">H", w) for w in words)
body = []
pc = 0x1000 + len(code)
lea_ext = pc + 2
tail_start = pc + 4 + 2 + 2 + 2 + 2 + 6 + 2 + 4 + 2   # lea(4) move.b(2) addq(2) add.l(2) lsl.w(2) jsr(6) bne.s(2) bra.w(4) rts(2)
table_at = tail_start
code += struct.pack(">HH", 0x41FA, (table_at - lea_ext) & 0xFFFF)
code += struct.pack(">H", 0x1818) + struct.pack(">H", 0x5245) + struct.pack(">H", 0xD481) + struct.pack(">H", 0xE54B)
helper_a, helper_b = 0x400, 0x800
jsr_at = 0x1000 + len(code)


def expected_for(helper):
    c = code + struct.pack(">HI", 0x4EB9, helper)
    bne_at = 0x1000 + len(c)
    c += struct.pack(">BB", 0x66, (0x1000 - (bne_at + 2)) & 0xFF)       # bne.s entry
    bra_at = 0x1000 + len(c)
    done_at = bra_at + 4
    c += struct.pack(">HH", 0x6000, (done_at - (bra_at + 2)) & 0xFFFF)   # bra.w done
    c += struct.pack(">H", 0x4E75) + bytes([1, 2, 3, 4])
    return c


ok("check the injected project", "mod", "check", d)
rc, doc, out_a = apply(d, A)
cli.check(rc == 0 and doc and doc["blocks_injected"] == 1 and doc["revision"] == "rev-a", f"apply rev-a: {doc}")
want = bytearray(A)
exp = expected_for(helper_a)
want[0x1000:0x1000 + len(exp)] = exp
cli.check(out_a == bytes(want), "rev-a: the injected block is byte-identical to the hand-assembled routine and nothing else changed")
rc, doc, out_b = apply(d, B)
want = bytearray(B)
exp_b = expected_for(helper_b)
want[0x1000:0x1000 + len(exp_b)] = exp_b
cli.check(rc == 0 and out_b == bytes(want), "rev-b: the same source assembles against that revision's verified symbol")
cli.check(doc and doc["injected_bytes"] == len(exp_b) and doc["edits_applied"] == 0, f"apply stats: {doc}")

# determinism: three runs give identical bytes and identical digests
runs = [apply(d, A, f"r{i}.bin") for i in range(3)]
cli.check(all(r[2] == runs[0][2] for r in runs) and len({r[1]["effective_sha256"] for r in runs}) == 1, "three runs produce identical bytes")

# ------------------------------------------------------------------ T004: invalid emitted code fails with a stable diagnostic
def asm_case(what, code_text, expect, prefix=FREE + "org * 0x1000\n", **kw):
    dd = project(prefix + code_text, **kw)
    refused(what, expect, "mod", "check", dd)


asm_case("branch out of short range", "start:\n  bra.s far\n" + "  nop\n" * 200 + "far:\n  rts\n", "E_GUEST_BRANCH_RANGE")
asm_case("numeric branch target", "  bra.w 0x1000\n", "E_GUEST_BRANCH_TARGET")
asm_case("stop is forbidden", "  stop #0x2700\n", "E_GUEST_FORBIDDEN_INSTRUCTION")
asm_case("trap is forbidden", "  trap #0\n", "E_GUEST_FORBIDDEN_INSTRUCTION")
asm_case("rte is forbidden", "  rte\n", "E_GUEST_FORBIDDEN_INSTRUCTION")
asm_case("reset is forbidden", "  reset\n", "E_GUEST_FORBIDDEN_INSTRUCTION")
asm_case("move to sr is forbidden", "  move.w d0,sr\n", "E_GUEST_FORBIDDEN_INSTRUCTION")
asm_case("raw opcode words in the code stream", "  dc.w 0x4e71,0x4e72\n", "E_GUEST_SYNTAX")
asm_case("single raw opcode word in the code stream", "  dc.w 0x4e72\n", "E_GUEST_SYNTAX")
asm_case("jump into data holding a stop", "  jmp l\ndata\nl:\n  dc.w 0x4e72,0x2700\n", "E_GUEST_BRANCH_TARGET")
asm_case("branch into data", "  bra.w l\ndata\nl:\n  dc.w 0x4e71\n", "E_GUEST_BRANCH_TARGET")
asm_case("code falling into data", "  nop\ndata\n  dc.w 0x4e72\n", "E_GUEST_SYNTAX")
asm_case("label arithmetic in a target", "s:\n  jmp s+0x7ffff000\n", "E_GUEST_BRANCH_TARGET")
ok("data after an unconditional transfer is fine", "mod", "check", project(FREE + "org * 0x1000\n  rts\ndata\nt:\n  dc.w 0x4e72\ncode\n  lea t(pc),a0\n  rts\n"))
asm_case("write to ROM", "  move.w d1,0x001000\n", "E_GUEST_WRITE_OUTSIDE_RAM")
asm_case("write straddling the RAM window", "  move.l d1,0xfeffff\n", "E_GUEST_WRITE_OUTSIDE_RAM")
asm_case("write to the VDP is not RAM", "  clr.w 0xc00004\n", "E_GUEST_WRITE_OUTSIDE_RAM")
asm_case("unknown label", "  bra.w nowhere\n", "E_GUEST_LABEL_UNKNOWN")
asm_case("duplicate label", "a:\n  nop\na:\n  nop\n", "E_GUEST_LABEL_DUPLICATE")
asm_case("unknown mnemonic", "  fmove d0,d1\n", "E_GUEST_SYNTAX")
asm_case("upper case is refused", "  NOP\n", "E_GUEST_SYNTAX")
asm_case("raw bytes in code", "  dc.b 1,2\n", "E_GUEST_SYNTAX")
asm_case("missing size suffix", "  clr d0\n", "E_GUEST_SYNTAX")
asm_case("immediate out of range", "  moveq #200,d0\n", "E_GUEST_VALUE_RANGE")
asm_case("odd instruction address", "  rts\ndata\n  dc.b 1\ncode\n  nop\n", "E_GUEST_ALIGN")
asm_case("unverified symbol", "  jsr @helper\n", "E_SYMBOL_UNKNOWN")
asm_case("m68k org with at", "", "E_GUEST_ADDRESS", prefix=FREE + "org * 0x1000 at 0x2000\n  nop\n")
asm_case("odd org offset", "", "E_GUEST_ALIGN", prefix=FREE + "org * 0x1001\n  nop\n")
asm_case("block outside any free range", "", "E_GUEST_OUTSIDE_FREE_RANGE", prefix=FREE + "org * 0x1100\n  nop\n")
asm_case("block crossing the end of its free range", "", "E_GUEST_OUTSIDE_FREE_RANGE", prefix=FREE + "org * 0x10fe\n  nop\n  nop\n")
asm_case("block beyond the 4 MiB window", "", "E_MAPPER_UNSUPPORTED", prefix="free * 0x400000 0x100 0xff\norg * 0x400000\n  nop\n")
asm_case("undeclared revision", "", "E_GUEST_REVISION", prefix="free rev-z 0x1000 0x100 0xff\norg rev-z 0x1000\n  nop\n")
asm_case("statement before any org", "", "E_GUEST_SYNTAX", prefix="  nop\n")
asm_case("empty block", "", "E_GUEST_SYNTAX", prefix=FREE + "org * 0x1000\n")
jsr_manual = project(FREE + "org * 0x1000\n  jsr @helper\n", extra=SYM.format(rev="rev-a", addr="0x00000400").replace("verified", "manual") + SYM.format(rev="rev-b", addr="0x00000400"))
refused("a manual symbol cannot be a branch target", "E_SYMBOL_NOT_VERIFIED", "mod", "check", jsr_manual)

# the original image must have the declared free bytes
bad_free = genesis_rom(free=(0x1000, 0x100, 0x00))
dd = project(FREE + "org * 0x1000\n  nop\n", revs=[("rev-a", sha(bad_free), None)])
ok("the project itself is well-formed", "mod", "check", dd)
rc, doc, _ = apply(dd, bad_free)
cli.check(rc == 1 and doc and doc["class"] == "E_GUEST_RANGE_NOT_FREE", f"original bytes differing from the declared fill are refused at build: {doc}")
short = genesis_rom()[:0x1080]
dd = project(FREE + "org * 0x1000\n  nop\n", revs=[("rev-a", sha(short), None)])
rc, doc, _ = apply(dd, short)
cli.check(rc == 1 and doc and doc["class"] == "E_GUEST_RANGE", f"a free range beyond the image is refused: {doc}")
ssf = bytearray(genesis_rom())
ssf[0x100:0x108] = b"SEGA SSF"
rc, doc, _ = apply(project(FREE + "org * 0x1000\n  nop\n", revs=[("rev-a", sha(bytes(ssf)), None)]), bytes(ssf))
cli.check(rc == 1 and doc and doc["class"] == "E_MAPPER_UNSUPPORTED", f"a bank-switched cartridge mapper is refused: {doc}")

# ------------------------------------------------------------------ T004: payload combination and platform rules
refused("two guest-asm", "E_PAYLOAD_COMBINATION", "mod", "check", (lambda dd: (write(dd / "asm" / "b.asm", FREE + "org * 0x1000\n  nop\n"), write(dd / "mod.toml", (dd / "mod.toml").read_text() + '[[source]]\nkind = "guest-asm"\npath = "asm/b.asm"\ncpu = "m68k"\n'), dd)[2])(project(FREE + "org * 0x1000\n  nop\n")))
refused("m68k guest-asm on master-system", "E_PAYLOAD_PLATFORM", "mod", "check", project(FREE + "org * 0x1000\n  nop\n", platform="master-system", revs=[("rev-a", sha(sms_rom()), "rom_only")]))

# ------------------------------------------------------------------ T004: Z80 (Master System and the Genesis sound CPU)
SMS = sms_rom()
Z_ASM = "free * 0x3000 0x80 0x00\norg * 0x3000\n  ld a,5\n  ld b,a\n  ld hl,0xc010\n  ld (hl),a\n  ld (0xc020),a\nloop:\n  dec b\n  jr nz,loop\n  push hl\n  pop hl\n  call sub\n  ret\nsub:\n  ret\n"
dz = project(Z_ASM, platform="master-system", cpu="z80", revs=[("rev-a", sha(SMS), "rom_only")])
rc, doc, out = apply(dz, SMS)
z_exp = bytes([0x3E, 5, 0x47, 0x21, 0x10, 0xC0, 0x77, 0x32, 0x20, 0xC0, 0x05, 0x20, 0xFD, 0xE5, 0xE1, 0xCD, 0x13, 0x30, 0xC9, 0xC9])
zw = bytearray(SMS)
zw[0x3000:0x3000 + len(z_exp)] = z_exp
cli.check(rc == 0 and out == bytes(zw), f"Z80 routine is byte-identical to the hand-assembled bytes: {doc}")
for what, line, code in [("halt", "  halt\n", "E_GUEST_FORBIDDEN_INSTRUCTION"), ("di", "  di\n", "E_GUEST_FORBIDDEN_INSTRUCTION"), ("ei", "  ei\n", "E_GUEST_FORBIDDEN_INSTRUCTION"),
                         ("port write", "  out (0xbf),a\n", "E_GUEST_FORBIDDEN_INSTRUCTION"), ("port read", "  in a,(0xdc)\n", "E_GUEST_FORBIDDEN_INSTRUCTION"),
                         ("rst", "  rst 0x38\n", "E_GUEST_FORBIDDEN_INSTRUCTION"), ("im", "  im 1\n", "E_GUEST_FORBIDDEN_INSTRUCTION"), ("reti", "  reti\n", "E_GUEST_FORBIDDEN_INSTRUCTION"),
                         ("ld i,a", "  ld i,a\n", "E_GUEST_FORBIDDEN_INSTRUCTION"), ("write to ROM", "  ld (0x3000),a\n", "E_GUEST_WRITE_OUTSIDE_RAM"),
                         ("write to a port-mapped mirror", "  ld (0xfffe),a\n", "E_GUEST_WRITE_OUTSIDE_RAM"), ("relative range", "start:\n  jr far\n" + "  nop\n" * 200 + "far:\n  ret\n", "E_GUEST_BRANCH_RANGE"),
                         ("numeric jp", "  jp 0x0038\n", "E_GUEST_BRANCH_TARGET"), ("jp to data", "  jp l\ndata\nl:\n  db 0x76\n", "E_GUEST_BRANCH_TARGET"),
                         ("label arithmetic", "s:\n  jp s+0x4000\n", "E_GUEST_BRANCH_TARGET"), ("falling into data", "  nop\ndata\n  db 0x76\n", "E_GUEST_SYNTAX")]:
    refused("Z80 " + what, code, "mod", "check", project("free * 0x3000 0x80 0x00\norg * 0x3000\n" + line, platform="master-system", cpu="z80", revs=[("rev-a", sha(SMS), "rom_only")]))
refused("a bank-switched mapper is rejected", "E_MAPPER_UNSUPPORTED", "mod", "check", project(Z_ASM, platform="master-system", cpu="z80", revs=[("rev-a", sha(SMS), "sega")]))
refused("Z80 beyond the unmapped window", "E_GUEST_ADDRESS", "mod", "check", project("free * 0xc000 0x80 0x00\norg * 0xc000\n  nop\n", platform="master-system", cpu="z80", revs=[("rev-a", sha(SMS), "rom_only")]))

# the Genesis Z80 runs from sound RAM: its code needs an `at` address, which only the Z80 may use
GZ = "free * 0x1000 0x100 0xff\norg * 0x1000 at 0x0100\n  ld a,1\n  ld (0x1000),a\n  ret\n"
dg = project(GZ, cpu="z80", revs=[("rev-a", SHA_A, None)])
rc, doc, out = apply(dg, A)
gw = bytearray(A)
gw[0x1000:0x1006] = bytes([0x3E, 1, 0x32, 0x00, 0x10, 0xC9])
cli.check(rc == 0 and out == bytes(gw), f"Genesis Z80 block lands at its ROM offset, assembled for its sound-RAM address: {doc}")
refused("Genesis Z80 without at", "E_GUEST_ADDRESS", "mod", "check", project("free * 0x1000 0x100 0xff\norg * 0x1000\n  ret\n", cpu="z80", revs=[("rev-a", SHA_A, None)]))
refused("Genesis Z80 write to the YM2612", "E_GUEST_WRITE_OUTSIDE_RAM", "mod", "check", project("free * 0x1000 0x100 0xff\norg * 0x1000 at 0x0100\n  ld (0x4000),a\n", cpu="z80", revs=[("rev-a", SHA_A, None)]))

# ------------------------------------------------------------------ T005: collisions and ranges (adjacent ok, overlap fails)
EDIT = lambda off, n=2: f"* u16 0x{off:08x} 0x{struct.unpack_from('>H', A, off)[0]:04x} 0xabcd\n"
nop = "org * 0x1000\n  nop\n"
ok("free ranges that touch are fine", "mod", "check", project("free * 0x1000 0x80 0xff\nfree * 0x1080 0x80 0xff\n" + nop))
refused("overlapping free ranges", "E_GUEST_COLLISION", "mod", "check", project("free * 0x1000 0x81 0xff\nfree * 0x1080 0x80 0xff\n" + nop))
ok("an edit adjacent to the free range", "mod", "check", project(FREE + nop, edits=EDIT(0x0FFE)))
ok("an edit adjacent to the end of the free range", "mod", "check", project(FREE + nop, edits=EDIT(0x1100)))
refused("an edit overlapping the free range start", "E_GUEST_COLLISION", "mod", "check", project(FREE + nop, edits=EDIT(0x0FFF)))
refused("an edit inside the free range", "E_GUEST_COLLISION", "mod", "check", project(FREE + nop, edits=EDIT(0x1010)))
ok("blocks that touch are fine", "mod", "check", project(FREE + "org * 0x1000\n  nop\norg * 0x1002\n  nop\n"))
refused("overlapping blocks", "E_GUEST_COLLISION", "mod", "check", project(FREE + "org * 0x1000\n  nop\n  nop\norg * 0x1002\n  nop\n"))
ok("per revision independence", "mod", "check", project("free rev-a 0x1000 0x80 0xff\nfree rev-b 0x1040 0x80 0xff\norg rev-a 0x1000\n  nop\norg rev-b 0x1040\n  nop\n"))
refused("a revision-specific range does not cover the other revision", "E_GUEST_OUTSIDE_FREE_RANGE", "mod", "check", project("free rev-a 0x1000 0x80 0xff\norg * 0x1000\n  nop\n"))

# ------------------------------------------------------------------ T005: Genesis header repair
HDR = genesis_rom(size=0x400, free=(0x300, 0x40, 0xFF), valid_header=True)
cli.check(struct.unpack_from(">H", HDR, 0x18E)[0] == checksum(HDR), "fixture: the synthetic ROM verifies its own checksum")
H_ASM = "free * 0x300 0x40 0xff\norg * 0x300\n  moveq #1,d0\n  rts\n"
H_EDIT = "* u16 0x00000250 0x%04x 0x1234\n" % struct.unpack_from(">H", HDR, 0x250)[0]


def hproject(extra="", asm=H_ASM, edits=H_EDIT, image=HDR):
    return project(asm, revs=[("rev-a", sha(image), None)], edits=edits, extra=extra)


rc, doc, out = apply(hproject(), HDR)
manual = bytearray(HDR)
manual[0x250:0x252] = bytes.fromhex("1234")
manual[0x300:0x304] = bytes.fromhex("70014e75")
struct.pack_into(">H", manual, 0x18E, checksum(manual))
cli.check(rc == 0 and out == bytes(manual) and doc["header_repaired"] is True and doc["warnings"] == [], f"default: the checksum of a verifying ROM is repaired (independent computation): {doc}")
cli.check(struct.unpack_from(">H", out, 0x18E)[0] == checksum(out) and out[0x18E:0x190] != HDR[0x18E:0x190], "the repaired checksum verifies and differs from the stale one")

rc, doc, out = apply(hproject('[build]\nheader_repair = "none"\n'), HDR)
stale = bytearray(manual)
stale[0x18E:0x190] = HDR[0x18E:0x190]
cli.check(rc == 0 and out == bytes(stale) and doc["header_repaired"] is False and doc["warnings"] == ["W_CHECKSUM_NOT_REPAIRED"], f"declaring no repair on a verifying ROM warns and leaves the stale checksum: {doc}")
# a ROM that does NOT verify its checksum: no warning (nothing to preserve) and, by default, no repair
BAD = bytearray(HDR)
struct.pack_into(">H", BAD, 0x18E, (checksum(HDR) + 1) & 0xFFFF)
BAD = bytes(BAD)
rc, doc, out = apply(project(H_ASM, revs=[("rev-a", sha(BAD), None)], edits=H_EDIT, extra='[build]\nheader_repair = "none"\n'), BAD)
cli.check(rc == 0 and doc["warnings"] == [] and out[0x18E:0x190] == BAD[0x18E:0x190], f"a ROM that never verified is not warned about: {doc}")
rc, doc, out = apply(project(H_ASM, revs=[("rev-a", sha(BAD), None)], edits=H_EDIT), BAD)
cli.check(rc == 0 and doc["header_repaired"] is False and out[0x18E:0x190] == BAD[0x18E:0x190], f"default repair leaves a non-verifying header untouched: {doc}")
# explicit genesis-checksum: also corrects the ROM-end field
SHORT_END = bytearray(HDR)
struct.pack_into(">I", SHORT_END, 0x1A4, 0x2FF)
SHORT_END = bytes(SHORT_END)
rc, doc, out = apply(project(H_ASM, revs=[("rev-a", sha(SHORT_END), None)], edits=H_EDIT, extra='[build]\nheader_repair = "genesis-checksum"\n'), SHORT_END)
cli.check(rc == 0 and struct.unpack_from(">I", out, 0x1A4)[0] == 0x3FF and struct.unpack_from(">H", out, 0x18E)[0] == checksum(out) and doc["header_repaired"] is True, f"explicit repair rewrites ROM end and checksum: {doc}")
# unsupported headers and mappers fail closed
ODD = HDR + b"\x00"
rc, doc, _ = apply(project("free * 0x300 0x40 0xff\norg * 0x300\n  nop\n", revs=[("rev-a", sha(ODD), None)], extra='[build]\nheader_repair = "genesis-checksum"\n'), ODD)
cli.check(rc == 1 and doc["class"] == "E_HEADER_UNSUPPORTED", f"an odd-sized image has no supported header: {doc}")
startz = bytearray(HDR)
struct.pack_into(">I", startz, 0x1A0, 0x1000)
rc, doc, _ = apply(project(H_ASM, revs=[("rev-a", sha(bytes(startz)), None)], extra='[build]\nheader_repair = "genesis-checksum"\n'), bytes(startz))
cli.check(rc == 1 and doc["class"] == "E_HEADER_UNSUPPORTED", f"a non-zero ROM start is unsupported: {doc}")
ssf2 = bytearray(HDR)
ssf2[0x100:0x108] = b"SEGA SSF"
rc, doc, _ = apply(project("free * 0x300 0x40 0xff\norg * 0x300\n  nop\n", revs=[("rev-a", sha(bytes(ssf2)), None)], extra='[build]\nheader_repair = "genesis-checksum"\n'), bytes(ssf2))
cli.check(rc == 1 and doc["class"] == "E_MAPPER_UNSUPPORTED", f"an SSF cartridge is an unsupported mapper: {doc}")
refused("header repair on another platform", "E_HEADER_UNSUPPORTED", "mod", "check", project(Z_ASM, platform="master-system", cpu="z80", revs=[("rev-a", sha(SMS), "rom_only")], extra='[build]\nheader_repair = "genesis-checksum"\n'))
refused("unknown header_repair value", "E_FIELD_VALUE", "mod", "check", hproject('[build]\nheader_repair = "crc"\n'))

# ---- header fields managed by the selected repair policy cannot also be authored (checksum 0x18E-0x18F; ROM end 0x1A4-0x1A7 for explicit repair)
def edit_at(img, off, n, new_hex):
    return "* bytes 0x%08x %s %s\n" % (off, img[off:off + n].hex(), new_hex)


def hdr_apply(img, edits, extra="", asm=None):
    return apply(project(asm or "free * 0x300 0x40 0xff\norg * 0x300\n  nop\n", revs=[("rev-a", sha(img), None)], edits=edits, extra=extra), img)


STRICT, NONE = '[build]\nheader_repair = "genesis-checksum"\n', '[build]\nheader_repair = "none"\n'
for what, extra, off, n, expect in [
    ("automatic repair: edit on the checksum", "", 0x18E, 2, "E_HEADER_FIELD_COLLISION"),
    ("automatic repair: edit straddling the checksum start", "", 0x18D, 2, "E_HEADER_FIELD_COLLISION"),
    ("automatic repair: ROM end is not managed", "", 0x1A4, 4, None),
    ("explicit repair: edit on the checksum", STRICT, 0x18F, 1, "E_HEADER_FIELD_COLLISION"),
    ("explicit repair: edit on the ROM end", STRICT, 0x1A6, 2, "E_HEADER_FIELD_COLLISION"),
    ("explicit repair: edit adjacent below the checksum", STRICT, 0x18C, 2, None),
    ("explicit repair: edit adjacent between the fields", STRICT, 0x190, 4, None),
    ("explicit repair: edit adjacent above the ROM end", STRICT, 0x1A8, 4, None),
    ("no repair: edit on the checksum is the author's", NONE, 0x18E, 2, None),
    ("no repair: edit on the ROM end is the author's", NONE, 0x1A4, 4, None),
]:
    rc, doc, out = hdr_apply(HDR, edit_at(HDR, off, n, "ab" * n), extra)
    if expect:
        cli.check(rc == 1 and doc["class"] == expect, f"{what}: expected {expect}, got {doc}")
    else:
        cli.check(rc == 0 and out[off:off + n] == bytes([0xAB]) * n or (what.startswith("automatic repair: ROM end") and rc == 0 and out[off:off + n] == bytes([0xAB]) * n), f"{what}: the authored bytes survive: {doc}")
# default repair does not apply to a header that never verified, so there nothing is managed
rc, doc, out = hdr_apply(BAD, edit_at(BAD, 0x18E, 2, "abab"))
cli.check(rc == 0 and out[0x18E:0x190] == b"\xab\xab", f"unverified original, default policy: the checksum edit stands: {doc}")
# injected blocks follow the same rule
Z = bytearray(HDR)
Z[0x188:0x198] = bytes(16)
Z = bytes(Z)
Z_FREE = lambda start, length: "free * 0x%x 0x%x 0x00\norg * 0x%x\n  nop\n" % (start, length, start)
rc, doc, _ = hdr_apply(Z, "", STRICT, Z_FREE(0x18E, 8))
cli.check(rc == 1 and doc["class"] == "E_HEADER_FIELD_COLLISION", f"explicit repair: a block over the checksum is refused: {doc}")
rc, doc, out = hdr_apply(Z, "", STRICT, Z_FREE(0x190, 8))
cli.check(rc == 0 and out[0x190:0x192] == bytes.fromhex("4e71") and doc["header_repaired"] is True, f"explicit repair: an adjacent block is kept and the header repaired: {doc}")
rc, doc, out = hdr_apply(Z, "", NONE, Z_FREE(0x18E, 8))
cli.check(rc == 0 and out[0x18E:0x190] == bytes.fromhex("4e71"), f"no repair: a block over the header is the author's: {doc}")

# ---- the output must never be an input image (path, equivalent path, symlink, hard link); an unrelated artifact is still replaced
import os
src = write(tmp / "guard" / "src.bin", A)
gd = project(ASM, extra=syms)
for what, outp in [("same path", src), ("equivalent path", tmp / "guard" / ".." / "guard" / "src.bin")]:
    for cmd in (("mod", "apply", gd, "--rom", src, "--out", outp), ("mod", "bps", gd, "--rom", src, "--out", outp), ("mod", "pack", gd, "--rom", src, "--out", outp)):
        refused(f"{cmd[1]} --out {what}", "E_OUTPUT_IS_INPUT", *cmd)
        cli.check(src.read_bytes() == A, f"{cmd[1]} --out {what}: the original image is byte-identical after rejection")
link = tmp / "guard" / "hard.bin"
os.link(src, link)
sym = tmp / "guard" / "sym.bin"
os.symlink(src, sym)
for what, outp in [("hard link", link), ("symlink", sym)]:
    for cmd in (("mod", "apply", gd, "--rom", src, "--out", outp), ("mod", "bps", gd, "--rom", src, "--out", outp), ("mod", "pack", gd, "--rom", src, "--out", outp)):
        refused(f"{cmd[1]} --out {what}", "E_OUTPUT_IS_INPUT", *cmd)
        cli.check(src.read_bytes() == A and link.read_bytes() == A, f"{cmd[1]} --out {what}: the original image is byte-identical after rejection")
# the writer's staging file: an input named <out>.partial is deleted by the atomic writer, even though <out> itself does not exist
for cmd_name in ("apply", "bps", "pack"):
    stage_in = write(tmp / "guard" / f"orig-{cmd_name}.bin.partial", A)
    target_out = tmp / "guard" / f"orig-{cmd_name}.bin"
    refused(f"{cmd_name}: input is the output's .partial staging path", "E_OUTPUT_IS_INPUT", "mod", cmd_name, gd, "--rom", stage_in, "--out", target_out)
    cli.check(stage_in.read_bytes() == A and not target_out.exists(), f"{cmd_name}: the .partial input survives and no output appears")
other = write(tmp / "guard" / "old-artifact.bin", b"stale artifact")
ok("an unrelated existing output is replaced", "mod", "apply", gd, "--rom", src, "--out", other)
cli.check(other.read_bytes() == apply(gd, A)[2] and src.read_bytes() == A, "the unrelated artifact was replaced and the source untouched")
ok("a new output path", "mod", "bps", gd, "--rom", src, "--out", tmp / "guard" / "fresh.bps")

# the declaration decides identity: two packages differing only in header_repair are different packages
da, db = hproject(), hproject('[build]\nheader_repair = "none"\n')
ia, ib = ok("check default", "mod", "check", da), ok("check none", "mod", "check", db)
cli.check(ia["package_identity"] != ib["package_identity"] and ia["content_digest"] != ib["content_digest"], "header_repair is part of the package identity and content digest")

# ------------------------------------------------------------------ T006: deterministic BPS, verified by an independent applier
def varint(data, pos):
    value, shift = 0, 1
    while True:
        x = data[pos]
        pos += 1
        value += (x & 0x7F) * shift
        if x & 0x80:
            return value, pos
        shift <<= 7
        value += shift


def apply_bps_ref(patch, source):
    assert patch[:4] == b"BPS1"
    assert zlib.crc32(patch[:-4]) == struct.unpack("<I", patch[-4:])[0], "patch crc"
    pos = 4
    ssize, pos = varint(patch, pos)
    tsize, pos = varint(patch, pos)
    msize, pos = varint(patch, pos)
    pos += msize
    assert ssize == len(source)
    end = len(patch) - 12
    out = bytearray()
    s_rel = t_rel = 0
    while len(out) < tsize:
        word, pos = varint(patch, pos)
        mode, length = word & 3, (word >> 2) + 1
        if mode == 0:
            out += source[len(out):len(out) + length]
        elif mode == 1:
            out += patch[pos:pos + length]
            pos += length
        else:
            raw, pos = varint(patch, pos)
            off = (-1 if raw & 1 else 1) * (raw >> 1)
            if mode == 2:
                s_rel += off
                out += source[s_rel:s_rel + length]
                s_rel += length
            else:
                t_rel += off
                for _ in range(length):
                    out.append(out[t_rel])
                    t_rel += 1
    assert pos == end and len(out) == tsize
    assert zlib.crc32(source) == struct.unpack("<I", patch[end:end + 4])[0] and zlib.crc32(bytes(out)) == struct.unpack("<I", patch[end + 4:end + 8])[0]
    return bytes(out)


bd = project(ASM, extra=syms, edits=None)
rc, edoc, expected_a = apply(bd, A)
bps_path = tmp / "a.bps"
doc = ok("construct the BPS", "mod", "bps", bd, "--rom", write(tmp / "a.bin", A), "--out", bps_path)
patch_bytes = bps_path.read_bytes()
cli.check(apply_bps_ref(patch_bytes, A) == expected_a, "apply(build(edits, code)) equals the verified target (independent BPS applier)")
cli.check(doc["target_sha256"] == sha(expected_a) and doc["source_sha256"] == SHA_A and doc["bps_sha256"] == sha(patch_bytes) and doc["bps_bytes"] == len(patch_bytes), f"BPS report: {doc}")
doc2 = ok("construct again", "mod", "bps", bd, "--rom", tmp / "a.bin", "--out", tmp / "a2.bps")
doc3 = ok("construct a third time", "mod", "bps", bd, "--rom", tmp / "a.bin", "--out", tmp / "a3.bps")
cli.check((tmp / "a2.bps").read_bytes() == patch_bytes == (tmp / "a3.bps").read_bytes() and doc2 == doc3, "three builds yield identical BPS bytes")
cli.check(len(patch_bytes) < 200, f"the delta stays small ({len(patch_bytes)} bytes for a {len(exp)} byte routine)")
ok("the BPS verifies against its mod", "mod", "verify-bps", bd, "--rom", tmp / "a.bin", "--bps", bps_path)
# corruption and staleness
raw = bytearray(patch_bytes)
raw[10] ^= 0x01
refused("a corrupted BPS", "E_BPS_INVALID", "mod", "verify-bps", bd, "--rom", tmp / "a.bin", "--bps", write(tmp / "bad.bps", bytes(raw)))
refused("a truncated BPS", "E_BPS_INVALID", "mod", "verify-bps", bd, "--rom", tmp / "a.bin", "--bps", write(tmp / "trunc.bps", patch_bytes[:-5]))
other = project(ASM.replace("moveq #9,d1", "moveq #8,d1"), extra=syms)
refused("a BPS built for another mod is stale", "E_BPS_STALE", "mod", "verify-bps", other, "--rom", tmp / "a.bin", "--bps", bps_path)
rc, doc, _ = cli.run("mod", "bps", bd, "--rom", write(tmp / "b.bin", B), "--out", tmp / "b.bps")
cli.check(rc == 0 and apply_bps_ref((tmp / "b.bps").read_bytes(), B) != apply_bps_ref(patch_bytes, A), "each revision has its own BPS")
refused("a BPS built for revision B is not valid for image A (stale source digest)", "E_BPS_INVALID", "mod", "verify-bps", bd, "--rom", tmp / "a.bin", "--bps", tmp / "b.bps")
refused("an undeclared source image", "E_IMAGE_MISMATCH", "mod", "bps", bd, "--rom", write(tmp / "x.bin", b"unknown"), "--out", tmp / "x.bps")
cli.check(not (tmp / "x.bps").exists(), "a refused build writes no BPS")

# size limit: an image above the supported size is refused, no BPS is written
huge = tmp / "huge.bin"
huge.write_bytes(bytes(16 * 1024 * 1024 + 1))
refused("an image above the supported size", "E_IMAGE_MISMATCH", "mod", "bps", bd, "--rom", huge, "--out", tmp / "huge.bps")
cli.check(not (tmp / "huge.bps").exists(), "no BPS for an oversized image")
huge.unlink()

# pack --rom verifies every named image before the package is written
pk = tmp / "pk.segapatch"
doc = ok("pack with round-trip verification of both revisions", "mod", "pack", bd, "--out", pk, "--rom", tmp / "a.bin", "--rom", tmp / "b.bin")
cli.check([v["revision"] for v in doc["verified_images"]] == ["rev-a", "rev-b"] and doc["verified_images"][0]["target_sha256"] == sha(expected_a), f"pack reports each verified image: {doc.get('verified_images')}")
before = pk.read_bytes()
pk2 = tmp / "pk2.segapatch"
refused("pack refuses an image that is not a declared revision", "E_IMAGE_MISMATCH", "mod", "pack", bd, "--out", pk2, "--rom", write(tmp / "y.bin", b"nope"))
cli.check(not pk2.exists(), "a failed verification writes no package")
ok("pack without --rom is unchanged", "mod", "pack", bd, "--out", tmp / "pk3.segapatch")
cli.check((tmp / "pk3.segapatch").read_bytes() == before, "verification does not alter the package bytes")
# the package read back through the schema 3 reader assembles the same image
ok("apply from the packed package", "mod", "apply", pk, "--rom", tmp / "a.bin", "--out", tmp / "from_pkg.bin")
cli.check((tmp / "from_pkg.bin").read_bytes() == expected_a, "the packed package builds the identical target")

# property-style: random synthetic images and random non-overlapping edits; BPS applied by the independent applier equals the independently edited image
rng = random.Random(136)
for trial in range(12):
    size = rng.choice([0x400, 0x800, 0x1000, 0x2000])
    img = bytearray(rng.randrange(256) for _ in range(size))
    img[0x100:0x110] = b"SEGA MEGA DRIVE "
    if trial % 3 == 0:
        img[0x300:0x340] = bytes([0xFF]) * 0x40
    img = bytes(img)
    spots = sorted(rng.sample(range(0x200, size - 8, 8), 5))
    lines, want = [], bytearray(img)
    for off in spots:
        if 0x300 <= off < 0x340:
            continue
        n = rng.choice([1, 2, 4])
        new = bytes(rng.randrange(256) for _ in range(n))
        lines.append(f"* bytes 0x{off:08x} {img[off:off + n].hex()} {new.hex()}\n")
        want[off:off + n] = new
    asm = "free * 0x300 0x40 0xff\norg * 0x300\n  moveq #%d,d0\n  rts\n" % rng.randrange(-100, 100) if trial % 3 == 0 else None
    if asm:
        want[0x300:0x304] = bytes([0x70, int(asm.split("#")[1].split(",")[0]) & 0xFF, 0x4E, 0x75])
    proj = tmp / f"prop{trial}"
    if asm:
        write(proj / "asm" / "main.asm", asm)
    write(proj / "edits" / "main.edits", "".join(lines))
    sources = [{"kind": "rom-edit-set", "path": "edits/main.edits"}] + ([{"kind": "guest-asm", "path": "asm/main.asm", "cpu": "m68k"}] if asm else [])
    write(proj / "mod.toml", toml("genesis", [("rev-a", sha(img), None)], sources, '[build]\nheader_repair = "none"\n'))
    ip = write(tmp / f"prop{trial}.bin", img)
    rc, doc, p = cli.run("mod", "bps", proj, "--rom", ip, "--out", tmp / f"prop{trial}.bps")
    cli.check(rc == 0, f"property trial {trial}: {doc}")
    if rc == 0:
        cli.check(apply_bps_ref((tmp / f"prop{trial}.bps").read_bytes(), img) == bytes(want), f"property trial {trial}: apply(build(edits)) equals the expected target")

cli.done("mod_guest_test")
