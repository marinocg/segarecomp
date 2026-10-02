#!/usr/bin/env python3
"""SEG-032-T001 (ADR 0072): whole-machine adversarial smoke of the pinned Genesis behavioural reference.

The expectations are authored from public documents (Sega Genesis Technical Overview v1.00 = GTO1, Charles
MacDonald's Genesis hardware notes = MCD1) and cross-checked against the ares MegaDrive source (ares); each names its
source. The project-authored `bus_reset_probe` fixture (tools/genesis_z80_fixture_rom.py) runs black-box through the
Genesis Plus GX libretro core, built locally from the pinned checkout in a temporary copy and driven by
tests/genesis_oracle/libretro_host.c (injection: ROM program; observation: the work-RAM result block and the audio
frame count).

Also checked: two runs are byte-identical (determinism), and the negative control fixture (the first run sequence
omitted) must fail the Z80-ran expectations, proving the smoke discriminates.

Skips cleanly (exit 0) unless SEGARECOMP_GENESIS_ORACLE_CHECKOUT holds the pinned checkouts; a wrong pin fails.
usage: genesis_z80_oracle_smoke_test.py [cc]
"""
import glob
import json
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE / "genesis_oracle"))
sys.path.insert(0, str(REPO / "tools"))
import pins  # noqa: E402
import genesis_z80_fixture_rom as fx  # noqa: E402

NAMES = ["ekeeke_Genesis-Plus-GX"]
MAX_FRAMES = 600

# (result offset, mask, expected, source). Offsets are the result-block layout of tools/genesis_z80_fixture_rom.py.
EXPECT = [
    (0x00, 0x01, 0x01, "GTO1 p.76: D8 of $A11100 reads 1 while the 68K has not been granted the Z80 bus (ares cpu/io.cpp)"),
    (0x01, 0x01, 0x00, "GTO1 p.76 SS4 step 2: D8 reads 0 once the bus is granted (ares cpu/io.cpp)"),
    (0x0A, 0x01, 0x01, "GTO1 p.76 / ares apu.hpp busgrantedCPU / GPGX mem68k.c zstate==3: BUSREQ is not acknowledged while /RESET is asserted"),
    (0x02, 0xFF, 0x06, "GTO1 p.91: the 68K writes the Z80 program into Z80 RAM at $A00000 while it holds the bus"),
    (0x04, 0xFF, 0x53, "MCD1 Z80 map: Z80 $8000-$FFFF reads 68K address (bank << 15 | A14-A0); bank register resets to 0 (GPGX genesis.c zbank; ares apu.cpp)"),
    (0x05, 0x83, 0x00, "YM2612 status: busy and timer flags clear at idle (ares/GPGX fm_read)"),
    (0x07, 0xFF, 0x00, "reset released while the 68K holds the bus does not run the Z80 (GPGX zstate==3; ares busreqLatch)"),
    (0x08, 0xFF, 0x53, "after BUSREQ release the Z80 restarts from PC 0 (reset state) and re-executes its startup (GPGX gen_zreset_w; ares APU::restart)"),
]
EQUAL = [(0x03, 0x06, "bus held: the Z80 counter does not advance between two reads (GTO1 p.76 acquire sequence)")]
DIFFER = [(0x03, 0x09, "the Z80 ran between two bus holds (counter changed or restarted)")]


def build_core(root, tmp):
    tree = pins.private_copy(root, "ekeeke_Genesis-Plus-GX", tmp)
    jobs = str(max(1, min(8, os.cpu_count() or 1)))
    subprocess.run(["make", "-j" + jobs, "-f", "Makefile.libretro"], cwd=tree, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=900)
    found = [p for p in glob.glob(str(tree / "genesis_plus_gx_libretro.*")) if p.endswith((".so", ".dylib"))]
    if len(found) != 1:
        raise AssertionError("libretro core not produced")
    return found[0]


def run(host, core, rom):
    proc = subprocess.run([host, core, rom, str(MAX_FRAMES), "genesis_plus_gx_system_hw=mega drive / genesis",
                           "genesis_plus_gx_region_detect=ntsc-u", "genesis_plus_gx_bios=disabled"],
                          text=True, capture_output=True, timeout=120)
    if proc.returncode not in (0, 4) or not proc.stdout.strip():
        raise AssertionError("host failed (%d): %s" % (proc.returncode, proc.stderr.strip()))
    return json.loads(proc.stdout)


def evaluate(result):
    block = bytes.fromhex(result["results"])
    rows = []
    for offset, mask, expected, source in EXPECT:
        value = block[offset]
        rows.append(("R%X %s" % (offset, source), (value & mask) == expected, "%02X" % value))
    for a, b, source in EQUAL:
        rows.append(("R%X==R%X %s" % (a, b, source), block[a] == block[b], "%02X/%02X" % (block[a], block[b])))
    ran = block[0x03] != 0 or block[0x04] == 0x53
    rows.append(("Z80 ran after the first release (marker/counter nonzero)", ran, "%02X/%02X" % (block[0x03], block[0x04])))
    rows.append(("handshake completed", bool(result["complete"]), ""))
    return rows


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else "cc"
    root = pins.checkout(NAMES)
    if root is None:
        print("SKIP: " + pins.skip_reason(NAMES))
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        host = os.path.join(tmp, "host")
        subprocess.run([compiler, "-std=c11", "-O1", "-Wall", "-Wextra", "-o", host,
                        str(HERE / "genesis_oracle" / "libretro_host.c"), "-ldl"], check=True)
        core = build_core(root, tmp)
        roms = {}
        for name in ("bus_reset_probe", "bus_reset_control"):
            roms[name] = os.path.join(tmp, name + ".md")
            pathlib.Path(roms[name]).write_bytes(fx.build(name))
        first, second = run(host, core, roms["bus_reset_probe"]), run(host, core, roms["bus_reset_probe"])
        control = run(host, core, roms["bus_reset_control"])
    failed = 0
    for label, ok, value in evaluate(first):
        print("%-6s %s %s" % ("ok" if ok else "FAIL", label, value))
        failed += 0 if ok else 1
    if first != second:
        print("FAIL: two runs differ")
        failed += 1
    control_rows = [r for r in evaluate(control) if r[0].startswith("R4") or r[0].startswith("Z80 ran")]
    if all(ok for _, ok, _ in control_rows):
        print("FAIL: negative control passed the Z80-ran expectations")
        failed += 1
    print("genesis z80 oracle smoke: %s" % ("FAILED (%d)" % failed if failed else "ok"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
