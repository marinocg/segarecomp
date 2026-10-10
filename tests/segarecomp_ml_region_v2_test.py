#!/usr/bin/env python3
"""SEG-048 (ADR 0099): hermetic tests for tools/segarecomp_ml_region_v2.py. Synthetic data only; no scikit-learn, no ROM, no commercial data."""
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
sys.path.insert(0, TOOLS)
import segarecomp_ml_region as v1  # noqa: E402
import segarecomp_ml_region_v2 as m  # noqa: E402


def put(path, text, mode="w"):
    with open(path, mode, newline="\n") as sink:
        sink.write(text)


def get(path):
    with open(path, newline="") as source:
        return source.read()


def fake_title(tid, base_marker, labels, windows=None, truth=None, universe=None, seeds=()):
    t = m.Title.__new__(m.Title)
    count = windows or len(labels)
    t.id, t.count = tid, count
    t.rom = bytes(count * m.WINDOW_BYTES)
    t.sha = hashlib.sha256(tid.encode()).hexdigest()
    t.rom_path = "x"
    width = len(v1.base_feature_names())
    t.base = [[float(base_marker)] * width for _ in range(count)]
    t.labels = list(labels)
    t.seeds = set(seeds)
    t.truth = truth if truth is not None else {w * m.WINDOW_BYTES for w, y in enumerate(labels) if y}
    t.universe = universe if universe is not None else sorted(t.truth | {w * m.WINDOW_BYTES + 2 for w in range(count)})
    t.truth_sha = "0" * 64
    return t


class FakeRunner:
    def __init__(self, ranges_from_regions=True):
        self.calls = 0

    def run(self, title, regions):
        self.calls += 1
        ranges = [(int(l.split()[1], 16), int(l.split()[2], 16)) for l in regions.splitlines() if l.startswith("range ")]
        k = m.admitted_set(title.universe, ranges)
        return {"returncode": 0, "validator": "accepted", "k": str(len(k)), "k0": str(len(k)), "pruned": "0", "rounds": "1", "ranges": ranges}


class Schemas(unittest.TestCase):
    def test_sizes_and_f0_equals_v1(self):
        self.assertEqual(len(m.schema_names("F0")), 141)
        self.assertEqual(m.schema_names("F0"), v1.feature_names())
        self.assertEqual(m.schema_hash("F0"), "d2e7c82913139c29450511326d6de76e91579ec654edde81afae16b13cd1f570")
        self.assertEqual({k: len(m.schema_names(k)) for k in m.CANDIDATES}, {"F0": 141, "F1": 138, "F2": 138, "F3": 46, "F4": 93, "F5": 45})
        self.assertEqual(len({m.schema_hash(k) for k in m.CANDIDATES}), 6)

    def test_candidates_are_clean(self):
        for key in m.CANDIDATES:
            m.assert_schema(m.schema_names(key))
            if key != "F0":
                self.assertFalse(any("zlib" in n for n in m.schema_names(key)))
        self.assertTrue(any(n.startswith("b_") for n in m.schema_names("F5")) and not any("d_" in n[:6] for n in m.schema_names("F5")))
        self.assertFalse(any(n.split("_")[-1] == "x" or n.startswith(("b_", "prev_b_", "next_b_")) for n in m.schema_names("F4")))

    def test_forbidden_features_rejected(self):
        for bad in ("title_id", "rom_sha", "abs_address", "rom_offset", "window_ordinal", "page_index", "source_project", "path", "filename", "symbol",
                    "label_count", "truth_count", "runtime_count", "exec_pc", "coverage", "frame_no", "annotation", "rom_size", "game_id", "hash",
                    "normalized_addr", "window_position"):
            with self.assertRaises(SystemExit, msg=bad):
                m.assert_schema(["b_entropy", bad])
        with self.assertRaises(SystemExit):
            m.assert_schema(["b_entropy", "b_entropy"])

    def test_edge_treatments(self):
        width = len(v1.base_feature_names())
        base = [[float(w + 1)] * width for w in range(3)]
        zero = m.build_rows(base, "F1")
        self.assertEqual(zero[0][46:92], [0.0] * 46)             # missing previous neighbour = all-zero sentinel (F0/F1)
        rep = m.build_rows(base, "F2")
        self.assertEqual(rep[0][46:92], rep[0][:46])              # replicate the window's own base vector
        self.assertEqual(rep[2][92:], rep[2][:46])
        self.assertEqual(rep[1][46:92], [1.0] * 46)
        none = m.build_rows(base, "F3")
        self.assertEqual(len(none[0]), 46)
        self.assertEqual(len(m.build_rows(base, "F0")[0]), 141)


class Folds(unittest.TestCase):
    def titles(self):
        return {"a": fake_title("a", 1.0, [1, 1, 0, 0]), "b": fake_title("b", 2.0, [1, 0, 0, 0]), "c": fake_title("c", 3.0, [1, 1, 1, 0]),
                "d": fake_title("d", 99.0, [1, 0, 1, 0])}

    def test_held_out_excluded_from_fit_scaler_threshold(self):
        seen = []

        def fit(rows, labels):
            seen.append((rows, labels))
            mean = sum(r[0] for r in rows) / len(rows)  # a "scaler statistic" computed from exactly the rows given
            seen[-1] = (rows, labels, mean)
            return lambda x: [r[0] / 1000.0 for r in x]

        record = m.run_fold(self.titles(), "d", "F0", fit, 1.0, FakeRunner())
        rows, labels, mean = seen[0]
        self.assertTrue(all(r[0] in (1.0, 2.0, 3.0) for r in rows))   # no held-out (99.0) row reached the fit/scaler
        self.assertAlmostEqual(mean, (1.0 * 4 + 2.0 * 4 + 3.0 * 4) / 12)
        self.assertEqual(record["train_titles"], ["a", "b", "c"])
        # threshold = factor x min positive TRAINING probability = 1.0/1000 (held-out 99.0 would otherwise change nothing here, so also check P2)
        self.assertAlmostEqual(record["threshold"], 1.0 / 1000.0)
        half = m.run_fold(self.titles(), "d", "F0", fit, 0.5, FakeRunner())
        self.assertAlmostEqual(half["threshold"], 0.5 / 1000.0)
        self.assertAlmostEqual(half["train_min_positive"], 1.0 / 1000.0)

    def test_threshold_ignores_held_out_even_when_it_is_the_minimum(self):
        titles = self.titles()
        titles["d"].base = [[0.0001] * len(v1.base_feature_names()) for _ in range(4)]  # held-out would give the smallest score
        record = m.run_fold(titles, "d", "F0", lambda r, l: (lambda x: [row[0] / 1000.0 for row in x]), 1.0, FakeRunner())
        self.assertAlmostEqual(record["threshold"], 1.0 / 1000.0)
        self.assertEqual(record["c_outside_r"], 2)   # held-out positives scoring 1e-7 are NOT rescued by the threshold
        self.assertEqual(record["validator"], "rejected")   # an empty region is never sent to the prune

    def test_loto_runs_every_title_held_out_once(self):
        result = m.run_loto(self.titles(), "F0", lambda r, l: (lambda x: [0.5] * len(x)), 1.0, FakeRunner(), workers=1)
        self.assertEqual(sorted(f["title"] for f in result["folds"]), ["a", "b", "c", "d"])
        for fold in result["folds"]:
            self.assertNotIn(fold["title"], fold["train_titles"])
            self.assertEqual(len(fold["train_titles"]), 3)

    def test_source_positive_omission_fails_containment_and_gate(self):
        t = fake_title("z", 1.0, [1, 1, 0, 0])
        record = m.evaluate(t, [0.9, 0.0, 0.0, 0.0], 0.5, 0.1, FakeRunner())   # window 1 is a source positive scoring 0.0
        self.assertEqual(record["c_outside_r"], 1)
        self.assertEqual(record["c_outside_k"], 1)
        self.assertFalse(m.gates_pass(record))
        full = m.evaluate(t, [0.9, 0.8, 0.0, 0.0], 0.1, 0.1, FakeRunner())
        self.assertEqual((full["c_outside_r"], full["c_outside_k"]), (0, 0))
        self.assertAlmostEqual(full["margin_factor"], 8.0)

    def test_gate_arithmetic(self):
        base = {"c_outside_r": 0, "c_outside_k": 0, "validator": "accepted", "margin_factor": 2.0, "k_over_u": 0.6}
        self.assertTrue(m.gates_pass(base))
        for key, value in (("c_outside_r", 1), ("c_outside_k", 1), ("validator", "rejected"), ("margin_factor", 1.999), ("k_over_u", 0.6001)):
            self.assertFalse(m.gates_pass(dict(base, **{key: value})), key)

    def test_admitted_set(self):
        self.assertEqual(m.admitted_set([0, 2, 4, 6, 8], [(2, 6), (8, 10)]), {2, 4, 8})
        self.assertEqual(m.admitted_set([0, 2], []), set())

    def test_seeds_count_towards_r(self):
        t = fake_title("s", 1.0, [1, 0], seeds={0})
        record = m.evaluate(t, [0.0, 0.0], 0.5, 0.1, FakeRunner())
        self.assertEqual(record["c_outside_r"], 0)   # the certain-code union keeps the window


class Barrier(unittest.TestCase):
    def write_universe(self, root, name, sha):
        os.makedirs(os.path.dirname(os.path.join(root, name)), exist_ok=True)
        with open(os.path.join(root, name), "w", newline="\n") as sink:
            sink.write(f"segarecomp.m68k_source_universe.v1\nrom_sha256 {sha}\nproducer p\nsource_revision {'0' * 40}\nsource_config none\nentries 1\n00000000\nend\n")

    def test_seal_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.write_universe(tmp, "ok.u", "1" * 64)
            self.assertEqual(m.seal_check(tmp), [])
            sealed = next(iter(m.SEALED_ROM_SHA256))
            self.write_universe(tmp, "nested/x.u", sealed)
            self.assertEqual(len(m.seal_check(tmp)), 1)
            self.assertEqual(m.seal_check(os.path.join(tmp, "missing")), [])

    def test_blind_materialization_requires_freeze(self):
        with tempfile.TemporaryDirectory() as tmp:
            rom = os.path.join(tmp, "r.bin")
            with open(rom, "wb") as sink:
                sink.write(b"\x00" * 16)
            sha = hashlib.sha256(b"\x00" * 16).hexdigest()
            old = dict(m.SEALED_ROM_SHA256)
            m.SEALED_ROM_SHA256[sha] = "T"
            try:
                tools = os.path.join(tmp, "tools")
                os.makedirs(tools)
                with self.assertRaises(SystemExit):
                    m.materialize_truth("x.lst", rom, os.path.join(tmp, "out"), [], tools, None)
                self.assertFalse(os.path.exists(os.path.join(tmp, "out")))
            finally:
                m.SEALED_ROM_SHA256.clear()
                m.SEALED_ROM_SHA256.update(old)

    def test_sealed_rom_requires_freeze_commit_even_with_a_valid_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            tools = self.make_repo(tmp)
            rom = os.path.join(tmp, "r.bin")
            with open(rom, "wb") as sink:
                sink.write(b"\x00" * 16)
            sha = hashlib.sha256(b"\x00" * 16).hexdigest()
            m.SEALED_ROM_SHA256[sha] = "T"
            try:
                with self.assertRaises(SystemExit):
                    m.materialize_truth("x.lst", rom, os.path.join(tmp, "out"), [], tools, None)
            finally:
                del m.SEALED_ROM_SHA256[sha]

    def test_oracle_and_coverage_inputs_refused(self):
        for name in ("run.coverage.txt", "execution_oracle.bin", "x.bitmap"):
            with self.assertRaises(SystemExit):
                m.parse_title_spec(f"a:rom:w:{name}:u")

    def make_repo(self, root, no_candidate=False):
        tools = os.path.join(root, "tools")
        os.makedirs(tools)
        for name in m.V1_ARTIFACTS:
            with open(os.path.join(tools, name), "w", newline="\n") as sink:
                sink.write(f"v1 {name}\n")
        if no_candidate:
            frozen = {"schema": m.FREEZE_SCHEMA, "status": "NO_QUALIFYING_CANDIDATE",
                      "v1_artifact_sha256": {n: hashlib.sha256(get(os.path.join(tools, n)).encode()).hexdigest() for n in m.V1_ARTIFACTS}}
            with open(os.path.join(tools, "segarecomp_ml_region_v2.frozen.json"), "w", newline="\n") as sink:
                sink.write(json.dumps(frozen, indent=1, sort_keys=True) + "\n")
            return tools
        names = m.schema_names("F2")
        model = {"feature_schema_sha256": m.schema_hash("F2"), "probability_threshold": 0.25, "feature_version": m.CANDIDATES["F2"]["version"],
                 "feature_names": names}
        model_text = json.dumps(model, sort_keys=True) + "\n"
        parity_text = json.dumps({"folds": []}) + "\n"
        with open(os.path.join(tools, "segarecomp_ml_region_v2.model.json"), "w", newline="\n") as sink:
            sink.write(model_text)
        with open(os.path.join(tools, "segarecomp_ml_region_v2.parity.json"), "w", newline="\n") as sink:
            sink.write(parity_text)
        frozen = {"schema": m.FREEZE_SCHEMA, "status": "FROZEN", "candidate": "F2", "feature_schema_sha256": m.schema_hash("F2"), "threshold": 0.25,
                  "feature_version": m.CANDIDATES["F2"]["version"], "model_json_sha256": hashlib.sha256(model_text.encode()).hexdigest(),
                  "parity_json_sha256": hashlib.sha256(parity_text.encode()).hexdigest()}
        with open(os.path.join(tools, "segarecomp_ml_region_v2.frozen.json"), "w", newline="\n") as sink:
            sink.write(json.dumps(frozen, indent=1, sort_keys=True) + "\n")
        return tools

    def test_valid_freeze_and_mutations(self):
        with tempfile.TemporaryDirectory() as tmp:
            tools = self.make_repo(tmp)
            self.assertEqual(m.verify_freeze(tools)["candidate"], "F2")
            path = os.path.join(tools, "segarecomp_ml_region_v2.model.json")
            original = get(path)
            # modified model, modified threshold, schema reorder
            put(path, original.replace("0.25", "0.26"))
            with self.assertRaises(SystemExit):
                m.verify_freeze(tools)
            model = json.loads(original)
            model["feature_names"] = list(reversed(model["feature_names"]))
            text = json.dumps(model, sort_keys=True) + "\n"
            put(path, text)
            frozen_path = os.path.join(tools, "segarecomp_ml_region_v2.frozen.json")
            frozen = json.loads(get(frozen_path))
            frozen["model_json_sha256"] = hashlib.sha256(text.encode()).hexdigest()   # even with a re-pinned digest the reorder is caught
            put(frozen_path, json.dumps(frozen, indent=1, sort_keys=True) + "\n")
            with self.assertRaises(SystemExit):
                m.verify_freeze(tools)
            put(path, original)
            frozen["model_json_sha256"] = hashlib.sha256(original.encode()).hexdigest()
            frozen["threshold"] = 0.5   # modified frozen identity
            put(frozen_path, json.dumps(frozen, indent=1, sort_keys=True) + "\n")
            with self.assertRaises(SystemExit):
                m.verify_freeze(tools)
            frozen["threshold"] = 0.25
            frozen["status"] = "DRAFT"
            put(frozen_path, json.dumps(frozen, indent=1, sort_keys=True) + "\n")
            with self.assertRaises(SystemExit):
                m.verify_freeze(tools)

    def git(self, root, *args):
        subprocess.run(["git", "-C", root, "-c", "core.autocrlf=false", "-c", "user.name=t", "-c", "user.email=t@example.invalid", *args], check=True, capture_output=True)

    def test_post_freeze_mutation_and_v1_drift_detected_through_git(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.git(tmp, "init", "-q")
            tools = self.make_repo(tmp)
            self.git(tmp, "add", "-A")
            self.git(tmp, "commit", "-q", "-m", "freeze")
            sha = subprocess.run(["git", "-C", tmp, "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
            self.assertEqual(m.verify_freeze(tools, sha, tmp)["status"], "FROZEN")
            self.assertEqual(m.v1_untouched(tools, sha, tmp), [])
            # post-freeze mutation of the candidate (frozen record and model kept self-consistent) differs from the freeze commit
            path = os.path.join(tools, "segarecomp_ml_region_v2.parity.json")
            text = json.dumps({"folds": [1]}) + "\n"
            put(path, text)
            frozen_path = os.path.join(tools, "segarecomp_ml_region_v2.frozen.json")
            frozen = json.loads(get(frozen_path))
            frozen["parity_json_sha256"] = hashlib.sha256(text.encode()).hexdigest()
            put(frozen_path, json.dumps(frozen, indent=1, sort_keys=True) + "\n")
            self.assertEqual(m.verify_freeze(tools)["status"], "FROZEN")   # internally consistent...
            with self.assertRaises(SystemExit):
                m.verify_freeze(tools, sha, tmp)                            # ...but not the frozen commit
            put(os.path.join(tools, "segarecomp_ml_region.model.json"), "drift\n", "a")
            self.assertEqual(m.v1_untouched(tools, sha, tmp), ["segarecomp_ml_region.model.json"])
            with self.assertRaises(SystemExit):
                m.verify_freeze(tools, "f" * 40, tmp)

    def test_no_candidate_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            tools = self.make_repo(tmp, no_candidate=True)
            self.assertEqual(m.verify_freeze(tools)["status"], "NO_QUALIFYING_CANDIDATE")
            put(os.path.join(tools, "segarecomp_ml_region.frozen.json"), "drift\n", "a")
            with self.assertRaises(SystemExit):
                m.verify_freeze(tools)

    def test_missing_record_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(SystemExit):
                m.verify_freeze(tmp)


class Policy(unittest.TestCase):
    def test_threshold_policy(self):
        t, low = m.derive_threshold([0.9, 0.2, 0.05, 0.7], [1, 1, 0, 1], 0.5)
        self.assertEqual((t, low), (0.1, 0.2))
        with self.assertRaises(SystemExit):
            m.derive_threshold([0.1], [0], 1.0)
        self.assertEqual(m.POLICIES, {"P0": 1.0, "P1": 0.5, "P2": 0.25})
        self.assertEqual(m.LOGREG_C, (0.25, 1.0, 4.0))

    def test_canonical_v2_matches_direct_logistic(self):
        core = {"feature_names": ["a", "b"], "threshold": 0.5, "feature_version": "x", "feature_schema_sha256": "0" * 64, "training": {}}
        model = m.canonical_v2(core, [1.0, 2.0], [2.0, 4.0], [0.5, -0.25], 0.1)
        row = [3.0, 6.0]
        direct = 0.1 + 0.5 * (3.0 - 1.0) / 2.0 + (-0.25) * (6.0 - 2.0) / 4.0
        self.assertAlmostEqual(v1.canonical_logit(model, row) if False else model["folded_bias"] + sum(w * x for w, x in zip(model["folded_weight"], row)), direct)
        with self.assertRaises(SystemExit):
            m.canonical_v2(core, [1.0], [2.0, 4.0], [0.5, -0.25], 0.1)


if __name__ == "__main__":
    unittest.main()
