#!/usr/bin/env python3
"""SEG-021-T002: capability-coverage ratchet. Hermetic (no ROM, no Musashi): re-measures every legal
base-MC68000 form through the public pipeline (probe + strict C11 compile + native run) and compares
with the committed snapshot.

usage: m68k_capability_ratchet_test.py <probe-executable> <c-compiler> <product-root>

Rules: no form may drop a stage or route; determinism (two runs byte-identical); improvements require a
deliberate snapshot regeneration (`tools/m68k_capability_coverage.py --update-snapshot`), except for the
compiler-dependent stages (compile, native_exec, route_direct), where an improvement is only reported.
"""
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile

PROBE, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
TOOL = ROOT / "tools" / "m68k_capability_coverage.py"
SNAPSHOT = ROOT / "tests" / "fixtures" / "m68k-capability-coverage.json"
REPORT = ROOT / "docs" / "testing" / "m68k-capability-coverage.md"
MANIFEST = ROOT / "tests" / "fixtures" / "m68k-validation-manifest.json"
COMPILER_DEPENDENT = {"compile", "native_exec", "route_direct"}
PRODUCTION_DIRS = ("libs", "platforms", "apps")
FORBIDDEN_IN_PRODUCTION = ("m68k_capability", "m68k-capability", "m68k_legal_forms", "m68k-legal-forms")
FORBIDDEN_IN_TOOL = ("libs/cpu", "cpu/m68k/", "ea_mask", "eamask", "m68k_decode")


def check(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.exit(1)


def run(json_path):
    subprocess.run([sys.executable, str(TOOL), "--probe", PROBE, "--cc", CC, "--json", str(json_path)],
                   check=True)
    return pathlib.Path(json_path).read_bytes()


spec = importlib.util.spec_from_file_location("m68k_capability_coverage", TOOL)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

snapshot = json.loads(SNAPSHOT.read_text(encoding="utf-8"))
with tempfile.TemporaryDirectory() as tmp:
    first = run(pathlib.Path(tmp) / "a.json")
    second = run(pathlib.Path(tmp) / "b.json")
check(first == second, "coverage output is not byte-identical across two runs")
current = json.loads(first)

check(current["dataset"] == snapshot["dataset"], "dataset identity changed; regenerate the snapshot deliberately")
check(current["measurement"]["stage_order"] == snapshot["measurement"]["stage_order"], "stage order changed")
stages = snapshot["measurement"]["stage_order"]
check(sorted(current["form_masks"]) == sorted(snapshot["form_masks"]), "form set changed")

drops, improvements, tolerated = [], [], []
for form_id, old in snapshot["form_masks"].items():
    new = current["form_masks"][form_id]
    for stage, o, n in zip(stages, old, new):
        if o == n:
            continue
        if o == "1" and n == "0":
            drops.append("%s: %s dropped" % (form_id, stage))
        elif stage in COMPILER_DEPENDENT and o == "0" and n == "1":
            tolerated.append("%s: %s improved" % (form_id, stage))
        else:
            improvements.append("%s: %s %s -> %s" % (form_id, stage, o, n))
check(not drops, "capability regression (%d): %s" % (len(drops), "; ".join(drops[:10])))
check(not improvements, "coverage changed without a deliberate snapshot update (%d): %s; run "
      "tools/m68k_capability_coverage.py --update-snapshot" % (len(improvements), "; ".join(improvements[:10])))
if tolerated:
    print("note: %d compiler-dependent improvements not yet in the snapshot" % len(tolerated))

# The committed human report and snapshot must agree.
check(REPORT.read_text(encoding="utf-8") == tool.render_report(snapshot), "report does not match snapshot")

# Validation manifest: words are legal words; batch fixtures' vector words are all listed.
manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
_, forms = tool.load_forms()
legal = {w for f in forms for w in tool.expand(f)}
listed = {int(w, 16) for w in manifest["semantic_validated_words"]}
check(listed <= legal, "manifest lists a word that is not a legal primary word")
for name in ("m68k-batch-b-musashi-vectors", "m68k-batch-c-musashi-vectors"):
    vectors = json.loads((ROOT / "tests" / "fixtures" / (name + ".json")).read_text(encoding="utf-8"))["accepted"]
    check({int(v["code_hex"][:4], 16) for v in vectors} <= listed, name + " words missing from manifest")

# Independence in both directions.
tool_text = TOOL.read_text(encoding="utf-8")
check(not any(t in tool_text for t in FORBIDDEN_IN_TOOL), "coverage tool embeds production-legality tokens")
for directory in PRODUCTION_DIRS:
    for path in (ROOT / directory).rglob("*"):
        if path.is_file() and path.suffix in {".c", ".cc", ".cpp", ".h", ".hpp", ".txt", ".cmake", ".py", ".json"}:
            text = path.read_text(encoding="utf-8", errors="ignore")
            check(not any(t in text for t in FORBIDDEN_IN_PRODUCTION), "production file references coverage data: %s" % path)
print("m68k capability ratchet OK: %d forms, %d stages" % (len(current["form_masks"]), len(stages)))
