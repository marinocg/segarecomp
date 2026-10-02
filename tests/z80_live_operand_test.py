#!/usr/bin/env python3
"""SEG-032-T012 (ADR 0073): shape-stable live operands of RAM-backed Z80 images.

A RAM-backed image (`live 1`) compiled from RAM bytes with payload A must execute the SAME compiled instruction form when only the
descriptor displacement/immediate payload bytes of the live memory have become B (a self-patched operand), and must stop with
`code_mismatch` before any effect when any statically defining byte (prefix, opcode, final DDCB/FDCB opcode, register/condition/bit
selection) differs. Proved here against independent static references (an immutable image of the same form compiled directly from
the B bytes), over every legal canonical form that has a displacement or an immediate field (taken from the T001 legal-form
dataset, test side only):

  * payload matrix: for every such form (two opcode samples each) and six payload values B (boundaries included), the live image
    executed over memory B equals, instruction for instruction (state, memory digest, port-access digest, cycles), the immutable
    reference compiled from B; two register/flag initialisations cover both outcomes of conditional forms; payload really matters
    (the A-reference traces differ from the B ones);
  * structural matrix: for every case and every structural byte, three single-bit mutations stop at instruction entry with
    `code_mismatch` and leave state, memory and ports exactly as before the instruction;
  * re-fetch on loop and block-repeat re-entry: a program that patches the immediate of an instruction in a loop, a DJNZ whose
    displacement is patched between iterations, and an LDIR that writes the two immediate bytes of a later LD HL,nn;
  * an instruction that writes its own operand (LD (nn),HL / LD (nn),A / INC (IX+d)) uses the entry snapshot (equal to the static
    reference, expected memory and MEMPTR);
  * an interrupt handler whose immediate is patched executes correctly; a structurally mutated handler stops;
  * prologue order: a rejected instruction consumes neither the EI deferral nor the LD A,I/R marker (after repair the traces equal
    the unmutated run, where an interrupt is accepted at exactly the same boundary);
  * a host without the RAM-code callback fails closed; the linked program contains no decoder symbol; immutable emission carries
    no live snapshot.
usage: z80_live_operand_test.py <z80_image_emitter> <cc> <source-root>
"""
import concurrent.futures
import pathlib
import random
import re
import shutil
import subprocess
import sys
import tempfile

emitter, cc, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
sys.path.insert(0, str(root / "tools"))
import z80_conformance as z  # noqa: E402

RUNNER = root / "tests" / "tools" / "z80_live_guard_runner.c"
PREFIX = ("CB", "ED", "DD", "FD")
STEPS = 3
JOBS = 8

INITS = [
    ["a=5A", "f=00", "b=19", "c=33", "d=19", "e=44", "h=19", "l=80", "ix=1C80", "iy=1D80"],
    ["a=A5", "f=FF", "b=01", "c=00", "d=00", "e=00", "h=00", "l=00", "ix=0005", "iy=FFFE"],
]
DV = [0x00, 0x01, 0x7F, 0x80, 0xFF, 0x55]
NV = [0x00, 0x01, 0x7F, 0x80, 0xFF, 0xA5]
EV = [0x00, 0x05, 0x7F, 0x80, 0xFE, 0xE0]                 # never -1/-3/-4: those would land inside the instruction's own bytes
IMM16 = [0x0000, 0x00FF, 0x0100, 0x7FFF, 0x8000, 0xFFFF]
ADDR = [0x1800, 0x18FF, 0x1FFF, 0x2000, 0x4000, 0xFFFF]   # data region, a RAM mirror edge and non-RAM addresses
TARGET = [0x0000, 0x00FE, 0x0800, 0x0B55, 0x0FF0, 0x4000]  # zero-filled (NOP) regions and a non-code address
A_VALUES = {"d": 0x10, "n": 0x33, "e": 0x10, "imm16": 0x1234, "addr": 0x1810, "target": 0x0810}
failures = []


def check(ok, label, detail=None):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)
        if detail:
            print(detail)


def pick_opcodes(form):
    ranges = form["byte_ranges"]
    chosen = []
    for opcode in (ranges[0][0], ranges[-1][1]):
        if opcode not in chosen:
            chosen.append(opcode)
    return chosen


def payload_kind(form):
    tokens = [t for t in form["layout"].split() if t not in PREFIX and t != "op"]
    if tokens == ["e"]:
        return "e", 1
    if tokens in (["d"], ["d", "n"]):
        return "d", len(tokens)
    if tokens == ["n"]:
        return "n", 1
    assert tokens == ["nn_lo", "nn_hi"], tokens
    if form["mnemonic"] in ("JP", "CALL"):
        return "target", 2
    if "nn_ind" in (form["dst"], form["src"]):
        return "addr", 2
    return "imm16", 2


def payload_bytes(kind, count, index):
    """Payload bytes for value index `index` (None = the compiled A value)."""
    if kind in ("imm16", "addr", "target"):
        value = A_VALUES[kind] if index is None else {"imm16": IMM16, "addr": ADDR, "target": TARGET}[kind][index]
        return [value & 0xFF, value >> 8]
    if kind == "d":
        out = [A_VALUES["d"] if index is None else DV[index]]
        if count == 2:
            out.append(A_VALUES["n"] if index is None else NV[index])
        return out
    if kind == "n":
        return [A_VALUES["n"] if index is None else NV[index]]
    return [A_VALUES["e"] if index is None else EV[index]]


class Case:
    def __init__(self, form, opcode, address):
        self.form, self.opcode, self.address = form, opcode, address
        self.kind, self.count = payload_kind(form)
        tokens = form["layout"].split()
        self.structural = [i for i, t in enumerate(tokens) if t in PREFIX or t == "op"]
        self.length = len(tokens)
        self.payload_positions = [i for i, t in enumerate(tokens) if i not in self.structural]

    def encode(self, index):
        return z.encoding_code(self.form, self.opcode, payload_bytes(self.kind, self.count, index))


def build_cases():
    data, forms = z.load_dataset()
    cases = []
    rel_address, general_address = 0x0200, 0x1000
    for form in forms.values():
        tokens = form["layout"].split()
        if not any(t not in PREFIX and t != "op" for t in tokens) or form["scope"] != "in_scope":
            continue
        for opcode in pick_opcodes(form):
            if payload_kind(form)[0] == "e":
                cases.append(Case(form, opcode, rel_address))
                rel_address += 0x100
            else:
                cases.append(Case(form, opcode, general_address))
                general_address += 8
    assert rel_address < 0x0F00 and general_address < 0x1800, (hex(rel_address), hex(general_address))
    return cases


def base_ram():
    ram = bytearray(8192)
    rnd = random.Random(0x5EED)
    for address in range(0x1800, 0x2000):
        ram[address] = rnd.randrange(256)
    return ram


def ram_for(cases, index):
    ram = base_ram()
    for case in cases:
        code = case.encode(index)
        ram[case.address:case.address + len(code)] = code
    return bytes(ram)


def spec_for(ram, live):
    text = "image 1 %s\nwindow 1 0000 0 4000\n" % ("banked" if live else "invariant")
    if live:
        text += "live 1\n"
    doubled = ram * 2
    for start in range(0, len(doubled), 4096):
        text += "bytes 1 %s\n" % doubled[start:start + 4096].hex()
    return text


def build(tc, ram, live, workdir):
    stats, owners, error = z.emit_image(tc, spec_for(ram, live), workdir, "image")
    assert error is None, error
    exe, message = z.compile_units(tc, workdir, "image", extra_sources=[RUNNER])
    assert exe is not None, message
    return exe


def write_ram(tmp, name, ram):
    path = tmp / (name + ".hex")
    path.write_text(bytes(ram).hex())
    return path


def run_exe(exe, hexfile, steps, *extra):
    out = subprocess.run([str(exe), str(hexfile), "run", str(steps), *extra], text=True, capture_output=True, check=True).stdout
    return out


BATCH = 128
_batch_counter = [0]


def run_batch(tmp, requests):
    """Executes many (exe, hexfile, steps, args) runs, a chunk of runs per process (the runner's `batch` mode), in parallel. Returns the
    outputs in request order; each equals what `run_exe` returns for the same arguments."""
    outputs = [None] * len(requests)
    chunks = []
    by_exe = {}
    for index, request in enumerate(requests):
        by_exe.setdefault(request[0], []).append(index)
    for exe, indices in by_exe.items():
        for start in range(0, len(indices), BATCH):
            chunks.append((exe, indices[start:start + BATCH]))

    def one(chunk):
        exe, indices = chunk
        _batch_counter[0] += 1
        script = tmp / ("batch%d.txt" % _batch_counter[0])
        # The script is whitespace separated: RAM images are named relative to `tmp` (the working directory), never by an absolute path.
        script.write_text("".join(" ".join([requests[i][1].name, "run", str(requests[i][2]), *requests[i][3]]) + "\n" for i in indices))
        text = subprocess.run([str(exe), "batch", script.name], text=True, capture_output=True, check=True, cwd=tmp).stdout
        parts = text.split("BATCH_END\n")
        assert len(parts) == len(indices) + 1 and parts[-1] == "", "batch output does not match its script"
        for i, part in zip(indices, parts):
            outputs[i] = part

    parallel(one, chunks)
    return outputs


def init_args(init):
    args = []
    for item in init:
        args += ["init", item]
    return args


def drop_mem(text):
    return re.sub(r" mem=[0-9A-F]+", "", text)


def last_state(out):
    return out.splitlines()[-1]


def field(line, name):
    return re.search(r"\b%s=([0-9A-F]+)" % name, line).group(1)


def parallel(function, items):
    with concurrent.futures.ThreadPoolExecutor(JOBS) as pool:
        return list(pool.map(function, items))


def main():
    tc = z.Toolchain(cc, pathlib.Path(emitter), opt="-O0", cache=False, owner_group=128)
    cases = build_cases()
    kinds = sorted({c.kind for c in cases})
    forms_covered = {c.form["id"] for c in cases}
    check(set(kinds) == {"d", "n", "e", "imm16", "addr", "target"} and len(forms_covered) > 80,
          "the case matrix covers every payload class (%s) over %d forms, %d cases" % (",".join(kinds), len(forms_covered), len(cases)))
    check({c.form["space"] for c in cases} >= {"base", "ed", "dd", "fd", "ddcb", "fdcb"},
          "the matrix covers base, ED, DD, FD, DDCB and FDCB forms")
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        ram_a = ram_for(cases, None)
        live_exe = build(tc, ram_a, True, tmp / "live")
        tc1 = z.Toolchain(cc, pathlib.Path(emitter), opt="-O0", cache=False, owner_group=1, share_bodies=0)
        live_exe_ref_mode = build(tc1, ram_a, True, tmp / "live_g1")
        ref_a = build(tc, ram_a, False, tmp / "ref_a")
        rams = [ram_for(cases, j) for j in range(6)]
        hex_a = write_ram(tmp, "ram_a", ram_a)
        hex_b = [write_ram(tmp, "ram_b%d" % j, rams[j]) for j in range(6)]
        refs = [build(tc, rams[j], False, tmp / ("ref_b%d" % j)) for j in range(6)]

        # ---- payload matrix ----
        jobs = [(c, i, j) for c in cases for i in range(len(INITS)) for j in range(6)]

        # The A-compiled reference run does not depend on the payload value: one run per (case, init). The one-function-per-start emission
        # is exercised for two representative payload values per class (j=1 is the low boundary, j=3 the sign boundary of every table);
        # the shared emission is compared for all six.
        REF_MODE_VALUES = (1, 3)

        def start_args(c, i):
            return ["start", "%X" % c.address] + init_args(INITS[i])

        base_out = dict(zip([(c, i) for c in cases for i in range(len(INITS))],
                            run_batch(tmp, [(ref_a, hex_a, STEPS, [*start_args(c, i), "immutable"]) for c in cases for i in range(len(INITS))])))
        requests = []
        for c, i, j in jobs:
            requests.append((live_exe, hex_b[j], STEPS, start_args(c, i)))
            requests.append((refs[j], hex_b[j], STEPS, [*start_args(c, i), "immutable"]))
            if j in REF_MODE_VALUES:
                requests.append((live_exe_ref_mode, hex_b[j], STEPS, start_args(c, i)))
        outputs = iter(run_batch(tmp, requests))
        results = []
        for c, i, j in jobs:
            live, ref = next(outputs), next(outputs)
            ref_mode = next(outputs) == ref if j in REF_MODE_VALUES else True
            results.append((live == ref, ref_mode, base_out[(c, i)] != ref, "code_mismatch" not in live))
        check(all(r[0] for r in results), "payload matrix: %d of %d live executions equal the static reference compiled from the B bytes" %
              (sum(r[0] for r in results), len(results)))
        check(all(r[1] for r in results), "payload matrix: the unshared one-function-per-start emission is equal as well (payload values 1 and 3)")
        check(all(r[3] for r in results), "payload matrix: no payload mutation is ever reported as code_mismatch")
        per_case = {}
        for (c, i, j), r in zip(jobs, results):
            per_case[id(c)] = per_case.get(id(c), False) or r[2]
        check(sum(per_case.values()) >= 0.9 * len(cases),
              "the payload is semantically live: %d of %d cases change behaviour between payload A and some B" % (sum(per_case.values()), len(cases)))

        # ---- structural matrix ----
        before_out = run_batch(tmp, [(live_exe, hex_a, 0, ["start", "%X" % c.address, *init_args(INITS[0])]) for c in cases])
        before_state = {id(c): drop_mem(last_state(out).split(" ", 3)[3]) for c, out in zip(cases, before_out)}
        sjobs = [(c, p, x) for c in cases for p in c.structural for x in (0x01, 0x08, 0x40)]
        mutated_out = run_batch(tmp, [(live_exe, hex_a, STEPS, ["start", "%X" % c.address, *init_args(INITS[0]), "mutate-after", "0", "%X" % (c.address + p), "%X" % x])
                                      for c, p, x in sjobs])
        sresults = []
        for (c, p, x), out in zip(sjobs, mutated_out):
            end = last_state(out)
            sresults.append(end.startswith("END code_mismatch 0 ") and out.count("STEP") == 0 and
                            drop_mem(end.split(" ", 3)[3]) == before_state[id(c)] and ("pc=%04X" % c.address) in end)
        check(all(sresults), "structural matrix: %d of %d single-bit prefix/opcode/selector mutations stop at entry with no effect" %
              (sum(sresults), len(sresults)))

        # ---- scenarios ----
        ram = bytearray(base_ram())

        def put(address, code):
            ram[address:address + len(code)] = bytes(code)
        put(0x0000, bytes.fromhex("ED56" "FB" "00000000" "76"))           # im 1; ei; nop x4; halt
        put(0x0038, bytes.fromhex("3E11" "FB" "ED4D"))                      # handler: ld a,11h; ei; reti
        put(0x0100, bytes.fromhex("110019" "210701" "3E00" "12" "13" "34" "7E" "FE04" "20F6" "76"))
        put(0x0200, bytes.fromhex("0604" "0C" "10FD" "76"))
        put(0x0300, bytes.fromhex("21001A" "110C03" "010200" "EDB0" "210000" "76"))
        put(0x1A00, bytes([0x34, 0x12]))
        put(0x0400, bytes.fromhex("22" "0104" "76"))                        # ld (0401h),hl : writes its own operand
        put(0x0410, bytes.fromhex("DD" "34" "05" "76"))                     # inc (ix+5) with ix+5 = its own displacement byte
        put(0x0420, bytes.fromhex("32" "2104" "76"))                        # ld (0421h),a
        put(0x0500, bytes.fromhex("FB" "00" "00" "00" "76"))                # ei; nop; nop; nop; halt
        put(0x0600, bytes.fromhex("ED57" "000000" "76"))                    # ld a,i; nop; nop; nop; halt
        put(0x0610, bytes.fromhex("ED57" "000000" "76"))
        scenario = bytes(ram)
        ram_b = bytearray(scenario)
        ram_b[0x0039] = 0x22                                                # handler immediate patched
        hex_s, hex_sb = write_ram(tmp, "ram_s", scenario), write_ram(tmp, "ram_sb", ram_b)
        live_s = build(tc, scenario, True, tmp / "live_s")
        ref_s = build(tc, scenario, False, tmp / "ref_s")
        ref_sb = build(tc, bytes(ram_b), False, tmp / "ref_sb")

        out = run_exe(live_s, hex_s, 80, "start", "100", "dump", "1900", "4")
        check("DUMP 00010203" in out and last_state(out).startswith("END halted") and "af=0400" not in last_state(out) and field(last_state(out), "hl") == "0107",
              "a loop that patches the immediate of its own LD A,n re-reads it on every iteration (stored 0,1,2,3)")
        out = run_exe(live_s, hex_s, 40, "start", "200", "init", "c=00")
        clean_bc = field(last_state(out), "bc")
        mutated = run_exe(live_s, hex_s, 40, "start", "200", "init", "c=00", "mutate-after", "4", "204", "FD")
        check(last_state(out).startswith("END halted") and clean_bc == "0004" and last_state(mutated).startswith("END halted") and
              field(last_state(mutated), "bc") == "0202",
              "a DJNZ displacement patched between iterations is re-read on the next iteration (loop exits early: BC 0004 -> 0202)",
              out + "\n--\n" + mutated)
        out = run_exe(live_s, hex_s, 60, "start", "300")
        check(last_state(out).startswith("END halted") and field(last_state(out), "hl") == "1234" and field(last_state(out), "bc") == "0000"
              and out.count("STEP") == 6, "an LDIR (two re-entries) that writes both immediate bytes of a later LD HL,nn: the new payload is executed", out)
        for label, start, dump, expect_dump, wz, extra in (
                ("LD (nn),HL with nn naming its own operand", "400", "400 3", "DUMP 22ABCD", "0402", ["init", "l=AB", "init", "h=CD"]),
                ("INC (IX+d) whose effective address is its own displacement byte", "410", "410 4", "DUMP DD340676", "0412", ["init", "ix=040D"]),
                ("LD (nn),A with nn naming its own operand", "420", "420 3", "DUMP 325A04", "5A22", ["init", "a=5A"])):
            args = ["start", start, "dump", *dump.split(), *extra]
            live = run_exe(live_s, hex_s, 5, *args)
            ref = run_exe(ref_s, hex_s, 5, *args, "immutable")
            check(live == ref and expect_dump in live and field(last_state(live), "wz") == wz and last_state(live).startswith("END halted"),
                  "self-written operand, %s: the entry snapshot is used (equals the static reference, MEMPTR and memory as expected)" % label)
        # interrupt handler with a patched immediate / structurally mutated handler
        live = run_exe(live_s, hex_sb, 40, "int-after", "3")
        ref = run_exe(ref_sb, hex_sb, 40, "int-after", "3", "immutable")
        check(live == ref and last_state(live).startswith("END halted") and "af=22" in live,
              "an IM1 handler with a patched immediate executes the live value (equals the static reference)")
        mutated = run_exe(live_s, hex_s, 40, "int-after", "3", "mutate-after", "0", "38", "01")
        check(last_state(mutated).startswith("END code_mismatch") and "pc=0038" in last_state(mutated),
              "a structurally mutated interrupt handler stops at the vector")
        # prologue order: a rejected instruction consumes neither the EI deferral nor the LD A,I/R marker
        for label, start, steps_before, offset, init in (
                ("after EI deferral", "500", "1", "501", ["init", "im=1"]),
                ("after LD A,I", "600", "1", "602", ["init", "iff=1", "init", "im=1"])):
            clean = run_exe(live_s, hex_s, 40, "start", start, "int-after", steps_before, *init)
            repaired = run_exe(live_s, hex_s, 40, "start", start, "mutate-after", steps_before, offset, "01", "repair", "int-on-repair", *init)
            mismatch = [l for l in repaired.splitlines() if l.startswith("MISMATCH")]
            rest = "\n".join(l for l in repaired.splitlines() if not l.startswith("MISMATCH"))
            marker = "df=1" if "EI" in label else "ai=1"
            check(len(mismatch) == 1 and marker in mismatch[0] and drop_mem(rest).strip() == drop_mem(clean).strip() and "0038" in clean,
                  "a code_mismatch %s keeps that boundary state (%s); after repair the run equals the unmutated one" % (label, marker),
                  repaired + "\n--\n" + clean)
        clean = run_exe(live_s, hex_s, 40, "start", "600", "int-after", "1", "init", "iff=1", "init", "im=1")
        control = run_exe(live_s, hex_s, 40, "start", "610", "int-after", "2", "init", "iff=1", "init", "im=1")

        def pv_after_handler_entry(text):
            for line in text.splitlines():
                if "pc=003" in line and line.startswith("STEP"):
                    return int(field(line, "af")[2:], 16) & 4
            return None
        check(pv_after_handler_entry(clean) == 0 and pv_after_handler_entry(control) == 4,
              "control: the LD A,I marker clears PV for an interrupt accepted right after it, and not one boundary later")

        # ---- host without the RAM-code callback, no decoder symbol, immutable emission ----
        out = run_exe(live_s, hex_s, 5, "start", "100", "no-matcher")
        check(last_state(out).startswith("END code_mismatch 0 ") and "STEP" not in out,
              "a host without code_fetch fails closed at the first instruction of a RAM-backed image")
        nm = shutil.which("nm")
        if nm:
            symbols = subprocess.run([nm, str(live_s)], text=True, capture_output=True).stdout
            check(not re.search(r"decode_at|classify_all|z80_decode|replay_bytes", symbols), "the linked RAM-backed program contains no decoder symbol")
        live_text = "".join(p.read_text() for p in sorted((tmp / "live").glob("*.c")))
        ref_text = "".join(p.read_text() for p in sorted((tmp / "ref_a").glob("*.c")))
        check("rt->live_code[" in live_text and "z80_live_guard(" in live_text, "RAM-backed emission reads its operands from the entry snapshot")
        check("live_code" not in ref_text and "z80_live_guard" not in ref_text and "z80_owner_begin" not in ref_text,
              "immutable emission carries no live snapshot, guard or split prologue")
    print("z80 live operands: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
