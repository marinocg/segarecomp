#!/usr/bin/env python3
"""SEG-098-T001 (ADR 0210): the loader survey - module-jump-table roots, survey counters and the aggregate-only report.

Project-authored discs only (tools/sega_cd_unvisited_fixture.py): `base` has loader code at the SP entry; `table_header` carries an authentic-style SP module
header and jump table (descriptor entry 0 = the header). usage: sega_cd_loader_survey_test.py <segarecomp> <c-compiler> <source-root>
"""
import json
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def discover(cue, *flags):
    proc = subprocess.run([SEGARECOMP, "sega-cd", "discover-modules", "--cue", str(cue), *flags], capture_output=True, text=True, timeout=120)
    return proc.returncode, json.loads(proc.stdout)


def main():
    with tempfile.TemporaryDirectory(prefix="scd-survey-") as tmp:
        tmp = pathlib.Path(tmp)
        base = tmp / "base"
        common.build_disc(base, "unvisited", "cooked", "--unvisited-case", "base")
        hdr = tmp / "hdr"
        oracle = common.build_disc(hdr, "unvisited", "cooked", "--unvisited-case", "table_header")

        # the default scan is unchanged by the survey counters; without a module header the table option falls back to the descriptor entry
        code0, found0 = discover(base / "disc.cue")
        code1, found1 = discover(base / "disc.cue", "--module-table-roots")
        key = lambda f: (f["modules"], f["operations"], f["refusals"])  # noqa: E731
        check(code0 == 0 and len(found0["modules"]) == 3 and key(found0) == key(found1) and not found1["survey"]["module_table_found"] and found1["survey"]["roots"] == 1,
              "no module header: the table option falls back to the descriptor entry; the result is identical (3 modules)")
        sv = found0["survey"]
        check(sv["cdd_commands"] >= 1 and sv["cdc_transfer_triggers"] >= 1 and sv["bios_service_calls"] == 0, "survey counters: CDD/CDC sites seen, no BIOS service calls on a fixture without a BIOS")

        # the authentic-style SP: the descriptor entry is the header (nothing to walk), the module table's init entry reaches the loader
        codeh, hdr_default = discover(hdr / "disc.cue")
        check(codeh != 0 and not hdr_default["modules"] and hdr_default["steps"] <= 3,
              "module header, descriptor entry only: the walk starts in the header and discovers nothing (the M1 limitation, measured)")
        codet, hdr_table = discover(hdr / "disc.cue", "--module-table-roots")
        check(codet == 0 and hdr_table["survey"]["module_table_found"] and hdr_table["survey"]["roots"] == 1 and len(hdr_table["modules"]) == 3,
              "module header + --module-table-roots: the init entry is the root; modules A, B and C are discovered")
        names = lambda f: [(m["block"], m["bases"], m["entries"], m["range"]) for m in f["modules"]]  # noqa: E731
        check(names(hdr_table) == names(found0), "the modules found through the module table equal those found from the entry-at-start fixture (same sources, bases, entries, ranges)")
        check(oracle["modules"]["A"]["bases"] == [m for m in hdr_table["modules"] if m["block"] == oracle["lba"]["A"]][0]["bases"], "A's base equals the oracle")

        # the aggregate-only tool: classes and counts, nothing reconstructable
        status = tmp / "status.json"
        a = hdr_table["modules"][0]
        status.write_text(json.dumps({"status": "ok", "rounds": 2, "stop": "idle", "extents": ["sub:%d:%d:%d" % (a["block"] * 2048, a["bases"][0], 2048), "main:%d:%d:%d" % (999 * 2048, 0xFF0000, 4096)],
                                      "history": [{"round": 0, "admitted": "extent"}, {"round": 1}], "frontier": None}))
        proc = subprocess.run([sys.executable, str(ROOT / "tools" / "sega_cd_loader_survey.py"), "--segarecomp", SEGARECOMP, "--cue", str(hdr / "disc.cue"), "--status", str(status)],
                              capture_output=True, text=True, timeout=300)
        check(proc.returncode == 0, "the survey tool runs")
        report = json.loads(proc.stdout)
        check(report["static_vs_observed"] == {"static_sources": 3, "observed_sources": 2, "overlap": 1}, "static vs observed: 3 static sources, 2 observed, overlap 1 (computed in memory)")
        check(report["scan_descriptor_entry_only"]["modules"] == 0 and report["scan_module_table_roots"]["modules"] == 3, "the three scan variants are reported side by side")
        text = proc.stdout
        forbidden = ("sha256", "\"block\"", "\"bases\"", "\"entries\"", "disc_identity", "lba", "name")
        check(not any(f in text for f in forbidden), "the report contains no hash, block, base, entry, identity or name field (aggregate classes and counts only)")
        coords = {str(a["block"] * 2048), str(a["bases"][0]), str(0xFF0000), str(999 * 2048), str(a["block"])}
        flat = set(__import__("re").findall(r"\d+", text))
        check(not (coords & flat), "no source offset, block or destination address of the inputs appears in the report")
    print("\n%s" % ("FAILED: %d" % len(failures) if failures else "all checks passed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
