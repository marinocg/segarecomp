#!/usr/bin/env python3
"""SEG-008-T009: hermetic self-test of the SingleStepTests/z80 corpus driver (tools/z80_sst_corpus.py).

usage: z80_sst_corpus_test.py <z80_image_emitter> <cc> <product-root>

Uses a tiny SYNTHETIC cache in the corpus schema (hand-derived NOP / LD A,n / JR e cases, no downloaded data, no network):
  * a prefix-truncated corpus file keeps exactly its complete leading cases;
  * the generated-native side agrees with the hand-derived finals;
  * an injected fault in the expectation (register, R, PC, memory, port, T-states) is reported as the exact field;
  * an ED RETN case with the corpus `ei` marker 0 is a classified (not failing) disagreement on the marker only.
With SEGARECOMP_Z80_ORACLE_CHECKOUT set it also runs the primary and secondary oracles over the same cache.
"""
import copy
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
import z80_sst_corpus as sst  # noqa: E402

EMITTER, CC = sys.argv[1], sys.argv[2]
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)


BASE = {"a": 0x11, "b": 2, "c": 3, "d": 4, "e": 5, "f": 0xFF, "h": 6, "l": 7, "i": 0x12, "r": 0x80, "ei": 0, "wz": 0x1234,
        "ix": 0x4321, "iy": 0x8765, "af_": 0x1111, "bc_": 0x2222, "de_": 0x3333, "hl_": 0x4444, "im": 1, "p": 0, "q": 0,
        "iff1": 0, "iff2": 0, "sp": 0xF000}


def case(name, pc, ram, final_delta, cycles, extra_ram=()):
    ini = dict(BASE, pc=pc, ram=[[(pc + i) & 0xFFFF, b] for i, b in enumerate(ram)] + [list(x) for x in extra_ram])
    fin = {k: v for k, v in ini.items() if k not in ("ram",)}
    fin.update(final_delta)
    fin["ram"] = copy.deepcopy(ini["ram"])
    return {"name": name, "initial": ini, "final": fin, "cycles": [[pc, None, "----"]] * cycles}


def corpus():
    return {
        "00": [case("00 0000", 0x4000, [0x00], {"pc": 0x4001, "r": 0x81, "q": 0}, 4)],
        "3e": [case("3e 0000", 0xFFFF, [0x3E, 0x5A], {"pc": 0x0001, "r": 0x81, "a": 0x5A}, 7)],
        "18": [case("18 0000", 0x0200, [0x18, 0xFE], {"pc": 0x0200, "r": 0x81, "wz": 0x0200}, 12)],
        "ed 45": [case("ed 45 0000", 0x3000, [0xED, 0x45], {"pc": 0x1234, "r": 0x82, "wz": 0x1234, "sp": 0xF002}, 14,
                              extra_ram=[(0xF000, 0x34), (0xF001, 0x12)])],
    }


def write_cache(directory, docs):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "PINNED_COMMIT").write_text(sst.PIN + "\n")
    for name, cases in docs.items():
        (directory / (name + ".json")).write_text(json.dumps(cases))


def run_cli(cache, work, extra=()):
    cmd = [sys.executable, str(ROOT / "tools" / "z80_sst_corpus.py"), "run", "--emitter", EMITTER, "--cc", CC,
           "--cache", str(cache), "--work", str(work), "--json", str(work / "report.json"), *extra]
    r = subprocess.run(cmd, text=True, capture_output=True)
    report = json.loads((work / "report.json").read_text()) if (work / "report.json").exists() else None
    return r, report


# a truncated prefix keeps only complete leading cases
raw = json.dumps([{"name": "a 0", "x": [1, 2]}, {"name": "a 1", "x": [3]}, {"name": "a 2", "x": [4, 5]}]).encode()
check(len(sst.complete_cases(raw[:len(raw) - 5])) == 2, "prefix truncation keeps the complete leading cases")
check(sst.complete_cases(raw) and len(sst.complete_cases(raw)) == 3, "a complete document keeps every case")

with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    good = corpus()
    write_cache(tmp / "good", good)
    r, rep = run_cli(tmp / "good", tmp / "w_good")
    check(r.returncode == 0 and rep and rep["totals"]["cases"] == 4 and rep["totals"]["generated_fail"] == 0,
          "generated side agrees with the hand-derived corpus finals: " + (r.stdout + r.stderr)[-300:])
    check(rep and rep["classified_generated_side"].get("retx_deferral_marker", 0) >= 0, "classification recorded")

    faults = {
        "a": ("00", lambda c: c["final"].__setitem__("a", 0x12)),
        "r": ("3e", lambda c: c["final"].__setitem__("r", 0x83)),
        "pc": ("18", lambda c: c["final"].__setitem__("pc", 0x0201)),
        "memory": ("00", lambda c: c["final"]["ram"].append([0x5000, 1])),
        "io": ("00", lambda c: c.__setitem__("ports", [[0x10, 1, "w"]])),
        "timing": ("18", lambda c: c["cycles"].append([0, None, "----"])),
    }
    for field, (file_name, mutate) in faults.items():
        docs = copy.deepcopy(good)
        mutate(docs[file_name][0])
        write_cache(tmp / ("bad_" + field), docs)
        r, rep = run_cli(tmp / ("bad_" + field), tmp / ("w_" + field))
        seen = rep["generated_fields"] if rep else {}
        check(r.returncode == 1 and field in seen, "injected %s fault is reported (got %s)" % (field, seen))

    # the RETN corpus marker is classified, but a wrong PC on the same file still fails
    docs = copy.deepcopy(good)
    docs["ed 45"][0]["initial"].update({"iff1": 0, "iff2": 1})
    docs["ed 45"][0]["final"].update({"iff1": 1, "iff2": 1})
    write_cache(tmp / "retn", docs)
    r, rep = run_cli(tmp / "retn", tmp / "w_retn")
    check(r.returncode == 0 and rep["classified_generated_side"].get("retx_deferral_marker") == 1,
          "RETN deferral marker is a classified disagreement: " + (r.stdout + r.stderr)[-300:])
    docs["ed 45"][0]["final"]["pc"] = 0x1235
    write_cache(tmp / "retn_bad", docs)
    r, rep = run_cli(tmp / "retn_bad", tmp / "w_retn_bad")
    check(r.returncode == 1 and "pc" in rep["generated_fields"], "a wrong RETN PC still fails")

    if os.environ.get("SEGARECOMP_Z80_ORACLE_CHECKOUT"):
        r, rep = run_cli(tmp / "good", tmp / "w_oracle", ["--oracle", "--secondary"])
        if "skipped" in r.stdout:
            print(r.stdout.strip())
        else:
            check(r.returncode == 0 and rep["totals"]["oracle_fail"] == 0 and
                  rep["secondary_vs_corpus"]["unexplained_cases"] == 0,
                  "primary and secondary oracles agree with the synthetic corpus: " + (r.stdout + r.stderr)[-400:])
    else:
        print("skipped: pinned Z80 oracle checkout unavailable (oracle/secondary part)")

if FAILED:
    for f in FAILED:
        print("FAIL: " + f)
    sys.exit(1)
print("z80 sst corpus driver self-test: ok")
