#!/usr/bin/env python3
"""SEG-075-T002 (ADR 0160): bounds-checked M68K executable/object containers and assembler listings (standard library only).

Only the formats the T001-qualified programs actually produce:
  containers  Atari TOS PRG (text/data/bss + fixup table), Amiga hunk executable and object (CODE/DATA/BSS, RELOC32/16/8, EXT refs,
              SYMBOL/DEBUG skipped), big-endian M68K ELF32 relocatable/executable (sections, symbols, RELA), a.out OMAGIC objects (vasm
              -Faout; text/data + relocations)
  listings    vasm (`-L`), A68K 2.71 (`-l`), GNU as (`-a`), each normalized into rows (section, offset, listed bytes, source text)

Every container read is bounds-checked; malformed or unsupported input raises FormatError (typed, never silently accepted). A relocation is
returned as (offset, width): the bytes it covers are *relocation evidence*, never compared or decoded as fixed instruction bytes.
"""
from __future__ import annotations

import re
import struct


class FormatError(Exception):
    pass


def _u32(data: bytes, off: int) -> int:
    if off < 0 or off + 4 > len(data):
        raise FormatError(f"truncated at {off:#x}")
    return struct.unpack_from(">I", data, off)[0]


def _u16(data: bytes, off: int) -> int:
    if off < 0 or off + 2 > len(data):
        raise FormatError(f"truncated at {off:#x}")
    return struct.unpack_from(">H", data, off)[0]


def _take(data: bytes, off: int, n: int) -> bytes:
    if n < 0 or off < 0 or off + n > len(data):
        raise FormatError(f"truncated: {n} bytes at {off:#x}")
    return data[off:off + n]


# ------------------------------------------------------------------------------------------------------------------------- TOS PRG
def parse_prg(data: bytes) -> dict:
    """Atari TOS PRG: header (0x601A, text, data, bss, symbols, reserved, flags, absflag), text+data image, fixup table -> 32-bit
    relocation offsets into the text+data image."""
    if len(data) < 28 or data[:2] != b"\x60\x1a":
        raise FormatError("not a TOS PRG")
    t, d, b, s = struct.unpack_from(">IIII", data, 2)
    flags = _u32(data, 22)
    absflag = _u16(data, 26)
    image = _take(data, 28, t + d)
    relocs: list[int] = []
    off = 28 + t + d + s
    if absflag == 0 and off + 4 <= len(data):
        first = _u32(data, off)
        off += 4
        if first:
            pos = first
            relocs.append(pos)
            while True:
                if off >= len(data):
                    raise FormatError("unterminated PRG fixup table")
                step = data[off]
                off += 1
                if step == 0:
                    break
                if step == 1:
                    pos += 254
                    continue
                if step & 1:
                    raise FormatError("odd PRG fixup step")
                pos += step
                relocs.append(pos)
    for r in relocs:
        if r & 1 or r + 4 > t + d:
            raise FormatError(f"PRG fixup {r:#x} outside text+data")
    return {"format": "tos-prg", "text": t, "data": d, "bss": b, "symbols": s, "flags": flags, "image": image,
            "relocs": [(r, 4) for r in relocs], "entry": 0}


# ------------------------------------------------------------------------------------------------------------------------ Amiga hunks
HUNK_UNIT, HUNK_NAME, HUNK_CODE, HUNK_DATA, HUNK_BSS = 0x3E7, 0x3E8, 0x3E9, 0x3EA, 0x3EB
HUNK_RELOC32, HUNK_RELOC16, HUNK_RELOC8, HUNK_EXT, HUNK_SYMBOL, HUNK_DEBUG, HUNK_END, HUNK_HEADER = (0x3EC, 0x3ED, 0x3EE, 0x3EF, 0x3F0,
                                                                                                   0x3F1, 0x3F2, 0x3F3)
HUNK_DREL32, HUNK_DREL16, HUNK_DREL8, HUNK_RELOC32SHORT = 0x3F7, 0x3F8, 0x3F9, 0x3FC
# EXT reference types and the field width each patches (definitions 1-3 carry one value long)
EXT_REF_WIDTH = {129: 4, 130: 2, 131: 1, 132: 2, 133: 2, 134: 2, 136: 4, 137: 2, 138: 1, 139: 1, 140: 4}


def _hunk_name(data: bytes, off: int) -> tuple[str, int]:
    n = _u32(data, off) & 0x00FFFFFF
    raw = _take(data, off + 4, n * 4)
    return raw.rstrip(b"\0").decode("latin-1"), off + 4 + n * 4


def parse_hunks(data: bytes) -> dict:
    """Amiga hunk executable (HUNK_HEADER) or object (HUNK_UNIT). Returns hunks [{type, size, data, relocs [(offset, width)]}]."""
    first = _u32(data, 0) & 0x3FFFFFFF
    if first not in (HUNK_HEADER, HUNK_UNIT):
        raise FormatError("not an Amiga hunk file")
    kind = "exe" if first == HUNK_HEADER else "object"
    off = 4
    if kind == "exe":
        while True:  # resident library names
            n = _u32(data, off)
            off += 4
            if n == 0:
                break
            off += n * 4
        _table, f, l = _u32(data, off), _u32(data, off + 4), _u32(data, off + 8)
        off += 12
        if l < f or l - f > 4096:
            raise FormatError("bad hunk table")
        for _ in range(l - f + 1):
            sz = _u32(data, off)
            off += 4
            if (sz >> 30) == 3:
                off += 4
    else:
        _name, off = _hunk_name(data, off)
    hunks: list[dict] = []
    cur: dict | None = None
    while off < len(data):
        raw = _u32(data, off)
        t = raw & 0x3FFFFFFF
        off += 4
        if t in (HUNK_CODE, HUNK_DATA):
            n = _u32(data, off) & 0x3FFFFFFF
            cur = {"type": "CODE" if t == HUNK_CODE else "DATA", "size": n * 4, "data": _take(data, off + 4, n * 4), "relocs": []}
            hunks.append(cur)
            off += 4 + n * 4
        elif t == HUNK_BSS:
            n = _u32(data, off) & 0x3FFFFFFF
            cur = {"type": "BSS", "size": n * 4, "data": b"", "relocs": []}
            hunks.append(cur)
            off += 4
        elif t == HUNK_NAME:
            _n, off = _hunk_name(data, off - 4 + 4)
        elif t in (HUNK_RELOC32, HUNK_RELOC16, HUNK_RELOC8, HUNK_DREL32, HUNK_DREL16, HUNK_DREL8):
            width = {HUNK_RELOC32: 4, HUNK_DREL32: 4, HUNK_RELOC16: 2, HUNK_DREL16: 2, HUNK_RELOC8: 1, HUNK_DREL8: 1}[t]
            short = t == HUNK_DREL32 and kind == "exe"  # V37 executables store HUNK_RELOC32SHORT under the DREL32 id
            while True:
                if short:
                    n = _u16(data, off)
                    off += 2
                    if n == 0:
                        break
                    off += 2
                    for _ in range(n):
                        _add_reloc(cur, _u16(data, off), width)
                        off += 2
                else:
                    n = _u32(data, off)
                    off += 4
                    if n == 0:
                        break
                    off += 4
                    for _ in range(n):
                        _add_reloc(cur, _u32(data, off), width)
                        off += 4
            if short and off % 4:
                off += 2
        elif t == HUNK_RELOC32SHORT:
            while True:
                n = _u16(data, off)
                off += 2
                if n == 0:
                    break
                off += 2
                for _ in range(n):
                    _add_reloc(cur, _u16(data, off), 4)
                    off += 2
            if off % 4:
                off += 2
        elif t == HUNK_EXT:
            while True:
                w = _u32(data, off)
                off += 4
                if w == 0:
                    break
                et, nl = w >> 24, w & 0x00FFFFFF
                off += nl * 4
                if et < 128:
                    if et == 0:
                        raise FormatError("EXT_SYMB entry in HUNK_EXT")
                    off += 4  # definition value
                    continue
                if et == 130:  # EXT_COMMON: size long precedes the count
                    off += 4
                width = EXT_REF_WIDTH.get(et)
                if width is None:
                    raise FormatError(f"unsupported EXT reference type {et}")
                cnt = _u32(data, off)
                off += 4
                for _ in range(cnt):
                    _add_reloc(cur, _u32(data, off), width)
                    off += 4
        elif t == HUNK_SYMBOL:
            while True:
                n = _u32(data, off)
                off += 4
                if n == 0:
                    break
                off += n * 4 + 4
        elif t == HUNK_DEBUG:
            n = _u32(data, off)
            off += 4 + n * 4
        elif t == HUNK_END:
            cur = None
        else:
            raise FormatError(f"unsupported hunk type {t:#x}")
    if off != len(data):
        raise FormatError("trailing bytes after the last hunk")
    for h in hunks:
        h["relocs"] = sorted(set(h["relocs"]))
        for o, w in h["relocs"]:
            if o + w > len(h["data"]):
                raise FormatError("relocation outside its hunk")
    return {"format": f"amiga-hunk-{kind}", "hunks": hunks}


def _add_reloc(cur: dict | None, offset: int, width: int) -> None:
    if cur is None:
        raise FormatError("relocation outside a hunk")
    cur["relocs"].append((offset, width))


# ------------------------------------------------------------------------------------------------------------------------- ELF32
R68K_WIDTH = {1: 4, 2: 2, 3: 1, 4: 4, 5: 2, 6: 1, 7: 4, 8: 2, 9: 1, 10: 4, 11: 2, 12: 1, 13: 4, 14: 2, 15: 1, 16: 4, 17: 2, 18: 1,
              19: 4, 20: 4, 21: 4, 22: 4, 23: 4}


def parse_elf32(data: bytes) -> dict:
    """Big-endian M68K ELF32: sections (name, type, flags, addr, data, relocs [(offset, width)]), symbols, entry."""
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 2:
        raise FormatError("not a big-endian ELF32")
    etype, machine = struct.unpack_from(">HH", data, 16)
    if machine != 4:
        raise FormatError("not an M68K ELF")
    entry = _u32(data, 24)
    shoff = _u32(data, 32)
    shentsize, shnum, shstrndx = struct.unpack_from(">HHH", data, 46)
    if shentsize != 40 or shnum > 65000:
        raise FormatError("bad ELF section table")
    raw = []
    for i in range(shnum):
        b = _take(data, shoff + i * 40, 40)
        name, typ, flags, addr, off, size, link, info, _al, entsize = struct.unpack(">IIIIIIIIII", b)
        raw.append({"name_off": name, "type": typ, "flags": flags, "addr": addr, "offset": off, "size": size, "link": link, "info": info,
                    "entsize": entsize})
    strtab = _take(data, raw[shstrndx]["offset"], raw[shstrndx]["size"]) if shstrndx < len(raw) else b""

    def cstr(tab: bytes, i: int) -> str:
        if i >= len(tab):
            raise FormatError("string index out of range")
        return tab[i:tab.index(b"\0", i)].decode("latin-1")

    secs = []
    for i, r in enumerate(raw):
        secs.append({"index": i, "name": cstr(strtab, r["name_off"]), "type": r["type"], "flags": r["flags"], "addr": r["addr"],
                     "size": r["size"], "data": _take(data, r["offset"], r["size"]) if r["type"] not in (0, 8) else b"", "relocs": []})
    symbols = []
    for i, r in enumerate(raw):
        if r["type"] == 2:  # SYMTAB
            names = _take(data, raw[r["link"]]["offset"], raw[r["link"]]["size"])
            for k in range(r["size"] // 16):
                n, value, size, info, _other, shndx = struct.unpack(">IIIBBH", _take(data, r["offset"] + k * 16, 16))
                symbols.append({"name": cstr(names, n), "value": value, "size": size, "type": info & 15, "bind": info >> 4, "shndx": shndx})
        if r["type"] in (4, 9):  # RELA / REL
            ent = 12 if r["type"] == 4 else 8
            target = secs[r["info"]] if r["info"] < len(secs) else None
            if target is None:
                raise FormatError("relocation section without a target")
            for k in range(r["size"] // ent):
                off, info = struct.unpack(">II", _take(data, r["offset"] + k * ent, 8))
                rt = info & 0xFF
                if rt == 0:
                    continue
                w = R68K_WIDTH.get(rt)
                if w is None:
                    raise FormatError(f"unsupported M68K relocation type {rt}")
                if off + w > target["size"]:
                    raise FormatError("relocation outside its section")
                target["relocs"].append((off, w))
    for s in secs:
        s["relocs"] = sorted(set(s["relocs"]))
    kind = {1: "relocatable", 2: "executable"}.get(etype, "other")
    return {"format": f"elf-m68k-{kind}", "sections": secs, "symbols": symbols, "entry": entry}


# ------------------------------------------------------------------------------------------------------------------------- a.out
def parse_aout(data: bytes) -> dict:
    """a.out OMAGIC (0407) object as written by vasm -Faout: text/data + relocation entries (address, r_length)."""
    if len(data) < 32:
        raise FormatError("truncated a.out header")
    magic = _u32(data, 0)
    if magic & 0xFFFF != 0o407:
        raise FormatError("not an OMAGIC a.out")
    t, d, b, syms, entry, trsize, drsize = struct.unpack_from(">IIIIIII", data, 4)
    text = _take(data, 32, t)
    dat = _take(data, 32 + t, d)

    def rels(off: int, size: int, limit: int) -> list[tuple[int, int]]:
        out = []
        for k in range(size // 8):
            addr, info = struct.unpack(">II", _take(data, off + k * 8, 8))
            width = 1 << ((info >> 5) & 3)
            if addr + width > limit:
                raise FormatError("a.out relocation outside its segment")
            out.append((addr, width))
        return sorted(set(out))

    tr = rels(32 + t + d, trsize, t)
    dr = rels(32 + t + d + trsize, drsize, d)
    return {"format": "aout", "text": text, "data": dat, "bss": b, "text_relocs": tr, "data_relocs": dr, "entry": entry, "symbols": syms}


# ------------------------------------------------------------------------------------------------------------------------- listings
class Row:
    __slots__ = ("section", "offset", "shown", "text", "line", "truncated")

    def __init__(self, section: str, offset: int, shown: bytes, text: str, line: int, truncated: bool = False):
        self.section, self.offset, self.shown, self.text, self.line, self.truncated = section, offset, shown, text, line, truncated

    def __repr__(self):
        return f"Row({self.section!r}, {self.offset:#x}, {self.shown.hex()}, {self.text!r})"


_VASM_SEC = re.compile(r'^(\d\d): "(.*)" \(([0-9A-F]+)-([0-9A-F]+)\)')
_VASM_ROW = re.compile(r"^(\d\d):([0-9A-F]{8}) ([0-9A-F]*)\s*\t\s*(\d+): ?(.*)$")
_VASM_CONT = re.compile(r"^(\d\d):([0-9A-F]{8}) ([0-9A-F]+)\s*$")


def parse_vasm_listing(text: str) -> tuple[list[str], list[Row]]:
    """vasm `-L` listing: section table ("NN: "name" (start-end)") and rows "NN:ADDR BYTES \\t LINE: source" with byte-only
    continuation rows. Returns (section names in index order, rows)."""
    sections: dict[int, str] = {}
    rows: list[Row] = []
    lines = text.splitlines()
    i = 0
    while i < len(lines) and not lines[i].startswith("Source:"):
        m = _VASM_SEC.match(lines[i])
        if m:
            sections[int(m.group(1))] = m.group(2)
        i += 1
    for ln in lines[i:]:
        m = _VASM_ROW.match(ln)
        if m:
            sec = int(m.group(1))
            rows.append(Row(str(sec), int(m.group(2), 16), bytes.fromhex(m.group(3)), m.group(5), int(m.group(4))))
            continue
        m = _VASM_CONT.match(ln)
        if m and rows:
            prev = rows[-1]
            if str(int(m.group(1))) != prev.section or int(m.group(2), 16) != prev.offset + len(prev.shown):
                raise FormatError(f"vasm continuation row does not extend its row at {m.group(2)}")
            prev.shown += bytes.fromhex(m.group(3))
    names = [sections[k] for k in sorted(sections)]
    return names, rows


_A68K_HEX = re.compile(r"^[0-9A-F ]*$")
_A68K_SECTION = re.compile(r"^\s*(?:\S+\s+)?SECTION\s+([^,\s]+)", re.IGNORECASE)


def parse_a68k_listing(text: str) -> tuple[list[str], list[Row]]:
    """A68K 2.71 `-l` listing (fixed columns): address [0:6], bytes [8:32] (at most 12 shown: long data rows are truncated; their
    extent comes from the next row), line number right-aligned ending at column 39, source from column 40. SECTION statements name the
    sections in hunk order."""
    order: list[str] = []
    current = None
    rows: list[Row] = []
    for ln in text.splitlines():
        if len(ln) < 39 or not ln[32:39].strip().isdigit():
            continue
        addr, hexf, src = ln[0:6], ln[8:32], ln[40:]
        m = _A68K_SECTION.match(src)
        if m:
            current = m.group(1)
            if current not in order:
                order.append(current)
        if not addr.strip():
            continue
        if not re.fullmatch(r"[0-9A-F]{6}", addr) or not _A68K_HEX.match(hexf):
            raise FormatError(f"unparseable A68K row: {ln[:40]!r}")
        shown = bytes.fromhex(hexf.replace(" ", ""))
        if current is None:
            current = "<default>"
            order.append(current)
        rows.append(Row(current, int(addr, 16), shown, src, int(ln[32:39]), truncated=len(hexf.replace(" ", "")) >= 24))
    return order, rows


_GAS_ROW = re.compile(r"^\s*(\d+) ([0-9a-f]{4,8}) ([0-9A-F ]*?)\s*\t(.*)$")
_GAS_CONT = re.compile(r"^\s*(\d+)\s{6,}([0-9A-F][0-9A-F ]*?)\s*$")
_GAS_SRC = re.compile(r"^\s*(\d+)\s+\t(.*)$")


def _gas_section(stmt: str, cur: str, stack: list[str], prev: str) -> tuple[str, str]:
    """Section after one GNU as statement (GCC/handwritten .S subset): .text/.data/.bss [n], .section NAME, .previous,
    .pushsection/.popsection. Returns (new current, new previous)."""
    s = stmt.strip()
    m = re.match(r"^\.(text|data|bss)\b(?:\s+(\d+))?", s)
    if m:
        name = "." + m.group(1) + (f"#{m.group(2)}" if m.group(2) and m.group(2) != "0" else "")
        return name, cur
    m = re.match(r'^\.section\s+"?([^",\s]+)"?', s)
    if m:
        return m.group(1), cur
    if re.match(r"^\.previous\b", s):
        return prev, cur
    m = re.match(r'^\.pushsection\s+"?([^",\s]+)"?', s)
    if m:
        stack.append(cur)
        return m.group(1), cur
    if re.match(r"^\.popsection\b", s):
        if not stack:
            raise FormatError(".popsection without .pushsection")
        return stack.pop(), cur
    return cur, prev


def parse_gas_listing(text: str) -> tuple[list[str], list[Row]]:
    """GNU as `-a[lmn]` listing: "LINE OFFS BYTES\\tsource" with byte-only continuation lines; the section is tracked from the
    section-changing statements in the listed source (GNU as prints no section column)."""
    cur, prev = ".text", ".text"
    stack: list[str] = []
    order: list[str] = [".text"]
    rows: list[Row] = []
    for ln in text.splitlines():
        m = _GAS_ROW.match(ln)
        if m:
            src = _strip_expansion(m.group(4))
            r = Row(cur, int(m.group(2), 16), bytes.fromhex(m.group(3).replace(" ", "")), src.strip(), int(m.group(1)))
            rows.append(r)
            # On a section switch GNU as lists the outgoing section's end padding (m68k: to an even size) on the switching row; the
            # bytes belong to the section that was current before the switch (r.section) and are labelled as padding (unknown).
            ncur, prev = _gas_section(_strip_gas_label(src), cur, stack, prev)
            cur = ncur
        else:
            mc = _GAS_CONT.match(ln)
            if mc and rows and int(mc.group(1)) == rows[-1].line:
                rows[-1].shown += bytes.fromhex(mc.group(2).replace(" ", ""))
                continue
            ms = _GAS_SRC.match(ln)
            if ms:
                # statements listed without an offset are not rows: macro-definition bodies and false conditional-assembly branches
                # are listed that way legitimately, so their absence of bytes is not evidence of byte lag
                ncur, prev = _gas_section(_strip_gas_label(_strip_expansion(ms.group(2))), cur, stack, prev)
                cur = ncur
        if cur not in order:
            order.append(cur)
    return order, rows


def _strip_expansion(src: str) -> str:
    """GNU as marks macro-expansion / inline-assembly lines with a leading '>'."""
    s = src.lstrip()
    return s[1:] if s.startswith(">") else src


def _strip_gas_label(src: str) -> str:
    s = src.strip()
    m = re.match(r"^[A-Za-z_.$][\w.$]*:\s*(.*)$", s)
    return m.group(1) if m else s


# --------------------------------------------------------------------------------------------------------------- row semantics
_MOT_DATA = re.compile(r"^(dc|dcb|ds|blk|dx|dr)(\.[bwlsdxpq])?$|^(incbin|ascii|asciiz|string|cstring|text|fcc|fcb|fdb|byte|word|long)$",
                       re.IGNORECASE)
_MOT_ALIGN = re.compile(r"^(cnop|even|odd|align|ds\.?\s*0)$", re.IGNORECASE)
_GAS_DATA = re.compile(r"^\.(byte|2byte|4byte|8byte|word|short|hword|long|int|quad|octa|ascii|asciz|string|string8|string16|zero|skip|space|"
                       r"fill|float|single|double|uleb128|sleb128|dc(\.[bwlsdx])?|ds(\.[bwlsdx])?|incbin)$", re.IGNORECASE)
_GAS_ALIGN = re.compile(r"^\.(align|balign|balignw|balignl|p2align|p2alignw|p2alignl|even|org)$", re.IGNORECASE)
_DIRECTIVE_OTHER = re.compile(r"^\.|^(section|code|data|bss|cseg|dseg|org|rorg|equ|set|macro|endm|rept|endr|include|incdir|xdef|xref|public|"
                              r"idnt|opt|machine|mc680[0-9]0|cpu|chip|fpu|end|if\w*|else|endc|endif|reg|equr|rs\.?\w*|rsreset|rsset|"
                              r"output|list|nolist|page|ttl|spc|near|far|auto|basereg|endb|offset|clrfo|setfo|fo\.\w|so\.\w|"
                              r"globl|global|local|weak|extern|cargs|fail|printt|printv|inline|einline)$", re.IGNORECASE)


def statement(src: str, syntax: str) -> tuple[str, str]:
    """(mnemonic, operands) of one listed source line with label and comment removed ('' when it is not a statement)."""
    s = src.rstrip("\n")
    if syntax == "gas":
        s = s.split("|", 1)[0] if "|" in s and "'" not in s and '"' not in s else s
        s = _strip_gas_label(s)
        if s.startswith("#") or not s:
            return "", ""
    else:
        if not s or s[0] in "*;":
            return "", ""
        if s[0] not in " \t":  # a label in column 1 (with or without ':')
            parts = s.split(None, 1)
            s = parts[1] if len(parts) > 1 else ""
        s = s.strip()
        m = re.match(r"^[A-Za-z_.@$][\w.@$]*:\s*(.*)$", s)
        if m and not re.match(r"^\w+\.[bwlsdxpq]\b", s, re.IGNORECASE):
            s = m.group(1)
        s = re.split(r"\s;|^;", s, maxsplit=1)[0].strip()
    if not s:
        return "", ""
    parts = s.split(None, 1)
    return parts[0], (parts[1] if len(parts) > 1 else "")


def row_kind(mnemonic: str, syntax: str) -> str:
    """insn | data | align | other. Every emitting row that is neither data nor alignment nor another directive is an instruction
    candidate; the CPU-owned decoder then decides whether it is a legal base-68000 instruction of the listed length."""
    m = mnemonic
    if not m:
        return "other"
    if syntax == "gas":
        if _GAS_DATA.match(m):
            return "data"
        if _GAS_ALIGN.match(m):
            return "align"
        if m.startswith("."):
            return "other"
        return "insn"
    if _MOT_DATA.match(m):
        return "data"
    if _MOT_ALIGN.match(m):
        return "align"
    if _DIRECTIVE_OTHER.match(m):
        return "other"
    return "insn"
