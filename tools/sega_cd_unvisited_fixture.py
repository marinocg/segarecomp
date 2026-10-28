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
CASES = ("base", "overwritten", "header_patched", "unknown_lba", "unknown_dest", "prg_dest", "recursion",
         "pi", "pi_odd", "pi_escape", "pi_overwrite", "long_dest", "stale_lba", "table_header")
SP_TABLE_PREFIX = 0x2A   # SEG-098-T001: an authentic-style SP header ("MAIN", module jump table at +$20, init at +$2A) before the loader code
MARK = {"A": F.SUB_DATA + 0x20, "B": F.SUB_DATA + 0x22, "C": F.SUB_DATA + 0x24}
COUNT = {"A": F.SUB_DATA + 0x30, "B": F.SUB_DATA + 0x32, "C": F.SUB_DATA + 0x34}
MARKER = {"A": 0xAAAA, "B": 0xBBBB, "C": 0xCCCC}
RESULT_WORDS = 8
PI_BASE = F.SUB_DATA + 0x40
PI = {name: PI_BASE + 2 * i for i, name in enumerate(("W1", "W2", "CNT", "EXC", "EXC2", "HELP", "W3", "W4"))}
PI_SUM = F.SUB_DATA + 0x60   # long: the sum of the addresses LEA d(PC) produced at each base
PI_DATA_WORD, PI_TAB = 0x1357, (0x1111, 0x2222, 0x3333)


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
    code = m.assemble()
    module_code.starts = [pc - base for pc in m.starts]   # the encoder's own instruction starts (offsets in the code; the test oracle's labels)
    return code


class PiAsm(F.Asm):
    """The shared encoder plus the PC-relative and byte-displacement forms the source-relative matrix needs."""

    def mark(self):
        if not self.starts or self.starts[-1] != self.pc:
            self.starts.append(self.pc)

    def lea_pc(self, label, an):
        self.mark()
        self.w(0x41FA | (an << 9))
        self.fixups.append((len(self.words), label, "rel16"))
        self.w(0)

    def move_w_pc(self, label, dn):
        self.mark()
        self.w(0x303A | (dn << 9))
        self.fixups.append((len(self.words), label, "rel16"))
        self.w(0)

    def move_w_pc_index(self, label, xn, dn):  # move.w label(pc,xn.w),dn  (brief extension word, d8 resolved by the assembler)
        self.mark()
        self.w(0x303B | (dn << 9))
        self.fixups.append((len(self.words), label, "rel8w"))
        self.w(xn << 12)

    def jsr_pc(self, label):
        self.mark()
        self.w(0x4EBA)
        self.fixups.append((len(self.words), label, "rel16"))
        self.w(0)

    def bcc_s(self, cond, label):
        self.mark()
        self.w(0x6000 | (F.COND[cond] << 8))
        self.fixups.append((len(self.words) - 1, label, "rel8"))

    def assemble(self):
        keep = []
        for index, label, kind in self.fixups:
            if kind == "rel8":
                disp = self.labels[label] - (self.base + 2 * index) - 2
                assert -128 <= disp < 128 and disp != 0, label
                self.words[index] |= disp & 0xFF
            elif kind == "rel8w":
                disp = self.labels[label] - (self.base + 2 * index)
                assert -128 <= disp < 128, label
                self.words[index] |= disp & 0xFF
            else:
                keep.append((index, label, kind))
        self.fixups = keep
        return super().assemble()


def module_code_pi(base, helper, variant):
    """Module C of the source-relative matrix: PC-relative effective addresses (lea, d16(PC), (d8,PC,Xn)), a PC-relative JSR, a computed JMP,
    forward/backward/short branches, DBRA, a local BSR, an ABSOLUTE call into the boot image, absolute stores, TRAP and divide-by-zero
    exceptions, then RTS. Everything is position-independent except the absolute helper call and the absolute stores."""
    m = PiAsm(base)
    m.moveq(3, 0)
    m.move("w", F.imm(MARKER["C"]), F.absl(MARK["C"]))
    m.quick("add", "w", 1, F.absl(COUNT["C"]))
    if variant == "odd":
        m.lea_pc("data", 0)
        m.quick("add", "l", 1, F.a(0))
        m.move("w", ("ind", 0), F.d(0))                    # an odd-address word read: the address-error path
    if variant == "escape":
        m.mark()
        m.w(0x4EF9)                                      # jmp $0B8000: outside every admitted image
        m.lw(0x0B8000)
    m.lea_pc("data", 0)
    m.move("l", F.a(0), F.d(5))
    m.move("l", F.absl(PI_SUM), F.d(6))
    m.alu("add", "l", F.d(5), 6)
    m.move("l", F.d(6), F.absl(PI_SUM))
    m.move_w_pc("data", 1)
    m.move("w", F.d(1), F.absl(PI["W1"]))
    m.moveq(2, 3)
    m.move_w_pc_index("tab", 3, 4)
    m.move("w", F.d(4), F.absl(PI["W2"]))
    m.bsr("leaf")
    m.moveq(3, 2)
    m.label("loop")
    m.quick("add", "w", 1, F.absl(PI["CNT"]))
    m.dbra(2, "loop")
    m.bcc_s("t", "skip")
    m.nop()
    m.nop()
    m.label("skip")
    m.moveq(1, 7)
    m.bcc_s("eq", "target")                              # never taken; gives the computed JMP's destination a static edge (the emitter compiles reached code)
    m.immop("cmp", "w", PI_DATA_WORD, F.d(1))
    m.bcc_s("ne", "bad")
    m.jsr_pc("jtarget")
    m.lea_pc("target", 1)
    m.mark()
    m.w(0x4ED1)                                          # jmp (a1)
    m.nop()
    m.nop()
    m.label("target")
    m.quick("add", "w", 1, F.absl(PI["W4"]))
    m.jsr(F.absl(helper))
    m.mark()
    m.w(0x4E40)                                          # trap #0
    m.moveq(1, 0)
    m.mark()
    m.w(0x80FC)                                          # divu.w #0,d0  (vector 5)
    m.w(0)
    m.label("bad")
    m.rts()
    m.label("jtarget")
    m.quick("add", "w", 1, F.absl(PI["W3"]))
    m.rts()
    m.label("leaf")
    m.nop()
    m.rts()
    m.label("data")
    m.w(PI_DATA_WORD)
    m.label("tab")
    m.w(*PI_TAB)
    code = m.assemble()
    module_code_pi.starts = [pc - base for pc in m.starts]   # offsets of the instructions of the last call (the test oracle)
    return code, m.labels["data"] - base


def sector(kind, variant=None, helper=0, pi_base=BASES_C[0]):
    """(2,048-byte sector, number of meaningful bytes). Filler after the module is seeded pseudo-random data."""
    rng = random.Random({"A": 0xA0, "B": 0xB0, "C": 0xC0, "D": 0xD0, "X": 0xE0}[kind])
    if kind == "D":
        return bytes(rng.randrange(256) for _ in range(2048)), None
    if kind == "A":
        body = b"SYNTHHDR" + bytes(rng.randrange(256) for _ in range(HEADER - 8)) + module_code("A", WORD_RAM + HEADER)
    elif kind == "X":  # a valid instruction stream that no loader operation names
        body = bytes(HEADER) + module_code("A", WORD_RAM + HEADER)
    elif kind == "C" and variant is not None:
        body = module_code_pi(pi_base, helper, variant)[0]
    else:
        body = module_code(kind, WORD_RAM)
    return (body + bytes(rng.randrange(256) for _ in range(2048 - len(body)))), len(body)


def build(options):
    case = options.get("unvisited_case", "base")
    assert case in CASES, case
    table = case == "table_header" or bool(options.get("sp_table"))   # SEG-098-T002: the authentic-style SP header + module jump table, with any case
    bios = bool(options.get("bios"))                                    # SEG-098-T002: a BIOS-image profile boot: the BIOS starts the sub, so the IP does not
    sub = F.Asm(F.SP_BASE + (SP_TABLE_PREFIX if table else 0))

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

    pi = case.startswith("pi")
    sub.label("entry")
    if case == "recursion":
        sub.bsr("rec")
    if pi:   # install the TRAP #0 and divide-by-zero handlers in the sub's vector table (MOVE.L #handler,(vector).L)
        for label, vector in (("h_trap", 0x80), ("h_div", 0x14)):
            sub.w(0x23FC)
            sub.lw(label)
            sub.lw(vector)
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
    if case == "stale_lba":   # a second transfer with no new play command: the drive has moved on, so the position is not the one last commanded
        args(LBA["B"], 0x0A8000)
        sub.bsr("load_sector_noplay")
        sub.jsr(F.absl(0x0A8000))
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
        if case == "pi_overwrite" and base == BASES_C[1]:
            sub.move("w", F.imm(0x4E71), F.absl(base + 4))   # one word inside the second mapping, after it was loaded
        sub.jsr(F.absl(base))

    sub.label("done")
    for i, name in enumerate(("A", "B", "C")):
        sub.move("w", F.absl(COUNT[name]), F.absl(F.GA_SUB + 0x20 + 2 * i))
    for i, name in enumerate(("A", "B", "C")):
        sub.move("w", F.absl(MARK[name]), F.absl(F.GA_SUB + 0x26 + 2 * i))
    stop_words, _ = F.cdd_words(1)
    for i, word in enumerate(stop_words):
        sub.move("w", F.imm(word), F.absl(F.GA_SUB + 0x42 + 2 * i))
    sub.move("w", F.absl(PI_SUM + 2), F.absl(F.GA_SUB + 0x2C))
    sub.move("w", F.absl(PI["W2"]), F.absl(F.GA_SUB + 0x2E))
    sub.move("b", F.imm(0x01), F.absl(F.GA_SUB + 0x03))   # RET = 1: Word RAM back to main
    sub.move("b", F.imm(0x02), F.absl(F.GA_SUB + 0x0F))
    sub.stop(0x2700)

    def loader(label, destination, play=True, long_dest=False):
        # one sector: the play command (d1..d4), wait for its header (d6, d7), CDC DMA of 2,048 bytes to the destination at d5 * 8
        sub.label(label)
        if play:
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
        if long_dest:   # a LONG write to the Gate Array: the runtime splits it into two word accesses (the high word selects destination 0)
            sub.move("l", F.imm(destination), F.absl(F.GA_SUB + 0x04))
        else:
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
    if pi:
        sub.label("pi_helper")                            # an absolute call target for the module (never relocated)
        sub.quick("add", "w", 1, F.absl(PI["HELP"]))
        sub.rts()
        sub.label("h_trap")
        sub.quick("add", "w", 1, F.absl(PI["EXC"]))
        sub.rte()
        sub.label("h_div")
        sub.quick("add", "w", 1, F.absl(PI["EXC2"]))
        sub.rte()
    loader("load_sector", 7, long_dest=(case == "long_dest"))
    if case == "stale_lba":
        loader("load_sector_noplay", 7, play=False)
    if case == "prg_dest":
        loader("load_sector_prg", 5)
    sp = sub.assemble()
    if table:   # descriptor entry stays 0 (the header); the BIOS enters through the module jump table: init, main, int2, user
        header = bytearray(SP_TABLE_PREFIX)
        header[0:8] = b"MAIN    "
        header[0x18:0x1C] = (0x20).to_bytes(4, "big")
        header[0x20:0x28] = (0x000A).to_bytes(2, "big") + bytes(6)
        sp = bytes(header) + sp

    main = F.Asm(F.IP_BASE)
    if not bios:
        F.main_start_sub(main, F.SP_BASE + (SP_TABLE_PREFIX if table else 0))
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

    variant = {"pi": "pi", "pi_overwrite": "pi", "pi_odd": "odd", "pi_escape": "escape"}.get(case)
    helper = sub.labels.get("pi_helper", 0)
    sectors = {}
    layout = {}
    for kind in "ABCDX":
        data, length = sector(kind, variant, helper)
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
    labels = {}
    for kind in "ABC":
        if variant is not None and kind == "C":
            module_code_pi(BASES_C[0], helper, variant)
            labels[kind] = list(module_code_pi.starts)
        else:
            module_code(kind, WORD_RAM + (HEADER if kind == "A" else 0))
            labels[kind] = [o + (HEADER if kind == "A" else 0) for o in module_code.starts]
    expected["labels"] = {"boot_ip": [pc - F.IP_BASE for pc in main.starts], "boot_sp": [pc - F.SP_BASE for pc in sub.starts],
                          "lba%d" % LBA["A"]: labels["A"], "lba%d" % LBA["B"]: labels["B"], "lba%d" % LBA["C"]: labels["C"]}
    if variant is not None:
        _, data_offset = module_code_pi(BASES_C[0], helper, variant)
        expected["pi"] = {"sum_low16": sum(base + data_offset for base in BASES_C) & 0xFFFF, "w2": PI_TAB[1], "w1": PI_DATA_WORD,
                          "helper": helper, "data_offset": data_offset, "scratch": {k: v for k, v in PI.items()}, "sum_address": PI_SUM}
    return main.assemble(), sp, sectors, expected
