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
FILES = {"MODA.BIN;1": (40, 2), "MODB.BIN;1": (44, 1), "MODC.BIN;1": (46, 1)}   # name -> (first LBA, sectors)
BASE_A, BASE_B, BASE_C = 0x090000, 0x0A0000, 0x0B0000
HEADER_A = 0x40
FAR_A = 0x840
CASES = ("base", "alias", "unmodelled", "two_paths", "corrupt_pvd", "corrupt_extent", "no_exec", "stale_service", "late_patch", "entry_unknown")
SP_TABLE_PREFIX = 0x2A


def module_a():
    """Module A at BASE_A: header, entry at +0x40 (marker, a call into sector 1 that counts), RTS; the routine is at +0x840 (in the second sector)."""
    m = F.Asm(BASE_A + HEADER_A)
    m.moveq(1, 0)
    m.move("w", F.imm(U.MARKER["A"]), F.absl(U.MARK["A"]))
    m.w(0x6100)                                             # BSR.W far
    far_disp_at = len(m.words)
    m.w(0)
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
    s.lea(F.absl(WORK), 5)
    s.move("b", F.imm(3), work(V_MODE))                        # device byte: sub read
    s.immop("cmp", "b", 2, F.d(7))
    s.bcc("eq", "pick_b")
    if case == "two_paths":
        s.immop("cmp", "b", 3, F.d(7))
        s.bcc("eq", "pick_c")

    def pick(label, name_label, base, entry_offset):
        s.label(label)
        s.lea(F.absl(name_label), 0)
        s.move("l", F.imm(base), work(V_BASE))                  # the destination, carried in the work area
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
        s.bsr("load_file")
        if case == "no_exec" and label == "pick_a":
            s.bra("done")                                       # loaded and never executed: data
        if case == "entry_unknown" and label == "pick_a":
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
    pick("pick_b", "name_b", BASE_B, 0)
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

    a_sectors, a_code_len, far_len = module_a()
    b_sector, b_code_len = module_plain("B", BASE_B, 0xB1)
    c_sector, c_code_len = module_plain("C", BASE_C, 0xC1)
    sectors = {}
    sectors[FILES["MODA.BIN;1"][0]] = a_sectors[:2048]
    sectors[FILES["MODA.BIN;1"][0] + 1] = a_sectors[2048:]
    sectors[FILES["MODB.BIN;1"][0]] = b_sector
    sectors[FILES["MODC.BIN;1"][0]] = c_sector
    file_sizes = {"MODA.BIN;1": (FILES["MODA.BIN;1"][0], 4096), "MODB.BIN;1": (FILES["MODB.BIN;1"][0], 2048), "MODC.BIN;1": (FILES["MODC.BIN;1"][0], 2048)}
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
        },
        "directory_lba": LBA_ROOT, "pvd_lba": LBA_PVD,
        "sha256": {str(k): hashlib.sha256(v).hexdigest() for k, v in sectors.items() if k >= 40},
        "work": WORK, "dirbuf": DIRBUF,
    }
    return main.assemble(), sp, sectors, oracle
