#!/usr/bin/env python3
"""Project-authored Sega CD fixture for the SEG-097-T001 unvisited-module discovery matrix (ADR 0206). Stdlib only.

No BIOS or commercial byte is read or reproduced. The fixture is a boot disc for the synthetic post-BIOS profile: the IP (main) reads the
runtime input (the machine input `--set main:d7=N`: absent = A, 2 = B, 3 = C; that profile has no controller port) and tells the sub which module
to run. The SP (sub) is an ordinary MC68000 loader: it waits for the choice, then loads the chosen module's sector from the disc through the
CDD/CDC into 2M Word RAM with ONE shared register-argument routine (`load_sector`: the play-command words, the header words and the DMA address
are arguments) and calls it:

  default (no input)    module A (64-byte non-code header, code at +0x40) at $080000; a data sector D is also loaded (never executed)
  input 2 (B)           module B (code from +0) at the same address $080000
  input 3 (C)           module C at two bases ($090000 and $0A0000)
  never referenced      decoy X: a sector holding a valid instruction stream that no loader operation names

Cases (`--unvisited-case`) change only the loader; every module stays the same:
  base          as above
  overwritten   after loading A the loader overwrites one word inside A's executable range, then calls A
  header_patched after loading A the loader overwrites a word of A's NON-executable header, then calls A (must still run)
  recursion     the program recurses without bound before anything else (the scan must exhaust its call-depth bound, not hang)
  unknown_lba   the C arm takes its play-command words from a Gate Array register the program cannot know
  unknown_dest  the C arm takes its DMA address from such a register
  prg_dest      the C arm selects PRG RAM (not Word RAM) as the CDC destination

The expected layout in the returned dict is the TEST ORACLE; it is never an input of discovery.
"""
import hashlib
import pathlib
import random
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sega_cd_fixture_disc as F  # noqa: E402

LBA = {"A": 28, "B": 29, "C": 30, "D": 31, "X": 32}
WORD_RAM = 0x080000
BASE_A, BASE_B = WORD_RAM, WORD_RAM
BASES_C = (0x090000, 0x0A0000)
BASE_D = 0x0B0000
HEADER = 0x40
CASES = ("base", "overwritten", "header_patched", "unknown_lba", "unknown_dest", "prg_dest", "recursion")
MARK = {"A": F.SUB_DATA + 0x20, "B": F.SUB_DATA + 0x22, "C": F.SUB_DATA + 0x24}
COUNT = {"A": F.SUB_DATA + 0x30, "B": F.SUB_DATA + 0x32, "C": F.SUB_DATA + 0x34}
MARKER = {"A": 0xAAAA, "B": 0xBBBB, "C": 0xCCCC}
RESULT_WORDS = 6


def module_code(kind, base):
    """Position-independent code (a local BSR, no absolute self reference): the same bytes run at any base."""
    m = F.Asm(base)
    m.moveq({"A": 1, "B": 2, "C": 3}[kind], 0)
    m.move("w", F.imm(MARKER[kind]), F.absl(MARK[kind]))
    m.quick("add", "w", 1, F.absl(COUNT[kind]))
    m.bsr("leaf")
    m.rts()
    m.label("leaf")
    m.nop()
    m.moveq({"A": 4, "B": 5, "C": 6}[kind], 1)
    m.rts()
    return m.assemble()


def sector(kind):
    """(2,048-byte sector, number of meaningful bytes). Filler after the module is seeded pseudo-random data."""
    rng = random.Random({"A": 0xA0, "B": 0xB0, "C": 0xC0, "D": 0xD0, "X": 0xE0}[kind])
    if kind == "D":
        return bytes(rng.randrange(256) for _ in range(2048)), None
    if kind == "A":
        body = b"SYNTHHDR" + bytes(rng.randrange(256) for _ in range(HEADER - 8)) + module_code("A", WORD_RAM + HEADER)
    elif kind == "X":  # a valid instruction stream that no loader operation names
        body = bytes(HEADER) + module_code("A", WORD_RAM + HEADER)
    else:
        body = module_code(kind, WORD_RAM)
    return (body + bytes(rng.randrange(256) for _ in range(2048 - len(body)))), len(body)


def build(options):
    case = options.get("unvisited_case", "base")
    assert case in CASES, case
    sub = F.Asm(F.SP_BASE)

    def cdc_write(reg, value):
        sub.move("b", F.imm(reg), F.absl(F.GA_SUB + 0x05))
        sub.move("b", F.imm(value), F.absl(F.GA_SUB + 0x07))

    def args(lba, dest, unknown_lba=False, unknown_dest=False):
        words, _ = F.cdd_words(3, lba)
        msf = lba + 150
        want = [(F.bcd(msf // 75 // 60) << 8) | F.bcd(msf // 75 % 60), (F.bcd(msf % 75) << 8) | 0x01]
        for i, reg in enumerate((1, 2, 3, 4)):
            if unknown_lba:
                sub.move("w", F.absl(F.GA_SUB + 0x10 + 2 * i), F.d(reg))   # a Gate Array register the program cannot know statically
            else:
                sub.move("w", F.imm(words[1 + i]), F.d(reg))
        if unknown_dest:
            sub.move("w", F.absl(F.GA_SUB + 0x18), F.d(5))
        else:
            sub.move("w", F.imm((dest - WORD_RAM) >> 3), F.d(5))
        sub.move("w", F.imm(want[0]), F.d(6))
        sub.move("w", F.imm(want[1]), F.d(7))

    sub.label("entry")
    if case == "recursion":
        sub.bsr("rec")
    cdc_write(0x0F, 0x00)
    cdc_write(0x01, 0x02)
    cdc_write(0x0A, 0x84)
    sub.move("b", F.imm(0x00), F.absl(F.GA_SUB + 0x07))
    sub.move("b", F.imm(0x01), F.absl(F.GA_SUB + 0x0F))   # ready
    sub.label("w_go")
    sub.tst("b", F.absl(F.GA_SUB + 0x0E))
    sub.bcc("eq", "w_go")
    sub.label("w_own")
    sub.btst(0, F.absl(F.GA_SUB + 0x03))
    sub.bcc("ne", "w_own")
    sub.move("b", F.absl(F.GA_SUB + 0x0E), F.d(0))        # the choice made by the player (main reads the pad)
    sub.immop("cmp", "b", 2, F.d(0))
    sub.bcc("eq", "pick_b")
    sub.immop("cmp", "b", 3, F.d(0))
    sub.bcc("eq", "pick_c")

    sub.label("pick_a")
    args(LBA["A"], BASE_A)
    sub.bsr("load_sector")
    args(LBA["D"], BASE_D)
    sub.bsr("load_sector")
    # D is loaded and never used. A's DMA was overwritten by D only if the bases overlapped (they do not).
    args(LBA["A"], BASE_A)
    sub.bsr("load_sector")
    if case == "header_patched":
        sub.move("w", F.imm(0x1234), F.absl(BASE_A + 8))      # inside the 64-byte header, outside the executable subrange
    if case == "overwritten":
        sub.move("w", F.imm(0x4E71), F.absl(BASE_A + HEADER + 4))   # one word inside A's executable range
    sub.jsr(F.absl(BASE_A + HEADER))
    sub.bra("done")

    sub.label("pick_b")
    args(LBA["B"], BASE_B)
    sub.bsr("load_sector")
    sub.jsr(F.absl(BASE_B))
    sub.bra("done")

    sub.label("pick_c")
    for base in BASES_C:
        args(LBA["C"], base, unknown_lba=(case == "unknown_lba"), unknown_dest=(case == "unknown_dest") and base == BASES_C[0])
        sub.bsr("load_sector_prg" if case == "prg_dest" else "load_sector")
        sub.jsr(F.absl(base))

    sub.label("done")
    for i, name in enumerate(("A", "B", "C")):
        sub.move("w", F.absl(COUNT[name]), F.absl(F.GA_SUB + 0x20 + 2 * i))
    for i, name in enumerate(("A", "B", "C")):
        sub.move("w", F.absl(MARK[name]), F.absl(F.GA_SUB + 0x26 + 2 * i))
    stop_words, _ = F.cdd_words(1)
    for i, word in enumerate(stop_words):
        sub.move("w", F.imm(word), F.absl(F.GA_SUB + 0x42 + 2 * i))
    sub.move("b", F.imm(0x01), F.absl(F.GA_SUB + 0x03))   # RET = 1: Word RAM back to main
    sub.move("b", F.imm(0x02), F.absl(F.GA_SUB + 0x0F))
    sub.stop(0x2700)

    def loader(label, destination):
        # one sector: the play command (d1..d4), wait for its header (d6, d7), CDC DMA of 2,048 bytes to the destination at d5 * 8
        sub.label(label)
        sub.move("w", F.imm(0x0300), F.absl(F.GA_SUB + 0x42))
        for i, reg in enumerate((1, 2, 3, 4)):
            sub.move("w", F.d(reg), F.absl(F.GA_SUB + 0x44 + 2 * i))
        sub.label("w_valid_" + label)
        sub.move("b", F.imm(0x0F), F.absl(F.GA_SUB + 0x05))
        sub.move("b", F.absl(F.GA_SUB + 0x07), F.d(0))
        sub.btst(7, F.d(0))
        sub.bcc("ne", "w_valid_" + label)
        sub.move("b", F.imm(0x04), F.absl(F.GA_SUB + 0x05))
        for reg in (0, 4):
            sub.move("b", F.absl(F.GA_SUB + 0x07), F.d(reg))
            sub.lsl_w(8, reg)
            sub.move("b", F.absl(F.GA_SUB + 0x07), F.d(reg))
        sub.alu("cmp", "w", F.d(6), 0)
        sub.bcc("ne", "w_valid_" + label)
        sub.alu("cmp", "w", F.d(7), 4)
        sub.bcc("ne", "w_valid_" + label)
        sub.moveq(0, 1)
        sub.move("b", F.absl(F.GA_SUB + 0x07), F.d(1))
        sub.moveq(0, 2)
        sub.move("b", F.absl(F.GA_SUB + 0x07), F.d(2))
        sub.lsl_w(8, 2)
        sub.alu("or", "w", F.d(1), 2)
        sub.quick("add", "w", 4, F.d(2))                  # DAC = PT + 4
        sub.move("b", F.imm(destination), F.absl(F.GA_SUB + 0x04))
        sub.move("w", F.d(5), F.absl(F.GA_SUB + 0x0A))
        sub.move("b", F.imm(0x02), F.absl(F.GA_SUB + 0x05))
        sub.move("b", F.imm(0xFF), F.absl(F.GA_SUB + 0x07))
        sub.move("b", F.imm(0x07), F.absl(F.GA_SUB + 0x07))
        sub.move("b", F.d(2), F.absl(F.GA_SUB + 0x07))
        sub.move("w", F.d(2), F.d(3))
        sub.lsr_w(8, 3)
        sub.move("b", F.d(3), F.absl(F.GA_SUB + 0x07))
        sub.move("b", F.imm(0x00), F.absl(F.GA_SUB + 0x07))   # DTRG
        sub.label("w_edt_" + label)
        sub.btst(7, F.absl(F.GA_SUB + 0x04))
        sub.bcc("eq", "w_edt_" + label)
        sub.rts()

    if case == "recursion":
        sub.label("rec")
        sub.bsr("rec")
        sub.rts()
    loader("load_sector", 7)
    if case == "prg_dest":
        loader("load_sector_prg", 5)
    sp = sub.assemble()

    main = F.Asm(F.IP_BASE)
    F.main_start_sub(main, F.SP_BASE)
    F.wait_flag(main, F.GA_MAIN + 0x0F, 1, "w_ready")
    main.moveq(1, 1)                                       # default: module A
    main.immop("cmp", "w", 2, F.d(7))                      # the runtime input: the machine input main:d7 (this profile has no controller port)
    main.bcc("ne", "not_b")
    main.moveq(2, 1)
    main.bra("go")
    main.label("not_b")
    main.immop("cmp", "w", 3, F.d(7))
    main.bcc("ne", "go")
    main.moveq(3, 1)
    main.label("go")
    main.move("b", F.imm(0x02), F.absl(F.GA_MAIN + 0x03))  # DMNA: Word RAM to the sub
    main.move("b", F.d(1), F.absl(F.GA_MAIN + 0x0E))
    F.wait_flag(main, F.GA_MAIN + 0x0F, 2, "w_done")
    main.lea(F.absl(F.RESULT), 3)
    for reg in range(RESULT_WORDS):
        main.move("w", F.absl(F.GA_MAIN + 0x20 + 2 * reg), ("inc", 3))
    F.main_finish(main)

    sectors = {}
    layout = {}
    for kind in "ABCDX":
        data, length = sector(kind)
        sectors[LBA[kind]] = data
        layout[kind] = {"lba": LBA[kind], "sha256": hashlib.sha256(data).hexdigest(), "code_bytes": length}
    code_a = module_code("A", BASE_A + HEADER)
    expected = {
        "case": case, "lba": LBA, "result_words": RESULT_WORDS,
        "modules": {
            "A": {"lba": LBA["A"], "bases": [BASE_A], "entries": [HEADER], "range": [HEADER, len(code_a)]},
            "B": {"lba": LBA["B"], "bases": [BASE_B], "entries": [0], "range": [0, len(module_code("B", BASE_B))]},
            "C": {"lba": LBA["C"], "bases": list(BASES_C), "entries": [0], "range": [0, len(module_code("C", BASES_C[0]))]},
        },
        "data_sector": LBA["D"], "decoy_sector": LBA["X"], "layout": layout,
        "markers": {k: MARKER[k] for k in "ABC"},
    }
    return main.assemble(), sp, sectors, expected
