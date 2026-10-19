#!/usr/bin/env python3
"""SEG-075-T002 (ADR 0160): canonical cross-platform MC68000 corpus extraction with checkable label provenance (OFFLINE research tooling).

  truth     for the compiler-path programs whose assembler listings T001 never generated (GCC 13.3 / GCC 4.6.4 / vbcc), rerun the qualified
            recipe in a fresh copy of the pinned source inside the SAME preserved offline toolchain image, with the assembler wrapped to
            also write its listing (`as -almn=<obj>.lst`, `vasm -L <obj>.lst`). The truth run is accepted only if every M68K object and
            every final executable it produces is byte-identical to the qualified T001 run `a`; then the listings provably describe the
            qualified objects
  extract   normalize every T001-qualified program into canonical samples (one per loadable image: PRG text+data, each Amiga hunk, a raw
            ROM, an ELF load image, or an object-only text segment). Labels come only from assembler/compiler listings:
              1. each listing row's extent is the distance to the next emitting row; listed bytes must equal the assembled section bytes
                 outside relocation fields, or the whole section stays unknown;
              2. each assembled section is placed in the final image only where its non-relocated bytes match at exactly one aligned
                 position (or, without a separate object, the listing section IS the executable hunk);
              3. instruction rows become I + c..c, data rows D, alignment/padding, macro and other rows U, unexplained bytes U;
              4. every I row is checked with the CPU-owned base-MC68000 decoder (`segarecomp m68k-decode-token-report`): it must decode
                 as an ordinary instruction (or the intended ILLEGAL form) of exactly the listed length, else it is typed
                 (DECODER_REJECTED / EXCEPTION_FORM / LENGTH_MISMATCH / ODD_START) and becomes U, so no unsupported 68010+/68020+ form
                 (guarded EmuTOS code, hand-encoded dc.w words) enters the base-68000 code labels;
              5. a data row that directly follows a non-terminating instruction (execution may fall into it: inline arguments, encoded
                 instructions) becomes U (DATA_AFTER_FALLTHROUGH);
            Output per sample: image.bin, classes.bin (I/c/D/U), tokens.bin, seeds.json (cells of independently established executable
            roots: the container entry only), relocs.json, spans.json (typed failures), meta.json (identity, provenance, provisional
            lineage, audit) - the SEG-072 `segarecomp_ml_v3_train.Image` layout, loadable unchanged
  coverage  sanitized aggregate coverage (per program/platform: starts, I/c/D/U fractions, decode agreement, typed failures, placement)
  load      smoke: load every canonical sample with the unchanged SEG-072 loader (`segarecomp_ml_v3_train.Image`)

Unknown stays unknown: compiler symbol tables, linear disassembly and section flags never create labels. Durable output is aggregate-only.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_xplat_formats as fm  # noqa: E402

SCHEMA = "segarecomp.xplat_corpus.sample.v1"
CELL_BYTES = 128
COVERAGE = HERE / "segarecomp_xplat_corpus.coverage.json"
TOKEN_FIELDS = 8
# control classes of the decode token (hybrid_admission.cpp): 2 direct jump, 4 dynamic jump, 6 return are terminators
TERMINATING_CONTROL = (2, 4, 6)
MIN_PLACE_BYTES = 8
SPAN_CAP = 20000  # spans.json keeps at most this many typed spans per program; the overflow is counted as SPANS_TRUNCATED

# ------------------------------------------------------------------------------------------------------------------ truth recipes
# Assembler wrapper installed inside the ephemeral container (never persisted): every invocation that writes `-o X` also writes the
# listing X.lst. The wrapper calls the real assembler by absolute path, so symlinked tool names (neogeosdk's toolchain links) work.
GAS_WRAP = r'''set -e
wrap_gas() { real=$(readlink -f "$1"); [ -f "$real.real" ] && return 0; mv "$real" "$real.real"
  printf '#!/bin/sh\nexec python3 /wrap/_relist_wrap.py %s.real "$@"\n' "$real" > "$real"; chmod +x "$real"; }
for g in "$@"; do wrap_gas "$($g -print-prog-name=as)"; a=$(command -v "${g%gcc}as" || true); [ -n "$a" ] && wrap_gas "$a"; done
'''
# GNU as relist wrapper (python3, inside the container): assemble exactly as invoked plus a listing, keep the input, and re-assemble a
# copy with only the `#APP`/`#NO_APP` scrubbing markers removed (GNU as loses its byte-to-line attribution after inline-asm blocks).
RELIST_WRAP = (HERE / "segarecomp_xplat_relist_wrap.py")
VASM_WRAP = r'''set -e
real=$(readlink -f "$(command -v vasmm68k_mot)"); mv "$real" "$real.real"
printf '#!/bin/sh\nout=""; prev=""; has=0\nfor a in "$@"; do [ "$prev" = "-o" ] && out="$a"; [ "$a" = "-L" ] && has=1; prev="$a"; done\nif [ -n "$out" ] && [ $has = 0 ]; then exec %s.real -L "$out.lst" "$@"; fi\nexec %s.real "$@"\n' "$real" "$real" > "$real"; chmod +x "$real"
'''
TRUTH = {
    ("toslibc", "lib+examples"): {"wrap": "gas", "tools": ["m68k-elf-gcc"]},
    ("neogeosdk", "demo-compile-link"): {"wrap": "gas", "tools": ["m68k-elf-gcc"]},
    ("space-invaders", "objects-vbcc"): {"wrap": "vasm"},
    **{("emutos", r): {"wrap": "gas", "tools": ["m68k-atari-mint-gcc"]} for r in ("rom-192", "rom-256", "cart", "amiga-rom")},
}

# ----------------------------------------------------------------------------------------------------------------- program specs
# Fixed by the T001 format findings (ADR 0159/0160). Each program: where its listings/objects live ("t001" = the qualified run `a`,
# "truth" = the listing-instrumented truth run), the listing syntax, how listing sections reach the final image, and the final images.
PLATFORM = {"BattleIsleEditor": "amiga", "amiga-realtime3d": "amiga", "HelloAmi": "amiga", "toslibc": "atari-st", "emutos": "atari-st+amiga",
            "space-invaders": "atari-st", "neogeosdk": "neo-geo"}
TOOLCHAIN = {"BattleIsleEditor": "A68K 2.71 + vlink 0.18a", "amiga-realtime3d": "vasm 2.0f", "HelloAmi": "vasm 2.0f",
             "toslibc": "m68k-elf GCC 13.3.0 + binutils 2.42", "emutos": "m68k-atari-mint GCC 4.6.4 + binutils 2.30",
             "space-invaders": "vbcc 0.9g + vasm 2.0f", "neogeosdk": "m68k-elf GCC 13.3.0 + binutils 2.42"}
WORKLOAD = {"BattleIsleEditor": "application", "amiga-realtime3d": "demo/tutorial", "HelloAmi": "hello-world+bootblock",
            "toslibc": "application+runtime-library", "emutos": "firmware", "space-invaders": "game", "neogeosdk": "sdk+game"}


def program_specs() -> list[dict]:
    specs = []
    for n in ("BIEd16", "BIEdDD112", "BIEdDD211", "BIEdDD211_512K", "HLEd12", "HLEd12_512K"):
        specs.append({"cid": "BattleIsleEditor", "recipe": "editors", "root": "t001", "syntax": "a68k", "unit": n,
                      "units": [{"listing": f"out/{n}.lst", "object": f"out/{n}.o"}], "final": {"kind": "hunk-exe", "path": f"out/{n}"}})
    for i in range(1, 10):
        specs.append({"cid": "amiga-realtime3d", "recipe": "programs", "root": "t001", "syntax": "vasm", "unit": f"Program_{i:02d}",
                      "units": [{"listing": f"out/P{i:02d}.lst", "object": None}], "final": {"kind": "hunk-exe", "path": f"out/Program_{i:02d}"}})
    for n, final in (("HelloAmi", "HelloAmi"), ("BBlock0x.bb", "BBlock0x.bb"), ("Disk.info", "Disk.info"), ("HelloAmi.info", "HelloAmi.info")):
        specs.append({"cid": "HelloAmi", "recipe": "units", "root": "t001", "syntax": "vasm", "unit": n,
                      "units": [{"listing": f"out/{n}.lst", "object": None}], "final": {"kind": "hunk-exe", "path": f"out/{final}"}})
    for n in ("example/ALERT.PRG", "example/COOKIE.TOS", "example/DESK.ACC", "example/HELLO.TOS", "example/WINDOW.PRG", "example/XBRA.PRG",
              "test/SUITE.TOS"):
        specs.append({"cid": "toslibc", "recipe": "lib+examples", "root": "truth", "syntax": "gas", "unit": n, "units": "objects:**/*.o",
                      "final": {"kind": "prg", "path": n}})
    for r, img in (("rom-192", "etos192us.img"), ("rom-256", "etos256us.img"), ("cart", "etoscart.img"), ("amiga-rom", "emutos-amiga.rom")):
        specs.append({"cid": "emutos", "recipe": r, "root": "truth", "syntax": "gas", "unit": img, "units": "objects:obj/*.o",
                      "final": {"kind": "raw-rom", "path": img}})
    specs.append({"cid": "neogeosdk", "recipe": "demo-compile-link", "root": "truth", "syntax": "gas", "unit": "game", "units": "objects:out/*.o",
                  "final": {"kind": "elf-exe", "path": "out/game"}})
    specs.append({"cid": "space-invaders", "recipe": "objects-vbcc", "root": "truth", "syntax": "vasm", "unit": "*",
                  "units": "per-object:out/*.o", "final": {"kind": "object-only"}})
    return specs


EXCLUDED_UNITS = {"HelloAmi": "HelloAmi.adf/BootWB1x.adf: disk-image containers that embed the other units (duplicated content)"}


# ---------------------------------------------------------------------------------------------------------------- small helpers
def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def masked_equal(a: bytes, b: bytes, mask: bytearray) -> bool:
    """a == b at every position whose mask byte is 0 (pure; unit-tested)."""
    if len(a) != len(b):
        return False
    return all(x == y or m for x, y, m in zip(a, b, mask))


def reloc_mask(size: int, relocs) -> bytearray:
    m = bytearray(size)
    for off, w in relocs:
        for k in range(off, min(size, off + w)):
            m[k] = 1
    return m


# ------------------------------------------------------------------------------------------------------------- listing -> labels
class Typed:
    """Typed per-row/per-section findings (never silently dropped)."""

    def __init__(self):
        self.counts: dict[str, int] = {}
        self.spans: list[dict] = []

    def add(self, kind: str, n: int = 1, span: dict | None = None):
        self.counts[kind] = self.counts.get(kind, 0) + n
        if span is not None:
            if len(self.spans) < SPAN_CAP:
                self.spans.append({"kind": kind, **span})
            else:
                self.counts["SPANS_TRUNCATED"] = self.counts.get("SPANS_TRUNCATED", 0) + 1


def section_labels(rows: list, data: bytes, mask: bytearray, syntax: str, typed: Typed, section: str) -> tuple[bytearray, list[dict]] | None:
    """Per-byte labels of one assembled section from its listing rows (before decoding). Returns (classes, row records) or None when
    the listing does not reproduce the section (then the whole section stays unknown). Row record: offset, extent, kind, mnemonic."""
    emit = []
    for r in rows:
        mnem, _ops = fm.statement(r.text, syntax)
        kind = fm.row_kind(mnem, syntax)
        if syntax == "gas" and kind == "other" and r.shown and not any(r.shown) and mnem.startswith("."):
            # zero fill listed on a section-changing / allocation directive: the outgoing section's even-size padding (m68k GNU as)
            typed.add("SECTION_PADDING", len(r.shown))
            kind = "align"
        if syntax == "gas" and ((kind == "other" and r.shown) or (kind == "insn" and not r.shown)
                                or (kind == "data" and not r.shown and _ops.strip() and not _zero_size_data(mnem, _ops))):
            # GNU as listed bytes on a statement that cannot emit them, or none on an instruction: its byte-to-line attribution is
            # out of step here (seen in inline asm / hand-written .S), so no row of this section can be trusted
            typed.add("LISTING_BYTE_LAG", span={"section": section, "offset": r.offset})
            return None
        if r.offset < 0:
            continue  # offset-less GNU as statement: only used by the lag check above
        if r.shown or kind in ("data", "align"):
            emit.append((r.offset, r, kind, mnem))
    emit.sort(key=lambda x: (x[0], 0 if x[1].shown else 1))
    cls = bytearray(b"U" * len(data))
    recs = []
    for i, (off, r, kind, mnem) in enumerate(emit):
        nxt = len(data)
        for j in range(i + 1, len(emit)):
            if emit[j][0] > off:
                nxt = emit[j][0]
                break
            if emit[j][0] == off and emit[j][1].shown and r.shown:
                typed.add("OVERLAPPING_ROWS", span={"section": section, "offset": off})
                return None
        extent = nxt - off
        if extent <= 0:
            continue
        if off + extent > len(data) or len(r.shown) > extent:
            typed.add("ROW_OUTSIDE_SECTION", span={"section": section, "offset": off})
            return None
        if r.shown and not masked_equal(r.shown, data[off:off + len(r.shown)], mask[off:off + len(r.shown)]):
            typed.add("LISTING_OBJECT_DISAGREE", span={"section": section, "offset": off})
            return None
        if kind == "insn" and not r.truncated and len(r.shown) < extent:
            # the listing proves exactly the shown instruction bytes; the rest up to the next listed row (an unlisted alignment, e.g.
            # vasm's `cnop`) is unexplained
            typed.add("UNLISTED_GAP", extent - len(r.shown))
            extent = len(r.shown)
        if kind == "insn":
            cls[off] = ord("I")
            for k in range(off + 1, off + extent):
                cls[k] = ord("c")
        elif kind == "data":
            for k in range(off, off + extent):
                cls[k] = ord("D")
        else:
            typed.add("ALIGN_OR_OTHER_ROW" if kind == "align" else "OTHER_EMITTING_ROW", extent)
        recs.append({"offset": off, "extent": extent, "kind": kind, "mnemonic": mnem.lower()})
    return cls, recs


def encoded_non_base_opcode(word: int) -> bool:
    """First word of a 68010+/coprocessor instruction (pure; unit-tested): MOVEC 4E7A/4E7B, MOVES 0Exx, RTD 4E74, BKPT 4848-484F,
    line-F with coprocessor id 0-3 (F000-F7FF: 68030 MMU PMOVE, 68040 CINV/CPUSH, FPU). Plain 0xF8xx-0xFFxx words (common table values
    such as -1) are not claimed."""
    return word in (0x4E7A, 0x4E7B, 0x4E74) or 0x0E00 <= word <= 0x0EFF or 0x4848 <= word <= 0x484F or 0xF000 <= word <= 0xF7FF


def apply_decoder(classes: bytearray, rows: list[dict], tokens: bytes, typed: Typed, base: int, size: int, image: bytes = b"") -> None:
    """Decoder cross-check of the placed I rows and the fall-through rule (mutates classes; positions are image offsets = base + row
    offset). A failing instruction row becomes U with a typed reason."""
    prev_insn_terminates = None
    prev_end = None
    executable = any(r["kind"] == "insn" for r in rows)

    def image_word(pos: int):
        return int.from_bytes(image[pos:pos + 2], "big") if pos + 2 <= len(image) else None
    for r in sorted(rows, key=lambda x: x["offset"]):
        pos = base + r["offset"]
        if r["kind"] == "insn" and classes[pos] == ord("I"):
            reason = None
            if pos & 1:
                reason = "ODD_START"
            else:
                w = pos // 2
                status, length = tokens[w * TOKEN_FIELDS], tokens[w * TOKEN_FIELDS + 1]
                control = tokens[w * TOKEN_FIELDS + 3]
                if status == 0:
                    reason = "DECODER_REJECTED"
                elif status == 1 and r["mnemonic"] != "illegal":
                    reason = "EXCEPTION_FORM"
                elif length * 2 != r["extent"]:
                    reason = "LENGTH_MISMATCH"
            if reason:
                typed.add(reason, span={"offset": pos, "extent": r["extent"], "mnemonic": r["mnemonic"]})
                for k in range(pos, pos + r["extent"]):
                    classes[k] = ord("U")
                prev_insn_terminates, prev_end = None, None
                continue
            typed.add("BASE_68000_INSTRUCTION")
            prev_insn_terminates = control in TERMINATING_CONTROL
            prev_end = pos + r["extent"]
            continue
        if r["kind"] == "data" and prev_end == pos and prev_insn_terminates is False and classes[pos] == ord("D"):
            # execution may fall into this data (inline arguments, or instruction words encoded with dc.w/dc.l such as EmuTOS's
            # MOVEC/PMOVE macros): unknown, and so is every contiguous data row after it until the next instruction row
            typed.add("DATA_AFTER_FALLTHROUGH", r["extent"], span={"offset": pos, "extent": r["extent"]})
            w = pos // 2
            if not pos & 1 and tokens[w * TOKEN_FIELDS] != 2:
                typed.add("ENCODED_NON_BASE_WORD_AFTER_FALLTHROUGH")
            for k in range(pos, pos + r["extent"]):
                classes[k] = ord("U")
            prev_end = pos + r["extent"]  # prev_insn_terminates stays False: the fall-through region continues
            continue
        if r["kind"] == "data" and executable and classes[pos] == ord("D") and not pos & 1 and tokens[(pos // 2) * TOKEN_FIELDS] != 2:
            word = image_word(pos)
            if word is not None and encoded_non_base_opcode(word):
                # a data row in an instruction-bearing section that starts with a 68010+/coprocessor opcode word (MOVEC/MOVES/RTD/
                # BKPT/line-F cp 0-3): treated as a possibly executed encoded instruction -> unknown, never base-68000 data
                typed.add("D_ROW_ENCODED_NON_BASE_OPCODE", span={"offset": pos, "extent": r["extent"]})
                for k in range(pos, pos + r["extent"]):
                    classes[k] = ord("U")
            else:
                # residual diagnostic (upper bound): other data whose first word is not an ordinary base decode; label kept
                typed.add("D_ROW_NON_BASE_WORD_IN_CODE_SECTION")
        prev_insn_terminates, prev_end = None, None


# ------------------------------------------------------------------------------------------------------------------ placement
def place(section: bytes, mask: bytearray, image: bytes, alignment: int = 2) -> list[int]:
    """Every aligned position where `section` matches `image` outside its relocation fields (pure; unit-tested)."""
    n = len(section)
    if n == 0 or n > len(image):
        return []
    best, cur_start, run = (0, 0), None, 0
    for i in range(n + 1):
        if i < n and not mask[i]:
            if cur_start is None:
                cur_start = i
            run = i - cur_start + 1
            if run > best[1]:
                best = (cur_start, run)
        else:
            cur_start = None
    a_off, a_len = best
    a_len = min(a_len, 64)
    if a_len < 4:
        return []
    anchor = section[a_off:a_off + a_len]
    out = []
    start = image.find(anchor)
    while start >= 0:
        base = start - a_off
        if base >= 0 and base % alignment == 0 and base + n <= len(image) and masked_equal(section, image[base:base + n], mask):
            out.append(base)
        start = image.find(anchor, start + 1)
    return out


# ------------------------------------------------------------------------------------------------------------------- objects
def object_sections(path: pathlib.Path) -> tuple[str, list[dict]]:
    """Assembled sections of one object: [{name, data, relocs}] in the order the assembler listed them."""
    data = path.read_bytes()
    if data[:4] == b"\x7fELF":
        e = fm.parse_elf32(data)
        secs = [{"name": s["name"], "data": s["data"], "relocs": s["relocs"], "size": s["size"]} for s in e["sections"]
                if s["type"] == 1 and s["flags"] & 2]  # PROGBITS + ALLOC
        return "elf", secs
    if data[:4] == b"\x00\x00\x03\xe7":
        h = fm.parse_hunks(data)
        return "hunk", [{"name": str(i), "data": hk["data"], "relocs": hk["relocs"], "size": hk["size"], "type": hk["type"]}
                        for i, hk in enumerate(h["hunks"])]
    a = fm.parse_aout(data)
    return "aout", [{"name": "text", "data": a["text"], "relocs": a["text_relocs"], "size": len(a["text"])},
                    {"name": "data", "data": a["data"], "relocs": a["data_relocs"], "size": len(a["data"])}]


def listing_for(path: pathlib.Path, syntax: str) -> tuple[list[str], list]:
    text = path.read_text(encoding="latin-1")
    if syntax == "gas":
        return fm.parse_gas_listing(text)
    if syntax == "a68k":
        return fm.parse_a68k_listing(text)
    return fm.parse_vasm_listing(text)


def rows_by_section(names: list[str], rows: list, syntax: str, obj_kind: str, secs: list[dict]) -> dict[int, list]:
    """Associate listing sections with assembled sections. vasm numbers its sections in output order; A68K emits one hunk per distinct
    SECTION name in first-appearance order; GNU as names them (an ELF object lists PROGBITS sections by name)."""
    out: dict[int, list] = {}
    if obj_kind == "aout" and syntax == "gas":
        for r in rows:  # GNU as a.out (MiNT): .text / .data segments only
            name = r.section.split("#")[0]
            k = {".text": 0, ".data": 1}.get(name, -1)
            out.setdefault(k, []).append(r)
        return out
    if syntax == "gas":
        idx = {s["name"]: i for i, s in enumerate(secs)}
        for r in rows:
            name = r.section.split("#")[0]
            if name in idx:
                out.setdefault(idx[name], []).append(r)
            elif r.shown:
                out.setdefault(-1, []).append(r)  # bytes in a non-ALLOC/unknown section: never labelled
        return out
    if syntax == "a68k":
        order = {n: i for i, n in enumerate(names)}
        for r in rows:
            out.setdefault(order[r.section], []).append(r)
        return out
    if obj_kind == "aout":
        for r in rows:  # vasm -Faout: section names of the a.out segments
            nm = names[int(r.section)] if int(r.section) < len(names) else ""
            out.setdefault(0 if nm.lower() in ("text", "code", "") else 1, []).append(r)
        return out
    for r in rows:
        out.setdefault(int(r.section), []).append(r)
    return out


# ----------------------------------------------------------------------------------------------------------------- final images
def final_images(spec: dict, root: pathlib.Path) -> list[dict]:
    """Loadable images of the program: [{name, image, relocs, entry (image offset or None), hunk index}]."""
    f = spec["final"]
    if f["kind"] == "hunk-exe":
        h = fm.parse_hunks((root / f["path"]).read_bytes())
        out = []
        code_seen = False
        for i, hk in enumerate(h["hunks"]):
            if hk["type"] == "BSS" or not hk["data"]:
                continue
            entry = 0 if (hk["type"] == "CODE" and not code_seen and i == 0) else None
            code_seen = code_seen or hk["type"] == "CODE"
            out.append({"name": f"hunk{i}", "image": hk["data"], "relocs": hk["relocs"], "entry": entry, "hunk": i, "hunk_type": hk["type"]})
        return out
    if f["kind"] == "prg":
        p = fm.parse_prg((root / f["path"]).read_bytes())
        return [{"name": "text+data", "image": p["image"], "relocs": p["relocs"], "entry": 0, "text": p["text"]}]
    if f["kind"] == "raw-rom":
        img = (root / f["path"]).read_bytes()
        return [{"name": "rom", "image": img, "relocs": [], "entry": None}]
    if f["kind"] == "elf-exe":
        e = fm.parse_elf32((root / f["path"]).read_bytes())
        loads = sorted((s for s in e["sections"] if s["type"] == 1 and s["flags"] & 2 and s["size"]), key=lambda s: s["addr"])
        lo = loads[0]["addr"]
        hi = max(s["addr"] + s["size"] for s in loads)
        if hi - lo > 16 << 20:
            raise fm.FormatError("ELF load image unreasonably large")
        img = bytearray(hi - lo)
        for s in loads:
            img[s["addr"] - lo:s["addr"] - lo + s["size"]] = s["data"]
        entry = e["entry"] - lo if lo <= e["entry"] < hi else None
        return [{"name": "load", "image": bytes(img), "relocs": [], "entry": entry, "base_address": lo}]
    raise fm.FormatError(f"unknown final kind {f['kind']}")


def map_text_base(map_text: str) -> int:
    """Load address of the output .text section from a GNU ld map (pure; unit-tested)."""
    for ln in map_text.splitlines():
        m = re.match(r"^\.text\s+0x([0-9a-fA-F]+)\s+0x[0-9a-fA-F]+", ln)
        if m:
            return int(m.group(1), 16)
    raise fm.FormatError("link map has no output .text section")


def _zero_size_data(mnem: str, ops: str) -> bool:
    """A data directive that legitimately emits nothing (e.g. `.zero 0`, `.skip 0`, `.ascii ""`)."""
    o = ops.strip()
    return o in ("0", '""') or (mnem.lower() in (".zero", ".skip", ".space") and o.split(",")[0].strip() == "0")


def rom_entry(img: bytes, rom_base: int) -> int | None:
    """Reset PC of an EmuTOS ROM image (vector at offset 4) as an image offset, when it points into the image."""
    if len(img) < 8:
        return None
    pc = int.from_bytes(img[4:8], "big")
    off = pc - rom_base
    return off if 0 <= off < len(img) else None


# ------------------------------------------------------------------------------------------------------------------------ truth
def cmd_truth(a) -> int:
    sys.path.insert(0, str(HERE))
    import segarecomp_xplat_qualify as q
    plan = q.load_plan()
    cache = pathlib.Path(a.cache).resolve()
    out_root = pathlib.Path(a.out).resolve()
    q.require_private(out_root)
    results = {}
    for (cid, rname), t in sorted(TRUTH.items()):
        if a.only and cid not in a.only:
            continue
        cand = next(c for c in plan["candidates"] if c["id"] == cid)
        recipe = next(r for r in cand["recipes"] if r["name"] == rname)
        q.observed_source(cache / "src" / cid, cand["commit"])
        work = out_root / cid / rname
        if work.exists():
            shutil.rmtree(work)
        shutil.copytree(cache / "src" / cid, work, symlinks=True, ignore=shutil.ignore_patterns(".git"))
        wrapper = out_root / "_wrap.sh"
        wrapper.write_text(GAS_WRAP if t["wrap"] == "gas" else VASM_WRAP)
        shutil.copyfile(RELIST_WRAP, out_root / "_relist_wrap.py")
        prologue = f"bash /wrap/_wrap.sh {' '.join(t.get('tools', []))}"
        script = recipe["script"]
        if cand.get("shim") == "ci_alias":
            script = f"python3 /xplat/ci_alias.py . && {{ {script}; }}"
        script = f"{prologue} && export PATH=/xplat/bin:$PATH XPLAT_DATE='{q.RUN_DATES['a']}'; {script}"
        mount = cand.get("mount", "/w")
        cmd = ["docker", "run", "--rm", "--network", "none", "-v", f"{work}:{mount}", "-w", mount, "-v", f"{cache / 'xplat'}:/xplat:ro",
               "-v", f"{out_root}:/wrap:ro", "--entrypoint", "/bin/bash", q.image(recipe["toolchain"]), "-c", script]
        proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=3600)
        (out_root / cid / f"{rname}.log").write_text(proc.stdout + proc.stderr)
        ref = cache / "build" / cid / rname / "a"
        same = diff = missing = listed = 0
        diffs = []
        for p in sorted(ref.rglob("*")):
            if not p.is_file() or p.is_symlink():
                continue
            data = p.read_bytes()
            fmt = q.probe_format(data)["format"]
            is_obj = fmt in ("elf-m68k-relocatable", "elf-m68k-executable", "amiga-hunk-object", "amiga-hunk-exe", "tos-prg", "aout") or \
                (p.suffix == ".o" and fmt == "raw" and data[:2] == b"\x00\x00")
            if fmt == "elf-other" or not (is_obj or p.suffix in (".img", ".rom")):
                continue
            mine = work / p.relative_to(ref)
            if not mine.is_file():
                missing += 1
                diffs.append(str(p.relative_to(ref)))
                continue
            if mine.read_bytes() == data:
                same += 1
            else:
                diff += 1
                diffs.append(str(p.relative_to(ref)))
            if (mine.parent / (mine.name + ".lst")).is_file():
                listed += 1
        ok = proc.returncode == 0 and diff == 0 and missing == 0 and same > 0
        results[f"{cid}/{rname}"] = {"rc": proc.returncode, "identical": same, "different": diff, "missing": missing, "with_listing": listed,
                                     "accepted": ok}
        print(json.dumps({f"{cid}/{rname}": results[f"{cid}/{rname}"], "diffs": diffs[:5]}), flush=True)
    (out_root / "truth.json").write_text(json.dumps(results, indent=1, sort_keys=True) + "\n")
    return 0 if all(r["accepted"] for r in results.values()) else 1


# ---------------------------------------------------------------------------------------------------------------------- extract
def tokens_for(cli: str, image: bytes, scratch: pathlib.Path) -> bytes:
    import segarecomp_ml_region_gen2 as gen2
    img = scratch / "img.bin"
    rep = scratch / "tok.report"
    img.write_bytes(image if len(image) % 2 == 0 else image + b"\0")
    subprocess.run([cli, "m68k-decode-token-report", "--rom", str(img), "--output", str(rep)], check=True, capture_output=True, timeout=600)
    padded = len(image) + (len(image) % 2)
    body = gen2.parse_token_report(rep.read_bytes(), padded)
    rep.unlink()
    img.unlink()
    # the SEG-072 representation has one token row per whole word of the image (an odd trailing byte has none)
    return body[:(len(image) // 2) * TOKEN_FIELDS]


def extract_program(spec: dict, cache: pathlib.Path, truth_root: pathlib.Path, cli: str, out: pathlib.Path, scratch: pathlib.Path) -> list[dict]:
    root = (cache / "build" / spec["cid"] / spec["recipe"] / "a") if spec["root"] == "t001" else (truth_root / spec["cid"] / spec["recipe"])
    samples = []
    if spec["final"]["kind"] == "object-only":
        for obj in sorted(root.glob(spec["units"].split(":", 1)[1])):
            lst = obj.parent / (obj.name + ".lst")
            if not lst.is_file():
                lst = obj.parent / (obj.name.removesuffix("_s.o") + ".lst")
            sub = dict(spec, unit=str(obj.relative_to(root)), units=[{"listing": str(lst.relative_to(root)), "object": str(obj.relative_to(root))}],
                       final={"kind": "object-text", "path": str(obj.relative_to(root))})
            samples += _extract(sub, root, cli, out, scratch)
        return samples
    return _extract(spec, root, cli, out, scratch)


def _units(spec: dict, root: pathlib.Path) -> list[dict]:
    if isinstance(spec["units"], list):
        return spec["units"]
    pat = spec["units"].split(":", 1)[1]
    out = []
    for obj in sorted(root.glob(pat)):
        if obj.name.endswith(".relist.o"):
            continue  # the truth run's re-assembly copy, not a build object
        data = obj.read_bytes()
        if data[:4] == b"\x7fELF" and data[18:20] != b"\x00\x04":
            continue  # host objects (toslink) are not M68K
        listing = str(obj.relative_to(root)) + ".lst"
        relist = obj.parent / (obj.name + ".relist.o")
        if relist.is_file() and relist.read_bytes() == data and (obj.parent / (obj.name + ".relist.lst")).is_file():
            # the scrubbed re-assembly produced a byte-identical object: its listing has consistent byte-to-line attribution
            listing = str(obj.relative_to(root)) + ".relist.lst"
        out.append({"listing": listing, "object": str(obj.relative_to(root))})
    return out


def _extract(spec: dict, root: pathlib.Path, cli: str, out: pathlib.Path, scratch: pathlib.Path) -> list[dict]:
    typed = Typed()
    if spec["final"]["kind"] == "object-text":
        o = fm.parse_aout((root / spec["final"]["path"]).read_bytes())
        images = [{"name": "text", "image": o["text"], "relocs": o["text_relocs"], "entry": None}]
    else:
        images = final_images(spec, root)
    if spec["final"]["kind"] == "raw-rom":
        # the ROM's load address is the link map's .text address (the build's own record); the reset vector at image offset 4 is the
        # entry root only when it points into the image (a cartridge image has no reset vector)
        base = map_text_base((root / "emutos.map").read_text(encoding="latin-1"))
        images[0]["entry"] = rom_entry(images[0]["image"], base) if spec["recipe"] != "cart" else None
        images[0]["base_address"] = base
    cls = [bytearray(b"U" * len(im["image"])) for im in images]
    placed_rows: list[list[tuple[int, list]]] = [[] for _ in images]
    stats = {"sections": 0, "placed": 0, "ambiguous": 0, "unplaced": 0, "too_short": 0, "rejected_listing": 0, "overlap": 0,
             "missing_listing": 0, "units": 0}
    claimed = [bytearray(len(im["image"])) for im in images]
    for u in _units(spec, root):
        stats["units"] += 1
        lpath = root / u["listing"]
        if not lpath.is_file():
            stats["missing_listing"] += 1
            typed.add("MISSING_LISTING")
            continue
        names, rows = listing_for(lpath, spec["syntax"])
        if u["object"]:
            okind, secs = object_sections(root / u["object"])
            if spec["final"]["kind"] == "object-text":
                secs = [s if i == 0 else dict(s, data=b"") for i, s in enumerate(secs)]  # object-only programs: the text segment
        else:  # vasm wrote the executable directly: listing section i IS hunk i
            # listing section i IS hunk i (vasm numbers sections in hunk order); map by hunk index, never by the position in the
            # loadable-image list (BSS/empty hunks are not images)
            by_hunk = {im["hunk"]: k for k, im in enumerate(images)}
            nh = max(by_hunk) + 1 if by_hunk else 0
            okind, secs = "direct", [({"name": images[by_hunk[h]]["name"], "data": images[by_hunk[h]]["image"], "relocs": images[by_hunk[h]]["relocs"],
                                       "size": len(images[by_hunk[h]]["image"]), "direct": by_hunk[h]} if h in by_hunk
                                      else {"name": f"hunk{h}", "data": b"", "relocs": [], "size": 0}) for h in range(nh)]
        by_sec = rows_by_section(names, rows, spec["syntax"], okind, secs)
        for si, srows in sorted(by_sec.items()):
            if si < 0 or si >= len(secs):
                if any(r.shown for r in srows):
                    typed.add("ROWS_IN_UNLOADED_SECTION")
                continue
            sec = secs[si]
            data = sec["data"]
            if not data:
                continue
            stats["sections"] += 1
            mask = reloc_mask(len(data), sec["relocs"])
            lab = section_labels(srows, data, mask, spec["syntax"], typed, f"{u['listing']}#{sec['name']}")
            if lab is None:
                stats["rejected_listing"] += 1
                continue
            labels, recs = lab
            if "direct" in sec:
                targets = [(sec["direct"], 0)]
            else:
                if sum(1 for m in mask if not m) < MIN_PLACE_BYTES:
                    stats["too_short"] += 1
                    typed.add("SECTION_TOO_SHORT_TO_PLACE")
                    continue
                targets = [(k, b) for k, im in enumerate(images) for b in place(data, mask, im["image"])]
            if len(targets) != 1:
                stats["ambiguous" if targets else "unplaced"] += 1
                typed.add("PLACEMENT_AMBIGUOUS" if targets else "SECTION_NOT_IN_IMAGE")
                continue
            k, base = targets[0]
            if any(claimed[k][base:base + len(data)]):
                stats["overlap"] += 1
                typed.add("PLACEMENT_OVERLAP")
                continue
            for i in range(base, base + len(data)):
                claimed[k][i] = 1
            cls[k][base:base + len(data)] = labels
            placed_rows[k].append((base, recs))
            stats["placed"] += 1
    samples = []
    last = max((k for k, im in enumerate(images) if im["image"]), default=-1)
    for k, im in enumerate(images):
        if not im["image"]:
            typed.add("EMPTY_IMAGE_SKIPPED")
            continue
        tokens = tokens_for(cli, im["image"], scratch)
        for base, recs in placed_rows[k]:
            apply_decoder(cls[k], [dict(r, offset=r["offset"]) for r in recs], tokens, typed, base, len(im["image"]), im["image"])
        sid = f"xp/{spec['cid']}/{spec['recipe']}/{spec['unit']}#{im['name']}"
        samples.append(write_sample(out, sid, spec, im, bytes(cls[k]), tokens, typed if k == last else None, stats))
    return samples


def write_sample(out: pathlib.Path, sid: str, spec: dict, im: dict, classes: bytes, tokens: bytes, typed: Typed | None, stats: dict) -> dict:
    import segarecomp_ml_v3_data as v3d
    d = out / sid.replace("#", "@")
    d.mkdir(parents=True, exist_ok=True)
    image = im["image"]
    (d / "image.bin").write_bytes(image)
    (d / "classes.bin").write_bytes(classes)
    (d / "tokens.bin").write_bytes(tokens)
    # an entry root is a seed only when the listing labels an instruction start there (a CODE hunk of icon data is not executable)
    entry = im.get("entry")
    seeds = sorted({entry // CELL_BYTES}) if entry is not None and 0 <= entry < len(classes) and classes[entry] == ord("I") else []
    (d / "seeds.json").write_text(json.dumps(seeds))
    (d / "relocs.json").write_text(json.dumps([list(r) for r in im["relocs"]]))
    au = v3d.audit(classes, tokens)
    counts = {c: classes.count(c.encode()) for c in "IcDU"}
    meta = {"schema": SCHEMA, "id": sid, "kind": "xplat_image", "lineage": spec["cid"], "candidate": spec["cid"], "recipe": spec["recipe"],
            "unit": spec["unit"], "section": im["name"], "platform": PLATFORM[spec["cid"]], "toolchain": TOOLCHAIN[spec["cid"]],
            "workload": WORKLOAD[spec["cid"]], "cpu_variant": "MC68000",
            "truth": {"t001": "listing of the qualified run a", "truth": "listing-instrumented truth run, objects byte-identical to run a"}[spec["root"]],
            "syntax": spec["syntax"], "image": str((d / "image.bin").resolve()), "sha256": sha256(image), "size": len(image),
            "classes_sha256": sha256(classes), "tokens_sha256": sha256(tokens), "seeds_sha256": sha256(json.dumps(seeds).encode()),
            "seed_cells": len(seeds), "seeds_source": "container entry, only where the listing labels an instruction start",
            "relocations": len(im["relocs"]), "audit": au, "bytes": counts, "base_address": im.get("base_address"),
            "hunk_type": im.get("hunk_type"), "provenance": "authored"}
    if typed is not None:
        meta["typed"] = dict(sorted(typed.counts.items()))
        meta["placement"] = stats
        (d / "spans.json").write_text(json.dumps(typed.spans, sort_keys=True))
    (d / "meta.json").write_text(json.dumps(meta, indent=1, sort_keys=True))
    return meta


def cmd_extract(a) -> int:
    cache = pathlib.Path(a.cache).resolve()
    out = pathlib.Path(a.out).resolve()
    sys.path.insert(0, str(HERE))
    import segarecomp_xplat_qualify as q
    q.require_private(out)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    scratch = out.parent / (out.name + ".scratch")
    scratch.mkdir(exist_ok=True)
    truth = pathlib.Path(a.truth).resolve()
    tj = json.loads((truth / "truth.json").read_text()) if (truth / "truth.json").is_file() else {}
    all_meta = []
    for spec in program_specs():
        if a.only and spec["cid"] not in a.only:
            continue
        if spec["root"] == "truth" and not tj.get(f"{spec['cid']}/{spec['recipe']}", {}).get("accepted"):
            print(json.dumps({"program": f"{spec['cid']}/{spec['recipe']}/{spec['unit']}", "error": "truth run not accepted"}))
            continue
        metas = extract_program(spec, cache, truth, a.cli, out, scratch)
        all_meta += metas
        for m in metas:
            print(json.dumps({"id": m["id"], "size": m["size"], "bytes": m["bytes"], "audit": m["audit"], "typed": m.get("typed", {}),
                              "placement": m.get("placement")}), flush=True)
    shutil.rmtree(scratch, ignore_errors=True)
    manifest = {"schema": "segarecomp.xplat_corpus.manifest.v2", "samples": [{k: m[k] for k in ("id", "sha256", "classes_sha256", "tokens_sha256",
                                                                                              "seeds_sha256", "size", "base_address")}
                                                                             for m in sorted(all_meta, key=lambda m: m["id"])]}
    data = (json.dumps(manifest, indent=1, sort_keys=True) + "\n").encode()
    (out / "MANIFEST.json").write_bytes(data)
    print(json.dumps({"samples": len(all_meta), "manifest_sha256": sha256(data)}))
    return 0


# --------------------------------------------------------------------------------------------------------------------- coverage
def load_metas(root: pathlib.Path) -> list[dict]:
    return [json.loads(p.read_text()) for p in sorted(root.rglob("meta.json"))]


def coverage(metas: list[dict]) -> dict:
    """Aggregate, non-reconstructable coverage per program and platform (pure; unit-tested)."""
    progs: dict[str, dict] = {}
    for m in metas:
        key = m["candidate"]
        p = progs.setdefault(key, {"platform": m["platform"], "toolchain": m["toolchain"], "workload": m["workload"], "samples": 0, "bytes": 0,
                                   "I": 0, "c": 0, "D": 0, "U": 0, "typed": {}, "placement": {}, "decode_checked": 0, "decode_ok": 0})
        p["samples"] += 1
        p["bytes"] += m["size"]
        for c in "IcDU":
            p[c] += m["bytes"][c]
        for k, v in (m.get("typed") or {}).items():
            p["typed"][k] = p["typed"].get(k, 0) + v
        for k, v in (m.get("placement") or {}).items():
            p["placement"][k] = p["placement"].get(k, 0) + v
    for p in progs.values():
        t = p["typed"]
        bad = sum(t.get(k, 0) for k in ("DECODER_REJECTED", "EXCEPTION_FORM", "LENGTH_MISMATCH", "ODD_START"))
        p["decode_checked"] = t.get("BASE_68000_INSTRUCTION", 0) + bad
        p["decode_ok"] = t.get("BASE_68000_INSTRUCTION", 0)
        p["decode_agreement"] = round(p["decode_ok"] / p["decode_checked"], 6) if p["decode_checked"] else None
        p["fractions"] = {c: round(p[c] / p["bytes"], 4) if p["bytes"] else 0.0 for c in "IcDU"}
        p["starts"] = p.pop("I")
        p["typed"] = dict(sorted(t.items()))
    plat: dict[str, dict] = {}
    for k, p in progs.items():
        q = plat.setdefault(p["platform"], {"programs": 0, "bytes": 0, "starts": 0})
        q["programs"] += 1
        q["bytes"] += p["bytes"]
        q["starts"] += p["starts"]
    return {"programs": dict(sorted(progs.items())), "platforms": dict(sorted(plat.items()))}


def cmd_coverage(a) -> int:
    root = pathlib.Path(a.corpus).resolve()
    metas = load_metas(root)
    manifest = (root / "MANIFEST.json").read_bytes()
    doc = {"schema": "segarecomp.xplat_corpus.coverage.v1", "task": "SEG-075-T002", "adr": "0160", "corpus_manifest_sha256": sha256(manifest),
           "samples": len(metas), "excluded_units": EXCLUDED_UNITS, **coverage(metas)}
    target = pathlib.Path(a.out) if a.out else COVERAGE
    target.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"samples": len(metas), "programs": {k: {x: v[x] for x in ("starts", "fractions", "decode_agreement")}
                                                          for k, v in doc["programs"].items()}}, indent=1))
    return 0


def cmd_load(a) -> int:
    """Load every sample with the UNCHANGED SEG-072 loader (needs numpy: run with the research interpreter)."""
    import segarecomp_ml_v3_train as tr
    root = pathlib.Path(a.corpus).resolve()
    n = cells = pos = 0
    for meta in sorted(root.rglob("meta.json")):
        im = tr.Image(meta.parent)
        n += 1
        cells += im.n_cells
        pos += int((im.labels == 1).sum())
        if im.size != json.loads(meta.read_text())["size"] or len(im.classes) != im.size:
            raise SystemExit(f"{meta.parent}: loader disagrees")
    print(json.dumps({"loaded": n, "cells": cells, "positive_cells": pos}))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("truth")
    t.add_argument("--cache", required=True)
    t.add_argument("--out", required=True)
    t.add_argument("--only", action="append")
    t.set_defaults(func=cmd_truth)
    e = sub.add_parser("extract")
    e.add_argument("--cache", required=True)
    e.add_argument("--truth", required=True)
    e.add_argument("--cli", required=True)
    e.add_argument("--out", required=True)
    e.add_argument("--only", action="append")
    e.set_defaults(func=cmd_extract)
    c = sub.add_parser("coverage")
    c.add_argument("--corpus", required=True)
    c.add_argument("--out")
    c.set_defaults(func=cmd_coverage)
    ld = sub.add_parser("load")
    ld.add_argument("--corpus", required=True)
    ld.set_defaults(func=cmd_load)
    args = ap.parse_args(argv)
    try:
        return args.func(args)
    except fm.FormatError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
