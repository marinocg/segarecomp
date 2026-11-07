#!/usr/bin/env python3
"""Tests for tools/check_product_truth.py: the real tree is clean and each rule rejects its own malformed fixture.

Public files only: no network, no external repository, no commercial image.
"""
import copy
import importlib.util
import json
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("check_product_truth", ROOT / "tools" / "check_product_truth.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)

LEDGER = json.loads((ROOT / gate.LEDGER_PATH).read_text(encoding="utf-8"))


def mutated(edit):
    ledger = copy.deepcopy(LEDGER)
    edit(ledger)
    return gate.validate_ledger(ledger, ROOT)


def game(ledger, game_id):
    return next(g for g in ledger["games"] if g["id"] == game_id)


class RealTreeTest(unittest.TestCase):
    def test_repository_is_clean(self):
        self.assertEqual(gate.run(ROOT), [])

    def test_human_completion_is_not_an_independent_run(self):
        for entry in LEDGER["games"]:
            if entry["execution_level"] == "completed":
                categories = {e["category"] for e in entry["evidence"]}
                self.assertIn("maintainer_reported_playthrough", categories, entry["id"])
                self.assertNotIn("independently_reproduced_completion", categories, entry["id"])


class LedgerRulesTest(unittest.TestCase):
    def assertRejected(self, edit, fragment):
        problems = mutated(edit)
        self.assertTrue(any(fragment in p for p in problems), f"{fragment!r} not in {problems}")

    def test_unknown_top_level_key(self):
        self.assertRejected(lambda l: l.update(extra=1), "unknown key 'extra'")

    def test_unsupported_platform_status(self):
        self.assertRejected(lambda l: l["platforms"][0].update(status="shipping"), "unsupported status")

    def test_planned_platform_cannot_claim_capabilities(self):
        def edit(l):
            next(p for p in l["platforms"] if p["status"] == "planned")["capabilities"] = ["plays games"]
        self.assertRejected(edit, "planned platform must not list capabilities")

    def test_unsupported_execution_level(self):
        self.assertRejected(lambda l: game(l, "genesis-sonic-2").update(execution_level="perfect"), "unsupported execution_level")

    def test_unsupported_evidence_category(self):
        self.assertRejected(lambda l: game(l, "genesis-sonic-2")["evidence"][0].update(category="vibes"), "unsupported evidence category")

    def test_automated_evidence_cannot_prove_playable(self):
        self.assertRejected(lambda l: game(l, "genesis-sonic-2")["evidence"][0].update(category="automated_gameplay_route"), "is not supported by its evidence")

    def test_completed_needs_human_or_independent_evidence(self):
        def edit(l):
            g = game(l, "sega_cd-sonic-cd")
            g["evidence"] = [e for e in g["evidence"] if e["category"] == "reference_comparison"]
        self.assertRejected(edit, "execution_level 'completed' is not supported")

    def test_unknown_level_contradicts_evidence(self):
        self.assertRejected(lambda l: game(l, "genesis-sonic-2").update(execution_level="unknown"), "contradicts non-empty evidence")

    def test_untested_evidence_cannot_support_a_level(self):
        self.assertRejected(lambda l: game(l, "genesis-sonic-2")["evidence"][0].update(category="untested"), "execution_level")

    def test_human_completion_must_list_unknown_conditions(self):
        self.assertRejected(lambda l: game(l, "sega_cd-sonic-cd").pop("unknown_conditions"), "must list unknown_conditions")

    def test_game_on_planned_platform(self):
        self.assertRejected(lambda l: game(l, "genesis-sonic-2").update(platform="saturn"), "planned platform")

    def test_patched_input_is_not_a_game_entry(self):
        self.assertRejected(lambda l: game(l, "genesis-sonic-2").update(input="modified"), "must use original input")

    def test_broken_documentation_reference(self):
        self.assertRejected(lambda l: game(l, "genesis-sonic-2")["evidence"][0].update(refs=["docs/does-not-exist.md"]), "broken evidence reference")

    def test_missing_adr_reference(self):
        self.assertRejected(lambda l: game(l, "genesis-sonic-2")["evidence"][0].update(refs=["ADR 9999"]), "broken evidence reference")

    def test_traversal_reference_is_refused(self):
        self.assertRejected(lambda l: game(l, "genesis-sonic-2")["evidence"][0].update(refs=["../outside.md"]), "broken evidence reference")

    def test_duplicate_ids(self):
        self.assertRejected(lambda l: l["games"].append(copy.deepcopy(l["games"][0])), "games: duplicate id")

    def test_patch_package_cannot_claim_release_without_asset(self):
        self.assertRejected(lambda l: l["patch_packages"][0].update(distribution="release"), "requires an actual release asset")

    def test_patch_package_must_state_what_was_not_tested(self):
        self.assertRejected(lambda l: l["patch_packages"][0].update(untested=[]), "untested must not be empty")

    def test_feature_release_must_be_latest_release(self):
        self.assertRejected(lambda l: l["features"][0].update(released_in="v9.9.9"), "released_in must be null or")

    def test_feature_unknown_platform(self):
        self.assertRejected(lambda l: l["features"][0].update(platforms=["dreamcast"]), "known platform ids")

    def test_feature_status_values(self):
        self.assertRejected(lambda l: l["features"][0].update(status="done"), "unsupported status")


class DocumentRulesTest(unittest.TestCase):
    def test_legacy_assertions_are_rejected(self):
        samples = {
            "a.md": "There is no key remapping yet.",
            "b.md": "Generated-native Z80 execution is **not** implemented yet.",
            "c.md": "there are no FM or PSG sounds (the Z80 does not execute)",
            "d.md": "No sound is available for Sega CD.",
            "e.md": "a second title has only **experimental observations**",
            "f.md": "| [Master System](master-system/README.md) | no | none |",
            "g.md": "See /home/user/private/patches",
            "h.md": "A human run that was independently validated completion.",
        }
        problems = gate.check_forbidden(samples)
        for name in samples:
            self.assertTrue(any(p.startswith(name) for p in problems), f"{name} accepted: {problems}")

    def test_current_wording_is_accepted(self):
        self.assertEqual(gate.check_forbidden({"a.md": "Remap keys and gamepad buttons on the Controls page. Sega CD saves persist."}), [])

    def test_broken_local_link_is_found(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "good.md").write_text("ok", encoding="utf-8")
            (root / "README.md").write_text("[a](good.md) [b](missing.md) [c](https://example.invalid/x) [d](#anchor) ![e](img/none.png)\n```\n[f](ignored-in-fence.md)\n```\n", encoding="utf-8")
            problems = gate.check_links(root, ("README.md",))
        self.assertEqual(len(problems), 2, problems)
        self.assertTrue(any("missing.md" in p for p in problems) and any("none.png" in p for p in problems))

    def test_broken_anchor_and_traversal_link_are_found(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "README.md").write_text("# Real heading **(x)**\n[a](#real-heading-x) [b](#nope) [c](../../docs/x.md) [d](../../releases)\n", encoding="utf-8")
            problems = gate.check_links(root, ("README.md",))
        self.assertEqual(len(problems), 2, problems)
        self.assertTrue(any("#nope" in p for p in problems) and any("x.md" in p for p in problems))

    def test_ledger_and_status_pages_must_agree(self):
        texts = {
            "README.md": "Sonic CD ... (no ledger link, no release)",
            "platforms/README.md": "| [Genesis / Mega Drive](g) | yes | x |\n| [Master System](m) | no | x |\n| [Saturn](s) | yes | x |\n",
        }
        problems = gate.check_consistency(LEDGER, texts)
        for fragment in ("must link the compatibility ledger", "latest published release", "maintainer-reported", "master system is implemented", "saturn is planned"):
            self.assertTrue(any(fragment in p for p in problems), f"{fragment!r} not in {problems}")


if __name__ == "__main__":
    unittest.main()
