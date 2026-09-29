#!/usr/bin/env python3
"""SEG-008-T002/T003: Z80 capability-coverage ratchet. Hermetic (no ROM, no oracle): re-measures every form of the
independent legal-form dataset through the production entry points (decode probe, lowering probe, image emitter,
strict-C11 compilation, generated-native execution) and compares with the committed snapshot.

usage: z80_capability_ratchet_test.py <probe> <product-root> <lowering-probe> <emitter> <cc>

Rules: no form may drop a stage; improvements need a deliberate snapshot regeneration
(`tools/z80_capability_coverage.py --update-snapshot`); two runs are byte-identical; zero word-by-word decode/timing
mismatches; `decodes` and `timing_modeled` are 100% of forms; the T003 pipeline stages (lowers, emits, compiles,
executes, aot_admitted) are 100% of exactly the lowered forms; oracle stages are credited only for lowered, executing
forms with a fresh committed manifest entry and never exceed the lowered forms.
"""
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile

PROBE, ROOT = sys.argv[1], pathlib.Path(sys.argv[2]).resolve()
LOWERING_PROBE, EMITTER, CC = sys.argv[3], sys.argv[4], sys.argv[5]
TOOL = ROOT / "tools" / "z80_capability_coverage.py"
SNAPSHOT = ROOT / "tests" / "fixtures" / "z80-capability-coverage.json"
REPORT = ROOT / "docs" / "testing" / "z80-capability-coverage.md"
PRODUCTION_DIRS = ("libs", "platforms", "apps")
FORBIDDEN_IN_PRODUCTION = ("z80_capability", "z80-capability", "z80_legal_forms", "z80-legal-forms")
FORBIDDEN_IN_TOOL = ("libs/cpu", "cpu/z80/", "classify_opcode_byte", "form_descriptor")
PIPELINE = ("lowers", "emits", "compiles", "executes", "aot_admitted")
ORACLE = ("oracle_state", "oracle_memory", "oracle_io", "timing_validated")
SEED_FORMS = {"nop.base", "ld.r_r.base", "ld.r_n.base", "halt.base"}


def check(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.exit(1)


def run(json_path):
    subprocess.run([sys.executable, str(TOOL), "--probe", PROBE, "--lowering-probe", LOWERING_PROBE, "--emitter", EMITTER,
                    "--cc", CC, "--json", str(json_path)], check=True)
    return pathlib.Path(json_path).read_bytes()


spec = importlib.util.spec_from_file_location("z80_capability_coverage", TOOL)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

snapshot = json.loads(SNAPSHOT.read_text(encoding="utf-8"))
with tempfile.TemporaryDirectory() as tmp:
    first = run(pathlib.Path(tmp) / "a.json")
    second = run(pathlib.Path(tmp) / "b.json")
check(first == second, "coverage output is not byte-identical across two runs")
current = json.loads(first)

check(current["mismatches"]["count"] == 0, "production disagrees with the dataset: %s" % current["mismatches"]["first"])
check(current["dataset"] == snapshot["dataset"], "dataset identity changed; regenerate the snapshot deliberately")
stages = snapshot["measurement"]["stage_order"]
check(current["measurement"]["stage_order"] == stages, "stage order changed")
check(sorted(current["form_masks"]) == sorted(snapshot["form_masks"]), "form set changed")
drops, improvements = tool.diff_masks(snapshot["form_masks"], current["form_masks"], stages)
check(not drops, "capability regression (%d): %s" % (len(drops), "; ".join(drops[:10])))
check(not improvements, "coverage changed without a deliberate snapshot update (%d): %s" % (
    len(improvements), "; ".join(improvements[:10])))
check(REPORT.read_text(encoding="utf-8") == tool.render_report(snapshot), "report does not match snapshot")

# T002/T003 acceptance.
lowered = set(current["measurement"]["lowered_forms"])
check(SEED_FORMS <= lowered, "seed forms must lower: %s" % sorted(SEED_FORMS - lowered))
for stage, tally in current["totals"].items():
    if stage in ("decodes", "timing_modeled"):
        check(tally["passing_forms"] == tally["applicable_forms"] == current["dataset"]["forms"], "%s is not 100%%" % stage)
    elif stage in PIPELINE:
        check(tally["passing_forms"] == len(lowered), "%s must hold for exactly the %d lowered forms" % (stage, len(lowered)))
    else:
        check(tally["passing_forms"] <= len(lowered), "%s credits a form that does not lower" % stage)
for form in lowered:
    mask = dict(zip(stages, current["form_masks"][form]))
    check(all(mask[s] == "1" for s in PIPELINE), "%s lowers but a pipeline stage failed: %s" % (form, current["form_masks"][form]))
for form in SEED_FORMS:  # seed forms are oracle-validated (committed manifest credit, ADR 0057)
    mask = dict(zip(stages, current["form_masks"][form]))
    check(mask["oracle_state"] == "1" and mask["timing_validated"] == "1", "seed form %s lacks oracle credit" % form)
check(current["words"]["decode_passing"] == current["words"]["timing_passing"] == current["words"]["encodings"],
      "decode/timing word counts incomplete")

# Independence in both directions.
tool_text = TOOL.read_text(encoding="utf-8")
check(not any(t in tool_text for t in FORBIDDEN_IN_TOOL), "coverage tool embeds production-decoder tokens")
for directory in PRODUCTION_DIRS:
    for path in (ROOT / directory).rglob("*"):
        if path.is_file() and path.suffix in {".c", ".cc", ".cpp", ".h", ".hpp", ".txt", ".cmake", ".py", ".json"}:
            text = path.read_text(encoding="utf-8", errors="ignore")
            check(not any(t in text for t in FORBIDDEN_IN_PRODUCTION), "production file references coverage data: %s" % path)

# Negative controls: the ratchet detects a regression and an unrecorded improvement.
old = {"x": "10"}
check(tool.diff_masks(old, {"x": "00"}, ["a", "b"])[0], "control: drop not detected")
check(tool.diff_masks(old, {"x": "11"}, ["a", "b"])[1], "control: improvement not detected")
print("z80 capability ratchet OK: %d forms, %d stages" % (len(current["form_masks"]), len(stages)))
