#!/usr/bin/env python3
"""SEG-075-T002 (ADR 0160): tests of the cross-platform container/listing adapters and label extraction (synthetic data only).

Covers TOS PRG (fixup table, odd/oversized fixups rejected), Amiga hunk executables/objects (RELOC32/16, EXT references, SYMBOL/DEBUG
skipped, trailing garbage rejected), M68K ELF32 relocatables (RELA widths), a.out OMAGIC objects; vasm, A68K and GNU as listings
(continuations, truncated long rows, section tracking incl. macro-expansion '>' lines, zero section padding); per-section labelling (data,
alignment, unlisted gaps, overlapping rows and listing/object disagreement rejected, GNU as byte lag rejected, relocated fields masked);
placement (unique, ambiguous, relocation-invariant); decoder typing with synthetic tokens (rejected / exception form / length mismatch /
odd start, ILLEGAL accepted, fall-through propagation, encoded 68010+ opcode words); and, with the real CLI (argv[1]), an end-to-end
extraction in which 68020 forms (EXTB.L, scaled index, BRA.L) are typed and kept out of the base-68000 labels, deterministically.
"""

import json
import pathlib
import struct
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_xplat_corpus as xc  # noqa: E402
import segarecomp_xplat_formats as fm  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL:", msg)


def raises(fn, *args):
    try:
        fn(*args)
    except fm.FormatError:
        return True
    return False


# ------------------------------------------------------------------------------------------------------------------- containers
def prg(text: bytes, data: bytes, fixups: bytes, first: int) -> bytes:
    hdr = b"\x60\x1a" + struct.pack(">IIIIIIH", len(text), len(data), 0x40, 0, 0, 0, 0)
    return hdr + text + data + struct.pack(">I", first) + fixups


def test_prg():
    text = bytes(600)
    p = fm.parse_prg(prg(text, b"\0" * 8, bytes([1, 6, 0]), 2))
    check([r for r, _w in p["relocs"]] == [2, 256 + 6], "PRG fixups: first offset, 254-byte skip, even step")
    check(p["image"] == text + b"\0" * 8 and p["entry"] == 0, "PRG image is text+data, entry 0")
    check(fm.parse_prg(prg(text, b"", b"", 0))["relocs"] == [], "PRG without fixups")
    check(raises(fm.parse_prg, prg(text, b"", bytes([3, 0]), 2)), "odd PRG fixup step rejected")
    check(raises(fm.parse_prg, prg(text, b"", bytes([0]), 598)), "PRG fixup past text+data rejected")
    check(raises(fm.parse_prg, prg(text, b"", bytes([6]), 2)), "unterminated PRG fixup table rejected")
    check(raises(fm.parse_prg, b"\x60\x1a" + b"\0" * 10), "truncated PRG rejected")


def L(*xs):
    return b"".join(struct.pack(">I", x) for x in xs)


def test_hunks():
    code = b"\x4e\x75\x00\x00" + b"\x00\x00\x00\x08"
    exe = (L(0x3F3, 0, 3, 0, 2, 2, 1, 4) + L(0x3E9, 2) + code + L(0x3EC, 1, 1, 4, 0) + L(0x3F0, 1) + b"abc\0" + L(0x10, 0) + L(0x3F2)
           + L(0x3EA, 1) + b"DATA" + L(0x3F2) + L(0x3EB, 4) + L(0x3F2))
    h = fm.parse_hunks(exe)
    check(h["format"] == "amiga-hunk-exe" and [x["type"] for x in h["hunks"]] == ["CODE", "DATA", "BSS"], "hunk exe types")
    check(h["hunks"][0]["relocs"] == [(4, 4)] and h["hunks"][0]["data"] == code, "RELOC32 offset/width")
    obj = (L(0x3E7, 1) + b"u\0\0\0" + L(0x3E9, 2) + code + L(0x3ED, 1, 0, 2, 0)
           + L(0x3EF, (129 << 24) | 1) + b"ext\0" + L(1, 4) + L((1 << 24) | 1) + b"def\0" + L(0) + L(0) + L(0x3F2))
    o = fm.parse_hunks(obj)
    check(o["format"] == "amiga-hunk-object" and o["hunks"][0]["relocs"] == [(2, 2), (4, 4)], "object RELOC16 + EXT_REF32")
    check(raises(fm.parse_hunks, exe + b"\0\0\0\0"), "trailing bytes after the last hunk rejected")
    check(raises(fm.parse_hunks, L(0x3E7, 1) + b"u\0\0\0" + L(0x3E9, 1) + b"\0\0\0\0" + L(0x3EC, 1, 0, 8, 0) + L(0x3F2)),
          "relocation outside its hunk rejected")
    check(raises(fm.parse_hunks, L(0x3E7, 1) + b"u\0\0\0" + L(0x3FF)), "unknown hunk type rejected")


def elf_rel(text: bytes, relas: list[tuple[int, int]]) -> bytes:
    shstr = b"\0.text\0.rela.text\0.symtab\0.strtab\0.shstrtab\0"
    strtab = b"\0f\0"
    sym = b"\0" * 16 + struct.pack(">IIIBBH", 1, 0, len(text), 0x12, 0, 1)
    rela = b"".join(struct.pack(">IIi", off, (0 << 8) | t, 0) for off, t in relas)
    body = text + rela + sym + strtab + shstr
    off_text = 52
    off_rela = off_text + len(text)
    off_sym = off_rela + len(rela)
    off_str = off_sym + len(sym)
    off_sh = off_str + len(strtab)
    shoff = off_sh + len(shstr)
    shoff += (-shoff) % 4
    pad = b"\0" * (shoff - off_sh - len(shstr))

    def sh(name, typ, flags, off, size, link=0, info=0, ent=0):
        return struct.pack(">IIIIIIIIII", name, typ, flags, 0, off, size, link, info, 2, ent)

    secs = (sh(0, 0, 0, 0, 0) + sh(1, 1, 6, off_text, len(text)) + sh(7, 4, 0, off_rela, len(rela), 3, 1, 12)
            + sh(18, 2, 0, off_sym, len(sym), 4, 1, 16) + sh(26, 3, 0, off_str, len(strtab)) + sh(34, 3, 0, off_sh, len(shstr)))
    hdr = b"\x7fELF\x01\x02\x01" + b"\0" * 9 + struct.pack(">HHIIIIIHHHHHH", 1, 4, 1, 0, 0, shoff, 0, 52, 0, 0, 40, 6, 5)
    return hdr + body + pad + secs


def test_elf():
    e = fm.parse_elf32(elf_rel(b"\x4e\xb9\0\0\0\0\x61\x00\0\0\x4e\x75", [(2, 1), (8, 5)]))
    text = next(s for s in e["sections"] if s["name"] == ".text")
    check(text["relocs"] == [(2, 4), (8, 2)], "ELF RELA widths (R_68K_32 / R_68K_PC16)")
    check(any(s["name"] == "f" for s in e["symbols"]) and e["format"] == "elf-m68k-relocatable", "ELF symbols and kind")
    check(raises(fm.parse_elf32, elf_rel(b"\x4e\x75", [(1, 1)])), "relocation outside its section rejected")
    check(raises(fm.parse_elf32, elf_rel(b"\x4e\x75\0\0", [(0, 99)])), "unsupported relocation type rejected")
    check(raises(fm.parse_elf32, b"\x7fELF\x01\x01" + b"\0" * 60), "little-endian ELF rejected")


def test_aout():
    text = b"\x4e\xb9\0\0\0\0\x4e\x75"
    blob = struct.pack(">IIIIIIII", 0o407, len(text), 4, 0, 0, 0, 8, 0) + text + b"DATA" + struct.pack(">II", 2, 2 << 5)
    a = fm.parse_aout(blob)
    check(a["text"] == text and a["data"] == b"DATA" and a["text_relocs"] == [(2, 4)], "a.out text/data and long relocation")
    bad = struct.pack(">IIIIIIII", 0o407, len(text), 0, 0, 0, 0, 8, 0) + text + struct.pack(">II", 6, 2 << 5)
    check(raises(fm.parse_aout, bad), "a.out relocation outside text rejected")
    check(raises(fm.parse_aout, struct.pack(">I", 0o410) + b"\0" * 28), "non-OMAGIC a.out rejected")


# --------------------------------------------------------------------------------------------------------------------- listings
VASM = ('Sections:\n00: "CODE" (0-12)\n\n\nSource: "t.s"\n'
        "                            \t     1: ; c\n"
        "00:00000000 2200            \t     2: start\tmove.l\td0,d1\n"
        "00:00000002 4E75            \t     3: \trts\n"
        "00:00000004 4142434445464748\t     4: msg\tdc.b\t'ABCDEFGHIJ'\n"
        "00:0000000C 494A\n"
        "00:0000000E 4E714E71        \t     5: \tcnop\t0,4\n")


def test_vasm_listing():
    names, rows = fm.parse_vasm_listing(VASM)
    check(names == ["CODE"] and len(rows) == 4, "vasm sections and rows")
    check(rows[2].shown == b"ABCDEFGHIJ", "vasm continuation joined to its row")
    check([fm.row_kind(fm.statement(r.text, "vasm")[0], "vasm") for r in rows] == ["insn", "insn", "data", "align"], "vasm row kinds")
    check(raises(fm.parse_vasm_listing, VASM.replace("00:0000000C 494A", "00:00000010 494A")), "detached continuation rejected")


def a68k_line(addr: str, hexf: str, line: int, src: str) -> str:
    """One A68K 2.71 listing line in its fixed columns: address [0:6], bytes [8:32], line number ending at column 39, source [40:]."""
    return f"{addr:<6}  {hexf:<24}{line:>7} {src}\n"


A68K = (a68k_line("", "", 1, "* header") + a68k_line("000000", "", 2, "        SECTION one,CODE")
        + a68k_line("000000", "2200", 3, "Start   move.l  d0,d1")
        + a68k_line("000002", "4142434445464748494A4B4C", 4, "Tab     dc.b    'ABCDEFGHIJKLMN',0")
        + a68k_line("000011", "00", 5, "        even") + a68k_line("000012", "4E75", 6, "        rts"))


def test_a68k_listing():
    names, rows = fm.parse_a68k_listing(A68K)
    rows = [r for r in rows if r.shown]  # the SECTION statement line carries an address but no bytes
    check(names == ["one"] and [r.offset for r in rows] == [0, 2, 0x11, 0x12], "A68K fixed-column rows")
    check(rows[1].truncated and not rows[0].truncated, "A68K long data row marked truncated")
    check(fm.statement(rows[0].text, "a68k") == ("move.l", "d0,d1"), "A68K label stripped")


GAS = ("   1               \t#NO_APP\n"
       "   2               \t\t.text\n"
       "   3               \tf:\n"
       "   4 0000 2200      \t\tmove.l %d0,%d1\n"
       "   5 0002 203C 0001 \t\tmove.l #65536,%d0\n"
       "   5      0000 \n"
       "   6 0008 4E75      \t\trts\n"
       "   7               \t\t.pushsection .rodata\n"
       "   7               \t>  .pushsection .data2\n"
       "   7 0000 0102      \t>  .word 258\n"
       "   7               \t>  .popsection\n"
       "   8 0000 4142 4300 \t\t.string \"ABC\"\n"
       "   9 0004 00        \t\t.popsection\n"
       "  10 000a 4E71      \t\tnop\n")


def test_gas_listing():
    names, rows = fm.parse_gas_listing(GAS)
    secs = [(r.section, r.offset) for r in rows]
    check(secs == [(".text", 0), (".text", 2), (".text", 8), (".data2", 0), (".rodata", 0), (".rodata", 4), (".text", 10)],
          "GNU as section tracking incl. '>' expansion lines and push/pop")
    check(rows[1].shown == bytes.fromhex("203C00010000"), "GNU as continuation bytes")


# ---------------------------------------------------------------------------------------------------------------- labelling
def rows_of(listing: str, syntax: str):
    return {"vasm": fm.parse_vasm_listing, "a68k": fm.parse_a68k_listing, "gas": fm.parse_gas_listing}[syntax](listing)[1]


def test_section_labels():
    data = bytes.fromhex("2200" "4E75" "4142434445464748494A" "4E714E71")
    t = xc.Typed()
    cls, recs = xc.section_labels(rows_of(VASM, "vasm"), data, bytearray(len(data)), "vasm", t, "s")
    check(bytes(cls) == b"IcIc" + b"D" * 10 + b"UUUU", "insn/data/align labels from listing rows")
    # relocated field: listing shows zeros, object holds an addend; masked comparison accepts it
    lst = 'Sections:\n00: "CODE" (0-6)\n\n\nSource: "t.s"\n00:00000000 4EB900000000    \t     1: \tjsr\tfar\n'
    obj = bytes.fromhex("4EB912345678")
    mask = xc.reloc_mask(6, [(2, 4)])
    check(bytes(xc.section_labels(rows_of(lst, "vasm"), obj, mask, "vasm", xc.Typed(), "s")[0]) == b"Iccccc", "relocated field masked")
    check(xc.section_labels(rows_of(lst, "vasm"), obj, bytearray(6), "vasm", t, "s") is None, "unmasked listing/object disagreement rejected")
    # A68K: a truncated data row keeps its full extent from the next row
    a = bytes.fromhex("2200") + b"ABCDEFGHIJKLMN\0" + b"\0" + bytes.fromhex("4E75")
    cls, _ = xc.section_labels(rows_of(A68K, "a68k"), a, bytearray(len(a)), "a68k", xc.Typed(), "s")
    check(bytes(cls) == b"Ic" + b"D" * 15 + b"U" + b"Ic", "A68K truncated data extent and even padding unknown")
    # vasm: unlisted cnop padding after an instruction stays unknown, the instruction keeps its listed length
    gap = 'Sections:\n00: "CODE" (0-8)\n\n\nSource: "t.s"\n00:00000000 4E75            \t     1: \trts\n00:00000004 2200            \t     2: \tmove.l\td0,d1\n'
    t2 = xc.Typed()
    cls, _ = xc.section_labels(rows_of(gap, "vasm"), bytes.fromhex("4E7500002200"), bytearray(6), "vasm", t2, "s")
    check(bytes(cls) == b"IcUUIc" and t2.counts.get("UNLISTED_GAP") == 2, "unlisted gap unknown and typed")
    # overlapping emitting rows are rejected
    ov = 'Sections:\n00: "CODE" (0-2)\n\n\nSource: "t.s"\n00:00000000 4E75            \t     1: \trts\n00:00000000 4E75            \t     2: \trts\n'
    check(xc.section_labels(rows_of(ov, "vasm"), bytes.fromhex("4E75"), bytearray(2), "vasm", xc.Typed(), "s") is None, "overlap rejected")
    # GNU as byte lag: bytes listed on a non-emitting statement reject the section; zero padding on a section switch does not
    lag = GAS.replace("   3               \tf:\n", "   3 0000 2200      \tf:\n")
    t3 = xc.Typed()
    check(xc.section_labels([r for r in rows_of(lag, "gas") if r.section == ".text"], bytes.fromhex("2200203C000100004E754E71"),
                            bytearray(12), "gas", t3, "s") is None and t3.counts.get("LISTING_BYTE_LAG"), "GNU as byte lag rejected")
    text_rows = [r for r in rows_of(GAS, "gas") if r.section == ".text"]
    cls, _ = xc.section_labels(text_rows, bytes.fromhex("2200203C000100004E754E71"), bytearray(12), "gas", xc.Typed(), "s")
    check(bytes(cls) == b"IcIcccccIcIc", "GNU as instructions labelled")
    ro = [r for r in rows_of(GAS, "gas") if r.section == ".rodata"]
    t4 = xc.Typed()
    cls, _ = xc.section_labels(ro, b"ABC\0\0", bytearray(5), "gas", t4, "s")
    check(bytes(cls) == b"DDDDU" and t4.counts.get("SECTION_PADDING") == 1, "zero padding on a section switch is unknown, not lag")


def test_place():
    img = b"\x11\x22" * 8 + bytes.fromhex("4EB900000010" "4E75" "2200" "4E71") + b"\x33\x44" * 4
    sec = bytes.fromhex("4EB900000000" "4E75" "2200" "4E71")
    mask = xc.reloc_mask(len(sec), [(2, 4)])
    check(xc.place(sec, mask, img) == [16], "unique placement outside relocation fields")
    check(xc.place(sec, bytearray(len(sec)), img) == [], "relocated field is not matched without its mask")
    sec2 = bytes.fromhex("4EB9DEADBEEF" "4E75" "2200" "4E71")
    check(xc.place(sec2, mask, img) == [16], "relocation invariance: a different relocated value places identically")
    dup = img + bytes.fromhex("4EB900000020" "4E75" "2200" "4E71")
    check(len(xc.place(sec, mask, dup)) == 2, "duplicate content is ambiguous (two placements)")
    check(xc.place(bytes.fromhex("4E75"), bytearray(2), img) == [], "anchor shorter than 4 bytes is never placed")


def tok(entries: dict[int, tuple[int, int, int]], n_words: int) -> bytes:
    t = bytearray(8 * n_words)
    for w, (status, length, control) in entries.items():
        t[w * 8], t[w * 8 + 1], t[w * 8 + 3] = status, length, control
    return bytes(t)


def test_decoder_rules():
    # rows: insn@0 len2 ok, insn@2 rejected, insn@4 extent4 but decodes 1 word, data@8 after fall-through, data@10 continues, insn@12
    rows = [{"offset": 0, "extent": 2, "kind": "insn", "mnemonic": "move.l"}, {"offset": 2, "extent": 2, "kind": "insn", "mnemonic": "extb.l"},
            {"offset": 4, "extent": 4, "kind": "insn", "mnemonic": "bra.l"}, {"offset": 8, "extent": 2, "kind": "insn", "mnemonic": "nop"},
            {"offset": 10, "extent": 2, "kind": "data", "mnemonic": "dc.w"}, {"offset": 12, "extent": 4, "kind": "data", "mnemonic": "dc.l"},
            {"offset": 16, "extent": 2, "kind": "insn", "mnemonic": "illegal"}, {"offset": 18, "extent": 2, "kind": "insn", "mnemonic": "rts"},
            {"offset": 20, "extent": 2, "kind": "data", "mnemonic": "dc.w"}, {"offset": 22, "extent": 4, "kind": "data", "mnemonic": "dc.l"}]
    cls = bytearray(b"Ic" + b"Ic" + b"Iccc" + b"Ic" + b"DD" + b"DDDD" + b"Ic" + b"Ic" + b"DD" + b"DDDD")
    image = bytes(22) + bytes.fromhex("4E7B0801")
    tokens = tok({0: (2, 1, 0), 1: (0, 0, 0), 2: (2, 1, 0), 4: (2, 1, 0), 5: (1, 1, 7), 8: (1, 1, 7), 9: (2, 1, 6), 11: (1, 1, 7)}, 13)
    t = xc.Typed()
    xc.apply_decoder(cls, rows, tokens, t, 0, len(cls), image)
    check(bytes(cls[:2]) == b"Ic", "ordinary base-68000 instruction kept")
    check(bytes(cls[2:4]) == b"UU" and t.counts.get("DECODER_REJECTED") == 1, "rejected (68020) form typed and unknown")
    check(bytes(cls[4:8]) == b"UUUU" and t.counts.get("LENGTH_MISMATCH") == 1, "length mismatch (BRA.L on a 68000) typed and unknown")
    check(bytes(cls[10:16]) == b"UUUUUU" and t.counts.get("DATA_AFTER_FALLTHROUGH") == 6, "fall-through propagates over contiguous data")
    check(t.counts.get("ENCODED_NON_BASE_WORD_AFTER_FALLTHROUGH") == 2, "encoded non-base words after fall-through counted")
    check(bytes(cls[16:18]) == b"Ic", "ILLEGAL accepted as the intended exception form")
    check(bytes(cls[20:22]) == b"DD", "data after a terminator stays data")
    check(bytes(cls[22:26]) == b"UUUU" and t.counts.get("D_ROW_ENCODED_NON_BASE_OPCODE") == 1, "encoded MOVEC data word unknown")
    odd = bytearray(b"UIc")
    t5 = xc.Typed()
    xc.apply_decoder(odd, [{"offset": 1, "extent": 2, "kind": "insn", "mnemonic": "nop"}], tok({}, 1), t5, 0, 3, b"\0\0\0")
    check(bytes(odd) == b"UUU" and t5.counts.get("ODD_START") == 1, "odd instruction start typed")
    check(xc.encoded_non_base_opcode(0x4E7A) and xc.encoded_non_base_opcode(0xF200) and not xc.encoded_non_base_opcode(0xFFFF)
          and not xc.encoded_non_base_opcode(0x4E75), "encoded-opcode predicate")


# ------------------------------------------------------------------------------------------------------- end to end (real CLI)
def test_end_to_end(cli: str | None):
    if not cli:
        print("skip end-to-end: no CLI path")
        return
    # MOVE.L D0,D1 / EXTB.L D0 (68020) / MOVE.L (A0,D0.W*4),D1 (68020 scale) / BRA.L (68020) / ILLEGAL / RTS / table / NOP
    code = bytes.fromhex("2200" "49C0" "22300400" "60FF00000004" "4AFC" "4E75" "00010002" "4E71")
    lst = ('Sections:\n00: "CODE" (0-18)\n\n\nSource: "t.s"\n'
           "00:00000000 2200            \t     1: start\tmove.l\td0,d1\n"
           "00:00000002 49C0            \t     2: \textb.l\td0\n"
           "00:00000004 22300400        \t     3: \tmove.l\t(a0,d0.w*4),d1\n"
           "00:00000008 60FF00000004    \t     4: \tbra.l\tnext\n"
           "00:0000000E 4AFC            \t     5: \tillegal\n"
           "00:00000010 4E75            \t     6: next\trts\n"
           "00:00000012 00010002        \t     7: tab\tdc.w\t1,2\n"
           "00:00000016 4E71            \t     8: \tnop\n")
    exe = L(0x3F3, 0, 1, 0, 0, len(code) // 4) + L(0x3E9, len(code) // 4) + code + L(0x3F2)
    out = []
    for _ in range(2):
        with tempfile.TemporaryDirectory() as td:
            td = pathlib.Path(td)
            (td / "out").mkdir()
            (td / "out" / "t.lst").write_text(lst)
            (td / "out" / "t").write_bytes(exe)
            spec = {"cid": "HelloAmi", "recipe": "units", "root": "t001", "syntax": "vasm", "unit": "t",
                    "units": [{"listing": "out/t.lst", "object": None}], "final": {"kind": "hunk-exe", "path": "out/t"}}
            (td / "scratch").mkdir()
            metas = xc._extract(spec, td, cli, td / "corpus", td / "scratch")
            d = td / "corpus" / metas[0]["id"].replace("#", "@")
            out.append(((d / "classes.bin").read_bytes(), (d / "tokens.bin").read_bytes(), metas[0]["typed"]))
    cls, _tokens, typed = out[0]
    check(cls[:2] == b"Ic", "base instruction labelled through the real decoder")
    check(cls[2:14] == b"U" * 12, "EXTB.L, scaled index and BRA.L kept out of the base-68000 labels")
    check(cls[14:18] == b"IcIc", "ILLEGAL (intended) and RTS labelled")
    check(cls[18:22] == b"DDDD" and cls[22:24] == b"Ic", "table after RTS is data; following instruction labelled")
    bad = sum(typed.get(k, 0) for k in ("DECODER_REJECTED", "EXCEPTION_FORM", "LENGTH_MISMATCH"))
    check(bad == 3, f"three 68020 forms typed (got {typed})")
    check(out[0] == out[1], "extraction is deterministic (classes, tokens, typed counts)")


def test_review_rules():
    # ROM load address from the link map's output .text section
    m = "Memory Configuration\n\n.text           0x0000000000fa0000    0x1f894\n .text          0x0000000000fa0000      0x158 obj/startup.o\n"
    check(xc.map_text_base(m) == 0xFA0000, "ROM base read from the link map")
    check(raises(xc.map_text_base, "no sections\n"), "a map without .text is rejected")
    rom = bytearray(64)
    rom[4:8] = (0xFA0010).to_bytes(4, "big")
    check(xc.rom_entry(bytes(rom), 0xFA0000) == 0x10 and xc.rom_entry(bytes(rom), 0xFC0000) is None, "reset vector -> image offset")
    # GNU as: a data directive with operands but no listed bytes is byte lag
    lag = ("   1               \t\t.text\n   2 0000 4E75      \t\trts\n   3 0002           \t\t.long 5\n   4 0002 0000 0005 \t\tnop\n")
    t5 = xc.Typed()
    check(xc.section_labels(rows_of(lag, "gas"), bytes.fromhex("4E7500000005"), bytearray(6), "gas", t5, "s") is None
          and t5.counts.get("LISTING_BYTE_LAG"), "data row without listed bytes rejects the section")
    nolag = "   1               \t\t.text\n   2 0000 4E75      \t\trts\n   3 0002           \t\t.zero 0\n"
    check(xc.section_labels(rows_of(nolag, "gas"), bytes.fromhex("4E75"), bytearray(2), "gas", xc.Typed(), "s") is not None,
          "a zero-size data directive is not lag")
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        # relist selection: the scrubbed re-assembly's listing is used only when its object is byte-identical
        obj = b"\x7fELF\x01\x02\x01" + b"\0" * 11 + b"\x00\x04" + b"\0" * 40
        for name, relist in (("same.o", obj), ("diff.o", obj[:-1] + b"\x01")):
            (td / name).write_bytes(obj)
            (td / (name + ".relist.o")).write_bytes(relist)
            (td / (name + ".lst")).write_text("")
            (td / (name + ".relist.lst")).write_text("")
        units = {u["object"]: u["listing"] for u in xc._units({"units": "objects:*.o"}, td)}
        check(units == {"diff.o": "diff.o.lst", "same.o": "same.o.relist.lst"}, "relist listing only for a byte-identical re-assembly")
        # seeds: an entry root becomes a seed only where the listing labels an instruction start
        spec = {"cid": "HelloAmi", "recipe": "units", "unit": "u", "syntax": "vasm", "root": "t001"}
        im = {"name": "hunk0", "image": b"\0" * 256, "relocs": [], "entry": 0}
        m1 = xc.write_sample(td / "c", "xp/HelloAmi/units/a#hunk0", spec, im, b"D" * 256, b"\0" * 1024, None, {})
        m2 = xc.write_sample(td / "c", "xp/HelloAmi/units/b#hunk0", spec, im, b"Ic" + b"D" * 254, b"\0" * 1024, None, {})
        check(m1["seed_cells"] == 0 and m2["seed_cells"] == 1, "entry on data is not a seed; entry on an instruction start is")
    # direct vasm mapping: listing section 1 is hunk 1 even when hunk 0 (BSS) is not an image
    code = bytes.fromhex("22004E75")
    exe = L(0x3F3, 0, 2, 0, 1, 4, 1) + L(0x3EB, 4) + L(0x3F2) + L(0x3E9, 1) + code + L(0x3F2)
    lst = ('Sections:\n00: "BSS" (0-10)\n01: "CODE" (0-4)\n\n\nSource: "t.s"\n'
           "01:00000000 2200            \t     1: \tmove.l\td0,d1\n01:00000002 4E75            \t     2: \trts\n")
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        (td / "out").mkdir()
        (td / "out" / "t").write_bytes(exe)
        (td / "out" / "t.lst").write_text(lst)
        images = xc.final_images({"final": {"kind": "hunk-exe", "path": "out/t"}}, td)
        check(len(images) == 1 and images[0]["hunk"] == 1, "BSS hunk is not an image; the code hunk keeps its hunk index")
        names, rows = fm.parse_vasm_listing(lst)
        by_hunk = {im["hunk"]: k for k, im in enumerate(images)}
        secs = [({"data": images[by_hunk[h]]["image"]} if h in by_hunk else {"data": b""}) for h in range(max(by_hunk) + 1)]
        mapped = xc.rows_by_section(names, rows, "vasm", "direct", secs)
        check(set(mapped) == {1} and secs[1]["data"] == code, "listing section 1 maps to hunk 1 (not image 0)")


def test_committed_coverage():
    p = ROOT / "tools" / "segarecomp_xplat_corpus.coverage.json"
    if not p.is_file():
        return
    cov = json.loads(p.read_text())
    text = json.dumps(cov)
    check(cov["schema"] == "segarecomp.xplat_corpus.coverage.v1" and len(cov["corpus_manifest_sha256"]) == 64, "coverage identity")
    check('"offset"' not in text and ".lst" not in text and "spans" not in text, "coverage is aggregate-only")
    for name, prog in cov["programs"].items():
        check(abs(sum(prog["fractions"].values()) - 1.0) < 0.001, f"{name}: fractions sum to 1")


def main():
    cli = sys.argv[1] if len(sys.argv) > 1 else None
    for t in (test_prg, test_hunks, test_elf, test_aout, test_vasm_listing, test_a68k_listing, test_gas_listing, test_section_labels,
              test_place, test_decoder_rules, test_review_rules, test_committed_coverage):
        t()
    test_end_to_end(cli)
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    print("segarecomp_xplat_corpus_test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
