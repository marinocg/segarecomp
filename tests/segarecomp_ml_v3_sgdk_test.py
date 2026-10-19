#!/usr/bin/env python3
"""SEG-072 (ADR 0152): hermetic tests for the SGDK homebrew truth tooling and the private research registry helpers.
Synthetic project-authored inputs only (no toolchain, ROM or commercial data)."""
import json
import os
import pathlib
import sys
import tempfile
import unittest

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
sys.path.insert(0, TOOLS)
import segarecomp_ml_v3_data as data  # noqa: E402
import segarecomp_ml_v3_sgdk as sg  # noqa: E402

MAKEFILE = """release: FLAGS= -O3 -flto
$(OUT)/rom.out: $(OUT)/sega.o $(OUT)/cmd_ $(LIBMD)
\t$(MKDIR) -p $(dir $@)
\t$(CC) -m68000 -n -T $(GDK)/md.ld -nostdlib $(OUT)/sega.o @$(OUT)/cmd_ $(LIBMD) $(LIBGCC) -o $(OUT)/rom.out -Wl,--gc-sections -flto
\t$(RM) $(OUT)/cmd_
$(OUT)/sega.o: $(SRC)/boot/sega.s
\t$(CC) -x assembler-with-cpp $(AFLAGS) -c $(SRC)/boot/sega.s -o $@
$(OUT)/%.o: %.c
\t$(MKDIR) -p $(dir $@)
\t$(CC) $(CFLAGS) -MMD -c $< -o $@
"""


class Makefile(unittest.TestCase):
    def test_patch_adds_only_debug_temps_and_map(self):
        text, n_link, n_lst = sg.patch_makefile(MAKEFILE)
        self.assertEqual((n_link, n_lst), (1, 2))
        self.assertIn("-o $(OUT)/rom.out -Wl,--gc-sections -flto -g -save-temps -Wl,-Map=out/rom.map", text)
        self.assertIn("-c $< -o $@ -g -save-temps=obj", text)
        self.assertNotIn("MKDIR) -p $(dir $@) -g", text)
        old, _, _ = sg.patch_makefile(MAKEFILE, " -g -save-temps")
        self.assertIn("-c $< -o $@ -g -save-temps\n", old + "\n")


class Assembly(unittest.TestCase):
    SRC = "\n".join([
        "\t.file 1 \"src/main.c\"", "\t.file 2 \"D:/apps/SGDK/src/vdp.c\"", "\t.text", "\t.type main, @function", "main:", "\t.loc 1 10 0",
        "\tmove.w #1,%d0", "\tjra .L2", "\t.swbeg &2", ".L3:", "\t.word .L2-.L3", "\t.type VDP_x, @function", "VDP_x:", "\t.loc 2 5 0",
        "1:\trts", "\t.size VDP_x, .-VDP_x", "\t.macro M a", "\tnop", "\t.endm", "\tM 1", "/* \t.word 9 */",
    ])

    def test_instrument_rows_kinds_and_owner_files(self):
        text, rows = sg.instrument_source(self.SRC)
        kinds = [r["kind"] for r in rows]
        self.assertEqual(kinds, ["I", "I", "D", "D", "I", "M"])
        self.assertEqual([r["file"] for r in rows[:5]], ["src/main.c"] * 4 + ["D:/apps/SGDK/src/vdp.c"])
        self.assertEqual(text.count(sg.MARK), 6)
        self.assertNotIn("__v3m_6", text)  # the commented-out statement and the macro body get no marker
        self.assertNotIn("/*", text)

    def test_unknown_statement_fails_closed(self):
        with self.assertRaises(sg.TruthError):
            sg.instrument_source("\tfrobnicate %d0")

    def test_strip_comments_keeps_lines(self):
        t = "a /* x\ny */ b \"/*s*/\"\nc"
        out = sg.strip_c_comments(t)
        self.assertEqual(out.count("\n"), 2)
        self.assertIn("\"/*s*/\"", out)
        self.assertNotIn("x", out.split("\n")[0])

    def test_owners_and_suffixes(self):
        self.assertEqual(sg.file_owner("src/main.c"), "app")
        self.assertEqual(sg.file_owner("/src/src/main.c"), "app")
        self.assertEqual(sg.file_owner("D:/apps/SGDK/src/vdp.c"), "sdk")
        self.assertEqual(sg.file_owner("/sgdk/src/vdp.c"), "sdk")
        self.assertEqual(sg.file_owner(None), "ambiguous")
        self.assertEqual(sg.base_name("VDP_drawText.constprop.55"), "VDP_drawText")
        self.assertEqual(sg.base_name("f.lto_priv.3.part.0"), "f")
        self.assertEqual(sg.origin_of_file("Z:/x/sgdk-trees/v1.62/lib/libmd.a(vdp.o)"), "sdk")
        self.assertEqual(sg.origin_of_file("out/rom.out.ltrans3.ltrans.o"), "ltrans")
        self.assertEqual(sg.origin_of_file("out/res/sprite.o"), "resource")
        self.assertEqual(sg.origin_of_file("out/src/main.o"), "app_object")


MAP = """Archive member included
Linker script and memory map

LOAD out/sega.o
.text           0x00000000       0x10
 .text.keepboot
                0x00000000        0x4 out/sega.o
 .text          0x00000004        0x8 rom.out.ltrans0.ltrans.o
                0x00000004                main
 *fill*         0x0000000c        0x4
.data           0x00ff0000        0x0 load address 0x00000010
.debug_info     0x00000000       0x20
 .debug_info    0x00000000       0x20 out/sega.o
"""
DUMP = """
x.o:     file format elf32-m68k

Sections:
Idx Name          Size      VMA       LMA       File off  Algn
  0 .text         00000008  00000000  00000000  00000034  2**1

RELOCATION RECORDS FOR [.text]:
OFFSET   TYPE              VALUE
00000006 R_68K_16          sym

Contents of section .text:
 0000 4e714e75 12340000                    NqNu.4..
"""
SYM = """
SYMBOL TABLE:
00000000 l       .text\t00000000 __v3m_0
00000002 l       .text\t00000000 __v3m_1
00000004 l       .text\t00000000 __v3m_2
"""


class Truth(unittest.TestCase):
    def make(self, tmp, rom, rows):
        w = pathlib.Path(tmp)
        (w / "inst" / "out").mkdir(parents=True)
        (w / "inst" / ".v3" / "relist").mkdir(parents=True)
        (w / "inst" / "out" / "rom.bin").write_bytes(rom)
        (w / "inst" / "out" / "rom.map").write_text(MAP)
        (w / "inst" / ".v3" / "relist" / "u0.o.dump").write_text(DUMP)
        (w / "inst" / ".v3" / "relist" / "u0.m.o.dump").write_text(DUMP)
        (w / "inst" / ".v3" / "relist" / "u0.m.o.sym").write_text(SYM)
        (w / "inst" / ".v3" / "relist" / "u0.rows.json").write_text(json.dumps(rows))
        (w / "units.json").write_text(json.dumps([{"kind": "ltrans", "src": "x.s", "map_file_suffix": "rom.out.ltrans0.ltrans.o", "id": 0,
                                                   "rows": ".v3/relist/u0.rows.json", "as_flags": ["-m68000"]}]))
        return w

    ROWS = [{"kind": "I", "sym": "main", "symtype": "function", "file": "src/main.c"},
            {"kind": "I", "sym": "main", "symtype": "function", "file": "src/main.c"},
            {"kind": "D", "sym": "main", "symtype": "function", "file": "src/main.c"}]

    def test_end_to_end_classes(self):
        rom = bytes.fromhex("aabbccdd" + "4e714e75" + "1234beef" + "00000000")
        with tempfile.TemporaryDirectory() as tmp:
            cls, report = sg.build_truth(self.make(tmp, rom, self.ROWS))
        self.assertEqual(cls, b"UUUU" + b"IcIc" + b"DDDD" + b"DDDD")
        self.assertEqual(report["relocated_bytes"], 2)
        self.assertEqual(report["starts"], 2)

    def test_sdk_function_is_masked(self):
        rows = [dict(r, file="D:/apps/SGDK/src/vdp.c") for r in self.ROWS]
        rom = bytes.fromhex("aabbccdd" + "4e714e75" + "1234beef" + "00000000")
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(sg.TruthError):  # no application instruction left
                sg.build_truth(self.make(tmp, rom, rows))

    def test_unrelocated_byte_mismatch_fails(self):
        rom = bytes.fromhex("aabbccdd" + "4e714e74" + "1234beef" + "00000000")
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(sg.TruthError) as ctx:
                sg.build_truth(self.make(tmp, rom, self.ROWS))
        self.assertEqual(str(ctx.exception), "object_byte_differs_from_rom")

    def test_parsers(self):
        entries = sg.parse_map(MAP)
        files = [(e["file"], e["addr"], e["size"]) for e in entries if e["out"] == ".text"]
        self.assertEqual(files, [("out/sega.o", 0, 4), ("rom.out.ltrans0.ltrans.o", 4, 8), ("*fill*", 12, 4)])
        self.assertEqual(sg.objdump_contents(DUMP), {".text": bytes.fromhex("4e714e7512340000")})
        rel, sizes = sg.parse_relocs(DUMP)
        self.assertEqual((rel[".text"], sizes[".text"]), ({6, 7}, 8))
        self.assertEqual(sg.objdump_symbols(SYM), {0: (".text", 0), 1: (".text", 2), 2: (".text", 4)})


class Registry(unittest.TestCase):
    def test_decoder_audit(self):
        nf = len(data.TOKEN_FIELDS)
        tokens = bytearray(4 * nf)
        tokens[0:2] = bytes([2, 1])           # word 0: ordinary, 1 word
        tokens[nf:nf + 2] = bytes([2, 2])     # word 1: ordinary, 2 words
        self.assertEqual(data.audit(b"IcIcccDD", bytes(tokens)), {"starts": 2, "odd_starts": 0, "not_ordinary_decode": 0, "length_mismatch": 0})
        self.assertEqual(data.audit(b"IcIcDDDD", bytes(tokens))["length_mismatch"], 1)

    def test_shingles_only_code_runs(self):
        img = bytes(range(40))
        self.assertEqual(len(data.shingles(img, b"I" + b"c" * 13 + b"D" * 26)), 3)
        self.assertEqual(len(data.shingles(img, None)), 29)


if __name__ == "__main__":
    unittest.main()
