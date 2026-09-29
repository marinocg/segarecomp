#!/usr/bin/env python3
"""SEG-008-T004: hermetic flag/result check of the data_alu lowering against an independent Python model.

usage: z80_data_alu_flags_test.py <z80_image_emitter> <cc> <product-root>

The model is written from the contract flag table (docs/architecture/z80-cpu-contract.md section 5) with plain integer
arithmetic and no shared code with the C lowering. It checks the generated-native result of the swept 8-bit ADD/ADC/SUB/
SBC/CP/AND/XOR/OR immediate and register forms, DAA, NEG, CPL, SCF/CCF (including Q), the accumulator rotates, INC/DEC r,
ADD HL,rr and ADC/SBC HL,rr: result registers, F (incl. X/Y), WZ and Q. No oracle is needed; oracle credit comes from
the committed manifest and z80_conformance_oracle_test.py.
"""
import json
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
import z80_conformance as z  # noqa: E402

EMITTER, CC = sys.argv[1], sys.argv[2]
FAILED = []
REGS = {0: "b", 1: "c", 2: "d", 3: "e", 4: "h", 5: "l", 7: "a"}


def par(x):
    return 0x04 if bin(x & 0xFF).count("1") % 2 == 0 else 0


def szxy(r):
    return (r & 0xA8) | (0x40 if r & 0xFF == 0 else 0)


def alu(mn, a, v, c):
    if mn in ("and", "xor", "or"):
        r = {"and": a & v, "xor": a ^ v, "or": a | v}[mn]
        return r, szxy(r) | par(r) | (0x10 if mn == "and" else 0)
    sub = mn in ("sub", "sbc", "cp")
    cin = c if mn in ("adc", "sbc") else 0
    full = a - v - cin if sub else a + v + cin
    r = full & 0xFF
    h = ((a & 15) - (v & 15) - cin < 0) if sub else ((a & 15) + (v & 15) + cin > 15)
    ov = ((a ^ v) & (a ^ r) & 0x80) if sub else (~(a ^ v) & (a ^ r) & 0x80)
    xy = (v & 0x28) | (r & 0x80) | (0x40 if r == 0 else 0) if mn == "cp" else szxy(r)
    f = xy | (0x10 if h else 0) | (0x04 if ov else 0) | (0x02 if sub else 0) | (1 if full < 0 or full > 255 else 0)
    return r, f


def daa(a, f):
    c, h, n = f & 1, bool(f & 0x10), bool(f & 2)
    corr = (6 if h or (a & 15) > 9 else 0) | (0x60 if c or a > 0x99 else 0)
    nc = 1 if c or a > 0x99 else 0
    r = (a - corr if n else a + corr) & 0xFF
    hf = (h and (a & 15) < 6) if n else (a & 15) > 9
    return r, szxy(r) | (0x10 if hf else 0) | par(r) | (0x02 if n else 0) | nc


def check(cond, message):
    if not cond:
        FAILED.append(message)


def hexv(text):
    return int(text, 16)


def main():
    tc = z.Toolchain(CC, EMITTER)
    docs = {"data_alu": json.loads((ROOT / "tests/fixtures/z80-conformance-vectors/data_alu.json").read_text())}
    by_form = z.form_vectors(docs)
    _, forms = z.load_dataset()
    wanted = []
    for mn in ("add", "adc", "sub", "sbc", "cp", "and", "xor", "or"):
        wanted += ["%s.a_n.base" % mn, "%s.a_r.base" % mn]
    wanted += ["daa.a.base", "neg.a.ed", "cpl.a.base", "scf.a.base", "ccf.a.base", "rlca.a.base", "rrca.a.base",
               "rla.a.base", "rra.a.base", "inc.r.base", "dec.r.base", "add.hl_rr.base", "adc.hl_rr.ed", "sbc.hl_rr.ed"]
    vecs = [v for f in wanted for v in by_form[f]]
    with tempfile.TemporaryDirectory() as tmp:
        batches = z.slot_batches(vecs)
        report = z.run_batches(tc, batches, pathlib.Path(tmp), with_oracle=False)
        count = 0
        for batch in batches:
            r = report[batch.name]
            check(not r.get("error"), "batch failed: %s" % r.get("error"))
            if r.get("error"):
                continue
            for vec, _ in batch.vectors:
                count += 1
                steps = r["generated"][vec.name]
                if len(steps) == 2:  # a preceding instruction set Q: the form runs on the state after step 0
                    seeded = {k: hexv(v) for k, v in steps[0]["cpu"].items()}
                    model(vec, forms[vec.form], steps[1], seeded)
                else:
                    model(vec, forms[vec.form], steps[0], None)
    if FAILED:
        print("\n".join("FAIL: " + m for m in FAILED[:30]) + "\n%d failures" % len(FAILED))
        return 1
    print("z80 data_alu flags: ok (%d vectors against the independent model)" % count)
    return 0


def model(vec, form, step, seeded):
    st = vec.state if seeded is None else seeded
    cpu = {k: hexv(v) for k, v in step["cpu"].items()}
    prefix = 0 if seeded is not None else len(vec.code) - form["length"]
    op = vec.code[len(vec.code) - form["length"] + form["layout"].split().index("op")]
    mn, a, f = form["mnemonic"].lower().rstrip("_"), st["a"], st["f"]
    exp, wz = dict(st), None
    q_in = 0 if prefix else st["q"]
    if form["family"] != "data_alu":
        return
    if mn in ("add", "adc", "sub", "sbc", "cp", "and", "xor", "or") and form["dst"] == "a":
        v = vec.code[-1] if form["src"] == "n" else st[REGS[op & 7]]
        r, nf = alu(mn, a, v, f & 1)
        exp["a"] = a if mn == "cp" else r
        exp["f"] = nf
    elif mn == "daa":
        exp["a"], exp["f"] = daa(a, f)
    elif mn == "neg":
        exp["a"], exp["f"] = alu("sub", 0, a, 0)
    elif mn == "cpl":
        exp["a"] = a ^ 0xFF
        exp["f"] = (f & 0xC5) | (exp["a"] & 0x28) | 0x12
    elif mn in ("scf", "ccf"):
        xy = ((q_in ^ f) | a) & 0x28
        exp["f"] = (f & 0xC4) | xy | (1 if mn == "scf" else (0x10 if f & 1 else 1))
    elif mn in ("rlca", "rrca", "rla", "rra"):
        old = f & 1
        if mn in ("rlca", "rla"):
            cy = a >> 7
            r = ((a << 1) | (cy if mn == "rlca" else old)) & 0xFF
        else:
            cy = a & 1
            r = (a >> 1) | ((cy if mn == "rrca" else old) << 7)
        exp["a"], exp["f"] = r, (f & 0xC4) | (r & 0x28) | cy
    elif mn in ("inc", "dec") and form["dst"] == "r":
        reg = REGS[(op >> 3) & 7]
        t = st[reg]
        r = (t + 1 if mn == "inc" else t - 1) & 0xFF
        h = (t & 15) == (15 if mn == "inc" else 0)
        ov = t == (0x7F if mn == "inc" else 0x80)
        exp[reg] = r
        exp["f"] = (f & 1) | szxy(r) | (0x10 if h else 0) | (0x04 if ov else 0) | (0 if mn == "inc" else 2)
    elif form["dst"] == "hl" and form["src"] == "rr":
        pair = (op >> 4) & 3
        hl = st["h"] << 8 | st["l"]
        y = [st["b"] << 8 | st["c"], st["d"] << 8 | st["e"], hl, st["sp"]][pair]
        c = f & 1
        if mn == "add":
            full = hl + y
            r = full & 0xFFFF
            nf = (f & 0xC4) | ((r >> 8) & 0x28) | (0x10 if (hl & 0xFFF) + (y & 0xFFF) > 0xFFF else 0) | (1 if full > 0xFFFF else 0)
        else:
            sub = mn == "sbc"
            full = hl - y - c if sub else hl + y + c
            r = full & 0xFFFF
            h = ((hl & 0xFFF) - (y & 0xFFF) - c < 0) if sub else ((hl & 0xFFF) + (y & 0xFFF) + c > 0xFFF)
            ov = ((hl ^ y) & (hl ^ r) & 0x8000) if sub else (~(hl ^ y) & (hl ^ r) & 0x8000)
            nf = ((r >> 8) & 0xA8) | (0x40 if r == 0 else 0) | (0x10 if h else 0) | (0x04 if ov else 0) | \
                (2 if sub else 0) | (1 if full < 0 or full > 0xFFFF else 0)
        exp["h"], exp["l"], exp["f"] = r >> 8, r & 0xFF, nf
        wz = (hl + 1) & 0xFFFF
    else:
        return
    if wz is not None:
        exp["wz"] = wz
    for k in ("a", "b", "c", "d", "e", "h", "l", "f"):
        check(cpu[k] == exp[k], "%s: %s = %02X, model %02X" % (vec.name, k, cpu[k], exp[k]))
    if wz is not None:
        check(cpu["wz"] == wz, "%s: wz = %04X, model %04X" % (vec.name, cpu["wz"], wz))
    check(cpu["q"] == exp["f"], "%s: Q %02X, model %02X" % (vec.name, cpu["q"], exp["f"]))


if __name__ == "__main__":
    sys.exit(main())
