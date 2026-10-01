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
STRICT = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-D_CRT_SECURE_NO_WARNINGS"]
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
