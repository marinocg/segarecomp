#!/usr/bin/env python3
"""SEG-073 (ADR 0155): hermetic tests of the offline M68K ML failure diagnosis. Synthetic data only; standard library only."""
import hashlib
import json
import os
import pathlib
import sys
import tempfile
import unittest

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
sys.path.insert(0, TOOLS)
import segarecomp_ml_v3_diagnose as dg  # noqa: E402


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def make_bundle(root: pathlib.Path, files: dict, external: dict | None = None) -> pathlib.Path:
    payload = root / "payload"
    root.mkdir(parents=True, exist_ok=True)
    recs = {}
    for rel, data in files.items():
        p = payload / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        recs[rel] = {"sha256": sha(data), "bytes": len(data)}
    man = {"schema": "segarecomp.ml_v3_handoff.v1", "files": recs, "external_inputs": external or {}}
    (root / "MANIFEST.json").write_text(json.dumps(man))
    (root / "MANIFEST.sha256").write_text(sha((root / "MANIFEST.json").read_bytes()) + "  MANIFEST.json\n")
    return root


class BundleResolution(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_missing_manifest_is_typed(self):
        with self.assertRaises(dg.ArtifactError) as e:
            dg.Bundle(self.root / "nothing")
        self.assertEqual(e.exception.kind, "missing")

    def test_manifest_digest_mismatch(self):
        b = make_bundle(self.root / "b", {"seg072/x.bin": b"abc"})
        (b / "MANIFEST.sha256").write_text("0" * 64 + "  MANIFEST.json\n")
        with self.assertRaises(dg.ArtifactError) as e:
            dg.Bundle(b)
        self.assertEqual(e.exception.kind, "manifest_mismatch")

    def test_payload_digest_mismatch_and_missing_file(self):
        b = make_bundle(self.root / "b", {"seg072/x.bin": b"abc", "seg072/y.bin": b"def"})
        (b / "payload/seg072/x.bin").write_bytes(b"abd")
        (b / "payload/seg072/y.bin").unlink()
        bundle = dg.Bundle(b)
        with self.assertRaises(dg.ArtifactError) as e:
            bundle.file("seg072/x.bin")
        self.assertEqual(e.exception.kind, "digest_mismatch")
        with self.assertRaises(dg.ArtifactError) as e:
            bundle.file("seg072/y.bin")
        self.assertEqual(e.exception.kind, "missing")
        with self.assertRaises(dg.ArtifactError) as e:
            bundle.file("seg072/z.bin")
        self.assertEqual(e.exception.kind, "missing")

    def test_bundled_build_resolves_through_payload_not_archived_path(self):
        rom = b"\x4e\x75" * 64
        b = make_bundle(self.root / "b", {"seg072/hb/d09/inst/out/rom.bin": rom})
        meta = {"id": "d09", "image": "/somewhere/else/.cache/seg072/hb/d09/inst/out/rom.bin", "sha256": sha(rom)}
        p = dg.Bundle(b).resolve_image(meta)
        self.assertEqual(p, (b / "payload/seg072/hb/d09/inst/out/rom.bin").resolve())
        self.assertEqual(dg.Bundle(b).original_roots(), [])

    def test_external_input_rebase_and_digest(self):
        img = b"\x00\x01" * 32
        ext = self.root / "games"
        ext.mkdir()
        (ext / "a.bin").write_bytes(img)
        b = make_bundle(self.root / "b", {}, {"s1": {"path": "/old/games/a.bin", "sha256": sha(img), "bytes": len(img)}})
        with self.assertRaises(dg.ArtifactError) as e:
            dg.Bundle(b).resolve_image({"id": "s1", "image": "/old/games/a.bin", "sha256": sha(img)})
        self.assertEqual(e.exception.kind, "missing")
        bundle = dg.Bundle(b, {"/old/games": str(ext)})
        self.assertEqual(bundle.resolve_image({"id": "s1", "image": "/old/games/a.bin", "sha256": sha(img)}), ext / "a.bin")
        (ext / "a.bin").write_bytes(img[:-1] + b"\x02")
        with self.assertRaises(dg.ArtifactError) as e:
            bundle.resolve_image({"id": "s1", "image": "/old/games/a.bin", "sha256": sha(img)})
        self.assertEqual(e.exception.kind, "digest_mismatch")
        with self.assertRaises(dg.ArtifactError) as e:
            bundle.resolve_image({"id": "unknown", "image": "/x"})
        self.assertEqual(e.exception.kind, "unknown_image")

    def test_meta_identity_must_match_resolved_image(self):
        rom = b"\x4e\x71" * 8
        b = make_bundle(self.root / "b", {"seg072/hb/d05/inst/out/rom.bin": rom})
        with self.assertRaises(dg.ArtifactError) as e:
            dg.Bundle(b).resolve_image({"id": "d05", "image": "/x/seg072/hb/d05/inst/out/rom.bin", "sha256": "f" * 64})
        self.assertEqual(e.exception.kind, "digest_mismatch")

    def test_original_roots_and_forbidden_opens(self):
        meta = json.dumps({"id": "d09", "image": "/orig/.cache/seg072/hb/d09/inst/out/rom.bin"}).encode()
        b = make_bundle(self.root / "b", {"seg072/data/d09/meta.json": meta})
        roots = dg.Bundle(b).original_roots()
        self.assertEqual(roots, ["/orig/.cache/seg048", "/orig/.cache/seg049", "/orig/.cache/seg072"])
        opened = ["/orig/.cache/seg072/data/d09/meta.json", "/orig/.cache/seg072-handoff/payload/x", "/orig/.cache/seg049/venv/lib/x.py"]
        self.assertEqual(dg.forbidden_opens(opened, roots, allow=["/orig/.cache/seg049/venv"]), ["/orig/.cache/seg072/data/d09/meta.json"])

    def test_parse_rebase(self):
        self.assertEqual(dg.parse_rebase(["/a=/b"]), {"/a": "/b"})
        with self.assertRaises(SystemExit):
            dg.parse_rebase(["/a"])


class Ranking(unittest.TestCase):
    def test_rank_order_ties_and_positions(self):
        s = [0.5, 0.9, 0.5, 0.1]
        self.assertEqual(dg.rank_order(s), [1, 0, 2, 3])
        self.assertEqual(dg.rank_positions(s), [1, 0, 2, 3])
        with self.assertRaises(ValueError):
            dg.rank_order([float("nan")])
        with self.assertRaises(ValueError):
            dg.rank_order([True])

    def test_budget_and_region_accounting(self):
        self.assertEqual(dg.budget_cells(10, 35), 4)
        self.assertEqual(dg.budget_cells(2048, 35), 717)
        with self.assertRaises(ValueError):
            dg.budget_cells(10, 0)
        s = [0.1 * i for i in range(10)]
        region = dg.fixed_region(s, {0, 99}, 20)
        self.assertEqual(region, {9, 8, 0})
        self.assertEqual(dg.region_bytes({0, 2}, 128, 300), 128 + 44)
        self.assertEqual(dg.missed_starts([0, 130, 257, 299], {0, 2}, 128), 1)

    def test_resolution_conversion_is_max_and_equals_word_max(self):
        self.assertEqual(dg.to_coarser_cells([1, 3, 2, 0, 5], 2), [3, 2, 5])
        words = [0.3, -1.0, 2.0, 0.7, 0.1, 0.2, 0.9, 0.0]
        fine = [max(words[i:i + 2]) for i in range(0, 8, 2)]
        coarse = [max(words[i:i + 4]) for i in range(0, 8, 4)]
        self.assertEqual(dg.to_coarser_cells(fine, 2), coarse)
        with self.assertRaises(ValueError):
            dg.to_coarser_cells([1], 0)

    def test_fusion_methods(self):
        a = [0.9, 0.8, 0.1, 0.2]
        b = [0.1, 0.9, 0.8, 0.2]
        mr = dg.fuse_mean_rank(a, b)
        self.assertEqual(dg.rank_order(mr)[0], 1)
        rrf = dg.fuse_rrf(a, b, k=60)
        self.assertAlmostEqual(rrf[1], 1 / 62 + 1 / 61)
        self.assertEqual(dg.rank_order(rrf)[0], 1)
        with self.assertRaises(ValueError):
            dg.fuse_rrf([1, 2], [1])
        with self.assertRaises(ValueError):
            dg.fuse_mean_rank([1, 2], [1])
        self.assertEqual(dg.normalized_ranks([5.0]), [0.0])

    def test_fusion_rescues_complementary_misses_at_equal_budget(self):
        # model A ranks positive 3 just outside the budget, model B ranks positive 0 just outside; each ranks the other positive first
        labels = [1, 0, 0, 1, 0, 0]
        a = [0.9, 0.8, 0.7, 0.6, 0.5, 0.4]
        b = [0.6, 0.5, 0.4, 0.9, 0.8, 0.7]
        fused = dg.fuse_mean_rank(a, b)
        for s, want in ((a, 1), (b, 1), (fused, 0)):
            region = dg.fixed_region(s, set(), 50)
            self.assertEqual(len(region), 3)
            self.assertEqual(sum(1 for c, y in enumerate(labels) if y == 1 and c not in region), want)


class Metrics(unittest.TestCase):
    def test_ranking_metrics_separate_ranking_from_budget(self):
        labels = [1, 0, 1, 0, 0, 1, 0, 0, 0, 0]
        perfect = [0.9, 0.1, 0.8, 0.1, 0.1, 0.7, 0.1, 0.1, 0.1, 0.1]
        m = dg.ranking_metrics(perfect, labels, set())
        self.assertAlmostEqual(m["min_required_fraction"], 0.3)
        self.assertAlmostEqual(m["required_fraction"], 0.3)
        self.assertAlmostEqual(m["ranking_excess"], 0.0)
        poor = [0.9, 0.8, 0.7, 0.6, 0.5, 0.0, 0.4, 0.3, 0.2, 0.1]
        m = dg.ranking_metrics(poor, labels, set())
        self.assertAlmostEqual(m["required_fraction"], 1.0)
        self.assertAlmostEqual(m["ranking_excess"], 0.7)
        self.assertAlmostEqual(m["last_positive_rank"], 0.9)
        seeded = dg.ranking_metrics(poor, labels, {5})
        self.assertAlmostEqual(seeded["required_fraction"], 0.4)
        self.assertEqual(seeded["seed_covered_positive"], 1)

    def test_degenerate_all_positive_image(self):
        m = dg.ranking_metrics([0.1, 0.2, 0.3], [1, 1, 1], {0, 1, 2})
        self.assertTrue(m["all_positive"])
        self.assertIsNone(m["last_positive_rank"])
        self.assertAlmostEqual(m["min_required_fraction"], 1.0)

    def test_budget_diagnosis_classes(self):
        labels = [1, 1, 0, 0, 0, 0, 0, 0, 0, 0]
        good = [0.9, 0.8] + [0.1] * 8
        bad = [0.9, 0.0] + [0.5] * 8
        self.assertEqual(dg.budget_diagnosis(good, labels, set(), 20)["class"], "complete")
        d = dg.budget_diagnosis(bad, labels, set(), 20)
        self.assertEqual((d["class"], d["capacity_sufficient"], d["extra_cells_for_full_recall"]), ("ranking_failure", True, 8))
        dense = [1] * 5 + [0] * 5
        self.assertEqual(dg.budget_diagnosis([0.5] * 10, dense, set(), 30)["class"], "capacity_failure")

    def test_labels_occupancy_boundary(self):
        cls = b"I" + b"c" * 3 + b"D" * 4 + b"D" * 8 + b"U" * 8
        self.assertEqual(dg.cell_labels(cls, 8), [1, 0, -1])
        self.assertAlmostEqual(dg.occupancy(cls, 0, 8), 0.5)
        self.assertEqual(dg.boundary_distance([1, 1, 1, 0], 0), 3)
        self.assertIsNone(dg.boundary_distance([1, 1], 0))


def tok(status=2, length=1, control=0):
    return bytes([status, length, 1, control, 0, 0, 0, 0])


class Expansion(unittest.TestCase):
    def test_gap_fill_single_holes_only(self):
        self.assertEqual(dg.gap_fill({0, 2, 4, 5, 8}, 10), {1, 3})

    def test_direct_flow_from_roots_stops_at_terminal_invalid_and_bound(self):
        # 16 words; cell = 8 bytes (4 words). Root at 0: 3 one-word instructions, a 2-word one crossing into cell 1, then a return.
        words = [tok(), tok(), tok(), tok(length=2), b"\x00" * 8, tok(control=6), tok(), tok(),
                 tok(), tok(status=0, length=0), tok(), tok(), tok(length=5), tok(), tok(), tok()]
        tokens = b"".join(words)
        self.assertEqual(dg.direct_flow_cells(tokens, 32, [0], cell=8), {0, 1})
        self.assertEqual(dg.direct_flow_cells(tokens, 32, [16], cell=8), {2})        # stops at an undecodable word
        self.assertEqual(dg.direct_flow_cells(tokens, 32, [20], cell=8), {2})        # a 5+-word length is unknown: stop
        self.assertEqual(dg.direct_flow_cells(tokens, 32, [1, -2, 99], cell=8), set())
        self.assertEqual(dg.direct_flow_cells(tokens, 32, [0], cell=8, limit=4), {0})

    def test_conditional_and_call_fall_through_jump_does_not(self):
        tokens = tok(control=1) + tok(control=3) + tok(control=2) + tok() + tok() + tok() + tok() + tok()
        self.assertEqual(dg.direct_flow_cells(tokens, 16, [0], cell=4), {0, 1})


class LabelEvidence(unittest.TestCase):
    def test_code_blocks(self):
        self.assertEqual(dg.code_blocks(b"DDIcIcUIccD"), [(2, 6, 2), (7, 10, 1)])

    def test_signature_flags_data_read_as_code(self):
        table = bytes([0x20, 0x61, 0x60, 0x81] * 64)
        code = bytes([0x4E, 0x75, 0x30, 0x3C, 0x22, 0x48, 0x61, 0x00] * 16)
        rom = table + table[:128] + table
        cls = b"D" * 256 + (b"Ic" * 64) + b"D" * 256
        sig = dg.high_byte_signature(rom, cls, cls, (256, 384))
        self.assertGreater(sig, 0.9)
        rom2 = table + code + table
        self.assertLess(dg.high_byte_signature(rom2, cls, cls, (256, 384)), 0.2)
        self.assertIsNone(dg.high_byte_signature(rom2, b"U" * 256 + b"Ic" * 64 + b"U" * 256, b"U" * 640, (256, 384)))

    def test_ida_xref_parser(self):
        text = "\n".join(["ROM:0007099C loc_7099C:  ; CODE XREF: ROM:00070A0C↓j",
                          "ROM:00001000 sub_1000:  ; CODE XREF: sub_200+1A↑p",
                          "ROM:00001002 dc.b 1 ; DATA XREF: ROM:00000010↑o", "garbage"])
        self.assertEqual(dg.parse_ida_code_xrefs(text), [(0x7099C, 0x70A0C), (0x1000, 0x21A)])

    def test_reference_origin_must_be_supported(self):
        blocks = [(0, 10, 3), (20, 30, 2), (40, 50, 4)]
        starts = [b[0] for b in blocks]
        support = [True, False, True]
        self.assertTrue(dg.evidenced_origin(blocks, starts, support, 4))
        self.assertFalse(dg.evidenced_origin(blocks, starts, support, 24))   # unsupported block (e.g. a branch-like word in a table)
        self.assertFalse(dg.evidenced_origin(blocks, starts, support, 15))   # outside any labelled block
        self.assertFalse(dg.evidenced_origin(blocks, starts, support, -1))

    def test_classification_rules(self):
        self.assertEqual(dg.classify_block(True, False, 0, 0.9), "CORROBORATED")
        self.assertEqual(dg.classify_block(False, True, 2, None), "CORROBORATED")
        self.assertEqual(dg.classify_block(False, True, 0, 0.1), "PLAUSIBLE")
        self.assertEqual(dg.classify_block(False, False, 1, None), "PLAUSIBLE")
        self.assertEqual(dg.classify_block(False, False, 0, 0.8), "DATA_EVIDENCE")
        self.assertEqual(dg.classify_block(False, True, 0, 0.8), "UNRESOLVED")
        self.assertEqual(dg.classify_block(False, False, 0, 0.2), "UNRESOLVED")
        self.assertEqual(dg.classify_block(False, False, 0, None), "UNRESOLVED")
        self.assertEqual(dg.cell_evidence_class(["CORROBORATED", "DATA_EVIDENCE"]), "DATA_EVIDENCE")
        self.assertEqual(dg.cell_evidence_class([]), "UNRESOLVED")


class Exposure(unittest.TestCase):
    def test_epoch_segments_drop_remainder_and_tiny_images(self):
        segs = dg.m3r_epoch_segments([100, 10, 64], [5, 0, 0], seg=32, per_batch=2)
        # image 0: starts 5, 37 (69 + 32 > 100); image 1: none (10 cells); image 2: 0, 32 -> 4 segments, all kept
        self.assertEqual(sorted(segs), [(0, 5), (0, 37), (2, 0), (2, 32)])
        segs = dg.m3r_epoch_segments([100, 64], [5, 0], seg=32, per_batch=3, order=[3, 2, 1, 0])
        self.assertEqual(segs, [(1, 32), (1, 0), (0, 37)])
        with self.assertRaises(ValueError):
            dg.m3r_epoch_segments([64], [0], seg=32, order=[0, 0])

    def test_balanced_schedule_conserves_visits_and_weights_rare(self):
        images = [("A", 64, set()), ("A", 32, set()), ("B", 40, {20})]
        exp = dg.balanced_schedule_exposure(images, total_segments=10, seg=32, weight=3.0)
        self.assertAlmostEqual(sum(sum(v) for v in exp), 10 * 32)
        self.assertAlmostEqual(sum(exp[0]) + sum(exp[1]), 5 * 32)
        self.assertAlmostEqual(sum(exp[2]), 5 * 32)
        self.assertGreaterEqual(exp[2][20], exp[2][0])
        tiny = dg.balanced_schedule_exposure([("A", 10, set())], total_segments=4)
        self.assertEqual(sum(tiny[0]), 0.0)

    def test_cell_classes_and_rare_fragments(self):
        cls = (b"I" + b"c" * 7) + (b"I" + b"c" + b"D" * 6) + (b"D" * 8) + (b"U" * 8)
        self.assertEqual(dg.cell_classes(cls, "D07", 8), ["positive_dense", "positive_sparse", "negative", "masked_sdk_runtime"])
        self.assertEqual(dg.cell_classes(cls, "D01", 8)[3], "masked_other")
        labels = dg.cell_labels(cls, 8)
        self.assertEqual(dg.rare_fragment_cells(labels, cls, 8), {1})


class CommittedRecord(unittest.TestCase):
    """The committed SEG-073 record is aggregate-only: no private keys, no address-like or per-cell lists, and it is internally consistent."""

    def test_record_is_aggregate_only(self):
        rec = json.loads(pathlib.Path(TOOLS, "segarecomp_ml_v3.diagnosis.json").read_text())
        self.assertEqual(rec["schema"], dg.SCHEMA)
        found = []

        def walk(x, path):
            if isinstance(x, dict):
                for k, v in x.items():
                    if str(k).startswith("_"):
                        found.append(path + "/" + str(k))
                    walk(v, path + "/" + str(k))
            elif isinstance(x, list):
                if len(x) > 20 or any(isinstance(i, int) and not isinstance(i, bool) and i > 4096 for i in x):
                    found.append(path)
                for i, v in enumerate(x):
                    walk(v, f"{path}[{i}]")
        walk(rec, "")
        self.assertEqual(found, [])
        self.assertTrue(rec["sealed_consistency"]["all_equal"])
        e05 = rec["label_credibility"]["E05"]
        self.assertTrue(all(c["class"] == "DATA_EVIDENCE" for c in e05["missed_cells"] if "M3_R+B50" in c["missed_by"]))
        self.assertEqual(rec["label_credibility"]["development_signature_control"]["blocks_at_or_above"], 0)


class Reporting(unittest.TestCase):
    def test_public_strips_private_keys_and_rounds(self):
        self.assertEqual(dg.public({"a": 0.1234567891, "_cell": 3, "b": [{"_range": (1, 2), "c": 1}]}), {"a": 0.123457, "b": [{"c": 1}]})


if __name__ == "__main__":
    unittest.main()
