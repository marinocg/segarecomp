#!/usr/bin/env python3
"""SEG-099-T002 (ADR 0216): a loader whose arguments are carried in a RAM work area and the documented BIOS CD services (ADR 0215), found by the static scan.

Project-authored BIOS image + disc (tools/sega_cd_bios_services_fixture.py, tools/sega_cd_services_sp_fixture.py). The disc SP is a file engine: it stores the
destination in a work variable, reads the ISO9660 volume descriptor and root directory through ROMREADN/CDCSTAT/CDCREAD/CDCTRN/CDCACK, finds the file by NAME, and loads it
with a sector loop whose pointer, sector and count live in the work area; the entry is read back from memory. The build-pass route loads module A only; module B (selected by the
machine input) is never loaded by it. The scan (disc + boot images; no module list, no trace, no execution) must prove both, the supported build must compile B before it
ever runs, and the SAME binary must run it. Adversarial cases must NOT produce a module. usage: sega_cd_services_loader_test.py <segarecomp> <c-compiler> <source-root>
"""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
FIXTURE = ROOT / "tools" / "sega_cd_bios_services_fixture.py"
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


def scan(fx, *flags):
    done = subprocess.run([SEGARECOMP, "sega-cd", "discover-modules", "--cue", str(fx / "disc" / "disc.cue"), "--module-table-roots", "--constant-dispatch", *flags], capture_output=True, text=True, timeout=300)
    return json.loads(done.stdout)


def build(fx, out, cache, *extra):
    done = subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(fx / "disc" / "disc.cue"), "--bios", str(fx / "bios.bin"), "--output", str(out), "--runtime-dir", str(ROOT),
                           "--cc", CC, "--budget", BUDGET, "--jobs", "4", "--cache-dir", str(cache), *extra], capture_output=True, text=True, timeout=1800)
    status = out / "status.json"
    return done, json.loads(status.read_text()) if status.exists() else None


def run(program, fx, choice=0, disc=None):
    extra = ["--set", "main:d7=%d" % choice] if choice else []
    out = subprocess.run([str(program), "--disc-dir", str(disc or fx / "disc"), "--bios", str(fx / "bios.bin"), "--instruction-budget", BUDGET, *extra], capture_output=True, text=True, timeout=300)
    report = json.loads(out.stdout)
    block = bytes.fromhex(report.get("result_block", "")).ljust(16, b"\0")
    return report, [int.from_bytes(block[2 * i:2 * i + 2], "big") for i in range(8)]


def mods(report):
    return {m["block"]: m for m in report["modules"]}


def refusals(report):
    out = {}
    for r in report["refusals"]:
        out[r["reason"]] = out.get(r["reason"], 0) + 1
    return out


def main():
    with tempfile.TemporaryDirectory(prefix="scd-services-") as tmp:
        tmp = pathlib.Path(tmp)
        cache = tmp / "objects"
        fx, oracle = fixture(tmp, "base")
        A, B = oracle["modules"]["A"], oracle["modules"]["B"]

        # ---- the scan: control (no work-variable model) versus the model ----
        off = scan(fx)
        check(off["modules"] == [] and off["survey"]["sector_transfers"] == 0, "control: without the work-variable model the scan proves nothing on this loader (its arguments are memory-carried)")
        on = scan(fx, "--work-state")
        m = mods(on)
        check(on["complete"] and set(m) == {A["lba"], B["lba"]}, "base: the model proves both modules by name through the directory: A and B (no module list, no LBA constant anywhere in the SP)")
        check(m[A["lba"]]["size"] == 4096 and m[A["lba"]]["bases"] == A["bases"] and m[A["lba"]]["entries"] == A["entries"], "A: a TWO-sector run at its proven destination with the entry carried through memory")
        check(m[A["lba"]]["range"]["offset"] == A["entries"][0] and m[A["lba"]]["range"]["length"] == A["far"] - A["entries"][0] + A["far_bytes"],
              "A: the executable subrange spans the sector boundary (the routine in sector 1 is reached by the traversal)")
        check(m[B["lba"]]["size"] == 2048 and m[B["lba"]]["bases"] == B["bases"] and m[B["lba"]]["entries"] == [0], "B: one sector at its proven destination")
        sv = on["survey"]
        check(sv["bios_modelled_calls"] >= 6 and sv["bios_unmodelled_calls"] == 0 and sv["sector_transfers_unresolved"] == 0 and sv["memory_args_resolved"] >= 1 and sv["memory_args_unresolved"] == 0,
              "survey: every BIOS service was a modelled one; the {sector,count} block was read from tracked memory; no transfer was unresolved")
        check(set(refusals(on)) <= {"loaded_not_executed"}, "the only refusals are loaded-but-never-executed data sectors (the volume descriptor and the directory)")
        check(json.dumps(scan(fx, "--work-state"), sort_keys=True) == json.dumps(on, sort_keys=True), "determinism: two scans are identical")

        # ---- adversarial cases: the model must not invent a module ----
        expect = {
            "corrupt_pvd": (set(), "a corrupt volume descriptor: the loader's own check fails: nothing is loaded from the directory"),
            "corrupt_extent": ({A["lba"]}, "a directory record whose extent leaves the track is refused, not clamped"),
            "no_exec": ({B["lba"]}, "a module that is loaded but never executed is data, not a module"),
            "entry_unknown": ({B["lba"]}, "the source is proven but the execution mapping (the entry) is not: refused"),
        }
        for case, why in (("alias", "an unknown store through a computed pointer may alias the work area"),
                          ("unmodelled", "a BIOS service with unmodelled effects forgets the work area"),
                          ("stale_service", "an unsupported service number is never a successful load and forgets the work area")):
            f2, o2 = fixture(tmp, case)
            r = scan(f2, "--work-state")
            check(A["lba"] not in mods(r) and "destination_unknown" in refusals(r), "%s: %s: A's destination is unproven and the typed refusal destination_unknown names it" % (case, why))
            check(not r["complete"] or B["lba"] in mods(r), "%s: the result is either complete with B proven, or reported incomplete - never a silent partial inventory" % case)
        for case, (want, why) in expect.items():
            f2, o2 = fixture(tmp, case)
            r = scan(f2, "--work-state")
            check(set(mods(r)) == want, "%s: %s" % (case, why))
        r = scan(fixture(tmp, "corrupt_extent")[0], "--work-state")
        check(refusals(r).get("source_outside_track", 0) == 1, "corrupt_extent: the typed refusal is source_outside_track")
        r = scan(fixture(tmp, "no_exec")[0], "--work-state")
        check(refusals(r).get("loaded_not_executed", 0) >= 3, "no_exec: the typed refusal is loaded_not_executed")
        r = scan(fixture(tmp, "entry_unknown")[0], "--work-state")
        check("entry_unknown" in refusals(r), "entry_unknown: the typed refusal is entry_unknown")
        # ---- a CDCTRN whose effects the walk cannot bound must not leave stale knowledge (review follow-up) ----
        for case, proven, why in (("trn_disjoint", True, "fully known, disjoint buffers: the earlier knowledge is retained and A is proven"),
                                  ("trn_dev", True, "an unknown device byte with known disjoint buffers: CDCTRN needs the sub-read device, the known buffers are not written, A is retained"),
                                  ("trn_lba", True, "an unknown read position with known disjoint buffers: only those buffers are forgotten, A is retained"),
                                  ("trn_a0", False, "an unknown data-buffer pointer may overwrite the stored base: everything tracked is forgotten, A is not proven"),
                                  ("trn_lost", False, "the walk lost its read state through a documented drive call and CDCTRN then aims at the stored base: it may still write, so the stored base is forgotten and A is not proven"),
                                  ("trn_hdr", False, "an unknown header-buffer pointer: everything tracked is forgotten and the boot image is no longer trusted, A is not proven")):
            ft, ot = fixture(tmp, case)
            r = scan(ft, "--work-state", "--state-convergence")
            check((A["lba"] in mods(r)) == proven and (B["lba"] in mods(r) or case == "trn_lost") and r["complete"], "%s: %s%s" % (case, why, "" if case == "trn_lost" else "; the independent module B is proven"))
        fh, oh = fixture(tmp, "hdr_alias")
        r = scan(fh, "--work-state", "--state-convergence")
        check(set(mods(r)) == set() and "destination_unknown" in refusals(r), "hdr_alias: a CDCTRN header pointer whose ignored high byte aliases the base variable overwrites it: the destination is unknown, nothing is proven (A1 is masked to the 24-bit bus)")
        r = scan(fixture(tmp, "unmodelled")[0], "--work-state")
        check(r["survey"]["bios_unmodelled_calls"] == 1, "unmodelled: the unmodelled service is counted")
        f2, o2 = fixture(tmp, "two_paths")
        r = scan(f2, "--work-state")
        check(set(mods(r)) == {A["lba"], B["lba"], o2["modules"]["C"]["lba"]} and all(len(x["bases"]) == 1 for x in r["modules"]),
              "two_paths: three different file selectors on three paths give three modules - never a merged or invented fourth")

        # ---- the supported build and launcher ----
        done, st = build(fx, tmp / "n-base", cache)
        check(done.returncode == 0 and st and st["status"] == "ok" and st["stop"] == "none" and st["result"] == "idle", "build: the supported build succeeds and the build pass ends idle")
        sd = st["static_discovery"]
        check(sd["ran"] and sd["status"] == "admitted" and sd["complete"] and st["static_extents"] == 2, "build: the scan admitted 2 extents (A, B) before any pass")
        check(not any(e.startswith("sub:%d:" % (B["lba"] * 2048)) for e in st["extents"]), "build: B is not among the extents resolved from the build pass's delivery evidence")
        lines = (tmp / "n-base" / "static-extents.txt").read_text().split()
        a_line = [x for x in lines if x.split(":")[1] == str(A["lba"] * 2048 + A["entries"][0])]
        check(len(a_line) == 1 and int(a_line[0].split(":")[3]) == A["far"] - A["entries"][0] + A["far_bytes"], "build: A is admitted as ONE extent covering both of its sectors' executable bytes")
        program = tmp / "n-base" / ("program" + EXE)
        before = hashlib.sha256(program.read_bytes()).hexdigest()
        r0, w0 = run(program, fx, 0)
        check(r0["result"] == "idle" and w0[:3] == [1, 0, 0] and w0[3] == oracle["markers"]["A"], "route 0 (the build-pass route): A runs, including its routine in the second sector")
        r2, w2 = run(program, fx, 2)
        check(r2["result"] == "idle" and w2[:3] == [0, 1, 0] and w2[4] == oracle["markers"]["B"], "route 2: B (never loaded by the build pass) runs from the same binary")
        check(hashlib.sha256(program.read_bytes()).hexdigest() == before, "the binary is unchanged by the runs")
        done, ctl = build(fx, tmp / "n-ctl", cache, "--static-discovery", "0")
        rc2, wc2 = run(tmp / "n-ctl" / ("program" + EXE), fx, 2)
        check(rc2["stop"]["class"] != "none" and wc2[:3] == [0, 0, 0], "control: without the static producer route 2 stops (typed); B never executes")
        done2, st2 = build(fx, tmp / "n-base2", cache)
        check(st2["program_identity"] == st["program_identity"] and st2["extents"] == st["extents"], "determinism: two builds have the same identity and extents")
        bad = tmp / "disc-bad"
        bad.mkdir()
        data = bytearray((fx / "disc" / "track01.bin").read_bytes())
        data[B["lba"] * 2048 + 0x10] ^= 0xFF
        (bad / "track01.bin").write_bytes(bytes(data))
        (bad / "disc.cue").write_text((fx / "disc" / "disc.cue").read_text(), newline="\n")
        out = subprocess.run([str(program), "--disc-dir", str(bad), "--bios", str(fx / "bios.bin"), "--instruction-budget", BUDGET, "--set", "main:d7=2"], capture_output=True, text=True, timeout=300)
        rep = json.loads(out.stdout)
        check(rep["result"] == "disc_refused" and "result_block" not in rep, "a disc whose module bytes differ is refused as a different disc; nothing runs")

        # ---- provenance of the static_disc_extent admission (SEG-099-T005, ADR 0219) ----
        def emit_with(spec):
            done = subprocess.run([SEGARECOMP, "sega-cd", "emit", "--cue", str(fx / "disc" / "disc.cue"), "--bios", str(fx / "bios.bin"), "--out", str(tmp / "emit.c"), "--admit", "static-extent:" + spec],
                                  capture_output=True, text=True, timeout=300)
            return done.returncode, done.stdout
        good = lines[0]
        rc, out = emit_with(good)
        check(rc == 0, "provenance: an extent the scan proved is admitted by the emitter")
        off_, base_, len_ = (int(x) for x in good.split(":")[1:])
        rc, out = emit_with("sub:%d:%d:%d" % (oracle["pvd_lba"] * 2048, 0x90000, 2048))
        check(rc != 0 and "static_extent_unproven" in out, "provenance: a coordinate string naming a sector no loader proof covers (the volume descriptor as code) is refused: it is not a static result")
        rc, out = emit_with("sub:%d:%d:%d" % (off_, base_ + 0x2000, len_))
        check(rc != 0 and "static_extent_unproven" in out, "provenance: a proven source at a guest mapping the scan did not prove is refused")
        rc, out = emit_with("sub:%d:%d:%d" % (off_, base_, 6 * 2048))
        check(rc != 0 and "static_extent_unproven" in out, "provenance: an extent that runs past the proven source is refused")

        # ---- the declared executable RANGE must be a producer-authorized extent, not any subrange of a proven source (review follow-up) ----
        def producer(scope, *flags):
            out = tmp / ("ext-" + scope)
            subprocess.run([SEGARECOMP, "sega-cd", "discover-modules", "--cue", str(fx / "disc" / "disc.cue"), "--module-table-roots", "--constant-dispatch", "--work-state",
                            "--recursive-modules", "--state-convergence", "--static-extents", str(out), "--static-scope", scope, *flags], capture_output=True, text=True, timeout=300)
            return out.read_text().split()
        hull_lines, loaded_lines = producer("hull"), producer("loaded")
        a_off = A["lba"] * 2048
        hull_a = [l for l in hull_lines if int(l.split(":")[1]) >= a_off and int(l.split(":")[1]) < a_off + 4096][0]
        loaded_a = [l for l in loaded_lines if int(l.split(":")[1]) == a_off][0]
        check(all(emit_with(l)[0] == 0 for l in hull_lines + loaded_lines), "range authority: every extent either producer policy emits (hull and explicit loaded) is admitted exactly")
        ho, hb, hl = (int(x) for x in hull_a.split(":")[1:])
        lo_, lb_, ll_ = (int(x) for x in loaded_a.split(":")[1:])
        check(ho > lo_, "range authority: the fixture's module has a non-code header (hull starts after the loaded range)")
        for label, spec in (("a subrange inside the source but outside the hull (the header)", "sub:%d:%d:%d" % (lo_, lb_, ho - lo_)),
                            ("a partial range crossing the hull boundary", "sub:%d:%d:%d" % (ho - 16, hb - 16, 64)),
                            ("a strict subrange inside the hull", "sub:%d:%d:%d" % (ho, hb, hl - 8)),
                            ("the hull start extended past the hull end", "sub:%d:%d:%d" % (ho, hb, hl + 16)),
                            ("a valid source mapped at an unproven guest base", "sub:%d:%d:%d" % (ho, hb + 0x4000, hl))):
            rc, out = emit_with(spec)
            check(rc != 0 and "static_extent_unproven" in out, "range authority: %s is refused (static_extent_unproven)" % label)

        # ---- a mapped executable image that changes after loading ----
        f4, o4 = fixture(tmp, "late_patch")
        done, s4 = build(f4, tmp / "n-lp", cache)
        r, w = run(tmp / "n-lp" / ("program" + EXE), f4, 0)
        check(r["stop"]["class"] == "code_bytes_mismatch" and w[3] == 0, "late_patch: a word inside A's executable range changed after the load: typed stop code_bytes_mismatch, the patched instruction is not run")
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
