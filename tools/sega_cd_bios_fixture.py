#!/usr/bin/env python3
"""Project-authored 3.5 KiB synthetic BIOS-like image for the SEG-064-T002 mechanism proof (ADR 0142). Stdlib only.

Every byte is written here with the fixture encoder of tools/sega_cd_fixture_disc.py; no BIOS, security code or commercial byte is
read or reproduced. The image is a "BIOS image" only in layout (vectors, a "SEGA" system header at $100, main code at $200); it
exercises the four mechanisms of the BIOS-image profile:

  A  broad AOT of an immutable image: the main program reaches a routine only through a data table (JSR (A1)); data is interleaved
     with code; negative transfers (main d6 = 1..4): an odd target, an address past the image, an undecodable word, and the sub's code
     through main's PRG-RAM window (the wrong processor / memory view);
  B  a transformed RAM image: main writes the sub program into PRG RAM through the window, each word XORed with a key (not a verbatim
     copy), then releases the sub (SRES 1, SBRQ 0: the runnable epoch); variants (main d5): 3 arms the reset with the bus requested
     and starts the sub by the SBRQ release, 4 later resets it, writes the image again and releases it (two epochs); negatives (main d5): 1 releases after
     half the copy (incomplete), 2 corrupts one code word after the copy (wrong materialization);
  C  resident and transient ranges: the sub image holds resident code, mutable data the sub keeps writing, and a transient routine T1
     at $0480; main later halts the sub (SBRQ) and copies T2 - a verbatim ROM span, an independently admitted copy image - over it;
     T1 and T2 share their first instruction (one deduplicated variant) and differ after it (two variants selected by live bytes);
     negatives (sub d3): 1 the sub rewrites one of its own resident instructions (code mutation without an image); 2 a stale return:
     the sub waits inside T1 while main replaces it and then returns into a range that is a mixture of T2 and T1 bytes;
  D  mutable jump targets: case 1 an immutable `MOVEA.L (abs).L,A2; JSR (A2)` reading a RAM destination; case 2 RAM thunks
     `JMP (xxx).L` built by stores (main in work RAM before the epoch, sub in PRG RAM after it) whose operand is rewritten between two
     precompiled destinations; negatives (main d4): 1 an odd destination, 2 an uncompiled destination, 3 the thunk opcode rewritten to
     a JSR (template shape violated), 4 case 1 with an odd destination.

Result block (main work RAM $FFC000, 256 bytes; DONE = $A5 at $FFC0FF):
  +$00 $1111 hidden routine ran        +$02 $D001 thunk destination 1   +$04 $D002 thunk destination 2
  +$06 count of destination-1 calls (2: thunk + case 1)                 +$20.. COMSTAT0..2 as the sub left them ($7202, $5D01, $5D02)
  +$30 the sub's data counter (PRG $0802: initial $AAAA, four increments), reported through COMSTAT4
usage: sega_cd_bios_fixture.py build --out DIR   ->   DIR/bios.bin and DIR/expected.json (stdout: the expected JSON)
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sega_cd_fixture_disc as F  # noqa: E402  (the shared project-authored MC68000 encoder)

ROM_SIZE = 0x0E00
MAIN = 0x0200
MAIN_FAULT = 0x0420
HIDDEN = 0x0440
DEST1 = 0x0460
DEST2 = 0x0480
TABLE = 0x04A0
SUB_SRC = 0x0500          # encoded sub image in ROM
T2_SRC = 0x0C00           # T2, a verbatim span copied to PRG $0480 (T1)
UNDECODABLE = 0x0D00
KEY = 0x5A5A
RESULT = 0xFFC000
THUNK_MAIN = 0xFFFD00
CASE1_SLOT = 0xFFC100
GA = 0xA12000
WINDOW = 0x020000

SUB_SSP = 0x5000
SUB_RESIDENT = 0x0100
SUB_FAULT = 0x0240
SUB_WAIT = 0x0260
SDEST1 = 0x0200
SDEST2 = 0x0220
SUB_DATA = 0x0300
THUNK_SUB = 0x0400
T1 = 0x0480
T1_STALE = 0x04C0
T2_LEN = 0x48             # T2 covers T1..T1+$47: its last instruction would cross T1+$48, so a return to T1_STALE+6 meets mixed bytes
GA_SUB = 0xFF8000


def absw(addr):
    return ("absw", addr & 0xFFFF)


def jmp_an(asm, n):
    asm.w(0x4EC0 | (2 << 3) | n)


def sub_image():
    """The sub program (PRG RAM image at base 0), before the transform."""
    s = F.Asm(0)
    s.lw(SUB_SSP)
    s.lw(SUB_RESIDENT)
    while s.pc < SUB_RESIDENT:
        s.lw(SUB_FAULT)
    # resident code
    assert s.pc == SUB_RESIDENT
    s.move("w", F.imm(0x0001), F.absl(SUB_DATA))          # mutable data next to code
    s.quick("add", "w", 1, F.absl(SUB_DATA + 2))
    s.jsr(F.absl(T1))                                     # transient T1 (writes COMSTAT0 = $7101)
    s.quick("add", "w", 1, F.absl(SUB_DATA + 2))
    # case 2 on the sub: a JMP (xxx).L thunk built after the epoch, operand rewritten between two compiled destinations
    s.move("w", F.imm(0x4EF9), F.absl(THUNK_SUB))
    s.move("l", F.imm(SDEST1), F.absl(THUNK_SUB + 2))
    s.jsr(F.absl(THUNK_SUB))
    s.move("l", F.imm(SDEST2), F.absl(THUNK_SUB + 2))
    s.jsr(F.absl(THUNK_SUB))
    s.quick("add", "w", 1, F.absl(SUB_DATA + 2))
    s.immop("cmp", "w", 1, F.d(3))
    s.bcc("ne", "no_mutation")
    s.move("w", F.imm(0x4E71), F.absl(0))                 # patched below: rewrites a later resident instruction
    mutation_fixup = len(s.words) - 2
    s.label("no_mutation")
    s.immop("cmp", "w", 2, F.d(3))
    s.bcc("ne", "normal_wait")
    s.jsr(F.absl(T1_STALE))                               # waits inside T1 while main replaces it, then returns into mixed bytes
    s.bra("after_wait")
    s.label("normal_wait")
    s.jsr(F.absl(SUB_WAIT))
    s.label("after_wait")
    s.label("mutated")                                    # the instruction d3 = 1 overwrites (a 6-byte MOVE.W #,abs.W)
    s.move("w", F.imm(0x1234), absw(SUB_DATA + 4))
    s.jsr(F.absl(T1))                                     # now T2 (writes COMSTAT0 = $7202)
    s.quick("add", "w", 1, F.absl(SUB_DATA + 2))
    s.move("w", F.absl(SUB_DATA + 2), F.absl(GA_SUB + 0x28))  # COMSTAT4: the data counter
    s.move("b", F.imm(0x02), F.absl(GA_SUB + 0x0F))       # sub flag 2: done
    s.stop(0x2700)
    s.label("pad1")
    while s.pc < SDEST1:
        s.nop()
    s.move("w", F.imm(0x5D01), F.absl(GA_SUB + 0x22))
    s.rts()
    while s.pc < SDEST2:
        s.nop()
    s.move("w", F.imm(0x5D02), F.absl(GA_SUB + 0x24))
    s.rts()
    while s.pc < SUB_FAULT:
        s.nop()
    s.move("w", F.imm(0xDEAD), F.absl(GA_SUB + 0x2E))
    s.stop(0x2700)
    while s.pc < SUB_WAIT:
        s.nop()
    s.move("b", F.imm(0x01), F.absl(GA_SUB + 0x0F))       # sub flag 1: ready for the replacement
    s.label("wait2")
    s.immop("cmp", "w", 2, F.absl(GA_SUB + 0x10))         # COMCMD0 = 2: T2 installed
    s.bcc("ne", "wait2")
    s.rts()
    while s.pc < SUB_DATA:
        s.nop()
    for _ in range(0x40):                                 # mutable data, initially non-zero (part of the materialized image)
        s.w(0xAAAA)
    while s.pc < T1:
        s.w(0)
    # T1 (transient)
    s.nop()
    s.move("w", F.imm(0x7101), F.absl(GA_SUB + 0x20))
    s.rts()
    while s.pc < T1_STALE:
        s.nop()
    s.jsr(F.absl(SUB_WAIT))
    assert s.pc == T1_STALE + 6
    s.move("w", F.imm(0x7E57), F.absl(GA_SUB + 0x26))     # never reached after the replacement
    s.rts()
    words = s.assemble()
    image = bytearray(words)
    target = s.labels["mutated"]
    struct.pack_into(">I", image, 2 * mutation_fixup, target)  # MOVE.W #$4E71,(target).L
    return bytes(image)


def t2_image():
    t = F.Asm(T1)
    t.nop()                                               # identical to T1's first instruction (one deduplicated variant)
    t.move("w", F.imm(0x7202), F.absl(GA_SUB + 0x20))     # same shape as T1's, other immediate (two variants, live-byte selection)
    t.rts()
    while t.pc < T1_STALE + 6:
        t.nop()
    t.w(0x303C)                                           # MOVE.W #imm,D0: its immediate would lie past T2 (rejected: truncated)
    image = t.assemble()
    assert len(image) == T2_LEN, hex(len(image))
    return image


def main_program(sub_len):
    m = F.Asm(MAIN)
    # A: computed transfer into a routine only a data table names
    m.move("l", F.absl(TABLE), F.a(1))
    m.jsr(("ind", 1))
    for case, target in ((1, HIDDEN + 1), (2, ROM_SIZE + 0x100), (3, UNDECODABLE), (4, WINDOW + SUB_RESIDENT)):
        m.immop("cmp", "w", case, F.d(6))
        m.bcc("ne", "a%d" % case)
        m.move("l", F.imm(target), F.a(1))
        jmp_an(m, 1)
        m.label("a%d" % case)
    m.bra("after_data")
    for value in (0x0123, 0x4567, 0x89AB, 0xCDEF):        # data interleaved with code
        m.w(value)
    m.label("after_data")
    # B: transformed copy of the sub image into PRG RAM (bank 0) through the window, then the runnable epoch
    m.immop("cmp", "w", 3, F.d(5))                        # d5 >= 3: arm the reset with the bus still requested (SRES 1, SBRQ 1)
    m.bcc("lt", "nohold")
    m.move("b", F.imm(0x03), F.absl(GA + 0x01))
    m.label("nohold")
    m.move("l", F.imm(SUB_SRC), F.a(0))
    m.move("l", F.imm(WINDOW), F.a(1))
    m.move("w", F.imm(sub_len // 2 - 1), F.d(0))
    m.immop("cmp", "w", 1, F.d(5))
    m.bcc("ne", "full")
    m.move("w", F.imm(sub_len // 4 - 1), F.d(0))
    m.label("full")
    m.label("copy")
    m.move("w", ("inc", 0), F.d(1))
    m.immop("eor", "w", KEY, F.d(1))
    m.move("w", F.d(1), ("inc", 1))
    m.dbra(0, "copy")
    m.immop("cmp", "w", 2, F.d(5))
    m.bcc("ne", "release")
    m.move("w", F.imm(0x4E71), F.absl(WINDOW + SUB_RESIDENT + 4))
    m.label("release")
    # D, main side: a JMP (xxx).L thunk in work RAM built before the epoch
    m.move("w", F.imm(0x4EF9), F.absl(THUNK_MAIN))
    m.move("l", F.imm(DEST1), F.absl(THUNK_MAIN + 2))
    m.move("b", F.imm(0x01), F.absl(GA + 0x01))           # SRES 1, SBRQ 0: the sub runnable epoch (d5 >= 3: by the SBRQ release)
    m.immop("cmp", "w", 4, F.d(5))                        # d5 = 4: once the sub has run, reset it, write the image again,
    m.bcc("ne", "one_epoch")                              # clear its stack page and release it: a second verified epoch
    F.wait_flag(m, GA + 0x0F, 0x01, "wait_first_run")
    m.move("b", F.imm(0x00), F.absl(GA + 0x01))           # SRES 0: reset and halt (the window maps)
    m.move("l", F.imm(SUB_SRC), F.a(0))
    m.move("l", F.imm(WINDOW), F.a(1))
    m.move("w", F.imm(sub_len // 2 - 1), F.d(0))
    m.label("copy_again")
    m.move("w", ("inc", 0), F.d(1))
    m.immop("eor", "w", KEY, F.d(1))
    m.move("w", F.d(1), ("inc", 1))
    m.dbra(0, "copy_again")
    m.move("l", F.imm(WINDOW + SUB_SSP - 0x100), F.a(1))
    m.move("w", F.imm(0x3F), F.d(0))
    m.label("clear_stack")
    m.clr("l", ("inc", 1))
    m.dbra(0, "clear_stack")
    m.move("b", F.imm(0x01), F.absl(GA + 0x01))           # SRES 1, SBRQ 0: the second epoch
    m.label("one_epoch")
    m.jsr(F.absl(THUNK_MAIN))
    m.move("l", F.imm(DEST2), F.absl(THUNK_MAIN + 2))
    m.jsr(F.absl(THUNK_MAIN))
    m.move("l", F.imm(DEST1), F.absl(CASE1_SLOT))         # case 1: immutable instruction, mutable destination in RAM
    m.move("l", F.absl(CASE1_SLOT), F.a(2))
    m.jsr(("ind", 2))
    for case, (slot, value, size) in ((1, (THUNK_MAIN + 2, DEST1 + 1, "l")), (2, (THUNK_MAIN + 2, ROM_SIZE + 0x200, "l")),
                                      (3, (THUNK_MAIN, 0x4EB9, "w")), (4, (CASE1_SLOT, DEST1 + 1, "l"))):
        m.immop("cmp", "w", case, F.d(4))
        m.bcc("ne", "d%d" % case)
        m.move(size, F.imm(value), F.absl(slot))
        if case == 4:
            m.move("l", F.absl(CASE1_SLOT), F.a(2))
            m.jsr(("ind", 2))
        else:
            m.jsr(F.absl(THUNK_MAIN))
        m.label("d%d" % case)
    # C: wait for the sub, halt it (SBRQ), copy T2 verbatim over the transient range, release, signal
    F.wait_flag(m, GA + 0x0F, 0x01, "wait_sub1")
    m.move("b", F.imm(0x03), F.absl(GA + 0x01))           # SRES 1, SBRQ 1: the sub halts, the window maps
    m.move("l", F.imm(T2_SRC), F.a(0))
    m.move("l", F.imm(WINDOW + T1), F.a(1))
    m.move("w", F.imm(T2_LEN // 2 - 1), F.d(0))
    m.label("copy2")
    m.move("w", ("inc", 0), ("inc", 1))
    m.dbra(0, "copy2")
    m.move("b", F.imm(0x01), F.absl(GA + 0x01))           # SBRQ 0 (no reset: not an epoch)
    m.move("w", F.imm(0x0002), F.absl(GA + 0x10))         # COMCMD0 = 2
    F.wait_flag(m, GA + 0x0F, 0x02, "wait_sub2")
    m.move("w", F.absl(GA + 0x20), F.absl(RESULT + 0x20))
    m.move("w", F.absl(GA + 0x22), F.absl(RESULT + 0x22))
    m.move("w", F.absl(GA + 0x24), F.absl(RESULT + 0x24))
    m.move("w", F.absl(GA + 0x28), F.absl(RESULT + 0x30))
    m.move("b", F.imm(0xA5), F.absl(RESULT + 0xFF))
    m.stop(0x2700)
    code = m.assemble()
    assert MAIN + len(code) <= MAIN_FAULT, hex(MAIN + len(code))
    return code


def routine(base, body):
    r = F.Asm(base)
    body(r)
    return r.assemble()


def build_rom():
    sub = sub_image()
    sub_len = (len(sub) + 1) & ~1
    assert sub_len <= T2_SRC - SUB_SRC
    rom = bytearray(ROM_SIZE)
    struct.pack_into(">II", rom, 0, 0x00FFFE00, MAIN)
    for v in range(2, 64):
        struct.pack_into(">I", rom, 4 * v, MAIN_FAULT)
    rom[0x100:0x120] = b"SEGA BIOS-IMAGE FIXTURE SEG064  "
    main = main_program(sub_len)
    rom[MAIN:MAIN + len(main)] = main
    fault = routine(MAIN_FAULT, lambda r: (r.move("w", F.imm(0xDEAD), F.absl(RESULT + 0xF0)), r.stop(0x2700)))
    rom[MAIN_FAULT:MAIN_FAULT + len(fault)] = fault
    hidden = routine(HIDDEN, lambda r: (r.move("w", F.imm(0x1111), F.absl(RESULT)), r.rts()))
    dest1 = routine(DEST1, lambda r: (r.move("w", F.imm(0xD001), F.absl(RESULT + 2)), r.quick("add", "w", 1, F.absl(RESULT + 6)), r.rts()))
    dest2 = routine(DEST2, lambda r: (r.move("w", F.imm(0xD002), F.absl(RESULT + 4)), r.rts()))
    for base, code in ((HIDDEN, hidden), (DEST1, dest1), (DEST2, dest2)):
        rom[base:base + len(code)] = code
    struct.pack_into(">IIII", rom, TABLE, HIDDEN, DEST1, DEST2, 0x01234567)   # the data table (and more data)
    for i in range(0, len(sub), 2):
        struct.pack_into(">H", rom, SUB_SRC + i, struct.unpack_from(">H", sub.ljust(sub_len, b"\0"), i)[0] ^ KEY)
    t2 = t2_image()
    rom[T2_SRC:T2_SRC + len(t2)] = t2
    struct.pack_into(">H", rom, UNDECODABLE, UNDECODABLE_WORD)
    return bytes(rom), sub, t2


UNDECODABLE_WORD = 0x4E70  # RESET: the one word the CPU-owned decoder rejects under this profile (no compiled entry)


def expected(rom, sub, t2):
    return {
        "rom_size": len(rom), "rom_sha256": hashlib.sha256(rom).hexdigest(),
        "reset_pc": MAIN, "reset_ssp": 0x00FFFE00,
        "sub_image_length": (len(sub) + 0xFF) & ~0xFF, "sub_image_sha256": hashlib.sha256(sub.ljust((len(sub) + 0xFF) & ~0xFF, b"\0")).hexdigest(),
        "copy_image": {"processor": "sub", "base": T1, "rom_offset": T2_SRC, "length": len(t2)},
        "thunks": {"main": THUNK_MAIN, "sub": THUNK_SUB},
        "result": {"0": 0x1111, "2": 0xD001, "4": 0xD002, "6": 2, "32": 0x7202, "34": 0x5D01, "36": 0x5D02, "48": 0xAAAE},
        "sub_comstat": [0x7202, 0x5D01, 0x5D02, 0, 0xAAAE, 0, 0, 0],
        "positive_variants": {"main:d5=3": 1, "main:d5=4": 2},  # same results; verified epochs
        "negatives": {
            "main:d6=1": ["entry_not_compiled", 0, None], "main:d6=2": ["entry_not_compiled", 0, None],
            "main:d6=3": ["entry_not_compiled", 0, None], "main:d6=4": ["entry_not_compiled", 0, None],
            "main:d5=1": ["materialized_image_unverified", 1, None], "main:d5=2": ["materialized_image_unverified", 1, None],
            "main:d4=1": ["entry_not_compiled", 0, THUNK_MAIN], "main:d4=2": ["entry_not_compiled", 0, THUNK_MAIN],
            "main:d4=3": ["code_bytes_mismatch", 0, THUNK_MAIN], "main:d4=4": ["entry_not_compiled", 0, None],
            "sub:d3=1": ["code_bytes_mismatch", 1, None], "sub:d3=2": ["code_bytes_mismatch", 1, T1_STALE + 6],
        },
    }


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["build"])
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    rom, sub, t2 = build_rom()
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "bios.bin").write_bytes(rom)
    exp = expected(rom, sub, t2)
    (args.out / "expected.json").write_text(json.dumps(exp, indent=1, sort_keys=True) + "\n")
    print(json.dumps(exp, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
