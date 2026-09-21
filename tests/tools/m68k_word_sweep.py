#!/usr/bin/env python3
"""Pinned-Musashi 68000 primary-word sweep and manual-dataset cross-check (SEG-021-T001).

Overlap/hole cross-check ONLY. It compares the 65,536 primary opcode words of the manual
dataset (tests/fixtures/m68k-legal-forms.json, derived by tools/m68k_legal_forms.py from
public documentation) with what the pinned Musashi core, configured as a 68000, does with
each word from a fixed recorded state. It never infers extension-word or EA-form legality and
is never a coverage denominator. Development/test oracle only.

Usage: m68k_word_sweep.py --compiler CC [--write-fixture]   (needs SEGARECOMP_M68K_MULS_WORD_MUSASHI_CHECKOUT)
"""
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
CHECKOUT_ENV = "SEGARECOMP_M68K_MULS_WORD_MUSASHI_CHECKOUT"
RUNNER = ROOT / "tests" / "tools" / "m68k_word_sweep_musashi_runner.c"
FORMS = ROOT / "tests" / "fixtures" / "m68k-legal-forms.json"
DISAGREEMENTS = ROOT / "tests" / "fixtures" / "m68k-word-sweep-disagreements.json"

SWEEP_CONFIG = {
    "cpu_type": "M68K_CPU_TYPE_68000",
    "musashi_revision": PIN,
    "data_registers": "D0-D7 = 0x00000000",
    "address_registers": "A0-A6 = 0x00002000",
    "stack_pointers": "ISP = 0x00004000, USP = 0x00006000 (SR written without stack swap)",
    "supervisor_run_sr": "0x2700",
    "user_run_sr": "0x0700",
    "opcode_pc": "0x00001000",
    "extension_bytes": "all 0x00 (every byte other than the opcode word reads 0x00; writes discarded)",
    "vectors": "vector v (2..255) -> 0x00008000 + 8*v; reset vector SSP=0x4000 PC=0x1000",
    "cycles_per_word": 1,
    "outcome": "exception vector whose handler address the PC reached after one instruction, or none",
}

# Vector numbers a legal form may itself raise (instruction-defined exceptions in the dataset).
EXCEPTION_VECTORS = {
    "illegal_vector_4": (4,), "zero_divide_vector_5": (5,), "chk_vector_6": (6,),
    "trapv_vector_7": (7,), "trap_vector_32_47": tuple(range(32, 48)), "address_error_vector_3": (3,),
}

# Explained disagreements: (first, last, musashi (supervisor, user), category, resolution).
EXPLAINED = [
    (0xF620, 0xF627, ("none", "none"), "musashi_implementation_quirk",
     "0xF620-0xF627 is the 68040 MOVE16 (Ax)+,(Ay)+ encoding. Musashi's move16 handler carries no CPU-type "
     "guard, so the 68000 core executes it instead of raising line-F vector 11. The manual partition "
     "(line-F reserved exception on a base MC68000) is correct; Musashi is wrong for this range only."),
]


def _run(args, **kw):
    r = subprocess.run(args, text=True, capture_output=True, **kw)
    assert r.returncode == 0, (args, r.stderr[-2000:])
    return r


def _env():
    e = os.environ.copy()
    if not e.get("SDKROOT") and shutil.which("xcrun"):
        e["SDKROOT"] = _run(["xcrun", "--show-sdk-path"]).stdout.strip()
    return e


def checkout():
    """Pinned checkout path, or None when unavailable (callers skip cleanly)."""
    configured = os.environ.get(CHECKOUT_ENV)
    if not configured:
        return None
    path = pathlib.Path(configured)
    if not (path / "m68k_in.c").is_file():
        return None
    head = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], text=True, capture_output=True)
    if head.returncode != 0 or head.stdout.strip() != PIN:
        raise AssertionError("Musashi checkout is not the pinned revision %s" % PIN)
    dirty = subprocess.run(["git", "-C", str(path), "diff", "--quiet", "HEAD", "--"])
    assert dirty.returncode == 0, "pinned Musashi checkout has local modifications"
    return path


def sweep(compiler, chk):
    """Returns {word: (supervisor, user)} with outcomes 'none' or a vector number string."""
    with tempfile.TemporaryDirectory() as directory:
        temp = pathlib.Path(directory)
        env = _env()
        gen = temp / "m68kmake"
        _run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(chk / "m68kmake.c"),
              "-o", str(gen)], env=env)
        shutil.copyfile(chk / "m68k_in.c", temp / "m68k_in.c")
        _run([str(gen)], cwd=temp, env=env)
        exe = temp / "sweep"
        _run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-error=unused-variable", "-pedantic",
              "-I", str(temp), "-I", str(chk), "-I", str(chk / "softfloat"), str(RUNNER),
              str(chk / "m68kcpu.c"), str(temp / "m68kops.c"), str(chk / "softfloat/softfloat.c"),
              "-o", str(exe)], env=env)
        out = _run([str(exe)]).stdout.splitlines()
    result = {}
    for line in out:
        w, s, u = line.split()
        result[int(w, 16)] = ("none" if s == "-" else s, "none" if u == "-" else u)
    assert sorted(result) == list(range(0x10000)), "sweep did not cover all 65,536 words"
    return result


def manual_model():
    """word -> (class letter, allowed vectors) from the dataset fixture + its derivation rows."""
    data = json.loads(FORMS.read_text())
    rows = data["primary_word_partition"]["rows"]
    sys.path.insert(0, str(ROOT / "tools"))
    import m68k_legal_forms as forms  # test-side only: the dataset tool, never production code
    forms.derive()
    allowed = {}
    for r in forms.ROWS:
        vecs = set()
        for e in r["exceptions"]:
            vecs.update(EXCEPTION_VECTORS.get(e, ()))
        if vecs:
            for w in r["_words"]:
                allowed[w] = vecs
    return (lambda w: rows[w >> 8][w & 255]), allowed, data


def compare(result):
    """Returns (disagreements, matrix). A disagreement is a word whose Musashi behaviour is
    inconsistent with its manual class (exception-aware for legal forms)."""
    cls, allowed, data = manual_model()
    legend = data["primary_word_partition"]["legend"]
    dis, matrix = [], {}
    for w in range(0x10000):
        c, (s, u) = cls(w), result[w]
        vecs = allowed.get(w, set())
        if c in "LP":
            ok_s = s == "none" or int(s) in vecs
            ok_u = (u == "8") if c == "P" else (u == "none" or int(u) in vecs)
            if c == "P":
                ok_s = ok_s and s != "8"
            agree = ok_s and ok_u
        elif c == "A":
            agree = (s, u) == ("10", "10")
        elif c == "F":
            agree = (s, u) == ("11", "11")
        else:
            agree = (s, u) == ("4", "4")
        key = "%s|%s|%s" % (legend[c], s if s == "none" or not s.isdigit() or not 32 <= int(s) <= 47 else "32-47",
                            u if u == "none" or not u.isdigit() or not 32 <= int(u) <= 47 else "32-47")
        matrix[key] = matrix.get(key, 0) + 1
        if not agree:
            dis.append((w, legend[c], s, u))
    return dis, dict(sorted(matrix.items()))


def explain(dis):
    """Group disagreements into explained ranges; anything unmatched is 'unexplained'."""
    entries, unexplained = [], []
    covered = set()
    for first, last, outcome, category, resolution in EXPLAINED:
        members = [d for d in dis if first <= d[0] <= last and (d[2], d[3]) == outcome]
        if not members:
            continue
        covered.update(d[0] for d in members)
        entries.append({
            "first_word": "%04X" % first, "last_word": "%04X" % last, "words": len(members),
            "manual_class": members[0][1], "musashi_supervisor_vector": outcome[0], "musashi_user_vector": outcome[1],
            "category": category, "resolution": resolution,
        })
    unexplained = [d for d in dis if d[0] not in covered]
    return entries, unexplained


def build_fixture(result):
    dis, matrix = compare(result)
    entries, unexplained = explain(dis)
    assert not unexplained, "unexplained Musashi-vs-manual disagreements: %r" % unexplained[:8]
    return {
        "schema": 1,
        "dataset": "m68k-word-sweep-disagreements",
        "scope": "primary opcode words only; not evidence of extension-word or EA-form legality",
        "sweep_config": SWEEP_CONFIG,
        "outcome_matrix": matrix,
        "disagreements": entries,
        "unexplained_disagreements": 0,
        "notes": "68010+/68020+ words are class illegal_post_68000_encoding and must raise vector 4 on the 68000 "
                 "core; every such word agrees. Legal forms may raise their own defined exceptions (ILLEGAL=4, "
                 "DIVU/DIVS zero divisor=5, TRAP=32-47) and are treated as agreement.",
    }


def render(fixture):
    return json.dumps(fixture, indent=1, sort_keys=True) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default="cc")
    ap.add_argument("--write-fixture", action="store_true")
    args = ap.parse_args(argv)
    chk = checkout()
    if chk is None:
        print("skipped: %s not set to the pinned Musashi checkout" % CHECKOUT_ENV)
        return 0
    fixture = build_fixture(sweep(args.compiler, chk))
    if args.write_fixture:
        DISAGREEMENTS.write_bytes(render(fixture).encode("utf-8"))
    print("disagreements recorded: %d range(s), unexplained 0" % len(fixture["disagreements"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
