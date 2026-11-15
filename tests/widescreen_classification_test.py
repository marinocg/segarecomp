#!/usr/bin/env python3
"""SEG-140-T006 / SEG-140-T007: the committed, aggregate-only 32:9 classification (docs/product/widescreen-32x9-classification.json) is complete, internally consistent and
non-reconstructable, and the public docs mention every studied scene class.

It is a mechanical completeness gate, not a measurement: it fails when a studied scene class lacks a T007 row for any of the four aspects, when a class claims more than its recorded
evidence supports (a presentation-only or fully-supported claim without submitted actors, resident art, an in-scope scenery source and an observed run), when a NOT RUN entry has no
blocker, when an aggregate field looks like ROM-derived data (addresses, byte runs), or when the docs omit a class. The numbers themselves come from the private bounded runs described in
docs/architecture/widescreen-aspect-matrix.md section 5."""
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "docs/product/widescreen-32x9-classification.json"
DOCS = (ROOT / "docs/architecture/widescreen-aspect-matrix.md", ROOT / "docs/product/widescreen-and-patches.md")
ASPECTS = ["4:3", "16:9", "21:9", "32:9"]
EXTENSION_ASPECTS = ["16:9", "21:9", "32:9"]
SCENERY = {"native", "authored-verified-scope", "authored-unverified", "generic-wrapped", "generic-declined", "unsupported"}
HUD = {"native", "relocated", "none"}
ACTORS = {"native", "submitted-full", "submitted-partial", "present-not-submitted", "absent-or-not-loaded", "unverified"}
ART = {"native", "resident-for-submitted", "unverified"}
BUDGET = {"within", "constrained", "unverified"}
KIND = {"native-only", "presentation-only", "gameplay-changing-required", "unsupported", "inconclusive"}
BACKGROUND = {"native", "generic-available", "authored-unverified", "authored-verified-scope", "unsupported"}
EVIDENCE = {"observed-run", "source-analysis", "observed-run+source-analysis"}
REASONS = {"unverified_state", "unsupported_mode", "over_declared_geometry", "undeclared", "virtual_sprite_rejected", "virtual_sprite_unpresented", "invalid_surface",
           "provider_declined_scene", "no_virtual_sprite_consumer"}
NOT_RUN_MIN_BLOCKER = 20


def walk(value):
    if isinstance(value, dict):
        for k, v in value.items():
            yield k
            yield from walk(v)
    elif isinstance(value, list):
        for v in value:
            yield from walk(v)
    elif isinstance(value, str):
        yield value


class ClassificationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc = json.loads(DATA.read_text(encoding="utf-8"))
        cls.text = "\n".join(p.read_text(encoding="utf-8") for p in DOCS)

    def test_header(self):
        d = self.doc
        self.assertEqual(d["schema"], "segarecomp.widescreen-classification")
        self.assertEqual(d["schema_version"], 1)
        self.assertEqual(d["aspects"], ASPECTS)
        lim = d["limitations"]
        for key in ("virtual_sprite_compositor", "live_viewer_presents_virtual_sprites", "mod_can_author_virtual_sprites", "side_column_sprite_drawing"):
            self.assertIn(key, lim)
        # F28 (SEG-140-T008, ADR 0306): the live viewer presents virtual sprites in the native picture and a mod can author them (proved by tests/mod_vsprite_viewer_test.py); the side columns draw them in the presentation space (ADR 0307)
        self.assertEqual(lim["live_viewer_presents_virtual_sprites"], True)
        self.assertEqual(lim["mod_can_author_virtual_sprites"], True)
        self.assertEqual(lim["side_column_sprite_drawing"], True)
        self.assertTrue(lim["actor_coverage_claims_available"] == [] and isinstance(lim["actor_coverage_claims_available"], list))

    def test_every_class_has_a_row_for_every_aspect(self):
        studied = 0
        for t in self.doc["titles"]:
            for c in t["scene_classes"]:
                studied += 1
                self.assertEqual(sorted(c["rows"]), sorted(ASPECTS), c["id"])
                self.assertIn(c["image"], [i["id"] for i in t["images"] if i["status"] == "studied"], c["id"])
        self.assertGreater(studied, 0)

    def test_rows_use_the_closed_vocabularies_and_are_consistent(self):
        for t in self.doc["titles"]:
            for c in t["scene_classes"]:
                cls = c["classification"]
                self.assertIn(cls["scenery"], SCENERY | {"native"})
                self.assertIn(cls["actors"], ACTORS)
                self.assertIn(cls["art"], ART)
                self.assertIn(cls["budget"], BUDGET)
                self.assertIn(cls["feasibility_32x9"], KIND)
                self.assertTrue(len(cls["rationale"]) >= 40, c["id"])
                self.assertIn(c["evidence"], EVIDENCE)
                self.assertTrue(c["repeatable"] in (True, False))
                self.assertTrue(c["route"] and len(c["route"]) >= 20)
                for asp, r in c["rows"].items():
                    where = f"{c['id']}@{asp}"
                    self.assertIn(r["scenery"], SCENERY, where)
                    self.assertIn(r["hud"], HUD, where)
                    self.assertIn(r["actors"], ACTORS, where)
                    self.assertIn(r["art"], ART, where)
                    self.assertIn(r["budget"], BUDGET, where)
                    self.assertIn(r["background_extension"], BACKGROUND, where)
                    self.assertIn(r["kind"], KIND, where)
                    self.assertIn(r["evidence"], EVIDENCE, where)
                    self.assertTrue(set(r["fallback_reasons"]) <= REASONS, where)
                    if asp == "4:3":
                        self.assertEqual((r["kind"], r["scenery"], r["actors"]), ("native-only", "native", "native"), where)
                        self.assertEqual(r["fallback_reasons"], [], where)
                    else:
                        self.assertNotEqual(r["kind"], "native-only", where)
                # the 32:9 row and the class summary agree
                r32 = c["rows"]["32:9"]
                self.assertEqual(r32["kind"], cls["feasibility_32x9"], c["id"])
                self.assertEqual(r32["actors"], cls["actors"], c["id"])

    def test_no_claim_beyond_the_evidence(self):
        for t in self.doc["titles"]:
            for c in t["scene_classes"]:
                for asp, r in c["rows"].items():
                    where = f"{c['id']}@{asp}"
                    if r["kind"] == "presentation-only":
                        # presentation-only needs every component established, never a looks-right claim
                        self.assertEqual(r["actors"], "submitted-full", where)
                        self.assertEqual(r["art"], "resident-for-submitted", where)
                        self.assertEqual(r["scenery"], "authored-verified-scope", where)
                        self.assertIn("observed-run", r["evidence"], where)
                    if r["actors"] in ("present-not-submitted", "absent-or-not-loaded", "submitted-partial") and asp != "4:3":
                        self.assertIn(r["kind"], ("gameplay-changing-required", "unsupported", "inconclusive"), where)
                    if r["kind"] == "gameplay-changing-required":
                        self.assertTrue(c["classification"]["rationale"].lower().count("because") >= 1 or "needed" in c["classification"]["rationale"].lower(), where)
                    # an authored scenery verdict is only 'verified' inside the package's own validated scope (16:9)
                    if asp in ("21:9", "32:9"):
                        self.assertNotEqual(r["scenery"], "authored-verified-scope", f"{where}: 16:9 evidence is never extrapolated")
                        self.assertNotEqual(r["background_extension"], "authored-verified-scope", where)
                    # a virtual sprite never implies an actor
                    self.assertNotIn("virtual", r["actors"], where)

    def test_not_run_entries_are_explicit(self):
        for t in self.doc["titles"]:
            for n in t["not_run"]:
                self.assertEqual(n["result"], "NOT RUN", n["id"])
                self.assertGreaterEqual(len(n["blocker"]), NOT_RUN_MIN_BLOCKER, n["id"])
            for i in t["images"]:
                if i["status"] != "studied":
                    self.assertEqual(i["status"], "NOT RUN", i["id"])
                    self.assertGreaterEqual(len(i["blocker"]), NOT_RUN_MIN_BLOCKER, i["id"])
        titles = {t["id"] for t in self.doc["titles"]}
        self.assertEqual(titles, {"sonic-1", "sonic-2", "sonic-3-and-knuckles"})

    def test_aggregate_only(self):
        pattern = re.compile(r"0x[0-9a-fA-F]{3,}|\b[0-9a-fA-F]{32,}\b|\$[0-9A-Fa-f]{4,}")
        for s in walk(self.doc):
            self.assertIsNone(pattern.search(s), f"address- or digest-like text in an aggregate record: {s[:60]}")
            self.assertLess(len(s), 700, "long free text does not belong in an aggregate record")
        for t in self.doc["titles"]:
            for c in t["scene_classes"]:
                for k, v in c["observations"].items():
                    self.assertTrue(isinstance(v, (int, float, str, bool, list, dict)), k)

    def test_docs_mention_every_class_and_stay_consistent(self):
        for t in self.doc["titles"]:
            for c in t["scene_classes"]:
                self.assertTrue(c["id"] in self.text, f"docs omit the studied class {c['id']}")
            for n in t["not_run"]:
                self.assertTrue(n["id"] in self.text, f"docs omit the NOT RUN entry {n['id']}")
        self.assertIn("widescreen-32x9-classification.json", self.text)
        # the docs never claim authored 32:9 gameplay
        self.assertNotRegex(self.text, r"(?i)fully playable (authored )?32:9")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + sys.argv[1:])
