#!/usr/bin/env python3
"""SEG-021-T001: legal base-MC68000 form dataset -- reproducibility, counts, vocabulary and the
two-way independence rule (dataset/tool never import or read production legality; production
never imports or reads the dataset/tool). Hermetic: no Musashi, no ROM."""
import ast
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "m68k_legal_forms.py"
FIXTURE = ROOT / "tests" / "fixtures" / "m68k-legal-forms.json"
SWEEP_FIXTURE = ROOT / "tests" / "fixtures" / "m68k-word-sweep-disagreements.json"

STDLIB_ALLOWED = {"argparse", "json", "pathlib", "sys"}
# Tokens that would indicate production-legality coupling (checked in code string literals and the dataset).
FORBIDDEN_IN_DATASET_SIDE = ("libs/cpu", "cpu/m68k", "ea_mask", "eamask", "m68k_decode", "musashi")
# Tokens that would indicate production consuming the test-side expectation.
FORBIDDEN_IN_PRODUCTION = ("m68k_legal_forms", "m68k-legal-forms", "m68k-word-sweep", "m68k_word_sweep")
PRODUCTION_DIRS = ("libs", "platforms", "apps")
PRODUCTION_FILES = ("CMakeLists.txt", "CMakePresets.json")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".txt", ".cmake", ".py", ".json", ".in"}

# Pinned expectations for the committed dataset (regenerate deliberately when the dataset changes).
EXPECTED_TOTAL_FORMS = 1526
EXPECTED_LEGAL_WORDS = 45816
EXPECTED_FAMILIES = {
    "binary_coded_decimal": (12, 306), "bit_manipulation": (77, 1876), "data_movement": (357, 12638),
    "integer_arithmetic": (500, 15522), "logical": (228, 6360), "program_control": (193, 5083),
    "shift_and_rotate": (104, 3408), "system_control": (55, 623),
}
EXPECTED_PARTITION = {
    "legal_user": 45741, "legal_privileged": 75, "line_a_reserved_exception": 4096,
    "line_f_reserved_exception": 4096, "illegal_post_68000_encoding": 2249, "illegal_reserved_unassigned": 9279,
}


def check(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.exit(1)


def tool_imports_and_literals(source):
    tree = ast.parse(source)
    imports, literals = set(), []
    docstrings = set()
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
        for token in tokens:
            if token in text:
                hits.append((str(path.relative_to(root)), token))
    return hits


def test_independence():
    imports, literals = tool_imports_and_literals(TOOL.read_text())
    check(imports <= STDLIB_ALLOWED, "tool imports outside the stdlib allowlist: %r" % sorted(imports - STDLIB_ALLOWED))
    for lit in literals:
        low = lit.lower()
        for token in FORBIDDEN_IN_DATASET_SIDE:
            check(token not in low, "tool string literal references production legality/oracle: %r" % lit)
    dataset_text = FIXTURE.read_text().lower()
    for token in FORBIDDEN_IN_DATASET_SIDE:
        check(token not in dataset_text, "dataset references production legality/oracle token %r" % token)
    hits = scan_tree_for(ROOT, FORBIDDEN_IN_PRODUCTION, PRODUCTION_DIRS, PRODUCTION_FILES)
    check(not hits, "production references the test-side dataset/tool: %r" % hits[:5])
    # SEG-021-T002: the coverage measurement is the one product-side tool permitted to consume the dataset
    # (see docs/testing/m68k-legal-forms.md); it is itself covered by tests/m68k_capability_ratchet_test.py.
    other_tools = [p for p in (ROOT / "tools").glob("*")
                   if p.is_file() and p != TOOL and p.name != "m68k_capability_coverage.py"]
    for p in other_tools:
        if p.suffix in SOURCE_SUFFIXES:
            text = p.read_text(errors="replace").lower()
            check(not any(t in text for t in FORBIDDEN_IN_PRODUCTION), "product tool %s consumes the dataset" % p.name)


def test_independence_detectors_bite():
    """Negative controls: the scanners must flag a synthetic violation in each direction."""
    imports, literals = tool_imports_and_literals("import json\nfrom libs.cpu import m68k\nX = 'libs/cpu/m68k/ea.cpp'\n")
    check("libs" in imports and any("libs/cpu" in s for s in literals), "dataset-side detector missed a violation")
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / "libs" / "cpu").mkdir(parents=True)
        (root / "libs" / "cpu" / "x.cpp").write_text('#include "tests/fixtures/m68k-legal-forms.json"\n')
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
    check(outputs[0] == FIXTURE.read_bytes(), "committed dataset differs from a fresh derivation (run tools/m68k_legal_forms.py)")
    r = subprocess.run([sys.executable, str(TOOL), "--check", "--output", str(FIXTURE)])
    check(r.returncode == 0, "--check reported a stale dataset")


def test_counts_and_vocabulary():
    data = json.loads(FIXTURE.read_text())
    cols = data["form_columns"]
    rows = [dict(zip(cols, r)) for r in data["forms"]]
    check(len(rows) == data["totals"]["forms"] == EXPECTED_TOTAL_FORMS, "total form count")
    check(len({r["id"] for r in rows}) == len(rows), "form ids are not unique")
    check(sum(r["words"] for r in rows) == data["totals"]["legal_primary_words"] == EXPECTED_LEGAL_WORDS, "legal word count")
    fam = {}
    for r in rows:
        f = fam.setdefault(r["family"], [0, 0])
        f[0] += 1
        f[1] += r["words"]
    check({k: tuple(v) for k, v in fam.items()} == EXPECTED_FAMILIES, "per-family counts (recomputed from rows)")
    check({k: (v["forms"], v["primary_words"]) for k, v in data["family_counts"].items()} == EXPECTED_FAMILIES,
          "recorded per-family counts")
    check(data["totals"]["partition_word_counts"] == EXPECTED_PARTITION, "partition counts")
    part = data["primary_word_partition"]["rows"]
    check(len(part) == 256 and all(len(r) == 256 for r in part), "partition shape")
    letters = "".join(part)
    legend = data["primary_word_partition"]["legend"]
    check(set(letters) <= set(legend), "partition uses unknown class letters")
    check({legend[k]: letters.count(k) for k in legend} == EXPECTED_PARTITION, "partition letters disagree with counts")
    check(letters.count("L") + letters.count("P") == EXPECTED_LEGAL_WORDS, "legal partition != form word total")
    priv_words = sum(r["words"] for r in rows if r["privilege"] == "supervisor")
    check(priv_words == letters.count("P"), "privileged words disagree between forms and partition")
    check(all(r["privilege"] in ("user", "supervisor") for r in rows), "privilege vocabulary")
    check(any(r["exceptions"] for r in rows) and any(r["auto_update"] != "none" for r in rows), "exception/auto-update classes present")
    vocab = data["vocabulary"]
    for key in ("legal_form", "architecturally_illegal", "architecturally_reserved_exception",
                "post_68000_encoding", "privileged", "unsupported_by_segarecomp"):
        check(key in vocab, "vocabulary lacks " + key)
    check("MEASUREMENT-SIDE" in vocab["unsupported_by_segarecomp"], "unsupported vocabulary must be measurement-side only")
    check("unsupported" not in json.dumps(data["forms"]), "dataset rows must not carry an 'unsupported' status")
    # 68010+ words that must trap on a 68000 are never legal forms.
    for w, name in ((0x4848, "BKPT"), (0x42C0, "MOVE from CCR"), (0x4E7A, "MOVEC"), (0x49C0, "EXTB.L"), (0x4E74, "RTD")):
        check(part[w >> 8][w & 255] == "X", "%s must be classified illegal_post_68000_encoding" % name)
    check(part[0x4A][0xFC] == "L", "ILLEGAL (0x4AFC) is a legal form")
    check(part[0xA0][0] == "A" and part[0xF0][0] == "F", "line A/F classes")


def _rows():
    data = json.loads(FIXTURE.read_text())
    return data, [dict(zip(data["form_columns"], r)) for r in data["forms"]]


def test_concrete_encoding_mapping():
    """Every aggregate form maps unambiguously to its exact concrete primary encodings."""
    data, rows = _rows()
    check("word_ranges" in data["form_columns"] and "concrete_encodings" in data, "concrete-encoding contract missing")
    owner = {}
    for r in rows:
        prev_hi = -2
        total = 0
        for lo, hi in r["word_ranges"]:
            check(0 <= lo <= hi <= 0xFFFF and lo > prev_hi + 1, "%s: ranges not ascending/non-adjacent/in-bounds" % r["id"])
            prev_hi = hi
            total += hi - lo + 1
            for w in range(lo, hi + 1):
                check(w not in owner, "word %04X owned by both %s and %s" % (w, owner.get(w), r["id"]))
                owner[w] = r["id"]
        check(total == r["words"], "%s: ranges expand to %d words, row says %d" % (r["id"], total, r["words"]))
    letters = "".join(data["primary_word_partition"]["rows"])
    legal = {w for w, c in enumerate(letters) if c in "LP"}
    check(set(owner) == legal, "union of form encodings != legal partition (holes or extras)")
    check(len(owner) == EXPECTED_LEGAL_WORDS, "concrete legal word count")
    by_id = {r["id"]: r for r in rows}
    for w, c in enumerate(letters):
        if c in "LP":
            check((by_id[owner[w]]["privilege"] == "supervisor") == (c == "P"), "privilege disagrees at %04X" % w)


def test_register_count_condition_variants_enumerable():
    _, rows = _rows()
    by_id = {r["id"]: r for r in rows}

    def words(fid):
        return {w for lo, hi in by_id[fid]["word_ranges"] for w in range(lo, hi + 1)}

    # register identity: ADDQ.W #q,An covers every An (incl. A0, A5, A7) and every quick value 1..8
    addq = words("addq.quick_ea.w.quick.an.quick1to8")
    for a in (0, 5, 7):
        for q in range(8):
            check(0x5048 | q << 9 | a in addq, "ADDQ.W An=%d quick=%d missing" % (a, q))
    # data8: every MOVEQ immediate for every Dn
    moveq = words("moveq.imm8_dn.l.imm8.dn.data8")
    check(len(moveq) == 2048 and all(0x7000 | d << 9 | i in moveq for d in range(8) for i in (0, 0x7F, 0x80, 0xFF)), "MOVEQ data8/Dn")
    # conditions stay fully distinguishable: 14 Bcc conditions, pairwise disjoint, distinct condition field
    bcc = [r for r in rows if r["mnemonic"] == "Bcc"]
    conds = {r["variant"] for r in bcc}
    check(len(conds) == 14, "Bcc must expose 14 conditions (BRA/BSR are separate forms)")
    for c in conds:
        fields = {(w >> 8) & 0xF for r in bcc if r["variant"] == c for lo, hi in r["word_ranges"] for w in range(lo, hi + 1)}
        check(len(fields) == 1, "Bcc condition %s spans multiple condition fields" % c)
    # the expansion CLI agrees with the fixture
    r = subprocess.run([sys.executable, str(TOOL), "--output", str(FIXTURE), "--expand", "moveq.imm8_dn.l.imm8.dn.data8"],
                       capture_output=True, text=True)
    check(r.returncode == 0 and sorted(int(x, 16) for x in r.stdout.split()) == sorted(moveq), "--expand disagrees with fixture")


def test_sweep_fixture_shape():
    fx = json.loads(SWEEP_FIXTURE.read_text())
    check(fx["unexplained_disagreements"] == 0, "sweep fixture records unexplained disagreements")
    for entry in fx["disagreements"]:
        check(entry["resolution"].strip() and entry["category"], "disagreement without resolution")
    check(sum(fx["outcome_matrix"].values()) == 0x10000, "sweep matrix must cover 65,536 words")


def main():
    for t in (test_independence, test_independence_detectors_bite, test_reproducible,
              test_counts_and_vocabulary, test_concrete_encoding_mapping,
              test_register_count_condition_variants_enumerable, test_sweep_fixture_shape):
        t()
    print("m68k legal-form dataset tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
