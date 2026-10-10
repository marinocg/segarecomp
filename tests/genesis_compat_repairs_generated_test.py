#!/usr/bin/env python3
"""Generated-C coverage for the bounded Genesis compatibility repairs.

Synthetic, project-authored images only (no commercial data). Each image is emitted through the production
`emit-general-startup-bridge-c --reset-entry --immutable-rom-aot` route; a trailing `JMP (A0)` keeps the program on the
partial-program emission path the real cartridges use. Covered:

  * BSET on a routed-device absolute destination (Z80 bus request) is lowered, not rejected;
  * an IRQ6 vector slot that points into work RAM no longer rejects the build: no handler is rooted, the generated
    main records `irq6_vector_in_work_ram`, and the emitted program compiles as strict C11 and fail-closes at the
    first recognised VBlank interrupt (while the same image with a ROM handler still wires `irq6_handler_present`);
  * an IRQ6 vector slot into unmapped non-RAM space still rejects the build (the repair is not a blanket relaxation).
"""
import hashlib
import pathlib
import struct
import subprocess
import sys
import tempfile

BSET_Z80_BUSREQ = "08F9000000A11100"  # BSET #0,$00A11100.L
JMP_A0 = "4ED0"
NOP = "4E71"
DIVU_IMMEDIATE_ZERO = "80FC0000"  # DIVU #0,D0


def build_rom(code_hex: str, irq6_vector: int) -> bytes:
  rom = bytearray(0x400)

  def be32(offset: int, value: int) -> None:
    rom[offset:offset + 4] = struct.pack(">I", value)

  be32(0, 0x00FFFE00)
  for vector in range(1, 64):
    be32(vector * 4, 0x200)
  be32(0x78, irq6_vector)
  rom[0x100:0x110] = b"SEGA MEGA DRIVE "
  rom[0x110:0x120] = b"(C)TEST 2026.JAN"
  rom[0x120:0x150] = b"TEST".ljust(0x30)
  rom[0x150:0x180] = b"TEST".ljust(0x30)
  rom[0x180:0x18E] = b"GM 00000000-00"
  rom[0x1A0:0x1A8] = struct.pack(">II", 0, len(rom) - 1)
  rom[0x1A8:0x1B0] = struct.pack(">II", 0xFF0000, 0xFFFFFF)
  rom[0x1F0:0x1F3] = b"JUE"
  code = bytes.fromhex(code_hex)
  rom[0x200:0x200 + len(code)] = code
  return bytes(rom)


def emit(cli: str, rom: bytes, work: pathlib.Path, name: str, extra=()):
  rom_path = work / f"{name}.bin"
  rom_path.write_bytes(rom)
  out_path = work / f"{name}.c"
  run = subprocess.run(
      [cli, "emit-general-startup-bridge-c", "--rom", str(rom_path), "--reset-entry", "--rom-sha256",
       hashlib.sha256(rom).hexdigest(), "--immutable-rom-aot", *extra, "--generated-c-output", str(out_path)],
      text=True, capture_output=True)
  return run, out_path


def compile_and_run(compiler: str, root: str, source: pathlib.Path, work: pathlib.Path, name: str, patch=None, extra=()):
  text = source.read_text()
  if patch:
    text = patch(text)
  generated = work / f"{name}.patched.c"
  generated.write_text(text)
  runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
  program = work / name
  build = subprocess.run(
      [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", *extra, "-I", str(runtime_dir), str(generated),
       str(runtime_dir / "runtime.c"), "-o", str(program)], text=True, capture_output=True)
  assert build.returncode == 0, build.stderr
  return subprocess.run([str(program), "--instruction-budget", "20000"], text=True, capture_output=True)


def main() -> None:
  cli, compiler, root = sys.argv[1:]
  with tempfile.TemporaryDirectory() as temp:
    work = pathlib.Path(temp)

    # BSET on the Z80 bus-request register + a ROM IRQ6 handler (the handler is the entry stub itself).
    run, out = emit(cli, build_rom(BSET_Z80_BUSREQ + JMP_A0, 0x00000200), work, "rom_handler")
    assert run.returncode == 0, run.stderr + run.stdout
    text = out.read_text()
    assert "translation rejected" not in text[:200], text[:200]
    assert "runtime.irq6_handler_present = 1;" in text
    assert "irq6_vector_in_work_ram" not in text

    # Same image, IRQ6 vector in work RAM: the build proceeds, no handler is rooted, the flag is recorded.
    run, out = emit(cli, build_rom(BSET_Z80_BUSREQ + JMP_A0, 0x00FFFA7C), work, "ram_vector")
    assert run.returncode == 0, run.stderr + run.stdout
    text = out.read_text()
    assert "translation rejected" not in text[:200], text[:200]
    assert "runtime.irq6_vector_in_work_ram = 1;" in text
    assert "runtime.irq6_handler_present" not in text

    # Strict-C11 compile + deterministic run. Seed an admissible interrupt state (supervisor mode, mask open, VDP IE0,
    # the clock just before VBlank onset) exactly like the existing IRQ6 generated tests, then expect the fail-closed
    # interrupt stop instead of the silently dropped interrupt.
    anchor = "runtime.irq6_vector_in_work_ram = 1; "
    def seed(text: str) -> str:
      return text.replace(
          anchor,
          anchor + "runtime.sr = 0x2000; runtime.devices.vdp.registers[1] = 0x0020; "
                   "runtime.scheduler.master_ticks = GENESIS_NTSC_VBLANK_ONSET_TICK - 28U; ", 1)
    first = compile_and_run(compiler, root, out, work, "ram_vector_seeded", seed)
    second = compile_and_run(compiler, root, out, work, "ram_vector_seeded2", seed)
    assert first.stdout == second.stdout and first.returncode == second.returncode, "must be deterministic"
    assert '"stop_class":"unsupported_interrupt_or_scheduling_event"' in first.stdout, (
        first.returncode, first.stdout, first.stderr)
    assert '"diagnostic_category":"irq6_vector_in_work_ram"' in first.stdout, first.stdout

    # ADR 0097 amendment: a materialized work-RAM JMP thunk. The build-time producer validates the observed bytes through the
    # CPU-owned decoder (nothing in the runtime decodes RAM); the emitted entry carries a whole-instruction byte-identity guard.
    stub = "00FFFA7C"
    stub_bytes = "4EF900000200"  # JMP $00000200.L  (the program entry: a compiled ROM target)
    classify = subprocess.run([cli, "classify-ram-jump-thunk", stub, stub_bytes + "4E71"], text=True, capture_output=True)
    assert classify.returncode == 0 and classify.stdout.strip().lower() == stub_bytes.lower(), classify
    assert subprocess.run([cli, "classify-ram-jump-thunk", stub, "4E714E714E71"], capture_output=True).returncode == 1  # NOP: no
    assert subprocess.run([cli, "classify-ram-jump-thunk", stub, "4EB900000200"], capture_output=True).returncode == 1  # JSR: no
    assert subprocess.run([cli, "classify-ram-jump-thunk", "00FFFA7D", stub_bytes], capture_output=True).returncode == 1  # odd
    assert subprocess.run([cli, "classify-ram-jump-thunk", "00001000", stub_bytes], capture_output=True).returncode == 1  # ROM
    rom_with_ram_vector = build_rom(BSET_Z80_BUSREQ + JMP_A0, 0x00FFFA7C)
    run, out = emit(cli, rom_with_ram_vector, work, "thunk_bad", ("--ram-thunk", f"{stub}:4E714E714E71"))
    assert run.returncode != 0, "a non-JMP thunk descriptor must be rejected at build time"
    run, out = emit(cli, rom_with_ram_vector, work, "thunk", ("--ram-thunk", f"{stub}:{stub_bytes}"))
    assert run.returncode == 0, run.stderr + run.stdout
    text = out.read_text()
    assert "runtime.irq6_handler_present = 1;" in text and "irq6_vector_in_work_ram" not in text, "compiled stub is the IRQ6 handler"
    assert "work_ram[UINT32_C(64124)]" in text, "the thunk entry carries its work-RAM byte-identity guard"
    seed_irq = lambda t: t.replace("runtime.irq6_handler_present = 1; ",
        "runtime.irq6_handler_present = 1; runtime.sr = 0x2000; runtime.devices.vdp.registers[1] = 0x0020; "
        "runtime.scheduler.master_ticks = GENESIS_NTSC_VBLANK_ONSET_TICK - 28U; ", 1)
    def with_stub(bytes_hex):
      raw = bytes.fromhex(bytes_hex)
      writes = "".join(f"runtime.work_ram[{0xFA7C + i}] = UINT8_C({b}); " for i, b in enumerate(raw))
      return lambda t: seed_irq(t).replace("runtime.irq6_handler_present = 1; ", "runtime.irq6_handler_present = 1; " + writes, 1)
    # Matching live bytes: IRQ6 enters the stub (normal CPU exception entry), the stub's guarded native JMP reaches the compiled
    # ROM target, and the run continues past it (no IRQ6 or guard stop).
    good = compile_and_run(compiler, root, out, work, "thunk_good", with_stub(stub_bytes))
    assert "known_but_unemitted_target" not in good.stdout and "irq6_vector_in_work_ram" not in good.stdout, (good.stdout, good.stderr)
    assert good.returncode == 0, (good.returncode, good.stdout, good.stderr)
    # A rewritten stub (different JMP target) is NOT honoured: the guard fails closed with the typed unemitted-target stop.
    rewritten = compile_and_run(compiler, root, out, work, "thunk_rewritten", with_stub("4EF900000400"))
    assert "known_but_unemitted_target" in rewritten.stdout, (rewritten.returncode, rewritten.stdout, rewritten.stderr)

    # Edgeless direct JMP: TST.L D0; BNE.S over a statically foldable JMP whose target is outside every mapping, then JMP (A0).
    # The old emitter rejected the whole program ("incomplete C4 static edge"); now it builds, and reaching the JMP (D0 = 0 falls
    # through the BNE) stops with the typed known_but_unemitted_target. The taken-BNE path (JMP (A0)) is untouched.
    code = "4A80" + "6606" + "4EF9" + "00700000" + "4ED0"
    run, out = emit(cli, build_rom(code, 0x00000200), work, "edgeless_jmp")
    assert run.returncode == 0 and "translation rejected" not in out.read_text()[:200], run.stderr + run.stdout
    edgeless = compile_and_run(compiler, root, out, work, "edgeless_jmp_run")
    assert '"stop_class":"known_but_unemitted_target"' in edgeless.stdout, (edgeless.returncode, edgeless.stdout, edgeless.stderr)

    # DIVU #0,D0: the division arm is statically dead; an optimizing strict compile must not trip -Wdiv-by-zero, and the
    # guest takes the divide-by-zero exception path.
    run, out = emit(cli, build_rom(DIVU_IMMEDIATE_ZERO + JMP_A0, 0x00000200), work, "divu_zero")
    assert run.returncode == 0, run.stderr + run.stdout
    compile_and_run(compiler, root, out, work, "divu_zero_o2", extra=("-O2",))  # asserts the strict -O2 compile succeeds

    # An unmapped (non-RAM) IRQ6 vector is still a build-time rejection.
    run, out = emit(cli, build_rom(BSET_Z80_BUSREQ + JMP_A0, 0x00500000), work, "unmapped_vector")
    assert run.returncode != 0 or (out.exists() and "translation rejected" in out.read_text()[:200]), (
        run.returncode, run.stdout, run.stderr)
    assert not out.exists() or "irq6_vector_in_work_ram" not in out.read_text()


if __name__ == "__main__":
  main()
