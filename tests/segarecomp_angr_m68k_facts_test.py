#!/usr/bin/env python3
"""SEG-041-T008/correction: adversarial regression tests for tools/segarecomp_angr_m68k_facts.py's
exhaustiveness algorithm itself (the Python producer), distinct from the C++ consumer's own regression
tests (tests/analysis_hybrid_plan_test.cpp), which only exercise the already-written fact-file format.

Project-authored synthetic MC68000 bytes only; no commercial input. SKIPPED (not failed) when angr is
not importable in the current interpreter, matching the existing optional-dependency convention in this
test suite (see tests/m68k_conformance_harness_test.py's pinned-Musashi-unavailable handling).
"""
from __future__ import annotations

import struct
import sys
import tempfile
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS_DIR))

try:
    import angr  # noqa: F401
    from archinfo import ArchPcode
    ArchPcode("68000:BE:32:CPU32")  # the specific capability these tests need; not every angr install carries it
except Exception as exc:  # noqa: BLE001 -- any failure here means "not usable in this interpreter", never a test failure
    print(f"angr (or its M68K p-code support) unavailable in this interpreter ({exc!r}): "
          f"SEG-041-T008 producer exhaustiveness regression tests SKIPPED")
    raise SystemExit(0)

import segarecomp_angr_m68k_facts as producer  # noqa: E402

failures = 0


def expect(condition: bool, message: str) -> None:
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {message}", file=sys.stderr)


def make_rom(code: bytes, entry: int = 0x400, size: int = 0x4000) -> bytes:
    data = bytearray(size)
    struct.pack_into(">I", data, 0x0, 0x00FFFE00)
    struct.pack_into(">I", data, 0x4, entry)
    data[entry:entry + len(code)] = code
    return bytes(data)


def w(*words: int) -> bytes:
    out = b""
    for word in words:
        out += struct.pack(">H", word & 0xFFFF)
    return out


def movea_l_imm(a: int, value: int) -> bytes:
    return w(0x207C | (a << 9)) + struct.pack(">I", value)


def tst_b_abs_l(addr: int) -> bytes:
    return w(0x4A39) + struct.pack(">I", addr)


def movea_l_abs_l(a: int, addr: int) -> bytes:
    return w(0x2079 | (a << 9)) + struct.pack(">I", addr)


NOP = w(0x4E71)
JMP_A0 = w(0x4ED0)
JMP_A1 = w(0x4ED1)
BRA_SELF = w(0x60FE)
SYM_CELL = 0x00009000  # never written anywhere: fully symbolic/unconstrained to angr with no premise
UNCONSTRAINED_CELL = 0x0000A000


def two_path_rom(nop_count: int, target_a: int, target_b: int):
    """entry -> tst.b(SYM_CELL) -> beq short-path (movea #A; bra L_TARGET) : bra long-path (N NOPs;
    movea #B; fallthrough L_TARGET) -> L_TARGET: jmp (A0). Returns (rom_bytes, entry, target_pc)."""
    entry = 0x400
    code = tst_b_abs_l(SYM_CELL)             # 6 bytes
    beq_pos = len(code)
    code += w(0x6700)                         # beq.b (fixup)         2 bytes
    bra_pos = len(code)
    code += w(0x6000)                         # bra.b (fixup)         2 bytes  -> L_LONG
    l_short = len(code)
    code += movea_l_imm(0, target_a)          # 6 bytes
    bra2_pos = len(code)
    code += w(0x6000, 0x0000)                 # bra.w (fixup) -> L_TARGET: must span the whole NOP block  4 bytes
    l_long = len(code)
    code += NOP * nop_count
    code += movea_l_imm(0, target_b)          # 6 bytes
    l_target = len(code)
    code += JMP_A0

    code = bytearray(code)

    # beq/bra (to L_LONG) are short-range (.b, 8-bit); the SHORT path's jump over the whole NOP block to
    # L_TARGET is long-range and uses .w (16-bit) explicitly -- an 8-bit displacement here would silently
    # wrap for any nop_count large enough to matter for this test (exactly what motivated this fix).
    code[beq_pos + 1] = (entry + l_short - (entry + beq_pos + 2)) & 0xFF
    code[bra_pos + 1] = (entry + l_long - (entry + bra_pos + 2)) & 0xFF
    struct.pack_into(">h", code, bra2_pos + 2, entry + l_target - (entry + bra2_pos + 2))

    rom = make_rom(bytes(code), entry=entry)
    return rom, entry, entry + l_target


def write_rom(tmpdir: Path, rom: bytes) -> Path:
    path = tmpdir / "synth.bin"
    path.write_bytes(rom)
    return path


def test_a_delayed_second_target(tmpdir: Path) -> None:
    rom, entry, target_pc = two_path_rom(nop_count=80, target_a=0x300, target_b=0x380)
    rom_path = write_rom(tmpdir, rom)
    targets, steps, reason = producer.explore_exact_target(
        str(rom_path), entry, target_pc, "a0", [], max_entries=64, max_steps=100_000)
    expect(targets == [0x300, 0x380], f"A: delayed second target -- expected [0x300, 0x380], got {targets} ({reason})")
    expect(steps > 65, f"A: the long path genuinely required more than 65 steps to reach the target (steps={steps})")


def test_b_resource_exhaustion(tmpdir: Path) -> None:
    rom, entry, target_pc = two_path_rom(nop_count=80, target_a=0x300, target_b=0x380)
    rom_path = write_rom(tmpdir, rom)
    # The short path reaches the target quickly; the long path (80+ NOPs) cannot within this tiny budget.
    targets, steps, reason = producer.explore_exact_target(
        str(rom_path), entry, target_pc, "a0", [], max_entries=64, max_steps=20)
    expect(targets is None, f"B: resource exhaustion must never emit a partial set (got {targets})")
    expect(reason == "resource_exhausted", f"B: expected resource_exhausted, got {reason}")


def test_c_unresolved_path(tmpdir: Path) -> None:
    entry = 0x400
    code = tst_b_abs_l(SYM_CELL)
    beq_pos = len(code)
    code += w(0x6700)
    bra_pos = len(code)
    code += w(0x6000)
    l_short = len(code)
    code += movea_l_imm(0, 0x300)
    bra2_pos = len(code)
    code += w(0x6000)
    l_long = len(code)
    # The long path becomes genuinely unconstrained (a computed jump through a never-written, never-
    # constrained register) BEFORE it would otherwise reach the shared target -- this must poison the
    # whole proof even though the short path already found a valid target.
    code += movea_l_abs_l(1, UNCONSTRAINED_CELL)
    code += JMP_A1
    code += NOP * 4
    code += movea_l_imm(0, 0x380)
    l_target = len(code)
    code += JMP_A0

    code = bytearray(code)
    code[beq_pos + 1] = (entry + l_short - (entry + beq_pos + 2)) & 0xFF
    code[bra_pos + 1] = (entry + l_long - (entry + bra_pos + 2)) & 0xFF
    code[bra2_pos + 1] = (entry + l_target - (entry + bra2_pos + 2)) & 0xFF
    rom = make_rom(bytes(code), entry=entry)
    rom_path = write_rom(tmpdir, rom)
    target_pc = entry + l_target

    targets, steps, reason = producer.explore_exact_target(
        str(rom_path), entry, target_pc, "a0", [], max_entries=64, max_steps=100_000)
    expect(targets is None, f"C: an unresolved/unconstrained sibling path must poison the whole proof (got {targets})")
    expect(reason == "unconstrained_path", f"C: expected unconstrained_path, got {reason}")


def test_d_target_side_loop_terminates(tmpdir: Path) -> None:
    entry = 0x400
    code = movea_l_imm(0, 0x300)
    code += JMP_A0
    target_pc = entry + len(code) - len(JMP_A0)
    rom = make_rom(bytes(code), entry=entry)
    # 0x300 itself is a self-loop (bra.s *) -- the engine must not step the query-complete state into it.
    full = bytearray(rom)
    full[0x300:0x300 + len(BRA_SELF)] = BRA_SELF
    rom_path = write_rom(tmpdir, bytes(full))

    targets, steps, reason = producer.explore_exact_target(
        str(rom_path), entry, target_pc, "a0", [], max_entries=64, max_steps=100_000)
    expect(targets == [0x300], f"D: target-side loop -- expected [0x300], got {targets} ({reason})")
    expect(steps < 100, f"D: must terminate quickly without stepping into the target's own self-loop (steps={steps})")


def test_e_entry_bound(tmpdir: Path) -> None:
    # Four feasible, concretely-determined targets (no symbolic branch needed): a small jump table
    # indexed by a bounded, concrete register value, reached via four distinct concrete entries.
    entry = 0x400
    targets_concrete = [0x300, 0x340, 0x380, 0x3C0]
    chunks = []
    base = entry
    offsets = []
    for t in targets_concrete:
        offsets.append(len(b"".join(chunks)))
        chunks.append(movea_l_imm(0, t) + JMP_A0)
    code = b"".join(chunks)
    rom = make_rom(code, entry=entry)
    rom_path = write_rom(tmpdir, rom)
    # Every one of the four entries jumps through the SAME shared instruction address (immediately after
    # its own movea) -- but since each chunk's jmp sits at a different PC, use the first chunk's jmp as
    # the probed target_pc and confirm max-entries=1 correctly discards the (here, single) target only
    # when it would exceed the bound; this primarily exercises explore_exact_target's own max_entries
    # plumbing (the full multi-target bound case is already covered by the C++ consumer's
    # external_fact_discarded_over_entry_bound test against the written fact file).
    target_pc = entry + offsets[0] + len(movea_l_imm(0, targets_concrete[0]))
    targets, steps, reason = producer.explore_exact_target(
        str(rom_path), entry, target_pc, "a0", [], max_entries=0, max_steps=1000)
    expect(targets is None and reason == "entry_bound_exceeded",
           f"E: a single target with max_entries=0 must be discarded as over-bound (got {targets}, {reason})")


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        test_a_delayed_second_target(tmpdir)
        test_b_resource_exhaustion(tmpdir)
        test_c_unresolved_path(tmpdir)
        test_d_target_side_loop_terminates(tmpdir)
        test_e_entry_bound(tmpdir)
    if failures:
        print(f"{failures} failure(s)", file=sys.stderr)
        return 1
    print("segarecomp_angr_m68k_facts_test: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
