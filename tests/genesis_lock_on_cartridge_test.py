#!/usr/bin/env python3
"""ADR 0268: the declared lock-on pass-through cartridge (`build --mapper lock_on`, emitter `--genesis-lock-on`).

Drives the production compile-and-run driver (tools/genesis_startup_bridge.py) on project-authored images mapped at
address 0. A declared lock-on base cartridge (2 MiB) is never mirrored into $200000-$3FFFFF: a DATA read in that empty
slot answers $FF per byte, where the same image without the declaration reads its own mirror. A 4 MiB composite reads the
attached cartridge's bytes there. Negative fixtures: a write into the empty slot and an image of another size fail
closed.
"""
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile

LEA_A1 = lambda address: bytes((0x43, 0xF9)) + address.to_bytes(4, "big")
MOVE_L_A1_POSTINC_D0 = bytes((0x20, 0x19))
MOVE_L_D0_TO_A1 = bytes((0x22, 0x80))
RESET = bytes((0x4E, 0x70))
SLOT = 0x200000


def image(size: int, code: bytes, upper: bytes = b"") -> bytes:
    rom = bytearray(size)
    rom[: len(code)] = code
    rom[0x100:0x104] = b"BASE"
    if upper:
        rom[SLOT + 0x100 : SLOT + 0x100 + len(upper)] = upper
    return bytes(rom)


def run(driver, binary, compiler, rom, root, work, name, lock_on, expect_ok=True):
    out_dir = pathlib.Path(tempfile.mkdtemp(dir=root / "build", prefix=f"lockon-{name}-"))
    full_path = work / f"{name}-full.json"
    command = [sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler), "--rom", str(rom),
               "--entry", "00000000", "--mapping-base", "00000000", "--mode", "synthetic", "--out-dir", str(out_dir),
               "--full-report-path", str(full_path)]
    if lock_on:
        command += ["--emitter-flag=--genesis-lock-on"]
    result = subprocess.run(command, text=True, capture_output=True, cwd=root)
    if not expect_ok:
        return result
    assert result.returncode == 0, (name, result.stderr[-2000:])
    return json.loads(result.stdout), json.loads(full_path.read_text()), (out_dir / "bridge.generated.c").read_text()


def main() -> int:
    binary, compiler, root = (pathlib.Path(value).resolve() for value in sys.argv[1:])
    driver = root / "tools" / "genesis_startup_bridge.py"
    read_slot = LEA_A1(SLOT + 0x100) + MOVE_L_A1_POSTINC_D0 + RESET
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)
        base = work / "base.bin"
        base.write_bytes(image(0x200000, read_slot))
        # Without the declaration: the ordinary power-of-two mirror (ADR 0049) shows the base's own bytes in the slot.
        sanitized, full, source = run(driver, binary, compiler, base, root, work, "mirror", False)
        assert full["runtime"]["d"][0] == "0x42415345", full["runtime"]["d"]  # "BASE"
        assert "cartridge_empty_upper_slot" not in source
        # Declared lock-on, empty slot: no mirror row, the documented empty-slot value.
        sanitized, full, source = run(driver, binary, compiler, base, root, work, "empty", True)
        assert sanitized["stop_class"] == "unsupported_cpu_form", sanitized  # the trailing RESET
        assert full["runtime"]["d"][0] == "0xffffffff", full["runtime"]["d"]
        assert "runtime.owned_region_count = UINT32_C(1);" in source
        assert "runtime.cartridge_empty_upper_slot = 1;" in source
        # Declared lock-on composite: the attached cartridge's bytes, no empty slot.
        composite = work / "composite.bin"
        composite.write_bytes(image(0x400000, read_slot, b"ATCH"))
        sanitized, full, source = run(driver, binary, compiler, composite, root, work, "attached", True)
        assert full["runtime"]["d"][0] == "0x41544348", full["runtime"]["d"]  # "ATCH"
        assert "cartridge_empty_upper_slot" not in source
        # Negative: a write into the empty slot is not a device: it fails closed (the slot is read-only).
        writer = work / "writer.bin"
        writer.write_bytes(image(0x200000, LEA_A1(SLOT + 0x100) + MOVE_L_D0_TO_A1 + RESET))
        sanitized, full, source = run(driver, binary, compiler, writer, root, work, "write", True)
        assert sanitized["stop_class"] == "unsupported_memory_region", sanitized
        # Negative: the declaration needs a 2 MiB base (optionally + a 2 MiB attached cartridge).
        odd = work / "odd.bin"
        odd.write_bytes(image(0x100000, read_slot))
        result = run(driver, binary, compiler, odd, root, work, "odd", True, expect_ok=False)
        assert result.returncode != 0 and "--genesis-lock-on needs a 2 MiB base cartridge image" in result.stderr, result.stderr[-500:]
        # The attached cartridge's header-declared SRAM, reached by ABSOLUTE operands: the translation-time gate and the runtime owner
        # use the same lock-on rule (map it through $A130F1, store, load back).
        sys.path.insert(0, str(root / "tests"))
        import genesis_cartridge_sram_generated_test as t  # noqa: E402  (project-authored SRAM program helpers)
        code = t.move_b_imm_abs(1, 0xA130F1) + t.move_b_imm_abs(0xA5, 0x200001) + t.move_b_abs_dn(0x200001, 1)
        rom = bytearray(t.build_rom(code, size=0x400000, header=None))
        rom[0x2001B0:0x2001B4] = b"RA" + bytes([0xF8, 0x20])
        rom[0x2001B4:0x2001BC] = struct.pack(">II", 0x200001, 0x2003FF)
        sram_image = work / "attached_sram.bin"
        sram_image.write_bytes(bytes(rom))
        source = work / "attached_sram.c"
        r = subprocess.run([str(binary), "emit-general-startup-bridge-c", "--rom", str(sram_image), "--reset-entry", "--rom-sha256",
                            hashlib.sha256(bytes(rom)).hexdigest(), "--genesis-lock-on", "--generated-c-output", str(source)],
                           text=True, capture_output=True)
        assert r.returncode == 0 and "genesis_cartridge_sram_install" in source.read_text(), r.stderr[-500:]
        stop, full = t.run_program(str(compiler), root, source, work, "attached_sram", 4000)
        assert full and full["runtime"]["d"][1].endswith("a5"), (stop, full and full["runtime"]["d"])
        # without the declaration the attached header is not consulted: the absolute store is an ordinary ROM write (rejected)
        r = subprocess.run([str(binary), "emit-general-startup-bridge-c", "--rom", str(sram_image), "--reset-entry", "--rom-sha256",
                            hashlib.sha256(bytes(rom)).hexdigest(), "--generated-c-output", str(work / "undeclared.c")], text=True, capture_output=True)
        assert r.returncode != 0 and "rom_write_prohibited" in (r.stdout + r.stderr), (r.returncode, r.stdout[-300:])
    print("genesis_lock_on_cartridge_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
