#!/usr/bin/env python3
"""SEG-044-T002: deterministic, ROM-bound, fail-closed source-derived M68K executable-universe extractor.

Input: an assembler LISTING (never source text) for an exact external source project that rebuilds the pinned ROM,
plus that ROM. Output: `segarecomp.m68k_source_universe.v1`, the set `C` of instruction-start addresses of every
listing row that was assembled while the effective CPU was MC68000 and whose mnemonic is in the closed MC68000 set.

Trust contract (ADR 0093): `C` is an EXTERNAL semantic-completeness authority; segarecomp re-verifies structure only
(mapped/even/decodable/bounded/ROM hash). Anything this tool does not recognise is NO AUTHORITY: the whole extraction
fails with a stable error class, it never guesses a code/data classification. The listing and its contents are
commercial-derived and are never printed; diagnostics contain only the error class and counts.

Listing dialects (SEG-048 ADR 0099 adds the last two without changing the first): `as` is the Macroassembler AS `-L` listing with `listing purecode` (macro invocations appear as `(MACRO)` rows
followed by their expanded rows; skipped conditional regions are absent; macro DEFINITION bodies appear without bytes). AS Bld 89 prints the
continuation bytes of a long row on an address-less line, accepted only when they extend the preceding row. `asm68k` is the SN 68k 2.53 listing
(`ADDR bytes source`, 24-column byte field, `+` marks truncation, `M` marks macro-expanded rows); instruction extents are the displayed bytes,
truncated data extents come from the next listing address, and the coverage accounting below catches any wrong inference (overlap or gap).
A uniform 00/FF gap whose END is an `org` directive row is explained as forward `org` padding (p2bin `-p=FF`); a Z80 `save`...`restore` block whose
`save` follows a rebasing `org 0` is anchored at the last ROM-address row before that `org`.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys

SCHEMA = "segarecomp.m68k_source_universe.v1"
MAX_ENTRIES = 1 << 16          # same defence-in-depth bound as the island bound
MAX_LISTING_BYTES = 256 << 20  # bounded input
MAX_ROM_BYTES = 1 << 24

# Closed MC68000 mnemonic set (base mnemonic, size suffix stripped). Anything else with emitted bytes is NO AUTHORITY.
M68K = frozenset("""
abcd add adda addi addq addx and andi asl asr
bcc bcs beq bge bgt bhi bhs ble blo bls blt bmi bne bpl bvc bvs bra bsr bchg bclr bset btst
chk clr cmp cmpa cmpi cmpm
dbcc dbcs dbeq dbf dbge dbgt dbhi dble dbls dblt dbmi dbne dbpl dbra dbt dbvc dbvs dbhs dblo
divs divu eor eori exg ext illegal jmp jsr lea link lsl lsr
move movea movem movep moveq muls mulu nbcd neg negx nop not or ori pea reset rol ror roxl roxr rte rtr rts
sbcd scc scs seq sf sge sgt shi sle sls slt smi sne spl st svc svs shs slo stop sub suba subi subq subx swap tas trap trapv tst unlk
""".split())
# Directives that legitimately emit bytes but assert nothing about execution.
DATA = frozenset("dc dw db ds dcb even cnop align padding".split())
# Directives allowed to START an otherwise unexplained ROM range (binary assets, padding, Z80 blocks).
BLOCK_CLOSERS = frozenset(["endm", "endr", "endw"])
BLOCK_OPENERS = frozenset("while rept irp irpc".split())
ASSET_GAP = frozenset("binclude incbin save".split())        # sized by the directive (binary assets, Z80 blocks)
PAD_GAP = frozenset("even cnop align org ds dcb padding".split())  # must be a uniform 00/FF fill
GAP = ASSET_GAP | PAD_GAP

# ASM68K macro bodies: directives that neither emit nor control instructions / that emit data only. A macro is DATA-ONLY when every line of
# its echoed definition body is one of these or a call to another data-only macro; bytes it emits without a listing row are data.
NEUTRAL = frozenset("""rept endr shift case endcase if ifne ifeq ifd ifnd ifc ifnc ifdef ifndef ifge ifgt ifle iflt else elseif endif endc mexit
fail inform rsset rsreset opt list nolist nopage equ set = rs rs.b rs.w rs.l equr reg include macro endm""".split())
DATA_EMITTERS = frozenset("dc dcb ds incbin binclude even cnop align padding".split())

ROW = re.compile(r"^(?:\(\d+\))?\s*(\d+)/\s*([0-9A-F]+) :(?: (.*))?$")
CONT = re.compile(r"^\s+([0-9A-F]+) : (.*)$")
HEX = re.compile(r"(?:[0-9A-F]{2}(?:[0-9A-F]{2})?(?: |$))+")
SHA = re.compile(r"[0-9a-f]{64}")
REV = re.compile(r"[0-9a-f]{40}")
LABEL = re.compile(r"^[A-Za-z_.@][\w.@]*:(.*)$")
TOKEN = re.compile(r"[A-Za-z0-9._+-]{1,64}")


class SourceMapError(Exception):
    """Stable, content-free failure class (never includes listing text)."""

    def __init__(self, code: str, count: int = 0):
        super().__init__(code)
        self.code = code
        self.count = count


def _fail(code: str, count: int = 0):
    raise SourceMapError(code, count)


def _tokens(src: str) -> list[str]:
    text = src.split(";", 1)[0].strip()
    parts = text.split()
    if parts:
        label = LABEL.match(parts[0])
        if label:  # `label:` or `label:directive`
            parts = ([label.group(1)] if label.group(1) else []) + parts[1:]
    return parts


CONT2 = re.compile(r"^\s{10,}((?:[0-9A-F]{2}(?:[0-9A-F]{2})?(?: |$))+)\s*$")  # AS Bld 89 address-less continuation
HEX8 = re.compile(r"[0-9A-F]{8}")


def _collect_as(listing: str):
    """Macroassembler AS dialect -> (rows, gap_tokens). A row is [addr, bytes, kind, base, length]."""
    stack: list[str] = []
    mode = "68000"
    macro_depth = 0
    rows = []
    gap_tokens: dict[int, set[str]] = {}
    last = None
    seen_row = False
    anchor = None  # address of the latest non-`org` row (the ROM position a rebasing `org 0` + `save` block hangs off)
    for raw in listing.split("\n"):
        line = raw.rstrip("\r")
        match = ROW.match(line.rstrip())
        if match:
            seen_row = True
            last = None
            addr = int(match.group(2), 16)
            rest = match.group(3) or ""
            field = rest[:20].ljust(20)
            src = rest[20:]
            tokens = _tokens(src)
            if field.strip() == "ALL":
                field = " " * 20
            first = tokens[0].lower().lstrip("!") if tokens else ""
            if macro_depth:
                if field.strip() and HEX.fullmatch(field.rstrip()):
                    _fail("bytes_inside_macro_definition")
                lowered = [t.lower() for t in tokens[:2]]
                if first in BLOCK_CLOSERS:
                    macro_depth -= 1
                elif first in BLOCK_OPENERS or "macro" in lowered:
                    macro_depth += 1  # nested `while`/`rept`/`irp`/`macro` blocks also close with `endm`
                continue
            if len(tokens) >= 1 and (first == "macro" or (len(tokens) >= 2 and tokens[1].lower() == "macro")):
                macro_depth = 1
                continue
            if first == "save":
                stack.append(mode)
                gap_tokens.setdefault(addr, set()).add("save")
                if anchor is not None and anchor != addr:
                    gap_tokens.setdefault(anchor, set()).add("save")
                continue
            if first == "restore":
                if not stack:
                    _fail("unbalanced_restore")
                mode = stack.pop()
                continue
            if first == "cpu":
                if len(tokens) != 2 or tokens[1].lower() not in ("68000", "z80"):
                    _fail("unknown_cpu")
                mode = "z80" if tokens[1].lower() == "z80" else "68000"
                continue
            if first.lstrip("!") != "org":
                anchor = addr
            fieldtxt = field.rstrip()
            if fieldtxt and HEX.fullmatch(fieldtxt):
                data = bytes.fromhex(fieldtxt.replace(" ", ""))
                base = first.split(".")[0]
                if mode == "z80":
                    kind = "Z"
                elif base in M68K:
                    kind = "I"
                elif base in DATA:
                    kind = "D"
                else:
                    _fail("unknown_construct_with_bytes")
                row = [addr, bytearray(data), kind, base, 0]
                rows.append(row)
                last = row
            elif first in GAP:
                gap_tokens.setdefault(addr, set()).add(first)
            continue
        cont = CONT.match(line.rstrip())
        if cont and last is not None:
            field = cont.group(2)[:20].rstrip()
            if field and HEX.fullmatch(field):
                addr = int(cont.group(1), 16)
                if addr != last[0] + len(last[1]):
                    _fail("listing_continuation_gap")
                last[1] += bytes.fromhex(field.replace(" ", ""))
            continue
        cont2 = CONT2.match(line.rstrip())
        if cont2 and last is not None:
            last[1] += bytes.fromhex(cont2.group(1).replace(" ", ""))
    if not seen_row:
        _fail("not_a_listing")
    if macro_depth:
        _fail("unterminated_macro_definition")
    if stack or mode != "68000":
        _fail("cpu_mode_not_restored")
    for row in rows:
        row[4] = len(row[1])
    return rows, gap_tokens


def _asm68k_tokens(src: str):
    """Mnemonic tokens of an ASM68K source column: an unindented first token is a label (with or without `:`)."""
    text = src.split(";", 1)[0]
    if text.lstrip().startswith("*"):
        return []
    parts = text.split()
    if not parts:
        return []
    label = LABEL.match(parts[0])
    if label:  # `label:` or `label:mnemonic`
        return ([label.group(1)] if label.group(1) else []) + parts[1:]
    if text[:1] not in (" ", "\t"):  # unindented token without a colon is a label
        return parts[1:]
    return parts


def _collect_asm68k(listing: str):
    """SN 68k 2.53 dialect -> (rows, gap_tokens, data_spans); see the module docstring for the extent rules."""
    rows = []
    gap_tokens: dict[int, set[str]] = {}
    macros: dict[str, list[str]] = {}
    current: list[str] | None = None
    calls: list[tuple[int, str, int]] = []  # (line index, called token, address) of rows that list no bytes
    addrs: list[int] = []
    pending = None  # truncated data row waiting for the next address to fix its extent
    seen = False
    for raw in listing.split("\n"):
        line = raw.rstrip("\r")
        if len(line) < 9 or line[8] != " " or not HEX8.fullmatch(line[:8]):
            continue
        seen = True
        addr = int(line[:8], 16)
        addrs.append(addr)
        if pending is not None and addr > pending[0]:
            if addr - pending[0] < len(pending[1]):
                _fail("data_extent_unknown")
            pending[4] = addr - pending[0]
            pending = None
        field = line[9:33].rstrip()
        truncated = line[33:34] == "+"
        src = line[35:]
        if field.startswith("="):
            continue
        tokens = _asm68k_tokens(src)
        if tokens and tokens[0].startswith("=") and len(tokens[0]) > 1:  # `=3 dc.b ...` / `=? dc.b ...` case-arm marker
            tokens = tokens[1:]
        first = tokens[0].lower() if tokens else ""
        base = first.split(".")[0].lstrip("!")
        raw_tokens = src.split(";", 1)[0].split()
        if current is not None:
            if field:
                _fail("bytes_inside_macro_definition")
            if base == "endm":
                current = None
            elif base:
                current.append(base)
            continue
        if "macro" in [t.lower() for t in raw_tokens[:2]] and not field:
            name = raw_tokens[0].rstrip(":").lower() if raw_tokens[0].lower() != "macro" else ""
            if not name:
                _fail("macro_without_name")
            current = macros.setdefault(name, [])
            continue
        if field:
            if not HEX.fullmatch(field):
                _fail("unparseable_byte_field")
            if base in M68K:
                kind = "I"
            elif base in DATA:
                kind = "D"
            else:
                _fail("unknown_construct_with_bytes")
            data = bytes.fromhex(field.replace(" ", ""))
            row = [addr, bytearray(data), kind, base, len(data)]
            if truncated:
                if kind != "D":
                    _fail("instruction_truncated")
                pending = row
                row[4] = 0
            rows.append(row)
        else:
            if base in GAP:
                gap_tokens.setdefault(addr, set()).add(base)
            if base:
                calls.append((len(addrs) - 1, base, addr))
    if not seen:
        _fail("not_a_listing")
    if pending is not None or current is not None:
        _fail("data_extent_unknown" if pending is not None else "unterminated_macro_definition")
    # Data-only closure (least fixed point of "no instruction mnemonic, every call is to a data-only macro").
    data_only: set[str] = set()
    changed = True
    while changed:
        changed = False
        for name, body in macros.items():
            if name in data_only:
                continue
            if all((t in NEUTRAL or t in DATA_EMITTERS or t in data_only) and t not in M68K for t in body):
                data_only.add(name)
                changed = True
    spans = []
    for index, base, addr in calls:
        if base in data_only:
            following = next((a for a in addrs[index + 1:] if a > addr), None)
            if following is not None:
                spans.append((addr, following))
    return rows, gap_tokens, spans


def parse_listing(listing: str, rom: bytes, dialect: str = "as"):
    """Returns (instruction_starts, summary). Raises SourceMapError."""
    if len(listing) > MAX_LISTING_BYTES:
        _fail("listing_too_large")
    if len(rom) == 0 or len(rom) > MAX_ROM_BYTES or len(rom) % 2:
        _fail("rom_size")
    spans = []
    if dialect == "as":
        rows, gap_tokens = _collect_as(listing)
        exact = True
    elif dialect == "asm68k":
        rows, gap_tokens, spans = _collect_asm68k(listing)
        exact = False  # the listing shows first-pass placeholder operands/data; opcode words and final addresses are still checked
    else:
        _fail("unknown_dialect")
    return _account(rows, gap_tokens, rom, exact, spans)


def _account(rows, gap_tokens, rom: bytes, exact: bool, data_spans=()):
    # cell state: 0 none, 1 instruction opcode word, 3 instruction operand byte, 2 data, 4 operand byte overwritten by data
    cover = bytearray(len(rom))
    starts = set()
    counts = {"I": 0, "D": 0}
    pending_diffs: set[int] = set()
    overlays = 0
    for addr, data, kind, base, length in rows:
        if kind == "Z":
            continue
        end = addr + length
        if end > len(rom):
            _fail("row_outside_rom")
        if kind == "I":
            if addr & 1 or length < 2 or length & 1 or len(data) != length:
                _fail("instruction_odd")
            if bytes(data[:2]) != rom[addr:addr + 2]:
                # First-pass listings (ASM68K) print a forward-referenced 8-bit displacement/immediate as 00 inside a 2-byte branch or moveq
                # opcode word; the high byte (the operation) must still match the ROM, nothing else is relaxed.
                relaxed = (not exact and length == 2 and data[0] == rom[addr]
                           and (0x60 <= data[0] <= 0x6F or (0x70 <= data[0] <= 0x7E and data[0] % 2 == 0)))
                if not relaxed:
                    _fail("instruction_opcode_differs_from_rom")
            if exact:
                for offset in range(length):
                    if data[offset] != rom[addr + offset]:
                        pending_diffs.add(addr + offset)  # must later be explained by a data overwrite
            starts.add(addr)
            counts["I"] += length
        else:
            if exact and bytes(data) != rom[addr:end]:
                _fail("data_differs_from_rom")
            counts["D"] += length
        for position in range(addr, end):
            state = cover[position]
            if state:
                # The source's own `org *-1` / `dc.b` fixup idiom: a DATA row may overwrite an instruction OPERAND byte
                # (never an opcode word, other data, or another instruction) with the final ROM value.
                if kind == "D" and state == 3 and exact:
                    cover[position] = 4
                    overlays += 1
                    continue
                _fail("overlapping_rows")
            cover[position] = (1 if position - addr < 2 else 3) if kind == "I" else 2
    if pending_diffs and any(cover[position] != 4 for position in pending_diffs):
        _fail("listing_bytes_differ_from_rom")
    # Bytes a data-only macro emitted without a listing row (ASM68K `case` arms) are data: they are claimed only where no row covers them.
    for begin, end in data_spans:
        for position in range(begin, min(end, len(rom))):
            if not cover[position]:
                cover[position] = 2
                counts["D"] += 1
    # Every uncovered ROM range must START at a directive that explains it (binary include, padding, Z80 block).
    unexplained = 0
    position = 0
    uncovered = 0
    while position < len(rom):
        if cover[position]:
            position += 1
            continue
        begin = position
        while position < len(rom) and not cover[position]:
            position += 1
        uncovered += position - begin
        # Split the gap at every directive row inside it; each segment must start at its own explaining directive.
        marks = [begin] + [a for a in sorted(gap_tokens) if begin < a < position and gap_tokens[a] & GAP]
        marks.append(position)
        for seg_begin, seg_end in zip(marks, marks[1:]):
            tokens_here = gap_tokens.get(seg_begin, set())
            if tokens_here & ASSET_GAP:
                continue
            fill = set(rom[seg_begin:seg_end])
            if (tokens_here & PAD_GAP) and len(fill) == 1 and fill <= {0x00, 0xFF}:
                continue
            # Forward `org` padding: the gap ends exactly at the `org` row (the listing prints the post-org PC) and is one uniform fill.
            if "org" in gap_tokens.get(seg_end, set()) and len(fill) == 1 and fill <= {0x00, 0xFF}:
                continue
            unexplained += 1
    if unexplained:
        _fail("unexplained_rom_range", unexplained)
    if not starts:
        _fail("empty_universe")
    if len(starts) > MAX_ENTRIES:
        _fail("universe_too_large")
    return sorted(starts), {
        "instruction_starts": len(starts), "instruction_bytes": counts["I"], "data_bytes": counts["D"],
        "other_bytes": uncovered, "operand_overlays": overlays,
    }


def format_universe(starts, rom_sha256: str, producer: str, source_revision: str, source_config: str) -> str:
    if not SHA.fullmatch(rom_sha256) or not REV.fullmatch(source_revision):
        _fail("bad_identity")
    for value in (producer, source_config):
        if not TOKEN.fullmatch(value):
            _fail("bad_identity")
    out = [SCHEMA, f"rom_sha256 {rom_sha256}", f"producer {producer}", f"source_revision {source_revision}",
           f"source_config {source_config}", f"entries {len(starts)}"]
    out += [f"{a:08x}" for a in starts]
    out.append("end")
    return "\n".join(out) + "\n"


def extract(listing: str, rom: bytes, source_revision: str, expect_source_revision: str | None,
            producer: str = "s1disasm-listing-v1", source_config: str = "none", expect_rom_sha256: str | None = None,
            dialect: str = "as"):
    rom_sha256 = hashlib.sha256(rom).hexdigest()
    if expect_rom_sha256 is not None and expect_rom_sha256 != rom_sha256:
        _fail("rom_hash_mismatch")
    if expect_source_revision is not None and expect_source_revision != source_revision:
        _fail("source_revision_mismatch")
    starts, summary = parse_listing(listing, rom, dialect)
    return format_universe(starts, rom_sha256, producer, source_revision, source_config), summary


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--listing", required=True)
    parser.add_argument("--rom", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--source-revision", required=True, help="exact 40-hex source revision the listing was built from")
    parser.add_argument("--source-dir", help="checkout; HEAD must equal --source-revision")
    parser.add_argument("--source-config", default="none", help="token naming the source's own configuration switch (e.g. Revision0)")
    parser.add_argument("--expect-rom-sha256")
    parser.add_argument("--producer", default="s1disasm-listing-v1")
    parser.add_argument("--dialect", choices=("as", "asm68k"), default="as")
    args = parser.parse_args(argv)
    try:
        if args.source_dir:
            head = subprocess.run(["git", "-C", args.source_dir, "rev-parse", "HEAD"], capture_output=True, text=True, check=False)
            if head.returncode != 0 or head.stdout.strip() != args.source_revision:
                _fail("source_revision_mismatch")
        rom = open(args.rom, "rb").read(MAX_ROM_BYTES + 1)
        listing = open(args.listing, "rb").read(MAX_LISTING_BYTES + 1).decode("ascii", errors="replace")
        text, summary = extract(listing, rom, args.source_revision, None, args.producer, args.source_config, args.expect_rom_sha256, args.dialect)
    except SourceMapError as error:
        print(json.dumps({"schema": "segarecomp.m68k_source_universe.error.v1", "error": error.code, "count": error.count}))
        return 1
    except OSError:
        print(json.dumps({"schema": "segarecomp.m68k_source_universe.error.v1", "error": "io", "count": 0}))
        return 1
    with open(args.output, "w", encoding="ascii", newline="\n") as sink:
        sink.write(text)
    print(json.dumps({"schema": "segarecomp.m68k_source_universe.summary.v1", **summary}, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
