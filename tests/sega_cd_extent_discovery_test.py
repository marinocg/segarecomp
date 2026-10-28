#!/usr/bin/env python3
"""SEG-078-T003 (ADR 0205): reference-independent disc-extent discovery - the pre-registered synthetic matrix.

Project-authored BIOS-like image and disc (tools/sega_cd_bios_loader_fixture.py): the sub reads modules through the CDD/CDC into 2M Word RAM and runs,
alters, decodes or generates code. The build driver runs with `--discover-extents` and NO manual extent, recipe, trace or reference. Checked against the
table of ADR 0205, with the ground truth (the fixture's own record of what it loaded where) used only to JUDGE the result, never as an input:

  direct, siblings, overlay        discovered extents are exact copies of the loaded disc spans; the program runs to the end and writes every marker
  duplicates                       two identical delivered copies: `ambiguous_source`, the module is never compiled, the program stops there
  late_flip                        the exact-copy prefix is admitted, the flipped instruction is refused (`no_source`): the program stops before executing it
  generated, transformed, undelivered   `no_source`: nothing is admitted, the program stops at the frontier
  determinism                      the same inputs give the same extents, admissions and program identity
  evidence                         the resolver refuses incomplete evidence; an `extent:` admission with a wrong pass identity, outside the processor's RAM,
                                   or beyond the track is refused by the emitter

usage: sega_cd_extent_discovery_test.py <segarecomp> <c-compiler> <source-root>
"""
import json
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
FIXTURE = ROOT / "tools" / "sega_cd_bios_loader_fixture.py"
DRIVER = ROOT / "tools" / "sega_cd_bios_build.py"
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def fixture(tmp, case):
    out = tmp / case
    truth = json.loads(subprocess.run([sys.executable, str(FIXTURE), "build", "--case", case, "--out", str(out)], check=True, capture_output=True, text=True).stdout)
    return out, truth


def build(tmp, case, out, name=None, extra=()):
    done = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(out / "disc" / "disc.cue"), "--bios", str(out / "bios.bin"),
                           "--work", str(tmp / (name or "work-" + case)), "--discover-extents", "--timeout", "300", *extra], capture_output=True, text=True, timeout=1800)
    return json.loads(done.stdout) if done.stdout.strip() else {"ok": False}


def run_program(summary, out):
    done = subprocess.run([summary["program"], "--disc-dir", str(out / "disc"), "--bios", str(out / "bios.bin"), "--instruction-budget", "3000000"],
                          capture_output=True, text=True, timeout=300)
    report = json.loads(done.stdout)
    block = bytes.fromhex(report["result_block"])
    return done.returncode, report, [int.from_bytes(block[0x10 + 2 * i:0x12 + 2 * i], "big") for i in range(3)]


def norm(admissions):
    """Admissions without the host path of an epoch snapshot (the work directory differs between builds)."""
    return [":".join(a.split(":")[:2]) if a.startswith("epoch:") else a for a in admissions]


def extents_of(summary):
    return [tuple(int(v) for v in e.split(":")[1:]) for e in summary.get("extents", [])]   # (offset, base, length)


def truth_extents(truth):
    return [(e["offset"], e["base"], e["length"]) for e in truth["extents"]]


def main():
    with tempfile.TemporaryDirectory(prefix="scd-discover-") as tmp:
        tmp = pathlib.Path(tmp)
        results = {}
        for case in ("direct", "siblings", "duplicates", "overlay", "late_flip", "generated", "transformed", "undelivered"):
            out, truth = fixture(tmp, case)
            summary = build(tmp, case, out)
            check(summary.get("ok") is True, "%s: the build completes (a refusal ends the build at the frontier, it is not a build failure)" % case)
            code, report, markers = run_program(summary, out) if summary.get("ok") else (None, {}, [])
            results[case] = (out, truth, summary, code, report, markers)

        out, truth, s, code, report, markers = results["direct"]
        check(extents_of(s) == truth_extents(truth) and s["frontier"] is None and report["result"] == "idle" and markers == truth["markers"],
              "direct: one extent, equal to the module's disc span, base and length; the program runs to the end and writes the module's marker")
        out, truth, s, code, report, markers = results["siblings"]
        got = extents_of(s)
        check(len(got) == 1 and got[0][1] == 0x80000 and got[0][2] == 8192 and s["frontier"] is None and report["result"] == "idle" and markers == truth["markers"],
              "siblings: the extent explains the whole window (P and Q), not the shorter sibling; Q runs and writes its marker")
        out, truth, s, code, report, markers = results["duplicates"]
        check(s["extents"] == [] and s["frontier"] and s["frontier"]["refusal"] == "ambiguous_source" and report["stop"]["class"] == "entry_not_compiled" and
              report["stop"]["pc"] == 0x80000 and markers == [0, 0, 0],
              "duplicates: two identical delivered sources are ambiguous_source; nothing is admitted and the module never runs (typed stop at its first instruction)")
        out, truth, s, code, report, markers = results["overlay"]
        check(extents_of(s) == truth_extents(truth) and len({e[1] for e in extents_of(s)}) == 1 and s["frontier"] is None and markers == truth["markers"] and report["result"] == "idle",
              "overlay: two extents at one execution address (two modules loaded in turn), both run, both markers written")
        out, truth, s, code, report, markers = results["late_flip"]
        got = extents_of(s)
        flip = 0x80000 + truth["flip_offset"]
        check(len(got) == 1 and got[0][1] == 0x80000 and 64 <= got[0][2] <= truth["flip_offset"] and s["frontier"] and s["frontier"]["refusal"] == "no_source" and
              report["stop"]["class"] == "entry_not_compiled" and report["stop"]["pc"] <= flip and report["ga"]["comstat"][2] == 0xA001,
              "late_flip: only the exact-copy prefix (before the flipped byte) is admitted; the flipped instruction is no_source and the program stops at or before it")
        for case in ("generated", "transformed", "undelivered"):
            out, truth, s, code, report, markers = results[case]
            check(s["extents"] == [] and s["frontier"] and s["frontier"]["refusal"] in ("no_source", "no_delivery_evidence") and report["stop"]["class"] == "entry_not_compiled" and
                  markers == [0, 0, 0],
                  "%s: no extent is admitted (%s) and the code never runs" % (case, s["frontier"]["refusal"] if s.get("frontier") else None))

        # ---- determinism: the same inputs, a second build in a fresh work directory ----
        out, truth, first, *_ = results["overlay"]
        again = build(tmp, "overlay", out, "work-overlay-2")
        check(again.get("program_identity") == first["program_identity"] and again["extents"] == first["extents"] and norm(again["admissions"]) == norm(first["admissions"]) and
              again["history"] == first["history"], "determinism: the same inputs give the same extents, admissions, rounds and program identity")

        # ---- evidence refusals through the resolver and the emitter ----
        out, truth, first, *_ = results["direct"]
        cue = str(out / "disc" / "disc.cue")
        work = tmp / "evidence"
        work.mkdir()
        window = bytes(range(256)) * 16
        (work / "region.bin").write_bytes(window)

        def resolve(*extra, pc=0x80000, region=work / "region.bin", base=0x80000):
            done = subprocess.run([SEGARECOMP, "sega-cd", "resolve-source", "--cue", cue, "--cpu", "sub", "--pc", str(pc), "--region", str(region), "--region-base", str(base), *extra],
                                  capture_output=True, text=True)
            return done.returncode, json.loads(done.stdout) if done.stdout.strip() else {}

        code, answer = resolve("--delivered", "40:2", "--delivered-overflow", "1")
        check(code == 8 and answer["error"] == "ledger_overflow", "evidence: an overflowed delivery ledger is refused (ledger_overflow)")
        code, answer = resolve()
        check(code == 8 and answer["error"] == "no_delivery_evidence", "evidence: an empty ledger is refused (no_delivery_evidence)")
        code, answer = resolve("--delivered", "0:16")
        check(code == 8 and answer["error"] in ("no_source", "ambiguous_source"), "evidence: a window that is not in the delivered blocks has no source (%s)" % answer.get("error"))
        (work / "tiny.bin").write_bytes(b"\0" * 8)
        code, answer = resolve("--delivered", "40:2", region=work / "tiny.bin")
        check(code == 8 and answer["error"] == "bad_evidence", "evidence: a window smaller than an anchor is refused (bad_evidence)")
        code, answer = resolve("--delivered", "40:2", pc=0x90000)
        check(code == 8 and answer["error"] == "bad_evidence", "evidence: a program counter outside the window is refused (bad_evidence)")
        data = (out / "disc" / "track01.bin").read_bytes()[40 * 2048:40 * 2048 + 2048]
        (work / "short.bin").write_bytes(data[:32] + b"\xAA" * 16)       # 32 matching bytes then a different tail: a run of 32 < 64
        code, answer = resolve("--delivered", "40:2", region=work / "short.bin")
        check(code == 8 and answer["error"] == "too_short", "evidence: an explanation shorter than 64 bytes is refused (too_short)")
        (work / "good.bin").write_bytes(data[:1024])
        code, answer = resolve("--delivered", "40:2", region=work / "good.bin")
        check(code == 0 and answer["ok"] and answer["offset"] == 40 * 2048 and answer["base"] == 0x80000 and answer["length"] == 1024,
              "evidence: an exact copy of 1,024 delivered bytes resolves to its offset, base and length")
        code, answer = resolve("--delivered", "42:2", region=work / "good.bin")
        check(code == 8 and answer["error"] == "no_source", "evidence: the same bytes are refused when only OTHER blocks were delivered")

        epochs = [a for a in first["admissions"] if a.startswith("epoch:")]
        extent = [a for a in first["admissions"] if a.startswith("extent:")][-1]
        parts = extent.split(":")                      # extent, pass, cpu, offset, base, length

        def emit(spec):
            admits = []
            for a in epochs + [spec]:
                admits += ["--admit", a]
            return subprocess.run([SEGARECOMP, "sega-cd", "emit", "--cue", cue, "--bios", str(out / "bios.bin"), "--out", str(work / "x.c"), *admits], capture_output=True, text=True)

        done = emit(extent)
        check(done.returncode == 0, "emitter: the discovered extent admission re-validates (the chain, the disc bytes and the processor's RAM)")
        done = emit(":".join([parts[0], "0" * 64] + parts[2:]))
        check(done.returncode != 0 and "provenance_mismatch" in done.stdout, "emitter: an extent admission with a wrong pass identity is refused (provenance_mismatch)")
        done = emit(":".join(parts[:4] + ["1048576", parts[5]]))
        check(done.returncode != 0 and "disc_image_invalid" in done.stdout, "emitter: an extent admission outside the processor's RAM is refused (disc_image_invalid)")
        done = emit(":".join(parts[:3] + ["99999999"] + parts[4:]))
        check(done.returncode != 0 and "disc_image_outside_track" in done.stdout, "emitter: an extent admission beyond track 1 is refused")
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
