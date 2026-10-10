#!/usr/bin/env python3
"""SEG-044-T002: tests for tools/segarecomp_source_map_extract.py using project-authored synthetic listings/ROMs only."""
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import segarecomp_source_map_extract as ex  # noqa: E402

REV = "0123456789abcdef0123456789abcdef01234567"


def row(num, addr, data, src, nest=""):
    field = " ".join(data[i:i + 4] for i in range(0, len(data), 4)) if data else ""
    return f"{nest}{num:>6}/{addr:>8X} : {field:<20}{src}"


def cont(addr, data):
    return f"{'':>13}{addr:>5X} : {data}"


class Fixture:
    """Builds a consistent (listing, rom) pair: nop; bra.w; dc.w; binclude(8); rts; Z80 block; move.w; dc.b pad."""

    def __init__(self):
        self.rom = bytearray(64)
        self.lines = ["AS V1.42 Beta [Bld 212] - Source File x.asm - Page 1", ""]
        self.n = 0

    def put(self, addr, hexdata, src, nest=""):
        self.n += 1
        self.rom[addr:addr + len(hexdata) // 2] = bytes.fromhex(hexdata)
        self.lines.append(row(self.n, addr, hexdata, src, nest))

    def directive(self, addr, src, nest=""):
        self.n += 1
        self.lines.append(row(self.n, addr, "", src, nest))

    def build(self):
        return "\n".join(self.lines) + "\n", bytes(self.rom)


def good():
    f = Fixture()
    f.directive(0, "\tcpu 68000")
    f.put(0, "4E71", "Start:\tnop")                       # 0
    f.put(2, "60000004", "\tbra.w\tNext")                 # 2..5
    f.put(6, "1234", "\tdc.w\t$1234")                      # 6..7 data
    f.directive(8, "\tbinclude\t\"asset.bin\"")            # 8..15 binary asset (no bytes in listing)
    f.rom[8:16] = bytes(range(1, 9))
    f.put(16, "4E75", "Next:\trts")                        # 16
    f.directive(18, "\tsave")
    f.directive(18, "\tCPU Z80")
    f.put(18, "AF", "\txor\ta")                            # z80 byte, must not enter C
    f.directive(19, "\trestore")
    f.put(20, "303C0001", "\tmove.w\t#1,d0")               # 20
    return f


class Extract(unittest.TestCase):
    def run_ok(self, f, **kw):
        listing, rom = f.build()
        return ex.extract(listing, rom, REV, None, **kw)

    def code(self, f, **kw):
        listing, rom = f.build()
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract(listing, rom, REV, kw.pop("expect_rev", None), **kw)
        return ctx.exception.code

    def test_success_and_gap_accounting(self):
        # bytes 21..63 stay uncovered: tail needs an explaining directive
        f = good()
        f.directive(24, "\tbinclude\t\"tail.bin\"")
        text, summary = self.run_ok(f)
        lines = text.splitlines()
        self.assertEqual(lines[0], ex.SCHEMA)
        self.assertEqual(lines[1], "rom_sha256 " + hashlib.sha256(f.build()[1]).hexdigest())
        self.assertIn("entries 4", lines)
        self.assertEqual(lines[lines.index("entries 4") + 1:-1], ["00000000", "00000002", "00000010", "00000014"])
        self.assertEqual(lines[-1], "end")
        self.assertEqual(summary["instruction_starts"], 4)

    def test_unexplained_range_is_rejected(self):
        self.assertEqual(self.code(good()), "unexplained_rom_range")

    def test_wrong_rom_hash(self):
        f = good(); f.directive(24, "\tbinclude\t\"t\"")
        listing, rom = f.build()
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract(listing, rom, REV, None, expect_rom_sha256="0" * 64)
        self.assertEqual(ctx.exception.code, "rom_hash_mismatch")

    def test_wrong_source_revision(self):
        f = good(); f.directive(24, "\tbinclude\t\"t\"")
        self.assertEqual(self.code(f, expect_rev="f" * 40), "source_revision_mismatch")
        listing, rom = f.build()
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract(listing, rom, "main", None)
        self.assertEqual(ctx.exception.code, "bad_identity")

    def test_duplicate_entry(self):
        f = good(); f.directive(24, "\tbinclude\t\"t\"")
        f.put(0, "4E71", "\tnop")
        self.assertEqual(self.code(f), "overlapping_rows")

    def test_odd_entry(self):
        f = Fixture()
        f.put(1, "4E71", "\tnop")
        self.assertEqual(self.code(f), "instruction_odd")

    def test_row_outside_rom(self):
        f = Fixture()
        listing, rom = f.build()
        listing += row(9, 0x1000, "4E71", "\tnop") + "\n"
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract(listing, rom, REV, None)
        self.assertEqual(ctx.exception.code, "row_outside_rom")

    def test_opcode_differs_from_rom(self):
        f = Fixture()
        f.put(0, "4E71", "\tnop")
        listing, rom = f.build()
        rom = b"\x4e\x75" + rom[2:]
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract(listing, rom, REV, None)
        self.assertEqual(ctx.exception.code, "instruction_opcode_differs_from_rom")

    def test_malformed_listing(self):
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract("garbage\nmore garbage\n", bytes(8), REV, None)
        self.assertEqual(ctx.exception.code, "not_a_listing")
        f = good(); f.directive(24, "\tbinclude\t\"t\"")
        listing, rom = f.build()
        listing = listing.replace("6000 0004", "6000     ")  # bytes truncated to a non-hex field
        # the row now emits no bytes: its ROM range becomes unexplained rather than silently trusted
        with self.assertRaises(ex.SourceMapError):
            ex.extract(listing, rom, REV, None)

    def test_continuation_gap_rejected(self):
        f = Fixture()
        f.put(0, "4E71", "\tdc.b\t1")
        listing, rom = f.build()
        listing += cont(0x40, "0000") + "\n"
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract(listing, rom, REV, None)
        self.assertEqual(ctx.exception.code, "listing_continuation_gap")

    def test_unknown_construct_with_bytes(self):
        f = Fixture()
        f.put(0, "4E71", "\tfrobnicate\td0")
        self.assertEqual(self.code(f), "unknown_construct_with_bytes")

    def test_unknown_cpu_and_unbalanced_modes(self):
        f = Fixture(); f.directive(0, "\tcpu 6502")
        self.assertEqual(self.code(f), "unknown_cpu")
        f = Fixture(); f.directive(0, "\trestore")
        self.assertEqual(self.code(f), "unbalanced_restore")
        f = Fixture(); f.directive(0, "\tsave"); f.directive(0, "\tcpu z80"); f.put(0, "AF", "\txor a")
        self.assertEqual(self.code(f), "cpu_mode_not_restored")

    def test_macro_definition_bodies_and_conditionals_are_ignored(self):
        f = good(); f.directive(24, "\tbinclude\t\"t\"")
        f.directive(24, "stuff:\tmacro")
        f.directive(24, "\tcpu z80")                       # inside a macro definition: must not change the mode
        f.directive(24, "\tmove.w\td0,d1")                 # definition body: no bytes, no authority
        f.directive(24, "\tendm")
        text, _ = self.run_ok(f)
        self.assertIn("entries 4", text)

    def test_macro_expanded_instruction_counts(self):
        f = good()
        f.directive(24, "\tstopZ80")
        f.put(24, "33FC0100", "\tmove.w\t#$100,(z80_bus_request).l", nest="(1)")
        f.directive(28, "\tbinclude\t\"t\"")
        text, _ = self.run_ok(f)
        self.assertIn("00000018", text)

    def test_macro_bytes_in_definition_rejected(self):
        f = Fixture()
        f.directive(0, "m:\tmacro")
        f.put(0, "4E71", "\tnop")
        self.assertEqual(self.code(f), "bytes_inside_macro_definition")

    def test_operand_overlay_idiom_only(self):
        # `1+field` operand printed by an earlier pass, then fixed by `org *-1 / dc.b 0`: allowed on an operand byte
        f = Fixture()
        f.put(0, "D1690001", "\tadd.w\td0,1+0(a1)")
        listing, rom = f.build()
        rom = bytearray(rom); rom[3] = 0
        overlay = row(2, 3, "00", "\tdc.b\t0")
        text, summary = ex.extract(listing + overlay + "\n" + row(3, 4, "", "\tbinclude\t\"t\"") + "\n", bytes(rom), REV, None)
        self.assertEqual(summary["operand_overlays"], 1)
        # without the overlay the same difference is rejected
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract(listing + row(3, 4, "", "\tbinclude\t\"t\"") + "\n", bytes(rom), REV, None)
        self.assertEqual(ctx.exception.code, "listing_bytes_differ_from_rom")
        # overwriting an opcode word is never allowed
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract(listing + row(2, 1, "00", "\tdc.b\t0") + "\n", bytes(rom), REV, None)
        self.assertIn(ctx.exception.code, ("overlapping_rows", "instruction_opcode_differs_from_rom", "data_differs_from_rom"))

    def test_reordered_instruction_rows_give_identical_output(self):
        f = good(); f.directive(24, "\tbinclude\t\"t\"")
        a, _ = self.run_ok(f)
        listing, rom = f.build()
        lines = listing.split("\n")
        idx = [i for i, l in enumerate(lines) if "nop" in l or "rts" in l]
        lines[idx[0]], lines[idx[1]] = lines[idx[1]], lines[idx[0]]
        b, _ = ex.extract("\n".join(lines), rom, REV, None)
        self.assertEqual(a, b)

    def test_mutation_omitting_an_instruction_row_is_detected(self):
        f = good(); f.directive(24, "\tbinclude\t\"t\"")
        listing, rom = f.build()
        mutated = "\n".join(l for l in listing.split("\n") if "bra.w" not in l)
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract(mutated, rom, REV, None)
        self.assertEqual(ctx.exception.code, "unexplained_rom_range")

    def test_nested_block_inside_macro_definition_does_not_end_it_early(self):
        # reviewer repro: `while ... endm` inside a macro body used to end macro mode early, so a following `cpu z80` took effect
        f = good(); f.directive(24, "\tbinclude\t\"t\"")
        f.directive(24, "m:\tmacro")
        f.directive(24, "\twhile 0")
        f.directive(24, "\tendm")
        f.directive(24, "\tsave")
        f.directive(24, "\tcpu z80")
        f.directive(24, "\tendm")  # closes the macro definition (still hidden in the body)
        text, _ = self.run_ok(f)
        self.assertIn("entries 4", text)

    def test_pad_directive_cannot_mask_a_deleted_instruction_row(self):
        # reviewer repro: an `even` row at the address of a deleted instruction row must not explain a non-fill gap
        f = good(); f.directive(24, "\tbinclude\t\"t\"")
        f.directive(20, "\teven")
        listing, rom = f.build()
        mutated = "\n".join(l for l in listing.split("\n") if "move.w" not in l)
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract(mutated, rom, REV, None)
        self.assertEqual(ctx.exception.code, "unexplained_rom_range")

    def test_size_and_count_exhaustion(self):
        f = good(); f.directive(24, "\tbinclude\t\"t\"")
        listing, rom = f.build()
        old = ex.MAX_ENTRIES
        try:
            ex.MAX_ENTRIES = 2
            with self.assertRaises(ex.SourceMapError) as ctx:
                ex.extract(listing, rom, REV, None)
            self.assertEqual(ctx.exception.code, "universe_too_large")
        finally:
            ex.MAX_ENTRIES = old
        old = ex.MAX_LISTING_BYTES
        try:
            ex.MAX_LISTING_BYTES = 10
            with self.assertRaises(ex.SourceMapError) as ctx:
                ex.extract(listing, rom, REV, None)
            self.assertEqual(ctx.exception.code, "listing_too_large")
        finally:
            ex.MAX_LISTING_BYTES = old

    def test_cli_is_deterministic_and_fail_closed(self):
        f = good(); f.directive(24, "\tbinclude\t\"t\"")
        listing, rom = f.build()
        tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "segarecomp_source_map_extract.py")
        with tempfile.TemporaryDirectory() as tmp:
            lp, rp = os.path.join(tmp, "x.lst"), os.path.join(tmp, "x.rom")
            open(lp, "w").write(listing); open(rp, "wb").write(rom)
            outs = []
            for i in range(2):
                out = os.path.join(tmp, f"u{i}.txt")
                result = subprocess.run([sys.executable, tool, "--listing", lp, "--rom", rp, "--output", out, "--source-revision", REV],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                outs.append(open(out).read())
            self.assertEqual(outs[0], outs[1])
            bad = subprocess.run([sys.executable, tool, "--listing", lp, "--rom", rp, "--output", os.path.join(tmp, "z"), "--source-revision", REV,
                                  "--expect-rom-sha256", "1" * 64], capture_output=True, text=True)
            self.assertEqual(bad.returncode, 1)
            self.assertEqual(json.loads(bad.stdout)["error"], "rom_hash_mismatch")
            self.assertFalse(os.path.exists(os.path.join(tmp, "z")))


def a68(addr, field, src, trunc=False, macro=False):
    """One ASM68K 2.53 listing line: 8-hex address, 24-column byte field, '+' truncation flag, 'M' macro marker, source."""
    return f"{addr:08X} {field:<24}{'+' if trunc else ' '}{'M' if macro else ' '} {src}"


class Asm68kAndAdapters(unittest.TestCase):
    """SEG-048 (ADR 0099) adapters; synthetic project-authored listings only."""

    def test_as_bld89_addressless_continuation(self):
        rom = bytearray(32)
        rom[0:16] = bytes(range(0x11, 0x21))
        rom[16:18] = bytes.fromhex("4E75")
        lines = ["AS V1.42 Beta [Bld 89]", f"{1:>6}/{0:>8X} : {'1112 1314 1516 1718':<20}\tdc.l\ta,b,c,d", f"{'':<20}191A 1B1C 1D1E 1F20 ",
                 row(2, 16, "4E75", "\trts"), row(3, 18, "", "\tbinclude \"t.bin\"")]
        text, summary = ex.extract("\n".join(lines) + "\n", bytes(rom), REV, None)
        self.assertEqual(summary["instruction_starts"], 1)
        self.assertEqual(summary["data_bytes"], 16)

    def test_org_forward_padding_gap(self):
        rom = bytearray(b"\xff" * 16)
        rom[0:2] = bytes.fromhex("4E75")
        lines = [row(1, 0, "4E75", "\trts"), row(2, 2, "", "Pad:"), row(3, 14, "", "\torg\t$E"), row(4, 14, "FFFF", "\tdc.w $FFFF")]
        text, _ = ex.extract("\n".join(lines) + "\n", bytes(rom), REV, None)
        self.assertIn("entries 1", text)
        rom[5] = 0x12  # non-uniform fill: no longer plain padding
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract("\n".join(lines) + "\n", bytes(rom), REV, None)
        self.assertEqual(ctx.exception.code, "unexplained_rom_range")

    def test_z80_save_block_after_rebasing_org(self):
        rom = bytearray(32)
        rom[0:2] = bytes.fromhex("4E75")
        rom[2:6] = bytes([0xAF, 0xAF, 0xAF, 0xAF])  # Z80 block bytes live in the ROM gap
        rom[6:8] = bytes.fromhex("4E75")
        lines = [row(1, 0, "4E75", "\trts"), row(2, 2, "", "Z80Block:"), row(3, 0, "", "\t!org 0"), row(4, 0, "", "\tsave"),
                 row(5, 0, "", "\tCPU Z80"), row(6, 0, "AFAFAFAF", "\tdb 1,2,3,4"), row(7, 4, "", "\trestore"), row(8, 6, "", "\t!org 6"),
                 row(9, 6, "4E75", "\trts"), row(10, 8, "", "\tbinclude \"t\"")]
        text, _ = ex.extract("\n".join(lines) + "\n", bytes(rom), REV, None)
        self.assertIn("entries 2", text)

    def asm68k_fixture(self):
        rom = bytearray(0x40)
        code = {0x00: "4E71", 0x02: "6100", 0x04: "7200"}
        rom[0:2] = bytes.fromhex("4E71")
        rom[2:4] = bytes.fromhex("610C")   # bsr.s: displacement byte is a first-pass placeholder 00 in the listing
        rom[4:6] = bytes.fromhex("7212")   # moveq with a forward equate
        rom[6:22] = bytes(range(0x30, 0x40))  # 16-byte dc.b row: listing truncates it
        rom[22:24] = bytes.fromhex("4E75")
        rom[24:26] = bytes([0x05, 0x06])       # bytes emitted by a data-only macro, no listing row
        rom[26:28] = bytes.fromhex("4E71")
        lines = [
            a68(0, "", "dataOnly: macro"), a68(0, "", "    case narg"), a68(0, "", "=1  dc.b strlen(\\1)"), a68(0, "", "    endcase"), a68(0, "", "    endm"),
            a68(0, "4E71", "Start:\tnop"), a68(2, "6100", "\tbsr.s\tLater"), a68(4, "7200", "\tmoveq\t#FWD,d1"),
            a68(6, "3031 3233 3435 3637 3839", "\tdc.b 'xxxxxxxxxxxxxxxx'", trunc=True), a68(22, "4E75", "Later:rts"),
            a68(24, "", "\tdataOnly \"A\""), a68(26, "4E71", "\tnop"), a68(28, "", "\tincbin \"x.bin\""),
        ]
        return "\n".join(lines) + "\n", bytes(rom)

    def test_asm68k_listing(self):
        listing, rom = self.asm68k_fixture()
        text, summary = ex.extract(listing, rom, REV, None, dialect="asm68k")
        self.assertEqual(summary["instruction_starts"], 5)
        self.assertEqual(summary["data_bytes"], 18)
        self.assertIn("entries 5", text)

    def test_asm68k_forward_placeholder_is_limited_to_branch_and_moveq(self):
        listing, rom = self.asm68k_fixture()
        bad = bytearray(rom)
        bad[0] = 0x4E; bad[1] = 0x75  # a placeholder-looking difference on a non-branch opcode word
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract(listing, bytes(bad), REV, None, dialect="asm68k")
        self.assertEqual(ctx.exception.code, "instruction_opcode_differs_from_rom")
        bad = bytearray(rom)
        bad[2] = 0x70  # different operation in the high byte of a branch word
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract(listing, bytes(bad), REV, None, dialect="asm68k")
        self.assertEqual(ctx.exception.code, "instruction_opcode_differs_from_rom")

    def test_asm68k_unlisted_bytes_need_a_data_only_macro(self):
        listing, rom = self.asm68k_fixture()
        broken = listing.replace('=1  dc.b strlen(\\1)', "=1  move.b d0,d1")  # the macro body now contains an instruction
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract(broken, rom, REV, None, dialect="asm68k")
        self.assertEqual(ctx.exception.code, "unexplained_rom_range")

    def test_asm68k_truncated_instruction_and_unknown_dialect(self):
        lines = [a68(0, "4E71 4E71 4E71 4E71 4E71", "\tnop", trunc=True)]
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract("\n".join(lines) + "\n", bytes(16), REV, None, dialect="asm68k")
        self.assertEqual(ctx.exception.code, "instruction_truncated")
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract("x\n", bytes(16), REV, None, dialect="other")
        self.assertEqual(ctx.exception.code, "unknown_dialect")


    def test_asm68k_duplicate_echo_line_is_ignored_but_other_repeats_are_not(self):
        listing, rom = self.asm68k_fixture()
        lines = listing.splitlines()
        echo = lines.index(a68(22, "4E75", "Later:rts"))
        doubled = "\n".join(lines[:echo + 1] + [lines[echo]] + lines[echo + 1:]) + "\n"
        self.assertEqual(ex.extract(doubled, rom, REV, None, dialect="asm68k")[0], ex.extract(listing, rom, REV, None, dialect="asm68k")[0])
        other = "\n".join(lines[:echo + 1] + [a68(0, "", "; interleaved"), lines[echo]] + lines[echo + 1:]) + "\n"
        with self.assertRaises(ex.SourceMapError):
            ex.extract(other, rom, REV, None, dialect="asm68k")   # a non-adjacent repeat is a genuine overlap

    def test_placeholder_fill_is_opt_in_and_uniform_only(self):
        rom = bytearray(32)
        rom[0:2] = bytes.fromhex("4E75")
        rom[2:10] = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88])
        lines = [row(1, 0, "4E75", "\trts"), row(2, 2, "FFFFFFFFFFFFFFFF", "\tdc.b [8]$FF"), row(3, 10, "", "\tbinclude \"t\"")]
        listing = "\n".join(lines) + "\n"
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract(listing, bytes(rom), REV, None)
        self.assertEqual(ctx.exception.code, "data_differs_from_rom")
        text, summary = ex.extract(listing, bytes(rom), REV, None, placeholder_fill=True)
        self.assertEqual(summary["instruction_starts"], 1)
        mixed = "\n".join([lines[0], row(2, 2, "FFFFFFFFFFFFFF00", "\tdc.b 1,2"), lines[2]]) + "\n"
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract(mixed, bytes(rom), REV, None, placeholder_fill=True)   # not one uniform fill
        self.assertEqual(ctx.exception.code, "data_differs_from_rom")

    def test_max_entries_bound_is_explicit(self):
        listing, rom = self.asm68k_fixture()
        with self.assertRaises(ex.SourceMapError) as ctx:
            ex.extract(listing, rom, REV, None, dialect="asm68k", max_entries=2)
        self.assertEqual(ctx.exception.code, "universe_too_large")
        self.assertIn("entries 5", ex.extract(listing, rom, REV, None, dialect="asm68k", max_entries=5)[0])

    def test_deterministic(self):
        listing, rom = self.asm68k_fixture()
        self.assertEqual(ex.extract(listing, rom, REV, None, dialect="asm68k"), ex.extract(listing, rom, REV, None, dialect="asm68k"))


if __name__ == "__main__":
    unittest.main()
