#!/usr/bin/env python3
"""SEG-008-T001: cross-check every encoding of the independent Z80 legal-form dataset (length, T-states per
timing class, M1/R increment) and the parametric DD/FD prefix rules against the pinned redcode/Z80 oracle.

Test-side only. Skips cleanly (exit 0) unless SEGARECOMP_Z80_ORACLE_CHECKOUT names the directory holding the
pinned redcode_Z80 and redcode_Zeta clones (see docs/decisions/0057-*); a wrong or dirty pin is a hard failure.
The dataset stays independent: the oracle only falsifies it, it never generates it."""
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from z80_oracle_adapter_smoke_test import DEFINES, checkout  # noqa: E402

ROOT = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else HERE.parent
FIXTURE = ROOT / "tests" / "fixtures" / "z80-legal-forms.json"
SOURCE = HERE / "z80_oracle" / "legal_forms_crosscheck.c"
PREFIX = {"base": [], "cb": [0xCB], "ed": [0xED], "dd": [0xDD], "fd": [0xFD]}
REPEAT_B = {"INIR", "INDR", "OTIR", "OTDR"}


def timing_pair(t):
    c = t["class"]
    if c in ("fixed", "halt"):
        return t["t_states"], t["t_states"]
    if c == "conditional":
        return t["taken"], t["not_taken"]
    return t["final"], t["final"]


def encode(space, form, b):
    if space in ("ddcb", "fdcb"):
        return [0xDD if space == "ddcb" else 0xFD, 0xCB, 0x00, b]
    return PREFIX[space] + [b] + [0] * (form["length"] - len(PREFIX[space]) - 1)


def cases():
    data = json.loads(FIXTURE.read_text())
    forms = [dict(zip(data["form_columns"], r)) for r in data["forms"]]
    base = {}
    out = []
    for f in forms:
        bc = 0x0101 if f["mnemonic"] in REPEAT_B else 0x0001
        ta, tb = timing_pair(f["timing"])
        length = -1 if "control" in f["effects"] else f["length"]
        for lo, hi in f["byte_ranges"]:
            for b in range(lo, hi + 1):
                if f["space"] == "base":
                    base[b] = f
                # LD R,A overwrites R with A (= 0 in the harness state) instead of incrementing it.
                r_inc = 0 if f["id"] == "ld.r_refresh_a.ed" else f["m1_fetches"]
                out.append((encode(f["space"], f, b), length, ta, tb, r_inc, bc))
    part = data["space_partition"]["spaces"]
    for space, pre in (("dd", 0xDD), ("fd", 0xFD)):
        for b in range(256):
            if part[space][b] != "P" or b in (0xCB, 0xDD, 0xFD, 0xED):
                continue
            f = base[b]
            ta, tb = timing_pair(f["timing"])
            length = -1 if "control" in f["effects"] else 1 + f["length"]
            out.append(([pre] + encode("base", f, b), length, ta + 4, tb + 4, 1 + f["m1_fetches"], 0x0001))
    # parametric chains: k prefixes (repeated / alternating) before LD IX/IY,nn (effective = last prefix)
    for k in range(1, 9):
        for pattern in ([0xDD], [0xFD], [0xDD, 0xFD], [0xFD, 0xDD]):
            chain = [pattern[i % len(pattern)] for i in range(k)]
            out.append((chain + [0x21, 0x34, 0x12], k + 3, 4 * (k - 1) + 14, 4 * (k - 1) + 14, (k - 1) + 2, 0x0001))
    # prefix before ED (ignored) and before a DDCB form
    out.append(([0xDD, 0xED, 0x44], 3, 12, 12, 3, 0x0001))
    out.append(([0xFD, 0xDD, 0xCB, 0x00, 0x06], 5, 27, 27, 3, 0x0001))
    return out


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else "cc"
    root = checkout()
    if root is None:
        print("skipped: pinned Z80 oracle checkout unavailable (SEGARECOMP_Z80_ORACLE_CHECKOUT unset)")
        return 0
    text = "".join("%d %s %d %d %d %d %04x\n" % (len(bs), " ".join("%02x" % x for x in bs), ln, ta, tb, r, bc)
                   for bs, ln, ta, tb, r, bc in cases())
    with tempfile.TemporaryDirectory() as tmp:
        exe = pathlib.Path(tmp) / "crosscheck"
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", *DEFINES,
                        "-I%s" % (root / "redcode_Z80" / "API"), "-I%s" % (root / "redcode_Zeta" / "API"),
                        str(SOURCE), str(root / "redcode_Z80" / "sources" / "Z80.c"), "-o", str(exe)], check=True)
        run = subprocess.run([str(exe)], input=text, text=True, capture_output=True, timeout=120)
        sys.stdout.write(run.stdout)
        return run.returncode


if __name__ == "__main__":
    sys.exit(main())
