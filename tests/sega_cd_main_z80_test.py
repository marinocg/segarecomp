#!/usr/bin/env python3
"""SEG-114 (ADR 0258): the Sega CD main-side Z80 - the Genesis generated-native Z80 attached to the main processor's sound area.

Project-authored fixture (tools/sega_cd_bios_hw_fixture.py cases 12-14; hand-assembled Z80 bytes, no commercial content), built by the product
`segarecomp sega-cd build`:
  - case 12: the Z80 writes a byte to sound RAM that the main program reads back: the build discovers the Z80 image epochs in its passes (no extra round
    for an epoch a pass already saw), compiles them, and the program observes the Z80's write; deterministic for every scheduler partition;
  - case 13: a Z80 that rewrites an opcode it will execute is isolated (muted, Genesis contract section 18); the main program is not stopped and reads the sound RAM as it stands;
  - case 14: an executable program meeting an image no build pass saw stops with the typed `z80_unsupported` (diagnostic 56, z80_unknown_image), never interpreting it;
  - case 15 (SEG-115, ADR 0259): image epochs - a program uploaded in two bus holds while the previous image runs and then restarted (empty hold of its own), a restart
    without change, and the same program again with different irrelevant Z80 data: the correct image is selected each time (no stale image, no new image for the repeat);
  - case 16 (SEG-115): an isolated Z80 is re-armed by the next image epoch and runs the registered image of a fresh reset (recoveries = 1);
  - case 17 (SEG-115, ADR 0259): a Z80 program that patches the first byte of a routine it calls (JR -> RET -> JR) runs the form each live byte sequence is, because the static analysis
    proved the store and the entry compiled the RET alternative; the control `--z80-alternatives 0` is the isolating mismatch;
  - control `--main-z80 0`: the previous rule - the read after the Z80 ran is the typed stop main_device_unsupported - and no `z80` object in the report.
usage: sega_cd_main_z80_test.py <segarecomp> <c-compiler> <source-root>
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

FIXTURE = ROOT / "tools" / "sega_cd_bios_hw_fixture.py"
EXE = ".exe" if os.name == "nt" else ""
SLOT, SLOT_AFTER = 0x60, 0x62
EPOCH_SLOT = 0x64
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def build(tmp, disc, bios, name, case, *extra):
    out = tmp / name
    done = subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(disc / "disc.cue"), "--bios", str(bios), "--output", str(out), "--runtime-dir", str(ROOT), "--cc", CC,
                           "--budget", "3000000", "--jobs", "4", "--cache-dir", str(tmp / "cache"), "--pass-set", "main:d7=%d" % case,
                           "--emit-arg", "--application-entry", "--emit-arg", "0xFF0000", "--emit-arg", "--copy-image", "--emit-arg", "main:0xFF0000:0x1280:4", *extra],
                          capture_output=True, text=True, timeout=1800)
    status = out / "status.json"
    return done, (json.loads(status.read_text()) if status.exists() else None), out / ("program" + EXE)


def run(exe, disc, bios, case, *extra):
    done = subprocess.run([str(exe), "--disc-dir", str(disc), "--bios", str(bios), "--instruction-budget", "3000000", "--set", "main:d7=%d" % case, *map(str, extra)],
                          capture_output=True, text=True, timeout=300)
    return json.loads(done.stdout)


def main():
    with tempfile.TemporaryDirectory(prefix="scd-z80-") as tmp:
        tmp = pathlib.Path(tmp)
        disc = tmp / "disc"
        common.build_disc(disc, "comm")
        subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(tmp / "fx")], check=True, capture_output=True, text=True)
        bios = tmp / "fx" / "bios.bin"

        done, status, exe = build(tmp, disc, bios, "c12", 12)
        check(done.returncode == 0 and status and status["status"] == "ok", "case 12: the build succeeds")
        z = status["z80"]
        check(z["enabled"] is True and z["images"] == 2, "case 12: two Z80 image epochs were discovered by the build passes (the earlier fixture upload and the handshake program)")
        check(status["rounds"] <= 4, "case 12: the Z80 images add at most one round to the previous build (%s rounds)" % status["rounds"])
        report = run(exe, disc, bios, 12)
        block = bytes.fromhex(report["result_block"])
        check(report["result"] == "idle", "case 12: the program runs to its end (%s)" % report["stop"])
        check(block[SLOT] == 0x5A, "case 12: the main program observes the byte the generated-native Z80 wrote (%02x)" % block[SLOT])
        zr = report["z80"]
        check(zr["state"] == "modelled" and zr["images"] == 2 and zr["unknown_epochs"] == 0 and zr["cycles"] > 0 and zr["fault"] == 0, "case 12: the Z80 executed and is healthy (%s)" % {k: v for k, v in zr.items() if k != "epoch_log"})
        for extra in (("--slice-seed", 3), ("--slice-seed", 11), ("--slice-ticks", 12345)):
            other = run(exe, disc, bios, 12, *extra)
            check(other["z80"]["digest"] == zr["digest"] and other["device_log"] == report["device_log"] and other["ticks"] == report["ticks"] and other["result_block"] == report["result_block"],
                  "case 12: identical Z80 state, device log and result for the partition %s" % (extra,))

        stop = run(exe, disc, bios, 14)
        check(stop["result"] == "stopped" and stop["stop"]["class"] == "z80_unsupported" and stop["stop"]["address"] == 56,
              "case 14: an image no pass saw is the typed stop z80_unsupported / z80_unknown_image (%s)" % stop["stop"])

        done, status, exe13 = build(tmp, disc, bios, "c13", 13)
        check(done.returncode == 0 and status and status["status"] == "ok" and status["z80"]["images"] == 2, "case 13: the build succeeds")
        r13 = run(exe13, disc, bios, 13)
        b13 = bytes.fromhex(r13["result_block"])
        check(r13["result"] == "idle" and r13["stop"]["class"] == "none", "case 13: a Z80 that rewrites its own opcode does not stop the main processor (%s)" % r13["stop"])
        check(r13["z80"]["state"] == "faulted" and r13["z80"]["fault"] == 57, "case 13: the Z80 is isolated by the structural mutation (z80_code_mismatch)")
        check(b13[SLOT] == 0x5A and b13[SLOT_AFTER] == 0x5A, "case 13: the main program reads sound RAM as it stands before and after the isolation")

        done, status, exe15 = build(tmp, disc, bios, "c15", 15)
        check(done.returncode == 0 and status and status["status"] == "ok", "case 15: the build succeeds")
        z15 = status["z80"]
        check(z15["images"] == 3, "case 15: three Z80 images (the fixture's earlier upload and the two programs; the repeated upload is not a new image) (%s)" % z15["images"])
        r15 = run(exe15, disc, bios, 15)
        b15 = bytes.fromhex(r15["result_block"])
        check(r15["result"] == "idle", "case 15: the program runs to its end (%s)" % r15["stop"])
        got = [b15[EPOCH_SLOT + 2 * i] for i in range(4)]
        check(got == [0x5A, 0x5B, 0x5C, 0x5D], "case 15: each epoch ran its own image - first image, restart of the two-hold upload, restart without change, same program again (%s)" % got)
        zr15 = r15["z80"]
        check(zr15["state"] == "modelled" and zr15["fault"] == 0 and zr15["recoveries"] == 0 and zr15["unknown_epochs"] == 0 and zr15["epochs"] == 5,
              "case 15: healthy, five epochs, no fault, no recovery (%s)" % {k: v for k, v in zr15.items() if k != "epoch_log"})
        for extra in (("--slice-seed", 5), ("--slice-ticks", 4321)):
            other = run(exe15, disc, bios, 15, *extra)
            check(other["z80"]["digest"] == zr15["digest"] and other["result_block"] == r15["result_block"], "case 15: identical Z80 state and result for the partition %s" % (extra,))

        done, status, exe16 = build(tmp, disc, bios, "c16", 16)
        check(done.returncode == 0 and status and status["status"] == "ok" and status["z80"]["images"] == 3, "case 16: the build succeeds with three images")
        r16 = run(exe16, disc, bios, 16)
        b16 = bytes.fromhex(r16["result_block"])
        zr16 = r16["z80"]
        check(r16["result"] == "idle" and zr16["state"] == "modelled" and zr16["fault"] == 0 and zr16["recoveries"] == 1 and zr16["fault_epoch"] == 2,
              "case 16: the isolated Z80 is re-armed by the next epoch (%s)" % {k: v for k, v in zr16.items() if k != "epoch_log"})
        check(b16[EPOCH_SLOT] == 0x5A and b16[EPOCH_SLOT + 2] == 0x6B, "case 16: the first image ran before its isolation and the registered second image ran after the re-arm (%02x %02x)" % (b16[EPOCH_SLOT], b16[EPOCH_SLOT + 2]))

        done, status, exe17 = build(tmp, disc, bios, "c17", 17)
        check(done.returncode == 0 and status and status["status"] == "ok", "case 17: the build succeeds")
        alt = status["z80"]["alternatives"]
        check(alt["images"] >= 1 and alt["entries"] >= 1 and alt["forms"] >= 1 and alt["images_dropped"] == 0 and alt["complete"] is False,
              "case 17: the static analysis proved a code write and an entry carries an alternative form (%s); completeness is never claimed" % (alt,))
        r17 = run(exe17, disc, bios, 17)
        b17 = bytes.fromhex(r17["result_block"])
        z17 = r17["z80"]
        check(r17["result"] == "idle" and z17["state"] == "modelled" and z17["fault"] == 0,
              "case 17: the Z80 ran through the patched routine without isolation (%s)" % {k: v for k, v in z17.items() if k != "epoch_log"})
        check(b17[EPOCH_SLOT] == 0x5A and b17[0x66] == 2,
              "case 17: JR form, RET form, JR form again, each executed as what the live bytes are (marker %02x, routine runs %d)" % (b17[EPOCH_SLOT], b17[0x66]))
        other = run(exe17, disc, bios, 17, "--slice-seed", 9)
        check(other["z80"]["digest"] == z17["digest"] and other["result_block"] == r17["result_block"], "case 17: identical Z80 state and result for another scheduler partition")
        done, status, exe17c = build(tmp, disc, bios, "c17-plain", 17, "--z80-alternatives", "0")
        check(done.returncode == 0 and status and status["z80"]["alternatives"]["forms"] == 0, "case 17 control: --z80-alternatives 0 compiles no alternative form")
        c17 = run(exe17c, disc, bios, 17)
        check(c17["z80"]["state"] == "faulted" and c17["z80"]["fault"] == 57 and bytes.fromhex(c17["result_block"])[EPOCH_SLOT] != 0x5A,
              "case 17 control: without alternatives the patched routine is the isolating structural mismatch")

        done, status, exe0 = build(tmp, disc, bios, "c12-off", 12, "--main-z80", "0")
        check(done.returncode == 0 and status and status["z80"]["enabled"] is False, "control: --main-z80 0 builds without the Z80")
        r0 = run(exe0, disc, bios, 12)
        check(r0["result"] == "stopped" and r0["stop"]["class"] == "main_device_unsupported" and r0["stop"]["address"] == 0xA00100 and "z80" not in r0,
              "control: the read after the Z80 ran is the previous typed stop and the report has no z80 object (%s)" % r0["stop"])
    if failures:
        print("sega_cd_main_z80_test: %d FAILED" % len(failures))
        return 1
    print("sega_cd_main_z80_test: OK")
    return 0


sys.exit(main())
