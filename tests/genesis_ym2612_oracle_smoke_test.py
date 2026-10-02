#!/usr/bin/env python3
"""SEG-032-T001 (ADR 0074): adapter smoke of the independent YM2612 oracle and the production candidate.

Builds Nuked-OPN2 (LGPL-2.1, test-only oracle) and ymfm (BSD-3-Clause, production candidate) in temporary copies of the
pinned checkouts, drives both with the same project-authored register trace (tests/genesis_oracle/trace.h) and checks:
  * injection and observation work for both (a varying output after key-on, a constant output without it: the YM2612 idle
    output is the DAC discontinuity offset, not zero, so "silent" means constant, see the audio artifact rules);
  * each is deterministic (two runs identical) and the key-on / no-key-on digests differ (the smoke discriminates);
  * both report the idle status as 0 and a busy indication after a write (Nuked: status bit 7; ymfm: the host-owned
    `ymfm_set_busy_end` callback, since ymfm leaves busy time to the host);
  * ymfm links with a C compiler driver and a ~9-line C shim: no C++ runtime library is needed (-fno-exceptions -fno-rtti).
The two implementations' sample streams are NOT compared here (different output scaling, multiplexing and sample phase);
the comparison policy belongs to SEG-032-T007. Skips cleanly unless SEGARECOMP_GENESIS_ORACLE_CHECKOUT holds the pinned
checkouts; a wrong or modified pin fails.
usage: genesis_ym2612_oracle_smoke_test.py [cc] [c++]
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "genesis_oracle"))
import pins  # noqa: E402

NAMES = ["nukeykt_Nuked-OPN2", "aaronsgiles_ymfm"]
OPN_SOURCES = ["ymfm_opn.cpp", "ymfm_adpcm.cpp", "ymfm_ssg.cpp"]


def run(exe, key_on):
    out = subprocess.run([exe, str(key_on)], text=True, capture_output=True, timeout=60, check=True).stdout.strip()
    return {k: v for k, v in re.findall(r"(\w+)=(\w+)", out)}


def main():
    cc = sys.argv[1] if len(sys.argv) > 1 else "cc"
    cxx = sys.argv[2] if len(sys.argv) > 2 else "c++"
    root = pins.checkout(NAMES)
    if root is None:
        print("SKIP: " + pins.skip_reason(NAMES))
        return 0
    probes = HERE / "genesis_oracle"
    with tempfile.TemporaryDirectory() as tmp:
        nuked = pins.private_copy(root, "nukeykt_Nuked-OPN2", tmp)
        ymfm = pins.private_copy(root, "aaronsgiles_ymfm", tmp)
        nuked_exe, ymfm_exe = os.path.join(tmp, "nuked"), os.path.join(tmp, "ymfm")
        subprocess.run([cc, "-std=c11", "-O1", "-I", str(nuked), "-I", str(probes), "-o", nuked_exe,
                        str(probes / "opn2_nuked_probe.c"), str(nuked / "ym3438.c")], check=True)
        objects = []
        for name in OPN_SOURCES + ["probe"]:
            source = ymfm / "src" / name if name != "probe" else probes / "ymfm_probe.cpp"
            obj = os.path.join(tmp, name + ".o")
            subprocess.run([cxx, "-std=c++14", "-O2", "-fno-exceptions", "-fno-rtti", "-I", str(ymfm / "src"),
                            "-I", str(probes), "-c", "-o", obj, str(source)], check=True)
            objects.append(obj)
        shim = os.path.join(tmp, "shim.o")
        subprocess.run([cc, "-std=c11", "-O1", "-c", "-o", shim, str(probes / "ymfm_shim.c")], check=True)
        subprocess.run([cc, "-o", ymfm_exe, *objects, shim], check=True)  # the C driver: no C++ runtime library
        rows = []
        for label, exe in (("Nuked-OPN2", nuked_exe), ("ymfm", ymfm_exe)):
            on, on2, off = run(exe, 1), run(exe, 1), run(exe, 0)
            rows += [
                (label + " deterministic", on == on2),
                (label + " varying output after key-on", int(on["distinct"]) > 100),
                (label + " constant output without key-on (idle is the DAC offset, not zero)", int(off["distinct"]) <= 2),
                (label + " digest discriminates key-on", on["fnv"] != off["fnv"]),
                (label + " idle status 0", on["status_idle"] == "0"),
                (label + " busy flag after write", on["busy_after_write"] == "1"),
            ]
    failed = 0
    for label, ok in rows:
        print("%-5s %s" % ("ok" if ok else "FAIL", label))
        failed += 0 if ok else 1
    print("genesis ym2612 oracle smoke: %s" % ("FAILED (%d)" % failed if failed else "ok"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
