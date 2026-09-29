#!/usr/bin/env python3
"""Deterministic derivation of the independent NMOS Z80 legal-form dataset (SEG-008-T001).

Sources of truth (public; transcribed here as data tables, not read from any code):
  * Zilog Z80 CPU User Manual UM0080 (instruction set, opcode tables, T-state/M-cycle counts);
  * Sean Young, "The Undocumented Z80 Documented" v0.91 (undocumented opcodes, prefix behaviour,
    DDCB/FDCB register copy-back, ED holes, IN (C)/OUT (C),0, SLL);
  * Cristian Dinu, "Decoding Z80 Opcodes" (x/y/z/p/q field decomposition used below).
This file is TEST-SIDE EXPECTATION ONLY.

Independence rule (enforced by tests/z80_legal_forms_test.py, both directions):
  * this tool imports only the Python standard library and never reads any production Z80 decode
    predicate/table or any oracle output;
  * no production source imports or reads this tool or its dataset.

Model:
  * seven finite canonical opcode spaces of 256 bytes each: base, cb, ed, dd, fd, ddcb, fdcb (for
    ddcb/fdcb the byte is the fourth byte of `DD CB d op` / `FD CB d op`);
  * every byte of every space is owned by exactly one form, or by exactly one prefix-behaviour class
    (escape to another space, prefix chain, ignored prefix); nothing is left implicit;
  * DD/FD prefix chains are unbounded byte sequences and are therefore specified parametrically
    (`prefix_rules`), never enumerated.

Output: tests/fixtures/z80-legal-forms.json, byte-for-byte reproducible (sorted keys, no timestamps,
fixed row order). `--check` fails when the committed file differs from a fresh derivation.
"""
import argparse
import json
import pathlib
import sys

SCHEMA = 1

SPACES = ("base", "cb", "ed", "dd", "fd", "ddcb", "fdcb")
SPACE_PREFIX = {"base": "", "cb": "CB", "ed": "ED", "dd": "DD", "fd": "FD", "ddcb": "DD CB d", "fdcb": "FD CB d"}

# Owning family -> the single SEG-008 implementation task that owns every form of that family.
FAMILIES = {
    "data_alu": "SEG-008-T004",
    "control_stack": "SEG-008-T005",
    "cb_bit_prefix": "SEG-008-T006",
    "ed_io_interrupt": "SEG-008-T007",
}

COVERAGE_STAGES = ("decodes", "lowers", "emits", "compiles", "executes", "aot_admitted",
                   "oracle_state", "oracle_memory", "oracle_io", "timing_modeled", "timing_validated")

R = ["b", "c", "d", "e", "h", "l", "(hl)", "a"]
RP = ["bc", "de", "hl", "sp"]
RP2 = ["bc", "de", "hl", "af"]
CC = ["nz", "z", "nc", "c", "po", "pe", "p", "m"]
ALU = ["ADD", "ADC", "SUB", "SBC", "AND", "XOR", "OR", "CP"]
ROT = ["RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"]
ACC = ["RLCA", "RRCA", "RLA", "RRA", "DAA", "CPL", "SCF", "CCF"]


def fields(b):
    x, y, z = b >> 6, (b >> 3) & 7, b & 7
    return x, y, z, y >> 1, y & 1


def op(mnemonic, dst, src, family, t, length, layout, m1=1, documented=True, alias_of="", effects=(),
       note=""):
    """One classification of one concrete byte; bytes with identical classifications become one form."""
    if isinstance(t, int):
        timing = {"class": "fixed", "t_states": t}
    else:
        timing = dict(t)
    return {
        "mnemonic": mnemonic, "dst": dst, "src": src, "family": family, "timing": timing,
        "length": length, "layout": layout, "m1_fetches": m1, "documented": documented,
        "alias_of": alias_of, "effects": sorted(set(effects)), "note": note,
    }


def cond(taken, not_taken):
    return {"class": "conditional", "taken": taken, "not_taken": not_taken}


def repeat(repeating, final):
    return {"class": "repeat", "repeating": repeating, "final": final}


def prefix(cls):
    return {"prefix_class": cls}


# ---------------------------------------------------------------------------------------------
# Operand-class helpers
# ---------------------------------------------------------------------------------------------

def r_class(i):
    """Base-space 8-bit register operand class."""
    return "hl_ind" if i == 6 else "r"


def mem_effects(*classes):
    eff = []
    for c in classes:
        if c in ("hl_ind", "rr_ind", "nn_ind", "ix_d", "iy_d"):
            eff.append("memory")
    return eff


# ---------------------------------------------------------------------------------------------
# base space (no prefix)
# ---------------------------------------------------------------------------------------------

def classify_base(b):
    x, y, z, p, q = fields(b)
    if x == 0:
        if z == 0:
            if y == 0:
                return op("NOP", "none", "none", "control_stack", 4, 1, "op")
            if y == 1:
                return op("EX", "af", "af_alt", "control_stack", 4, 1, "op")
            if y == 2:
                return op("DJNZ", "none", "e", "control_stack", cond(13, 8), 2, "op e", effects=("control",))
            if y == 3:
                return op("JR", "none", "e", "control_stack", 12, 2, "op e", effects=("control",))
            return op("JR", "cc4", "e", "control_stack", cond(12, 7), 2, "op e", effects=("control",))
        if z == 1:
            if q == 0:
                return op("LD", "rr", "nn", "data_alu", 10, 3, "op nn_lo nn_hi")
            return op("ADD", "hl", "rr", "data_alu", 11, 1, "op")
        if z == 2:
            if p < 2:
                if q == 0:
                    return op("LD", "rr_ind", "a", "data_alu", 7, 1, "op", effects=("memory",))
                return op("LD", "a", "rr_ind", "data_alu", 7, 1, "op", effects=("memory",))
            if p == 2:
                if q == 0:
                    return op("LD", "nn_ind", "hl", "data_alu", 16, 3, "op nn_lo nn_hi", effects=("memory",))
                return op("LD", "hl", "nn_ind", "data_alu", 16, 3, "op nn_lo nn_hi", effects=("memory",))
            if q == 0:
                return op("LD", "nn_ind", "a", "data_alu", 13, 3, "op nn_lo nn_hi", effects=("memory",))
            return op("LD", "a", "nn_ind", "data_alu", 13, 3, "op nn_lo nn_hi", effects=("memory",))
        if z == 3:
            return op("INC" if q == 0 else "DEC", "rr", "none", "data_alu", 6, 1, "op")
        if z in (4, 5):
            mn = "INC" if z == 4 else "DEC"
            if y == 6:
                return op(mn, "hl_ind", "none", "data_alu", 11, 1, "op", effects=("memory",))
            return op(mn, "r", "none", "data_alu", 4, 1, "op")
        if z == 6:
            if y == 6:
                return op("LD", "hl_ind", "n", "data_alu", 10, 2, "op n", effects=("memory",))
            return op("LD", "r", "n", "data_alu", 7, 2, "op n")
        return op(ACC[y], "a", "none", "data_alu", 4, 1, "op")
    if x == 1:
        if b == 0x76:
            return op("HALT", "none", "none", "ed_io_interrupt", {"class": "halt", "t_states": 4,
                      "halted_cycle": 4}, 1, "op", effects=("halt",))
        if y == 6:
            return op("LD", "hl_ind", "r", "data_alu", 7, 1, "op", effects=("memory",))
        if z == 6:
            return op("LD", "r", "hl_ind", "data_alu", 7, 1, "op", effects=("memory",))
        return op("LD", "r", "r", "data_alu", 4, 1, "op")
    if x == 2:
        if z == 6:
            return op(ALU[y], "a", "hl_ind", "data_alu", 7, 1, "op", effects=("memory",))
        return op(ALU[y], "a", "r", "data_alu", 4, 1, "op")
    # x == 3
    if z == 0:
        return op("RET", "cc", "none", "control_stack", cond(11, 5), 1, "op", effects=("control", "stack"))
    if z == 1:
        if q == 0:
            return op("POP", "rr2", "none", "control_stack", 10, 1, "op", effects=("stack",))
        if p == 0:
            return op("RET", "none", "none", "control_stack", 10, 1, "op", effects=("control", "stack"))
        if p == 1:
            return op("EXX", "none", "none", "control_stack", 4, 1, "op")
        if p == 2:
            return op("JP", "none", "hl", "control_stack", 4, 1, "op", effects=("control", "indirect"))
        return op("LD", "sp", "hl", "data_alu", 6, 1, "op")
    if z == 2:
        return op("JP", "cc", "nn", "control_stack", 10, 3, "op nn_lo nn_hi", effects=("control",))
    if z == 3:
        if y == 0:
            return op("JP", "none", "nn", "control_stack", 10, 3, "op nn_lo nn_hi", effects=("control",))
        if y == 1:
            return prefix("escape_cb")
        if y == 2:
            return op("OUT", "n_port", "a", "ed_io_interrupt", 11, 2, "op n", effects=("io",))
        if y == 3:
            return op("IN", "a", "n_port", "ed_io_interrupt", 11, 2, "op n", effects=("io",))
        if y == 4:
            return op("EX", "sp_ind", "hl", "control_stack", 19, 1, "op", effects=("stack", "memory"))
        if y == 5:
            return op("EX", "de", "hl", "control_stack", 4, 1, "op")
        if y == 6:
            return op("DI", "none", "none", "ed_io_interrupt", 4, 1, "op", effects=("interrupt_state",))
        return op("EI", "none", "none", "ed_io_interrupt", 4, 1, "op", effects=("interrupt_state",))
    if z == 4:
        return op("CALL", "cc", "nn", "control_stack", cond(17, 10), 3, "op nn_lo nn_hi",
                  effects=("control", "stack"))
    if z == 5:
        if q == 0:
            return op("PUSH", "none", "rr2", "control_stack", 11, 1, "op", effects=("stack",))
        if p == 0:
            return op("CALL", "none", "nn", "control_stack", 17, 3, "op nn_lo nn_hi", effects=("control", "stack"))
        return prefix({1: "escape_dd", 2: "escape_ed", 3: "escape_fd"}[p])
    if z == 6:
        return op(ALU[y], "a", "n", "data_alu", 7, 2, "op n")
    return op("RST", "none", "p", "control_stack", 11, 1, "op", effects=("control", "stack"))


# ---------------------------------------------------------------------------------------------
# CB space
# ---------------------------------------------------------------------------------------------

def classify_cb(b):
    x, y, z, _, _ = fields(b)
    ind = z == 6
    dst = "hl_ind" if ind else "r"
    eff = ("memory",) if ind else ()
    if x == 0:
        return op(ROT[y], dst, "none", "cb_bit_prefix", 15 if ind else 8, 2, "CB op", m1=2,
                  documented=ROT[y] != "SLL", effects=eff)
    mn = ("BIT", "RES", "SET")[x - 1]
    if x == 1:
        return op(mn, "bit", dst, "cb_bit_prefix", 12 if ind else 8, 2, "CB op", m1=2, effects=eff)
    return op(mn, "bit", dst, "cb_bit_prefix", 15 if ind else 8, 2, "CB op", m1=2, effects=eff)


# ---------------------------------------------------------------------------------------------
# ED space
# ---------------------------------------------------------------------------------------------

ED_NOP_NOTE = "undefined ED opcode: two-byte no-operation on NMOS Z80 (8 T-states, two M1 fetches)"
BLOCK = {
    (4, 0): ("LDI", "memory"), (4, 1): ("CPI", "memory"), (4, 2): ("INI", "io"), (4, 3): ("OUTI", "io"),
    (5, 0): ("LDD", "memory"), (5, 1): ("CPD", "memory"), (5, 2): ("IND", "io"), (5, 3): ("OUTD", "io"),
    (6, 0): ("LDIR", "memory"), (6, 1): ("CPIR", "memory"), (6, 2): ("INIR", "io"), (6, 3): ("OTIR", "io"),
    (7, 0): ("LDDR", "memory"), (7, 1): ("CPDR", "memory"), (7, 2): ("INDR", "io"), (7, 3): ("OTDR", "io"),
}


def ed_nop():
    return op("NOP", "none", "ed_undefined", "ed_io_interrupt", 8, 2, "ED op", m1=2, documented=False,
              note=ED_NOP_NOTE)


def classify_ed(b):
    x, y, z, p, q = fields(b)
    if x == 1:
        if z == 0:
            if y == 6:
                return op("IN", "flags_only", "c_port", "ed_io_interrupt", 12, 2, "ED op", m1=2, documented=False,
                          effects=("io",), note="IN (C) / IN F,(C): reads the port, sets flags, discards the value")
            return op("IN", "r", "c_port", "ed_io_interrupt", 12, 2, "ED op", m1=2, effects=("io",))
        if z == 1:
            if y == 6:
                return op("OUT", "c_port", "zero", "ed_io_interrupt", 12, 2, "ED op", m1=2, documented=False,
                          effects=("io",), note="OUT (C),0: NMOS drives 0x00 (CMOS drives 0xFF; variant parameter)")
            return op("OUT", "c_port", "r", "ed_io_interrupt", 12, 2, "ED op", m1=2, effects=("io",))
        if z == 2:
            return op("SBC" if q == 0 else "ADC", "hl", "rr", "data_alu", 15, 2, "ED op", m1=2)
        if z == 3:
            alias = "ld.nn_ind_hl.base" if q == 0 else "ld.hl_nn_ind.base"
            if p == 2:
                # UM0080 lists HL in the `dd` register field of LD (nn),dd / LD dd,(nn).
                return op("LD", "nn_ind" if q == 0 else "hl", "hl" if q == 0 else "nn_ind", "data_alu", 20, 4,
                          "ED op nn_lo nn_hi", m1=2, alias_of=alias, effects=("memory",),
                          note="ED-space encoding of the base LD (nn),HL / LD HL,(nn) operation")
            if q == 0:
                return op("LD", "nn_ind", "rr_ed", "data_alu", 20, 4, "ED op nn_lo nn_hi", m1=2, effects=("memory",))
            return op("LD", "rr_ed", "nn_ind", "data_alu", 20, 4, "ED op nn_lo nn_hi", m1=2, effects=("memory",))
        if z == 4:
            if y == 0:
                return op("NEG", "a", "none", "data_alu", 8, 2, "ED op", m1=2)
            return op("NEG", "a", "none", "data_alu", 8, 2, "ED op", m1=2, documented=False, alias_of="neg.a.ed")
        if z == 5:
            if y == 1:
                return op("RETI", "none", "none", "ed_io_interrupt", 14, 2, "ED op", m1=2,
                          effects=("control", "stack", "interrupt_state"))
            if y == 0:
                return op("RETN", "none", "none", "ed_io_interrupt", 14, 2, "ED op", m1=2,
                          effects=("control", "stack", "interrupt_state"))
            return op("RETN", "none", "none", "ed_io_interrupt", 14, 2, "ED op", m1=2, documented=False,
                      alias_of="retn.ed", effects=("control", "stack", "interrupt_state"))
        if z == 6:
            mode = [0, 0, 1, 2, 0, 0, 1, 2][y]
            documented = y in (0, 2, 3)
            note = ("includes the undefined 'IM 0/1' encodings ED 4E/6E, which select mode 0 on NMOS"
                    if not documented and mode == 0 else "")
            return op("IM", "im%d" % mode, "none", "ed_io_interrupt", 8, 2, "ED op", m1=2, documented=documented,
                      alias_of="" if documented else "im.im%d.ed" % mode, effects=("interrupt_state",), note=note)
        if y == 0:
            return op("LD", "i", "a", "ed_io_interrupt", 9, 2, "ED op", m1=2)
        if y == 1:
            return op("LD", "r_refresh", "a", "ed_io_interrupt", 9, 2, "ED op", m1=2)
        if y == 2:
            return op("LD", "a", "i", "ed_io_interrupt", 9, 2, "ED op", m1=2, effects=("interrupt_state",))
        if y == 3:
            return op("LD", "a", "r_refresh", "ed_io_interrupt", 9, 2, "ED op", m1=2, effects=("interrupt_state",))
        if y == 4:
            return op("RRD", "a", "hl_ind", "ed_io_interrupt", 18, 2, "ED op", m1=2, effects=("memory",))
        if y == 5:
            return op("RLD", "a", "hl_ind", "ed_io_interrupt", 18, 2, "ED op", m1=2, effects=("memory",))
        return ed_nop()
    if x == 2 and z <= 3 and y >= 4:
        mn, kind = BLOCK[(y, z)]
        eff = ("memory",) if kind == "memory" else ("io", "memory")
        if y >= 6:
            return op(mn, "none", "none", "ed_io_interrupt", repeat(21, 16), 2, "ED op", m1=2, effects=eff)
        return op(mn, "none", "none", "ed_io_interrupt", 16, 2, "ED op", m1=2, effects=eff)
    return ed_nop()


# ---------------------------------------------------------------------------------------------
# DD / FD spaces (index register ix or iy)
# ---------------------------------------------------------------------------------------------

def classify_index(b, ix):
    """ix is 'ix' or 'iy'. Returns a form classification or a prefix-behaviour class."""
    pre = "DD" if ix == "ix" else "FD"
    d_ind = ix + "_d"
    half = ix + "_half"          # IXH/IXL or IYH/IYL (undocumented)
    x, y, z, p, q = fields(b)
    if b == 0xCB:
        return prefix("escape_" + ("ddcb" if ix == "ix" else "fdcb"))
    if b in (0xDD, 0xFD):
        return prefix("prefix_chain")
    if b == 0xED:
        return prefix("prefix_ignored_before_ed")
    lay = pre + " op"
    if x == 0:
        if z == 1 and q == 0 and p == 2:
            return op("LD", ix, "nn", "data_alu", 14, 4, lay + " nn_lo nn_hi", m1=2)
        if z == 1 and q == 1:
            return op("ADD", ix, "rr_" + ix, "data_alu", 15, 2, lay, m1=2)
        if z == 2 and p == 2:
            if q == 0:
                return op("LD", "nn_ind", ix, "data_alu", 20, 4, lay + " nn_lo nn_hi", m1=2, effects=("memory",))
            return op("LD", ix, "nn_ind", "data_alu", 20, 4, lay + " nn_lo nn_hi", m1=2, effects=("memory",))
        if z == 3 and p == 2:
            return op("INC" if q == 0 else "DEC", ix, "none", "data_alu", 10, 2, lay, m1=2)
        if z in (4, 5) and y in (4, 5, 6):
            mn = "INC" if z == 4 else "DEC"
            if y == 6:
                return op(mn, d_ind, "none", "data_alu", 23, 3, lay + " d", m1=2, effects=("memory",))
            return op(mn, half, "none", "data_alu", 8, 2, lay, m1=2, documented=False)
        if z == 6 and y in (4, 5, 6):
            if y == 6:
                return op("LD", d_ind, "n", "data_alu", 19, 4, lay + " d n", m1=2, effects=("memory",))
            return op("LD", half, "n", "data_alu", 11, 3, lay + " n", m1=2, documented=False)
        return prefix("prefix_ignored")
    if x == 1:
        if b == 0x76:
            return prefix("prefix_ignored")
        if y == 6:
            return op("LD", d_ind, "r", "data_alu", 19, 3, lay + " d", m1=2, effects=("memory",))
        if z == 6:
            return op("LD", "r", d_ind, "data_alu", 19, 3, lay + " d", m1=2, effects=("memory",))
        if y in (4, 5) or z in (4, 5):
            dst = half if y in (4, 5) else "r"
            src = half if z in (4, 5) else "r"
            return op("LD", dst, src, "data_alu", 8, 2, lay, m1=2, documented=False)
        return prefix("prefix_ignored")
    if x == 2:
        if z == 6:
            return op(ALU[y], "a", d_ind, "data_alu", 19, 3, lay + " d", m1=2, effects=("memory",))
        if z in (4, 5):
            return op(ALU[y], "a", half, "data_alu", 8, 2, lay, m1=2, documented=False)
        return prefix("prefix_ignored")
    if b == 0xE1:
        return op("POP", ix, "none", "control_stack", 14, 2, lay, m1=2, effects=("stack",))
    if b == 0xE5:
        return op("PUSH", "none", ix, "control_stack", 15, 2, lay, m1=2, effects=("stack",))
    if b == 0xE9:
        return op("JP", "none", ix, "control_stack", 8, 2, lay, m1=2, effects=("control", "indirect"))
    if b == 0xF9:
        return op("LD", "sp", ix, "data_alu", 10, 2, lay, m1=2)
    if b == 0xE3:
        return op("EX", "sp_ind", ix, "control_stack", 23, 2, lay, m1=2, effects=("stack", "memory"))
    return prefix("prefix_ignored")


def classify_index_cb(b, ix):
    x, y, z, _, _ = fields(b)
    pre = "DD" if ix == "ix" else "FD"
    d_ind = ix + "_d"
    lay = pre + " CB d op"
    copy = z != 6
    if x == 1:
        return op("BIT", "bit", d_ind, "cb_bit_prefix", 20, 4, lay, m1=2, documented=not copy,
                  alias_of="bit.bit_%s.%s" % (d_ind, "ddcb" if ix == "ix" else "fdcb") if copy else "", effects=("memory",),
                  note="register field ignored; identical to the documented (z=6) encoding" if copy else "")
    mn = ROT[y] if x == 0 else ("RES", "SET")[x - 2]
    dst = d_ind + "_copy_r" if copy else d_ind
    documented = not copy and mn != "SLL"
    note = "result also written to register r (register copy-back)" if copy else ""
    if x == 0:
        return op(mn, dst, "none", "cb_bit_prefix", 23, 4, lay, m1=2, documented=documented,
                  effects=("memory",), note=note)
    return op(mn, "bit", dst, "cb_bit_prefix", 23, 4, lay, m1=2, documented=documented,
              effects=("memory",), note=note)


def classify(space, b):
    if space == "base":
        return classify_base(b)
    if space == "cb":
        return classify_cb(b)
    if space == "ed":
        return classify_ed(b)
    if space == "dd":
        return classify_index(b, "ix")
    if space == "fd":
        return classify_index(b, "iy")
    if space == "ddcb":
        return classify_index_cb(b, "ix")
    return classify_index_cb(b, "iy")


# ---------------------------------------------------------------------------------------------
# prefix-behaviour classes and parametric prefix-chain rules
# ---------------------------------------------------------------------------------------------

PREFIX_CLASSES = {
    "escape_cb": "base-space CB: the next byte is decoded in the cb space",
    "escape_ed": "base-space ED: the next byte is decoded in the ed space",
    "escape_dd": "base-space DD: the next byte is decoded in the dd space (see prefix_rules)",
    "escape_fd": "base-space FD: the next byte is decoded in the fd space (see prefix_rules)",
    "escape_ddcb": "DD CB: a displacement byte d follows, then the opcode byte decoded in the ddcb space",
    "escape_fdcb": "FD CB: a displacement byte d follows, then the opcode byte decoded in the fdcb space",
    "prefix_chain": "DD/FD after DD/FD: the earlier prefix is superseded (rule index_prefix_chain)",
    "prefix_ignored": "DD/FD before an opcode that does not use HL/H/L/(HL): the prefix is ignored and the byte "
                      "executes as its base-space form (rule index_prefix_ignored)",
    "prefix_ignored_before_ed": "DD/FD before ED: the prefix is ignored and ED escapes to the ed space "
                                "(rule index_prefix_ignored)",
}

PARTITION_LEGEND = {"D": "documented_form", "U": "undocumented_form", "P": "prefix_behavior", "X": "excluded"}

PREFIX_RULES = [
    {
        "id": "index_prefix_chain",
        "family": "cb_bit_prefix",
        "pattern": "P1 P2 ... Pk b, each Pi in {DD, FD}, k >= 1, b not in {DD, FD}",
        "effective_prefix": "Pk (the last prefix); P1..P(k-1) are ignored",
        "decode": "b is decoded in the dd space if Pk = DD, in the fd space if Pk = FD",
        "t_states": "4 * (k - 1) + T(effective instruction)",
        "m1_fetches_and_r_increment": "(k - 1) + m1_fetches(effective instruction); R low 7 bits += that count, "
                                      "bit 7 preserved",
        "interrupt_acceptance": "never between a prefix and the following byte: a chain of any length plus its "
                                "effective instruction is one indivisible boundary (docs/architecture/"
                                "z80-cpu-contract.md section 4.7); each superseded prefix is still a separately "
                                "timed 4-T-state M1 cycle",
        "length": "k + length(effective instruction) - 1",
        "bound": "no maximum length. Bytes are fetched at successive logical addresses wrapping 0xFFFF -> 0x0000 "
                 "through the generation-time logical code mapping, never through a storage-image edge. A run that "
                 "never reaches a non-prefix opcode (it revisits a (mapping, address, effective prefix) state) is a "
                 "deterministic prefix lock: 4 T-states and one M1 per prefix forever, no interrupt acceptance. A "
                 "byte whose mapping cannot be statically identified fails closed",
    },
    {
        "id": "index_prefix_ignored",
        "family": "cb_bit_prefix",
        "pattern": "P b, P in {DD, FD}, b classified prefix_ignored or prefix_ignored_before_ed in the P space",
        "effective_prefix": "none",
        "decode": "b (and any following bytes) are decoded in the base space; ED escapes to the ed space",
        "t_states": "4 + T(base or ed instruction)",
        "m1_fetches_and_r_increment": "1 + m1_fetches(base or ed instruction)",
        "interrupt_acceptance": "as index_prefix_chain",
        "length": "1 + length(base or ed instruction)",
        "bound": "as index_prefix_chain",
    },
    {
        "id": "index_cb_displacement",
        "family": "cb_bit_prefix",
        "pattern": "P CB d op, P in {DD, FD}",
        "effective_prefix": "P",
        "decode": "d is a signed displacement read before op; op is decoded in the ddcb/fdcb space",
        "t_states": "per ddcb/fdcb form (BIT 20, all others 23)",
        "m1_fetches_and_r_increment": "2 (d and op are ordinary memory reads, not M1 fetches)",
        "interrupt_acceptance": "only after op completes",
        "length": "4",
        "bound": "as index_prefix_chain (d and op are fetched at wrapping logical addresses)",
    },
    {
        "id": "no_other_chains",
        "family": "cb_bit_prefix",
        "pattern": "CB and ED",
        "effective_prefix": "n/a",
        "decode": "CB and ED never chain: every byte after CB is a cb-space opcode and every byte after ED is an "
                  "ed-space opcode (ED DD / ED FD / ED CB / ED ED are ed-space two-byte no-operations)",
        "t_states": "per form",
        "m1_fetches_and_r_increment": "per form",
        "interrupt_acceptance": "after the form",
        "length": "per form",
        "bound": "as above",
    },
]


# ---------------------------------------------------------------------------------------------
# derivation
# ---------------------------------------------------------------------------------------------

def form_key(space, c):
    return (space, c["mnemonic"], c["dst"], c["src"], c["documented"], c["alias_of"])


def form_id(space, c):
    parts = [c["mnemonic"].lower()]
    ops = [o for o in (c["dst"], c["src"]) if o != "none"]
    if ops:
        parts.append("_".join(ops))
    tag = space
    if not c["documented"] and c["alias_of"]:
        tag += ".alias"
    elif not c["documented"]:
        tag += ".undoc"
    parts.append(tag)
    return ".".join(parts)


def to_ranges(values):
    out = []
    for v in sorted(values):
        if out and out[-1][1] + 1 == v:
            out[-1][1] = v
        else:
            out.append([v, v])
    return out


def observables(c):
    obs = ["state"]
    eff = set(c["effects"])
    if eff & {"memory", "stack"}:
        obs.append("memory")
    if "io" in eff:
        obs.append("io")
    return obs


def derive():
    forms = {}
    order = []
    partition = {}
    prefix_bytes = {}
    for space in SPACES:
        letters = []
        for b in range(256):
            c = classify(space, b)
            if "prefix_class" in c:
                prefix_bytes.setdefault(space, {}).setdefault(c["prefix_class"], []).append(b)
                letters.append("P")
                continue
            key = form_key(space, c)
            if key not in forms:
                forms[key] = dict(c, space=space, bytes=[])
                order.append(key)
            f = forms[key]
            for field in ("family", "timing", "length", "layout", "m1_fetches", "effects", "note"):
                assert f[field] == c[field], ("non-uniform form", space, hex(b), field, f[field], c[field])
            f["bytes"].append(b)
            letters.append("D" if c["documented"] else "U")
        partition[space] = "".join(letters)
    rows = []
    for key in order:
        f = forms[key]
        rid = form_id(f["space"], f)
        rows.append({
            "id": rid, "space": f["space"], "mnemonic": f["mnemonic"], "dst": f["dst"], "src": f["src"],
            "family": f["family"], "task": FAMILIES[f["family"]],
            "status": "documented" if f["documented"] else "undocumented",
            "alias_of": f["alias_of"], "length": f["length"], "layout": f["layout"],
            "m1_fetches": f["m1_fetches"], "timing": f["timing"], "effects": f["effects"],
            "observables": observables(f), "scope": "in_scope", "note": f["note"],
            "encodings": len(f["bytes"]), "byte_ranges": to_ranges(f["bytes"]),
        })
    rows.sort(key=lambda r: (SPACES.index(r["space"]), r["byte_ranges"][0][0], r["id"]))
    ids = [r["id"] for r in rows]
    assert len(ids) == len(set(ids)), "duplicate form ids: %r" % sorted({i for i in ids if ids.count(i) > 1})
    known = set(ids)
    for r in rows:
        assert not r["alias_of"] or r["alias_of"] in known, ("dangling alias", r["id"], r["alias_of"])
    # exactly-one ownership per (space, byte)
    for space in SPACES:
        owned = sum(r["encodings"] for r in rows if r["space"] == space)
        owned += sum(len(v) for v in prefix_bytes.get(space, {}).values())
        assert owned == 256, (space, owned)
    per_space = {}
    for space in SPACES:
        s = partition[space]
        per_space[space] = {
            "forms": sum(1 for r in rows if r["space"] == space),
            "documented_encodings": s.count("D"), "undocumented_encodings": s.count("U"),
            "prefix_behavior_bytes": s.count("P"), "excluded_encodings": s.count("X"),
            "prefix_classes": {k: len(v) for k, v in sorted(prefix_bytes.get(space, {}).items())},
        }
    per_family = {}
    for fam in FAMILIES:
        fr = [r for r in rows if r["family"] == fam]
        per_family[fam] = {
            "task": FAMILIES[fam], "forms": len(fr), "encodings": sum(r["encodings"] for r in fr),
            "documented_forms": sum(1 for r in fr if r["status"] == "documented"),
            "undocumented_forms": sum(1 for r in fr if r["status"] == "undocumented"),
            "mnemonics": sorted({r["mnemonic"] for r in fr}),
        }
    columns = ["id", "space", "mnemonic", "dst", "src", "family", "task", "status", "alias_of", "scope", "length",
               "layout", "m1_fetches", "timing", "effects", "observables", "encodings", "byte_ranges", "note"]
    data = {
        "schema": SCHEMA,
        "dataset": "z80-legal-forms",
        "target": "NMOS Zilog Z80, complete encoding space (documented and undocumented); CMOS differences are "
                  "variant parameters recorded in 'variant_parameters', not separate forms",
        "sources": [
            "Zilog Z80 CPU User Manual UM0080 (public): instruction set, opcode tables, M-cycle/T-state counts",
            "Sean Young, The Undocumented Z80 Documented, v0.91 (public): undocumented opcodes, prefixes, "
            "DDCB/FDCB copy-back, ED holes, IN (C), OUT (C),0, SLL",
            "Cristian Dinu, Decoding Z80 Opcodes (public): x/y/z/p/q opcode-field decomposition",
        ],
        "independence": "test-side expectation only; derived without reading any segarecomp Z80 decoder or any "
                        "oracle output; never imported by production code",
        "spaces": {s: {"prefix": SPACE_PREFIX[s], "bytes": 256} for s in SPACES},
        "families": {k: {"task": v} for k, v in FAMILIES.items()},
        "vocabulary": {
            "form": "an in-space mnemonic x operand-class combination; 'byte_ranges' lists its exact opcode bytes "
                    "within 'space' (inclusive [lo,hi] runs). A consumer may claim a form in a coverage stage only "
                    "if every listed byte passes",
            "operand_classes": "r = B,C,D,E,H,L,A; hl_ind = (HL); rr_ind = (BC)/(DE); nn_ind = (nn); ix_d/iy_d = "
                               "(IX+d)/(IY+d); ix_half/iy_half = IXH,IXL/IYH,IYL; rr = BC,DE,HL,SP; rr2 = BC,DE,HL,AF; "
                               "rr_ix/rr_iy = BC,DE,IX/IY,SP; rr_ed = BC,DE,SP (ED LD); cc = 8 conditions; cc4 = "
                               "NZ,Z,NC,C; n/nn/e/p = immediate, word, relative displacement, RST vector; n_port/c_port "
                               "= (n)/(C); bit = bit index 0-7; *_copy_r = (IX+d)/(IY+d) result also copied to r",
            "status": "documented = listed by UM0080; undocumented = listed only by public undocumented-behaviour "
                      "references; alias_of names the documented form with identical architectural effect",
            "scope": "in_scope or excluded:<reason>; T001 scope decision excludes nothing",
            "timing": "NMOS T-states from UM0080 (Young for undocumented forms): fixed; conditional "
                      "(taken/not_taken); repeat (repeating iteration / final iteration); halt (per halted "
                      "4-T-state NOP cycle). Prefix-chain cost is parametric: see prefix_rules",
            "m1_fetches": "opcode-fetch (M1) cycles; the refresh register R low 7 bits increment once per M1 fetch "
                          "(bit 7 preserved); repeat forms repeat both M1 fetches per iteration",
            "effects": "coarse observable-effect classes: memory, stack, io, control, indirect, interrupt_state, halt",
            "observables": "which oracle comparison columns apply to the form: state always; memory if the form "
                           "reads or writes memory; io if it performs I/O transactions",
            "unsupported_by_segarecomp": "MEASUREMENT-SIDE ONLY: assigned by coverage tooling, never stored here",
        },
        "variant_parameters": {
            "cpu_variant": "nmos (default for Master System / Genesis)",
            "cmos_differences": [
                "OUT (C),0 (ED 71) drives 0xFF instead of 0x00",
                "LD A,I / LD A,R P/V: no NMOS interrupt-acceptance clearing quirk",
            ],
            "policy": "one NMOS model; the two CMOS differences are the only variant parameters and are "
                      "introduced only when a consumer (e.g. Game Gear) requires them",
        },
        "coverage_schema": {
            "unit": "form (claimed only when every concrete encoding in byte_ranges passes)",
            "stages": list(COVERAGE_STAGES),
            "stage_meaning": {
                "decodes": "production decoder yields this form identity, length and operands for every byte",
                "lowers": "the form lowers to Z80 C11 semantics without a fail-closed rejection",
                "emits": "generated C for the form is emitted",
                "compiles": "the emitted C compiles as strict C11 with warnings enabled",
                "executes": "generated-native execution runs the form",
                "aot_admitted": "the static-code strategy admits the form as an owner/instruction start",
                "oracle_state": "registers, flags, internal state (MEMPTR, Q, IFF, IM, R, HALT) match the oracle",
                "oracle_memory": "memory write sequence matches the oracle (n/a when 'memory' not in observables)",
                "oracle_io": "I/O transaction sequence matches the oracle (n/a when 'io' not in observables)",
                "timing_modeled": "generated code accounts the form's T-states per the timing class",
                "timing_validated": "accounted T-states match the oracle for every timing outcome",
            },
            "ratchet": "coverage snapshots are regenerated deterministically; a form never regresses in a stage",
        },
        "prefix_classes": PREFIX_CLASSES,
        "prefix_rules": PREFIX_RULES,
        "form_columns": columns,
        "forms": [[r[c] for c in columns] for r in rows],
        "counts": {
            "forms": len(rows),
            "documented_forms": sum(1 for r in rows if r["status"] == "documented"),
            "undocumented_forms": sum(1 for r in rows if r["status"] == "undocumented"),
            "excluded_forms": sum(1 for r in rows if r["scope"] != "in_scope"),
            "encodings": sum(r["encodings"] for r in rows),
            "per_space": per_space,
            "per_family": per_family,
        },
        "space_partition": {
            "legend": PARTITION_LEGEND,
            "index": "character i of a space string classifies opcode byte i of that space",
            "spaces": {s: partition[s] for s in SPACES},
        },
    }
    return data


def render(data):
    """Stable text: sorted keys; 'forms' one row per line."""
    lines = ["{"]
    keys = sorted(data)
    for i, k in enumerate(keys):
        v = data[k]
        comma = "," if i + 1 < len(keys) else ""
        if k == "forms":
            body = ",\n".join("  " + json.dumps(r, separators=(",", ":"), sort_keys=True) for r in v)
            lines.append(' "forms": [\n%s\n ]%s' % (body, comma))
        else:
            text = json.dumps(v, sort_keys=True, indent=1)
            lines.append(" %s: %s%s" % (json.dumps(k), text.replace("\n", "\n "), comma))
    lines.append("}")
    return "\n".join(lines) + "\n"


def default_output():
    return pathlib.Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "z80-legal-forms.json"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--output", type=pathlib.Path, default=default_output())
    ap.add_argument("--check", action="store_true", help="fail if the output differs from a fresh derivation")
    ap.add_argument("--summary", action="store_true", help="print per-space and per-family counts")
    args = ap.parse_args(argv)
    data = derive()
    text = render(data)
    if args.summary:
        print(json.dumps(data["counts"], sort_keys=True, indent=1))
        return 0
    if args.check:
        return 0 if args.output.read_bytes() == text.encode("utf-8") else 1
    args.output.write_bytes(text.encode("utf-8"))
    c = data["counts"]
    print("%d forms (%d documented, %d undocumented), %d encodings" % (
        c["forms"], c["documented_forms"], c["undocumented_forms"], c["encodings"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
