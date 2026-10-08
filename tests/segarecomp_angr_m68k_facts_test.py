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


def test_f_rte_on_path_poisons_proof(tmpdir: Path) -> None:
    """SEG-041-T002: RTE is legal base-MC68000 (so a general legal-opcode screen would not catch it) but
    has no usable exception-return semantics in the available p-code backend -- it lifts to an
    unconditional jump to address 0, confirmed empirically, not merely asserted. Constructs a path that
    executes RTE and -- if RTE were (incorrectly) treated as ordinary control transfer -- would reach a
    normal-looking, decodable target: a `movea.l`+`jmp` sequence placed at address 0 (where RTE's actual
    p-code lift transfers control), landing on a legal `bra.s *` at 0x300. Without this task's explicit
    RTE exclusion, this exact fixture soundly (by the algorithm's own other rules) produces `exact
    {0x300}` -- confirmed by a one-off negative control during development, not shipped here, since the
    point of this regression is that the REAL producer must refuse it, not that a hypothetically-
    unguarded one would reach it."""
    entry = 0x400
    rte = w(0x4E73)
    movea_and_jmp = movea_l_imm(0, 0x300) + JMP_A0
    rom = bytearray(make_rom(rte, entry=entry))
    rom[0:len(movea_and_jmp)] = movea_and_jmp  # RTE's p-code lift jumps to address 0 in this backend
    rom[0x300:0x300 + len(BRA_SELF)] = BRA_SELF
    rom_path = write_rom(tmpdir, bytes(rom))
    target_pc = len(movea_l_imm(0, 0x300))  # the jmp (A0) instruction, now living at address 0

    targets, steps, reason = producer.explore_exact_target(
        str(rom_path), entry, target_pc, "a0", [], max_entries=64, max_steps=1000)
    expect(targets is None, f"F: a path through RTE must never emit a fact (got {targets})")
    expect(reason == "unsupported_proof_path_rte", f"F: expected unsupported_proof_path_rte, got {reason}")


def jmp_pcidx_word(d_register: bool, index_num: int, long_index: bool, disp8: int) -> bytes:
    ext = (0x0000 if d_register else 0x8000) | (index_num << 12) | (0x0800 if long_index else 0x0000) | (disp8 & 0xFF)
    return w(0x4EFB) + w(ext)


def test_g_pc_step_matches_register_read(tmpdir: Path) -> None:
    """SEG-042-T002: `explore_exact_target_pc` (read the PC one step after the dynamic-control
    instruction) must agree with `explore_exact_target` (read the target register directly beforehand)
    on the shared case both can describe -- a plain `jmp (a0)`."""
    entry = 0x400
    code = movea_l_imm(0, 0x300) + JMP_A0
    target_pc = entry + len(movea_l_imm(0, 0x300))
    rom = bytearray(make_rom(bytes(code), entry=entry))
    rom[0x300:0x300 + len(BRA_SELF)] = BRA_SELF
    rom_path = write_rom(tmpdir, bytes(rom))

    register_targets, _, register_reason = producer.explore_exact_target(
        str(rom_path), entry, target_pc, "a0", [], max_entries=64, max_steps=1000)
    pc_targets, steps, pc_reason, fetched_words = producer.explore_exact_target_pc(
        str(rom_path), entry, target_pc, [], max_entries=64, max_steps=1000)
    expect(register_reason == "closed" and pc_reason == "closed", f"G: both modes must close ({register_reason}, {pc_reason})")
    expect(pc_targets == register_targets == [0x300], f"G: PC-step and register-read modes must agree (got {pc_targets} vs {register_targets})")
    expect(0x4ED0 in fetched_words, f"G: the jmp (a0) opcode word itself must be in fetched_words (got {[hex(x) for x in fetched_words]})")
    expect(steps >= 1, "G: the PC-step mode must have taken at least one step to execute the jmp itself")


def test_h_pc_step_resolves_pc_relative_index_form(tmpdir: Path) -> None:
    """SEG-042-T002: the actual new capability -- a `jmp (d8,PC,D0.w)` site, whose effective address is
    only computed as part of EXECUTING the instruction (address-of-extension-word + sign-extended D0.w +
    disp8), not sitting in any single register beforehand. `explore_exact_target` (register-read) cannot
    describe this family at all; `explore_exact_target_pc` must resolve it exactly, enumerating both
    concrete values a small, bounded D0 (set to 0 or 2 -- MC68000's brief extension word has no scale
    field, so two cleanly 2-byte-separated, even-aligned landing pads need an index that itself differs
    by 2) takes along the two feasible paths reaching it."""
    entry = 0x400
    code = tst_b_abs_l(SYM_CELL)              # 6 bytes
    beq_pos = len(code)
    code += w(0x6700)                          # beq.b (fixup) -> short path: D0 = 0
    bra_pos = len(code)
    code += w(0x6000)                          # bra.b (fixup) -> long path: D0 = 2
    l_short = len(code)
    code += w(0x7000)                           # moveq #0,D0
    bra2_pos = len(code)
    code += w(0x6000)                           # bra.b (fixup) -> l_join
    l_long = len(code)
    code += w(0x7002)                           # moveq #2,D0
    l_join = len(code)
    target_pc = entry + l_join                  # the jmp instruction's own opcode-word PC (the site)
    ext_word_pc = target_pc + 2
    pad_a_addr = target_pc + 4                   # right after the 4-byte jmp (d8,PC,D0.w) instruction
    pad_b_addr = pad_a_addr + 2
    disp8 = pad_a_addr - ext_word_pc
    code += jmp_pcidx_word(d_register=True, index_num=0, long_index=False, disp8=disp8)
    code += w(0x4E71) + w(0x4E71)                 # pad_a, pad_b: never executed, just need to exist as addresses

    code = bytearray(code)
    code[beq_pos + 1] = (entry + l_short - (entry + beq_pos + 2)) & 0xFF
    code[bra_pos + 1] = (entry + l_long - (entry + bra_pos + 2)) & 0xFF
    code[bra2_pos + 1] = (entry + l_join - (entry + bra2_pos + 2)) & 0xFF
    rom = make_rom(bytes(code), entry=entry)
    rom_path = write_rom(tmpdir, rom)

    targets, steps, reason, fetched_words = producer.explore_exact_target_pc(
        str(rom_path), entry, target_pc, [], max_entries=64, max_steps=1000)
    expect(reason == "closed", f"H: pc-relative index form must close soundly (got {reason})")
    expect(targets == [pad_a_addr, pad_b_addr], f"H: expected both D0-selected landing pads, got {targets}")
    expect(0x4EFB in fetched_words, "H: the jmp (d8,PC,Xn) opcode word itself must be in fetched_words")


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
        test_f_rte_on_path_poisons_proof(tmpdir)
        test_g_pc_step_matches_register_read(tmpdir)
        test_h_pc_step_resolves_pc_relative_index_form(tmpdir)
    if failures:
        print(f"{failures} failure(s)", file=sys.stderr)
        return 1
    print("segarecomp_angr_m68k_facts_test: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
