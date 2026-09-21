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
        check(all(len(v["mem"]) <= 2 and v["sr"] & 0x2000 for v in vectors), "supervisor vectors, bounded seeds")
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
        negx = copy.deepcopy(table)
        negx["rows"] = [{"id": "negx.unary.w.none.dn", "form": "negx.unary.w.none.dn", "ea": "dst", "bind": {"x": "ea"},
                         "profile": "unary_sweep"}]
        rep = mc.run(negx, forms, emitter, cc, None, scratch / "unsupported")
        check(rep["rows"][0]["status"] == "unsupported" and rep["rows"][0]["passing_words"] == 0,
              "an unsupported form must be reported as unsupported")

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
