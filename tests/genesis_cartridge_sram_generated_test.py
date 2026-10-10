#!/usr/bin/env python3
"""Generated-native coverage for the header-declared Genesis cartridge SRAM (ADR 0098).

Synthetic, project-authored images only (no commercial data).  Every program goes through the production
`emit-general-startup-bridge-c --reset-entry --immutable-rom-aot` route, is compiled as strict C11 and run with a finite
instruction budget.  Observable results are read from the full report (data registers, work RAM) or from the typed stop.

Covered: static absolute operands, address-register indirect and indexed operands reach the same runtime SRAM owner; the $A130F1
overlay (power-on ROM view, enable, disable, re-enable keeps the stored byte, write protect); odd-lane bounds; fail-closed
unsupported widths, lanes, control values, 16-bit and EEPROM headers; and the unchanged behaviour of a cartridge without SRAM.
"""
import base64
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile

SRAM_START, SRAM_END = 0x200001, 0x2003FF
ODD_ROM_BYTES = {0x200001: 0x3C, 0x200003: 0xC3, 0x200000: 0x11, 0x200002: 0x22}  # ROM beneath the extent (overlay images)


def finish(code_hex: str) -> str:
  """Appends a spin: A6 <- the address of a trailing JMP (A6), which jumps to itself.  An indirect terminal keeps the program on the
  partial-program emission path the real cartridges use (a plain BRA.S loop would take the narrower C3 route)."""
  spin = 0x200 + len(code_hex) // 2 + 6
  return code_hex + f"{0x2C7C:04X}{spin:08X}" + "4ED6"


LOOP = ""  # code bodies are wrapped by finish() in build_rom


def build_rom(code_hex: str, size: int = 0x400, header=(0xF8, 0x20, SRAM_START, SRAM_END), rom_bytes=None) -> bytes:
  rom = bytearray(size)

  def be32(offset: int, value: int) -> None:
    rom[offset:offset + 4] = struct.pack(">I", value)

  be32(0, 0x00FFFE00)
  for vector in range(1, 64):
    be32(vector * 4, 0x200)
  rom[0x100:0x110] = b"SEGA MEGA DRIVE "
  rom[0x110:0x120] = b"(C)TEST 2026.JAN"
  rom[0x120:0x150] = b"TEST".ljust(0x30)
  rom[0x150:0x180] = b"TEST".ljust(0x30)
  rom[0x180:0x18E] = b"GM 00000000-00"
  rom[0x1A0:0x1A8] = struct.pack(">II", 0, 0x3FF)
  rom[0x1A8:0x1B0] = struct.pack(">II", 0xFF0000, 0xFFFFFF)
  rom[0x1F0:0x1F3] = b"JUE"
  if header is not None:
    rom[0x1B0:0x1B4] = b"RA" + bytes([header[0], header[1]])
    rom[0x1B4:0x1BC] = struct.pack(">II", header[2], header[3])
  code = bytes.fromhex(finish(code_hex))
  rom[0x200:0x200 + len(code)] = code
  for address, value in (rom_bytes or {}).items():
    rom[address] = value
  return bytes(rom)


def emit(cli, rom, work, name, aot=True):
  rom_path = work / f"{name}.bin"
  rom_path.write_bytes(rom)
  out_path = work / f"{name}.c"
  run = subprocess.run(
      [cli, "emit-general-startup-bridge-c", "--rom", str(rom_path), "--reset-entry", "--rom-sha256",
       hashlib.sha256(rom).hexdigest(), *(["--immutable-rom-aot"] if aot else []), "--generated-c-output", str(out_path)],
      text=True, capture_output=True)
  assert run.returncode == 0, run.stderr + run.stdout
  assert "translation rejected" not in out_path.read_text()[:300], out_path.read_text()[:300]
  return out_path


def run_program(compiler, root, source, work, name, budget=3000):
  runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
  program = work / name
  build = subprocess.run(
      [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(runtime_dir), str(source),
       str(runtime_dir / "runtime.c"), "-o", str(program)], text=True, capture_output=True)
  assert build.returncode == 0, build.stderr
  report = work / f"{name}.full.json"
  run = subprocess.run([str(program), "--instruction-budget", str(budget), "--full-report-path", str(report)],
                       text=True, capture_output=True)
  sanitized = json.loads(run.stdout)
  full = json.loads(report.read_text()) if report.exists() else None
  return sanitized, full


def regs(full):
  return [int(value, 16) for value in full["runtime"]["d"]]


def ram(full):
  return base64.b64decode(full["runtime"]["work_ram_base64"])


def exercise(cli, compiler, root, work, name, rom, budget=3000):
  # A ROM that reaches into the $200000 window is megabytes long: the whole-ROM AOT universe would compile all of it, so only
  # the reachable code is emitted for those images.
  source = emit(cli, rom, work, name, aot=len(rom) <= 0x10000)
  first = run_program(compiler, root, source, work, name, budget)
  second = run_program(compiler, root, source, work, name + "_again", budget)
  assert first == second, "the run must be deterministic"
  return source, first[0], first[1]


def stop(sanitized):
  return sanitized["stop_class"], sanitized["diagnostic_category"]


# Instruction encodings (MC68000), all hand-assembled.
def move_b_imm_abs(value, address):  # MOVE.B #value,address.L
  return f"13FC{value:04X}{address:08X}"


def move_b_abs_dn(address, dn):  # MOVE.B address.L,Dn
  return f"{0x1039 | (dn << 9):04X}{address:08X}"


def move_b_dn_abs(dn, address):  # MOVE.B Dn,address.L
  return f"{0x13C0 | dn:04X}{address:08X}"


def movea_l_imm(an, value):  # MOVEA.L #value,An
  return f"{0x207C | (an << 9):04X}{value:08X}"


def move_b_imm_ind(value, an):  # MOVE.B #value,(An)
  return f"{0x10BC | (an << 9):04X}{value:04X}"


def move_b_ind_dn(an, dn):  # MOVE.B (An),Dn
  return f"{0x1010 | (dn << 9) | an:04X}"


def move_w_ind_dn(an, dn):  # MOVE.W (An),Dn
  return f"{0x3010 | (dn << 9) | an:04X}"


def main() -> None:
  cli, compiler, root = sys.argv[1:]
  with tempfile.TemporaryDirectory() as temp:
    work = pathlib.Path(temp)

    # 1. ROM ends below the extent (always mapped).  Static absolute, address-register indirect and indexed operands, the
    #    $A130F1 write protect, and odd-lane bounds (first and last declared address).
    code = "".join([
        move_b_imm_abs(0xA5, 0x200001), move_b_abs_dn(0x200001, 1),                      # static write, static read
        movea_l_imm(0, 0x200003), move_b_imm_ind(0x5A, 0), move_b_ind_dn(0, 2),          # (A0) write and read
        movea_l_imm(1, 0x200000), "7005", "13BC00770000", "16310000",                    # 0(A1,D0.W) -> $200005 write, read to D3
        move_b_imm_abs(0x11, 0x2003FF), move_b_abs_dn(0x2003FF, 6),                      # last declared address (D6)
        move_b_imm_abs(0x03, 0xA130F1), move_b_imm_abs(0xEE, 0x200001), move_b_abs_dn(0x200001, 4),  # protected: write ignored
        move_b_imm_abs(0x01, 0xA130F1), move_b_imm_abs(0xC3, 0x200001), move_b_abs_dn(0x200001, 5),  # unprotected again
        move_b_abs_dn(0x200007, 7),                                                      # never written: deterministic fill
        move_b_dn_abs(1, 0xFF0000), move_b_dn_abs(2, 0xFF0001), move_b_dn_abs(3, 0xFF0002), LOOP])
    _, sanitized, full = exercise(cli, compiler, root, work, "always_mapped", build_rom(code))
    assert sanitized["result"] == "runner_resource_limit", sanitized
    d = regs(full)
    assert d[1] & 0xFF == 0xA5 and d[2] & 0xFF == 0x5A and d[3] & 0xFF == 0x77 and d[6] & 0xFF == 0x11, d
    assert d[4] & 0xFF == 0xA5, "a write-protected store must be ignored"
    assert d[5] & 0xFF == 0xC3, "writes resume after the protect bit is cleared"
    assert d[7] & 0xFF == 0xFF, "unwritten SRAM holds the documented deterministic fill"
    assert ram(full)[:3] == bytes([0xA5, 0x5A, 0x77])

    # 2. ROM extends into the extent: ROM at power on, SRAM when mapped, ROM again (unmodified) when unmapped, and the stored
    #    byte survives a disable/enable cycle.
    overlay = "".join([
        movea_l_imm(0, 0x200001), move_b_ind_dn(0, 1),                                   # D1 <- ROM byte (SRAM hidden at power on)
        movea_l_imm(2, 0x200000), move_w_ind_dn(2, 7),                                   # D7 <- ROM word across the extent edge
        move_b_imm_abs(0x01, 0xA130F1), move_b_imm_abs(0xA5, 0x200001), move_b_abs_dn(0x200001, 2),  # D2 <- SRAM
        move_b_abs_dn(0x200003, 6),                                                      # D6 <- fresh SRAM (fill), not the ROM byte
        move_b_imm_abs(0x00, 0xA130F1), move_b_ind_dn(0, 3),                             # D3 <- ROM again
        move_b_imm_abs(0x01, 0xA130F1), move_b_ind_dn(0, 4), LOOP])                      # D4 <- the stored SRAM byte
    overlay_rom = build_rom(overlay, size=0x200400, header=(0xB8, 0x20, SRAM_START, SRAM_END), rom_bytes=ODD_ROM_BYTES)
    _, sanitized, full = exercise(cli, compiler, root, work, "overlay", overlay_rom, 4000)
    # Without the AOT universe the trailing JMP (A6) is an unresolved indirect target: that typed stop ends the run.
    assert stop(sanitized) == ("unresolved_indirect_target", "reached_unresolved_direct_edge"), sanitized
    d = regs(full)
    assert d[1] & 0xFF == 0x3C, "ROM is visible at power on"
    assert d[7] & 0xFFFF == 0x113C, "every width reads the ROM while the SRAM is hidden"
    assert d[2] & 0xFF == 0xA5 and d[6] & 0xFF == 0xFF, "enabled: the access reaches the SRAM, not the ROM"
    assert d[3] & 0xFF == 0x3C, "disabled again: the ROM byte is back"
    assert d[4] & 0xFF == 0xA5, "re-enabled: the stored byte survived"

    # 3. A write while the SRAM is hidden is an ordinary ROM write; it never reaches the SRAM or the ROM.
    hidden_write = movea_l_imm(0, 0x200001) + move_b_imm_ind(0x42, 0) + LOOP
    _, sanitized, _ = exercise(cli, compiler, root, work, "hidden_write",
                               build_rom(hidden_write, size=0x200400, header=(0xB8, 0x20, SRAM_START, SRAM_END)))
    assert stop(sanitized) == ("unsupported_memory_region", "rom_write_prohibited"), sanitized

    # 4. Fail-closed forms on a supported odd-lane SRAM: typed cartridge SRAM diagnostics, never rom_write_prohibited.
    cases = {
        "static_word": "33FC0001" + "00200000",                    # MOVE.W #1,$200000.L  (touches the extent, word)
        "static_opposite_lane": move_b_abs_dn(0x200002, 1),         # byte on the other lane
        "indirect_word": movea_l_imm(0, 0x200000) + move_w_ind_dn(0, 1),
        "indirect_opposite_lane_write": movea_l_imm(0, 0x200002) + move_b_imm_ind(1, 0),
        "control_undefined_bit": move_b_imm_abs(0x80, 0xA130F1),
    }
    for name, body in cases.items():
      _, sanitized, _ = exercise(cli, compiler, root, work, "rej_" + name, build_rom(body + LOOP))
      assert stop(sanitized) == ("unsupported_device_access", "unsupported_cartridge_sram_access"), (name, sanitized)

    # A WORD write at the even byte of the register pair is not the control register: it stays the unchanged unmapped stop.
    word = build_rom(movea_l_imm(0, 0xA130F0) + "30BC0001")
    _, sanitized, _ = exercise(cli, compiler, root, work, "rej_control_word", word)
    assert stop(sanitized) == ("unsupported_memory_region", "unmapped_data_access"), sanitized

    # 5. A declared 16-bit SRAM is recorded but unsupported: any access to its extent is a typed layout stop.
    wide = build_rom(move_b_abs_dn(0x200001, 1) + LOOP, header=(0xE0, 0x20, 0x200000, 0x2003FF))
    _, sanitized, _ = exercise(cli, compiler, root, work, "wide_sram", wide)
    assert stop(sanitized) == ("unsupported_device_access", "unsupported_cartridge_sram_layout"), sanitized

    # 6. Cartridges without a supported SRAM descriptor keep the PR #88 idle behaviour: $A130F1 accepts only bit 0 clear.
    for label, header in (("no_descriptor", None), ("eeprom", (0xE8, 0x40, 0x200001, 0x200001))):
      ok = build_rom(move_b_imm_abs(0x00, 0xA130F1) + LOOP, header=header)
      _, sanitized, _ = exercise(cli, compiler, root, work, label + "_idle", ok)
      assert sanitized["result"] == "runner_resource_limit", (label, sanitized)
      bad = build_rom(move_b_imm_abs(0x01, 0xA130F1) + LOOP, header=header)
      _, sanitized, _ = exercise(cli, compiler, root, work, label + "_enable", bad)
      assert stop(sanitized) == ("unsupported_memory_region", "unmapped_data_access"), (label, sanitized)
      # The extent of such a cartridge is not SRAM: a store there is still the ordinary ROM/unmapped failure.
      store = build_rom(movea_l_imm(0, 0x200001) + move_b_imm_ind(1, 0) + LOOP, header=header)
      _, sanitized, _ = exercise(cli, compiler, root, work, label + "_store", store)
      assert stop(sanitized)[1] == "rom_write_prohibited", (label, sanitized)

    # 7. A malformed descriptor creates no device and is not treated as SRAM.
    malformed = build_rom(movea_l_imm(0, 0x200001) + move_b_imm_ind(1, 0) + LOOP, header=(0xF8, 0x20, 0x203FFF, 0x200001))
    _, sanitized, _ = exercise(cli, compiler, root, work, "malformed", malformed)
    assert stop(sanitized)[1] == "rom_write_prohibited", sanitized


if __name__ == "__main__":
  main()
