#!/usr/bin/env python3
"""SEG-009-T007: the Sega PSG device library has no platform dependency (SEG-032 can consume it as a second consumer).

usage: sms_psg_dependency_test.py <product-root> <cc>

Checks that (a) its CMake target links nothing, (b) no source or header includes or names a Master System, Genesis, CPU
or runtime path, (c) it compiles standalone as strict C11 with only its own include directory, and (d) the dependency
direction is one-way: the SMS runtime links the device, the device never names the runtime.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[1]).resolve()
CC = sys.argv[2]
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
import host_cc  # noqa: E402
import sms_cc_cache  # noqa: E402
LIB = ROOT / "libs" / "device" / "sega" / "psg"
failed = []


def check(cond, message):
    if not cond:
        failed.append(message)
        print("FAIL:", message)


cmake = (LIB / "CMakeLists.txt").read_text(encoding="utf-8")
code = re.sub(r"#.*", "", cmake)
check("target_link_libraries" not in code, "the device target must not link any library")
check(not re.search(r"sms|genesis|master|z80|m68k|runtime|platforms", code, re.I), "CMake names a platform or CPU")
sources = sorted(p for p in LIB.rglob("*") if p.suffix in (".c", ".h"))
check(len(sources) >= 2, "device sources missing")
for path in sources:
    text = path.read_text(encoding="utf-8")
    for include in re.findall(r'#\s*include\s*[<"]([^>"]+)[>"]', text):
        check(include.startswith("segarecomp/device/sega/psg/") or "/" not in include and include.endswith(".h") and
              include in ("stddef.h", "stdint.h", "string.h"), "%s includes a non-device header %s" % (path.name, include))
    stripped = re.sub(r"/\*.*?\*/", "", text, flags=re.S)  # prose may mention consumers; code may not name them
    check(not re.search(r"\bSms[A-Z]|\bsms_|genesis|Genesis|Z80|z80|M68k|m68k", stripped), "%s names a platform or CPU" % path.name)
with tempfile.TemporaryDirectory() as tmp:
    obj = pathlib.Path(tmp) / "sn76489.o"
    r = sms_cc_cache.compile_object([CC, *host_cc.STRICT_C11, "-I", str(LIB / "include"), "-c",
                                     str(LIB / "src" / "sn76489.c"), "-o", str(obj)])
    check(r.returncode == 0, "the device does not compile standalone as strict C11: " + r.stderr[:400])
runtime = (ROOT / "platforms" / "master-system" / "runtime" / "CMakeLists.txt").read_text(encoding="utf-8")
check("segarecomp::device_psg" in runtime, "the SMS runtime must link the device library")
print("sms psg dependency: %s" % ("FAILED" if failed else "ok"))
sys.exit(1 if failed else 0)
