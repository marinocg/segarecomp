#!/usr/bin/env python3
"""SEG-071-T001: a disc extent mapped into the main processor's 2M Word RAM view (ADR 0176).

A project-authored "BIOS" (tools/sega_cd_bios_boot_fixture.py, cases 20/22/23) places a project-authored disc IP and enters it, as in
SEG-064-T005. The IP copies a routine from its own image into 2M Word RAM (which main owns after reset) and calls it there - the shape of
an application whose main code the sub loaded into Word RAM and main executes in place. The routine image is declared to the emitter as
an extent of the fixture disc mapped at its Word RAM base. Checked:
  - the Word RAM routine runs as compiled, byte-verified entries; it calls a BIOS service through the mutable work-RAM thunk and returns;
    a data write into its own image (a byte that is no instruction) does not invalidate it;
  - owner rule: when main has given 2M Word RAM to the sub (DMNA) before the call, main's fetch fails closed (code_bytes_mismatch at the
    routine, nothing retires there) - the extent is not a RAM snapshot and is never run from bytes main cannot see;
  - live-byte guard: a corrupted live byte of the copied routine stops code_bytes_mismatch before it retires;
  - authority: without the Word RAM extent the routine is entry_not_compiled; an extent whose disc bytes differ never runs; an extent
    that leaves the Word RAM view ($200000-$23FFFF), lies past it, or names the sub at a main Word RAM base is refused by the emitter;
  - determinism over eight deadline partitions and a repeat.

usage: sega_cd_bios_wordram_extent_test.py <segarecomp> <c-compiler> <source-root>
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
THUNK = 0xFFFD00
ROUTINE = 0x200400        # inside the main 2M Word RAM view
GA_MAIN_MODE = 0xA12003   # main's memory-mode register (DMNA = bit 1)
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
    """The Word RAM routine (assembled for ROUTINE): a marker, the BIOS service through the thunk, a data write into its own image, a
    marker, return; then one data word (no instruction) that the write changes."""
    r = F.Asm(ROUTINE)
    r.move("w", F.imm(0x1234), F.absl(RESULT + 0x44))
    r.jsr(F.absl(THUNK))
    r.move("w", F.imm(0xABCD), F.absl(0))           # patched below: the data word of this image
    r.move("w", F.imm(0x5678), F.absl(RESULT + 0x46))
    r.rts()
    r.label("data")
    r.w(0)
    first = r.assemble()
    data = r.labels["data"]
    r = F.Asm(ROUTINE)
    r.move("w", F.imm(0x1234), F.absl(RESULT + 0x44))
    r.jsr(F.absl(THUNK))
    r.move("w", F.imm(0xABCD), F.absl(data))
    r.move("w", F.imm(0x5678), F.absl(RESULT + 0x46))
    r.rts()
    r.label("data")
    r.w(0)
    code = r.assemble()
    assert len(code) == len(first) and r.labels["data"] == data
    return code, data


def application_ip(routine):
    """The disc IP: boot stub, the entry, the copy of the routine into Word RAM, case 23 corrupts a live byte, case 22 gives Word RAM
    to the sub, the call, and the routine's image."""
    def make(source):
        ip = F.Asm(F.IP_BASE)
        ip.move("w", F.imm(0x600D), F.absl(RESULT + 0x40))
        ip.bra("app")
        ip.nop()
        ip.label("app")
        ip.move("w", F.imm(0xBEEF), F.absl(RESULT + 0x42))
        ip.move("l", F.imm(source), F.a(0))
        ip.move("l", F.imm(ROUTINE), F.a(1))
        ip.move("w", F.imm(len(routine) // 2 - 1), F.d(0))
        ip.label("copy")
        ip.move("w", ("inc", 0), ("inc", 1))
        ip.dbra(0, "copy")
        ip.immop("cmp", "w", 23, F.d(7))
        ip.bcc("ne", "not23")
        ip.move("b", F.imm(0x5A), F.absl(ROUTINE + 1))
        ip.label("not23")
        ip.immop("cmp", "w", 22, F.d(7))
        ip.bcc("ne", "go")
        ip.move("b", F.imm(0x02), F.absl(GA_MAIN_MODE))   # 2M, DMNA = 1: Word RAM to the sub (main no longer sees it)
        ip.label("go")
        ip.jsr(F.absl(ROUTINE))
        ip.move("w", F.imm(0x0D0E), F.absl(RESULT + 0x48))
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
    with tempfile.TemporaryDirectory(prefix="scd-wordram-") as tmp:
        tmp = pathlib.Path(tmp)
        disc = tmp / "disc"
        described = common.build_disc(disc, "bootarea")
        ip_offset, sp_offset, sp_size = described["extents"]["ip"][0], described["extents"]["sp"][0], described["sp_size"]
        routine, data = routine_code()
        ip_bytes, entry, routine_at = application_ip(routine)
        assert len(ip_bytes) <= sp_offset - ip_offset
        track = bytearray((disc / "track01.bin").read_bytes())
        track[ip_offset:ip_offset + len(ip_bytes)] = ip_bytes
        (disc / "track01.bin").write_bytes(bytes(track))
        (tmp / "ip.bin").write_bytes(ip_bytes)
        (tmp / "sp.bin").write_bytes(bytes(track[sp_offset:sp_offset + sp_size]))
        subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(tmp / "fx"), "--boot-ip", str(tmp / "ip.bin"), "--boot-sp", str(tmp / "sp.bin"),
                        "--boot-ip-alt", str(tmp / "ip.bin"), "--entry", hex(entry)], check=True, capture_output=True)
        bios = tmp / "fx" / "bios.bin"
        ip_image = ["--disc-image", "main:%d:0xFF0000:%d" % (ip_offset, len(ip_bytes))]
        sp_image = ["--disc-image", "sub:%d:0x6000:%d" % (sp_offset, sp_size)]
        wr_image = ["--disc-image", "main:%d:0x%X:%d" % (ip_offset + routine_at, ROUTINE, len(routine))]

        summary = build(tmp, "w", disc, bios, ip_image + sp_image + wr_image)
        check(summary.get("ok") is True, "the program with a main Word RAM extent builds (%s)" % (summary.get("failure") or summary.get("rounds")))
        if not summary.get("ok"):
            return 1
        exe = pathlib.Path(summary["program"])
        wr = [i for i in summary["emit"]["images"] if i.get("processor") == "main" and i.get("start") == ROUTINE]
        check(len(wr) == 1 and wr[0].get("compiled", 0) >= 5, "the Word RAM extent is an admitted main image with compiled entries (%s)" % wr)

        def case(program, number, *extra):
            return run(program, disc, bios, "--set", "main:d7=%d" % number, "--set", "sub:a6=%d" % number, *extra)

        def word(r, offset):
            return int.from_bytes(bytes.fromhex(r["result_block"])[offset:offset + 2], "big")

        # ---- the Word RAM routine runs, calls the BIOS service through the thunk, writes its own data word, returns ----
        code, a = case(exe, 20)
        check(code == 0 and a.get("result") == "idle" and a["stop"]["class"] == "none", "case 20: the run ends idle, no typed stop (%s %s)" % (code, a.get("stop")))
        check(word(a, 0x42) == 0xBEEF and word(a, 0x44) == 0x1234 and word(a, 0x4A) == 0x5E0A and word(a, 0x46) == 0x5678 and word(a, 0x48) == 0x0D0E,
              "case 20: the entry, the Word RAM routine, BIOS service A through the thunk, the routine's tail after its data write, the return")

        # ---- owner rule: Word RAM given to the sub before the call ----
        code, b = case(exe, 22)
        stop = b.get("stop", {})
        check(code == 10 and stop.get("class") == "code_bytes_mismatch" and stop.get("cpu") == 0 and stop.get("pc") == ROUTINE,
              "case 22: with 2M Word RAM given to the sub, main's fetch of the routine fails closed (%s)" % stop)
        check(word(b, 0x44) == 0 and word(b, 0x48) == 0 and b["cpus"][0]["pc"] == ROUTINE, "case 22: nothing of the routine retired")

        # ---- live-byte guard ----
        code, d = case(exe, 23)
        stop = d.get("stop", {})
        check(code == 10 and stop.get("class") == "code_bytes_mismatch" and stop.get("cpu") == 0 and stop.get("pc") == ROUTINE,
              "case 23: a corrupted live byte of the Word RAM routine stops code_bytes_mismatch at it (%s)" % stop)
        check(word(d, 0x42) == 0xBEEF and word(d, 0x44) == 0, "case 23: the corrupted instruction did not retire")

        # ---- authority ----
        without = build(tmp, "w_none", disc, bios, ip_image + sp_image)
        check(without.get("ok") is True, "the program without the Word RAM extent builds")
        if without.get("ok"):
            code, e = case(without["program"], 20)
            stop = e.get("stop", {})
            check(code == 10 and stop.get("class") == "entry_not_compiled" and stop.get("pc") == ROUTINE and word(e, 0x44) == 0,
                  "without the extent the Word RAM routine has no compiled entry: entry_not_compiled (%s)" % stop)
        wrong = build(tmp, "w_wrong", disc, bios, ip_image + sp_image + ["--disc-image", "main:%d:0x%X:%d" % (ip_offset + routine_at + 2, ROUTINE, len(routine))])
        check(wrong.get("ok") is True, "an extent at the wrong disc offset builds")
        if wrong.get("ok"):
            code, e = case(wrong["program"], 20)
            stop = e.get("stop", {})
            check(code == 10 and stop.get("class") in ("code_bytes_mismatch", "entry_not_compiled") and stop.get("pc") == ROUTINE and word(e, 0x44) == 0,
                  "an extent whose disc bytes differ from the live ones never runs (%s)" % stop)
        for label, spec in (("straddling the end of the Word RAM view", "main:%d:0x23FFF0:32" % ip_offset),
                            ("past the Word RAM view", "main:%d:0x240000:32" % ip_offset),
                            ("below the Word RAM view (the PRG-RAM window)", "main:%d:0x1FFFF0:32" % ip_offset),
                            ("for the sub at a main Word RAM base", "sub:%d:0x200000:32" % ip_offset)):
            code, report = emit(tmp, disc, bios, *ip_image, *sp_image, "--disc-image", spec)
            check(code == 6 and report.get("ok") is False and "disc_image_invalid" in json.dumps(report),
                  "an extent %s is refused as disc_image_invalid (exit %d, %s)" % (label, code, report))
        code, report = emit(tmp, disc, bios, *ip_image, *sp_image, "--disc-image", "main:%d:0x23FFE0:32" % ip_offset)
        check(code == 0 and report.get("ok") is True, "an extent ending exactly at the end of the Word RAM view is accepted (exit %d)" % code)

        # ---- determinism ----
        check(case(exe, 20)[1] == a, "a repeat of case 20 is identical")
        check(all(case(exe, 20, "--slice-seed", s)[1] == a for s in range(8)), "case 20: identical under eight deadline partitions")
        check(all(case(exe, 22, "--slice-seed", s)[1] == b for s in range(4)), "case 22: identical under four deadline partitions")
    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
