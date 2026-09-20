#!/usr/bin/env python3
"""C4 report pairs are finite, stop-owned, and independently validated."""
import base64
import importlib.util
import pathlib
import re
import sys


def load_bridge(root: pathlib.Path):
    spec = importlib.util.spec_from_file_location("genesis_startup_bridge", root / "tools" / "genesis_startup_bridge.py")
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    if len(sys.argv) != 2:
        return 2
    bridge = load_bridge(pathlib.Path(sys.argv[1]).resolve())
    root = pathlib.Path(sys.argv[1]).resolve()
    digest = "0" * 64

    # The bridge is a wire-schema validator, so its finite whitelist must
    # match the generated runtime's only C4 family serializer exactly.
    runtime = (root / "platforms" / "genesis" / "runtime" / "runtime.c").read_text(encoding="utf-8")
    serializer = runtime.split("static int genesis_c4_lowering_dimensions_fields", 1)[1].split(
        "static const char *genesis_access_width_name", 1)[0]
    emitted_families = tuple(re.findall(r'\*family = "([^"]+)"; return 1;', serializer))
    assert bridge.C4_LOWERING_DIMENSIONS == tuple({"family": family} for family in emitted_families)

    def full(dimensions: object, category: str = "c4_lowering_gap") -> dict:
        return {
            "schema_version": 1, "report_kind": "full", "rom_sha256": digest, "result": "stop",
            "runtime": {"d": ["0x00000000"] * 8, "a": ["0x00000000"] * 8,
                        "usp": "0x00000000", "sr": "0x0000", "pc": "0x00000000",
                        "work_ram_base64": base64.b64encode(bytes(65536)).decode("ascii")},
            "stop_class": "c4_lowering_gap", "diagnostic_category": category,
            "c4_lowering_dimensions": dimensions,
            "provenance": {"has_instruction_provenance": False, "instruction": None,
                           "has_access": False, "access_address": "0x00000000", "access_width": None,
                           "access_direction": None, "mapping_claim_count": 0, "mapping_claims": [],
                           "bus_access_count": 0, "bus_accesses": []},
        }

    def sanitized(dimensions: object, category: str = "c4_lowering_gap") -> dict:
        return {
            "schema_version": 1, "report_kind": "sanitized", "rom_sha256": digest,
            "result": "stop", "stop_class": "c4_lowering_gap",
            "diagnostic_category": category, "cpu_dimensions": None,
            "c4_lowering_dimensions": dimensions,
        }

    # These finite generated-runtime C4 dimensions were absent only from the
    # bridge's matching report whitelist.  Each remains valid only as the
    # exact C4 stop/diagnostic/dimension trio in both reports.
    repaired_families = (
        "cmp_auto_update", "cmpi_auto_update", "cmp_missing_fact", "cmpi_missing_fact",
        "subtract_auto_update", "subtract_immediate_auto_update",
        "subtract_missing_fact", "subtract_immediate_missing_fact",
    )
    for family in repaired_families:
        repaired = {"family": family}
        public = sanitized(repaired)
        private = full(repaired)
        assert bridge.valid_sanitized(public, digest)
        assert bridge.valid_full(private, public, digest)

    # Unknown and malformed C4 dimensions must remain fail-closed.
    assert not bridge.valid_sanitized(sanitized({}), digest)
    assert not bridge.valid_sanitized(sanitized({"family": "cmp_auto_update", "extra": "value"}), digest)
    assert not bridge.valid_sanitized(sanitized({"family": "add_immediate_missing_fact"}), digest)
    # A valid full report cannot borrow a different sanitized dimension.
    assert not bridge.valid_full(private, sanitized({"family": "cmpi_auto_update"}), digest)

    # valid_full must validate the peer itself, not merely its fields that are
    # shared with the full report.  This peer has matching public fields but
    # violates the sanitized C4 schema.
    malformed_peer = sanitized(repaired)
    malformed_peer["cpu_dimensions"] = {}
    assert not bridge.valid_full(private, malformed_peer, digest)

    # An invalid stop/diagnostic pair cannot be accepted even when both peers
    # agree on an otherwise valid C4 dimension.
    malformed_sanitized = sanitized(repaired, "internal_dispatch_inconsistency")
    assert not bridge.valid_full(full(repaired, "internal_dispatch_inconsistency"), malformed_sanitized, digest)
    print("genesis startup bridge full validation: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
