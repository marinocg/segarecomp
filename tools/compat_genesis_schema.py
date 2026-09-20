#!/usr/bin/env python3
"""SEG-007-T206 / ADR-0034: structural schema guard for platforms/genesis/compat/<rom-sha256>.json.

Each file under ``platforms/genesis/compat/`` is a small, committed, per-ROM JSON array of
curated, individually-justified scalar table-descriptor assertions -- never a bulk
inventory, never a byte-array/disassembly-shaped record. This module implements the
one shared validator both ``tests/compat_genesis_schema_test.py`` (the enforcement
gate) and any future tooling consume, so the rule is defined exactly once.

See docs/decisions/0034-committed-genesis-compatibility-metadata-scalar-table-descriptors.md,
docs/decisions/0036-committed-scalar-table-descriptor-file-count-is-a-defensive-ceiling.md
(ADR-0036: corrects the record-count rationale below from a "small curated set" limit to a
defensive corruption/runaway-generation ceiling), and docs/testing/commercial-games.md's own
narrow carve-out for the full rationale.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

# ADR-0036: a defensive corruption/runaway-generation ceiling, NOT a "small
# curated set" design limit. A ROM-bound platforms/genesis/compat/<rom-sha256>.json file
# may legitimately contain the complete set of bounded scalar
# logical_table_descriptor assertions required for that ROM -- record count
# alone is not a semantic restriction. This ceiling exists only to catch a
# truly malformed or runaway-generated file long before it could plausibly
# represent a legitimate per-ROM table inventory. Widening it further still
# requires a new ADR, not a quiet PR.
MAX_RECORDS_PER_FILE = 1024

# Only kind recognized today. A future kind requires a new ADR (mirrors how
# ADR-0032/ADR-0033 each introduced their own distinct kind deliberately).
ALLOWED_KINDS = {"logical_table_descriptor"}

REQUIRED_RECORD_KEYS = {
    "rom_sha256",
    "kind",
    "base_address",
    "entry_width_bytes",
    "stride_bytes",
    "entry_count",
    "provenance",
}
REQUIRED_PROVENANCE_KEYS = {"tool", "tool_version", "timestamp", "human_reviewed"}

_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


def _uint32(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= 0xFFFFFFFF


def validate_compat_genesis_text(raw_text: str, *, expected_rom_sha256: str | None = None) -> list[str]:
    """Returns a list of human-readable errors; empty means the text is well-formed."""

    errors: list[str] = []
    try:
        payload = json.loads(raw_text)
    except json.JSONDecodeError as error:
        return [f"invalid JSON: {error}"]

    if not isinstance(payload, list):
        return ["top-level content must be a JSON array"]
    if len(payload) > MAX_RECORDS_PER_FILE:
        errors.append(
            f"file carries {len(payload)} records, exceeding the defensive "
            f"corruption/runaway-generation ceiling of {MAX_RECORDS_PER_FILE} "
            f"(ADR-0036; this ceiling guards against malformed/runaway generation, "
            f"not against a large legitimate per-ROM table inventory)"
        )

    for index, record in enumerate(payload):
        prefix = f"record[{index}]"
        if not isinstance(record, dict):
            errors.append(f"{prefix}: not a JSON object")
            continue
        extra_keys = set(record.keys()) - REQUIRED_RECORD_KEYS
        if extra_keys:
            errors.append(f"{prefix}: disallowed extra field(s): {sorted(extra_keys)}")
        missing_keys = REQUIRED_RECORD_KEYS - set(record.keys())
        if missing_keys:
            errors.append(f"{prefix}: missing required field(s): {sorted(missing_keys)}")
            continue

        kind = record.get("kind")
        if kind not in ALLOWED_KINDS:
            errors.append(f"{prefix}: disallowed kind {kind!r} (allowed: {sorted(ALLOWED_KINDS)})")

        rom_sha256 = record.get("rom_sha256")
        if not isinstance(rom_sha256, str) or not _SHA256_RE.match(rom_sha256):
            errors.append(f"{prefix}: rom_sha256 is not a well-formed lowercase SHA-256 hex digest")
        elif expected_rom_sha256 is not None and rom_sha256 != expected_rom_sha256:
            errors.append(
                f"{prefix}: rom_sha256 {rom_sha256!r} does not match the filename identity "
                f"{expected_rom_sha256!r}"
            )

        for field in ("base_address", "entry_width_bytes", "stride_bytes", "entry_count"):
            if not _uint32(record.get(field)):
                errors.append(f"{prefix}: {field} must be a non-negative 32-bit integer")

        if isinstance(record.get("entry_width_bytes"), int) and record.get("entry_width_bytes") == 0:
            errors.append(f"{prefix}: entry_width_bytes must be non-zero")
        if isinstance(record.get("stride_bytes"), int) and record.get("stride_bytes") == 0:
            errors.append(f"{prefix}: stride_bytes must be non-zero")
        if isinstance(record.get("entry_count"), int) and record.get("entry_count") == 0:
            errors.append(f"{prefix}: entry_count must be non-zero")

        provenance = record.get("provenance")
        if not isinstance(provenance, dict):
            errors.append(f"{prefix}: provenance must be a JSON object")
        else:
            extra_provenance_keys = set(provenance.keys()) - REQUIRED_PROVENANCE_KEYS
            if extra_provenance_keys:
                errors.append(f"{prefix}: provenance has disallowed extra field(s): {sorted(extra_provenance_keys)}")
            missing_provenance_keys = REQUIRED_PROVENANCE_KEYS - set(provenance.keys())
            if missing_provenance_keys:
                errors.append(f"{prefix}: provenance missing required field(s): {sorted(missing_provenance_keys)}")
            for field in ("tool", "tool_version", "timestamp"):
                if field in provenance and not isinstance(provenance.get(field), str):
                    errors.append(f"{prefix}: provenance.{field} must be a string")
            if "human_reviewed" in provenance and not isinstance(provenance.get("human_reviewed"), bool):
                errors.append(f"{prefix}: provenance.human_reviewed must be a boolean")

    return errors


def validate_compat_genesis_file(path: Path) -> list[str]:
    stem = path.stem
    expected_sha = stem if _SHA256_RE.match(stem) else None
    errors = validate_compat_genesis_text(path.read_text(encoding="utf-8"), expected_rom_sha256=expected_sha)
    if expected_sha is None:
        errors.append(f"filename {path.name!r} is not <64-lowercase-hex-sha256>.json")
    return errors
