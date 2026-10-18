#!/usr/bin/env python3
"""SEG-064-T005: the BIOS-image profile past the application handoff (ADR 0145).

A project-authored "BIOS" (tools/sega_cd_bios_boot_fixture.py, cases 20-23) fills a work-RAM jump-table thunk (JMP abs.L) the way the
authentic BIOS does and enters a disc IP; the IP - project-authored here, written into the fixture disc's boot area - runs past its entry
instruction, copies a routine from its own image to work RAM (a RAM-resident routine, declared as a second disc extent at another base),
and calls the routine, which calls a BIOS service through the mutable thunk and returns. The program is built WITHOUT the application-entry
boundary. Checked:
  - the post-handoff instructions execute through normal generated-native dispatch (entry instruction, copied routine, BIOS service,
    return): the run ends idle with every marker, and the executed code is exactly the compiled, byte-verified entries;
  - the mutable table: another compiled target runs another service; a target with nothing compiled is entry_not_compiled at the target
    (never a silent return); a corrupted live byte of the copied routine stops code_bytes_mismatch before that instruction retires;
  - image authority: without the copy extent the copied routine is entry_not_compiled; an extent whose disc bytes differ is a mismatch;
    an extent that is not in the declared processor's RAM is refused; more than four extents (six here) build and run identically;
  - the machine-time end (`--until-tick`): result `deadline`, resumable (a partitioned run is identical), zero is a usage error;
  - the diagnostics added for the interval: control-flow edges (`<coverage>.edges`) record the application -> thunk -> BIOS service call and
    return, and the report carries the vertical-blank onset count;
  - determinism over eight deadline partitions and a repeat; the T004 stop-enabled program of the same images still ends at the entry.

usage: sega_cd_bios_posthandoff_test.py <segarecomp> <c-compiler> <source-root>
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
ROUTINE = 0xFF1000
SERVICE_A, SERVICE_B = 0x1C00, 0x1C20
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
    # the build passes run case 20 (the thunk is filled), so the fixed point meets the thunk frontier the authentic BIOS also produces
    out = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(disc / "disc.cue"), "--bios",
                          str(bios), "--work", str(tmp / name), "--pass-set", "main:d7=20", "--pass-set", "sub:a6=20", *extra],
                         capture_output=True, text=True, timeout=1800)
    return json.loads(out.stdout) if out.stdout.strip() else {"ok": False}


def emit(tmp, disc, bios, *extra):
    out = subprocess.run([SEGARECOMP, "sega-cd", "emit", "--cue", str(disc / "disc.cue"), "--bios", str(bios), "--out", str(tmp / "emit.c"), *extra],
                         capture_output=True, text=True, timeout=300)
    return out.returncode, (json.loads(out.stdout) if out.stdout.strip() else {})


def routine_code():
    """The RAM-resident routine (assembled for $FF1000): a marker, the BIOS service through the thunk, a marker, return."""
    r = F.Asm(ROUTINE)
    r.move("w", F.imm(0x1234), F.absl(RESULT + 0x44))
    r.jsr(F.absl(THUNK))
    r.move("w", F.imm(0x5678), F.absl(RESULT + 0x46))
    r.rts()
    return r.assemble()


def application_ip(routine):
    """The disc IP: boot stub, the application entry, the case-23 corruption, the copy of the routine, the call, and the routine's image."""
    def make(source):
        ip = F.Asm(F.IP_BASE)
        ip.move("w", F.imm(0x600D), F.absl(RESULT + 0x40))
        ip.bra("app")
        ip.nop()
        ip.label("app")
        ip.move("w", F.imm(0xBEEF), F.absl(RESULT + 0x42))           # the application entry instruction
        ip.move("l", F.imm(source), F.a(0))
        ip.move("l", F.imm(ROUTINE), F.a(1))
        ip.move("w", F.imm(len(routine) // 2 - 1), F.d(0))
        ip.label("copy")
        ip.move("w", ("inc", 0), ("inc", 1))
        ip.dbra(0, "copy")
        ip.immop("cmp", "w", 23, F.d(7))                               # case 23: one live byte of the copied routine is overwritten
        ip.bcc("ne", "go")
        ip.move("b", F.imm(0x5A), F.absl(ROUTINE + 1))
        ip.label("go")
        ip.jsr(F.absl(ROUTINE))
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
    with tempfile.TemporaryDirectory(prefix="scd-posthandoff-") as tmp:
        tmp = pathlib.Path(tmp)
        disc = tmp / "disc"
        described = common.build_disc(disc, "bootarea")
        ip_offset, sp_offset, sp_size = described["extents"]["ip"][0], described["extents"]["sp"][0], described["sp_size"]
        routine = routine_code()
        ip_bytes, entry, routine_at = application_ip(routine)
        assert len(ip_bytes) <= sp_offset - ip_offset
        track = bytearray((disc / "track01.bin").read_bytes())
        track[ip_offset:ip_offset + len(ip_bytes)] = ip_bytes           # the project-authored application IP, on the fixture disc
        (disc / "track01.bin").write_bytes(bytes(track))
        (tmp / "ip.bin").write_bytes(ip_bytes)
        (tmp / "sp.bin").write_bytes(bytes(track[sp_offset:sp_offset + sp_size]))
        subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(tmp / "fx"), "--boot-ip", str(tmp / "ip.bin"), "--boot-sp", str(tmp / "sp.bin"),
                        "--boot-ip-alt", str(tmp / "ip.bin"), "--entry", hex(entry)], check=True, capture_output=True)
        bios = tmp / "fx" / "bios.bin"
        ip_image = ["--disc-image", "main:%d:0xFF0000:%d" % (ip_offset, len(ip_bytes))]
        sp_image = ["--disc-image", "sub:%d:0x6000:%d" % (sp_offset, sp_size)]
        copy_image = ["--disc-image", "main:%d:0x%X:%d" % (ip_offset + routine_at, ROUTINE, len(routine))]
        images = ip_image + sp_image + copy_image

        summary = build(tmp, "w", disc, bios, images)
        check(summary.get("ok") is True, "the post-handoff program (no application-entry boundary) builds (%s)" % (summary.get("failure") or summary.get("rounds")))
        if not summary.get("ok"):
            return 1
        exe = pathlib.Path(summary["program"])
        templates = [a for a in summary["admissions"] if a.startswith("template:")]
        check(len(templates) >= 1 and any(":main:0x%x:" % THUNK in a for a in templates),
              "the work-RAM jump-table thunk was admitted as an operand template by the build fixed point (%d templates)" % len(templates))

        def case(program, number, *extra):
            return run(program, disc, bios, "--set", "main:d7=%d" % number, "--set", "sub:a6=%d" % number, *extra)

        def block(r):
            return bytes.fromhex(r["result_block"])

        def word(r, offset):
            return int.from_bytes(block(r)[offset:offset + 2], "big")

        # ---- the post-handoff run: entry instruction, copied routine, BIOS service through the thunk, return ----
        code, a = case(exe, 20)
        check(code == 0 and a.get("result") == "idle", "case 20: the run past the entry ends idle (%s %s)" % (code, a.get("stop")))
        check(word(a, 0x42) == 0xBEEF and word(a, 0x44) == 0x1234 and word(a, 0x4A) == 0x5E0A and word(a, 0x46) == 0x5678,
              "case 20: the entry instruction, the copied routine, BIOS service A through the thunk and the return all executed")
        check(a["cpus"][0]["pc"] != 0 and a["stop"]["class"] == "none", "case 20: no typed stop")
        code, b = case(exe, 22)
        check(code == 0 and word(b, 0x4A) == 0x5E0B and word(b, 0x46) == 0x5678 and word(b, 0x42) == 0xBEEF,
              "case 22: the mutable table reaches service B (another compiled target) and returns")
        check(a["digests"]["work_ram"] != b["digests"]["work_ram"], "case 20 / 22 differ in memory (the table is genuinely mutable)")

        # ---- a target with nothing compiled; a corrupted copy ----
        code, c = case(exe, 21)
        stop = c.get("stop", {})
        check(code == 10 and stop.get("class") == "entry_not_compiled" and stop.get("cpu") == 0 and stop.get("pc") == THUNK and
              stop.get("address") == 0xFFFF00,
              "case 21: a thunk target with no compiled instruction is entry_not_compiled (the thunk's pc, the target's address), not a silent return (%s)" % stop)
        check(word(c, 0x44) == 0x1234 and word(c, 0x46) == 0 and word(c, 0x4A) == 0, "case 21: the routine ran up to the call; nothing after it, and no service ran")
        code, d = case(exe, 23)
        stop = d.get("stop", {})
        check(code == 10 and stop.get("class") == "code_bytes_mismatch" and stop.get("cpu") == 0 and stop.get("pc") == ROUTINE,
              "case 23: a corrupted live byte of the copied routine stops code_bytes_mismatch at the routine (%s)" % stop)
        check(word(d, 0x42) == 0xBEEF and word(d, 0x44) == 0 and d["cpus"][0]["pc"] == ROUTINE, "case 23: the corrupted instruction did not retire (pc still there, its marker absent)")

        # ---- image authority ----
        without = build(tmp, "w_nocopy", disc, bios, ip_image + sp_image)
        check(without.get("ok") is True, "the program without the copy extent builds")
        if without.get("ok"):
            code, e = case(without["program"], 20)
            stop = e.get("stop", {})
            check(code == 10 and stop.get("class") == "entry_not_compiled" and stop.get("pc") == ROUTINE and word(e, 0x44) == 0,
                  "without the copy extent the RAM routine has no compiled entry: entry_not_compiled at it (%s)" % stop)
        wrong = build(tmp, "w_wrongbytes", disc, bios, ip_image + sp_image + ["--disc-image", "main:%d:0x%X:%d" % (ip_offset + routine_at + 2, ROUTINE, len(routine))])
        check(wrong.get("ok") is True, "an extent at the wrong disc offset (other bytes) builds")
        if wrong.get("ok"):
            code, e = case(wrong["program"], 20)
            stop = e.get("stop", {})
            check(code == 10 and stop.get("class") in ("code_bytes_mismatch", "entry_not_compiled") and stop.get("pc") == ROUTINE and word(e, 0x44) == 0,
                  "an extent whose disc bytes differ from the live ones never runs (%s)" % stop)
        code, report = emit(tmp, disc, bios, *ip_image, *sp_image, "--disc-image", "sub:%d:0xFF1000:%d" % (ip_offset + routine_at, len(routine)))
        check(code != 0 and report.get("ok") is not True, "an extent whose base is not in the declared processor's RAM is refused (exit %d)" % code)

        extra_images = []
        for i in range(3):   # three more extents (six in all) at distinct bases: more than the earlier limit of four
            extra_images += ["--disc-image", "main:%d:0x%X:%d" % (ip_offset, 0xFF4000 + 0x400 * i, len(ip_bytes))]
        six = build(tmp, "w_six", disc, bios, images + extra_images)
        check(six.get("ok") is True and len(six["emit"]["images"]) >= 7, "six disc extents (plus the BIOS and the epoch) build")
        if six.get("ok"):
            code, f = case(six["program"], 20)
            check(code == 0 and word(f, 0x4A) == 0x5E0A and word(f, 0x46) == 0x5678, "six extents: the same post-handoff behaviour")
        code, report = emit(tmp, disc, bios, *(images + extra_images * 6))
        check(code != 0, "more than sixteen extents are refused (exit %d)" % code)

        # ---- the machine-time end ----
        code, full = case(exe, 20, "--until-tick", 4000000000000000)
        check(code == 0 and full == a, "--until-tick past the end of the run changes nothing (the report is identical)")
        ticks = a["ticks"]
        mid = ticks // 2
        code, h = case(exe, 20, "--until-tick", mid)
        check(code == 11 and h.get("result") == "deadline" and 0 < h["ticks"] <= mid and h["stop"]["class"] == "none",
              "--until-tick inside the run ends with the result deadline, no typed stop (ticks %s of %s)" % (h.get("ticks"), mid))
        check(all(case(exe, 20, "--until-tick", mid, "--slice-seed", s)[1] == h for s in range(8)),
              "the deadline end is identical under eight deadline partitions")
        check(case(exe, 20, "--until-tick", mid, "--slice-ticks", 1000003)[1] == h, "... and under a fixed slice")
        code, _ = case(exe, 20, "--until-tick", 0)
        check(code == 2, "--until-tick 0 is a usage error (exit %d)" % code)

        # ---- diagnostics: control-flow edges and the vertical-blank count ----
        cov = tmp / "cov.txt"
        code, g = case(exe, 20, "--coverage", cov)
        edges = set()
        for line in pathlib.Path(str(cov) + ".edges").read_text().splitlines():
            cpu, source, target = line.split()
            edges.add((int(cpu), int(source, 16), int(target, 16)))
        calls = {t for (cpu, s, t) in edges if cpu == 0 and s == THUNK}
        check(calls == {SERVICE_A}, "the edges record the thunk -> BIOS service A call (targets %s)" % sorted(map(hex, calls)))
        returns = {t for (cpu, s, t) in edges if cpu == 0 and SERVICE_A <= s < SERVICE_A + 16 and t >= 0xFF1000}
        check(len(returns) == 1, "... and the service's return into the routine")
        into_routine = {s for (cpu, s, t) in edges if cpu == 0 and t == ROUTINE}
        check(len(into_routine) == 1, "... and the IP's call of the RAM-resident routine")
        check("vblank_onsets" in g["bios_profile"] and g["bios_profile"]["vblank_onsets"] >= 0, "the report carries the vertical-blank onset count")

        # ---- determinism ----
        check(case(exe, 20)[1] == a, "a repeat of case 20 is identical")
        check(all(case(exe, 20, "--slice-seed", s)[1] == a for s in range(8)), "case 20: identical under eight deadline partitions")
        check(all(case(exe, 23, "--slice-seed", s)[1] == d for s in range(8)), "case 23: identical under eight deadline partitions")

        # ---- the T004 stop-enabled program of the same images is unchanged: it ends at the entry, before it executes ----
        stopped = build(tmp, "w_entry", disc, bios, images + ["--application-entry", hex(entry)])
        check(stopped.get("ok") is True, "the same images with the application-entry boundary build")
        if stopped.get("ok"):
            code, s = case(stopped["program"], 20)
            check(code == 10 and s["stop"]["class"] == "application_handoff" and s["stop"]["pc"] == entry and word(s, 0x42) == 0,
                  "with the boundary the run still ends application_handoff at the entry, before the entry instruction (T004 behaviour)")
            check(stopped["program_identity"] != summary["program_identity"], "the boundary is part of the program identity (a different program from the post-handoff one)")
    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
