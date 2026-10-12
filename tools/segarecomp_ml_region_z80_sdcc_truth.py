#!/usr/bin/env python3
"""SEG-056 (ADR 0117): source-backed instruction-start truth from SDCC/ASxxxx relocated listings (`sdldz80 -u` -> `.rst`).

Private research tooling; stdlib only. Input: relocated listings of the GAME-AUTHORED modules plus the linked ROM. Output: a
class string with the SEG-051 alphabet I (instruction start) / c (continuation) / D (confident data) / A (ambiguous) / U
(unlisted or deliberately masked, e.g. shared library, crt0 and runtime modules).

Rules (documented in ADR 0117):
- A row's bytes must equal the ROM at the listed address (identity of listing and image). The only tolerated mismatch is the
  SMS header region 0x7FF0-0x7FFF, which `ihx2sms` rewrites (checksum); any other mismatch aborts (`main` exits non-zero).
- Mnemonic rows are instruction starts. In an SDCC-generated module, `.db/.dw/.ds` rows between two data-free rows of code are
  `A` (inline assembly could hide code); every other `.db/.dw/.ds` row is compiler-emitted constant data (`D`).
  In a hand-written module every `.db` is `A`.
- Shared libraries (SMSlib, PSGlib, crt0, runtime) are NOT labelled: they are the same code in every SDCC lineage and would
  break lineage independence, so their bytes stay `U`.
"""
from __future__ import annotations

import hashlib
import json
import re
import sys

MN = set("adc add and bit call ccf cp cpd cpdr cpi cpir cpl daa dec di djnz ei ex exx halt im in inc ind indr ini inir jp jr "
         "ld ldd lddr ldi ldir neg nop or otdr otir out outd outi pop push res ret reti retn rl rla rlc rlca rld rr rra rrc "
         "rrca rrd rst sbc scf set sla sll sra srl sub xor".split())
DATA = (".db", ".dw", ".ds", ".byte", ".word", ".blkb", ".blkw", ".ascii", ".asciz", ".str")
ROW = re.compile(r"^\s*([0-9A-F]{6})\s+((?:[0-9A-F]{2} )+)\s*(?:\[\s*\d+\])?\s+(\d+)\s?(.*)$")
LABEL = re.compile(r"^(?:[A-Za-z_.$][\w.$]*::?|\d+\$:)\s*")


def classify_listing(text: str, rom: bytes, sdcc_generated: bool):
    """Return (per-offset class list overlay: dict offset -> class, stats)."""
    rows = []  # (offset, nbytes, kind) kind in I/D/A/cont
    bad = 0
    bad_offsets = []
    for line in text.splitlines():
        m = ROW.match(line)
        if not m:
            continue
        off = int(m.group(1), 16)
        hexs = m.group(2).split()
        src = m.group(4).split(";")[0].rstrip()
        src = src.lstrip()
        while True:
            lm = LABEL.match(src)
            if not lm or not src:
                break
            src = src[lm.end():]
        tok = src.split()[0].lower() if src.split() else ""
        data = bytes(int(h, 16) for h in hexs)
        if off + len(data) > len(rom) or rom[off:off + len(data)] != data:
            bad += 1
            bad_offsets.append(off)
            continue
        if tok in MN:
            kind = "I"
        elif tok.startswith(DATA):
            kind = "d"
        elif tok == "":
            kind = "cont"
        else:
            continue
        rows.append((off, len(data), kind))
    out = {}
    prev_kind = None
    for i, (off, n, kind) in enumerate(rows):
        if kind == "I":
            out[off] = "I"
            for k in range(1, n):
                out[off + k] = "c"
        elif kind == "d":
            if sdcc_generated:
                before = rows[i - 1][2] if i else None
                after = rows[i + 1][2] if i + 1 < len(rows) else None
                cls = "A" if before == "I" and after == "I" else "D"
            else:
                cls = "A"
            for k in range(n):
                out.setdefault(off + k, cls)
            prev_kind = cls
            continue
        else:  # continuation row of a wrapped data/instruction row
            cls = prev_kind or "A"
            for k in range(n):
                out.setdefault(off + k, cls)
            continue
        prev_kind = None
    classify_listing.last_bad_offsets = bad_offsets
    return out, bad


HEADER_REWRITTEN = range(0x7FF0, 0x8000)


def build_truth(rom: bytes, listings, sdcc_flags):
    cls = ["U"] * len(rom)
    total_bad = 0
    build_truth.bad_offsets = []
    for text, flag in zip(listings, sdcc_flags):
        o, bad = classify_listing(text, rom, flag)
        total_bad += bad
        build_truth.bad_offsets += classify_listing.last_bad_offsets
        for off, c in o.items():
            if cls[off] == "U" or (cls[off] == "A" and c in "ID") or (c == "I"):
                cls[off] = c
    s = "".join(cls)
    return {"rom_sha256": hashlib.sha256(rom).hexdigest(), "classes": s}, total_bad


def main(argv):
    if len(argv) < 4:
        raise SystemExit("usage: rom out.json sdcc|hand:LISTING.rst [...]")
    rom = open(argv[1], "rb").read()
    listings, flags = [], []
    for spec in argv[3:]:
        kind, path = spec.split(":", 1)
        listings.append(open(path, errors="replace").read())
        flags.append(kind == "sdcc")
    truth, bad = build_truth(rom, listings, flags)
    unexplained = [o for o in build_truth.bad_offsets if o not in HEADER_REWRITTEN]
    if unexplained:
        raise SystemExit(f"listing does not match the ROM at {len(unexplained)} row(s) outside the rewritten header")
    json.dump(truth, open(argv[2], "w"))
    s = truth["classes"]
    print(json.dumps({"size": len(rom), **{k: s.count(k) for k in "IcDAU"}, "inconsistent_rows": bad}))


if __name__ == "__main__":
    main(sys.argv)
