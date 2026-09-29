#!/usr/bin/env python3
"""SEG-008-T001: cross-check every encoding of the independent Z80 legal-form dataset against the pinned
redcode/Z80 oracle, with exact expectations:

  * every form encoding: exact T-states, exact low-7-bit R (M1 fetches) and exact resulting PC (length for
    straight-line forms; the computed target or the fall-through for control forms);
  * conditional forms (JR cc, JP cc, CALL cc, RET cc, DJNZ): run with the condition forced true and false,
    each asserting the exact taken / not-taken T-states and PC;
  * repeat forms (LDIR/CPIR/INIR/OTIR and decrementing): run a repeating iteration (PC stays, repeating
    T-states) and a final iteration (PC advances, final T-states);
  * the DD/FD `prefix_ignored` bytes (4 + base) and parametric chains (k = 1..8; repeated and alternating),
    a prefix before ED and a chain before DDCB;
  * logical fetch wrap (ADR 0058): instructions, displacement/operand bytes and prefix chains that continue
    from 0xFFFF to 0x0000 (a full 64 KiB mapping has no edge).

Mutation controls: the same run is repeated on in-memory corrupted copies of the dataset (taken/not-taken
swapped, repeating T-states changed, control-form lengths changed, one M1 count changed); each must produce
mismatches, proving the check bites.

Test-side only. Skips cleanly (exit 0) unless SEGARECOMP_Z80_ORACLE_CHECKOUT names the directory holding the
pinned redcode_Z80 and redcode_Zeta clones (docs/decisions/0057-*); a wrong or dirty pin is a hard failure.
The dataset stays independent: the oracle only falsifies it, it never generates it."""
import copy
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from z80_oracle_adapter_smoke_test import DEFINES, checkout, skip_reason  # noqa: E402

ROOT = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else HERE.parent
FIXTURE = ROOT / "tests" / "fixtures" / "z80-legal-forms.json"
SOURCE = HERE / "z80_oracle" / "legal_forms_crosscheck.c"
PREFIX = {"base": [], "cb": [0xCB], "ed": [0xED], "dd": [0xDD], "fd": [0xFD]}
START, STACK_WORD, A = 0x1000, 0x5678, 0x55
OPERANDS = [0x12, 0x34]          # n = 0x12, d = +0x12, e = +0x12, nn = 0x3412
IO_REPEAT = {"INIR", "INDR", "OTIR", "OTDR"}
JP_REG = {"hl": 0x4000, "ix": 0x5000, "iy": 0x6000}


def encode(space, length, b):
    if space in ("ddcb", "fdcb"):
        return [0xDD if space == "ddcb" else 0xFD, 0xCB, OPERANDS[0], b]
    pre = PREFIX[space]
    return pre + [b] + (OPERANDS * 4)[:length - len(pre) - 1]


def condition_true(idx, f):
    flag = {0: 0x40, 1: 0x40, 2: 0x01, 3: 0x01, 4: 0x04, 5: 0x04, 6: 0x80, 7: 0x80}[idx]
    return bool(f & flag) == bool(idx & 1)


def outcomes(form, b, length, extra_t=0, extra_m1=0):
    """Yield (af, bc, expected_pc, expected_t, expected_r) cases for one encoding of `form`."""
    m, t, eff = form["mnemonic"], form["timing"], form["effects"]
    m1 = form["m1_fetches"] + extra_m1
    r = (A & 0x7F) if form["id"] == "ld.r_refresh_a.ed" else m1
    fall = START + length
    cc_form = form["dst"] in ("cc", "cc4")
    if t["class"] == "repeat":
        for repeating in (True, False):
            if m in IO_REPEAT:
                bc = 0x0201 if repeating else 0x0101
            else:
                bc = 0x0002 if repeating else 0x0001
            yield (A << 8, bc, START if repeating else fall,
                   (t["repeating"] if repeating else t["final"]) + extra_t, r)
        return
    if m == "DJNZ":
        for bc, taken in ((0x0201, True), (0x0101, False)):
            yield (A << 8, bc, (START + length + OPERANDS[0]) if taken else fall,
                   (t["taken"] if taken else t["not_taken"]) + extra_t, r)
        return
    for f in ((0x00, 0xFF) if cc_form else (0x00,)):
        taken = True
        if cc_form:
            idx = ((b >> 3) & 7) - (4 if form["dst"] == "cc4" else 0)
            taken = condition_true(idx, f)
        if "control" not in eff:
            pc = fall
        elif not taken:
            pc = fall
        elif m in ("JR",):
            pc = START + length + OPERANDS[0]
        elif m in ("JP", "CALL") and form["src"] == "nn":
            pc = OPERANDS[1] << 8 | OPERANDS[0]
        elif m == "JP":
            pc = JP_REG[form["src"]]
        elif m in ("RET", "RETI", "RETN"):
            pc = STACK_WORD
        elif m == "RST":
            pc = b & 0x38
        else:
            raise AssertionError("unmodelled control form " + form["id"])
        if t["class"] == "conditional":
            tt = t["taken"] if taken else t["not_taken"]
        else:
            tt = t["t_states"]
        yield (A << 8 | f, 0x0101, pc, tt + extra_t, r)


def cases(data):
    forms = [dict(zip(data["form_columns"], row)) for row in data["forms"]]
    base = {}
    out = []
    for f in forms:
        for lo, hi in f["byte_ranges"]:
            for b in range(lo, hi + 1):
                if f["space"] == "base":
                    base[b] = f
                bs = encode(f["space"], f["length"], b)
                out += [(bs,) + c for c in outcomes(f, b, f["length"])]
    part = data["space_partition"]["spaces"]
    for space, pre in (("dd", 0xDD), ("fd", 0xFD)):
        for b in range(256):
            if part[space][b] != "P" or b in (0xCB, 0xDD, 0xFD, 0xED):
                continue
            f = base[b]
            bs = [pre] + encode("base", f["length"], b)
            out += [(bs,) + c for c in outcomes(f, b, 1 + f["length"], extra_t=4, extra_m1=1)]
    for k in range(1, 9):
        for pattern in ([0xDD], [0xFD], [0xDD, 0xFD], [0xFD, 0xDD]):
            chain = [pattern[i % len(pattern)] for i in range(k)]
            out.append((chain + [0x21, 0x34, 0x12], A << 8, 0x0101, START + k + 3, 4 * (k - 1) + 14, (k - 1) + 2))
    out.append(([0xDD, 0xED, 0x44], A << 8, 0x0101, START + 3, 12, 3))
    out.append(([0xFD, 0xDD, 0xCB, 0x12, 0x06], A << 8, 0x0101, START + 5, 27, 3))
    out = [(START,) + c for c in out]
    # logical fetch wrap across 0xFFFF -> 0x0000 (start, bytes, af, bc, expected pc, T, R)
    out += [
        (0xFFFF, [0x3E, 0x42], A << 8, 0x0101, 0x0001, 7, 1),                   # LD A,n: operand at 0x0000
        (0xFFFE, [0xDD, 0x21, 0x34, 0x12], A << 8, 0x0101, 0x0002, 14, 2),      # LD IX,nn: nn at 0x0000-1
        (0xFFFF, [0xDD, 0xCB, 0x12, 0x06], A << 8, 0x0101, 0x0003, 23, 2),      # RLC (IX+d): d, op wrapped
        (0xFFFD, [0xDD, 0xFD, 0xDD, 0x21, 0x34, 0x12], A << 8, 0x0101, 0x0003, 22, 4),  # chain across wrap
        (0xFFFF, [0x18, 0x12], A << 8, 0x0101, 0x0013, 12, 1),                  # JR e: e at 0x0000
        (0xFFFE, [0xC3, 0x34, 0x12], A << 8, 0x0101, 0x1234, 10, 1),            # JP nn: nn high at 0x0000
    ]
    return out


def render(case_list):
    return "".join("%04x %d %s %04x %04x %04x %d %02x\n" % (start, len(bs), " ".join("%02x" % x for x in bs), af, bc,
                                                             pc, t, r)
                   for start, bs, af, bc, pc, t, r in case_list)


def mutations(data):
    cols = data["form_columns"]
    ti, li, mi, ei, ii = (cols.index(c) for c in ("timing", "length", "m1_fetches", "effects", "id"))

    def mutate(fn):
        d = copy.deepcopy(data)
        for row in d["forms"]:
            fn(row)
        return d

    def swap(row):
        if row[ti]["class"] == "conditional":
            row[ti]["taken"], row[ti]["not_taken"] = row[ti]["not_taken"], row[ti]["taken"]

    def repeating(row):
        if row[ti]["class"] == "repeat":
            row[ti]["repeating"] = 99

    def control_length(row):
        if "control" in row[ei]:
            row[li] += 1

    def one_m1(row):
        if row[ii] == "ld.ix_nn.dd":
            row[mi] += 1

    return [("conditional taken/not-taken swapped", mutate(swap)),
            ("repeating T-states corrupted", mutate(repeating)),
            ("control-form lengths corrupted", mutate(control_length)),
            ("one M1 count corrupted", mutate(one_m1))]


def run(exe, case_list):
    r = subprocess.run([str(exe)], input=render(case_list), text=True, capture_output=True, timeout=300)
    return r.returncode, r.stdout


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else "cc"
    root = checkout()
    if root is None:
        print("skipped: " + skip_reason())
        return 0
    data = json.loads(FIXTURE.read_text())
    with tempfile.TemporaryDirectory() as tmp:
        exe = pathlib.Path(tmp) / "crosscheck"
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", *DEFINES,
                        "-I%s" % (root / "redcode_Z80" / "API"), "-I%s" % (root / "redcode_Zeta" / "API"),
                        str(SOURCE), str(root / "redcode_Z80" / "sources" / "Z80.c"), "-o", str(exe)], check=True)
        rc, out = run(exe, cases(data))
        sys.stdout.write(out)
        if rc != 0:
            return rc
        for name, mutated in mutations(data):
            mrc, mout = run(exe, cases(mutated))
            summary = mout.strip().splitlines()[-1] if mout.strip() else "(no output)"
            print("mutation control [%s]: %s" % (name, summary))
            if mrc == 0:
                print("FAIL: mutation control did not bite: " + name)
                return 1
    print("OK: dataset agrees with the pinned oracle; all mutation controls bite")
    return 0


if __name__ == "__main__":
    sys.exit(main())
