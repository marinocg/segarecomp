#!/usr/bin/env python3
"""Project-authored synthetic BIOS-like image with RAM jump-slot tables for the SEG-079-T002 mechanism proof (ADR 0180). Stdlib only.

Every byte is written here with the fixture encoder of tools/sega_cd_fixture_disc.py; nothing is read from or modelled on a commercial image.
The main program builds two tables of `JMP (xxx).L` slots by stores in work RAM (6-byte stride), then calls every slot with `JSR abs.L`:

  table A  - 24 contiguous slots at $FFD000, slot k jumps to precompiled routine k, which stores k + 1 at result word k;
  table B  - slot 0 and 1 `JMP (xxx).L`, slot 2 `JMP (xxx).W` padded with a NOP (a different opcode: it ends a run), slot 3 `JMP (xxx).L`.

Negatives (main d4, applied after the tables are built): 1 slot A3's opcode rewritten to a JSR (template shape violated), 2 slot A2's operand
pointed past the image (no compiled entry), 3 slot A5's operand odd, 4 (positive variant) slot A7's operand changed to another compiled routine.
Result block (main work RAM $FFC000; DONE = $A5 at $FFC0FF): +2k = k + 1 (k < 24), +$40 + 2j = $B0 + j (table B).
usage: sega_cd_slot_table_fixture.py build --out DIR   ->   DIR/bios.bin and DIR/expected.json (stdout: the expected JSON)
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sega_cd_fixture_disc as F  # noqa: E402

ROM_SIZE = 0x1000
MAIN = 0x0200
MAIN_FAULT = 0x0900
DESTS = 0x0A00            # routine i at DESTS + 0x10 * i
RESULT = 0xFFC000
SLOT_A = 0xFFD000
SLOT_B = 0xFFD200
A_SLOTS = 24
UNCOMPILED = ROM_SIZE + 0x100  # past the image: no compiled entry


def dest(i):
    return DESTS + 0x10 * i


def routine(base, offset, value):
    r = F.Asm(base)
    r.move("w", F.imm(value), F.absl(RESULT + offset))
    r.rts()
    return r.assemble()


def main_program():
    m = F.Asm(MAIN)
    for k in range(A_SLOTS):
        m.move("w", F.imm(0x4EF9), F.absl(SLOT_A + 6 * k))
        m.move("l", F.imm(dest(k)), F.absl(SLOT_A + 6 * k + 2))
    for j, (opcode, size) in enumerate(((0x4EF9, "l"), (0x4EF9, "l"), (0x4EF8, "w"), (0x4EF9, "l"))):
        base = SLOT_B + 6 * j
        m.move("w", F.imm(opcode), F.absl(base))
        if size == "l":
            m.move("l", F.imm(dest(A_SLOTS + j)), F.absl(base + 2))
        else:
            m.move("w", F.imm(dest(A_SLOTS + j)), F.absl(base + 2))
            m.move("w", F.imm(0x4E71), F.absl(base + 4))      # NOP padding to the 6-byte stride
    for case, (address, value, size) in ((1, (SLOT_A + 6 * 3, 0x4EB9, "w")), (2, (SLOT_A + 6 * 2 + 2, UNCOMPILED, "l")),
                                         (3, (SLOT_A + 6 * 5 + 2, dest(5) + 1, "l")), (4, (SLOT_A + 6 * 7 + 2, dest(9), "l"))):
        m.immop("cmp", "w", case, F.d(4))
        m.bcc("ne", "n%d" % case)
        m.move(size, F.imm(value), F.absl(address))
        m.label("n%d" % case)
    for k in range(A_SLOTS):
        m.jsr(F.absl(SLOT_A + 6 * k))
    for j in range(4):
        m.jsr(F.absl(SLOT_B + 6 * j))
    m.move("b", F.imm(0xA5), F.absl(RESULT + 0xFF))
    m.stop(0x2700)
    code = m.assemble()
    assert MAIN + len(code) <= MAIN_FAULT, hex(MAIN + len(code))
    return code


def build_rom():
    rom = bytearray(ROM_SIZE)
    struct.pack_into(">II", rom, 0, 0x00FFFE00, MAIN)
    for v in range(2, 64):
        struct.pack_into(">I", rom, 4 * v, MAIN_FAULT)
    rom[0x100:0x120] = b"SEGA BIOS-IMAGE FIXTURE SEG079  "
    main = main_program()
    rom[MAIN:MAIN + len(main)] = main
    fault = F.Asm(MAIN_FAULT)
    fault.move("w", F.imm(0xDEAD), F.absl(RESULT + 0xF0))
    fault.stop(0x2700)
    code = fault.assemble()
    rom[MAIN_FAULT:MAIN_FAULT + len(code)] = code
    for k in range(A_SLOTS):
        code = routine(dest(k), 2 * k, k + 1)
        rom[dest(k):dest(k) + len(code)] = code
    for j in range(4):
        code = routine(dest(A_SLOTS + j), 0x40 + 2 * j, 0xB0 + j)
        rom[dest(A_SLOTS + j):dest(A_SLOTS + j) + len(code)] = code
    return bytes(rom)


def expected(rom):
    result = {str(2 * k): k + 1 for k in range(A_SLOTS)}
    result.update({str(0x40 + 2 * j): 0xB0 + j for j in range(4)})
    return {
        "rom_size": len(rom), "rom_sha256": hashlib.sha256(rom).hexdigest(), "result": result,
        "slots": {"a_base": SLOT_A, "a_count": A_SLOTS, "b_base": SLOT_B},
        "variant": {"main:d4=4": {"14": 0, "18": 10}},  # slot A7 now reaches routine 9: word 7 is never written
        "negatives": {
            "main:d4=1": ["code_bytes_mismatch", 0, SLOT_A + 6 * 3],
            "main:d4=2": ["entry_not_compiled", 0, SLOT_A + 6 * 2],
            "main:d4=3": ["entry_not_compiled", 0, SLOT_A + 6 * 5],
        },
    }


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["build"])
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    rom = build_rom()
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "bios.bin").write_bytes(rom)
    exp = expected(rom)
    (args.out / "expected.json").write_text(json.dumps(exp, indent=1, sort_keys=True) + "\n")
    print(json.dumps(exp, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
