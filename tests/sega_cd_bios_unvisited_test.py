#!/usr/bin/env python3
"""SEG-098-T002 (ADR 0211): static admission of never-visited disc modules in the supported BIOS-image build (`segarecomp sega-cd build`).

Project-authored BIOS image + disc (tools/sega_cd_bios_unvisited_fixture.py). The build pass runs the default route (module A); B (same address as A) and C (two
mappings) are never loaded by it. The static loader scan (disc + boot images; no module list, no trace, no execution) proposes their extents; the SAME program
binary then runs every route. The oracle (expected.json) is read only here. usage: sega_cd_bios_unvisited_test.py <segarecomp> <c-compiler> <source-root>
"""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
FIXTURE = ROOT / "tools" / "sega_cd_bios_unvisited_fixture.py"
EXE = ".exe" if os.name == "nt" else ""
BUDGET = "3000000"
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def fixture(tmp, case):
    out = tmp / ("fx-" + case)
    oracle = json.loads(subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(out), "--case", case], check=True, capture_output=True, text=True).stdout)
    return out, oracle


def build(fx, out, cache, *extra):
    done = subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(fx / "disc" / "disc.cue"), "--bios", str(fx / "bios.bin"), "--output", str(out), "--runtime-dir", str(ROOT),
                           "--cc", CC, "--budget", BUDGET, "--jobs", "4", "--cache-dir", str(cache), *extra], capture_output=True, text=True, timeout=1800)
    status = out / "status.json"
    return done, json.loads(status.read_text()) if status.exists() else None


def run(program, fx, choice=0, disc=None):
    extra = ["--set", "main:d7=%d" % choice] if choice else []
    out = subprocess.run([str(program), "--disc-dir", str(disc or fx / "disc"), "--bios", str(fx / "bios.bin"), "--instruction-budget", BUDGET, *extra], capture_output=True, text=True, timeout=300)
    report = json.loads(out.stdout)
    block = bytes.fromhex(report.get("result_block", "")).ljust(16, b"\0")   # a run that stopped before the IP finished leaves no result block: all zero
    return report, [int.from_bytes(block[2 * i:2 * i + 2], "big") for i in range(8)]   # counts A,B,C then the module markers


def main():
    with tempfile.TemporaryDirectory(prefix="scd-bios-unvisited-") as tmp:
        tmp = pathlib.Path(tmp)
        cache = tmp / "objects"
        fx, oracle = fixture(tmp, "base")
        mark = oracle["markers"]

        # ---- the supported build: static admission, no manifest, no execution-derived admission of B or C ----
        done, st = build(fx, tmp / "n-base", cache)
        check(done.returncode == 0 and st and st["status"] == "ok" and st["stop"] == "none" and st["result"] == "idle", "base: the supported build succeeds and the build pass ends idle")
        sd = st["static_discovery"]
        check(sd["ran"] and sd["status"] == "admitted" and sd["complete"] and st["static_extents"] == 4, "base: the static scan admitted 4 extents (A, B, and C at two bases) before any pass")
        check(not any(e.startswith("sub:%d:" % (oracle["lba"]["B"] * 2048)) for e in st["extents"]) and not any(e.startswith("sub:%d:" % (oracle["lba"]["C"] * 2048)) for e in st["extents"]),
              "base: B and C are not among the extents resolved from the build pass's delivery evidence (no execution-derived admission of them)")
        history = [h for h in st["history"]]
        check(len(st["extents"]) == 2 and all(h.get("stop") != "code_bytes_mismatch" for h in history), "base: only the boot IP and SP were resolved from evidence; no round stopped at a module")
        lines = (tmp / "n-base" / "static-extents.txt").read_text().split()
        a_line = [l for l in lines if l.split(":")[1] == str(oracle["lba"]["A"] * 2048 + 0x40)]
        check(len(a_line) == 1 and int(a_line[0].split(":")[3]) == oracle["modules"]["A"]["range"][1],
              "A: the admitted extent starts at its executable subrange (the 64-byte header is outside it)")
        check(len(lines) == 4 and len({l.split(":")[1] for l in lines}) == 3, "B and C: one extent per mapping, three sources (C twice)")
        program = tmp / "n-base" / ("program" + EXE)
        before = hashlib.sha256(program.read_bytes()).hexdigest()

        # ---- the same binary executes the unvisited modules ----
        r0, w0 = run(program, fx, 0)
        check(r0["result"] == "idle" and w0[:3] == [1, 0, 0] and w0[3] == mark["A"], "route 0 (the build-pass route): A runs")
        r2, w2 = run(program, fx, 2)
        check(r2["result"] == "idle" and w2[:3] == [0, 1, 0] and w2[4] == mark["B"], "route 2: B (never loaded by the build) runs from the same binary, same address as A, selected by live bytes")
        r3, w3 = run(program, fx, 3)
        check(r3["result"] == "idle" and w3[:3] == [0, 0, 2] and w3[5] == mark["C"], "route 3: C runs at both verified mappings")
        check(hashlib.sha256(program.read_bytes()).hexdigest() == before, "the binary is unchanged by the runs (nothing is compiled or generated after the build)")

        # ---- control: without the static producer the unvisited module is not compiled ----
        done, ctl = build(fx, tmp / "n-ctl", cache, "--static-discovery", "0")
        cprog = tmp / "n-ctl" / ("program" + EXE)
        check(ctl and ctl["status"] == "ok" and ctl["static_extents"] == 0 and ctl["static_discovery"] == {"ran": False}, "control: --static-discovery 0 reproduces the previous build (no static admission)")
        rc0, wc0 = run(cprog, fx, 0)
        rc2, wc2 = run(cprog, fx, 2)
        check(wc0[:3] == [1, 0, 0] and rc2["stop"]["class"] != "none" and wc2[:3] == [0, 0, 0], "control: route 2 stops (typed) in the build without static admission; B never executes")
        check(ctl["program_identity"] != st["program_identity"], "the static admissions are part of the program identity")

        # ---- determinism and cache ----
        done2, st2 = build(fx, tmp / "n-base2", cache)
        check(st2["program_identity"] == st["program_identity"] and st2["extents"] == st["extents"] and (tmp / "n-base2" / "static-extents.txt").read_text() == (tmp / "n-base" / "static-extents.txt").read_text(),
              "two builds of the same inputs have the same identity, extents and static admissions")

        # ---- changed disc bytes: fail closed ----
        bad = tmp / "disc-bad"
        bad.mkdir()
        data = bytearray((fx / "disc" / "track01.bin").read_bytes())
        at = oracle["lba"]["B"] * 2048 + 0x10
        data[at] ^= 0xFF                                           # a byte inside B's executable range, as the disc delivers it at run time
        (bad / "track01.bin").write_bytes(bytes(data))
        (bad / "disc.cue").write_text((fx / "disc" / "disc.cue").read_text(), newline="\n")
        out = subprocess.run([str(program), "--disc-dir", str(bad), "--bios", str(fx / "bios.bin"), "--instruction-budget", BUDGET, "--set", "main:d7=2"], capture_output=True, text=True, timeout=300)
        rep = json.loads(out.stdout)
        check(rep["result"] == "disc_refused" and rep["error"] == "disc_identity_mismatch" and "result_block" not in rep,
              "a disc whose module bytes differ is refused as a different disc (the program is built for one verified disc image); nothing runs")

        # ---- negative loader cases (the scan proposes only what it proves) ----
        for case, want in (("unknown_lba", "lba_unknown"), ("unknown_dest", "destination_unknown"), ("prg_dest", "destination_unsupported")):
            f2, o2 = fixture(tmp, case)
            done, s2 = build(f2, tmp / ("n-" + case), cache)
            p2 = tmp / ("n-" + case) / ("program" + EXE)
            check(s2 and s2["status"] == "ok" and s2["static_discovery"]["refusals"].get(want) == 1, "%s: the scan refuses the unprovable load (%s) and reports it" % (case, want))
            r2, w2 = run(p2, f2, 2)
            check(w2[:3] == [0, 1, 0] and w2[4] == o2["markers"]["B"], "%s: the independently proven module B still runs" % case)
            r3, w3 = run(p2, f2, 3)
            check((r3["stop"]["class"] != "none" or r3["result"] == "budget_exhausted") and w3[:3] == [0, 0, 0],
                  "%s: the unproven mapping fails closed (a typed stop, or the loader never completes its own transfer; nothing of C executes)" % case)

        # ---- SEG-098-T003: the modules sit behind a PC-relative offset-table dispatcher (constant selectors) ----
        fd, od = fixture(tmp, "dispatch")
        done, sd_ = build(fd, tmp / "n-dsp", cache)
        pd = tmp / "n-dsp" / ("program" + EXE)
        check(sd_ and sd_["status"] == "ok" and sd_["static_discovery"]["status"] == "admitted" and sd_["static_extents"] == 4 and len(sd_["extents"]) == 2,
              "dispatch: the static scan resolves the table dispatch and admits A, B and C before any pass (only the IP and SP are resolved from evidence)")
        for choice, want_counts, idx in ((0, [1, 0, 0], 3), (2, [0, 1, 0], 4), (3, [0, 0, 2], 5)):
            rd, wd = run(pd, fd, choice)
            check(rd["result"] == "idle" and wd[:3] == want_counts and wd[idx] == od["markers"]["ABC"[want_counts.index(max(want_counts))]], "dispatch: route %d runs its module from the same binary" % choice)
        fu, ou = fixture(tmp, "dispatch_unknown")
        done, su = build(fu, tmp / "n-dsu", cache)
        pu = tmp / "n-dsu" / ("program" + EXE)
        check(su and su["static_discovery"]["refusals"] == {"computed_control_unresolved": 1} and su["static_extents"] == 2, "dispatch_unknown: A and B admitted, the unresolved computed call is reported (typed), C is not admitted")
        rb, wb = run(pu, fu, 2)
        rc, wc = run(pu, fu, 3)
        check(wb[:3] == [0, 1, 0] and wc[2] == 0 and wc[5] == 0,
              "dispatch_unknown: B runs; C never executes (the register-fed selector dispatches to a compiled module - here A - or stops; it never reaches uncompiled code)")

        # ---- mutable data outside an instruction's range, and a changed instruction inside it ----
        f3, o3 = fixture(tmp, "header_patched")
        done, s3 = build(f3, tmp / "n-hp", cache)
        r, w = run(tmp / "n-hp" / ("program" + EXE), f3, 0)
        check(s3["stop"] == "none" and w[:3] == [1, 0, 0] and w[3] == o3["markers"]["A"], "header_patched: a loader write into A's non-executable header leaves A valid and running")
        f4, o4 = fixture(tmp, "overwritten")
        done, s4 = build(f4, tmp / "n-ow", cache)
        r, w = run(tmp / "n-ow" / ("program" + EXE), f4, 0)
        check(r["stop"]["class"] == "code_bytes_mismatch" and w[3] == 0, "overwritten: a word inside A's executable range changed after the load: typed stop code_bytes_mismatch, the patched instruction is not run")

        # ---- no manifest anywhere ----
        usage = subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(fx / "disc" / "disc.cue"), "--modules", "x"], capture_output=True, text=True)
        check(usage.returncode == 2 or usage.returncode != 0, "the build accepts no module manifest option")
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
