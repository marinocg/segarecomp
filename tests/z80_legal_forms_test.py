#!/usr/bin/env python3
"""SEG-008-T001: independent NMOS Z80 legal-form dataset -- reproducibility, complete exactly-one ownership of
the seven finite opcode spaces, family -> task mapping, pinned counts, spot encodings/timings, and the two-way
independence rule (dataset/tool never import or read production decode or oracle code; production never imports
or reads the dataset/tool). Hermetic: no oracle checkout, no ROM."""
import ast
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "z80_legal_forms.py"
FIXTURE = ROOT / "tests" / "fixtures" / "z80-legal-forms.json"

STDLIB_ALLOWED = {"argparse", "json", "pathlib", "sys"}
FORBIDDEN_IN_DATASET_SIDE = ("libs/cpu", "cpu/z80", "z80_decode", "codegen_c11_z80", "kosarev", "superzazu",
                             "redcode", "floooh", "singlesteptests")
FORBIDDEN_IN_PRODUCTION = ("z80_legal_forms", "z80-legal-forms")
PRODUCTION_DIRS = ("libs", "platforms", "apps")
# Test-side measurement tools that legitimately consume the dataset (the ratchet test guards their independence).
MEASUREMENT_TOOLS = ("z80_capability_coverage.py", "z80_conformance.py")
PRODUCTION_FILES = ("CMakeLists.txt", "CMakePresets.json")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".txt", ".cmake", ".py", ".json", ".in"}

SPACES = ("base", "cb", "ed", "dd", "fd", "ddcb", "fdcb")
FAMILY_TASKS = {"data_alu": "SEG-008-T004", "control_stack": "SEG-008-T005",
                "cb_bit_prefix": "SEG-008-T006", "ed_io_interrupt": "SEG-008-T007"}

# Pinned expectations (regenerate deliberately when the dataset changes).
EXPECTED_FORMS = 261
EXPECTED_DOCUMENTED_FORMS = 199
EXPECTED_ENCODINGS = 1446
EXPECTED_PER_SPACE = {  # space: (forms, documented encodings, undocumented encodings, prefix-behaviour bytes)
    "base": (75, 252, 0, 4), "cb": (22, 248, 8, 0), "ed": (44, 58, 198, 0), "dd": (38, 39, 46, 171),
    "fd": (38, 39, 46, 171), "ddcb": (22, 31, 225, 0), "fdcb": (22, 31, 225, 0),
}
EXPECTED_PER_FAMILY = {"data_alu": (128, 378), "control_stack": (26, 63), "cb_bit_prefix": (66, 768),
                       "ed_io_interrupt": (41, 237)}


def check(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.exit(1)


def tool_imports_and_literals(source):
    tree = ast.parse(source)
    imports, literals, docstrings = set(), [], set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.FunctionDef, ast.ClassDef)):
            body = node.body
            if body and isinstance(body[0], ast.Expr) and isinstance(body[0].value, ast.Constant) \
                    and isinstance(body[0].value.value, str):
                docstrings.add(id(body[0].value))
        if isinstance(node, ast.Import):
            imports.update(a.name.split(".")[0] for a in node.names)
        elif isinstance(node, ast.ImportFrom):
            imports.add((node.module or "").split(".")[0])
        elif isinstance(node, ast.Constant) and isinstance(node.value, str) and id(node) not in docstrings:
            literals.append(node.value)
    return imports, literals


def scan_tree_for(root, tokens, dirs, files):
    hits = []
    targets = [root / f for f in files if (root / f).is_file()]
    for d in dirs:
        base = root / d
        if base.is_dir():
            targets += [p for p in base.rglob("*") if p.is_file() and p.suffix in SOURCE_SUFFIXES]
    for path in sorted(targets):
        text = path.read_text(errors="replace").lower()
        hits += [(str(path.relative_to(root)), t) for t in tokens if t in text]
    return hits


def rows():
    data = json.loads(FIXTURE.read_text())
    return data, [dict(zip(data["form_columns"], r)) for r in data["forms"]]


def test_independence():
    imports, literals = tool_imports_and_literals(TOOL.read_text())
    check(imports <= STDLIB_ALLOWED, "tool imports outside the stdlib allowlist: %r" % sorted(imports - STDLIB_ALLOWED))
    for lit in literals:
        for token in FORBIDDEN_IN_DATASET_SIDE:
            check(token not in lit.lower(), "tool string literal references production/oracle: %r" % lit)
    dataset_text = FIXTURE.read_text().lower()
    for token in FORBIDDEN_IN_DATASET_SIDE:
        check(token not in dataset_text, "dataset references production/oracle token %r" % token)
    hits = scan_tree_for(ROOT, FORBIDDEN_IN_PRODUCTION, PRODUCTION_DIRS, PRODUCTION_FILES)
    check(not hits, "production references the test-side dataset/tool: %r" % hits[:5])
    for p in (ROOT / "tools").glob("*"):
        if p.is_file() and p != TOOL and p.name not in MEASUREMENT_TOOLS and p.suffix in SOURCE_SUFFIXES:
            text = p.read_text(errors="replace").lower()
            check(not any(t in text for t in FORBIDDEN_IN_PRODUCTION), "product tool %s consumes the dataset" % p.name)


def test_independence_detectors_bite():
    imports, literals = tool_imports_and_literals("import json\nfrom libs.cpu import z80\nX = 'libs/cpu/z80/decode.cpp'\n")
    check("libs" in imports and any("libs/cpu" in s for s in literals), "dataset-side detector missed a violation")
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / "libs" / "cpu").mkdir(parents=True)
        (root / "libs" / "cpu" / "x.cpp").write_text('#include "tests/fixtures/z80-legal-forms.json"\n')
        check(scan_tree_for(root, FORBIDDEN_IN_PRODUCTION, PRODUCTION_DIRS, PRODUCTION_FILES),
              "production-side detector missed a violation")


def test_reproducible():
    outputs = []
    for seed in ("1", "2"):
        with tempfile.TemporaryDirectory() as d:
            out = pathlib.Path(d) / "forms.json"
            env = dict(os.environ, PYTHONHASHSEED=seed)
            r = subprocess.run([sys.executable, str(TOOL), "--output", str(out)], env=env, capture_output=True, text=True)
            check(r.returncode == 0, "derivation failed: " + r.stderr)
            outputs.append(out.read_bytes())
    check(outputs[0] == outputs[1], "derivation is not byte-for-byte reproducible across runs")
    check(outputs[0] == FIXTURE.read_bytes(), "committed dataset differs from a fresh derivation (run tools/z80_legal_forms.py)")
    r = subprocess.run([sys.executable, str(TOOL), "--check", "--output", str(FIXTURE)])
    check(r.returncode == 0, "--check reported a stale dataset")


def test_complete_exactly_one_ownership():
    data, forms = rows()
    part = data["space_partition"]["spaces"]
    check(tuple(sorted(part)) == tuple(sorted(SPACES)) and all(len(part[s]) == 256 for s in SPACES), "partition shape")
    owner = {}
    for r in forms:
        check(r["space"] in SPACES, "%s: unknown space" % r["id"])
        prev_hi, total = -2, 0
        for lo, hi in r["byte_ranges"]:
            check(0 <= lo <= hi <= 0xFF and lo > prev_hi + 1, "%s: ranges not ascending/non-adjacent" % r["id"])
            prev_hi = hi
            total += hi - lo + 1
            for b in range(lo, hi + 1):
                key = (r["space"], b)
                check(key not in owner, "%s %02X owned by %s and %s" % (r["space"], b, owner.get(key), r["id"]))
                owner[key] = r["id"]
                letter = part[r["space"]][b]
                check(letter == ("D" if r["status"] == "documented" else "U"), "%s %02X: partition letter" % key)
        check(total == r["encodings"], "%s: encodings count" % r["id"])
    for s in SPACES:
        for b in range(256):
            letter = part[s][b]
            check(((s, b) in owner) == (letter in "DU"), "%s %02X: form ownership vs partition letter %s" % (s, b, letter))
            check(letter in "DUPX", "%s %02X: unknown letter" % (s, b))
        pc = data["counts"]["per_space"][s]["prefix_classes"]
        check(sum(pc.values()) == part[s].count("P"), "%s: prefix-class bytes != P letters" % s)
    for cls in {c for s in SPACES for c in data["counts"]["per_space"][s]["prefix_classes"]}:
        check(cls in data["prefix_classes"], "prefix class %s is undefined" % cls)


def test_counts_families_and_scope():
    data, forms = rows()
    c = data["counts"]
    check(c["forms"] == len(forms) == EXPECTED_FORMS, "form count")
    check(c["documented_forms"] == EXPECTED_DOCUMENTED_FORMS, "documented form count")
    check(c["encodings"] == sum(r["encodings"] for r in forms) == EXPECTED_ENCODINGS, "encoding count")
    check(len({r["id"] for r in forms}) == len(forms), "form ids are not unique")
    for s, (nf, dd, uu, pp) in EXPECTED_PER_SPACE.items():
        v = c["per_space"][s]
        check((v["forms"], v["documented_encodings"], v["undocumented_encodings"], v["prefix_behavior_bytes"])
              == (nf, dd, uu, pp), "per-space counts for %s: %r" % (s, v))
        check(nf == sum(1 for r in forms if r["space"] == s), "recomputed forms for %s" % s)
    fam = {}
    for r in forms:
        check(r["family"] in FAMILY_TASKS and r["task"] == FAMILY_TASKS[r["family"]], "%s: family/task" % r["id"])
        check(r["scope"] == "in_scope" or r["scope"].startswith("excluded:"), "%s: scope vocabulary" % r["id"])
        f = fam.setdefault(r["family"], [0, 0])
        f[0] += 1
        f[1] += r["encodings"]
    check({k: tuple(v) for k, v in fam.items()} == EXPECTED_PER_FAMILY, "per-family counts %r" % fam)
    check({k: (v["forms"], v["encodings"]) for k, v in c["per_family"].items()} == EXPECTED_PER_FAMILY,
          "recorded per-family counts")
    check(c["excluded_forms"] == 0, "T001 scope decision: no exclusions")
    ids = {r["id"] for r in forms}
    for r in forms:
        check(not r["alias_of"] or r["alias_of"] in ids, "%s: dangling alias" % r["id"])
        check(r["status"] in ("documented", "undocumented"), "%s: status" % r["id"])
        check(r["timing"]["class"] in ("fixed", "conditional", "repeat", "halt"), "%s: timing class" % r["id"])
        check(set(r["observables"]) <= {"state", "memory", "io"} and "state" in r["observables"], "%s: observables" % r["id"])
    check(list(data["coverage_schema"]["stages"]) == ["decodes", "lowers", "emits", "compiles", "executes",
          "aot_admitted", "oracle_state", "oracle_memory", "oracle_io", "timing_modeled", "timing_validated"],
          "coverage schema stages")
    check("unsupported" not in json.dumps(data["forms"]), "dataset rows must not carry an 'unsupported' status")
    rules = {r["id"] for r in data["prefix_rules"]}
    check({"index_prefix_chain", "index_prefix_ignored", "index_cb_displacement"} <= rules, "prefix rules")


def form_at(forms, space, b):
    for r in forms:
        if r["space"] == space and any(lo <= b <= hi for lo, hi in r["byte_ranges"]):
            return r
    return None


def test_spot_encodings():
    data, forms = rows()
    part = data["space_partition"]["spaces"]
    expect = [  # (space, byte, id, length, timing)
        ("base", 0x00, "nop.base", 1, {"class": "fixed", "t_states": 4}),
        ("base", 0x10, "djnz.e.base", 2, {"class": "conditional", "taken": 13, "not_taken": 8}),
        ("base", 0x76, "halt.base", 1, {"class": "halt", "t_states": 4, "halted_cycle": 4}),
        ("base", 0xC4, "call.cc_nn.base", 3, {"class": "conditional", "taken": 17, "not_taken": 10}),
        ("base", 0xC2, "jp.cc_nn.base", 3, {"class": "fixed", "t_states": 10}),
        ("base", 0xE3, "ex.sp_ind_hl.base", 1, {"class": "fixed", "t_states": 19}),
        ("cb", 0x36, "sll.hl_ind.cb.undoc", 2, {"class": "fixed", "t_states": 15}),
        ("cb", 0x46, "bit.bit_hl_ind.cb", 2, {"class": "fixed", "t_states": 12}),
        ("ed", 0x71, "out.c_port_zero.ed.undoc", 2, {"class": "fixed", "t_states": 12}),
        ("ed", 0x70, "in.flags_only_c_port.ed.undoc", 2, {"class": "fixed", "t_states": 12}),
        ("ed", 0x4E, "im.im0.ed.alias", 2, {"class": "fixed", "t_states": 8}),
        ("ed", 0x63, "ld.nn_ind_hl.ed", 4, {"class": "fixed", "t_states": 20}),
        ("ed", 0xB0, "ldir.ed", 2, {"class": "repeat", "repeating": 21, "final": 16}),
        ("ed", 0x00, "nop.ed_undefined.ed.undoc", 2, {"class": "fixed", "t_states": 8}),
        ("ed", 0xFF, "nop.ed_undefined.ed.undoc", 2, {"class": "fixed", "t_states": 8}),
        ("dd", 0x64, "ld.ix_half_ix_half.dd.undoc", 2, {"class": "fixed", "t_states": 8}),
        ("dd", 0x66, "ld.r_ix_d.dd", 3, {"class": "fixed", "t_states": 19}),
        ("dd", 0x36, "ld.ix_d_n.dd", 4, {"class": "fixed", "t_states": 19}),
        ("fd", 0xE9, "jp.iy.fd", 2, {"class": "fixed", "t_states": 8}),
        ("ddcb", 0x06, "rlc.ix_d.ddcb", 4, {"class": "fixed", "t_states": 23}),
        ("ddcb", 0x00, "rlc.ix_d_copy_r.ddcb.undoc", 4, {"class": "fixed", "t_states": 23}),
        ("ddcb", 0x40, "bit.bit_ix_d.ddcb.alias", 4, {"class": "fixed", "t_states": 20}),
        ("fdcb", 0xFE, "set.bit_iy_d.fdcb", 4, {"class": "fixed", "t_states": 23}),
    ]
    for space, b, fid, length, timing in expect:
        r = form_at(forms, space, b)
        check(r is not None and r["id"] == fid, "%s %02X: expected %s got %s" % (space, b, fid, r and r["id"]))
        check(r["length"] == length and r["timing"] == timing, "%s %02X: length/timing %r %r" % (space, b, r["length"], r["timing"]))
    for space, b in (("base", 0xCB), ("base", 0xDD), ("base", 0xED), ("base", 0xFD), ("dd", 0x40), ("dd", 0x76),
                     ("dd", 0xEB), ("dd", 0xDD), ("dd", 0xFD), ("dd", 0xED), ("dd", 0xCB), ("fd", 0xD9)):
        check(part[space][b] == "P" and form_at(forms, space, b) is None, "%s %02X must be a prefix-behaviour byte" % (space, b))
    check(form_at(forms, "ddcb", 0x46)["status"] == "documented", "DDCB BIT b,(IX+d) documented encoding")
    check(form_at(forms, "ddcb", 0x41)["alias_of"] == "bit.bit_ix_d.ddcb", "DDCB BIT register-field alias")


def main():
    for t in (test_independence, test_independence_detectors_bite, test_reproducible,
              test_complete_exactly_one_ownership, test_counts_families_and_scope, test_spot_encodings):
        t()
    print("OK: z80 legal-form dataset")


if __name__ == "__main__":
    main()
