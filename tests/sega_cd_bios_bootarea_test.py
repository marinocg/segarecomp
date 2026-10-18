#!/usr/bin/env python3
"""SEG-064-T004: disc-extent executable images and the application-entry boundary of the Sega CD BIOS-image profile (ADR 0142 decision 5).

A project-authored "BIOS" (tools/sega_cd_bios_boot_fixture.py) releases a tiny sub program, then places the fixture disc's IP in main work
RAM and its SP in sub PRG RAM - over stale transient code that the sub's verified epoch image holds at the same addresses - the way a
BIOS does, and enters the IP. The extents are declared to the emitter (`--disc-image`) as disc track-1 byte ranges, read through the
verified disc reader at build time. Checked:
  - the emission report lists the disc images (authority immutable_input, the declared range, compiled entries) and the program identity
    covers them (a different range or a different disc is a different program);
  - case 12: the SP replaces the stale code (COMSTAT1 = the SP's marker, never the stale marker), the IP executes from the verified disc
    image (its marker) and the run ends with the typed stop application_handoff at the application entry - the entry instruction does
    not run; the same program without `--application-entry` runs the entry and ends idle;
  - case 14: with the SP never placed, the stale epoch variant runs (so the replacement above is by live bytes, not by address);
  - case 13: one corrupted IP byte has no compiled variant: the typed stop code_bytes_mismatch at the IP, nothing of it ran;
  - invalid declarations are typed errors; a modified disc is refused at start-up; eight deadline partitions give identical reports.

usage: sega_cd_bios_bootarea_test.py <segarecomp> <c-compiler> <source-root>
"""
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

FIXTURE = ROOT / "tools" / "sega_cd_bios_boot_fixture.py"
DRIVER = ROOT / "tools" / "sega_cd_bios_build.py"
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def run(exe, disc, bios, *extra):
    out = subprocess.run([str(exe), "--disc-dir", str(disc), "--bios", str(bios), "--instruction-budget", "3000000", *map(str, extra)],
                         capture_output=True, text=True, timeout=common.RUN_TIMEOUT)
    return out.returncode, (json.loads(out.stdout) if out.stdout.strip() else {})


def build(tmp, name, disc, bios, extra):
    out = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(disc / "disc.cue"), "--bios",
                          str(bios), "--work", str(tmp / name), *extra], capture_output=True, text=True, timeout=1800)
    return json.loads(out.stdout) if out.stdout.strip() else {"ok": False}


def emit(disc, bios, *extra):
    out = subprocess.run([SEGARECOMP, "sega-cd", "emit", "--cue", str(disc / "disc.cue"), "--bios", str(bios), "--out", "/dev/null", *extra],
                         capture_output=True, text=True, timeout=300)
    return out.returncode, (json.loads(out.stdout) if out.stdout.strip() else {})


def main():
    with tempfile.TemporaryDirectory(prefix="scd-bootarea-") as tmp:
        tmp = pathlib.Path(tmp)
        disc = tmp / "disc"
        described = common.build_disc(disc, "bootarea")
        track = (disc / "track01.bin").read_bytes()
        ip_offset, ip_size = described["extents"]["ip"][0], described["ip_size"]
        sp_offset, sp_size = described["extents"]["sp"][0], described["sp_size"]
        (tmp / "ip.bin").write_bytes(track[ip_offset:ip_offset + ip_size])
        (tmp / "sp.bin").write_bytes(track[sp_offset:sp_offset + sp_size])
        subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(tmp / "fx"), "--boot-ip", str(tmp / "ip.bin"), "--boot-sp",
                        str(tmp / "sp.bin")], check=True, capture_output=True)
        bios = tmp / "fx" / "bios.bin"
        entry = described["application_entry"]
        images = ["--disc-image", "main:%d:0xFF0000:%d" % (ip_offset, ip_size), "--disc-image", "sub:%d:0x6000:%d" % (sp_offset, sp_size)]
        summary = build(tmp, "w", disc, bios, images + ["--application-entry", hex(entry)])
        check(summary.get("ok") is True and summary["rounds"] == 2, "fixed point: the epoch, then the candidate (%s)" % (summary.get("failure") or summary.get("rounds")))
        if not summary.get("ok"):
            return 1
        by_name = {i["name"]: i for i in summary["emit"]["images"]}
        d0, d1 = by_name.get("disc0", {}), by_name.get("disc1", {})
        check(d0.get("processor") == "main" and d0.get("authority") == "immutable_input" and d0.get("start") == 0xFF0000 and d0.get("length") == ip_size and
              d0.get("compiled", 0) > 0, "disc0: the IP extent, main $FF0000, immutable_input, compiled entries (%s)" % d0)
        check(d1.get("processor") == "sub" and d1.get("authority") == "immutable_input" and d1.get("start") == 0x6000 and d1.get("length") == sp_size and
              d1.get("compiled", 0) > 0, "disc1: the SP extent, sub $6000, immutable_input, compiled entries (%s)" % d1)
        exe = pathlib.Path(summary["program"])

        code, handoff = run(exe, disc, bios, "--set", "main:d7=12", "--set", "sub:a6=12")
        stop = handoff.get("stop", {})
        block = bytes.fromhex(handoff.get("result_block", "00" * 256))
        check(code == 10 and stop.get("class") == "application_handoff" and stop.get("cpu") == 0 and stop.get("pc") == entry and stop.get("address") == entry,
              "case 12: the run ends application_handoff at the application entry $%X (%s)" % (entry, stop))
        check(int.from_bytes(block[0x40:0x42], "big") == described["ip_marker"] and block[0x42:0x44] == b"\0\0",
              "case 12: the IP ran from the verified disc image (its marker) and the entry instruction did not")
        check(handoff["ga"]["comstat"][1] == described["sp_marker"] and handoff["ga"]["comstat"][0] == 0xC0DE and handoff["bios_profile"]["epochs"] == 1,
              "case 12: the SP replaced the stale transient code ($%04X, not the stale marker); one verified epoch" % described["sp_marker"])
        check(handoff["cpus"][0]["pc"] == entry, "case 12: the main pc is at the entry (nothing there retired)")

        code, stale = run(exe, disc, bios, "--set", "main:d7=14", "--set", "sub:a6=14")
        check(stale["ga"]["comstat"][1] == 0x1111 and stale["stop"]["class"] == "application_handoff" and stale["stop"]["pc"] == entry,
              "case 14: without the SP placed, the stale epoch variant runs ($1111): the variants are selected by live bytes; the IP is entered through $FFFF0000 (24-bit pc) (%s %s)" % (stale["ga"]["comstat"][1], stale["stop"]))

        code, corrupt = run(exe, disc, bios, "--set", "main:d7=13", "--set", "sub:a6=13")
        stop = corrupt.get("stop", {})
        cblock = bytes.fromhex(corrupt["result_block"])
        check(code == 10 and stop.get("class") == "code_bytes_mismatch" and stop.get("cpu") == 0 and stop.get("pc") == 0xFF0000 and
              stop.get("address") == 0xFF0003 and cblock[0x40:0x42] == b"\0\0",
              "case 13: a corrupted IP byte has no compiled variant: code_bytes_mismatch at the instruction ($FF0000), reporting the first differing byte ($FF0003); nothing of the IP ran (%s)" % stop)
        check(corrupt["cpus"][0]["pc"] == 0xFF0000 and corrupt["ga"]["comstat"][1] == described["sp_marker"],
              "case 13: the main pc is still at the corrupted instruction (it did not retire)")

        # the same images without the entry boundary: the IP runs through its entry
        open_summary = build(tmp, "w_open", disc, bios, images)
        check(open_summary.get("ok") is True, "the program without --application-entry builds")
        if open_summary.get("ok"):
            code, open_run = run(open_summary["program"], disc, bios, "--set", "main:d7=12", "--set", "sub:a6=12")
            oblock = bytes.fromhex(open_run["result_block"])
            check(code == 0 and open_run["result"] == "idle" and int.from_bytes(oblock[0x42:0x44], "big") == 0xBEEF,
                  "without the boundary the IP executes its entry instruction (BEEF) and the run ends idle")
            check(open_summary["program_identity"] != summary["program_identity"], "the entry boundary is part of the program identity")

        # determinism
        for seed in range(8):
            check(run(exe, disc, bios, "--set", "main:d7=12", "--set", "sub:a6=12", "--slice-seed", seed)[1] == handoff,
                  "case 12: deadline partition seed %d identical" % seed)
        check(run(exe, disc, bios, "--set", "main:d7=12", "--set", "sub:a6=12")[1] == handoff, "case 12: repeat identical")

        # identity: the extents are part of the program; a modified disc is refused
        code_a, a = emit(disc, bios, *images)
        code_b, b = emit(disc, bios, "--disc-image", "main:%d:0xFF0000:%d" % (ip_offset, ip_size))
        check(code_a == 0 and code_b == 0 and a["program_identity"] != b["program_identity"], "a different set of disc images is a different program")
        other = tmp / "other"
        shutil.copytree(disc, other)
        data = bytearray((other / "track01.bin").read_bytes())
        data[ip_offset + 3] ^= 0x01
        (other / "track01.bin").write_bytes(bytes(data))
        code, refused = run(exe, other, bios)
        check(code == 20 and refused.get("result") == "disc_refused", "a modified disc is refused at start-up (%s)" % refused.get("error"))
        code_c, c = emit(other, bios, *images)
        check(code_c == 0 and c["program_identity"] != a["program_identity"], "the same declaration on a modified disc is a different program")

        # invalid declarations
        for label, extra, status in (
            ("an odd length", ["--disc-image", "main:512:0xFF0000:25"], 6),
            ("an extent past track 1", ["--disc-image", "main:%d:0xFF0000:2048" % (40 * 2048 - 1024)], 7),
            ("a base outside the processor's RAM", ["--disc-image", "main:512:0x100000:26"], 6),
            ("a sub base past PRG RAM", ["--disc-image", "sub:512:0x7FFF0:26"], 6),
            ("a bad syntax", ["--disc-image", "main:512"], 7),
            ("too many images", ["--disc-image", "main:512:0xFF0000:26"] * 5, 6),
            ("a zero application entry", ["--application-entry", "0"], 2),
            ("an odd application entry", ["--application-entry", "0xFF0001"], 2),
            ("an application entry past 24 bits", ["--application-entry", "0x1000000"], 2),
        ):
            code, report = emit(disc, bios, *extra)
            check(code == status and (code == 2 or report.get("ok") is False), "%s is refused (exit %d, %s)" % (label, code, report.get("error")))
    if failures:
        print("%d failure(s)" % len(failures))
        return 1
    print("sega_cd_bios_bootarea_test: OK")
    return 0


sys.exit(main())
