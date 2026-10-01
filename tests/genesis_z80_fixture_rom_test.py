#!/usr/bin/env python3
"""SEG-032-T001: the synthetic Genesis Z80 fixture ROMs are reproducible and free of commercial content."""
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import genesis_z80_fixture_rom as fx  # noqa: E402


def main():
    subprocess.run([sys.executable, str(REPO / "tools" / "genesis_z80_fixture_rom.py"), "--check"], check=True)
    for name in sorted(fx.FIXTURES):
        a, b = fx.build(name), fx.build(name)
        assert a == b, "%s is not deterministic" % name
        assert a[0x100:0x110] == b"SEGA MEGA DRIVE ", "header"
        assert len(a) == fx.ROM_SIZE and a[0x18E:0x190] != b"\xff\xff"
    probe, control = fx.build("bus_reset_probe"), fx.build("bus_reset_control")
    assert probe != control
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([sys.executable, str(REPO / "tools" / "genesis_z80_fixture_rom.py"), "--out", tmp], check=True)
        assert sorted(p.name for p in pathlib.Path(tmp).iterdir()) == sorted(n + ".md" for n in fx.FIXTURES)
    print("genesis z80 fixture ROMs ok")


if __name__ == "__main__":
    main()
