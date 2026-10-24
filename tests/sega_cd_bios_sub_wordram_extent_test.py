#!/usr/bin/env python3
"""Disc extents in the sub processor's linear 2M Word RAM view ($080000-$0BFFFF; contract sections 4, 15 and 18).

The companion of sega_cd_bios_wordram_extent_test.py for the sub processor. A project-authored "BIOS" (tools/sega_cd_bios_boot_fixture.py,
cases 20/22/23) places a project-authored disc IP (main) and SP (sub). The IP copies a routine from its own image into 2M Word RAM (main
owns it after reset; $200400 in main's view is $080400 in the sub's) and, unless the case says otherwise, gives Word RAM to the sub (DMNA).
The SP raises the sub flag the BIOS waits for, then waits until it owns Word RAM (RET = 0) and calls the routine in place - the shape of an
application whose sub code is loaded into Word RAM. The routine image is declared to the emitter as an extent of the fixture disc mapped at
its sub Word RAM base. All bytes are project-authored; the routine reports through the sub's own COMSTAT registers. Checked:
  - valid execution: the routine runs as compiled, byte-verified entries; a data write into its own image (a byte that is no instruction)
    does not invalidate it and is read back; the routine's tail runs after that write and the SP regains control (the run ends idle);
  - owner rule: when main still owns 2M Word RAM (case 22: the SP calls the routine without waiting), the sub's fetch fails closed
    (code_bytes_mismatch at the routine, nothing retires there) - the declared extent never overrides the hardware ownership;
  - live-byte guard: a corrupted live byte of the copied routine stops code_bytes_mismatch before it retires (case 23);
  - authority: without the extent the routine is entry_not_compiled; an extent whose disc bytes differ never runs; extents that straddle
    either end of the view, lie in the 1M bank window, name the main processor at a sub Word RAM base or the sub at a main Word RAM base are
    refused by the emitter (disc_image_invalid); an extent ending exactly at $0BFFFF is accepted;
  - determinism over eight deadline partitions and a repeat.

usage: sega_cd_bios_sub_wordram_extent_test.py <segarecomp> <c-compiler> <source-root>
"""
import json
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))
import sega_cd_common as common  # noqa: E402
import sega_cd_fixture_disc as F  # noqa: E402

FIXTURE = ROOT / "tools" / "sega_cd_bios_boot_fixture.py"
DRIVER = ROOT / "tools" / "sega_cd_bios_build.py"
RESULT = 0xFFC000
MAIN_ROUTINE = 0x200400   # main's view of the routine in 2M Word RAM
SUB_ROUTINE = 0x080400    # the sub's view of the same bytes
GA_MAIN_MODE = 0xA12003   # main's memory-mode register (DMNA = bit 1)
GA_SUB = 0xFF8000
SUB_MODE = GA_SUB + 0x03  # RET = bit 0 (0 while the sub owns 2M Word RAM)
COMSTAT = lambda n: GA_SUB + 0x20 + 2 * n  # noqa: E731
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def run(exe, disc, bios, *extra, budget=3000000):
    out = subprocess.run([str(exe), "--disc-dir", str(disc), "--bios", str(bios), "--instruction-budget", str(budget), *map(str, extra)],
                         capture_output=True, text=True, timeout=common.RUN_TIMEOUT)
    return out.returncode, (json.loads(out.stdout) if out.stdout.strip() else {})


def build(tmp, name, disc, bios, extra):
    out = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(disc / "disc.cue"), "--bios",
                          str(bios), "--work", str(tmp / name), "--pass-set", "main:d7=20", "--pass-set", "sub:a6=20", *extra],
                         capture_output=True, text=True, timeout=1800)
    return json.loads(out.stdout) if out.stdout.strip() else {"ok": False}


def emit(tmp, disc, bios, *extra):
    out = subprocess.run([SEGARECOMP, "sega-cd", "emit", "--cue", str(disc / "disc.cue"), "--bios", str(bios), "--out", str(tmp / "emit.c"), *extra],
                         capture_output=True, text=True, timeout=300)
    return out.returncode, (json.loads(out.stdout) if out.stdout.strip() else {})


def routine_code():
    """The sub's Word RAM routine (assembled for SUB_ROUTINE): COMSTAT2 marker, a data write into its own image, COMSTAT3 marker, the
    data word read back into COMSTAT5, return; then one data word (no instruction) that the write changes."""
    def make(data):
        r = F.Asm(SUB_ROUTINE)
        r.move("w", F.imm(0x1234), F.absl(COMSTAT(2)))
        r.move("w", F.imm(0xABCD), F.absl(data))
        r.move("w", F.imm(0x5678), F.absl(COMSTAT(3)))
        r.move("w", F.absl(data), F.absl(COMSTAT(5)))
        r.rts()
        r.label("data")
        r.w(0)
        return r
    data = make(0).labels["data"]
    r = make(data)
    code = r.assemble()
    assert r.labels["data"] == data
    return code


def application_sp():
    """The disc SP (sub, entered at $6000 by the sub program after the BIOS's go): marker, the sub flag the BIOS waits for, then - except in
    case 22 - wait until the sub owns Word RAM (RET = 0), call the routine in place, mark the return and return."""
    s = F.Asm(F.SP_BASE)
    s.move("w", F.imm(0x2222), F.absl(COMSTAT(1)))
    s.move("b", F.imm(0x02), F.absl(GA_SUB + 0x0F))
    s.move("l", F.a(6), F.d(7))
    s.immop("cmp", "w", 22, F.d(7))
    s.bcc("eq", "go")
    s.label("wait_owner")
    s.btst(0, F.absl(SUB_MODE))
    s.bcc("ne", "wait_owner")
    s.label("go")
    s.jsr(F.absl(SUB_ROUTINE))
    s.move("w", F.imm(0x0D0E), F.absl(COMSTAT(4)))
    s.rts()
    return s.assemble()


def application_ip(routine):
    """The disc IP (main): boot stub, the entry, the copy of the routine into Word RAM, case 23 corrupts a live byte, case 22 keeps
    Word RAM, otherwise it is given to the sub (DMNA), then main stops; and the routine's image."""
    def make(source):
        ip = F.Asm(F.IP_BASE)
        ip.move("w", F.imm(0x600D), F.absl(RESULT + 0x40))
        ip.bra("app")
        ip.nop()
        ip.label("app")
        ip.move("w", F.imm(0xBEEF), F.absl(RESULT + 0x42))
        ip.move("l", F.imm(source), F.a(0))
        ip.move("l", F.imm(MAIN_ROUTINE), F.a(1))
        ip.move("w", F.imm(len(routine) // 2 - 1), F.d(0))
        ip.label("copy")
        ip.move("w", ("inc", 0), ("inc", 1))
        ip.dbra(0, "copy")
        ip.immop("cmp", "w", 23, F.d(7))
        ip.bcc("ne", "not23")
        ip.move("b", F.imm(0x5A), F.absl(MAIN_ROUTINE + 1))   # a live instruction byte differs from the declared extent
        ip.label("not23")
        ip.immop("cmp", "w", 22, F.d(7))
        ip.bcc("eq", "keep")
        ip.move("b", F.imm(0x02), F.absl(GA_MAIN_MODE))       # 2M, DMNA = 1: Word RAM to the sub
        ip.label("keep")
        ip.stop(0x2700)
        ip.label("routine_src")
        for i in range(0, len(routine), 2):
            ip.w(int.from_bytes(routine[i:i + 2], "big"))
        return ip
    first = make(0)
    ip = make(first.labels["routine_src"])
    assert ip.labels["routine_src"] == first.labels["routine_src"]
    return ip.assemble(), ip.labels["app"], ip.labels["routine_src"] - F.IP_BASE


def main():
    with tempfile.TemporaryDirectory(prefix="scd-subwordram-") as tmp:
        tmp = pathlib.Path(tmp)
        disc = tmp / "disc"
        described = common.build_disc(disc, "bootarea")
        ip_offset, sp_offset = described["extents"]["ip"][0], described["extents"]["sp"][0]
        routine = routine_code()
        ip_bytes, entry, routine_at = application_ip(routine)
        sp_bytes = application_sp()
        assert len(ip_bytes) <= sp_offset - ip_offset and len(sp_bytes) <= 0x400
        track = bytearray((disc / "track01.bin").read_bytes())
        track[ip_offset:ip_offset + len(ip_bytes)] = ip_bytes
        track[sp_offset:sp_offset + len(sp_bytes)] = sp_bytes
        (disc / "track01.bin").write_bytes(bytes(track))
        (tmp / "ip.bin").write_bytes(ip_bytes)
        (tmp / "sp.bin").write_bytes(sp_bytes)
        subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(tmp / "fx"), "--boot-ip", str(tmp / "ip.bin"), "--boot-sp", str(tmp / "sp.bin"),
                        "--boot-ip-alt", str(tmp / "ip.bin"), "--entry", hex(entry)], check=True, capture_output=True)
        bios = tmp / "fx" / "bios.bin"
        ip_image = ["--disc-image", "main:%d:0xFF0000:%d" % (ip_offset, len(ip_bytes))]
        sp_image = ["--disc-image", "sub:%d:0x6000:%d" % (sp_offset, len(sp_bytes))]
        wr_image = ["--disc-image", "sub:%d:0x%X:%d" % (ip_offset + routine_at, SUB_ROUTINE, len(routine))]

        summary = build(tmp, "w", disc, bios, ip_image + sp_image + wr_image)
        check(summary.get("ok") is True, "the program with a sub Word RAM extent builds (%s)" % (summary.get("failure") or summary.get("rounds")))
        if not summary.get("ok"):
            return 1
        exe = pathlib.Path(summary["program"])
        wr = [i for i in summary["emit"]["images"] if i.get("processor") == "sub" and i.get("start") == SUB_ROUTINE]
        check(len(wr) == 1 and wr[0].get("compiled", 0) >= 4, "the Word RAM extent is an admitted sub image with compiled entries (%s)" % wr)

        def case(program, number, *extra):
            return run(program, disc, bios, "--set", "main:d7=%d" % number, "--set", "sub:a6=%d" % number, *extra)

        # ---- valid execution: the sub owns 2M Word RAM, runs the routine in place, writes its own data word, returns ----
        code, a = case(exe, 20)
        comstat = a.get("ga", {}).get("comstat", [])
        check(code == 0 and a.get("result") == "idle" and a["stop"]["class"] == "none", "case 20: the run ends idle, no typed stop (%s %s)" % (code, a.get("stop")))
        check(comstat[1:6] == [0x2222, 0x1234, 0x5678, 0x0D0E, 0xABCD],
              "case 20: SP marker, the routine's first marker, its tail after the data write, the SP's return marker and the data word read back (%s)" % comstat)

        # ---- owner rule: main still owns 2M Word RAM when the SP calls the routine ----
        code, b = case(exe, 22)
        stop = b.get("stop", {})
        comstat = b.get("ga", {}).get("comstat", [])
        check(code == 10 and stop.get("class") == "code_bytes_mismatch" and stop.get("cpu") == 1 and stop.get("pc") == SUB_ROUTINE,
              "case 22: while main owns 2M Word RAM the sub's fetch of the routine fails closed (%s)" % stop)
        check(comstat[2:6] == [0, 0, 0, 0] and b["cpus"][1]["pc"] == SUB_ROUTINE and b["ga"]["ret"] == 1,
              "case 22: nothing of the routine retired, the result state is unchanged and main still owns Word RAM (%s)" % comstat)

        # ---- live-byte guard ----
        code, d = case(exe, 23)
        stop = d.get("stop", {})
        comstat = d.get("ga", {}).get("comstat", [])
        check(code == 10 and stop.get("class") == "code_bytes_mismatch" and stop.get("cpu") == 1 and stop.get("pc") == SUB_ROUTINE,
              "case 23: a corrupted live byte of the Word RAM routine stops code_bytes_mismatch at it (%s)" % stop)
        check(comstat[1] == 0x2222 and comstat[2:6] == [0, 0, 0, 0], "case 23: the corrupted instruction did not retire")

        # ---- authority ----
        without = build(tmp, "w_none", disc, bios, ip_image + sp_image)
        check(without.get("ok") is True, "the program without the sub Word RAM extent builds")
        if without.get("ok"):
            code, e = case(without["program"], 20)
            stop = e.get("stop", {})
            check(code == 10 and stop.get("class") == "entry_not_compiled" and stop.get("cpu") == 1 and stop.get("pc") == SUB_ROUTINE and e["ga"]["comstat"][2] == 0,
                  "without the extent the routine has no compiled entry: entry_not_compiled (%s)" % stop)
        wrong = build(tmp, "w_wrong", disc, bios, ip_image + sp_image + ["--disc-image", "sub:%d:0x%X:%d" % (ip_offset + routine_at + 2, SUB_ROUTINE, len(routine))])
        check(wrong.get("ok") is True, "an extent at the wrong disc offset builds")
        if wrong.get("ok"):
            code, e = case(wrong["program"], 20)
            stop = e.get("stop", {})
            check(code == 10 and stop.get("class") in ("code_bytes_mismatch", "entry_not_compiled") and stop.get("cpu") == 1 and stop.get("pc") == SUB_ROUTINE
                  and e["ga"]["comstat"][2] == 0,
                  "an extent whose disc bytes differ from the live ones never runs (%s)" % stop)
        for label, spec in (("straddling the end of the sub Word RAM view", "sub:%d:0xBFFF0:32" % ip_offset),
                            ("in the 1M bank window just past the view", "sub:%d:0xC0000:32" % ip_offset),
                            ("straddling the start of the view (PRG RAM end)", "sub:%d:0x7FFF0:32" % ip_offset),
                            ("for the main processor at a sub Word RAM base", "main:%d:0x80400:32" % ip_offset),
                            ("for the sub at a main Word RAM base", "sub:%d:0x200000:32" % ip_offset)):
            code, report = emit(tmp, disc, bios, *ip_image, *sp_image, "--disc-image", spec)
            check(code == 6 and report.get("ok") is False and "disc_image_invalid" in json.dumps(report),
                  "an extent %s is refused as disc_image_invalid (exit %d, %s)" % (label, code, report))
        for label, spec in (("ending exactly at the end of the sub Word RAM view ($0BFFFF)", "sub:%d:0xBFFE0:32" % ip_offset),
                            ("starting exactly at the start of the view ($080000)", "sub:%d:0x80000:32" % ip_offset)):
            code, report = emit(tmp, disc, bios, *ip_image, *sp_image, "--disc-image", spec)
            check(code == 0 and report.get("ok") is True, "an extent %s is accepted (exit %d)" % (label, code))

        # ---- determinism ----
        check(case(exe, 20)[1] == a, "a repeat of case 20 is identical")
        check(all(case(exe, 20, "--slice-seed", s)[1] == a for s in range(8)), "case 20: identical under eight deadline partitions")
        check(all(case(exe, 22, "--slice-seed", s)[1] == b for s in range(4)), "case 22: identical under four deadline partitions")
    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
