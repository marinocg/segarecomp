#!/usr/bin/env python3
"""SEG-008-T009: SingleStepTests/z80 external-corpus falsification of generated-native Z80 execution.

The corpus (MIT, pinned in ADR 0057) is falsification input only: never committed, never bulk-fetched. `fetch` takes
a bounded byte prefix of every pinned per-opcode file (HTTP Range, complete cases only) into an ignored cache
(default `<product>/.tools/z80-oracles/sst-cache`). `run` converts every cached case to the T003 vector text, places the
case's own instruction bytes in banked code images at their real PC (per-vector code-image map, window-relative owners),
runs the generated-native runner and, when the pinned oracle is present, the redcode oracle on the identical text, and
compares each result stream with the corpus final state (registers, WZ/MEMPTR, Q, IFF, IM, R, EI marker, LD A,I marker,
RAM, port transactions and T-states = the corpus cycle count). Output is aggregated per opcode file and field;
`--json` writes only non-reconstructable aggregates.

  z80_sst_corpus.py fetch [--cache DIR] [--prefix-bytes N] [--only PATTERN]
  z80_sst_corpus.py run --emitter <z80_image_emitter> [--cache DIR] [--cc cc] [--work DIR] [--oracle] [--json OUT]
"""
import argparse
import concurrent.futures
import fnmatch
import json
import os
import pathlib
import sys
import tempfile
import urllib.parse
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import z80_conformance as zc  # noqa: E402

PIN = "ebe1875d48f374bcfd4b505d8eb8ee751568b5f7"
ROOT = pathlib.Path(__file__).resolve().parents[1]
CACHE_ENV = "SEGARECOMP_Z80_SST_CACHE"
TREE_URL = "https://api.github.com/repos/SingleStepTests/z80/git/trees/%s?recursive=1"
RAW_URL = "https://raw.githubusercontent.com/SingleStepTests/z80/%s/v1/%s"
MAX_CODE = 0x4000
CODE_WINDOW = 4  # longest single instruction (DD CB d op)


def default_cache():
    configured = os.environ.get(CACHE_ENV)
    if configured:
        return pathlib.Path(configured)
    checkout = os.environ.get(zc.CHECKOUT_ENV)
    base = pathlib.Path(checkout) if checkout else ROOT / ".tools" / "z80-oracles"
    return base / "sst-cache"


# ------------------------------------------------------------------------------------------------------- fetch
def http(url, headers=None):
    request = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def complete_cases(raw):
    """Parses the prefix of a JSON array of cases, keeping only the complete leading cases."""
    text = raw.decode("utf-8", "replace")
    decoder, cases, pos = json.JSONDecoder(), [], text.index("[") + 1
    while True:
        while pos < len(text) and text[pos] in ", \n\r\t":
            pos += 1
        try:
            case, pos = decoder.raw_decode(text, pos)
        except (ValueError, IndexError):
            return cases
        cases.append(case)


def fetch(cache, prefix_bytes, only):
    cache.mkdir(parents=True, exist_ok=True)
    tree = json.loads(http(TREE_URL % PIN))
    names = sorted(t["path"][3:-5] for t in tree["tree"] if t["path"].startswith("v1/") and t["path"].endswith(".json"))
    names = [n for n in names if not only or fnmatch.fnmatch(n, only)]

    def one(name):
        target = cache / (name + ".json")
        if target.exists():
            return name, len(json.loads(target.read_text()))
        url = RAW_URL % (PIN, urllib.parse.quote(name + ".json"))
        raw = http(url, {"Range": "bytes=0-%d" % (prefix_bytes - 1)})
        cases = complete_cases(raw)
        target.write_text(json.dumps(cases, separators=(",", ":")))
        return name, len(cases)

    total = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        for _, count in pool.map(one, names):
            total += count
    (cache / "PINNED_COMMIT").write_text(PIN + "\n")
    print("fetched %d files, %d cases (prefix %d bytes) pinned %s" % (len(names), total, prefix_bytes, PIN[:12]))


# --------------------------------------------------------------------------------------------------- conversion
def case_vector(case, file_name, offset):
    """Returns (vector text, code bytes, expected dict) for one corpus case, code placed at window offset `offset`."""
    ini, fin = case["initial"], case["final"]
    ram = {a: v for a, v in ini["ram"]}
    pc = ini["pc"]
    code = bytearray()
    while len(code) < CODE_WINDOW and ((pc + len(code)) & 0xFFFF) in ram:
        code.append(ram[(pc + len(code)) & 0xFFFF])
    name = (file_name + "_" + case["name"].split()[-1]).replace(" ", "_")
    af2, bc2, de2, hl2 = ini["af_"], ini["bc_"], ini["de_"], ini["hl_"]
    lines = ["V " + name,
             "R %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X" % (
                 ini["a"], ini["f"], ini["b"], ini["c"], ini["d"], ini["e"], ini["h"], ini["l"], af2 >> 8, af2 & 255,
                 bc2 >> 8, bc2 & 255, de2 >> 8, de2 & 255, hl2 >> 8, hl2 & 255),
             "P %04X %04X %04X %04X %04X" % (ini["ix"], ini["iy"], ini["sp"], pc, ini["wz"]),
             "X %X %X %X %X %X %X 0 %X %X 0 0" % (ini["i"], ini["r"], ini["im"], ini["iff1"], ini["iff2"], ini["q"],
                                                 ini["ei"], ini["p"])]
    for addr, value in sorted(ram.items()):
        lines.append("M %04X %02X" % (addr, value))
    ins = [v for _, v, kind in case.get("ports", []) if kind == "r"]
    if ins:
        lines.append("IN " + bytes(ins).hex())
    first = min(len(code), 0x10000 - pc)
    base = (pc - offset) & 0xFFFF
    lines.append("K 0 %X %X 1 %X" % (pc, pc + first, base))
    if first < len(code):  # logical fetch wraps past 0xFFFF
        lines.append("K 0 0 %X 1 %X" % (len(code) - first, (0 - (offset + first)) & 0xFFFF))
    lines += ["S i 1 0 0 map=0", "E"]
    ports = [("I" if kind == "r" else "O", port, value) for port, value, kind in case.get("ports", [])]
    expected = {"fin": fin, "ini_ram": ram, "ports": ports, "t": len(case["cycles"]), "name": name}
    return "\n".join(lines) + "\n", bytes(code), expected


def batches(cache, only):
    """Groups cases into image batches: [(batch name, image spec text, vector text, {name: expected})]."""
    cur_text, cur_expected, image, out = [], {}, bytearray(), []

    def flush():
        nonlocal cur_text, cur_expected, image
        if not cur_text:
            return
        # two identical windows make the owners window-relative (emit_image_set: relative iff several windows)
        spec = "image 1 banked\nwindow 1 0 0 %X\nwindow 1 8000 0 %X\nbytes 1 %s\n" % (
            len(image), len(image), bytes(image).hex())
        out.append(("sst%03d" % len(out), spec, "".join(cur_text), cur_expected))
        cur_text, cur_expected, image = [], {}, bytearray()

    for path in sorted(cache.glob("*.json")):
        if only and not fnmatch.fnmatch(path.stem, only):
            continue
        for case in json.loads(path.read_text()):
            text, code, expected = case_vector(case, path.stem, len(image))
            if len(image) + len(code) > MAX_CODE:
                flush()
                text, code, expected = case_vector(case, path.stem, 0)
            image += code
            cur_text.append(text)
            cur_expected[expected["name"]] = expected
    flush()
    return out


# --------------------------------------------------------------------------------------------------- comparison
def ex_state(fin):
    """The corpus final state in harness field names (hex text for value fields)."""
    def h(v, w):
        return "%0*X" % (w, v)
    af2, bc2, de2, hl2 = fin["af_"], fin["bc_"], fin["de_"], fin["hl_"]
    return {"a": h(fin["a"], 2), "f": h(fin["f"], 2), "b": h(fin["b"], 2), "c": h(fin["c"], 2), "d": h(fin["d"], 2),
            "e": h(fin["e"], 2), "h": h(fin["h"], 2), "l": h(fin["l"], 2), "a2": h(af2 >> 8, 2), "f2": h(af2 & 255, 2),
            "b2": h(bc2 >> 8, 2), "c2": h(bc2 & 255, 2), "d2": h(de2 >> 8, 2), "e2": h(de2 & 255, 2),
            "h2": h(hl2 >> 8, 2), "l2": h(hl2 & 255, 2), "ix": h(fin["ix"], 4), "iy": h(fin["iy"], 4),
            "sp": h(fin["sp"], 4), "pc": h(fin["pc"], 4), "wz": h(fin["wz"], 4), "i": h(fin["i"], 2),
            "r": h(fin["r"], 2), "im": str(fin["im"]), "iff1": str(fin["iff1"]), "iff2": str(fin["iff2"]),
            "q": h(fin["q"], 2), "deferral": str(fin["ei"]), "ldair": str(fin["p"])}


# Classified corpus/contract disagreements (never counted as a failure, always reported). The corpus `ei` field is the
# EI-deferral marker only: it is 0 after every ED RETN/RETI encoding, while the contract (docs/architecture/
# z80-cpu-contract.md section 4.2; Weissflog 2021, Sainz de Baranda 2022; ADR 0057 unresolved item 6) and the pinned
# oracle defer maskable INT by one boundary after an IFF1-changing RETI/RETN. Our internal bit is set whenever IFF1 was
# clear before the instruction (observably identical). The IFF/IM/PC/WZ/R/T-state fields still compare.
RETX_FILES = {"ed_%02x" % op for op in (0x45, 0x4D, 0x55, 0x5D, 0x65, 0x6D, 0x75, 0x7D)}
CLASSIFIED = {}


def diff_case(step, expected, count=True):
    """Differences between one result step and the corpus expectation: a list of field names."""
    bad = []
    masked = "deferral" if opcode_key(expected["name"]) in RETX_FILES else None
    for key, want in ex_state(expected["fin"]).items():
        if step["cpu"][key] != want:
            if key == masked:
                if count:
                    CLASSIFIED["retx_deferral_marker"] = CLASSIFIED.get("retx_deferral_marker", 0) + 1
                continue
            bad.append(key)
    ram = dict(expected["ini_ram"])
    for item in step["w"]:
        addr, value = item.split(":")
        ram[int(addr, 16)] = int(value, 16)
    final = {a: v for a, v in expected["fin"]["ram"]}
    if any(ram.get(a) != v for a, v in final.items()) or any(a not in final and ram[a] != expected["ini_ram"].get(a)
                                                            for a in ram):
        bad.append("memory")
    if [tuple(x) for x in expected["ports"]] != [(i.split(":")[0], int(i.split(":")[1], 16), int(i.split(":")[2], 16))
                                                 for i in step["io"]]:
        bad.append("io")
    if step["t"] != expected["t"]:
        bad.append("timing")
    return bad


def opcode_key(name):
    return name.rsplit("_", 1)[0]


def run(args):
    cache = pathlib.Path(args.cache) if args.cache else default_cache()
    if not (cache / "PINNED_COMMIT").is_file() or (cache / "PINNED_COMMIT").read_text().strip() != PIN:
        print("skipped: corpus cache missing or not at the pinned revision (%s)" % cache)
        return 0
    root = zc.oracle_checkout() if args.oracle else None
    if args.oracle and root is None:
        print("skipped: " + zc.skip_reason())
        return 0
    secondary_root = zc.secondary_checkout(os.environ.get(zc.CHECKOUT_ENV)) if args.secondary else None
    if args.secondary and secondary_root is None:
        print("skipped: pinned kosarev/z80 checkout unavailable")
        return 0
    tc = zc.Toolchain(args.cc, args.emitter, root)
    all_batches = batches(cache, args.only)
    totals = {"cases": 0, "generated_fail": 0, "oracle_fail": 0, "both_agree_fail": 0, "gen_only_fail": 0}
    by_file, fields, timing_only, sec_by_file = {}, {}, 0, {}
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(args.work) if args.work else pathlib.Path(tmp)
        work.mkdir(parents=True, exist_ok=True)
        oracle_exe = zc.build_oracle(tc, work) if root else None
        secondary_exe = zc.build_secondary(secondary_root, work) if secondary_root else None
        sec_fields, sec_cases = {}, 0
        for batch_name, spec, text, expected in all_batches:
            bdir = work / batch_name
            built = zc.build_generated(tc, spec, bdir, stem="z80_" + batch_name)
            if built["error"]:
                print("build error", batch_name, built["error"][:400])
                return 1
            generated = zc.run_exe(built["exe"], text, bdir)
            oracle = zc.run_exe(oracle_exe, text, bdir) if oracle_exe else None
            secondary = zc.run_exe(secondary_exe, text, bdir) if secondary_exe else None
            for name, exp in sorted(expected.items()):
                totals["cases"] += 1
                g_steps = generated.get(name)
                g_bad = diff_case(g_steps[0], exp) if g_steps else ["outcome"]
                if g_steps and g_steps[0]["out"] not in ("deadline", "halted", "prefix_lock"):
                    g_bad = ["outcome:" + g_steps[0]["out"]] + g_bad
                o_bad = diff_case(oracle[name][0], exp, count=False) if oracle else None
                if secondary is not None:
                    bad = [f for f in diff_case(secondary[name][0], exp, count=False) if f not in zc.SECONDARY_UNMODELLED]
                    if bad:
                        sec_cases += 1
                        key = opcode_key(name)
                        family = next((fam for fam, (files, allowed) in zc.SECONDARY_MASK.items()
                                       if key in files and set(bad) <= allowed), "unexplained")
                        rec = sec_by_file.setdefault(family, {})
                        rec[key] = rec.get(key, 0) + 1
                        for f in bad:
                            sec_fields[family + ":" + f] = sec_fields.get(family + ":" + f, 0) + 1
                if g_bad:
                    totals["generated_fail"] += 1
                    rec = by_file.setdefault(opcode_key(name), {"cases": 0, "generated": {}, "oracle": {}})
                    rec["cases"] += 1
                    for f in g_bad:
                        rec["generated"][f] = rec["generated"].get(f, 0) + 1
                        fields[f] = fields.get(f, 0) + 1
                    if o_bad is not None and o_bad:
                        totals["both_agree_fail"] += 1
                        for f in o_bad:
                            rec["oracle"][f] = rec["oracle"].get(f, 0) + 1
                    elif o_bad is not None:
                        totals["gen_only_fail"] += 1
                if o_bad:
                    totals["oracle_fail"] += 1
            print("batch %s: %d cases" % (batch_name, len(expected)), flush=True)
    report = {"pin": PIN, "totals": totals, "classified_generated_side": dict(CLASSIFIED), "generated_fields": fields, "files_with_disagreement": len(by_file),
              "by_file": by_file}
    if secondary_exe:
        report["secondary_vs_corpus"] = {"cases_with_difference": sec_cases, "fields": sec_fields,
                                         "by_family": {k: {"files": len(v), "cases": sum(v.values())} for k, v in sorted(sec_by_file.items())},
                                         "unexplained_cases": sum(sec_by_file.get("unexplained", {}).values())}
        totals["secondary_unexplained"] = sum(sec_by_file.get("unexplained", {}).values())
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(report, indent=1, sort_keys=True) + "\n")
    print(json.dumps({k: report[k] for k in ("pin", "totals", "classified_generated_side", "generated_fields", "files_with_disagreement", "secondary_vs_corpus") if k in report}, sort_keys=True))
    return 1 if totals["generated_fail"] or totals.get("secondary_unexplained") else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("--cache")
    f.add_argument("--prefix-bytes", type=int, default=98304)
    f.add_argument("--only")
    r = sub.add_parser("run")
    r.add_argument("--emitter", required=True)
    r.add_argument("--cache")
    r.add_argument("--cc", default="cc")
    r.add_argument("--work")
    r.add_argument("--oracle", action="store_true")
    r.add_argument("--secondary", action="store_true", help="also run the pinned kosarev/z80 secondary oracle")
    r.add_argument("--json")
    r.add_argument("--only")
    args = parser.parse_args()
    if args.cmd == "fetch":
        fetch(pathlib.Path(args.cache) if args.cache else default_cache(), args.prefix_bytes, args.only)
        return 0
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
