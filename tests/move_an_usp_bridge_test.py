#!/usr/bin/env python3
"""End-to-end synthetic C11 regressions for selected system-control forms.

The C++ pipeline test covers all selected decode/lift forms. This test uses
the initialized A7 source for MOVE An,USP, an immediate source for MOVE to
SR, and a straight-line MOVEQ; NOP prefix for NOP, all through the production
generate/compile/run driver. Each fixture is terminated by RESET (0x4E70),
the retained typed CPU frontier that bounds general_startup discovery.
"""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile


def run_bridge(binary: pathlib.Path, compiler: pathlib.Path, root: pathlib.Path,
               image: bytes, stem: str, entry: str) -> tuple[subprocess.CompletedProcess[str], str, dict]:
    driver = root / "tools" / "genesis_startup_bridge.py"
    with tempfile.TemporaryDirectory() as temporary_directory:
        temporary = pathlib.Path(temporary_directory)
        rom = temporary / f"{stem}.bin"
        rom.write_bytes(image)
        with tempfile.TemporaryDirectory(dir=root / "build", prefix=f"{stem}-") as output_directory:
            output = pathlib.Path(output_directory)
            full_report = temporary / f"{stem}-full.json"
            result = subprocess.run(
                [sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
                 "--rom", str(rom), "--entry", entry, "--mode", "synthetic",
                 "--out-dir", str(output), "--full-report-path", str(full_report)],
                text=True, capture_output=True, check=False, cwd=root)
            source = (output / "bridge.generated.c").read_text() if (output / "bridge.generated.c").exists() else ""
            try:
                full = json.loads(full_report.read_text())
            except (OSError, json.JSONDecodeError):
                full = {}
    return result, source, full


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    binary, compiler, root = (pathlib.Path(argument).resolve() for argument in sys.argv[1:])
    # MOVE A7,USP; RESET. RESET is a retained typed CPU frontier, ensuring the
    # first instruction is accepted, emitted, compiled, and executed.
    image = bytes((0x4E, 0x67, 0x4E, 0x70))
    result, source, full = run_bridge(binary, compiler, root, image, "move-an-usp", "00000c00")
    try:
        report = json.loads(result.stdout)
    except json.JSONDecodeError:
        report = {}
    if (result.returncode != 0 or result.stderr or
            report.get("result") != "stop" or report.get("stop_class") != "unsupported_cpu_form" or
            report.get("rom_sha256") != hashlib.sha256(image).hexdigest() or
            full.get("runtime", {}).get("usp") != "0x00ff0004" or
            "runtime->usp = runtime->a[7];" not in source or
            "runtime->pc += UINT32_C(2);" not in source or
            "genesis_route_access" in source[source.find("runtime->usp = runtime->a[7];"):source.find("runtime->usp = runtime->a[7];") + 100] or
            "runtime->sr" in source[source.find("runtime->usp = runtime->a[7];"):source.find("runtime->usp = runtime->a[7];") + 100]):
        sys.stderr.write("MOVE An,USP bridge regression failed\n")
        return 1

    # SEG-007-T088: MOVE #imm,SR; RESET. This selected immediate source proves
    # the production bridge emits, strict-C11 compiles, and executes the full
    # SR overwrite and the instruction's four-byte PC advance before reaching
    # the independent RESET frontier. The bytes and expected state are entirely
    # project-authored synthetic fixture material.
    sr_image = bytes((0x46, 0xFC, 0x12, 0x34, 0x4E, 0x70))
    sr_result, sr_source, sr_full = run_bridge(
        binary, compiler, root, sr_image, "move-to-sr", "00000c20")
    try:
        sr_report = json.loads(sr_result.stdout)
    except json.JSONDecodeError:
        sr_report = {}
    if (sr_result.returncode != 0 or sr_result.stderr or
            sr_report.get("result") != "stop" or sr_report.get("stop_class") != "unsupported_cpu_form" or
            sr_report.get("rom_sha256") != hashlib.sha256(sr_image).hexdigest() or
            sr_full.get("runtime", {}).get("sr") != "0x1234" or
            sr_full.get("runtime", {}).get("pc") != "0x00000c24" or
            "runtime->sr = (uint16_t)(UINT32_C(0x00001234));" not in sr_source or
            "runtime->pc += UINT32_C(4);" not in sr_source):
        sys.stderr.write("MOVE #imm,SR bridge regression failed\n")
        return 1

    # SEG-007-T114: MOVEQ #0,D0; NOP; RESET. Proves the production bridge
    # emits, strict-C11 compiles, and executes NOP as a pure two-byte PC
    # advance -- no register/SR/memory write of any kind -- inside a
    # straight-line retained block before the independent RESET frontier.
    # The entry (0x00000c40) plus MOVEQ (2) plus NOP (2) leaves pc at
    # 0x00000c44. All bytes and expected state are project-authored synthetic
    # fixture material.
    nop_image = bytes((0x70, 0x00, 0x4E, 0x71, 0x4E, 0x70))
    nop_result, nop_source, nop_full = run_bridge(
        binary, compiler, root, nop_image, "nop", "00000c40")
    try:
        nop_report = json.loads(nop_result.stdout)
    except json.JSONDecodeError:
        nop_report = {}
    nop_lowering = nop_source[nop_source.find("/* NOP */"):nop_source.find("/* NOP */") + 64]
    if (nop_result.returncode != 0 or nop_result.stderr or
            nop_report.get("result") != "stop" or nop_report.get("stop_class") != "unsupported_cpu_form" or
            nop_report.get("rom_sha256") != hashlib.sha256(nop_image).hexdigest() or
            nop_full.get("runtime", {}).get("pc") != "0x00000c44" or
            nop_full.get("runtime", {}).get("sr") != "0x0004" or
            nop_full.get("runtime", {}).get("d", [None])[0] != "0x00000000" or
            "/* NOP */" not in nop_source or
            "runtime->pc += UINT32_C(2);" not in nop_lowering or
            "runtime->sr" in nop_lowering or "runtime->d[" in nop_lowering or
            "runtime->a[" in nop_lowering or "genesis_route_access" in nop_lowering):
        sys.stderr.write("NOP bridge regression failed\n")
        return 1
    print("system-control bridge regressions: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
