#!/usr/bin/env python3
"""Project-authored BIOS-like image for the SEG-078 live (interactive) viewer tests (ADR 0204). Stdlib only.

Every byte is written here with the fixture encoder of tools/sega_cd_fixture_disc.py; no BIOS or commercial byte is read or reproduced.
The main program is a minimal game loop driven by the vertical interrupt, so that live pad input is visible in the picture:

  CTRL1 <- $40; VDP: Mode 5, display on, 256 x 224, auto-increment 2; the vertical interrupt is enabled (register #1 IE0).
  Level 6 handler (every frame): reads port 1 with TH high and TH low, stores both raw bytes (RESULT + $10, + $11), derives a 9-bit colour
  from the pressed lines and writes it to CRAM entry 0 (the backdrop), then counts the frame at RESULT + $20.
      colour = ((p & 7) << 1) | ((p & $38) << 2) | ((q & $30) << 5)     p = ~TH-high byte & $3F (U D L R B C), q = ~TH-low byte & $30 (A Start)
  The main loop waits until the frame counter reaches d6 (`--set main:d6=N`; absent or 0: 65,535 frames, a session that ends when the viewer
  closes), then disables the interrupt, writes $A5 to RESULT + $FF and stops (the run ends idle).

The picture is therefore a flat backdrop whose colour is a function of the pad as the handler last saw it: every distinct pad state is a
distinct frame digest, and a missed or late input is visible as a different digest in the headless replay.

usage: sega_cd_bios_live_fixture.py build --out DIR   ->   DIR/bios.bin and DIR/expected.json (stdout: the expected JSON)
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
HANDLER = 0x0600
FAULT = 0x0C00
RESULT = 0xFFC000
RAW_HIGH = RESULT + 0x10
RAW_LOW = RESULT + 0x11
FRAMES = RESULT + 0x20
DATA1 = 0xA10003
VDP_DATA = 0xC00000
VDP_CTRL = 0xC00004
DEFAULT_FRAMES = 0xFFFF


def vdp_reg(m, reg, value):
    m.move("w", F.imm(0x8000 | (reg << 8) | value), F.absl(VDP_CTRL))


def main_program():
    m = F.Asm(MAIN)
    m.move("b", F.imm(0x40), F.absl(0xA10009))            # CTRL1: TH output
    vdp_reg(m, 0, 0x04)                                   # register #0: bit 2 (the palette-select mode off)
    vdp_reg(m, 1, 0x44)                                   # display on, Mode 5, no interrupt yet
    vdp_reg(m, 12, 0x00)                                  # H32, no shadow/highlight, no interlace
    vdp_reg(m, 15, 0x02)                                  # auto-increment 2
    m.clr("w", F.absl(FRAMES))
    m.tst("w", F.d(6))
    m.bcc("ne", "have_count")
    m.move("w", F.imm(DEFAULT_FRAMES), F.d(6))
    m.label("have_count")
    vdp_reg(m, 1, 0x64)                                   # IE0: the vertical interrupt
    m.move_sr(0x2000)
    m.label("frames")
    m.move("w", F.absl(FRAMES), F.d(1))
    m.alu("cmp", "w", F.d(6), 1)                          # d1 - d6
    m.bcc("cs", "frames")                                 # d1 < d6 (unsigned): keep waiting
    m.move_sr(0x2700)
    vdp_reg(m, 1, 0x44)
    m.move("b", F.imm(0xA5), F.absl(RESULT + 0xFF))
    m.stop(0x2700)
    code = m.assemble()
    assert MAIN + len(code) <= HANDLER, hex(MAIN + len(code))
    return code


def handler_program():
    h = F.Asm(HANDLER)
    h.moveq(0, 0)
    h.moveq(0, 2)
    h.move("b", F.imm(0x40), F.absl(DATA1))               # TH high: 1 1 C B R L D U
    h.move("b", F.absl(DATA1), F.d(0))
    h.move("b", F.imm(0x00), F.absl(DATA1))               # TH low: 0 0 S A 0 0 D U
    h.move("b", F.absl(DATA1), F.d(2))
    h.move("b", F.d(0), F.absl(RAW_HIGH))
    h.move("b", F.d(2), F.absl(RAW_LOW))
    h.immop("eor", "w", 0x3F, F.d(0))                     # p: pressed = 1 (U D L R B C = bits 0..5)
    h.immop("and", "w", 0x30, F.d(2))                     # TH low: only A (bit 4) and Start (bit 5)
    h.immop("eor", "w", 0x30, F.d(2))                     # q: pressed = 1
    h.move("w", F.d(0), F.d(3))
    h.immop("and", "w", 0x07, F.d(3))
    h.lsl_w(1, 3)                                         # red: bits 1..3
    h.move("w", F.d(0), F.d(4))
    h.immop("and", "w", 0x38, F.d(4))
    h.lsl_w(2, 4)                                         # green: bits 5..7
    h.alu("or", "w", F.d(4), 3)
    h.move("w", F.d(2), F.d(4))
    h.lsl_w(5, 4)                                         # blue: bits 9..10
    h.alu("or", "w", F.d(4), 3)
    h.move("l", F.imm(0xC0000000), F.absl(VDP_CTRL))      # CRAM write, address 0
    h.move("w", F.d(3), F.absl(VDP_DATA))
    h.quick("add", "w", 1, F.absl(FRAMES))
    h.rte()
    code = h.assemble()
    assert HANDLER + len(code) <= FAULT, hex(HANDLER + len(code))
    return code


def build():
    rom = bytearray(ROM_SIZE)
    struct.pack_into(">II", rom, 0, 0x00FFFE00, MAIN)
    for v in range(2, 64):
        struct.pack_into(">I", rom, 4 * v, FAULT)
    struct.pack_into(">I", rom, 30 * 4, HANDLER)          # level 6
    rom[0x100:0x120] = b"SEGA BIOS-IMAGE LIVE FIXTURE 078 "[:32]
    main = main_program()
    rom[MAIN:MAIN + len(main)] = main
    handler = handler_program()
    rom[HANDLER:HANDLER + len(handler)] = handler
    fault = F.Asm(FAULT)
    fault.move("w", F.imm(0xDEAD), F.absl(RESULT + 0xF0))
    fault.stop(0x2700)
    rom[FAULT:FAULT + len(fault.assemble())] = fault.assemble()
    return bytes(rom)


def color(high, low):
    """The backdrop word the handler writes for the raw TH-high and TH-low bytes (hand-derived from the formula in the docstring)."""
    p = (~high) & 0x3F
    q = (~low) & 0x30
    return ((p & 7) << 1) | ((p & 0x38) << 2) | (q << 5)


def expected(rom):
    return {
        "rom_size": len(rom), "rom_sha256": hashlib.sha256(rom).hexdigest(),
        "released": {"high": 0x7F, "low": 0x33, "color": color(0x7F, 0x33)},
        "result": {"raw_high": 0x10, "raw_low": 0x11, "frames": 0x20, "end_marker": 0xFF},
        "colors": {name: color(high, low) for name, (high, low) in {
            "RIGHT": (0x77, 0x33), "UP": (0x7E, 0x33), "START": (0x7F, 0x13), "A": (0x7F, 0x23)}.items()},
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
