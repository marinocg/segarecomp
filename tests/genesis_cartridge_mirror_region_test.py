#!/usr/bin/env python3
"""SEG-021-T041 / ADR 0049: cartridge ROM mirroring for reads past a power-of-two ROM chip.

Drives the production compile-and-run driver (tools/genesis_startup_bridge.py) on project-authored images mapped at
address 0. A 64 KiB image (a power-of-two chip mapped at the cartridge window base) is mirrored across the rest of
the 4 MiB cartridge window, so a runtime-computed read at `chip_size * k + offset` returns the byte at `offset`
(the upper cartridge address lines are not decoded by the chip; see ADR 0049). Negative fixtures: a non-power-of-two
image and a read beyond the window get no mirror and keep failing closed.
"""
import json
import pathlib
import subprocess
import sys
import tempfile

LEA_A1 = lambda address: bytes((0x43, 0xF9)) + address.to_bytes(4, "big")
MOVE_L_A1_POSTINC_D0 = bytes((0x20, 0x19))
MOVE_L_A1_POSTINC_D1 = bytes((0x22, 0x19))
RESET = bytes((0x4E, 0x70))
DATA0 = bytes((0x12, 0x34, 0x56, 0x78))
DATA1 = bytes((0x9A, 0xBC, 0xDE, 0xF0))


def image(size: int, read_address: int) -> bytes:
    code = LEA_A1(read_address) + MOVE_L_A1_POSTINC_D0 + MOVE_L_A1_POSTINC_D1 + RESET
    data_offset = len(code)  # 0x0C
    body = code + DATA0 + DATA1
    assert data_offset == 0x0C
    return body + bytes(size - len(body))


def run(driver, binary, compiler, rom, root, work, name):
    out_dir = pathlib.Path(tempfile.mkdtemp(dir=root / "build", prefix=f"mirror-{name}-"))
    full_path = work / f"{name}-full.json"
    result = subprocess.run(
        [sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler), "--rom", str(rom),
         "--entry", "00000000", "--mapping-base", "00000000", "--mode", "synthetic", "--out-dir", str(out_dir),
         "--full-report-path", str(full_path)], text=True, capture_output=True, cwd=root)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout), json.loads(full_path.read_text()), (out_dir / "bridge.generated.c").read_text()


def main() -> int:
    binary, compiler, root = (pathlib.Path(value).resolve() for value in sys.argv[1:])
    driver = root / "tools" / "genesis_startup_bridge.py"
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)
        for name, size, read_address in (("mirror1", 0x10000, 0x1000C), ("mirror2", 0x10000, 0x2000C),
                                         ("mirror-last", 0x10000, 0x3F000C)):
            rom = work / f"{name}.bin"
            rom.write_bytes(image(size, read_address))
            sanitized, full, source = run(driver, binary, compiler, rom, root, work, name)
            runtime = full["runtime"]
            assert sanitized["stop_class"] == "unsupported_cpu_form", (name, sanitized)  # the trailing RESET
            assert runtime["d"][0] == "0x12345678" and runtime["d"][1] == "0x9abcdef0", (name, runtime["d"])
            assert source.count("genesis_owned_region_data_0") >= 63, name  # 63 mirrors share the one array
            assert "runtime.owned_region_count = UINT32_C(64);" in source, name
            assert source.count("static const uint8_t genesis_owned_region_data_") == 1, name  # no duplicated data
        # Negative: a non-power-of-two image is not mirrored; the same read fails closed.
        rom = work / "odd.bin"
        rom.write_bytes(image(0x12000, 0x2000C))
        sanitized, full, source = run(driver, binary, compiler, rom, root, work, "odd")
        assert sanitized["stop_class"] != "unsupported_cpu_form", sanitized
        assert "runtime.owned_region_count = UINT32_C(1);" in source
        # Negative: a power-of-two chip below the mirror granularity is not mirrored either.
        rom = work / "small.bin"
        rom.write_bytes(image(0x1000, 0x1000C))
        sanitized, _, source = run(driver, binary, compiler, rom, root, work, "small")
        assert sanitized["stop_class"] != "unsupported_cpu_form", sanitized
    print("genesis_cartridge_mirror_region_test: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
