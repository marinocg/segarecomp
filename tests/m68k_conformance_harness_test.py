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
    if FAILURES:
        print("%d failure(s)" % len(FAILURES))
        return 1
    print("m68k conformance harness: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
