#!/usr/bin/env python3
"""SEG-021-T003: acceptance tests of the table-driven generated-native differential conformance harness.

usage: m68k_conformance_harness_test.py <m68k_conformance_emitter> <cc>

Always runs (no oracle needed): table/manifest consistency, deterministic vector expansion, the emit ->
strict-C11 -> native pipeline and its determinism, unsupported-form reporting.
Runs only with the pinned Musashi checkout (any of the harness's checkout environment variables): the oracle
comparison, the injected-fault first-divergence reports, and the full committed table. Without the pin the
oracle claims are SKIPPED, never failed.
"""
import copy
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import m68k_conformance as mc  # noqa: E402

FAILURES = []


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print("FAIL:", message)


def force_function(source, code, old, new):
    """Test-only fault injection into a temporary copy of ONE emitted function."""
    start = source.index("static int cf_%s(" % code)
    end = source.index("static int cf_", start + 10) if "static int cf_" in source[start + 10:] else len(source)
    body = source[start:end]
    assert old in body, (code, old)
    return source[:start] + body.replace(old, new, 1) + source[end:]


# --- SEG-021-T021: outcome-dependent timing -------------------------------------------------------------
# Test-owned transcription of the published MC68000 User's Manual section 8 rows (Table 8-10 Bcc/DBcc, Table 8-6
# Scc Dn, Table 8-9 register shift/rotate) and of the Bcc/DBcc/Scc condition tests (Programmer's Reference
# Manual table 3-19); independent of the production timing owner and of Musashi.
def condition_true(cond, sr):
    c, v, z, n = sr & 1, (sr >> 1) & 1, (sr >> 2) & 1, (sr >> 3) & 1
    return {"t": True, "f": False, "hi": not c and not z, "ls": bool(c or z), "cc": not c, "cs": bool(c),
            "ne": not z, "eq": bool(z), "vc": not v, "vs": bool(v), "pl": not n, "mi": bool(n), "ge": n == v,
            "lt": n != v, "gt": n == v and not z, "le": bool(z) or n != v}[cond]


def published_cycles(row_id, vector):
    parts = row_id.split(".")
    if parts[0] == "bra":
        return 10
    if parts[0] == "bcc":
        return 10 if condition_true(parts[-1], vector["sr"]) else (8 if parts[2] == "b" else 12)
    if parts[0] == "dbcc":
        if condition_true(parts[-1], vector["sr"]):
            return 12
        return 14 if vector["x"] & 0xFFFF == 0 else 10
    if parts[0] == "scc":
        return 6 if condition_true(parts[-1], vector["sr"]) else 4
    base = 8 if parts[2] == "l" else 6
    count = vector["x"] & 63 if parts[1] == "dn_dn" else (((vector["word"] >> 9) & 7) or 8)
    return base + 2 * count


TIMING_SAMPLE = ["asl.dn_dn.b.dn.dn", "bcc.disp16.w.none.target.lt", "bcc.disp8.b.none.target.eq",
                 "bra.disp8.b.none.target", "dbcc.dn_disp16.w.dn.target.f", "dbcc.dn_disp16.w.dn.target.ne",
                 "dbcc.dn_disp16.w.dn.target.t", "lsr.dn_dn.w.dn.dn", "ror.imm_dn.w.count1to8.dn.count1to8",
                 "roxl.dn_dn.l.dn.dn", "roxr.imm_dn.l.count1to8.dn.count1to8", "scc.unary.b.none.dn.hi"]


# SEG-021-T022: every conformance row is a timing row except these, whose published row the pinned Musashi core
# does not reproduce (documented in docs/testing/m68k-conformance-harness.md, SEG-021-T022 audit): register-direct /
# immediate long ALU and ADDA/SUBA rows (Table 8-4 "**" 8-clock rule; Musashi 6), byte/word #<data>,Dn ALU and
# ADDA/SUBA.W #<data> (Musashi +2), ADDQ.W #,An (Table 8-5: 8; Musashi 4), ANDI.L #,Dn (Table 8-5: 16; Musashi 14),
# memory-bound CHK traps (Table 8-14: 40 + EA; Musashi 40), TAS memory (Table 8-6: 10 + EA; Musashi 14 + EA) and the
# MULS rows whose shared profile has positive sources (Table 8-4 n; Musashi n - 1; their `.timing` siblings cover the
# forms with zero/negative sources).
TIMING_ORACLE_DEVIATION_ROWS = {
    "add.ea_dn.b.imm.dn", "add.ea_dn.l.dn.dn", "add.ea_dn.w.imm.dn", "adda.ea_an.l.an.an", "adda.ea_an.l.dn.an",
    "adda.ea_an.w.imm.an", "addq.quick_ea.w.quick.an.quick1to8", "and.ea_dn.b.imm.dn", "and.ea_dn.l.dn.dn",
    "and.ea_dn.w.imm.dn", "andi.imm_ea.l.imm.dn", "chk.ea_dn.w.absl.dn", "chk.ea_dn.w.absw.dn",
    "chk.ea_dn.w.disp.dn", "chk.ea_dn.w.imm.dn", "chk.ea_dn.w.ind.dn", "chk.ea_dn.w.index.dn",
    "chk.ea_dn.w.pcdisp.dn", "chk.ea_dn.w.pcindex.dn", "chk.ea_dn.w.postinc.dn", "chk.ea_dn.w.predec.dn",
    "muls.ea_dn.w.absl.dn", "muls.ea_dn.w.absw.dn", "muls.ea_dn.w.disp.dn", "muls.ea_dn.w.dn.dn",
    "muls.ea_dn.w.imm.dn", "muls.ea_dn.w.ind.dn", "muls.ea_dn.w.index.dn", "muls.ea_dn.w.pcdisp.dn",
    "muls.ea_dn.w.pcindex.dn", "muls.ea_dn.w.postinc.dn", "muls.ea_dn.w.predec.dn", "or.ea_dn.b.imm.dn",
    "or.ea_dn.l.dn.dn", "or.ea_dn.w.imm.dn", "sub.ea_dn.b.imm.dn", "sub.ea_dn.l.dn.dn", "sub.ea_dn.w.imm.dn",
    "suba.ea_an.l.an.an", "suba.ea_an.l.dn.an", "suba.ea_an.w.imm.an", "tas.unary.b.none.absl",
    "tas.unary.b.none.absw", "tas.unary.b.none.disp", "tas.unary.b.none.ind", "tas.unary.b.none.index",
    "tas.unary.b.none.postinc", "tas.unary.b.none.predec"}


def timing_checks(emitter, cc, table, forms, checkout, scratch):
    timed = [r for r in table["rows"] if r.get("timing")]
    untimed = {r["id"] for r in table["rows"] if not r.get("timing")}
    check(untimed == TIMING_ORACLE_DEVIATION_ROWS,
          "every row is a timing row except the documented oracle deviations: %s" % sorted(
              untimed ^ TIMING_ORACLE_DEVIATION_ROWS))
    families = {r["id"].split(".")[0] for r in timed}
    check({"bcc", "bra", "dbcc", "scc", "asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr", "mulu", "muls",
           "trap", "trapv", "chk", "illegal", "stop", "rte", "tst", "move", "cmpa", "bclr", "btst"} <= families,
          "timing rows cover the outcome-dependent, MUL, exception and audited families: %s" % sorted(families))
    check(len([r for r in timed if r["id"].split(".")[0] in ("asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr")
               and r["id"].split(".")[1] in ("dn_dn", "imm_dn")])
          == 48, "every register shift/rotate family x {Dn count, immediate count} x {B, W, L} is a timing row")
    shift_counts = {int(x, 16) for x, _ in table["profiles"]["shift_count"]["pairs"]}
    check({0, 1, 8, 63} <= {c & 63 for c in shift_counts} and any(c > 63 for c in shift_counts),
          "shift count vectors must include 0/1/8/63 and a count register above 63 (modulo 64)")
    by_id = {r["id"]: r for r in table["rows"]}
    vectors = [v for rid in TIMING_SAMPLE for v in mc.expand_row(by_id[rid], table, forms)]
    generated, status, deterministic = mc.run_generated(vectors, emitter, cc, scratch / "timing")
    check(deterministic and all(s == "ok" for s in status.values()), "timing sample emits, compiles and runs")
    outcomes = {}
    for v in vectors:
        expected = published_cycles(v["row"], v)
        outcomes.setdefault(v["row"], set()).add(expected)
        got = generated[v["id"]].get("cycles")
        if got != expected:
            check(False, "generated cycles %s != published %s for %s" % (got, expected, v["id"]))
            break
    check(outcomes["bcc.disp8.b.none.target.eq"] == {10, 8} and outcomes["bcc.disp16.w.none.target.lt"] == {10, 12},
          "Bcc taken and not-taken outcomes (byte and word) are exercised")
    check(outcomes["dbcc.dn_disp16.w.dn.target.ne"] == {12, 10, 14} and outcomes["dbcc.dn_disp16.w.dn.target.f"] == {10, 14}
          and outcomes["dbcc.dn_disp16.w.dn.target.t"] == {12}, "DBcc true / expired / branch-taken outcomes exercised")
    check({8 + 2 * n for n in (0, 1, 8, 63)} <= outcomes["roxl.dn_dn.l.dn.dn"] and
          {6 + 2 * n for n in (0, 1, 8, 63)} <= outcomes["asl.dn_dn.b.dn.dn"], "shift counts 0/1/8/63 exercised")
    # fail closed: a timing row whose generated function reports no timing rule is unsupported (SEG-021-T022: every
    # decodable form now has a rule, so the missing rule is injected into one function of a temporary copy)
    rep = mc.run(table, forms, emitter, cc, None, scratch / "timing-norule", ["bcc.disp8.b.none.target.eq"],
                 mutate=lambda s: force_function(s, "6702", "  cf_cycles = ", "  (void)"))
    check(rep["rows"][0]["status"] == "unsupported" and rep["rows"][0]["passing_words"] == rep["rows"][0]["words"] - 1,
          "a timing row without a timing rule must be reported unsupported: %s" % rep["rows"][0]["status"])
    if checkout is None:
        print("pinned Musashi unavailable: timing oracle cross-check SKIPPED")
        return
    oracle = mc.run_oracle(vectors, checkout, cc, scratch / "timing-oracle")
    bad = [v["id"] for v in vectors if oracle[v["id"]].get("cycles") != published_cycles(v["row"], v)]
    check(not bad, "pinned Musashi cycle report must agree with the published tables: %s" % bad[:5])
    rep = mc.run(table, forms, emitter, cc, checkout, scratch / "timing-run", TIMING_SAMPLE)
    check(all(r["status"] == "validated" and r["counts"]["timing_compared"] == r["counts"]["compared"] > 0
              and r["credit"]["timing"] for r in rep["rows"]), "timing sample validates against Musashi: %s" % [
                  (r["row"], r["status"], r["first_divergence"]) for r in rep["rows"] if r["status"] != "validated"])
    # injected timing fault: BEQ.S not-taken retires 10 instead of 8 -> only word 6702 diverges, domain timing
    faulty = mc.run(table, forms, emitter, cc, checkout, scratch / "timing-fault", ["bcc.disp8.b.none.target.eq"],
                    mutate=lambda s: force_function(s, "6702", "UINT32_C(8)", "UINT32_C(10)"))
    r = faulty["rows"][0]
    check(r["status"] == "diverged" and r["passing_words"] == r["words"] - 1 and r["first_divergence"]["domain"] == "timing"
          and r["first_divergence"]["fields"][0]["field"] == "cycles", "timing fault must fail exactly word 6702: %s" % r)
    check(not mc.credited_words(faulty, forms, table)["timing"], "a timing-diverging row must credit nothing")


# --- SEG-021-T022: MUL, exception-entry timing and the documented oracle deviations ------------------------
def mulu_n(source):
    return bin(source & 0xFFFF).count("1")


def muls_n(source):  # Table 8-4: 01/10 pairs of <source word>:0, walked explicitly
    n, previous = 0, 0
    for bit in range(16):
        current = (source >> bit) & 1
        n += current != previous
        previous = current
    return n


def t022_checks(emitter, cc, table, forms, checkout, scratch):
    pairs = lambda name: [int(x, 16) for x, _ in table["profiles"][name]["pairs"]]
    check({mulu_n(x) for x in pairs("mulu_timing")} == set(range(17)), "MULU timing sources cover n = 0..16")
    check({muls_n(x) for x in pairs("muls_timing")} == {0} | set(range(1, 16, 2)) and
          all(x & 0xFFFF == 0 or x & 0x8000 for x in pairs("muls_timing")),
          "MULS timing sources are zero or negative and cover n = 0 and every odd n (the oracle-comparable half)")
    by_id = {r["id"]: r for r in table["rows"]}
    # MULS positive sources: the generated rule follows Table 8-4 (every n, even n included); the pinned Musashi core
    # stops counting when the remaining source bits are zero, so it misses the final 1 -> 0 pair of every positive
    # nonzero source and reports exactly 2 cycles less (documented deviation, pinned here so it cannot drift).
    positive = copy.deepcopy(table)
    positive["profiles"]["muls_positive"] = {"kind": "pair_list", "sr": ["2700"], "pairs": [
        ["%08X" % x, "00000003"] for x in (0x0001, 0x0003, 0x5555, 0x4000, 0x7FFF, 0x0F0F, 0x1234)]}
    row = dict(by_id["muls.ea_dn.w.dn.dn.timing"], profile="muls_positive")
    vectors = mc.expand_row(row, positive, forms)
    generated, status, deterministic = mc.run_generated(vectors, emitter, cc, scratch / "muls-positive")
    check(deterministic and all(s == "ok" for s in status.values()), "MULS positive-source vectors emit and run")
    check(all(generated[v["id"]].get("cycles") == 38 + 2 * muls_n(v["x"]) for v in vectors),
          "generated MULS cycles follow Table 8-4 for positive sources")
    # Exception-entry timing on the direct route: a taken CHK trap reports the entry (40 + the bound's EA cell),
    # the retiring path its row; TRAP / line A report 34.
    chk_rows = ["chk.ea_dn.w.dn.dn", "chk.ea_dn.w.ind.dn"]
    chk_vectors = [v for rid in chk_rows for v in mc.expand_row(by_id[rid], table, forms)]
    chk_generated, _, _ = mc.run_generated(chk_vectors, emitter, cc, scratch / "chk-entry")
    def chk_expected(v):
        memory = v["row"].endswith(".ind.dn")
        value, bound = mc.sext(v["x"] & 0xFFFF, 16), mc.sext(v["y"] & 0xFFFF, 16)  # x = Dn, y = the bound
        trapped = value < 0 or value > bound
        return (40 if trapped else 10) + (4 if memory else 0), trapped
    outcomes = {(v["row"], chk_expected(v)[1]) for v in chk_vectors}
    check(len(outcomes) == 4, "CHK Dn and (An) rows exercise both the trap and the in-range path: %s" % outcomes)
    check(all(chk_generated[v["id"]].get("cycles") == chk_expected(v)[0] for v in chk_vectors),
          "CHK reports 10/14 in range and the 40 + EA entry when the trap is taken")
    if checkout is None:
        print("pinned Musashi unavailable: T022 oracle deviation pins SKIPPED")
        return
    oracle = mc.run_oracle(vectors, checkout, cc, scratch / "muls-positive-oracle")
    check(all(oracle[v["id"]].get("cycles") == 38 + 2 * muls_n(v["x"]) - 2 for v in vectors),
          "documented deviation: the pinned Musashi reports Table 8-4 - 2 for every positive nonzero MULS source")
    # Musashi undoes the whole instruction row (EA included) on a CHK trap: 40 flat instead of 40 + EA.
    chk_oracle = mc.run_oracle(chk_vectors, checkout, cc, scratch / "chk-entry-oracle")
    check(all(chk_oracle[v["id"]].get("cycles") == (40 if chk_expected(v)[1] else chk_expected(v)[0])
              for v in chk_vectors),
          "documented deviation: the pinned Musashi charges a taken CHK trap 40 regardless of the bound's EA")


def main():
    emitter, cc = pathlib.Path(sys.argv[1]), sys.argv[2]
    table, forms = mc.load_table(), mc.load_forms()
    checkout = mc.musashi_checkout(None)

    # --- table shape and manifest consistency (static, no oracle) -------------------------------
    ids = [r["id"] for r in table["rows"]]
    check(ids == sorted(ids) and len(ids) == len(set(ids)), "table rows must be unique and sorted")
    a = [mc.expand_row(r, table, forms) for r in table["rows"]]
    b = [mc.expand_row(r, table, forms) for r in table["rows"]]
    check(a == b and sum(map(len, a)) > 50000, "vector expansion must be deterministic and non-trivial")
    for vectors in a:
        check(all(len(v["mem"]) <= 2 for v in vectors), "bounded memory seeds")
    manifest = json.loads(mc.MANIFEST.read_text(encoding="utf-8"))
    declared = mc.declared_words(table, forms)
    for aspect, key in mc.MANIFEST_KEYS.items():
        tagged = {int(w, 16) for w, s in manifest[key].items() if mc.SOURCE_TAG in s}
        check(tagged == declared[aspect], "manifest %s words attributed to the conformance table must equal the table's "
              "declared words (only a passing Musashi run may add them)" % key)
    check(mc.SOURCE_TAG in manifest["sources"], "manifest must list the conformance table as a source")

    # --- boundary coverage the table promises ---------------------------------------------------
    row = next(r for r in table["rows"] if r["id"] == "add.ea_dn.w.dn.dn")
    vectors = mc.expand_row(row, table, forms)
    check({v["sr"] for v in vectors} >= {0x2700, 0x2710, 0x271F}, "X/C interaction states must be present")
    check(any(v["code"] == "D643" and v["x"] == v["y"] for v in vectors), "register aliasing (Dn,Dn same register) present")
    a7 = mc.expand_row(next(r for r in table["rows"] if r["id"] == "neg.unary.b.none.predec"), table, forms)
    check(any(v["code"] == "4427" for v in a7), "A7 byte auto-update word present")

    # --- expansion controls for shapes the production pipeline may not support yet (never credited) ----------
    def expand(fid, **row):
        r = {"id": "control:" + fid, "form": fid, "profile": "unary_sweep"}
        r.update(row)
        return mc.expand_row(r, table, forms)

    def seeds(v):
        return {a: p.hex().upper() for a, p in v["mem"]}

    # two auto-update operands (CMPM (Ay)+,(Ax)+): both memory operands seeded at their own An, aliasing collapses
    cmpm = expand("cmpm.postinc_postinc.w.postinc.postinc", bind={"x": "pi@0", "y": "pi@9"}, profile="alu_sweep")
    v = next(v for v in cmpm if v["code"] == "B348" and v["x"] == 0xFF and v["y"] == 0x01)  # CMPM.W (A0)+,(A1)+
    check(seeds(v) == {0x10000: "00FF", 0x11000: "0001"}, "two auto-update operands: %s" % seeds(v))
    check(all(x["x"] == x["y"] for x in cmpm if x["code"] == "B349"), "aliased (A1)+,(A1)+ keeps only x == y states")
    addx = expand("addx.predec_predec.b.predec.predec", bind={"x": "pd@0", "y": "pd@9"}, profile="alu_sweep")
    v = next(v for v in addx if v["code"] == "D10F" and v["x"] == 0xFF and v["y"] == 0x01)  # ADDX.B -(A7),-(A0)
    check(seeds(v) == {0x16FFE: "FF", 0xFFFF: "01"}, "A7 byte predecrement adjusts by two: %s" % seeds(v))
    # displacement, brief-indexed, absolute, PC-relative EA classes taken from literal extension data
    v = next(v for v in expand("neg.unary.w.none.disp", bind={"x": "ea.dst"}, ext=["FFF0"]) if v["code"] == "4469FFF0")
    check(list(seeds(v)) == [0x11000 - 16], "d16(An) address = An + sext(disp), code = primary + suffix: %s" % seeds(v))
    v = expand("neg.unary.w.none.index", bind={"x": "ea.dst"}, ext=["1804"], init={"d1": "00000010"})
    v = next(v for v in v if v["code"] == "4472" + "1804")
    check(list(seeds(v)) == [0x12000 + 0x10 + 4], "d8(An,Xn) uses the Xn preset and d8: %s" % seeds(v))
    v = expand("neg.unary.w.none.absw", bind={"x": "ea.dst"}, ext=["2000"])[0]
    check(list(seeds(v)) == [0x2000], "absolute.W")
    v = expand("divu.ea_dn.w.absl.dn", bind={"x": "ea.src"}, ext=["00012000"])[0]
    check(list(seeds(v)) == [0x12000] and v["code"].endswith("00012000"), "absolute.L")
    v = expand("divu.ea_dn.w.pcdisp.dn", bind={"x": "ea.src"}, ext=["0010"])[0]
    check(list(seeds(v)) == [0x2000 + 2 + 0x10], "PC-relative displacement is relative to the extension word address")
    v = expand("divu.ea_dn.w.pcindex.dn", bind={"x": "ea.src"}, ext=["0004"], init={"d0": "00000010"})[0]
    check(list(seeds(v)) == [0x2000 + 2 + 0x10 + 4], "PC-relative brief indexed")
    # immediate: literal suffix data, no state bound; extension-bearing no-operand controls
    v = expand("divu.ea_dn.w.imm.dn", bind={"x": "d@9"}, ext=["0003"])
    check(v and all(x["code"].endswith("0003") and not x["mem"] for x in v), "immediate operand lives in the suffix")
    for fid, ext in (("stop.imm16.none.imm.none", ["2700"]), ("link.an_disp16.w.an.disp16", ["FFF8"]),
                     ("movem.reglist_mem.w.reglist.ind", ["00FF"]), ("bra.disp16.w.none.target", ["0010"]),
                     ("trap.vector.none.none.none.vector0to15", [""])):
        got = expand(fid, ext=ext, profile="state_modes")
        check(got and all(x["code"].endswith(ext[0]) and not x["mem"] for x in got), "no-operand row %s" % fid)
    # explicit stack state: supervisor A7 = SSP, user A7 = USP, both carried separately
    for x in expand("nop.none.none.none.none", profile="state_modes"):
        check(x["a"][7] == (x["ssp"] if x["sr"] & 0x2000 else x["usp"]) and x["usp"] != x["ssp"], "explicit stack state")
    check({x["sr"] & 0x2000 for x in expand("nop.none.none.none.none", profile="state_modes")} == {0, 0x2000},
          "both supervisor and user initial states are representable")
    # the committed canaries are ordinary rows; the tool has no per-mnemonic logic
    for canary in ("neg.unary.w.none.disp", "addi.imm_ea.w.imm.dn", "nop.none.none.none.none"):
        check(canary in ids, "canary row %s missing" % canary)
    tool_text = (ROOT / "tools" / "m68k_conformance.py").read_text().lower()
    check(not any(re.search(r"\b%s\b" % m, tool_text) for m in ("nop", "addi", "movem", "neg")),
          "harness code must hold no per-mnemonic logic")

    small = ["add.ea_dn.w.dn.dn", "neg.unary.w.none.postinc", "neg.unary.b.none.predec", "addq.quick_ea.l.quick.ind.quick1to8"]
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        # --- synthetic self-consistency, no oracle -------------------------------------------------
        report = mc.run(table, forms, emitter, cc, None, scratch / "synthetic", small)
        check(report["musashi"] == "skipped" and report["deterministic"], "no-oracle run: skipped oracle, deterministic")
        check(all(r["status"] == "self_consistent" and r["passing_words"] == r["words"] for r in report["rows"]),
              "no-oracle run: every word emits, compiles strictly, runs")
        again = mc.run(table, forms, emitter, cc, None, scratch / "synthetic2", small)
        check(mc.render(report) == mc.render(again), "report must be byte-identical across runs")

        # --- unsupported forms are reported as unsupported, never as divergence ------------------
        # (RESET is a legal base-MC68000 form the pipeline does not implement -- SEG-021-T019 implemented the former
        # RTR control; choose another unsupported operandless form here if a later task supplies it.)
        negx = copy.deepcopy(table)
        negx["rows"] = [{"id": "reset.none.none.none.none", "form": "reset.none.none.none.none",
                         "profile": "state_modes"}]
        rep = mc.run(negx, forms, emitter, cc, None, scratch / "unsupported")
        check(rep["rows"][0]["status"] == "unsupported" and rep["rows"][0]["passing_words"] == 0,
              "an unsupported form must be reported as unsupported")

        # exception-vector observation hook, synthetic (no oracle): vectors below 32, 32, 47 and above 47
        probe = scratch / "vector_probe.c"
        probe.write_text(r'''#include "m68k_conformance_common.h"
static uint8_t a[CF_MEM_SIZE], b[CF_MEM_SIZE];
int main(void) { cf_vector v; unsigned d[8] = {0}, r[8] = {0}, k; static const unsigned pcs[] = {CF_HANDLER(4), CF_HANDLER(32), CF_HANDLER(47), CF_HANDLER(64), CF_HANDLER(255), CF_HANDLER(1), 0x2002};
  memset(&v, 0, sizeof v); strcpy(v.id, "p");
  for (k = 0; k < sizeof pcs / sizeof pcs[0]; ++k) cf_print(&v, a, b, pcs[k], 0x2700, 0, 0, d, r);
  return 0; }
''')
        built = subprocess.run([cc, "-std=c11", "-I", str(ROOT / "tests" / "tools"), str(probe), "-o", str(scratch / "vector_probe")],
                               capture_output=True, text=True)
        check(built.returncode == 0, "vector probe must build: " + built.stderr[:300])
        lines = subprocess.run([str(scratch / "vector_probe")], capture_output=True, text=True).stdout.splitlines()
        seen = [[e["v"] for e in json.loads(l)["effects"] if e["k"] == 2] for l in lines]
        check(seen == [[4], [32], [47], [64], [255], [], []], "vector hook must cover 2..255 and only handler PCs: %s" % seen)

        if checkout is None:
            print("pinned Musashi unavailable: oracle comparison and fault-injection checks SKIPPED "
                  "(synthetic self-consistency completed)")
        else:
            rep = mc.run(table, forms, emitter, cc, checkout, scratch / "oracle", small)
            check(all(r["status"] == "validated" and r["passing_words"] == r["words"] and r["counts"]["compared"] > 0
                      for r in rep["rows"]), "migrated forms must match the pinned Musashi: %s" % [
                          (r["row"], r["status"], r["first_divergence"]) for r in rep["rows"]])
            credit = mc.credited_words(rep, forms, table)
            check(credit["ea"] and credit["ccr"] and credit["semantic"], "validated rows must credit aspects")

            # canaries: an extension-bearing form and a no-operand form (user + supervisor state) are ordinary rows
            canary = mc.run(table, forms, emitter, cc, checkout, scratch / "canary",
                            ["neg.unary.w.none.disp", "addi.imm_ea.w.imm.dn", "nop.none.none.none.none"])
            check(all(r["status"] == "validated" and r["passing_words"] == r["words"] and r["counts"]["compared"] > 0
                      for r in canary["rows"]), "canary rows must validate against pinned Musashi: %s" % [
                          (r["row"], r["status"], r["first_divergence"]) for r in canary["rows"]])
            # real Musashi exception entries: TRAP #0..#15 -> vectors 32..47, divide by zero -> vector 5
            trap = expand("trap.vector.none.none.none.vector0to15", profile="state_modes")
            got = mc.run_oracle(trap, checkout, cc, scratch / "trap-oracle")
            for vec in trap:
                if vec["sr"] & 0x2000:
                    hook = [e["v"] for e in got[vec["id"]]["effects"] if e["k"] == 2]
                    check(hook == [32 + (vec["word"] & 15)], "TRAP vector hook: %04X -> %s" % (vec["word"], hook))
            divide = expand("divu.ea_dn.w.imm.dn", bind={"x": "d@9"}, ext=["0000"], profile="unary_sweep")
            got = mc.run_oracle(divide, checkout, cc, scratch / "div-oracle")
            check(all([e["v"] for e in got[v["id"]]["effects"] if e["k"] == 2] == [5] for v in divide),
                  "divide-by-zero must report vector 5 (below 32)")
            user = [v for v in expand("nop.none.none.none.none", profile="state_modes") if not v["sr"] & 0x2000]
            got = mc.run_oracle(user, checkout, cc, scratch / "user-oracle")
            check(user and all(got[v["id"]]["usp"] == v["usp"] and got[v["id"]]["ssp"] == v["ssp"] for v in user),
                  "user-mode initial state must round-trip USP and SSP through the oracle")

            # injected emitter fault: ALU result off by one -> first differing boundary with field detail
            faulty = mc.run(table, forms, emitter, cc, checkout, scratch / "fault-alu", ["add.ea_dn.w.dn.dn"],
                            mutate=lambda s: force_function(s, "D041", "add_destination + add_source;",
                                                            "add_destination + add_source + UINT32_C(1);"))
            r = faulty["rows"][0]
            check(r["status"] == "diverged" and r["passing_words"] == r["words"] - 1, "ALU fault must fail exactly word D041")
            first = r["first_divergence"]
            check(first and first["first_differing_boundary"] == 1 and first["domain"] == "cpu"
                  and first["image"].startswith("add.ea_dn.w.dn.dn:D041:"), "fault must be reported at boundary 1 for the vector")
            names = {f["field"] for f in first["fields"]}
            check("d0" in names and all("generated" in f and "oracle" in f for f in first["fields"]),
                  "field-level difference must name the register: %s" % names)
            check(mc.credited_words(faulty, forms, table)["semantic"].isdisjoint(set()) and
                  not mc.credited_words(faulty, forms, table)["semantic"],
                  "a diverging row must credit nothing")

            # injected memory-RMW/auto-update fault: wrong post-increment amount
            faulty = mc.run(table, forms, emitter, cc, checkout, scratch / "fault-rmw", ["neg.unary.w.none.postinc"],
                            mutate=lambda s: force_function(s, "4458", "m68k_neg_auto_ea += UINT32_C(2);",
                                                            "m68k_neg_auto_ea += UINT32_C(4);"))
            first = faulty["rows"][0]["first_divergence"]
            check(faulty["rows"][0]["status"] == "diverged" and first and
                  {f["field"] for f in first["fields"]} == {"a0"}, "auto-update fault must differ only in a0: %s" % first)
        timing_checks(emitter, cc, table, forms, checkout, scratch)
        t022_checks(emitter, cc, table, forms, checkout, scratch)
    if FAILURES:
        print("%d failure(s)" % len(FAILURES))
        return 1
    print("m68k conformance harness: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
