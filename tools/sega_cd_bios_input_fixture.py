#!/usr/bin/env python3
"""Project-authored BIOS-like image for the SEG-070-T002 timed standard-pad machine input (ADR 0149). Stdlib only.

Every byte is written here with the fixture encoder of tools/sega_cd_fixture_disc.py; no BIOS or commercial byte is read or reproduced.
The main program drives the controller registers with ordinary MC68000 instructions, exactly as a game's pad routine does:

  CTRL1 and CTRL2 <- $40 (TH is an output, the six pad lines inputs)
  32 iterations; each one stores four bytes at RESULT + 4 * i:
    +0 DATA1 after writing $00 (port 1, TH low)     +1 DATA1 after writing $40 (port 1, TH high)
    +2 DATA2 after writing $00 (port 2, TH low)     +3 DATA2 after writing $40 (port 2, TH high)
  and then waits a fixed delay loop; $A5 at RESULT + $FF marks the end, then STOP #$2700 (the run ends idle).

Negatives (main d7, `--set main:d7=N`): 1 a BYTE read of $A10002 (the even lane of the data register: not a modelled shape); 2 a BYTE
write of the version register $A10001; 3 a LONG read of $A10010 (the unmodelled block above the pad ports).

usage: sega_cd_bios_input_fixture.py build --out DIR   ->   DIR/bios.bin and DIR/expected.json (stdout: the expected JSON)
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sega_cd_fixture_disc as F  # noqa: E402  (the shared project-authored MC68000 encoder)

ROM_SIZE = 0x1000
MAIN = 0x0200
FAULT = 0x0C00
RESULT = 0xFFC000
ITERATIONS = 32
DELAY = 40          # dbra iterations between polls (10 cycles each while it loops)
NEG_RANGES = {}


def negative(m, case, body):
    m.immop("cmp", "w", case, F.d(7))
    m.bcc("ne", "neg%d" % case)
    start = m.pc
    body(m)
    NEG_RANGES[case] = [start, m.pc]
    m.label("neg%d" % case)


def main_program():
    m = F.Asm(MAIN)
    m.move("b", F.imm(0x40), F.absl(0xA10009))            # CTRL1: TH output
    m.move("b", F.imm(0x40), F.absl(0xA1000B))            # CTRL2: TH output
    m.move("l", F.imm(RESULT), F.a(0))
    m.move("w", F.imm(ITERATIONS - 1), F.d(0))
    m.label("poll")
    for data in (0xA10003, 0xA10005):
        m.move("b", F.imm(0x00), F.absl(data))            # TH low
        m.move("b", F.absl(data), ("inc", 0))
        m.move("b", F.imm(0x40), F.absl(data))            # TH high
        m.move("b", F.absl(data), ("inc", 0))
    m.move("w", F.imm(DELAY), F.d(1))
    m.label("wait")
    m.dbra(1, "wait")
    m.dbra(0, "poll")
    m.move("b", F.imm(0xA5), F.absl(RESULT + 0xFF))
    negative(m, 1, lambda n: n.move("b", F.absl(0xA10002), F.d(2)))
    negative(m, 2, lambda n: n.move("b", F.imm(0x00), F.absl(0xA10001)))
    negative(m, 3, lambda n: n.move("l", F.absl(0xA10010), F.d(2)))
    m.stop(0x2700)
    code = m.assemble()
    assert MAIN + len(code) <= FAULT, hex(MAIN + len(code))
    return code


def build():
    rom = bytearray(ROM_SIZE)
    struct.pack_into(">II", rom, 0, 0x00FFFE00, MAIN)
    for v in range(2, 64):
        struct.pack_into(">I", rom, 4 * v, FAULT)
    rom[0x100:0x120] = b"SEGA BIOS-IMAGE INPUT FIXTURE T2 "[:32]
    main = main_program()
    rom[MAIN:MAIN + len(main)] = main
    fault = F.Asm(FAULT)
    fault.move("w", F.imm(0xDEAD), F.absl(RESULT + 0xF0))
    fault.stop(0x2700)
    rom[FAULT:FAULT + len(fault.assemble())] = fault.assemble()
    return bytes(rom)


def expected(rom):
    return {
        "rom_size": len(rom), "rom_sha256": hashlib.sha256(rom).hexdigest(),
        "iterations": ITERATIONS, "reads_per_iteration": 4,
        # nothing pressed: TH low $33 (Start, A released; bits 3-2 low; Down, Up released), TH high $7F, on both ports
        "released_pattern": [0x33, 0x7F, 0x33, 0x7F],
        "negatives": {
            "main:d7=1": {"range": NEG_RANGES[1], "address": 0xA10002, "width": 1, "write": 0},
            "main:d7=2": {"range": NEG_RANGES[2], "address": 0xA10001, "width": 1, "write": 1},
            "main:d7=3": {"range": NEG_RANGES[3], "address": 0xA10011, "width": 1, "write": 0},  # a long is read as words, a word as its odd byte
        },
    }


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["build"])
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    rom = build()
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "bios.bin").write_bytes(rom)
    exp = expected(rom)
    (args.out / "expected.json").write_text(json.dumps(exp, indent=1, sort_keys=True) + "\n")
    print(json.dumps(exp, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
