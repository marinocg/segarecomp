#!/usr/bin/env python3
"""SEG-042-T002: regression tests for tools/segarecomp_recomp_map_harvest.py, the automatic real-title
external-fact harvester. Project-authored synthetic MC68000 bytes/report fixtures only; no commercial
input.

Pure-logic tests (backward-walk starting-scope rule, external-entry check, eligible-site filtering, fact-
file formatting) run hermetically against a hand-built synthetic report dict -- no angr, no native
binary, no ROM. The end-to-end test additionally needs angr (SKIPPED, not failed, when unavailable,
matching this suite's existing optional-dependency convention) and the native primary-word classifier
CLI (its path is the first CLI argument; SKIPPED when not given/not found, since it is only built as part
of the full product build, not by this test file itself).
"""
from __future__ import annotations

import pathlib
import struct
import sys
import tempfile

TOOLS_DIR = pathlib.Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS_DIR))

import segarecomp_recomp_map_harvest as harvest_module  # noqa: E402

failures = 0


def expect(condition: bool, message: str) -> None:
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {message}", file=sys.stderr)


def test_eligible_sites_excludes_rte_and_unclassified() -> None:
    report = {
        "computed_sites": {
            "000100": {"family": "rte", "outcome": "unknown"},
            "000200": {"family": "unclassified", "outcome": "unknown"},
            "000300": {"family": "rts_computed", "outcome": "unknown"},
            "000400": {"family": "jsr_an", "outcome": "unknown"},
            "000500": {"family": "jsr_an", "outcome": "resolved"},  # already resolved: not eligible either
        },
    }
    sites = harvest_module.eligible_sites(report)
    pcs = [s["pc"] for s in sites]
    expect(pcs == [0x300, 0x400], f"eligible_sites must exclude rte/unclassified/resolved, got {[hex(p) for p in pcs]}")


def test_backward_walk_and_external_entry_check() -> None:
    # entry(0x200, 4 bytes) -> site(0x204); a second, unrelated function g(0x300, 4 bytes) has a static
    # successor landing INSIDE (entry, site] at 0x206 -- an external entry the inner candidate must reject,
    # forcing the walk to widen to the next (and here, only remaining) call-target entry: the program root.
    discovered_lengths = {"000200": 4, "000204": 4, "000300": 4}
    call_target_entries = {0x200}
    roots = {0x200}
    static_successors = {"000300": ["000206"]}  # g jumps into the middle of entry's span
    predecessor_index = harvest_module.build_predecessor_index(discovered_lengths)
    expect(predecessor_index == {0x204: 0x200, 0x208: 0x204, 0x304: 0x300},
           f"build_predecessor_index: unexpected index {predecessor_index}")

    # Without the external successor, 0x200 is immediately sound for a site at 0x204.
    clean_start = harvest_module.find_sound_start_pc(0x204, predecessor_index, call_target_entries, roots, {})
    expect(clean_start == 0x200, f"find_sound_start_pc: expected 0x200 with no external entries, got {clean_start}")

    # With the external successor landing inside (0x200, 0x208], a site at 0x208 must reject 0x200 and find
    # no further candidate (0x200 is the only call-target entry/root reachable by the backward walk here).
    violated = harvest_module.external_entry_violates(0x200, 0x208, static_successors)
    expect(violated, "external_entry_violates: must detect the edge from 0x300 into (0x200, 0x208]")
    rejected = harvest_module.find_sound_start_pc(0x208, predecessor_index, call_target_entries, roots, static_successors)
    expect(rejected is None, f"find_sound_start_pc: must reject the only candidate and return None, got {rejected}")


def test_write_facts_format() -> None:
    results = [
        {"pc": 0x300, "outcome": "exact", "targets": [0x400, 0x500]},
        {"pc": 0x200, "outcome": "contained", "targets": [0x206, 0x20c]},
        {"pc": 0x900, "outcome": "unresolved", "reason": "no_sound_starting_scope"},
    ]
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / "facts.txt"
        written = harvest_module.write_facts(results, "a" * 64, "test-producer", str(out))
        expect(written == 2, f"write_facts: must write exactly the exact+contained results, wrote {written}")
        text = out.read_text(encoding="ascii")
    lines = text.splitlines()
    expect(lines[0] == "segarecomp.m68k_external_facts.v1", f"write_facts: wrong schema line {lines[0]!r}")
    expect(lines[1] == "rom_sha256 " + "a" * 64, f"write_facts: wrong rom_sha256 line {lines[1]!r}")
    expect(lines[2] == "producer test-producer", f"write_facts: wrong producer line {lines[2]!r}")
    # Strictly ascending by PC (the format's own requirement: 0x200 before 0x300) and ascending entries.
    expect(lines[3] == "fact 00000200 contained 00000206,0000020c", f"write_facts: unexpected fact line {lines[3]!r}")
    expect(lines[4] == "fact 00000300 exact 00000400,00000500", f"write_facts: unexpected fact line {lines[4]!r}")
    expect(lines[5] == "end", f"write_facts: missing trailing end line, got {lines[5]!r}")


def w(*words: int) -> bytes:
    out = b""
    for word in words:
        out += struct.pack(">H", word & 0xFFFF)
    return out


def make_rom(code: bytes, entry: int = 0x400, size: int = 0x4000) -> bytes:
    data = bytearray(size)
    struct.pack_into(">I", data, 0x0, 0x00FFFE00)
    struct.pack_into(">I", data, 0x4, entry)
    data[entry:entry + len(code)] = code
    return bytes(data)


def movea_l_imm(a: int, value: int) -> bytes:
    return w(0x207C | (a << 9)) + struct.pack(">I", value)


def test_end_to_end_exact_and_contained(classifier_path: str) -> None:
    """One `jmp (a0)` exact site (entry2) reachable from a single root, and one `rts_computed` site
    (callee) reachable from two static call sites in a separate function -- a hand-built report dict
    (mirroring exactly what the real C++ tool emits) drives the real angr exploration and the real native
    classifier binary end to end, with no ROM/report generation shortcuts."""
    entry = 0x400
    callee = 0x440
    entry2 = 0x480

    jsr_callee = w(0x4EB9) + struct.pack(">I", callee)  # jsr.l callee (6 bytes)
    code_at_entry = jsr_callee + jsr_callee + w(0x4E71)  # two call sites, then a NOP (halts discovery cleanly)
    call_site_1 = entry
    call_site_2 = entry + 6
    continuation_1 = call_site_1 + 6
    continuation_2 = call_site_2 + 6

    code_at_callee = w(0x4E75)  # rts
    code_at_entry2 = movea_l_imm(0, 0x4C0) + w(0x4ED0)  # movea.l #0x4C0,a0 ; jmp (a0)
    jmp_site = entry2 + len(movea_l_imm(0, 0x4C0))

    rom = bytearray(make_rom(code_at_entry, entry=entry))
    rom[callee:callee + len(code_at_callee)] = code_at_callee
    rom[entry2:entry2 + len(code_at_entry2)] = code_at_entry2
    rom[0x4C0:0x4C0 + 2] = w(0x60FE)  # bra.s * at the jmp's landing pad (a harmless, decodable halt)

    with tempfile.TemporaryDirectory() as tmp:
        rom_path = pathlib.Path(tmp) / "synth.bin"
        rom_path.write_bytes(bytes(rom))

        report = {
            "roots": [f"{entry:06x}", f"{entry2:06x}"],
            "discovered_lengths": {
                f"{call_site_1:06x}": 6, f"{call_site_2:06x}": 6, f"{continuation_2:06x}": 2,
                f"{callee:06x}": 2,
                f"{entry2:06x}": 6, f"{jmp_site:06x}": 2,
            },
            "call_target_continuations": {f"{callee:06x}": [f"{continuation_1:06x}", f"{continuation_2:06x}"]},
            "static_successors": {f"{call_site_1:06x}": [f"{callee:06x}"], f"{call_site_2:06x}": [f"{callee:06x}"]},
            "computed_sites": {
                f"{callee + len(code_at_callee) - 2:06x}": {"family": "rts_computed", "outcome": "unknown"},
                f"{jmp_site:06x}": {"family": "jmp_an", "outcome": "unknown"},
            },
        }

        results = harvest_module.harvest(report, str(rom_path), classifier_path, max_entries=64, max_steps=10_000)
        by_pc = {r["pc"]: r for r in results}

        rts_pc = callee + len(code_at_callee) - 2
        expect(by_pc[rts_pc]["outcome"] == "contained", f"E2E: rts_computed site must be contained, got {by_pc[rts_pc]}")
        expect(by_pc[rts_pc].get("targets") == sorted([continuation_1, continuation_2]),
               f"E2E: contained entries must be both call continuations, got {by_pc[rts_pc].get('targets')}")

        expect(by_pc[jmp_site]["outcome"] == "exact", f"E2E: jmp (a0) site must be exact, got {by_pc[jmp_site]}")
        expect(by_pc[jmp_site].get("targets") == [0x4C0], f"E2E: exact target must be 0x4C0, got {by_pc[jmp_site].get('targets')}")

        out = pathlib.Path(tmp) / "facts.txt"
        written = harvest_module.write_facts(results, "0" * 64, "test-producer", str(out))
        expect(written == 2, f"E2E: both facts must be written, wrote {written}")


def main() -> int:
    test_eligible_sites_excludes_rte_and_unclassified()
    test_backward_walk_and_external_entry_check()
    test_write_facts_format()

    classifier_path = sys.argv[1] if len(sys.argv) > 1 else None
    if not classifier_path or not pathlib.Path(classifier_path).exists():
        print("segarecomp-m68k-primary-word-classify binary not given/found: end-to-end test SKIPPED "
              "(pure-logic tests above still ran)")
    else:
        try:
            import angr  # noqa: F401
            from archinfo import ArchPcode
            ArchPcode("68000:BE:32:CPU32")
        except Exception as exc:  # noqa: BLE001
            print(f"angr (or its M68K p-code support) unavailable in this interpreter ({exc!r}): "
                  f"end-to-end test SKIPPED (pure-logic tests above still ran)")
        else:
            test_end_to_end_exact_and_contained(classifier_path)

    if failures:
        print(f"{failures} failure(s)", file=sys.stderr)
        return 1
    print("segarecomp_recomp_map_harvest_test: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
