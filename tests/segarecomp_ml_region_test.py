"""SEG-046: the experiment-only ML region proposer's deterministic, leakage-guarded, fail-closed pure-Python parts (synthetic data only).

No ML package is required here: training/propose import scikit-learn lazily and are exercised by the experiment itself.
"""
import hashlib
import importlib.util
import pathlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("ml_region", pathlib.Path(__file__).resolve().parents[1] / "tools" / "segarecomp_ml_region.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
SHA = "b" * 64


def universe_text(sha, addresses, count=None):
    body = "".join(f"{a:08x}\n" for a in addresses)
    return f"segarecomp.m68k_source_universe.v1\nrom_sha256 {sha}\nproducer p\nsource_revision r\nsource_config c\nentries {len(addresses) if count is None else count}\n{body}end\n"


def window_report(window_bytes, rows):
    head = f"segarecomp.m68k_window_features.v1 window_bytes {window_bytes}\ncolumns {' '.join(m.EXPECTED_DECODER_COLUMNS)}\n"
    return head + "".join(f"{start:08x} {' '.join(map(str, values))}\n" for start, values in rows) + "end\n"


class FeatureSchemaTest(unittest.TestCase):
    def test_schema_is_clean_and_hashed(self):
        names = m.feature_names()
        m.assert_feature_schema(names)
        self.assertEqual(len(names), 3 * len(m.base_feature_names()))
        self.assertNotEqual(m.schema_hash(256), m.schema_hash(512))
        self.assertEqual(m.schema_hash(256), m.schema_hash(256))

    def test_injected_address_feature_is_rejected(self):
        for bad in ("abs_address", "rom_offset", "window_ordinal", "title_code", "source_hit", "exec_count", "coverage_ratio", "window_start"):
            with self.assertRaises(SystemExit):
                m.assert_feature_schema(m.feature_names() + [bad])

    def test_duplicate_names_rejected(self):
        with self.assertRaises(SystemExit):
            m.assert_feature_schema(["a", "a"])


class ExtractionTest(unittest.TestCase):
    def rom(self):
        return bytes((i * 7 + (i >> 5)) & 0xFF for i in range(1024))

    def test_deterministic_and_independent_of_extraction_order(self):
        rom = self.rom()
        report = m.parse_window_report(window_report(256, [(0, [3] + [0] * 26), (0x200, [5] + [1] * 26)]), 256)
        first, count = m.window_matrix(rom, report, 256)
        self.assertEqual(count, 4)
        self.assertEqual(first, m.window_matrix(rom, report, 256)[0])
        # per-window features do not depend on other windows' extraction order (only on declared neighbour context)
        independent = [m.byte_features(rom[w * 256:(w + 1) * 256]) for w in reversed(range(4))][::-1]
        self.assertEqual([row[:len(m.BYTE_FEATURE_NAMES)] for row in first], independent)
        self.assertTrue(all(len(row) == len(m.feature_names()) for row in first))

    def test_no_title_or_path_dependence(self):
        rom = self.rom()
        report = m.parse_window_report(window_report(256, []), 256)
        self.assertEqual(m.window_matrix(rom, report, 256), m.window_matrix(bytes(rom), report, 256))

    def test_neighbour_context_is_previous_and_next_base_vector(self):
        rom = self.rom()
        report = m.parse_window_report(window_report(256, []), 256)
        rows, _ = m.window_matrix(rom, report, 256)
        n = len(m.base_feature_names())
        self.assertEqual(rows[1][n:2 * n], rows[0][:n])
        self.assertEqual(rows[1][2 * n:], rows[2][:n])
        self.assertEqual(rows[0][n:2 * n], [0.0] * n)
        self.assertEqual(rows[3][2 * n:], [0.0] * n)

    def test_window_report_fail_closed(self):
        good = window_report(256, [(0, [1] * 27)])
        m.parse_window_report(good, 256)
        for bad in (good.replace("window_bytes 256", "window_bytes 512"), good.replace("ident ", "idnt "), good.replace("\nend\n", "\n"),
                    window_report(256, [(0x80, [1] * 27)]), window_report(256, [(0, [1] * 26)]), window_report(256, [(0, [1] * 27), (0, [1] * 27)])):
            with self.assertRaises(SystemExit):
                m.parse_window_report(bad, 256)


class LabelTest(unittest.TestCase):
    def test_labels_and_containment(self):
        universe = m.read_universe(universe_text(SHA, [0x0, 0x102, 0x300]), SHA, 0x400)
        self.assertEqual(m.window_labels(universe, 0x400, 256), [1, 1, 0, 1])
        self.assertEqual(m.uncovered_source_addresses(universe, {0, 1, 3}, 256), [])
        self.assertEqual(m.uncovered_source_addresses(universe, {0, 3}, 256), [0x102])  # a dropped source-positive window is detected

    def test_wrong_rom_hash_or_malformed_universe_fails_closed(self):
        good = universe_text(SHA, [0x0, 0x2])
        for bad in (good.replace(SHA, "c" * 64), universe_text(SHA, [0x2, 0x0]), universe_text(SHA, [0x1]), universe_text(SHA, [0x0], count=2),
                    universe_text(SHA, [0x800]), good.replace("end\n", "")):
            with self.assertRaises(SystemExit):
                m.read_universe(bad, SHA, 0x400)

    def test_seed_windows_and_regions(self):
        self.assertEqual(m.seed_windows("0 4 1f0 200 2000000", 0x400, 256), {0, 1, 2})
        text = m.regions_text(SHA, 0x400, {0, 1, 3}, 256)
        self.assertEqual(text, f"segarecomp.m68k_executable_regions.v1\nrom_sha256 {SHA}\nrange 00000000 00000200\nrange 00000300 00000400\nend\n")
        for bad_windows in (set(),):
            with self.assertRaises(SystemExit):
                m.regions_text(SHA, 0x400, bad_windows, 256)


class CalibrationMutationTest(unittest.TestCase):
    def selected(self, scores, threshold, seeds=()):
        return {w for w, v in enumerate(scores) if v >= threshold} | set(seeds)

    def test_forced_low_score_for_a_source_positive_window_fails_containment(self):
        universe = m.read_universe(universe_text(SHA, [0x0, 0x102, 0x300]), SHA, 0x400)
        scores = [0.9, 0.8, 0.1, 0.7]
        self.assertEqual(m.uncovered_source_addresses(universe, self.selected(scores, 0.5), 256), [])
        scores[1] = 0.0  # force a low score on a positive window
        self.assertEqual(m.uncovered_source_addresses(universe, self.selected(scores, 0.5), 256), [0x102])
        self.assertEqual(m.uncovered_source_addresses(universe, self.selected(scores, 0.5, seeds={1}), 256), [])

    def test_committed_frozen_definition_is_consistent(self):
        import json
        frozen = json.loads(pathlib.Path(__file__).resolve().parents[1].joinpath("tools", "segarecomp_ml_region.frozen.json").read_text())
        m.require_frozen(frozen)
        self.assertEqual((frozen["window_bytes"], frozen["model"], frozen["seed"], frozen["feature_count"]), (512, "logreg", 46, len(m.feature_names())))
        self.assertEqual(frozen["training_rom_sha256"], "46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6")
        self.assertEqual(sorted(frozen["environment"]), ["numpy", "python", "sklearn"])


TOOLS = pathlib.Path(__file__).resolve().parents[1] / "tools"


class CanonicalModelTest(unittest.TestCase):
    def setUp(self):
        import json
        self.json = json
        self.frozen = json.loads((TOOLS / "segarecomp_ml_region.frozen.json").read_text())
        self.text = (TOOLS / "segarecomp_ml_region.model.json").read_text()
        self.model = json.loads(self.text)

    def vectors(self):
        # deterministic synthetic 141-element vectors (not ROM-derived)
        out = []
        state = 12345
        for _ in range(20):
            row = []
            for _ in range(141):
                state = (state * 1103515245 + 12345) & 0x7FFFFFFF
                row.append(state / 0x7FFFFFFF * 2.0 - 0.5)
            out.append(row)
        return out

    def test_artifact_matches_frozen_definition(self):
        m.validate_canonical_model(self.model, self.frozen)
        self.assertEqual(self.model["feature_count"], 141)
        for key in ("scaler_mean", "scaler_scale", "coef", "folded_weight"):
            self.assertEqual(len(self.model[key]), 141)
        self.assertEqual(self.model["probability_threshold"], 0.00835406801187952)
        self.assertEqual(self.model["feature_schema_sha256"], "d2e7c82913139c29450511326d6de76e91579ec654edde81afae16b13cd1f570")
        self.assertEqual(self.model["original_sklearn_artifact_sha256"], "b5ddae5aa0fe6571455803c244d2a6be4a2c3349e3436c8780bfc3d8aa8fa62b")
        self.assertEqual(self.model["feature_names"], m.feature_names())
        m.assert_feature_schema(self.model["feature_names"])

    def test_export_is_deterministic_and_rederivable(self):
        again = m.canonical_model_from_arrays(self.frozen, self.model["scaler_mean"], self.model["scaler_scale"], self.model["coef"],
                                              self.model["intercept"])
        self.assertEqual(m.canonical_json(again), self.text)

    def test_folded_and_unfolded_agree(self):
        for row in self.vectors():
            a, b = m.canonical_logit(self.model, row), m.canonical_logit(self.model, row, folded=False)
            self.assertLess(abs(a - b), 1e-9 * max(1.0, abs(a)))

    def test_decision_is_exact_around_the_boundary(self):
        import math
        row = self.vectors()[0]
        k = max(range(141), key=lambda i: abs(self.model["folded_weight"][i]))
        weight = self.model["folded_weight"][k]
        gap = self.model["logit_threshold"] - m.canonical_logit(self.model, row)
        for delta, expected in ((1e-6, True), (-1e-6, False)):
            shifted = list(row)
            shifted[k] += (gap + delta) / weight
            self.assertEqual(m.canonical_logit(self.model, shifted) >= self.model["logit_threshold"], expected)
        p = self.model["probability_threshold"]
        self.assertEqual(self.model["logit_threshold"], math.log(p / (1.0 - p)))
        self.assertEqual(m.canonical_selected(self.model, [row, shifted]), m.canonical_selected(self.model, [row, shifted]))

    def test_malformed_models_fail_closed(self):
        import copy
        def broken(mutate):
            model = copy.deepcopy(self.model)
            mutate(model)
            with self.assertRaises(SystemExit):
                m.validate_canonical_model(model, self.frozen)
        broken(lambda x: x["coef"].pop())
        broken(lambda x: x.__setitem__("scaler_scale", x["scaler_scale"][:-1] + [0.0]))
        broken(lambda x: x.__setitem__("schema", "other"))
        broken(lambda x: x.__setitem__("feature_version", "v0"))
        broken(lambda x: x.__setitem__("feature_schema_sha256", "0" * 64))
        broken(lambda x: x.__setitem__("original_sklearn_artifact_sha256", "0" * 64))
        broken(lambda x: x.__setitem__("probability_threshold", 0.01))
        broken(lambda x: x.__setitem__("logit_threshold", x["logit_threshold"] + 1e-9))
        broken(lambda x: x.__setitem__("folded_bias", float("nan")))
        broken(lambda x: x.__setitem__("class_order", [1, 0]))
        with self.assertRaises(SystemExit):
            m.canonical_logit(self.model, [0.0] * 140)

    def test_no_forbidden_data_in_artifacts(self):
        parity = self.json.loads((TOOLS / "segarecomp_ml_region.parity.json").read_text())
        self.assertEqual(sorted(parity["titles"]), ["cs", "or", "s1", "s2", "sor"])
        for entry in parity["titles"].values():
            self.assertTrue(entry["selected_equal_sklearn_vs_canonical"])
            for key in ("rom_sha256", "regions_sha256", "ml_only_regions_sha256", "sklearn_proba_float64_le_sha256"):
                self.assertRegex(entry[key], r"^[0-9a-f]{64}$")
            self.assertTrue(all(not isinstance(v, (list, dict)) for v in entry.values()))  # aggregates and digests only
        for text in (self.text, (TOOLS / "segarecomp_ml_region.parity.json").read_text()):
            self.assertNotIn("range ", text)
        self.assertEqual(sorted(k for k in self.model if isinstance(self.model[k], list)),
                         sorted(["class_order", "coef", "feature_names", "folded_weight", "scaler_mean", "scaler_scale"]))


class CrossValidationTest(unittest.TestCase):
    def test_blocked_folds_are_contiguous_purged_and_cover_everything_once(self):
        folds = m.blocked_folds(2048, 16, 2)
        self.assertEqual(len(folds), 16)
        seen = []
        for train, test in folds:
            self.assertEqual(test, list(range(test[0], test[-1] + 1)))
            self.assertFalse(set(train) & set(range(test[0] - 2, test[-1] + 3)))
            seen += test
        self.assertEqual(seen, list(range(2048)))

    def test_threshold_is_the_lowest_positive_score(self):
        self.assertEqual(m.threshold_at_full_recall([0.9, 0.1, 0.5, 0.2], [1, 0, 1, 0]), 0.5)
        with self.assertRaises(SystemExit):
            m.threshold_at_full_recall([0.1], [0])


class FreezeGuardTest(unittest.TestCase):
    def test_oracle_inputs_refused_before_freeze(self):
        for path in ("/x/coverage.bitmap", "/x/rt-oracle.out", "/x/foo.bitmap"):
            with self.assertRaises(SystemExit):
                m.refuse_oracle_inputs([path])
        m.refuse_oracle_inputs(["/x/rom.md", "/x/universe.txt"])

    def test_unfrozen_or_mismatched_definition_rejected(self):
        with self.assertRaises(SystemExit):
            m.require_frozen({"status": "DRAFT"})
        frozen = {"status": "FROZEN", "model_artifact_sha256": "d" * 64, "window_bytes": 256, "feature_schema_sha256": "e" * 64}
        with self.assertRaises(SystemExit):
            m.require_frozen(frozen)
        frozen["feature_schema_sha256"] = m.schema_hash(256)
        m.require_frozen(frozen)

    def test_artifact_hash_mismatch_is_rejected_before_loading(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp, "model.bin")
            path.write_bytes(b"not a pickle")
            with self.assertRaises(SystemExit):
                m.load_artifact(str(path), "0" * 64)
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), hashlib.sha256(b"not a pickle").hexdigest())


class NativeEmbeddingDriftTest(unittest.TestCase):
    """SEG-047 (ADR 0096): the checked-in native model table and the vendored zlib subset must not drift."""

    def test_native_model_table_matches_committed_model_json(self):
        gen = importlib.util.spec_from_file_location("native_data", pathlib.Path(__file__).resolve().parent.parent / "tools" / "segarecomp_ml_region_native_data.py")
        mod = importlib.util.module_from_spec(gen)
        gen.loader.exec_module(mod)
        self.assertEqual(mod.OUTPUT.read_text(), mod.render(mod.MODEL.read_bytes()))

    def test_vendored_zlib_files_match_recorded_digests(self):
        root = pathlib.Path(__file__).resolve().parent.parent / "third_party" / "zlib"
        recorded = dict(line.split("  ")[::-1] for line in (root / "README.md").read_text().split("```")[1].strip().splitlines())
        self.assertEqual(len(recorded), 10)
        for name, digest in recorded.items():
            self.assertEqual(hashlib.sha256((root / name).read_bytes()).hexdigest(), digest, name)
        self.assertIn('#define ZLIB_VERSION "1.3.1"', (root / "zlib.h").read_text())


if __name__ == "__main__":
    unittest.main()
