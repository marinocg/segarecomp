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


if __name__ == "__main__":
    unittest.main()
