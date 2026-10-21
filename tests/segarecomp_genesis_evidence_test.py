#!/usr/bin/env python3
"""SEG-077-T001: tests of the Genesis evidence-tier labeller's pure logic (standard library, synthetic data only; no MAME/Docker/ROM).

Covers image identity normalization (plain, 512-byte copier header, SMD interleave, rejection), the VDP DMA reconstruction state machine
(register writes, two-word commands, M1 gate, fill/copy exclusion, byte-write duplication, cancellation, zero length, 128 KiB window,
post-transfer source advance), MAME trace and m68k-disasm parsing, direct-flow kinds, executed/static-reach acceptance (two-decoder length
agreement, overlap, inline-argument calls never extended, conflicts), data assembly (hardware header, DMA/code conflict, Ghidra veto,
DR extents), data-read admission (code readers, prefetch shadow, unaccepted readers), the executed-byte mask (a rejected executed PC
never becomes data), withdrawal of static reach that conflicts with observed data, the SRAM rule, read-tap key decoding, cell statistics,
synthetic soundness scoring and the frozen-plan invariants.
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_genesis_evidence as ge  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL:", msg)


def raises(fn, *args):
    try:
        fn(*args)
    except ge.EvidenceError:
        return True
    return False


def cart(size=0x4000, reset=0x200):
    img = bytearray(size)
    img[0:4] = (0x00FFFE00).to_bytes(4, "big")
    img[4:8] = reset.to_bytes(4, "big")
    img[0x100:0x110] = b"SEGA GENESIS    "
    img[0x1A4:0x1A8] = (size - 1).to_bytes(4, "big")
    return img


def tokens(img, lengths):
    """Token rows: status 2 (ordinary) with the given word length at listed byte addresses, status 0 elsewhere."""
    t = bytearray(len(img) // 2 * ge.TOKEN_FIELDS)
    for pc, n in lengths.items():
        t[pc // 2 * ge.TOKEN_FIELDS] = 2
        t[pc // 2 * ge.TOKEN_FIELDS + 1] = n // 2
    return bytes(t)


def interleave(img):
    out = bytearray(len(img))
    for b in range(0, len(img), 0x4000):
        for i in range(0x2000):
            out[b + i] = img[b + 2 * i + 1]
            out[b + 0x2000 + i] = img[b + 2 * i]
    return bytes(out)


def test_identity():
    img = bytes(cart())
    i = ge.identity(img)
    check(i["format"] == "plain" and i["reset"] == 0x200 and i["reset_in_image"] and i["header_rom_end_matches"], "plain identity")
    hdr = bytearray(512)
    check(ge.identity(bytes(hdr) + img)["format"] == "copier512", "copier header stripped")
    check(ge.identity(bytes(hdr) + img)["sha256"] == i["sha256"], "copier normalized digest equals plain")
    hdr[8:10] = b"\xaa\xbb"
    smd = bytes(hdr) + interleave(img)
    j = ge.identity(smd)
    check(j["format"] == "smd" and j["sha256"] == i["sha256"], "SMD deinterleaved to the plain image")
    check(raises(ge.identity, bytes(0x4000)), "no SEGA header rejected")
    check(raises(ge.identity, bytes(img) + b"\0" * 7), "unaligned size rejected")


def test_vdp():
    v = ge.VdpDma()
    for w in (0x8114, 0x8F02, 0x9320, 0x9400, 0x9500 | 0x80, 0x9600 | 0x01, 0x9700):
        v.write(w)
    v.write(0x4000)
    v.write(0x0080)
    check(v.events == {((0x0180) * 2, 0x40): 1}, f"68K->VRAM DMA source/length {v.events}")
    check(v.regs[0x13] == 0 and v.regs[0x14] == 0 and v.regs[0x15] == 0xA0, "post-DMA length cleared, source advanced")
    v.write(0x9320)
    v.write(0x4000)
    v.write(0x0080)
    check(((0x01A0) * 2, 0x40) in v.events, "second DMA uses the advanced source")
    v2 = ge.VdpDma()
    for w in (0x8104, 0x9320, 0x4000, 0x0080):
        v2.write(w)
    check(v2.events == {}, "M1 clear: no DMA")
    v3 = ge.VdpDma()
    for w in (0x8114, 0x9780, 0x4000, 0x0080):
        v3.write(w)
    check(v3.events == {}, "fill/copy mode is not a 68K-bus source")
    v4 = ge.replay_vdp_log(["W 4 8114 ffff", "W 4 4000 ffff", "R 4", "W 4 8114 ffff", "W 4 93 ff", "W 4 4000 ffff", "W 6 80 ffff"])
    check(v4.regs[0x13] == 0 and len(v4.events) == 1, "byte write duplicated; read cancels a pending first half")
    k = list(v4.events)[0]
    check(k[1] == 0x93 * 2, f"byte-duplicated register 0x13 = 0x93 {k}")
    v5 = ge.VdpDma()
    for w in (0x8114, 0x9300, 0x9400, 0x9500, 0x96F0, 0x9700, 0x4000, 0x0080):
        v5.write(w)
    check(list(v5.events) == [(0xF000 * 2, 0x20000)], "zero length = 0x10000 words")
    check(v5.rom_extents(0x40000) == [(0x1E000, 0x20000)], "extent clipped at the 128 KiB window")
    check(raises(ge.replay_vdp_log, ["X 1"]), "bad log line rejected")


def test_parsers():
    first = ge.parse_trace_first(["000200: move    #$2700, SR", "000200: again", "FF0000: rts", "   (loops for 3 instructions)",
                                  "A00000: nop"])
    check(first["rom"] == {0x200: "move    #$2700, SR"} and first["ram"] == [0xFF0000] and first["other"] == 1, "trace first-occurrence")
    d = ge.parse_disasm("\tmovew #0x2700,%sr | 46fc 2700 @00000200\n\t.short 0x5283 | 5283 @000002be\n\t.short 0x1234\n"
                        "\trts | 4e75 @00000204\n")
    check(d == {0x200: (4, "movew"), 0x204: (2, "rts")}, f"disasm parse {d}")
    r = ge.parse_reads(["%x" % ((0x204 << 26) | (0x300 << 2) | 0), "%x" % ((0x204 << 26) | (0x310 << 2) | 2),
                        "%x" % ((0x206 << 26) | (0x5000 << 2) | 1)], 0x4000)
    check(r == {0x204: {0x300, 0x301, 0x311}}, f"read keys: lanes and out-of-image dropped {r}")
    check(ge.ranges([1, 2, 3, 7]) == [[1, 4], [7, 8]] and ge.unranges([[1, 3]]) == {1, 2}, "ranges")


def test_flow():
    img = bytearray(0x1000)

    def at(pc, *words):
        for i, w in enumerate(words):
            img[pc + 2 * i:pc + 2 * i + 2] = w.to_bytes(2, "big")
    at(0x200, 0x6004)              # BRA.S +4
    at(0x210, 0x6100, 0x0010)      # BSR.W
    at(0x220, 0x6702)              # BEQ.S
    at(0x230, 0x51C8, 0xFFFE)      # DBRA
    at(0x240, 0x4EB9, 0x0000, 0x0400)  # JSR abs.l
    at(0x250, 0x4EFA, 0x0010)      # JMP d16(PC)
    at(0x260, 0x4ED0)              # JMP (A0)
    at(0x270, 0x4E75)
    at(0x280, 0x4E41)
    at(0x290, 0xA123)
    at(0x2A0, 0x4EB8, 0x8000)      # JSR abs.w negative -> 0xFFFF8000
    b = bytes(img)
    check(ge.flow(b, 0x200, 2) == ([0x206], "term"), "BRA term")
    check(ge.flow(b, 0x210, 4) == ([0x222], "call"), "BSR call")
    check(ge.flow(b, 0x220, 2) == ([0x224], "normal"), "Bcc normal")
    check(ge.flow(b, 0x230, 4) == ([0x230], "normal"), "DBcc")
    check(ge.flow(b, 0x240, 6) == ([0x400], "call"), "JSR abs.l")
    check(ge.flow(b, 0x250, 4) == ([0x262], "term"), "JMP pc-relative")
    check(ge.flow(b, 0x260, 2) == ([], "term"), "dynamic JMP")
    check(ge.flow(b, 0x270, 2) == ([], "term"), "RTS")
    check(ge.flow(b, 0x280, 2) == ([], "call"), "TRAP is call-like")
    check(ge.flow(b, 0x290, 2) == ([], "term"), "line A")
    check(ge.flow(b, 0x2A0, 4)[0] == [0xFFFF8000], "abs.w sign extension")


def build_case():
    img = bytearray(cart(0x1000))

    def at(pc, *words):
        for i, w in enumerate(words):
            img[pc + 2 * i:pc + 2 * i + 2] = w.to_bytes(2, "big")
    at(0x200, 0x4EB9, 0x0000, 0x0300)   # JSR 0x300 (return site executed)
    at(0x206, 0x6704)                   # BEQ.S 0x20C (not taken)
    at(0x208, 0x4EBA, 0x0100)           # JSR 0x30A (return site NOT executed: inline args follow)
    at(0x20C, 0x7001)                   # moveq (static reach only)
    at(0x20E, 0x4E75)
    at(0x300, 0x4E75)
    at(0x30A, 0x4E75)
    return bytes(img)


def test_build_code():
    img = build_case()
    lengths = {0x200: 6, 0x206: 2, 0x208: 4, 0x20C: 2, 0x20E: 2, 0x300: 2, 0x30A: 2, 0x20A: 2}
    tok = tokens(img, lengths)
    walk = {pc: (n, "x") for pc, n in lengths.items()}
    typed = {}
    code = ge.build_code(img, [0x200, 0x206, 0x208, 0x300, 0x30A], {}, walk, tok, typed)
    check(code.get(0x200) == (6, "E") and code.get(0x30A) == (2, "E"), "executed accepted")
    check(code.get(0x20C) == (2, "R") and code.get(0x20E) == (2, "R"), "untaken Bcc target reached statically")
    check(0x20A not in code and 0x20C not in [p for p, (n, t) in code.items() if t == "E"], "no fall-through after an inline-argument JSR")
    typed = {}
    walk2 = dict(walk)
    walk2[0x206] = (4, "x")
    code2 = ge.build_code(img, [0x200, 0x206, 0x201], {}, walk2, tok, typed)
    check(0x206 not in code2 and typed.get("E_LENGTH_DISAGREE") == 1 and typed.get("E_ODD_PC") == 1, f"disagreement/odd typed {typed}")
    typed = {}
    tok3 = tokens(img, {0x200: 6, 0x202: 2})
    code3 = ge.build_code(img, [0x200, 0x202], {}, {0x200: (6, "x"), 0x202: (2, "x")}, tok3, typed)
    check(code3 == {} and typed.get("E_OVERLAPPING_EXECUTED") == 2, f"overlapping executed spans masked {typed}")
    typed = {}
    code4 = ge.build_code(img, [0x200, 0x206], {}, {0x200: (6, "x"), 0x206: (2, "x")}, tokens(img, {0x200: 6, 0x206: 2}), typed)
    check(0x20C not in code4 and typed.get("R_DISASM_NO_DECODE", 0) >= 1, "static reach requires the independent decoder")


def test_assemble_and_reads():
    img = build_case()
    code = {0x200: (6, "E"), 0x206: (2, "E"), 0x300: (2, "E")}
    block = ge.executed_mask([0x200, 0x206, 0x300], code, len(img))
    typed = {}
    cls, ext = ge.assemble(img, code, block, [(0x400, 0x480), (0x2FE, 0x310)], {0x600, 0x601}, [0x420], typed)
    check(cls[0:0x200] == b"D" * 0x200, "hardware header data")
    check(cls[0x400:0x420] == b"D" * 0x20 and cls[0x420:0x42A] == b"U" * 10 and cls[0x42A] == ord("D"), "Ghidra veto of 10 bytes")
    check(cls[0x2FE] == ord("U") and typed.get("DMA_CODE_CONFLICT") == 1 and any(e["masked"] for e in ext), "DMA over code masked whole")
    check(cls[0x600] == ord("D") and typed.get("DR_BYTES_ADDED") == 2 and {"kind": "DR", "start": 0x600, "end": 0x602, "masked": False} in ext,
          "data reads added and recorded as a DR extent")
    check(cls[0x200] == ord("I") and cls[0x201:0x206] == b"c" * 5, "I/c")
    readers = {0x200: {0x203, 0x700, 0x701}, 0x206: {0x800, 0x300}, 0x300: {0x900}, 0x999: {0x950}, 0xFF0000: {0x20C, 0xA00}}
    typed = {}
    dr = ge.data_reads(code, readers, block, 0x1000, typed)
    check(0x700 in dr and 0x701 in dr and 0x203 not in dr, "own fetch window excluded")
    check(0x800 not in dr and typed.get("DR_CODE_READER") == 1, "a reader of accepted code is excluded")
    check(0x900 in dr, "plain data read admitted")
    check(0x950 not in dr and typed.get("DR_READER_NOT_ACCEPTED") == 1, "unaccepted ROM reader excluded")
    check(0xA00 in dr and 0x20C in dr, "RAM reader admitted for non-code bytes")
    typed = {}
    dr3 = ge.data_reads(code, {0x300: {0x206 + 2 + 1}}, block, 0x1000, typed)
    check(dr3 == set() and typed.get("DR_PREFETCH_SHADOW") == 1, "prefetch shadow after executed code")


def test_rejected_executed_never_data():
    # C1 review reproducer: a branch's refill of a REJECTED executed target must not become data
    img = bytearray(0x1000)
    code = {0x200: (2, "E")}
    block = ge.executed_mask([0x200, 0x300], code, len(img))
    check(block[0x300:0x30A] == b"\x01" * 10 and block[0x30A] == 0, "rejected executed PC masks 10 bytes")
    typed = {}
    dr = ge.data_reads(code, {0x200: {0x200, 0x201, 0x202, 0x203, 0x300, 0x301, 0x302, 0x303}}, block, len(img), typed)
    check(dr == set() and typed.get("DR_CODE_READER") == 1, "a reader of rejected executed bytes is a code reader")
    typed = {}
    cls, _ = ge.assemble(bytes(img), code, block, [(0x2F0, 0x310)], {0x305}, [], typed)
    check(cls[0x300:0x30A] == b"U" * 10 and typed.get("DMA_CODE_CONFLICT") == 1 and typed.get("DR_BLOCKED_BYTES") == 1,
          "DMA extent masked whole and DR byte dropped over executed bytes")
    check(ge.executed_mask([0x301], {}, 0x1000)[0x300] == 1, "odd executed PC rounds down")


def test_execution_inside_rejected_instruction():
    # G1 finding: the outer instruction is rejected (one decoder cannot decode it) while two instructions executed inside its immediate
    # decode fine; none of them may be labelled
    img = bytearray(cart(0x1000))
    img[0x400:0x406] = bytes.fromhex("203c4e714e75")
    img[0x406:0x408] = bytes.fromhex("4e75")
    tok = tokens(img, {0x400: 6, 0x402: 2, 0x404: 2, 0x406: 2})
    walk = {0x402: (2, "nop"), 0x404: (2, "rts"), 0x406: (2, "rts")}
    typed = {}
    code = ge.build_code(bytes(img), [0x400, 0x402, 0x404, 0x406], {}, walk, tok, typed)
    check(0x402 not in code and 0x404 not in code and code.get(0x406) == (2, "E"), f"execution inside a rejected instruction masked {code}")
    check(typed.get("E_OVERLAPPING_EXECUTED") == 2 and typed.get("E_DISASM_NO_DECODE") == 1, f"typed {typed}")
    m = ge.executed_mask([0x400, 0x402, 0x404, 0x406], code, 0x1000, walk, {}, tok)
    check(m[0x400:0x408] == b"\x01" * 8 and m[0x408] == 0, "rejected extent uses the longest decoder length")


def test_r_withdrawn_on_data_conflict():
    img = build_case()
    lengths = {0x200: 6, 0x206: 2, 0x208: 4, 0x20C: 2, 0x20E: 2, 0x300: 2, 0x30A: 2}
    tok = tokens(img, lengths)
    walk = {pc: (n, "x") for pc, n in lengths.items()}
    executed = [0x200, 0x206, 0x208, 0x300, 0x30A]
    code = ge.build_code(img, executed, {}, walk, tok, {})
    check(code[0x20C][1] == "R" and code[0x20E][1] == "R", "R present before the conflict")
    typed = {}
    block = ge.executed_mask(executed, code, len(img))
    code2, block2 = ge.withdraw_conflicts(img, executed, {}, walk, tok, code, block, {0x20C}, [], typed)
    check(0x20C not in code2 and 0x20E not in code2 and typed.get("R_DATA_CONFLICT") == 1, "conflicting R and its R-only successor withdrawn")
    check(block2[0x20C] == 1 and block2[0x20D] == 1, "withdrawn bytes blocked from data")
    cls, _ = ge.assemble(img, code2, block2, [], {0x20C}, [], typed)
    check(cls[0x20C] == ord("U"), "conflict byte stays unknown")


def test_sram_rule():
    img = cart(0x400000)
    img[0x1B0:0x1B2] = b"RA"
    check(raises(ge.identity, bytes(img)), "declared SRAM over a >2 MiB image rejected")
    small = cart()
    small[0x1B0:0x1B2] = b"RA"
    check(ge.identity(bytes(small))["sram_declared"], "declared SRAM recorded")


def test_stats_and_score():
    cls = b"I" + b"c" * 3 + b"D" * 124 + b"D" * 128 + b"U" * 128
    img = bytes(len(cls))
    st = ge.cell_stats(cls, img, tokens(img, {}))
    check(st == {"cells": 3, "positive": 1, "negative": 1, "masked": 1, "mixed_positive": 1, "hard_negative": 1}, f"cells {st}")
    s = ge.score(b"IcDU", b"IcUD")
    check(s["wrong"] == {"I": 0, "c": 0, "D": 1} and s["recalled_starts"] == 1, f"score {s}")
    lua = ge.scenario_lua({"id": "p", "policy": "random", "seed": 3, "seconds": 9, "start_window": 4}, 0x1000)
    check("seed = 3" in lua and "seconds = 9" in lua and "__" not in lua, "scenario substitution")
    check(raises(ge.scenario_lua, {"policy": "magic", "seconds": 1}, 0x1000), "unknown policy rejected")


def test_plan_frozen():
    if ge.PLAN.exists():
        try:
            ge.verify_plan_static()
        except ge.EvidenceError as e:
            check(False, f"frozen plan no longer verifies: {e}")
        doc = __import__("json").loads(ge.PLAN.read_text())
        check("0 wrong I, c or D" in doc["gates"]["G1_synthetic_soundness"], "G1 is soundness on the synthetic fixture")
        check(doc["scenarios"] == ge.SCENARIOS and doc["replicas"] == 2, "frozen scenarios and replicas")


def main():
    for t in (test_plan_frozen, test_identity, test_vdp, test_parsers, test_flow, test_build_code, test_assemble_and_reads,
              test_rejected_executed_never_data, test_execution_inside_rejected_instruction, test_r_withdrawn_on_data_conflict, test_sram_rule, test_stats_and_score):
        t()
    print("FAILURES:", len(FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
