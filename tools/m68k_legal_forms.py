#!/usr/bin/env python3
"""Deterministic derivation of the independent legal base-MC68000 form dataset.

Source of truth: the public Motorola M68000 Family Programmer's Reference Manual
(instruction-set summary, per-instruction encodings and addressing-mode tables),
transcribed here as data tables. This file is TEST-SIDE EXPECTATION ONLY.

Independence rule (enforced by tests/m68k_legal_forms_test.py, both directions):
  * this tool imports only the Python standard library and never reads any
    production legality (libs/cpu/m68k decode predicates, EA masks) or any
    oracle output;
  * no production source imports or reads this tool or its dataset.

Output: tests/fixtures/m68k-legal-forms.json, byte-for-byte reproducible
(sorted keys, no timestamps, fixed row order).
"""
import argparse
import json
import pathlib
import sys

SCHEMA = 1

# --- effective-address classes (PRM addressing-mode names -> mode/register) ---
# name: (mode, fixed_register_or_None). Modes 0-6 take any of 8 registers.
EA_CLASSES = {
    "dn": (0, None), "an": (1, None), "ind": (2, None), "postinc": (3, None),
    "predec": (4, None), "disp": (5, None), "index": (6, None),
    "absw": (7, 0), "absl": (7, 1), "pcdisp": (7, 2), "pcindex": (7, 3), "imm": (7, 4),
}
EA_DESCRIPTIONS = {
    "dn": "Dn", "an": "An", "ind": "(An)", "postinc": "(An)+", "predec": "-(An)",
    "disp": "(d16,An)", "index": "(d8,An,Xn)", "absw": "(xxx).W", "absl": "(xxx).L",
    "pcdisp": "(d16,PC)", "pcindex": "(d8,PC,Xn)", "imm": "#<data>",
}
ALL = ["dn", "an", "ind", "postinc", "predec", "disp", "index", "absw", "absl", "pcdisp", "pcindex", "imm"]
DATA = [c for c in ALL if c != "an"]
ALTERABLE = [c for c in ALL if c not in ("pcdisp", "pcindex", "imm")]
DATA_ALT = [c for c in DATA if c in ALTERABLE]
MEM_ALT = [c for c in DATA_ALT if c != "dn"]
CONTROL = ["ind", "disp", "index", "absw", "absl", "pcdisp", "pcindex"]
CONTROL_ALT = ["ind", "disp", "index", "absw", "absl"]
MEMORY_CLASSES = {"ind", "postinc", "predec", "disp", "index", "absw", "absl", "pcdisp", "pcindex"}

SIZE_CODE = {"b": 0, "w": 1, "l": 2}
CONDITIONS = ["t", "f", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"]

FAMILIES = ("data_movement", "integer_arithmetic", "logical", "shift_and_rotate",
            "bit_manipulation", "binary_coded_decimal", "program_control", "system_control")


def ea_values(cls):
    mode, reg = EA_CLASSES[cls]
    return [mode << 3 | r for r in range(8)] if reg is None else [mode << 3 | reg]


ROWS = []      # each: dict(form fields) + "_words" list
_WORD_OWNER = {}


def form(mnemonic, family, form_id, size, src, dst, words, variant="", privilege="user",
         exceptions=(), implicit=""):
    words = sorted(set(words))
    assert words, (mnemonic, form_id)
    auto = []
    if src == "postinc": auto.append("src_postinc")
    if src == "predec": auto.append("src_predec")
    if dst == "postinc": auto.append("dst_postinc")
    if dst == "predec": auto.append("dst_predec")
    if implicit: auto.append(implicit)
    exc = set(exceptions)
    if privilege == "supervisor": exc.add("privilege_violation_vector_8")
    mem = src in MEMORY_CLASSES or dst in MEMORY_CLASSES
    # LEA computes an address only and MOVEP uses byte accesses: neither can raise an odd-address error.
    if mem and size in ("w", "l") and mnemonic not in ("LEA", "MOVEP"): exc.add("address_error_vector_3")
    if implicit in ("implicit_sp_push", "implicit_sp_pop", "implicit_sp_frame"): exc.add("address_error_vector_3")
    row_id = ".".join(p for p in (mnemonic.lower(), form_id, size or "none", src, dst, variant) if p)
    row = {
        "id": row_id, "mnemonic": mnemonic, "family": family, "form": form_id,
        "size": size or "none", "src": src, "dst": dst, "variant": variant,
        "privilege": privilege, "exceptions": sorted(exc),
        "auto_update": "+".join(auto) if auto else "none", "words": len(words),
    }
    for w in words:
        assert 0 <= w <= 0xFFFF
        if w in _WORD_OWNER:
            raise AssertionError("word %04X owned by both %s and %s" % (w, _WORD_OWNER[w], row_id))
        _WORD_OWNER[w] = row_id
    row["_words"] = words
    ROWS.append(row)


def build_forms():
    R8 = range(8)
    # --- BCD ---
    for mn, base in (("ABCD", 0xC100), ("SBCD", 0x8100)):
        form(mn, "binary_coded_decimal", "dn_dn", "b", "dn", "dn", [base | x << 9 | y for x in R8 for y in R8])
        form(mn, "binary_coded_decimal", "predec_predec", "b", "predec", "predec",
             [base | 8 | x << 9 | y for x in R8 for y in R8])
    for c in DATA_ALT:
        form("NBCD", "binary_coded_decimal", "unary", "b", "none", c, [0x4800 | e for e in ea_values(c)])
    # --- ADD/SUB/AND/OR/CMP/EOR ---
    for mn, base, fam in (("ADD", 0xD000, "integer_arithmetic"), ("SUB", 0x9000, "integer_arithmetic"),
                          ("AND", 0xC000, "logical"), ("OR", 0x8000, "logical"),
                          ("CMP", 0xB000, "integer_arithmetic")):
        for s in "bwl":
            sz = SIZE_CODE[s]
            if mn in ("AND", "OR"):
                srcs = DATA
            elif s == "b":
                srcs = DATA
            else:
                srcs = ALL
            for c in srcs:
                form(mn, fam, "ea_dn", s, c, "dn", [base | d << 9 | sz << 6 | e for d in R8 for e in ea_values(c)])
            if mn != "CMP":
                for c in MEM_ALT:
                    form(mn, fam, "dn_ea", s, "dn", c,
                         [base | 0x100 | d << 9 | sz << 6 | e for d in R8 for e in ea_values(c)])
    for s in "bwl":
        sz = SIZE_CODE[s]
        for c in DATA_ALT:
            form("EOR", "logical", "dn_ea", s, "dn", c,
                 [0xB100 | d << 9 | sz << 6 | e for d in R8 for e in ea_values(c)])
    # --- ADDA/SUBA/CMPA ---
    for mn, base in (("ADDA", 0xD000), ("SUBA", 0x9000), ("CMPA", 0xB000)):
        for s, opm in (("w", 3), ("l", 7)):
            for c in ALL:
                form(mn, "integer_arithmetic", "ea_an", s, c, "an",
                     [base | a << 9 | opm << 6 | e for a in R8 for e in ea_values(c)])
    # --- immediate ---
    for mn, base, fam in (("ORI", 0x0000, "logical"), ("ANDI", 0x0200, "logical"), ("SUBI", 0x0400, "integer_arithmetic"),
                          ("ADDI", 0x0600, "integer_arithmetic"), ("EORI", 0x0A00, "logical"),
                          ("CMPI", 0x0C00, "integer_arithmetic")):
        for s in "bwl":
            for c in DATA_ALT:
                form(mn, fam, "imm_ea", s, "imm", c, [base | SIZE_CODE[s] << 6 | e for e in ea_values(c)])
    for mn, base in (("ORI", 0x0000), ("ANDI", 0x0200), ("EORI", 0x0A00)):
        form(mn, "system_control", "imm_ccr", "b", "imm", "ccr", [base | 0x3C])
        form(mn, "system_control", "imm_sr", "w", "imm", "sr", [base | 0x7C], privilege="supervisor")
    # --- quick ---
    for mn, base in (("ADDQ", 0x5000), ("SUBQ", 0x5100)):
        for s in "bwl":
            for c in ALTERABLE:
                if c == "an" and s == "b":
                    continue
                form(mn, "integer_arithmetic", "quick_ea", s, "quick", c,
                     [base | q << 9 | SIZE_CODE[s] << 6 | e for q in R8 for e in ea_values(c)], variant="quick1to8")
    # --- ADDX/SUBX ---
    for mn, base in (("ADDX", 0xD100), ("SUBX", 0x9100)):
        for s in "bwl":
            form(mn, "integer_arithmetic", "dn_dn", s, "dn", "dn",
                 [base | x << 9 | SIZE_CODE[s] << 6 | y for x in R8 for y in R8])
            form(mn, "integer_arithmetic", "predec_predec", s, "predec", "predec",
                 [base | x << 9 | SIZE_CODE[s] << 6 | 8 | y for x in R8 for y in R8])
    for s in "bwl":
        form("CMPM", "integer_arithmetic", "postinc_postinc", s, "postinc", "postinc",
             [0xB108 | x << 9 | SIZE_CODE[s] << 6 | y for x in R8 for y in R8])
    # --- single-operand ---
    for mn, base, fam in (("NEGX", 0x4000, "integer_arithmetic"), ("CLR", 0x4200, "integer_arithmetic"),
                          ("NEG", 0x4400, "integer_arithmetic"), ("NOT", 0x4600, "logical"),
                          ("TST", 0x4A00, "integer_arithmetic")):
        for s in "bwl":
            for c in DATA_ALT:
                form(mn, fam, "unary", s, "none", c, [base | SIZE_CODE[s] << 6 | e for e in ea_values(c)])
    for c in DATA_ALT:
        form("TAS", "bit_manipulation", "unary", "b", "none", c, [0x4AC0 | e for e in ea_values(c)])
    for s, base in (("w", 0x4880), ("l", 0x48C0)):
        form("EXT", "integer_arithmetic", "dn", s, "none", "dn", [base | d for d in R8])
    # --- multiply/divide/CHK ---
    for mn, base, fam, exc in (("MULU", 0xC0C0, "integer_arithmetic", ()), ("MULS", 0xC1C0, "integer_arithmetic", ()),
                               ("DIVU", 0x80C0, "integer_arithmetic", ("zero_divide_vector_5",)),
                               ("DIVS", 0x81C0, "integer_arithmetic", ("zero_divide_vector_5",))):
        for c in DATA:
            form(mn, fam, "ea_dn", "w", c, "dn", [base | d << 9 | e for d in R8 for e in ea_values(c)], exceptions=exc)
    for c in DATA:
        form("CHK", "system_control", "ea_dn", "w", c, "dn", [0x4180 | d << 9 | e for d in R8 for e in ea_values(c)],
             exceptions=("chk_vector_6",))
    # --- MOVE family ---
    for s, top in (("b", 1), ("w", 3), ("l", 2)):
        for sc in (DATA if s == "b" else ALL):
            for dc in DATA_ALT:
                dm, dr = EA_CLASSES[dc]
                dregs = range(8) if dr is None else [dr]
                words = [top << 12 | r << 9 | dm << 6 | e for r in dregs for e in ea_values(sc)]
                form("MOVE", "data_movement", "ea_ea", s, sc, dc, words)
    for s, top in (("w", 3), ("l", 2)):
        for sc in ALL:
            form("MOVEA", "data_movement", "ea_an", s, sc, "an",
                 [top << 12 | a << 9 | 1 << 6 | e for a in R8 for e in ea_values(sc)])
    form("MOVEQ", "data_movement", "imm8_dn", "l", "imm8", "dn", [0x7000 | d << 9 | v for d in R8 for v in range(256)],
         variant="data8")
    for s, sz in (("w", 0), ("l", 1)):
        for c in CONTROL_ALT + ["predec"]:
            form("MOVEM", "data_movement", "reglist_mem", s, "reglist", c, [0x4880 | sz << 6 | e for e in ea_values(c)])
        for c in CONTROL + ["postinc"]:
            form("MOVEM", "data_movement", "mem_reglist", s, c, "reglist", [0x4C80 | sz << 6 | e for e in ea_values(c)])
    for s, opm_load, opm_store in (("w", 4, 6), ("l", 5, 7)):
        form("MOVEP", "data_movement", "mem_dn", s, "disp", "dn", [0x0108 | d << 9 | opm_load << 6 | a for d in R8 for a in R8])
        form("MOVEP", "data_movement", "dn_mem", s, "dn", "disp", [0x0108 | d << 9 | opm_store << 6 | a for d in R8 for a in R8])
    form("EXG", "data_movement", "dn_dn", "l", "dn", "dn", [0xC140 | x << 9 | y for x in R8 for y in R8])
    form("EXG", "data_movement", "an_an", "l", "an", "an", [0xC148 | x << 9 | y for x in R8 for y in R8])
    form("EXG", "data_movement", "dn_an", "l", "dn", "an", [0xC188 | x << 9 | y for x in R8 for y in R8])
    for c in CONTROL:
        form("LEA", "data_movement", "ea_an", "l", c, "an", [0x41C0 | a << 9 | e for a in R8 for e in ea_values(c)])
        form("PEA", "data_movement", "ea", "l", c, "none", [0x4840 | e for e in ea_values(c)], implicit="implicit_sp_push")
    form("LINK", "data_movement", "an_disp16", "w", "an", "disp16", [0x4E50 | a for a in R8], implicit="implicit_sp_frame")
    form("UNLK", "data_movement", "an", "none", "an", "none", [0x4E58 | a for a in R8], implicit="implicit_sp_frame")
    form("SWAP", "data_movement", "dn", "w", "none", "dn", [0x4840 | d for d in R8])
    # --- status register / privileged ---
    for c in DATA:
        form("MOVE", "system_control", "ea_ccr", "w", c, "ccr", [0x44C0 | e for e in ea_values(c)])
        form("MOVE", "system_control", "ea_sr", "w", c, "sr", [0x46C0 | e for e in ea_values(c)], privilege="supervisor")
    for c in DATA_ALT:
        form("MOVE", "system_control", "sr_ea", "w", "sr", c, [0x40C0 | e for e in ea_values(c)])
    form("MOVE", "system_control", "an_usp", "l", "an", "usp", [0x4E60 | a for a in R8], privilege="supervisor")
    form("MOVE", "system_control", "usp_an", "l", "usp", "an", [0x4E68 | a for a in R8], privilege="supervisor")
    # --- program control ---
    for c in CONTROL:
        form("JMP", "program_control", "ea", "none", c, "none", [0x4EC0 | e for e in ea_values(c)],
             exceptions=("address_error_vector_3",))  # odd jump target faults on the fetch
        form("JSR", "program_control", "ea", "none", c, "none", [0x4E80 | e for e in ea_values(c)], implicit="implicit_sp_push")
    for cond in range(16):
        cn = CONDITIONS[cond]
        if cond == 0:
            mn, opts = "BRA", ""
        elif cond == 1:
            mn, opts = "BSR", ""
        else:
            mn, opts = "Bcc", cn
        imp = "implicit_sp_push" if cond == 1 else ""
        form(mn, "program_control", "disp8", "b", "none", "target", [0x6000 | cond << 8 | d for d in range(1, 256)],
             variant=opts, implicit=imp, exceptions=("address_error_vector_3",))
        form(mn, "program_control", "disp16", "w", "none", "target", [0x6000 | cond << 8], variant=opts,
             implicit=imp, exceptions=("address_error_vector_3",))
        form("DBcc", "program_control", "dn_disp16", "w", "dn", "target", [0x50C8 | cond << 8 | d for d in R8],
             variant=cn, exceptions=("address_error_vector_3",))
        for c in DATA_ALT:
            form("Scc", "program_control", "unary", "b", "none", c, [0x50C0 | cond << 8 | e for e in ea_values(c)], variant=cn)
    form("NOP", "program_control", "none", "none", "none", "none", [0x4E71])
    form("RTS", "program_control", "none", "none", "none", "none", [0x4E75], implicit="implicit_sp_pop")
    form("RTR", "program_control", "none", "none", "none", "none", [0x4E77], implicit="implicit_sp_pop")
    # --- system control ---
    form("RTE", "system_control", "none", "none", "none", "none", [0x4E73], privilege="supervisor", implicit="implicit_sp_pop")
    form("RESET", "system_control", "none", "none", "none", "none", [0x4E70], privilege="supervisor")
    form("STOP", "system_control", "imm16", "none", "imm", "none", [0x4E72], privilege="supervisor")
    form("TRAP", "system_control", "vector", "none", "none", "none", [0x4E40 | v for v in range(16)],
         variant="vector0to15", exceptions=("trap_vector_32_47",), implicit="implicit_sp_push")
    form("TRAPV", "system_control", "none", "none", "none", "none", [0x4E76], exceptions=("trapv_vector_7",))
    form("ILLEGAL", "system_control", "none", "none", "none", "none", [0x4AFC], exceptions=("illegal_vector_4",))
    # --- bit manipulation ---
    for mn, dyn, sta, alt in (("BTST", 0x0100, 0x0800, False), ("BCHG", 0x0140, 0x0840, True),
                              ("BCLR", 0x0180, 0x0880, True), ("BSET", 0x01C0, 0x08C0, True)):
        dyn_dsts = DATA_ALT if alt else DATA
        sta_dsts = DATA_ALT if alt else [c for c in DATA if c != "imm"]
        for c in dyn_dsts:
            form(mn, "bit_manipulation", "dn_ea", "l" if c == "dn" else "b", "dn", c,
                 [dyn | d << 9 | e for d in R8 for e in ea_values(c)])
        for c in sta_dsts:
            form(mn, "bit_manipulation", "imm_ea", "l" if c == "dn" else "b", "imm", c, [sta | e for e in ea_values(c)])
    # --- shifts / rotates ---
    for tname, ttype in (("AS", 0), ("LS", 1), ("ROX", 2), ("RO", 3)):
        for dname, d in (("R", 0), ("L", 1)):
            mn = tname + dname
            for s in "bwl":
                sz = SIZE_CODE[s]
                form(mn, "shift_and_rotate", "imm_dn", s, "count1to8", "dn",
                     [0xE000 | c << 9 | d << 8 | sz << 6 | 0 << 5 | ttype << 3 | r for c in R8 for r in R8], variant="count1to8")
                form(mn, "shift_and_rotate", "dn_dn", s, "dn", "dn",
                     [0xE000 | c << 9 | d << 8 | sz << 6 | 1 << 5 | ttype << 3 | r for c in R8 for r in R8])
            for c in MEM_ALT:
                form(mn, "shift_and_rotate", "mem", "w", "none", c,
                     [0xE0C0 | ttype << 9 | d << 8 | e for e in ea_values(c)])


# --- reserved / illegal partition -------------------------------------------------------
LINE_A = (0xA000, 0xAFFF)
LINE_F = (0xF000, 0xFFFF)
# (mask, value, name): well-known post-68000 (68010/68020+) encodings that a 68000 treats
# as illegal. Provenance: public Motorola M68000 Family PRM/MC68020 encodings. Informational:
# it refines the illegal partition into "post_68000" vs "reserved_unassigned".
POST_68000 = [
    (0xFFF8, 0x4848, "bkpt"), (0xFFC0, 0x42C0, "move_from_ccr"), (0xFFF8, 0x49C0, "extb_l"),
    (0xFFF8, 0x4808, "link_l"), (0xFFFF, 0x4E74, "rtd"), (0xFFFE, 0x4E7A, "movec"),
    (0xFF00, 0x0E00, "moves"), (0xF1C0, 0x4100, "chk_l"), (0xFF80, 0x4C00, "mul_div_l"),
    (0xF0F8, 0x50F8, "trapcc"), (0xF9C0, 0x08C0, "cas"), (0xF9C0, 0x00C0, "chk2_cmp2"),
    (0xF1F0, 0x8140, "pack"), (0xF1F0, 0x8180, "unpk"), (0xF8C0, 0xE8C0, "bitfield"),
    (0xFFF0, 0x06C0, "rtm_callm"),
]

LEGEND = {
    "L": "legal_user", "P": "legal_privileged", "A": "line_a_reserved_exception",
    "F": "line_f_reserved_exception", "X": "illegal_post_68000_encoding", "I": "illegal_reserved_unassigned",
}


def build_partition(words_by_priv):
    cls = []
    for w in range(0x10000):
        if w in words_by_priv:
            cls.append("P" if words_by_priv[w] == "supervisor" else "L")
        elif LINE_A[0] <= w <= LINE_A[1]:
            cls.append("A")
        elif LINE_F[0] <= w <= LINE_F[1]:
            cls.append("F")
        elif any((w & m) == v for m, v, _ in POST_68000):
            cls.append("X")
        else:
            cls.append("I")
    return cls


def derive():
    ROWS.clear()
    _WORD_OWNER.clear()
    build_forms()
    ROWS.sort(key=lambda r: r["id"])
    ids = [r["id"] for r in ROWS]
    assert len(ids) == len(set(ids)), "duplicate form ids"
    priv = {}
    for r in ROWS:
        for w in r["_words"]:
            priv[w] = r["privilege"]
    part = build_partition(priv)
    families = {}
    for r in ROWS:
        f = families.setdefault(r["family"], {"forms": 0, "primary_words": 0, "mnemonics": set()})
        f["forms"] += 1
        f["primary_words"] += r["words"]
        f["mnemonics"].add(r["mnemonic"])
    for f in families.values():
        f["mnemonics"] = sorted(f["mnemonics"])
    counts = {k: part.count(k) for k in sorted(LEGEND)}
    assert sum(counts.values()) == 0x10000
    assert set(families) == set(FAMILIES), families.keys()
    columns = ["id", "mnemonic", "family", "form", "size", "src", "dst", "variant", "privilege",
               "exceptions", "auto_update", "words"]
    data = {
        "schema": SCHEMA,
        "dataset": "m68k-legal-forms",
        "target": "base MC68000 only (no 68010/68020+ forms, no format/vector-offset stack word)",
        "source": "Motorola M68000 Family Programmer's Reference Manual (public): instruction set summary, "
                  "per-instruction encodings and addressing-mode tables",
        "independence": "test-side expectation only; derived without reading segarecomp decoder/EA masks or any oracle; "
                        "never imported by production code",
        "vocabulary": {
            "legal_form": "an architecturally legal base-MC68000 mnemonic/form/size/EA-class combination listed in 'forms'",
            "architecturally_illegal": "a 16-bit primary word (or EA/size combination) with no base-MC68000 meaning: raises "
                                       "illegal-instruction vector 4 (partition classes I and X)",
            "architecturally_reserved_exception": "line-A (vector 10) and line-F (vector 11) opcode lines: defined "
                                                  "unimplemented-instruction exceptions, not legal forms (classes A and F)",
            "post_68000_encoding": "encoding that belongs to a later family (68010/68020+); illegal on a base 68000 (class X)",
            "privileged": "legal form that raises privilege-violation vector 8 in user mode (class P, privilege=supervisor)",
            "unsupported_by_segarecomp": "MEASUREMENT-SIDE status only (assigned by the T002 coverage tool); a legal_form that "
                                         "segarecomp does not yet decode/lower is 'unsupported', never 'illegal'; this dataset "
                                         "never carries that status",
            "variant": "condition (Bcc/DBcc/Scc: expanded one form per condition), quick1to8/count1to8/data8/vector0to15 "
                       "(operand-value variant recorded but not expanded into separate forms)",
            "primary_word_partition": "letters " + ", ".join("%s=%s" % kv for kv in sorted(LEGEND.items())),
        },
        "ea_classes": {k: EA_DESCRIPTIONS[k] for k in ALL},
        "form_columns": columns,
        "forms": [[r[c] for c in columns] for r in ROWS],
        "family_counts": families,
        "totals": {"forms": len(ROWS), "legal_primary_words": len(priv),
                   "partition_word_counts": {LEGEND[k]: v for k, v in counts.items()}},
        "primary_word_partition": {
            "index": "row = opcode word >> 8, column = opcode word & 0xFF",
            "legend": LEGEND,
            "rows": ["".join(part[r * 256:(r + 1) * 256]) for r in range(256)],
        },
    }
    return data


def render(data):
    """Stable text: sorted keys; 'forms' and partition rows one per line."""
    lines = ["{"]
    keys = sorted(data)
    for i, k in enumerate(keys):
        v = data[k]
        comma = "," if i + 1 < len(keys) else ""
        if k == "forms":
            body = ",\n".join("  " + json.dumps(r, separators=(",", ":")) for r in v)
            lines.append(' "forms": [\n%s\n ]%s' % (body, comma))
        elif k == "primary_word_partition":
            rows = ",\n".join('   "%s"' % r for r in v["rows"])
            head = json.dumps({"index": v["index"], "legend": v["legend"]}, sort_keys=True)[1:-1]
            lines.append(' "primary_word_partition": {\n  %s,\n  "rows": [\n%s\n  ]\n }%s' % (head, rows, comma))
        else:
            lines.append(" %s: %s%s" % (json.dumps(k), json.dumps(v, sort_keys=True), comma))
    lines.append("}")
    return "\n".join(lines) + "\n"


def default_output():
    return pathlib.Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "m68k-legal-forms.json"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--output", type=pathlib.Path, default=default_output())
    ap.add_argument("--check", action="store_true", help="fail if the output differs from a fresh derivation")
    args = ap.parse_args(argv)
    text = render(derive())
    if args.check:
        return 0 if args.output.read_bytes() == text.encode("utf-8") else 1
    args.output.write_bytes(text.encode("utf-8"))
    print("%d forms, %d legal primary words" % (len(ROWS), sum(r["words"] for r in ROWS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
