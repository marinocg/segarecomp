#!/usr/bin/env python3
"""SEG-009-T002: SMS generation route, typed ingestion errors and the generated-native bank-crossing fixture.

usage: sms_bank_crossing_test.py <sms_image_emitter> <cc> <product-root>

Builds the project-authored fixtures (tools/sms_fixture_rom.py), drives the public SMS generation route
(`emit_cartridge` through the test CLI), compiles the emitted image with the SMS runtime memory map and a host as strict
C11, and runs it with a finite cycle budget. Checks:
  * the same logical PC (0x4100) is dispatched under two image identities after a mapper write; slot 0 remap dispatches
    bank code at 0x0500 while the fixed first 1 KiB is unaffected;
  * a jump into work RAM stops with `mutable_code`; an instruction straddling the slot edge stops with
    `unresolved_fetch_mapping` and the emitter lists it as a typed stub owner;
  * a valid-header ROM without a mapper declaration, an unknown name, a non-baseline family, a conflicting declaration
    and a manifest digest mismatch all fail closed with their typed error and emit nothing;
  * emission is byte-identical across two runs; the rom_only fixture runs through the same route.
"""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

EMITTER, CC = sys.argv[1], sys.argv[2]
ROOT = pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sms_cc_cache  # noqa: E402
import sms_fixture_rom as builder  # noqa: E402

RUNTIME_DIR = ROOT / "platforms" / "master-system" / "runtime"
Z80_INCLUDE = ROOT / "libs" / "codegen" / "c11" / "include"
HOST = ROOT / "tests" / "tools" / "sms_bank_host.c"
import host_cc  # noqa: E402
STRICT = host_cc.STRICT_C11
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)


def run(cmd, timeout=600):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout)


def digest_tree(directory):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(pathlib.Path(directory).iterdir()) if p.is_file()}


def emit(rom, out, stem, *args):
    return run([EMITTER, rom, out, stem, *args])


def compile_program(workdir, stem):
    units = (workdir / (stem + ".units")).read_text().split()
    sources = [workdir / u for u in units] + [RUNTIME_DIR / "sms_memory.c", HOST]
    objs = []
    for src in sources:
        obj = workdir / (src.stem + ".o")
        generated = sms_cc_cache.GENERATED_UNIT_FLAGS if src.parent == workdir else []  # authored sources keep the full strict set
        r = sms_cc_cache.compile_object([CC, *STRICT, *generated, "-O0", "-I", Z80_INCLUDE, "-I", RUNTIME_DIR, "-I", workdir, "-c", src, "-o", obj])
        if r.returncode != 0:
            return None, r.stderr[:2000]
        objs.append(obj)
    exe = workdir / (stem + ".exe")
    r = run([CC, *objs, "-o", exe])
    return (exe, None) if r.returncode == 0 else (None, r.stderr[:2000])


def execute(exe, *args):
    r = run([exe, *args], timeout=120)
    result = {"code": r.returncode, "ci": []}
    for line in r.stdout.splitlines():
        key, _, rest = line.partition(" ")
        if key == "ci":
            a, i, b = rest.split()
            result["ci"].append((int(a, 16), int(i), int(b, 16)))
        else:
            result[key] = rest
    if "ram" in result:
        result["ram"] = bytes.fromhex(result["ram"])
    return result


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        builder_out = tmp / "roms"
        rc = builder.main(["--out", str(builder_out), "--fixture", "bank_crossing", "--fixture", "trivial"])
        check(rc == 0, "fixture builder failed")
        crossing = builder_out / "bank_crossing.sms"
        crossing_manifest = builder_out / "bank_crossing.mapper.json"
        trivial = builder_out / "trivial.sms"
        trivial_manifest = builder_out / "trivial.mapper.json"

        # --- typed fail-closed ingestion (nothing emitted) -------------------------------------------------------
        def expect_error(name, typed, *args, rom=crossing):
            out = tmp / ("err_" + name)
            r = emit(rom, out, "e", *args)
            check(r.returncode == 3 and r.stdout.startswith("sms_error " + typed),
                  "%s: expected %s, got rc=%d %r" % (name, typed, r.returncode, r.stdout[:200]))
            check(not out.exists() or not any(out.iterdir()), "%s: files were emitted for a rejected cartridge" % name)

        expect_error("undeclared", "SMS_ERROR_MAPPER_UNDECLARED")
        expect_error("unknown", "SMS_ERROR_MAPPER_UNDECLARED", "--mapper", "turbo")
        expect_error("codemasters", "SMS_ERROR_MAPPER_UNSUPPORTED", "--mapper", "codemasters")
        expect_error("conflict", "SMS_ERROR_MAPPER_UNDECLARED", "--mapper", "rom_only", "--manifest", str(crossing_manifest))
        expect_error("rom_only_size", "SMS_ERROR_ROM_SIZE_UNSUPPORTED", "--mapper", "rom_only")
        bad_manifest = tmp / "bad.mapper.json"
        meta = json.loads(crossing_manifest.read_text())
        meta["sha256"] = "0" * 64
        bad_manifest.write_text(json.dumps(meta))
        expect_error("manifest_digest", "SMS_ERROR_MAPPER_UNDECLARED", "--manifest", str(bad_manifest))
        japan = bytearray(crossing.read_bytes())
        japan[0x7FFF] = 0x3C
        japan_path = tmp / "japan.sms"
        japan_path.write_bytes(bytes(japan))
        expect_error("region", "SMS_ERROR_PROFILE_UNSUPPORTED", "--mapper", "sega", rom=japan_path)
        short = tmp / "short.sms"
        short.write_bytes(crossing.read_bytes()[:0x8001])
        expect_error("size", "SMS_ERROR_ROM_SIZE_UNSUPPORTED", "--mapper", "sega", rom=short)

        # --- generation route, determinism -----------------------------------------------------------------------
        out1, out2 = tmp / "gen1", tmp / "gen2"
        r1 = emit(crossing, out1, "sms", "--manifest", str(crossing_manifest), "--list")
        r2 = emit(crossing, out2, "sms", "--manifest", str(crossing_manifest), "--list")
        check(r1.returncode == 0 and r2.returncode == 0, "emission failed: " + (r1.stdout + r1.stderr)[:400])
        if FAILED:
            return
        check(r1.stdout == r2.stdout, "emitter reports differ between runs")
        check(digest_tree(out1) == digest_tree(out2), "generated files are not byte-identical across two runs")
        check(r1.stdout.startswith("ok mapper=sega source=fixture_builder"), "identity line: " + r1.stdout.splitlines()[0])
        prov = json.loads((out1 / "sms_cartridge.json").read_text())
        check(prov["mapper"] == "sega" and prov["declaration_source"] == "fixture_builder" and prov["size"] == 0x10000 and
              prov["sha256"] == hashlib.sha256(crossing.read_bytes()).hexdigest() and prov["region"] == "sms_export" and
              prov["bank_count"] == 4, "provenance record")
        owners = [line.split() for line in r1.stdout.splitlines() if line.startswith("owner ")]
        # bank 2 (identity 4): offset 0x3FFF is `ld a,n` whose operand would be in the next slot
        stub = [o for o in owners if o[1] == "4" and int(o[2], 16) == 0x3FFF]
        check(stub and stub[0][3] == "stub_unresolved_fetch_mapping", "a slot-straddling instruction is not an unresolved_fetch_mapping stub: %r" % stub)
        check(all(int(o[2], 16) < 0x400 for o in owners if o[1] == "1"), "invariant owners must be the first 1 KiB")

        exe, error = compile_program(out1, "sms")
        check(exe is not None, "generated C did not compile as strict C11: %s" % error)
        if exe is None:
            return

        # --- mandatory finite budget ------------------------------------------------------------------------------
        check(run([exe], timeout=30).returncode == 64, "the host must refuse to run without --cycle-budget")

        # --- bank crossing ----------------------------------------------------------------------------------------
        main_run = execute(exe, "--cycle-budget", "200000")
        ram = main_run.get("ram", b"\0" * 256)
        check(main_run["outcome"] == "halted" and main_run["sms_error"] == "SMS_OK", "main run: %r" % main_run.get("outcome"))
        check(ram[0x00] == 0x11 and ram[0x01] == 0x33 and ram[0x02] == 0x22 and ram[0x03] == 0x0F and ram[0x04] == 0x44 and ram[0xFF] == 0x5A,
              "result block (C200..): %s" % ram[:8].hex())
        at_4100 = [c for c in main_run["ci"] if c[0] == 0x4100]
        check([c[1] for c in at_4100][:2] == [3, 5] and all(c[2] == 0x4000 for c in at_4100),
              "logical PC 0x4100 must dispatch under identities 3 (bank 1) then 5 (bank 3): %r" % at_4100)
        at_500 = [c for c in main_run["ci"] if c[0] == 0x0500]
        check([c[1] for c in at_500][:2] == [2, 4] and all(c[2] == 0 for c in at_500),
              "logical PC 0x0500 (slot 0) must dispatch bank 0 then bank 2: %r" % at_500)
        at_210 = [c for c in main_run["ci"] if c[0] == 0x0210]
        check(at_210 and all(c[1] == 1 for c in at_210), "the fixed first 1 KiB keeps identity 1 after the slot 0 remap")
        check(main_run["regs"].split() == ["00", "02", "03", "02"], "mapper registers after the run: %s" % main_run["regs"])

        ram_jump = execute(exe, "--cycle-budget", "200000", "--pc", "0200")
        check(ram_jump["outcome"] == "mutable_code" and ram_jump["pc"] == "C300" and ram_jump["code"] == 3, "jump into RAM: %r" % ram_jump)
        straddle = execute(exe, "--cycle-budget", "200000", "--pc", "7FFF", "--write", "FFFE=2")
        check(straddle["outcome"] == "unresolved_fetch_mapping" and straddle["pc"] == "7FFF", "slot-straddling instruction: %r" % straddle)
        unmapped = execute(exe, "--cycle-budget", "200000", "--pc", "8000", "--write", "FFFC=08")
        check(unmapped["outcome"] == "mutable_code", "code in cartridge RAM must fail closed: %r" % unmapped)
        # SEG-009-T012: no_owner is unreachable through the SMS mapping by construction: every mappable ROM offset of every
        # declared image is an owner start (full owner or typed stub), so the emitter's owner list has no holes.
        per_identity = {}
        for o in owners:
            per_identity.setdefault(int(o[1]), set()).add(int(o[2], 16))
        check(per_identity.get(1) == set(range(0x400)), "invariant image: every offset of the first 1 KiB has an owner")
        check(all(ids == set(range(0x4000)) for ident, ids in per_identity.items() if ident >= 2),
              "banked images: every offset of every 16 KiB bank has an owner (no_owner unreachable through the SMS mapping)")
        # $FFFC bit 7 is accepted and has no effect on a mask ROM (no stop, mapping unchanged); cartridge RAM is zero at power-on
        bit7 = execute(exe, "--cycle-budget", "2000", "--write", "FFFC=80")
        check(bit7["sms_error"] == "SMS_OK" and bit7["regs"].split()[0] == "80", "$FFFC bit 7 is accepted: %r" % bit7)
        bad_ctl = execute(exe, "--cycle-budget", "200000", "--write", "FFFC=10")
        check(bad_ctl["sms_error"] == "SMS_ERROR_CONTROL_BIT_UNSUPPORTED", "unsupported $FFFC bit: %r" % bad_ctl)

        # --- an invariant-window instruction whose operand lies in the slot-0 bank (Mickey Mouse / Wonder Boy III idiom) -----------
        # `ld a,n` at 0x03FF: the opcode is in the fixed first 1 KiB, the immediate is the first byte of the slot-0 bank, so it must be
        # read from whichever bank slot 0 maps when the instruction begins (never a typed unresolved_fetch_mapping stop).
        patched = bytearray(crossing.read_bytes())
        patched[0x3FF] = 0x3E
        for bank, operand in ((0, 0x5A), (2, 0xA5)):
            base = bank * 0x4000 + 0x400
            patched[base:base + 5] = bytes([operand, 0x32, 0xF0, 0xC2, 0x76])  # operand; ld (0xC2F0),a ; halt
        patched_path = tmp / "straddle_invariant.sms"
        patched_path.write_bytes(bytes(patched))
        s_out = tmp / "straddle_gen"
        rs = emit(patched_path, s_out, "sms", "--mapper", "sega", "--list")
        check(rs.returncode == 0, "invariant straddle emission: " + (rs.stdout + rs.stderr)[:300])
        if rs.returncode == 0:
            owner_3ff = [l.split() for l in rs.stdout.splitlines() if l.startswith("owner ") and l.split()[1] == "1" and int(l.split()[2], 16) == 0x3FF]
            check(owner_3ff and owner_3ff[0][3] == "full", "an invariant start with a banked operand is a full owner: %r" % owner_3ff)
            s_exe, error = compile_program(s_out, "sms")
            check(s_exe is not None, "invariant straddle image did not compile as strict C11: %s" % error)
            if s_exe is not None:
                for label, extra, want in (("slot 0 = bank 0", [], 0x5A), ("slot 0 = bank 2", ["--write", "FFFD=2"], 0xA5)):
                    r_s = execute(s_exe, "--cycle-budget", "200000", "--pc", "03FF", *extra)
                    check(r_s["outcome"] == "halted" and r_s.get("ram", b"\0" * 256)[0xF0] == want,
                          "invariant straddle, %s: expected operand %02X, got %r" % (label, want, r_s.get("ram", b"\0" * 256)[0xF0:0xF1].hex() + " " + r_s["outcome"]))

        # --- 16-bit immediate crossing, structural crossing and a long prefix run at the invariant window end ----------------------
        def straddle_rom(name, edit):
            rom = bytearray(crossing.read_bytes())
            edit(rom)
            path = tmp / (name + ".sms")
            path.write_bytes(bytes(rom))
            out = tmp / (name + "_gen")
            r = emit(path, out, "sms", "--mapper", "sega", "--list")
            check(r.returncode == 0, "%s emission: %s" % (name, (r.stdout + r.stderr)[:300]))
            if r.returncode != 0:
                return None, None, None
            kinds = {int(l.split()[2], 16): l.split()[3] for l in r.stdout.splitlines() if l.startswith("owner ") and l.split()[1] == "1"}
            exe, error = compile_program(out, "sms")
            check(exe is not None, "%s did not compile as strict C11: %s" % (name, error))
            return kinds, exe, r.stdout

        def call_edit(rom):
            rom[0x3FB:0x400] = bytes([0x31, 0xF0, 0xDF, 0xCD, 0x00])  # ld sp,0xDFF0 (a push at SP 0 would hit the mapper registers) ; call nn  # call nn: low operand byte is invariant, high byte is slot-0 bank offset 0x400
            for bank, high, mark in ((0, 0x05, 0xB0), (2, 0x06, 0xB2)):
                base = bank * 0x4000
                rom[base + 0x400] = high
                dest = base + high * 0x100
                rom[dest:dest + 5] = bytes([0x3E, mark, 0x32, 0xF0, 0xC2])  # ld a,mark ; ld (0xC2F0),a
                rom[dest + 5] = 0x76  # halt
        kinds, c_exe, _ = straddle_rom("straddle_call", call_edit)
        if c_exe is not None:
            check(kinds.get(0x3FE) == "full", "call nn crossing into slot 0 is a full owner: %r" % kinds.get(0x3FE))
            for label, extra, want, dest, ident in (("bank 0", [], 0xB0, 0x0500, 2), ("bank 2", ["--write", "FFFD=2"], 0xB2, 0x0600, 4)):
                r_c = execute(c_exe, "--cycle-budget", "200000", "--pc", "03FB", *extra)
                check(r_c["outcome"] == "halted" and r_c["ram"][0xF0] == want, "call straddle, slot 0 = %s: %r %02X" % (label, r_c["outcome"], r_c["ram"][0xF0]))
                check((0x03FB, 1, 0) in r_c["ci"] and (dest, ident, 0) in r_c["ci"],
                      "call straddle, slot 0 = %s: same invariant owner then bank code at %04X: %r" % (label, dest, r_c["ci"]))

        for name, lead in (("straddle_ed", 0xED), ("straddle_cb", 0xCB), ("straddle_dd", 0xDD)):
            def structural_edit(rom, lead=lead):
                rom[0x3FF] = lead  # prefix in the invariant window, its opcode in the next slot
                rom[0x400] = 0x00
            kinds, x_exe, _ = straddle_rom(name, structural_edit)
            if x_exe is not None:
                check(kinds.get(0x3FF) == "stub_unresolved_fetch_mapping", "%s: a prefix with its opcode in the next slot stays a typed stub: %r" % (name, kinds.get(0x3FF)))
                r_x = execute(x_exe, "--cycle-budget", "200000", "--pc", "03FF")
                check(r_x["outcome"] == "unresolved_fetch_mapping" and r_x["pc"] == "03FF", "%s: %r" % (name, r_x))

        def prefix_edit(rom):
            rom[0x3D0:0x400] = bytes([0xDD, 0xFD] * 24)  # 48 consecutive prefixes ending at the window edge
            rom[0x400] = 0x21  # LD IX,nn would need an opcode; in any case bytes beyond the window are never fabricated
        kinds, p_exe, _ = straddle_rom("straddle_prefix_run", prefix_edit)
        if p_exe is not None:
            check(all(kinds.get(a) == "stub_unresolved_fetch_mapping" for a in range(0x3D0, 0x400)),
                  "a long prefix run at the window edge stays typed stubs: %r" % sorted(set(kinds.get(a) for a in range(0x3D0, 0x400))))
            r_p = execute(p_exe, "--cycle-budget", "200000", "--pc", "03D0")
            check(r_p["outcome"] == "unresolved_fetch_mapping" and r_p["pc"] == "03D0", "long prefix run: %r" % r_p)

        # --- rom_only through the same route ----------------------------------------------------------------------
        t_out = tmp / "trivial_gen"
        rt = emit(trivial, t_out, "sms", "--manifest", str(trivial_manifest))
        check(rt.returncode == 0 and rt.stdout.startswith("ok mapper=rom_only"), "rom_only emission: " + rt.stdout[:200])
        if rt.returncode == 0:
            t_exe, error = compile_program(t_out, "sms")
            check(t_exe is not None, "rom_only image did not compile: %s" % error)
            if t_exe is not None:
                t_run = execute(t_exe, "--cycle-budget", "100000")
                check(t_run["outcome"] in ("deadline", "halted") and t_run["sms_error"] == "SMS_OK", "rom_only run: %r" % t_run.get("outcome"))
                jump_out = execute(t_exe, "--cycle-budget", "1000", "--pc", "8000")
                check(jump_out["outcome"] == "mutable_code", "rom_only: code at $8000 is not code: %r" % jump_out)

    if FAILED:
        print("\n".join(FAILED[:20]))
        return 1
    print("sms bank crossing: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
