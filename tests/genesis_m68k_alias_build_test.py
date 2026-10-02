#!/usr/bin/env python3
"""SEG-028-T005 (ADR 0077, ADR 0049, ADR 0076): `segarecomp build` runs the build-time ADR 0049 alias preparation.

Project-authored synthetic Genesis ROMs only (hand-encoded MC68000 code on the tools/genesis_z80_fixture_rom.py layout). The reset
code stores the address of a copy prologue in work RAM and jumps there indirectly, so the prologue is reached only as an
immutable-ROM AOT identity (the shape of tests/genesis_immutable_copy_alias_generated_test.py). The prologue copies a 32-byte
routine from the cartridge to work RAM with MOVE.L (A0)+,(A1)+ / DBRA and transfers to the RAM copy with JMP abs.L. The routine
writes the PSG port once (the observable effect) and jumps back to a ROM spin loop that advances virtual time.

  copy      the routine is copied verbatim: the alias-less program stops fail-closed at the RAM target
            (known_but_unemitted_target); the build proposes exactly one alias (1 round, 32 bytes, route_advanced), reports it as
            one static_proof / genesis.copy_alias M68K image, and the final program runs past the copied routine (one PSG write,
            no stop, virtual time advances). Two independent builds are byte-identical (generated M68K C) with equal aggregates.
  computed  the 68K computes every routine byte (EORI.L with a key): the work-RAM code is not a verbatim copy, the preparation
            terminates frontier_not_verbatim_copy with no alias and the program stays fail-closed at the RAM target.

usage: genesis_m68k_alias_build_test.py <segarecomp> <cc> <cxx> <source-root>
"""
import concurrent.futures
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile

cli, cc, cxx, root = sys.argv[1], sys.argv[2], sys.argv[3], pathlib.Path(sys.argv[4]).resolve()
sys.path.insert(0, str(root / "tools"))
import genesis_z80_fixture_rom as fx  # noqa: E402

RAM_TARGET = 0xFF0400
POINTER = 0xFF0100
STACK = 0xFFFE00
KEY = 0x5A5AA5A5
failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def program(computed):
    def assemble(spin_address):
        routine = bytes.fromhex("13FC009F00C00011")  # MOVE.B #$9F,$C00011 (PSG: one write)
        routine += bytes.fromhex("4EF9") + spin_address.to_bytes(4, "big")  # JMP spin.L (back to the cartridge)
        routine += bytes.fromhex("4E71") * ((32 - len(routine)) // 2)  # NOP padding: a 32-byte routine
        m = fx.M68k()
        m.w(0x23FC)
        prologue_fixup = len(m.words)
        m.w(0, 0)
        m.l(POINTER)  # MOVE.L #prologue,POINTER.L
        m.w(0x2079)
        m.l(POINTER)  # MOVEA.L POINTER.L,A0
        m.w(0x4ED0)  # JMP (A0): the prologue is reached only as an immutable-ROM AOT identity
        m.label("prologue")
        m.w(0x41FA)
        m.fixups.append((len(m.words), "routine:pcrel"))
        m.w(0)  # LEA routine(PC),A0
        m.w(0x43F9)
        m.l(RAM_TARGET)  # LEA RAM_TARGET.L,A1
        m.w(0x303C, len(routine) // 4 - 1)  # MOVE.W #n-1,D0
        m.label("copy")
        if computed:
            m.w(0x2218)  # MOVE.L (A0)+,D1
            m.w(0x0A81)
            m.l(KEY)  # EORI.L #KEY,D1
            m.w(0x22C1)  # MOVE.L D1,(A1)+
        else:
            m.w(0x22D8)  # MOVE.L (A0)+,(A1)+
        m.w(0x51C8)
        m.fixups.append((len(m.words), "copy:dbra"))
        m.w(0)  # DBRA D0,copy
        m.w(0x4EF9)
        m.l(RAM_TARGET)  # JMP RAM_TARGET.L
        m.label("spin")
        m.branch(0x60, "spin")
        payload = routine if not computed else bytes(b ^ ((KEY >> (24 - 8 * (i % 4))) & 0xFF) for i, b in enumerate(routine))
        m.data("routine", payload)
        code = bytearray(m.resolve())
        code[2 * prologue_fixup:2 * prologue_fixup + 4] = m.labels["prologue"].to_bytes(4, "big")
        return bytes(code), m.labels["spin"]
    _, spin = assemble(0)
    code, again = assemble(spin)
    assert again == spin
    rom = bytearray(fx.build_rom(code))
    rom[0:4] = STACK.to_bytes(4, "big")  # initial SSP inside work RAM
    return bytes(rom)


def build(rom_bytes, out, jobs=None):
    out.mkdir(parents=True, exist_ok=True)
    rom = out.parent / (out.name + ".md")
    rom.write_bytes(rom_bytes)
    command = [cli, "build", "--rom", str(rom), "--output", str(out), "--cc", cc, "--cxx", cxx, "--runtime-dir",
               str(root / "platforms" / "genesis"), "--optimize", "0", "--runtime-optimize", "1"]
    if jobs:
        command += ["--jobs", str(jobs)]
    done = subprocess.run(command, text=True, capture_output=True, timeout=1800)
    status = json.loads((out / "status.json").read_text()) if (out / "status.json").exists() else {}
    return done, status


def run_program(exe, budget=3_000_000):
    done = subprocess.run([str(exe), "--instruction-budget", str(budget)], text=True, capture_output=True, timeout=300,
                          env={"SEGARECOMP_SOUND_SUMMARY": "1", "PATH": "/usr/bin:/bin"})
    summary = re.search(r"SOUND_SUMMARY (\{.*\})", done.stderr)
    return done, (json.loads(summary.group(1)) if summary else {})


def executable(out):
    return next((p for p in out.iterdir() if p.stem == "game" and p.is_file()), None)


def tree_digest(directory):
    digest = hashlib.sha256()
    for path in sorted(p for p in pathlib.Path(directory).rglob("*") if p.is_file()):
        digest.update(path.relative_to(directory).as_posix().encode() + b"\0" + path.read_bytes())
    return digest.hexdigest()


def stable(status):
    out = json.loads(json.dumps(status))
    for key in ("emit_ms", "compile_ms", "materialize_ms", "units_compiled", "units_reused", "executable_bytes"):
        out.get("z80", {}).pop(key, None)
    return out


def fixed_point_ends(out):
    return re.findall(r"^z80 materialization: outcome=none .* end=(\w+)$", (out / "build.log").read_text(), re.M)


def main():
    with tempfile.TemporaryDirectory(prefix="segarecomp-m68k-alias-build-") as directory:
        tmp = pathlib.Path(directory)
        copy, computed = program(False), program(True)
        jobs = {"copy": (copy, None), "copy_j1": (copy, 1), "computed": (computed, None)}
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            futures = {name: pool.submit(build, rom, tmp / name, j) for name, (rom, j) in jobs.items()}
            results = {name: f.result() for name, f in futures.items()}

        # ---- verbatim copy: one alias, reported, and the final program runs past the copied routine ----
        done, status = results["copy"]
        prep = status.get("m68k_alias_preparation", {})
        check(done.returncode == 0 and status.get("status") == "ok", "the copy ROM builds through the normal route: %s" % done.stdout[-300:])
        check(prep == {"rounds": 1, "aliases": 1, "alias_bytes": 32, "termination": "route_advanced"},
              "one preparation round proposes exactly one 32-byte alias and the route advances: %s" % prep)
        check(fixed_point_ends(tmp / "copy") == ["guest_stop", "window_complete"],
              "pre-producer behaviour: the alias-less fixed point ends with a guest stop; the aliased one observes the whole window")
        m68k = status.get("executable_images", {}).get("m68k", {})
        check(m68k.get("images") == 2 and m68k.get("authority", {}).get("static_proof") == 1 and
              m68k.get("authority", {}).get("immutable_input") == 1 and
              m68k.get("producers") == {"genesis.cartridge": 1, "genesis.copy_alias": 1},
              "the alias is one static_proof image of producer genesis.copy_alias: %s" % m68k)
        log = (tmp / "copy" / "build.log").read_text()
        check("m68k alias preparation: rounds=1 aliases=1 alias_bytes=32 termination=route_advanced" in log, "build.log records the preparation aggregates")
        check(('"m68k_alias_preparation":' + json.dumps(prep, separators=(",", ":"))) in (tmp / "copy" / "status.json").read_text() and
              not re.search(r"0x|[0-9a-fA-F]{8,}", json.dumps(prep)), "status.json carries counts and the termination only (no address, no hash)")
        check(status.get("z80", {}).get("end") == "window_complete", "the confirming fixed point of the final program observes the whole window")
        for stage in ("z80-materialize", "m68k-alias-prepare", "link"):
            check("@stage %s begin" % stage in done.stdout and "@stage %s done" % stage in done.stdout, "stage %s reported" % stage)
        ran, summary = run_program(executable(tmp / "copy"))
        check(summary.get("psg_writes") == 1 and summary.get("result_kind") == 3 and summary.get("stop_class") == 0 and
              summary.get("virtual_frames", 0) > 0,
              "the final program executes the copied routine (one PSG write) and runs on to the instruction budget: %s" %
              {k: summary.get(k) for k in ("psg_writes", "result_kind", "stop_class", "virtual_frames")})
        check(not (tmp / "copy" / "obj").exists(), "intermediate objects removed")

        # ---- determinism ----
        _, status_j1 = results["copy_j1"]
        check(tree_digest(tmp / "copy" / "generated") == tree_digest(tmp / "copy_j1" / "generated"),
              "the final generated M68K C is byte-identical across independent builds and worker counts")
        check(stable(status) == stable(status_j1), "aggregates (preparation, images, fixed point) are identical across builds")

        # ---- not a verbatim copy: no alias, fail-closed ----
        done, status = results["computed"]
        prep = status.get("m68k_alias_preparation", {})
        check(done.returncode == 0 and status.get("status") == "ok", "the computed-code ROM still builds")
        check(prep == {"rounds": 0, "aliases": 0, "alias_bytes": 0, "termination": "frontier_not_verbatim_copy"},
              "computed work-RAM code is not a verbatim copy: frontier_not_verbatim_copy, no alias: %s" % prep)
        m68k = status.get("executable_images", {}).get("m68k", {})
        check(m68k.get("authority", {}).get("static_proof") == 0 and m68k.get("producers") == {"genesis.cartridge": 1},
              "no copy-alias image is reported")
        ran, summary = run_program(executable(tmp / "computed"))
        check(summary.get("result_kind") == 1 and summary.get("stop_class") == 5 and summary.get("psg_writes") == 0 and
              '"stop_class":"known_but_unemitted_target"' in ran.stdout,
              "the program stays fail-closed at the RAM target (known_but_unemitted_target, the routine never runs)")

    print("genesis m68k alias build: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
