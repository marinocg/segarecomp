#!/usr/bin/env python3
"""SEG-007-T007: no-commercial-data-leakage test for the scanner's streaming/output path.

This test never touches a real ROM or a real Musashi adapter (the Musashi gate cannot be
satisfied synthetically, since its `git rev-parse HEAD` pin check requires the exact pinned
commit hash of a real upstream checkout). Instead it drives the tool's real ``run_scan`` streaming
driver against a small, project-authored, fully synthetic *fake adapter* executable that
implements exactly the documented per-instruction streaming wire protocol (one JSON line per
fully-executed instruction on stdout, blocking on a "continue"/"stop" reply read from stdin), using
only synthetic MC68000 opcode words and synthetic addresses -- never any content derived from a
commercial ROM. This exercises the exact code path the real --scan flow uses after both gates
pass (``run_scan`` -> ``write_cache`` -> ``summarize``), and inspects the tool's own emitted
stdout, the produced .cache file, and the fake adapter's stderr for the literal forbidden content
classes: raw ROM path/filename text, opcode/extension hex words appearing unnormalized, and
disassembly-shaped text.
"""
import contextlib
import io
import json
import pathlib
import stat
import sys
import tempfile

PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT))

from tools import sonic_startup_inventory as tool  # noqa: E402

# A deliberately unrelated, obviously-fake local path: if this string ever appeared in tool
# output it would prove a path leak, but it is never fed to run_scan below except as the
# forbidden needle checked for absence.
FORBIDDEN_FAKE_ROM_PATH = "/totally/fake/local/sonic-rom-path.bin"

# A small, fully synthetic, project-authored fake adapter implementing exactly the documented
# per-instruction streaming wire protocol from docs/testing/sonic-startup-inventory.md: one JSON
# line per fully-executed instruction, then block reading one "continue"/"stop" line from stdin.
# It ignores its <rom-path> argument entirely (this test never touches a real ROM). The final
# instruction's access is deliberately classified hardware_frontier by the real shared mapping
# probe, so the driver naturally replies "stop" and this fake adapter exits cleanly on its own.
_FAKE_ADAPTER_SOURCE = """#!/usr/bin/env python3
import json
import sys

INSTRUCTIONS = [
    {"pc": "0x00000206", "primary": "0x7000", "extension": None, "length": 2, "accesses": []},
    {"pc": "0x00000208", "primary": "0x7000", "extension": None, "length": 2, "accesses": []},
    {"pc": "0x0000020A", "primary": "0x41F9", "extension": "0x00FF0010", "length": 6, "accesses": [
        {"kind": "read", "address": "0x00000100", "byteWidth": 2},
        {"kind": "write", "address": "0x00FF0020", "byteWidth": 4},
    ]},
    {"pc": "0x0000020E", "primary": "0x4E71", "extension": None, "length": 2, "accesses": [
        {"kind": "write", "address": "0x00C00004", "byteWidth": 2},
    ]},
    # Never reached: the previous instruction's access is hardware_frontier, so the driver must
    # reply "stop" before this one is allowed to run.
    {"pc": "0x00000210", "primary": "0x7000", "extension": None, "length": 2, "accesses": []},
]


def main() -> int:
    ordinal = 0
    for instruction in INSTRUCTIONS:
        accesses = []
        for access in instruction["accesses"]:
            accesses.append({
                "ordinal": ordinal,
                "kind": access["kind"],
                "address": access["address"],
                "byteWidth": access["byteWidth"],
            })
            ordinal += 1
        line = {
            "ordinal": ordinal,
            "pc": instruction["pc"],
            "primary": instruction["primary"],
            "extension": instruction["extension"],
            "length": instruction["length"],
            "accesses": accesses,
        }
        ordinal += 1
        sys.stdout.write(json.dumps(line) + "\\n")
        sys.stdout.flush()
        reply = sys.stdin.readline().strip()
        if reply != "continue":
            return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
"""


def main() -> None:
    executable = sys.argv[1]
    image_length = 0x00010000

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        adapter_path = tmp_path / "fake-adapter.py"
        adapter_path.write_text(_FAKE_ADAPTER_SOURCE, encoding="utf-8")
        adapter_path.chmod(adapter_path.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
        rom_path = tmp_path / "unused-fake-rom.bin"
        # Keep the direct streaming invocation and both full main() invocations on identical
        # mapping inputs; the adapter ignores these project-authored bytes.
        rom_path.write_bytes(b"\x00" * image_length)

        normalized, adapter_stderr = tool.run_scan(adapter_path, rom_path, executable, image_length)

        # The fake adapter must never print anything to stderr for a clean run; this is the same
        # property a real adapter run must uphold.
        assert adapter_stderr == "", adapter_stderr

        summary = tool.summarize(normalized)
        normalized_text = json.dumps(normalized)

        forbidden_substrings = [
            FORBIDDEN_FAKE_ROM_PATH,
            str(rom_path),
            ".bin",
            "0x7000",
            "0x41F9",
            "0x4E71",
            "00FF0010",
            "disassembly",
        ]
        for needle in forbidden_substrings:
            assert needle not in normalized_text, (needle, normalized_text)
            assert needle not in summary, (needle, summary)
            assert needle not in adapter_stderr, (needle, adapter_stderr)

        # The normalized instruction/access records carry only sanitized fields.
        allowed_instruction_keys = {
            "family", "size", "addressingMode", "sourceAddressingMode", "destinationAddressingMode",
            "support", "firstObservedOrdinal", "observationCount",
        }
        for entry in normalized["instructions"]:
            assert set(entry.keys()) == allowed_instruction_keys, entry
            assert entry["family"], entry  # never empty/None per acceptance
        allowed_access_keys = {"category", "kind", "firstObservedOrdinal", "observationCount"}
        for entry in normalized["accesses"]:
            assert set(entry.keys()) == allowed_access_keys, entry

        # MOVEQ #0,D0 observed twice must aggregate to one entry with observationCount 2 (never
        # keyed on raw opcode bytes -- keyed on the normalized 6-tuple
        # (family, size, addressingMode, sourceAddressingMode, destinationAddressingMode, support)).
        moveq_entries = [e for e in normalized["instructions"] if e["family"] == "MOVEQ"]
        assert len(moveq_entries) == 1, normalized["instructions"]
        assert moveq_entries[0]["observationCount"] == 2, moveq_entries[0]
        assert moveq_entries[0]["support"] == "supported", moveq_entries[0]

        # LEA (xxx).L is stage-1 named precisely but stage-2's real decode/lift/effect/lowering
        # pipeline finds it unsupported (independent stages/authorities).
        lea_entries = [e for e in normalized["instructions"] if e["family"] == "LEA"]
        assert len(lea_entries) == 1, normalized["instructions"]
        assert lea_entries[0]["support"] == "unsupported", lea_entries[0]
        assert lea_entries[0]["size"] == "long", lea_entries[0]
        assert lea_entries[0]["addressingMode"] == "absolute_long", lea_entries[0]
        assert lea_entries[0]["sourceAddressingMode"] is None, lea_entries[0]
        assert lea_entries[0]["destinationAddressingMode"] is None, lea_entries[0]

        # An access within [0, image_length) is raw_cartridge_rom; within the RAM window is
        # synthetic_work_ram; outside both is hardware_frontier -- via the real shared,
        # width-aware mapping probe.
        categories = {(e["category"], e["kind"]) for e in normalized["accesses"]}
        assert ("raw_cartridge_rom", "read") in categories, categories
        assert ("synthetic_work_ram", "write") in categories, categories
        assert ("hardware_frontier", "write") in categories, categories

        # The driver -- never the fake adapter -- decided to stop at the hardware_frontier access;
        # the never-reached fifth synthetic instruction proves the adapter obeyed that reply.
        assert normalized["stopReason"] == "hardware_frontier", normalized["stopReason"]
        total_moveq_and_lea_and_nop_observations = sum(
            entry["observationCount"] for entry in normalized["instructions"]
        )
        assert total_moveq_and_lea_and_nop_observations == 4, normalized["instructions"]

        # Writing to a temporary cache path must not leak anything either.
        cache_path = tmp_path / "sonic-startup-inventory.json"
        tool.write_cache(normalized, cache_path)
        cache_text = cache_path.read_text(encoding="utf-8")
        for needle in forbidden_substrings:
            assert needle not in cache_text, (needle, cache_text)
        # Round-tripping twice from the same synthetic input is byte-identical.
        tool.write_cache(normalized, cache_path)
        cache_text_second = cache_path.read_text(encoding="utf-8")
        assert cache_text == cache_text_second

        # Run the complete post-gate path twice, each time spawning a fresh fake-adapter process
        # and writing a different cache file. This verifies deterministic aggregation, cache
        # serialization, and sanitized stdout rather than merely rewriting one Python object.
        cache_paths = [tmp_path / "fresh-scan-one.json", tmp_path / "fresh-scan-two.json"]
        summaries: list[str] = []
        old_rom_gate, old_musashi_gate = tool.check_rom_gate, tool.check_musashi_gate
        try:
            tool.check_rom_gate = lambda: (rom_path, None)
            tool.check_musashi_gate = lambda: (adapter_path, None)
            for fresh_cache_path in cache_paths:
                captured_stdout = io.StringIO()
                with contextlib.redirect_stdout(captured_stdout):
                    exit_code = tool.main(
                        ["--executable", executable, "--scan"], cache_path=fresh_cache_path,
                    )
                assert exit_code == tool.EXIT_SUCCESS, exit_code
                summaries.append(captured_stdout.getvalue())
        finally:
            tool.check_rom_gate, tool.check_musashi_gate = old_rom_gate, old_musashi_gate

        fresh_cache_texts = [path.read_text(encoding="utf-8") for path in cache_paths]
        assert fresh_cache_texts[0] == fresh_cache_texts[1]
        assert summaries[0] == summaries[1]
        assert fresh_cache_texts[0] == cache_text
        assert summaries[0] == summary + "\n"

    print("sonic startup inventory privacy test: ok")


if __name__ == "__main__":
    main()
