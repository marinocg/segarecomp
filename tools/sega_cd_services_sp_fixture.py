#!/usr/bin/env python3
"""Project-authored disc + sub program for the SEG-099 work-variable loader matrix (ADR 0216). Stdlib only.

No BIOS or commercial byte is read or reproduced. The sub program (SP) is an ordinary MC68000 "file engine" written for this project against the PUBLIC BIOS
CD-service ABI (ADR 0215): its load parameters live in a RAM work area addressed through A5 (sector, count, destination, device byte, module base); it reads the
ISO9660 volume descriptor and root directory through the BIOS services, compares directory records with a constant file name and loads the file:

    read_sectors:  write the device byte; CDCSTOP; ROMREADN with A0 -> {sector, count} in the work area; per sector: CDCSTAT, CDCREAD, CDCTRN(A0 = destination), CDCACK;
                   advance the destination and the sector, decrement the count in the work area (all memory-carried)
    load_file:     (A0 -> name, 12(A5) = destination) PVD at LBA 16 -> root directory -> record by name -> extent and size -> read_sectors

The disc has two files: module A (two sectors; its first instructions are in sector 0 and a routine in sector 1) and module B (one sector). The SP loads A on the
default route and B when the machine says so; both are found by name through the directory, nothing names an LBA.

`build(options)` -> (ip, sp, sectors, oracle). The BIOS image that implements the services is tools/sega_cd_bios_services_fixture.py.
"""
import hashlib
import pathlib
import random
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sega_cd_fixture_disc as F  # noqa: E402
import sega_cd_unvisited_fixture as U  # noqa: E402

BIOS_VECTOR = 0x5F22
FN = {"CDCSTOP": 0x89, "CDCSTAT": 0x8A, "CDCREAD": 0x8B, "CDCTRN": 0x8C, "CDCACK": 0x8D, "ROMREADN": 0x20}
WORK, DIRBUF = 0x020000, 0x021000
V_LBA, V_COUNT, V_BASE, V_DEST, V_HDR, V_MODE = 4, 8, 12, 16, 20, 40
LBA_PVD, LBA_TERM, LBA_ROOT = 16, 17, 18
FILES = {"MODA.BIN;1": (40, 2), "MODB.BIN;1": (44, 1), "MODC.BIN;1": (46, 1), "MODR.BIN;1": (48, 1)}   # name -> (first LBA, sectors)
BASE_A, BASE_B, BASE_C = 0x090000, 0x0A0000, 0x0B0000
BASE_R = 0x0A8000
XOR_KEY = 0x5A
HEADER_A = 0x40
FAR_A = 0x840
CASES = ("base", "alias", "unmodelled", "two_paths", "corrupt_pvd", "corrupt_extent", "no_exec", "stale_service", "late_patch", "entry_unknown",
         "loop_pointer", "loop_pointer_late", "loop_pointer_early_store", "loop_unknown_exit", "loop_memory", "loop_memory_base", "branch_two", "loop_alias", "computed",
         "recursive", "recursive_taken", "cycle", "transformed", "overlay", "hdr_alias",
         "trn_disjoint", "trn_a0", "trn_dev", "trn_lba", "trn_hdr")   # T005 follow-up: CDCTRN calls whose effects the walk cannot bound   # SEG-099-T003: state-convergence cases; T004: recursive discovery cases
SP_TABLE_PREFIX = 0x2A
SCRATCH = 0x040000
RAW_BUF, RAW_HDR = 0x050000, 0x050A00
BASE_A2 = 0x0B8000
V_X = 24


def module_a(extra=None):
    """Module A at BASE_A: header, entry at +0x40 (marker, a call into sector 1 that counts), RTS; the routine is at +0x840 (in the second sector)."""
    m = F.Asm(BASE_A + HEADER_A)
    m.moveq(1, 0)
    m.move("w", F.imm(U.MARKER["A"]), F.absl(U.MARK["A"]))
    m.w(0x6100)                                             # BSR.W far
    far_disp_at = len(m.words)
    m.w(0)
    if extra:
        extra(m)
    m.rts()
    code = bytearray(m.assemble())
    far = F.Asm(BASE_A + FAR_A)
    far.quick("add", "w", 1, F.absl(U.COUNT["A"]))
    far.rts()
    far_code = far.assemble()
    disp = (BASE_A + FAR_A) - (BASE_A + HEADER_A + 2 * far_disp_at)
    code[2 * far_disp_at:2 * far_disp_at + 2] = struct.pack(">h", disp)
    rng = random.Random(0xA1)
    sector0 = b"SYNTHHDR" + bytes(rng.randrange(256) for _ in range(HEADER_A - 8)) + bytes(code)
    sector0 = sector0 + bytes(rng.randrange(256) for _ in range(2048 - len(sector0)))
    body1 = bytearray(bytes(rng.randrange(256) for _ in range(2048)))
    body1[FAR_A - 2048:FAR_A - 2048 + len(far_code)] = far_code
    return sector0 + bytes(body1), len(code), len(far_code)


def module_plain(kind, base, seed):
    code = U.module_code(kind, base)
    rng = random.Random(seed)
    return code + bytes(rng.randrange(256) for _ in range(2048 - len(code))), len(code)


def iso_sectors(files, case):
    """PVD (16), terminator (17) and the root directory (18); `files` maps name -> (lba, size)."""
    pvd = bytearray(2048)
    pvd[0] = 1
    pvd[1:6] = b"CD002" if case == "corrupt_pvd" else b"CD001"
    pvd[6] = 1
    pvd[8:40] = b"SEGARECOMP".ljust(32)
    pvd[40:72] = b"SEGARECOMP_SERVICES".ljust(32)
    pvd[80:88] = F.iso_both32(96)
    pvd[120:124] = F.iso_both16(1)
    pvd[124:128] = F.iso_both16(1)
    pvd[128:132] = F.iso_both16(2048)
    pvd[156:190] = F.iso_record(b"\x00", LBA_ROOT, 2048, directory=True)
    pvd[881] = 1
    term = bytearray(2048)
    term[0] = 255
    term[1:6] = b"CD001"
    term[6] = 1
    root = bytearray()
    root += F.iso_record(b"\x00", LBA_ROOT, 2048, directory=True)
    root += F.iso_record(b"\x01", LBA_ROOT, 2048, directory=True)
    for name, (lba, size) in files.items():
        root += F.iso_record(name.encode(), lba, size)
    return {LBA_PVD: bytes(pvd), LBA_TERM: bytes(term), LBA_ROOT: bytes(root.ljust(2048, b"\0"))}


def build(options):
    case = options.get("services_case", "base")
    assert case in CASES, case
    s = F.Asm(F.SP_BASE + SP_TABLE_PREFIX)

    def bios(fn):
        s.move("w", F.imm(FN[fn]), F.d(0))
        s.jsr(F.absl(BIOS_VECTOR))

    def work(disp):
        return ("d16", disp, 5)

    s.label("entry")
    s.move("b", F.imm(0x01), F.absl(F.GA_SUB + 0x0F))        # ready
    s.label("w_go")
    s.tst("b", F.absl(F.GA_SUB + 0x0E))
    s.bcc("eq", "w_go")
    s.label("w_own")
    s.btst(0, F.absl(F.GA_SUB + 0x03))
    s.bcc("ne", "w_own")
    s.move("b", F.absl(F.GA_SUB + 0x0E), F.d(7))              # the player's choice (main reads the machine input)
    if case == "loop_pointer":                                  # a counted read loop over a known, advancing pointer: every iteration is a new exact state, nothing is stored
        s.lea(F.absl(SCRATCH), 2)
        s.move("w", F.imm(5999), F.d(3))
        s.label("lp_clear")
        s.move("l", ("inc", 2), F.d(5))
        s.dbra(3, "lp_clear")
    if case == "loop_pointer_early_store":                      # the store loop before any argument is stored: later knowledge is intact but the boot image is no longer trusted
        s.lea(F.absl(SCRATCH), 2)
        s.move("w", F.imm(5999), F.d(3))
        s.label("lpe_clear")
        s.clr("l", ("inc", 2))
        s.dbra(3, "lpe_clear")
    s.lea(F.absl(WORK), 5)
    s.move("b", F.imm(3), work(V_MODE))                        # device byte: sub read
    if case == "loop_pointer_late":                             # the same loop AFTER the arguments are stored: the widened pointer is an unknown store target, so the work area is forgotten
        s.lea(F.absl(SCRATCH), 2)
        s.move("w", F.imm(5999), F.d(3))
        s.label("lp_clear")
        s.clr("l", ("inc", 2))
        s.dbra(3, "lp_clear")
    if case == "loop_unknown_exit":                             # a register that grows without bound while the exit depends on a value the program cannot know
        s.moveq(0, 4)
        s.label("lu_loop")
        s.quick("add", "l", 1, F.d(4))
        s.move("w", F.absl(F.GA_SUB + 0x10), F.d(0))
        s.tst("w", F.d(0))
        s.bcc("ne", "lu_loop")
    if case == "loop_memory":                                   # a work variable that changes every iteration (unrelated to the load arguments)
        s.move("l", F.imm(0), work(V_X))
        s.label("lm_loop")
        s.quick("add", "l", 1, work(V_X))
        s.move("w", F.absl(F.GA_SUB + 0x10), F.d(0))
        s.tst("w", F.d(0))
        s.bcc("ne", "lm_loop")
    s.immop("cmp", "b", 2, F.d(7))
    s.bcc("eq", "pick_b")
    if case == "two_paths":
        s.immop("cmp", "b", 3, F.d(7))
        s.bcc("eq", "pick_c")

    def pick(label, name_label, base, entry_offset):
        s.label(label)
        s.lea(F.absl(name_label), 0)
        s.move("l", F.imm(base), work(V_BASE))                  # the destination, carried in the work area
        if case == "loop_memory_base" and label == "pick_a":    # the loop changes the destination itself: it must become unknown, never a chosen value
            s.label("lb_loop")
            s.immop("add", "l", 0x10000, work(V_BASE))
            s.move("w", F.absl(F.GA_SUB + 0x10), F.d(0))
            s.tst("w", F.d(0))
            s.bcc("ne", "lb_loop")
        if case == "branch_two" and label == "pick_a":          # two candidate destinations meet at the shared loader
            s.move("w", F.absl(F.GA_SUB + 0x10), F.d(0))
            s.tst("w", F.d(0))
            s.bcc("eq", "bt_join")
            s.move("l", F.imm(BASE_A2), work(V_BASE))
            s.label("bt_join")
        if case == "loop_alias" and label == "pick_a":          # every iteration stores through a pointer the program cannot know
            s.move("w", F.absl(F.GA_SUB + 0x12), F.d(0))
            s.move("w", F.d(0), F.a(3))
            s.label("la_loop")
            s.move("l", F.imm(0), ("inc", 3))
            s.move("w", F.absl(F.GA_SUB + 0x10), F.d(0))
            s.tst("w", F.d(0))
            s.bcc("ne", "la_loop")
        if case == "alias" and label == "pick_a":
            s.move("w", F.absl(F.GA_SUB + 0x10), F.d(0))        # an address the program cannot know ...
            s.move("w", F.d(0), F.a(3))
            s.move("l", F.imm(0), ("ind", 3))                   # ... stored through: it may alias the work area
        if case == "unmodelled" and label == "pick_a":
            s.move("w", F.imm(0x21), F.d(0))                    # ROMREADE: the two public sources disagree on its argument, so it is not modelled: it may write anything
            s.jsr(F.absl(BIOS_VECTOR))
        if case == "stale_service" and label == "pick_a":
            s.move("w", F.imm(0x0B), F.d(0))                    # a service number that is no load
            s.jsr(F.absl(BIOS_VECTOR))
        if case in ("trn_disjoint", "trn_a0", "trn_dev", "trn_lba", "trn_hdr") and label == "pick_a":
            # one hand-written sector transfer before the loader: the arguments are stored, then CDCTRN is called with fully known (disjoint) buffers or with ONE unknown input
            s.move("l", F.imm(16), work(V_LBA))
            s.move("l", F.imm(1), work(V_COUNT))
            if case == "trn_dev":
                s.move("b", F.absl(F.GA_SUB + 0x10), F.absl(F.GA_SUB + 0x04))     # a device byte the program cannot know
            else:
                s.move("b", work(V_MODE), F.absl(F.GA_SUB + 0x04))
            bios("CDCSTOP")
            s.lea(work(V_X) if case == "trn_lba" else work(V_LBA), 0)               # trn_lba: the {sector,count} block was never written: unknown
            bios("ROMREADN")
            s.label("raw_stat")
            bios("CDCSTAT")
            s.bcc("cs", "raw_stat")
            bios("CDCREAD")
            s.bcc("cs", "raw_stat")
            if case == "trn_a0":
                s.move("l", F.absl(F.GA_SUB + 0x10), F.a(0))
            else:
                s.move("l", F.imm(RAW_BUF), F.a(0))
            if case == "trn_hdr":
                s.move("l", F.absl(F.GA_SUB + 0x14), F.a(1))
            else:
                s.move("l", F.imm(RAW_HDR), F.a(1))
            bios("CDCTRN")
            bios("CDCACK")
            s.lea(F.absl(name_label), 0)                                              # the raw sequence used A0
        s.bsr("load_file")
        if case == "transformed" and label == "pick_b":         # the module is stored encoded and decoded in place before it runs: its executed bytes are not a disc span
            s.move("l", work(V_BASE), F.a(0))
            s.move("w", F.imm(2047), F.d(1))
            s.label("tx_loop")
            s.immop("eor", "b", XOR_KEY, ("inc", 0))
            s.dbra(1, "tx_loop")
        if case == "no_exec" and label == "pick_a":
            s.bra("done")                                       # loaded and never executed: data
        if case in ("entry_unknown", "computed") and label == "pick_a":
            s.move("l", F.absl(F.GA_SUB + 0x10), F.a(0))        # the entry comes from a Gate Array register: the load is proven, its execution mapping is not
        else:
            s.move("l", work(V_BASE), F.a(0))
        if entry_offset:
            s.lea(("d16", entry_offset, 0), 0)
        if case == "late_patch" and label == "pick_a":
            s.move("w", F.imm(0x4E71), ("d16", HEADER_A + 4, 0) if not entry_offset else ("d16", 4, 0))   # a word inside the loaded executable range, then the call
        s.w(0x4E90)                                             # JSR (A0)  (A0 from memory)
        s.bra("done")

    pick("pick_a", "name_a", BASE_A, HEADER_A)
    pick("pick_b", "name_b", BASE_A if case == "overlay" else BASE_B, 0)
    if case == "two_paths":
        pick("pick_c", "name_c", BASE_C, 0)

    s.label("done")
    for i, name in enumerate(("A", "B", "C")):
        s.move("w", F.absl(U.COUNT[name]), F.absl(F.GA_SUB + 0x20 + 2 * i))
    for i, name in enumerate(("A", "B", "C")):
        s.move("w", F.absl(U.MARK[name]), F.absl(F.GA_SUB + 0x26 + 2 * i))
    stop_words, _ = F.cdd_words(1)
    for i, word in enumerate(stop_words):
        s.move("w", F.imm(word), F.absl(F.GA_SUB + 0x42 + 2 * i))
    s.move("b", F.imm(0x01), F.absl(F.GA_SUB + 0x03))        # RET = 1: Word RAM back to main
    s.move("b", F.imm(0x02), F.absl(F.GA_SUB + 0x0F))
    s.stop(0x2700)

    # ---- read_sectors: the work-area driven BIOS sector reader --------------------------------------------------------------------
    s.label("read_sectors")
    s.move("b", work(V_MODE), F.absl(F.GA_SUB + 0x04))        # the device byte, from memory
    bios("CDCSTOP")
    s.lea(work(V_LBA), 0)                                      # A0 -> {sector, count}
    bios("ROMREADN")
    s.label("rs_next")
    bios("CDCSTAT")
    s.bcc("cs", "rs_next")
    bios("CDCREAD")
    s.bcc("cs", "rs_next")
    s.move("l", work(V_DEST), F.a(0))                          # A0 = destination (memory-carried)
    if case == "hdr_alias":                                    # the header pointer carries a high byte the 68000 bus ignores: it lands on the base variable
        s.move("l", F.imm(WORK + V_BASE + 0x01000000), F.a(1))
    else:
        s.lea(work(V_HDR), 1)
    bios("CDCTRN")
    bios("CDCACK")
    s.immop("add", "l", 2048, work(V_DEST))
    s.quick("add", "l", 1, work(V_LBA))
    s.quick("sub", "l", 1, work(V_COUNT))
    s.bcc("gt", "rs_next")
    s.rts()

    # ---- load_file: A0 -> 10-byte name, 12(A5) = destination ----------------------------------------------------------------------
    s.label("load_file")
    s.move("l", F.a(0), F.a(4))                                # the name pointer (A4 survives the BIOS calls)
    s.move("l", F.imm(LBA_PVD), work(V_LBA))
    s.move("l", F.imm(1), work(V_COUNT))
    s.move("l", F.imm(DIRBUF), work(V_DEST))
    s.bsr("read_sectors")
    s.lea(F.absl(DIRBUF), 1)
    for i, ch in enumerate(b"CD001"):                          # the volume descriptor identifier
        s.immop("cmp", "b", ch, ("d16", 1 + i, 1))
        s.bcc("ne", "lf_fail")
    s.move("l", ("d16", 162, 1), work(V_LBA))                  # the root directory extent (big-endian half of the both-endian field)
    s.move("l", F.imm(1), work(V_COUNT))
    s.move("l", F.imm(DIRBUF), work(V_DEST))
    s.bsr("read_sectors")
    s.lea(F.absl(DIRBUF), 1)
    s.label("lf_rec")
    s.moveq(0, 0)
    s.move("b", ("ind", 1), F.d(0))
    s.tst("b", F.d(0))
    s.bcc("eq", "lf_fail")                                     # a zero record length ends the directory
    s.lea(("d16", 33, 1), 2)
    s.move("l", F.a(4), F.a(3))
    s.move("w", F.imm(len("MODA.BIN;1") - 1), F.d(1))
    s.label("lf_cmp")
    s.move("b", ("inc", 2), F.d(2))
    s.w(0xB41B)                                                # CMP.B (A3)+,D2
    s.bcc("ne", "lf_next")
    s.dbra(1, "lf_cmp")
    s.bra("lf_found")
    s.label("lf_next")
    s.w(0xD2C0)                                                # ADDA.W D0,A1
    s.bra("lf_rec")
    s.label("lf_found")
    s.move("l", ("d16", 6, 1), work(V_LBA))                    # the file's extent (big-endian)
    s.move("l", ("d16", 14, 1), F.d(1))                        # the file's size (big-endian)
    s.moveq(0, 2)
    s.label("lf_count")
    s.quick("add", "l", 1, F.d(2))
    s.immop("sub", "l", 2048, F.d(1))
    s.bcc("gt", "lf_count")
    s.move("l", F.d(2), work(V_COUNT))
    s.move("l", work(V_BASE), work(V_DEST))
    s.bsr("read_sectors")
    s.label("lf_fail")
    s.rts()
    s.label("name_a")
    for pair in (b"MO", b"DA", b".B", b"IN", b";1"):
        s.w(struct.unpack(">H", pair)[0])
    s.label("name_b")
    for pair in (b"MO", b"DB", b".B", b"IN", b";1"):
        s.w(struct.unpack(">H", pair)[0])
    s.label("name_r")
    for pair in (b"MO", b"DR", b".B", b"IN", b";1"):
        s.w(struct.unpack(">H", pair)[0])
    s.label("name_c")
    for pair in (b"MO", b"DC", b".B", b"IN", b";1"):
        s.w(struct.unpack(">H", pair)[0])
    sp_code = s.assemble()
    header = bytearray(SP_TABLE_PREFIX)
    header[0:8] = b"MAIN    "
    header[0x18:0x1C] = (0x20).to_bytes(4, "big")
    header[0x20:0x28] = (0x000A).to_bytes(2, "big") + bytes(6)
    sp = bytes(header) + sp_code

    main = F.Asm(F.IP_BASE)
    F.wait_flag(main, F.GA_MAIN + 0x0F, 1, "w_ready")
    main.moveq(1, 1)
    main.immop("cmp", "w", 2, F.d(7))
    main.bcc("ne", "not_b")
    main.moveq(2, 1)
    main.bra("go")
    main.label("not_b")
    main.immop("cmp", "w", 3, F.d(7))
    main.bcc("ne", "go")
    main.moveq(3, 1)
    main.label("go")
    main.move("b", F.imm(0x02), F.absl(F.GA_MAIN + 0x03))
    main.move("b", F.d(1), F.absl(F.GA_MAIN + 0x0E))
    F.wait_flag(main, F.GA_MAIN + 0x0F, 2, "w_done")
    main.lea(F.absl(F.RESULT), 3)
    for reg in range(U.RESULT_WORDS):
        main.move("w", F.absl(F.GA_MAIN + 0x20 + 2 * reg), ("inc", 3))
    F.main_finish(main)

    L = s.labels   # the SP's own routine and name addresses: a module may call the SP's library (an absolute call into the boot image)

    def loader_call(m, name, base, then):
        m.lea(F.absl(L[name]), 0)
        m.move("l", F.imm(base), work(V_BASE))
        m.jsr(F.absl(L["load_file"]))
        m.move("l", work(V_BASE), F.a(0))
        then(m)

    extra_a = None
    if case == "recursive_taken":                              # the same, with the load on the TAKEN arm of a conditional branch inside module A
        def extra_a(m):
            m.immop("cmp", "b", 3, F.d(7))
            m.bcc("eq", "do_r")
            m.bra("skip_r")
            m.label("do_r")
            loader_call(m, "name_r", BASE_R, lambda mm: mm.w(0x4E90))
            m.label("skip_r")
    if case == "recursive":                                    # on route 3 module A itself loads and runs module R (never loaded by the build pass)
        def extra_a(m):
            m.immop("cmp", "b", 3, F.d(7))
            m.bcc("ne", "skip_r")
            loader_call(m, "name_r", BASE_R, lambda mm: mm.w(0x4E90))   # JSR (A0)
            m.label("skip_r")
    if case == "cycle":                                        # A loads B and jumps to it; B loads A and jumps back: a loading cycle
        def extra_a(m):
            loader_call(m, "name_b", BASE_B, lambda mm: mm.w(0x4ED0))   # JMP (A0)
    a_sectors, a_code_len, far_len = module_a(extra_a)
    b_sector, b_code_len = module_plain("B", BASE_B, 0xB1)
    if case == "cycle":
        mb = F.Asm(BASE_B)
        loader_call(mb, "name_a", BASE_A, lambda mm: (mm.lea(("d16", HEADER_A, 0), 0), mm.w(0x4ED0)))
        code_b = mb.assemble()
        rngb = random.Random(0xB1)
        b_sector = code_b + bytes(rngb.randrange(256) for _ in range(2048 - len(code_b)))
        b_code_len = len(code_b)
    if case == "transformed":
        b_sector = bytes(x ^ XOR_KEY for x in b_sector)
    c_sector, c_code_len = module_plain("C", BASE_C, 0xC1)
    r_sector, r_code_len = module_plain("C", BASE_R, 0xD1)
    sectors = {}
    sectors[FILES["MODA.BIN;1"][0]] = a_sectors[:2048]
    sectors[FILES["MODA.BIN;1"][0] + 1] = a_sectors[2048:]
    sectors[FILES["MODB.BIN;1"][0]] = b_sector
    sectors[FILES["MODC.BIN;1"][0]] = c_sector
    sectors[FILES["MODR.BIN;1"][0]] = r_sector
    file_sizes = {"MODA.BIN;1": (FILES["MODA.BIN;1"][0], 4096), "MODB.BIN;1": (FILES["MODB.BIN;1"][0], 2048), "MODC.BIN;1": (FILES["MODC.BIN;1"][0], 2048), "MODR.BIN;1": (FILES["MODR.BIN;1"][0], 2048)}
    if case == "corrupt_extent":
        file_sizes["MODB.BIN;1"] = (5000, 2048)                  # a directory record whose extent leaves the track
    iso = iso_sectors(file_sizes, case)
    sectors.update(iso)
    rng = random.Random(0x5EED)
    for lba in range(96):
        if lba >= 16 and lba not in sectors:
            sectors[lba] = bytes(rng.randrange(256) for _ in range(2048))
    oracle = {
        "case": case, "markers": {k: U.MARKER[k] for k in "ABC"},
        "modules": {
            "A": {"lba": FILES["MODA.BIN;1"][0], "sectors": 2, "bases": [BASE_A], "entries": [HEADER_A], "far": FAR_A, "far_bytes": far_len, "code_bytes": a_code_len},
            "B": {"lba": FILES["MODB.BIN;1"][0], "sectors": 1, "bases": [BASE_B], "entries": [0], "code_bytes": b_code_len},
            "C": {"lba": FILES["MODC.BIN;1"][0], "sectors": 1, "bases": [BASE_C], "entries": [0], "code_bytes": c_code_len},
            "R": {"lba": FILES["MODR.BIN;1"][0], "sectors": 1, "bases": [BASE_R], "entries": [0], "code_bytes": r_code_len},
        },
        "directory_lba": LBA_ROOT, "pvd_lba": LBA_PVD,
        "sha256": {str(k): hashlib.sha256(v).hexdigest() for k, v in sectors.items() if k >= 40},
        "work": WORK, "dirbuf": DIRBUF,
    }
    return main.assemble(), sp, sectors, oracle
