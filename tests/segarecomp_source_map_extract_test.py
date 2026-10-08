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


if __name__ == "__main__":
    unittest.main()
