#!/usr/bin/env python3
"""SEG-008-T002: Z80 capability coverage (test-side measurement).

Measures the production Z80 pipeline against the independent legal-form dataset
(tests/fixtures/z80-legal-forms.json). The dataset is the denominator; production knowledge is only reached
through the probe executable (public cpu_z80 entry points). Only `decodes` and `timing_modeled` are implemented
by T002; every later stage reports unsupported (0) until its task lands. The coverage unit is the form: a stage
is claimed for a form only when every concrete opcode byte of the form passes.

usage:
  z80_capability_coverage.py --probe <exe> [--json out.json] [--update-snapshot] [--check]
"""
import argparse
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
DATASET = ROOT / "tests" / "fixtures" / "z80-legal-forms.json"
SNAPSHOT = ROOT / "tests" / "fixtures" / "z80-capability-coverage.json"
REPORT = ROOT / "docs" / "testing" / "z80-capability-coverage.md"
IMPLEMENTED = ("decodes", "timing_modeled")
SPACES = ("base", "cb", "ed", "dd", "fd", "ddcb", "fdcb")
PREFIX_SPACE = {"base": "", "cb": "CB ", "ed": "ED ", "dd": "DD ", "fd": "FD ", "ddcb": "DD CB d ", "fdcb": "FD CB d "}


def load_dataset():
    return json.loads(DATASET.read_text(encoding="utf-8"))


def expand(ranges):
    for lo, hi in ranges:
        yield from range(lo, hi + 1)


def run_probe(probe):
    out = subprocess.run([probe], check=True, capture_output=True, text=True).stdout
    table = {}
    for line in out.splitlines():
        if line.startswith("#") or not line:
            continue
        f = line.split("\t")
        assert len(f) == 18, line
        table[(f[0], int(f[1]))] = {
            "class": f[2], "form": f[3], "mnemonic": f[4], "dst": f[5], "src": f[6], "documented": f[7] == "1",
            "alias_of": f[8], "length": int(f[9]), "m1": int(f[10]), "opcode_index": int(f[11]),
            "displacement_index": int(f[12]), "immediate_index": int(f[13]), "immediate_size": int(f[14]),
            "timing": (f[15], int(f[16]), int(f[17]))}
    return table


def layout_of(p, space):
    if space in ("ddcb", "fdcb"):
        return PREFIX_SPACE[space] + "op"
    text = PREFIX_SPACE[space] + "op"
    if p["displacement_index"] >= 0:
        text += " d"
    if p["immediate_size"] == 1:
        text += " e" if "e" in (p["dst"], p["src"]) else " n"
    elif p["immediate_size"] == 2:
        text += " nn_lo nn_hi"
    return text


def expected_timing(t):
    klass = t["class"]
    if klass == "fixed":
        return ("fixed", t["t_states"], 0)
    if klass == "conditional":
        return ("conditional", t["not_taken"], t["taken"])
    if klass == "repeat":
        return ("repeat", t["final"], t["repeating"])
    return ("halt", t["t_states"], t["halted_cycle"])


def expected_prefix_class(space, byte):
    """Prefix-behaviour class of a `P` byte, from the dataset's prefix_classes definitions."""
    if space == "base":
        return {0xCB: "escape_cb", 0xDD: "escape_dd", 0xED: "escape_ed", 0xFD: "escape_fd"}[byte]
    if byte == 0xCB:
        return "escape_ddcb" if space == "dd" else "escape_fdcb"
    if byte in (0xDD, 0xFD):
        return "prefix_chain"
    if byte == 0xED:
        return "prefix_ignored_before_ed"
    return "prefix_ignored"


def measure(probe):
    data = load_dataset()
    table = run_probe(probe)
    stages = data["coverage_schema"]["stages"]
    form_rows = [dict(zip(data["form_columns"], row)) for row in data["forms"]]
    owner = {}
    for row in form_rows:
        for b in expand(row["byte_ranges"]):
            owner[(row["space"], b)] = row
    mismatches = []
    masks, decode_words, timing_words = {}, 0, 0
    total_words = 0
    for row in form_rows:
        decode_ok = timing_ok = True
        for b in expand(row["byte_ranges"]):
            total_words += 1
            p = table.get((row["space"], b))
            bad = None
            if p is None or p["class"] != "form":
                bad = "not classified as a form"
            else:
                want = {"form": row["id"], "mnemonic": row["mnemonic"], "dst": row["dst"], "src": row["src"],
                        "documented": row["status"] == "documented", "alias_of": row["alias_of"] or "-",
                        "length": row["length"], "m1": row["m1_fetches"]}
                for key, value in want.items():
                    if p[key] != value:
                        bad = "%s: production %r != dataset %r" % (key, p[key], value)
                        break
                if bad is None and layout_of(p, row["space"]) != row["layout"]:
                    bad = "layout: production %r != dataset %r" % (layout_of(p, row["space"]), row["layout"])
            if bad:
                decode_ok = False
                mismatches.append("%s %02X (%s): %s" % (row["space"], b, row["id"], bad))
            else:
                decode_words += 1
            if p is None or p["class"] != "form" or p["timing"] != expected_timing(row["timing"]):
                timing_ok = False
                mismatches.append("%s %02X (%s): timing %r != dataset %r" % (
                    row["space"], b, row["id"], p and p["timing"], expected_timing(row["timing"])))
            else:
                timing_words += 1
        applicable = {"oracle_memory": "memory" in row["observables"], "oracle_io": "io" in row["observables"]}
        mask = []
        for stage in stages:
            if applicable.get(stage, True) is False:
                mask.append("-")
            elif stage == "decodes":
                mask.append("1" if decode_ok else "0")
            elif stage == "timing_modeled":
                mask.append("1" if timing_ok else "0")
            else:
                mask.append("0")
        masks[row["id"]] = "".join(mask)
    # Prefix-behaviour bytes: class and (for ignored prefixes) the base-space form that then executes.
    prefix_bytes = 0
    for space in SPACES:
        partition = data["space_partition"]["spaces"][space]
        for b, kind in enumerate(partition):
            if kind != "P":
                continue
            prefix_bytes += 1
            p = table[(space, b)]
            want = expected_prefix_class(space, b)
            if p["class"] != want:
                mismatches.append("%s %02X: prefix class %r != %r" % (space, b, p["class"], want))
            elif want == "prefix_ignored":
                base = owner.get(("base", b))
                if base is None or p["form"] != base["id"]:
                    mismatches.append("%s %02X: ignored prefix executes %r, dataset base form %r" % (
                        space, b, p["form"], base and base["id"]))
    # Every non-prefix byte must be a form (and vice versa) exactly as partitioned.
    for space in SPACES:
        for b, kind in enumerate(data["space_partition"]["spaces"][space]):
            if (kind in "DU") != ((space, b) in owner):
                mismatches.append("%s %02X: partition/ownership disagreement" % (space, b))

    totals = {}
    for i, stage in enumerate(stages):
        applicable = [m for m in masks.values() if m[i] != "-"]
        passing = [m for m in applicable if m[i] == "1"]
        totals[stage] = {"applicable_forms": len(applicable), "passing_forms": len(passing),
                         "percent_forms": "%.2f" % (100.0 * len(passing) / len(applicable))}
    return {
        "dataset": {"name": data["dataset"], "schema": data["schema"], "forms": data["counts"]["forms"],
                    "encodings": data["counts"]["encodings"], "prefix_behavior_bytes": prefix_bytes},
        "measurement": {"stage_order": stages, "implemented_stages": list(IMPLEMENTED),
                        "unit": "form (claimed only when every opcode byte of the form passes)"},
        "form_masks": {k: masks[k] for k in sorted(masks)},
        "totals": totals,
        "words": {"encodings": total_words, "decode_passing": decode_words, "timing_passing": timing_words},
        "mismatches": {"count": len(mismatches), "first": mismatches[:20]},
    }


def diff_masks(old_masks, new_masks, stages):
    """(drops, improvements needing a deliberate snapshot update)."""
    drops, improvements = [], []
    for form_id, old in old_masks.items():
        for stage, o, n in zip(stages, old, new_masks[form_id]):
            if o == n:
                continue
            (drops if o == "1" and n == "0" else improvements).append("%s: %s %s -> %s" % (form_id, stage, o, n))
    return drops, improvements


def render_report(result):
    t = result["totals"]
    lines = ["# Z80 capability coverage (SEG-008-T002)", "",
             "Generated by `python3 tools/z80_capability_coverage.py --update-snapshot` from "
             "`tests/fixtures/z80-legal-forms.json`;",
             "snapshot `tests/fixtures/z80-capability-coverage.json` is enforced by "
             "`tests/z80_capability_ratchet_test.py`. Do not edit by hand.", "",
             "Denominator: %d forms, %d form encodings, %d prefix-behaviour bytes (dataset schema %d). A form passes a "
             "stage only if every concrete opcode byte of the form passes. Decode-only: lowering, emission, "
             "execution and oracle stages belong to SEG-008-T003 to T007 and read 0%%." % (
                 result["dataset"]["forms"], result["dataset"]["encodings"],
                 result["dataset"]["prefix_behavior_bytes"], result["dataset"]["schema"]), "",
             "## Coverage by stage (percent of applicable forms)", "",
             "| stage | applicable forms | passing forms | percent |", "| --- | ---: | ---: | ---: |"]
    for stage in result["measurement"]["stage_order"]:
        s = t[stage]
        lines.append("| %s | %d | %d | %s%% |" % (stage, s["applicable_forms"], s["passing_forms"], s["percent_forms"]))
    w = result["words"]
    lines += ["", "Decode (form identity, mnemonic, operand classes, documented status, alias, length, M1 fetches, operand "
              "layout) passes %d of %d encodings; timing class and values pass %d of %d. Word-by-word mismatches "
              "against the dataset: %d." % (w["decode_passing"], w["encodings"], w["timing_passing"], w["encodings"],
                                            result["mismatches"]["count"]), "",
              "Prefix-behaviour bytes (escapes, superseding DD/FD, ignored prefixes) are checked in the same "
              "measurement, including the base-space form executed by an ignored prefix. DD/FD chains of any "
              "length, logical-fetch wrap, prefix lock and the unresolved/mutable classifications are covered by "
              "`cpu_z80_decode_tests`.", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--json")
    parser.add_argument("--update-snapshot", action="store_true")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    result = measure(args.probe)
    text = json.dumps(result, indent=1, sort_keys=True) + "\n"
    if args.json:
        pathlib.Path(args.json).write_text(text, encoding="utf-8")
    if args.update_snapshot:
        SNAPSHOT.write_text(text, encoding="utf-8")
        REPORT.write_text(render_report(result), encoding="utf-8")
    if args.check:
        if SNAPSHOT.read_text(encoding="utf-8") != text:
            print("snapshot differs; run with --update-snapshot")
            return 1
    print("z80 coverage: %s" % ", ".join("%s %s%%" % (s, result["totals"][s]["percent_forms"]) for s in IMPLEMENTED),
          "mismatches:", result["mismatches"]["count"])
    return 0 if result["mismatches"]["count"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
