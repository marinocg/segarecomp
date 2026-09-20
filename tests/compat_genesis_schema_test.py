#!/usr/bin/env python3
"""SEG-007-T206 / ADR-0034: structural schema guard for platforms/genesis/compat/<rom-sha256>.json.

Enforces the narrow, bounded scalar-assertion schema documented in
docs/decisions/0034-committed-genesis-compatibility-metadata-scalar-table-descriptors.md and
docs/testing/commercial-games.md. Fails the build if a future change ever widens
compat/genesis/'s committed content beyond this schema, or if a fixture attempts to smuggle a
byte-array/disassembly-shaped field, a code_entry_candidate/address_table_candidate record, or
an oversized bulk array into platforms/genesis/compat/.
"""

import json
import sys
import unittest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "tools"))

from compat_genesis_schema import (  # noqa: E402
    MAX_RECORDS_PER_FILE,
    validate_compat_genesis_file,
    validate_compat_genesis_text,
)

VALID_SHA = "a" * 64
VALID_RECORD = {
    "rom_sha256": VALID_SHA,
    "kind": "logical_table_descriptor",
    "base_address": 4096,
    "entry_width_bytes": 2,
    "stride_bytes": 2,
    "entry_count": 13,
    "provenance": {
        "tool": "segarecomp-static-discovery",
        "tool_version": "SEG-007-T206",
        "timestamp": "2026-09-11T00:00:00Z",
        "human_reviewed": False,
    },
}


class CompatGenesisSchemaFixtureTest(unittest.TestCase):
    def test_valid_single_record_passes(self) -> None:
        text = json.dumps([VALID_RECORD])
        self.assertEqual(validate_compat_genesis_text(text, expected_rom_sha256=VALID_SHA), [])

    def test_empty_array_passes(self) -> None:
        self.assertEqual(validate_compat_genesis_text("[]", expected_rom_sha256=VALID_SHA), [])

    def test_top_level_object_rejected(self) -> None:
        errors = validate_compat_genesis_text(json.dumps(VALID_RECORD))
        self.assertTrue(any("JSON array" in error for error in errors))

    def test_disallowed_extra_field_rejected(self) -> None:
        record = dict(VALID_RECORD)
        record["raw_bytes"] = [1, 2, 3]
        errors = validate_compat_genesis_text(json.dumps([record]))
        self.assertTrue(any("disallowed extra field" in error for error in errors))

    def test_disassembly_shaped_field_rejected(self) -> None:
        record = dict(VALID_RECORD)
        record["disassembly"] = "MOVE.W (d8,PC,D0.W),D1"
        errors = validate_compat_genesis_text(json.dumps([record]))
        self.assertTrue(any("disallowed extra field" in error for error in errors))

    def test_disallowed_kind_rejected(self) -> None:
        record = dict(VALID_RECORD)
        record["kind"] = "code_entry_candidate"
        errors = validate_compat_genesis_text(json.dumps([record]))
        self.assertTrue(any("disallowed kind" in error for error in errors))

    def test_address_table_candidate_kind_rejected(self) -> None:
        record = dict(VALID_RECORD)
        record["kind"] = "address_table_candidate"
        errors = validate_compat_genesis_text(json.dumps([record]))
        self.assertTrue(any("disallowed kind" in error for error in errors))

    def test_oversized_bulk_array_rejected(self) -> None:
        # ADR-0036: MAX_RECORDS_PER_FILE is a defensive corruption/runaway-generation
        # ceiling, not a "small curated set" limit -- but a file that exceeds even
        # that generous ceiling must still be rejected.
        records = []
        for index in range(MAX_RECORDS_PER_FILE + 1):
            record = dict(VALID_RECORD)
            record["base_address"] = 4096 + index * 4
            records.append(record)
        errors = validate_compat_genesis_text(json.dumps(records))
        self.assertTrue(any("runaway-generation ceiling" in error for error in errors))

    def test_large_legitimate_inventory_passes(self) -> None:
        # ADR-0036: a large (but within-ceiling) per-ROM inventory of
        # individually-distinct table identities is legitimate and must pass;
        # record count alone is not a semantic restriction.
        records = []
        for index in range(200):
            record = dict(VALID_RECORD)
            record["base_address"] = 4096 + index * 4
            records.append(record)
        self.assertLess(len(records), MAX_RECORDS_PER_FILE)
        errors = validate_compat_genesis_text(json.dumps(records), expected_rom_sha256=VALID_SHA)
        self.assertEqual(errors, [])

    def test_malformed_rom_sha256_rejected(self) -> None:
        record = dict(VALID_RECORD)
        record["rom_sha256"] = "not-a-sha"
        errors = validate_compat_genesis_text(json.dumps([record]))
        self.assertTrue(any("SHA-256" in error for error in errors))

    def test_mismatched_rom_sha256_rejected(self) -> None:
        errors = validate_compat_genesis_text(json.dumps([VALID_RECORD]), expected_rom_sha256="b" * 64)
        self.assertTrue(any("does not match the filename identity" in error for error in errors))

    def test_zero_entry_count_rejected(self) -> None:
        record = dict(VALID_RECORD)
        record["entry_count"] = 0
        errors = validate_compat_genesis_text(json.dumps([record]))
        self.assertTrue(any("entry_count must be non-zero" in error for error in errors))

    def test_two_distinct_descriptors_in_one_file_both_pass(self) -> None:
        # Mirrors the real committed platforms/genesis/compat/<rom-sha256>.json for the
        # authorized Sonic ROM after the SEG-007-T164 migration: two
        # logical_table_descriptor records at distinct base_address values in
        # one file, neither shadowing nor interfering with the other.
        second = dict(VALID_RECORD)
        second["base_address"] = 2926
        second["entry_count"] = 13
        second["provenance"] = {
            "tool": "ghidra",
            "tool_version": "SEG-007-T164-correction-cycle-2",
            "timestamp": "2026-09-04T00:00:00Z",
            "human_reviewed": False,
        }
        text = json.dumps([VALID_RECORD, second])
        self.assertEqual(validate_compat_genesis_text(text, expected_rom_sha256=VALID_SHA), [])

    def test_missing_provenance_field_rejected(self) -> None:
        record = dict(VALID_RECORD)
        record["provenance"] = {"tool": "x", "tool_version": "1", "timestamp": "t"}
        errors = validate_compat_genesis_text(json.dumps([record]))
        self.assertTrue(any("provenance missing required field" in error for error in errors))

    def test_extra_provenance_field_rejected(self) -> None:
        record = dict(VALID_RECORD)
        record["provenance"] = dict(VALID_RECORD["provenance"])
        record["provenance"]["raw_trace"] = "..."
        errors = validate_compat_genesis_text(json.dumps([record]))
        self.assertTrue(any("provenance has disallowed extra field" in error for error in errors))


class CompatGenesisRepoScanTest(unittest.TestCase):
    """Validates every file actually committed under platforms/genesis/compat/ today."""

    def test_every_committed_compat_genesis_file_is_well_formed(self) -> None:
        compat_dir = PROJECT_ROOT / "platforms" / "genesis" / "compat"
        if not compat_dir.is_dir():
            self.skipTest("platforms/genesis/compat/ does not exist yet")
        json_files = sorted(compat_dir.glob("*.json"))
        for path in json_files:
            with self.subTest(path=str(path.relative_to(PROJECT_ROOT))):
                errors = validate_compat_genesis_file(path)
                self.assertEqual(errors, [], f"{path}: {errors}")


class CompatGenesisSonicInventorySemanticTest(unittest.TestCase):
    """SEG-007-T218: semantic-composition assertions for the canonicalized whole-ROM
    Sonic REV00 logical_table_descriptor inventory. Deliberately avoids asserting only
    on array length: checks that the two previously-established identities kept their
    exact original provenance and entry_count after the merge, and that a sample of
    newly added identities validate."""

    SONIC_SHA = "46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6"

    def _load_sonic_records(self) -> list[dict] | None:
        path = PROJECT_ROOT / "platforms" / "genesis" / "compat" / f"{self.SONIC_SHA}.json"
        if not path.is_file():
            return None
        return json.loads(path.read_text(encoding="utf-8"))

    def test_previously_established_identities_keep_original_provenance(self) -> None:
        records = self._load_sonic_records()
        if records is None:
            self.skipTest("Sonic REV00 platforms/genesis/compat file does not exist yet")
        by_base = {record["base_address"]: record for record in records}

        seg_007_t164 = by_base.get(2926)
        self.assertIsNotNone(seg_007_t164, "SEG-007-T164 identity (base_address=2926) missing")
        self.assertEqual(seg_007_t164["entry_count"], 13)
        self.assertEqual(seg_007_t164["stride_bytes"], 2)
        self.assertEqual(seg_007_t164["provenance"]["tool"], "ghidra")
        self.assertEqual(seg_007_t164["provenance"]["tool_version"], "SEG-007-T164-correction-cycle-2")

        seg_007_t206 = by_base.get(94834)
        self.assertIsNotNone(seg_007_t206, "SEG-007-T206 identity (base_address=94834) missing")
        self.assertEqual(seg_007_t206["entry_count"], 2)
        self.assertEqual(seg_007_t206["stride_bytes"], 2)
        self.assertEqual(
            seg_007_t206["provenance"]["tool"],
            "seg-007-t206-bounded-development-time-semantic-analysis",
        )
        self.assertEqual(seg_007_t206["provenance"]["tool_version"], "1")

    def test_inventory_has_no_duplicate_identities_and_sample_validates(self) -> None:
        records = self._load_sonic_records()
        if records is None:
            self.skipTest("Sonic REV00 platforms/genesis/compat file does not exist yet")

        identities = [(record["base_address"], record["entry_width_bytes"]) for record in records]
        self.assertEqual(len(identities), len(set(identities)), "duplicate table identity present")

        # A large per-ROM inventory is expected and legitimate (ADR-0036); assert on
        # composition, not just a bare count threshold.
        # Exact canonical count, not just a lower bound: additional protection
        # against an accidental future insertion/removal beyond the specific,
        # individually-justified set this task established (198 originally
        # merged minus exactly one identity independently proven to cause a
        # live cross-root aggregation_conflict -- see the task's own Evidence
        # for the four other span-flagged identities that were re-tested
        # against the real production discovery/stitching mechanism and
        # restored after producing zero conflicts).
        self.assertEqual(len(records), 197)

        for record in records:
            self.assertEqual(record["rom_sha256"], self.SONIC_SHA)
            self.assertEqual(record["kind"], "logical_table_descriptor")
            self.assertEqual(record["entry_width_bytes"], 2)
            self.assertEqual(record["stride_bytes"], 2)
            self.assertTrue(1 <= record["entry_count"] <= 256)
            self.assertIsInstance(record["provenance"]["human_reviewed"], bool)


if __name__ == "__main__":
    unittest.main()
