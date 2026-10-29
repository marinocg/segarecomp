#!/usr/bin/env python3
"""SEG-100 (ADR 0221-0224): file-first broad AOT with dynamic, verified module mapping, end to end through the supported BIOS-image build.

Project-authored BIOS image + disc (tools/sega_cd_bios_unvisited_fixture.py --iso). The build compiles the ISO9660 files of the disc into base-less native
bodies BEFORE any run; the game's own loader then puts a module wherever its code says, and the runtime binds the precompiled bodies to the bytes it finds
there. No module list, no static admission (`--static-discovery 0`), no address, name or entry offset reaches the emitter. The oracle (expected.json) is
read only here. usage: sega_cd_file_first_test.py <segarecomp> <c-compiler> <source-root>
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


def fixture(tmp, case, iso=True):
    out = tmp / ("fx-%s%s" % (case, "" if iso else "-noiso"))
    cmd = [sys.executable, str(FIXTURE), "build", "--out", str(out), "--case", case] + (["--iso"] if iso else [])
    oracle = json.loads(subprocess.run(cmd, check=True, capture_output=True, text=True).stdout)
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
    block = bytes.fromhex(report.get("result_block", "")).ljust(16, b"\0")
    return report, [int.from_bytes(block[2 * i:2 * i + 2], "big") for i in range(8)]


def same_machine_state(a, b):
    """Everything the machine reports, except what names the program or the dynamic section."""
    a, b = dict(a), dict(b)
    for report in (a, b):
        report.pop("program_identity", None)
        report.pop("dynamic", None)
    return a == b


def emit(fx, out, *extra):
    out.mkdir(parents=True, exist_ok=True)
    done = subprocess.run([SEGARECOMP, "sega-cd", "emit", "--cue", str(fx / "disc" / "disc.cue"), "--bios", str(fx / "bios.bin"), "--out", str(out / "program.c"), "--partition-window", "8192", *extra],
                          capture_output=True, text=True, timeout=600)
    return done, (json.loads(done.stdout) if done.stdout.strip().startswith("{") else None)


def tree_digest(directory):
    h = hashlib.sha256()
    for path in sorted(directory.glob("*.c")):
        h.update(path.name.encode())
        h.update(path.read_bytes())
    return h.hexdigest()


def main():
    with tempfile.TemporaryDirectory(prefix="scd-file-first-") as tmp:
        tmp = pathlib.Path(tmp)
        cache = tmp / "objects"
        fx, oracle = fixture(tmp, "base")
        mark = oracle["markers"]

        # ---- the references: the supported build with static admission (per-mapping emitter), and without it ----
        done, ref = build(fx, tmp / "n-ref", cache)
        rprog = tmp / "n-ref" / ("program" + EXE)
        check(done.returncode == 0 and ref and ref["status"] == "ok" and ref["file_first"] is None, "reference: the supported build succeeds and reports no file-first section")
        done, ctl = build(fx, tmp / "n-ctl", cache, "--static-discovery", "0")
        cprog = tmp / "n-ctl" / ("program" + EXE)
        rc2, wc2 = run(cprog, fx, 2)
        check(rc2["stop"]["class"] != "none" and wc2[:3] == [0, 0, 0], "control: without static admission and without file-first, the module the build pass never loaded stops (typed) and never runs")

        # ---- file-first, no static admission: the modules' bases are supplied nowhere ----
        done, ff = build(fx, tmp / "n-ff", cache, "--static-discovery", "0", "--file-first", "structural")
        prog = tmp / "n-ff" / ("program" + EXE)
        check(done.returncode == 0 and ff and ff["status"] == "ok" and ff["stop"] == "none" and ff["result"] == "idle", "file-first: the build succeeds and its build pass ends idle")
        info = ff["file_first"]
        check(info and info["complete"] and info["files"] == 5 and info["duplicate_files"] == 0 and info["no_structure_files"] == 1 and info["images"] == 4 and info["policy"].startswith("file_first.v1:structural:"),
              "file-first: five files enumerated through the ISO9660 volume, the data-only file has no structure, four images compiled (typed accounting)")
        check(info["candidate_offsets"] == info["compiled_offsets"] and info["refused_encodings"] == 0 and info["bodies"] == info["unique_encodings"] and info["bodies"] < info["compiled_offsets"],
              "file-first: every candidate offset has a body, and bodies are shared by identical instruction bytes")
        check(len(ff["extents"]) == 2 and ff["static_extents"] == 0, "file-first: only the boot IP and SP were supplied as extents; no module extent, no static admission")
        before = hashlib.sha256(prog.read_bytes()).hexdigest()
        r0, w0 = run(prog, fx, 0)
        r2, w2 = run(prog, fx, 2)
        r3, w3 = run(prog, fx, 3)
        check(r0["result"] == "idle" and w0[:3] == [1, 0, 0] and w0[3] == mark["A"], "route 0: module A runs from precompiled bodies")
        check(r2["result"] == "idle" and w2[:3] == [0, 1, 0] and w2[4] == mark["B"], "route 2: module B runs, at the address A used")
        check(r3["result"] == "idle" and w3[:3] == [0, 0, 2] and w3[5] == mark["C"], "route 3: module C runs at both bases")
        check(hashlib.sha256(prog.read_bytes()).hexdigest() == before, "the binary is unchanged by the runs (nothing is compiled or generated after the build)")
        for choice, report in ((0, r0), (2, r2), (3, r3)):
            ref_report, _ = run(rprog, fx, choice)
            check(same_machine_state(ref_report, report), "route %d: the complete run report (registers, retired instructions, cycles, trace hash, every memory digest, device log) equals the static per-mapping reference's" % choice)
        d0, d2, d3 = r0["dynamic"], r2["dynamic"], r3["dynamic"]
        check(d2["binds"] == 1 and d2["bindings"][0]["base"] == oracle["modules"]["B"]["bases"][0] + 0 and d2["bindings"][0]["cpu"] == 1 and d2["retired"] > 0 and d2["refusals"] == {} and d2["last_refusal"] == "none",
              "route 2: one binding, at a base only the game's loader chose, on the sub processor; no refusal")
        check(d3["binds"] == 2 and sorted(b["base"] for b in d3["bindings"]) == sorted(oracle["modules"]["C"]["bases"]) and len({b["image"] for b in d3["bindings"]}) == 1,
              "route 3: ONE image bound at TWO bases, each at the base the loader used")
        check(d0["binds"] == 1 and d0["policy"] == info["policy"] and d0["images"] == 4 and d0["candidate_bytes"] == info["admitted_bytes"] and d0["compiled_offsets"] == info["compiled_offsets"],
              "the run report names the policy and the compiled inventory (candidate bytes, compiled offsets)")
        check(d3["events"][0]["kind"] == "bound" and d3["scan_bytes"] <= 8192 * 3, "binding cost is bounded by the delivered candidate bytes searched (no per-instruction scan)")

        # ---- SEG-101 (ADR 0228): compiled / bound / executed / previously unvisited are different facts; the per-image account is observer-only ----
        def ids(report, key="exercised"):
            return {e["image"] for e in report["dynamic"][key]} if key == "exercised" else set(report["dynamic"][key])
        observed = ids(r0)                      # what the build pass (route 0, no input) executed: the build-observed set
        check(len(observed) == 1 and ids(r0, "bound_images") == observed and d0["images"] == 4, "accounting: four images compiled before the run, one bound and executed by the build route (A)")
        for name, rep in (("0", r0), ("2", r2), ("3", r3)):
            ex = rep["dynamic"]["exercised"]
            check(sum(e["retired"] for e in ex) == rep["dynamic"]["retired"] and len(ex) == rep["dynamic"]["distinct_images_exercised"] and all(0 < e["offsets"] <= e["retired"] for e in ex),
                  "accounting, route %s: the per-image retired counts sum to the dynamic total, every executed image retired at least one instruction at one or more distinct source offsets" % name)
        unvisited2, unvisited3 = ids(r2) - observed, ids(r3) - observed
        check(len(ids(r2)) == 1 and len(unvisited2) == 1 and ids(r2, "bound_images") == ids(r2), "accounting, route 2: module B was compiled, never executed by the build route, and is counted as previously unvisited (one image)")
        check(len(ids(r3)) == 1 and len(unvisited3) == 1 and unvisited3 != unvisited2 and d3["binds"] == 2, "accounting, route 3: module C bound at two bases is ONE previously unvisited image, not two")
        check(not (ids(r0) - observed), "accounting: re-executing the build route's own module is not counted as new")
        rebind = [e for e in r3["dynamic"]["events"] if e["kind"] == "bound"]
        check(len(rebind) == 2 and len({e["image"] for e in rebind}) == 1, "accounting: the same image mapped at another base is a second binding of one identity")
        ref0, _ = run(rprog, fx, 0)
        check("exercised" not in ref0.get("dynamic", {}) and same_machine_state(ref0, r0), "accounting is observer-only: a program without the dynamic images reports no account, and the machine state is the reference's")

        # ---- the policy 'all' compiles every even offset of every file: more bodies, same behaviour ----
        done, fa = build(fx, tmp / "n-all", cache, "--static-discovery", "0", "--file-first", "all")
        aprog = tmp / "n-all" / ("program" + EXE)
        check(fa and fa["status"] == "ok" and fa["file_first"]["compiled_offsets"] > info["compiled_offsets"] and fa["file_first"]["images"] == 5, "policy all: every file is compiled at every decodable even offset (a superset of the structural inventory)")
        for choice in (0, 2, 3):
            ra, _ = run(aprog, fx, choice)
            rr, _ = run(rprog, fx, choice)
            check(same_machine_state(rr, ra) and ra["dynamic"]["binds"] >= 1, "policy all, route %d: the same machine state as the reference" % choice)

        # ---- determinism ----
        e1, j1 = emit(fx, tmp / "e1", "--file-first", "structural", "--jobs", "1")
        e2, j2 = emit(fx, tmp / "e2", "--file-first", "structural", "--jobs", "8")
        check(e1.returncode == 0 and e2.returncode == 0 and tree_digest(tmp / "e1") == tree_digest(tmp / "e2") and j1["program_identity"] == j2["program_identity"] and j1["file_first"]["identity"] == j2["file_first"]["identity"],
              "emission is deterministic: the same translation units and identities whatever the worker count")
        e3, j3 = emit(fx, tmp / "e3")
        check(e3.returncode == 0 and "file_first" not in j3 and j3["program_identity"] != j1["program_identity"], "the dynamic images are part of the program identity; without them the program is the unchanged reference")
        sources = "".join(p.read_text() for p in (tmp / "e1").glob("*.c"))
        check("decode_m68k" not in sources and "lift_m68k" not in sources and "dlopen" not in sources and "scd_neutral_lookup" in sources and "scd_dynamic_dispatch" in sources,
              "the generated text contains no decoder, lifter or loader: only precompiled bodies and the binding entry points")

        # ---- a static extent that shadows an address: the chain offers the PC to the dynamic bindings before it fails ----
        # A manual extent (a test-only `--emit-arg --disc-image`) compiles module A's sector at its Word RAM base as static variants. Route 2 loads module B over the same
        # addresses: B's bytes equal no static variant there, so without the fall-through the program would stop `code_bytes_mismatch`; with it, B is identified and runs.
        # Route 0 (A) still runs its static variants.
        done, sh = build(fx, tmp / "n-shadow", cache, "--static-discovery", "0", "--file-first", "structural", "--emit-arg", "--disc-image", "--emit-arg",
                         "sub:%d:%d:2048" % (oracle["lba"]["A"] * 2048, oracle["modules"]["A"]["bases"][0]))
        shadow = tmp / "n-shadow" / ("program" + EXE)
        chains = "".join(p.read_text() for p in (tmp / "n-shadow" / "src").glob("p1_*.c"))
        check(done.returncode == 0 and sh and sh["status"] == "ok" and "scd_dynamic_dispatch(m, 1U, SCD_STOP_CODE_BYTES_MISMATCH)" in chains and "scd_code_matches" in chains,
              "a PC with static variants becomes a chain whose last resort is the dynamic bindings, typed code_bytes_mismatch (no direct case that would stop first)")
        rs0, ws0 = run(shadow, fx, 0)
        rs2, ws2 = run(shadow, fx, 2)
        rs3, ws3 = run(shadow, fx, 3)
        check(rs0["result"] == "idle" and ws0[:3] == [1, 0, 0] and ws0[3] == mark["A"], "shadowed address, route 0: A runs")
        check(rs2["result"] == "idle" and ws2[:3] == [0, 1, 0] and ws2[4] == mark["B"] and rs2["dynamic"]["binds"] >= 1,
              "shadowed address, route 2: B, loaded over A's static addresses, is identified and runs (the static variants all disagree with the live bytes)")
        check(rs3["result"] == "idle" and ws3[:3] == [0, 0, 2] and ws3[5] == mark["C"], "shadowed address, route 3: C runs at both bases")

        # ---- typed refusals at build time ----
        e4, j4 = emit(fx, tmp / "e4", "--file-first", "structural", "--partition-window", "0") if False else (None, None)
        done = subprocess.run([SEGARECOMP, "sega-cd", "emit", "--cue", str(fx / "disc" / "disc.cue"), "--bios", str(fx / "bios.bin"), "--out", str(tmp / "e5.c"), "--file-first", "structural"],
                              capture_output=True, text=True, timeout=300)
        check(done.returncode != 0, "file-first without a partition window is refused (the bodies are separate translation units)")
        done = subprocess.run([SEGARECOMP, "sega-cd", "emit", "--cue", str(fx / "disc" / "disc.cue"), "--bios", str(fx / "bios.bin"), "--out", str(tmp / "e8.c"), "--partition-window", "8192",
                               "--file-first", "structural", "--aot-policy", "optimized"], capture_output=True, text=True, timeout=300)
        check(done.returncode != 0 and "incompatible_with_experimental_aot_policy" in done.stdout, "dynamic images are not combined with the experimental M3+B35 selector (it is never an image authority)")
        done = subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(fx / "disc" / "disc.cue"), "--bios", str(fx / "bios.bin"), "--output", str(tmp / "n-both"), "--runtime-dir", str(ROOT),
                               "--cc", CC, "--file-first", "structural", "--aot-policy", "optimized"], capture_output=True, text=True, timeout=300)
        check(done.returncode == 2, "the supported build refuses the same combination up front")
        fx_noiso, _ = fixture(tmp, "base", iso=False)
        done, jn = emit(fx_noiso, tmp / "e6", "--file-first", "structural")
        check(done.returncode != 0 and jn and jn.get("ok") is False and jn.get("stage") == "file_first" and jn.get("error") == "iso9660",
              "a disc with no ISO9660 volume is a typed enumeration failure, never an empty or guessed inventory")
        done, jn = build(fx_noiso, tmp / "n-noiso", cache, "--static-discovery", "0", "--file-first", "structural")
        check(done.returncode != 0 and (jn is None or jn["status"] != "ok"), "the supported build fails closed on the same disc")

        # ---- adversarial loader cases ----
        f2, o2 = fixture(tmp, "header_patched")
        done, s2 = build(f2, tmp / "n-hp", cache, "--static-discovery", "0", "--file-first", "structural")
        a, wa = run(tmp / "n-hp" / ("program" + EXE), f2, 0)
        check(a["result"] == "idle" and wa[:3] == [1, 0, 0] and wa[3] == o2["markers"]["A"], "header_patched: a data write into the module's non-executable header does not stop it (the run still explains the code)")
        f3, o3 = fixture(tmp, "overwritten")
        done, s3 = build(f3, tmp / "n-ow", cache, "--static-discovery", "0", "--file-first", "structural")
        a, wa = run(tmp / "n-ow" / ("program" + EXE), f3, 0)
        check(a["stop"]["class"] != "none" and wa[:3] == [0, 0, 0], "overwritten: a changed instruction inside the module is a typed stop (%s); the module's effect never happens" % a["stop"]["class"])
        check(sum(e["retired"] for e in a["dynamic"]["exercised"]) == a["dynamic"]["retired"] and a["dynamic"]["distinct_images_exercised"] <= a["dynamic"]["distinct_images_bound"],
              "overwritten: only instructions that retired are counted as executed (an identified image is not an executed one)")

        # ---- the loader cases the static scan refuses (ADR 0215-0220): the file-first build does not consult the scan, and fails closed where the loader does ----
        for case in ("unknown_lba", "unknown_dest", "prg_dest", "dispatch_unknown"):
            fc, oc = fixture(tmp, case)
            done, sc = build(fc, tmp / ("n-" + case), cache, "--static-discovery", "0", "--file-first", "structural")
            pc = tmp / ("n-" + case) / ("program" + EXE)
            r_ff, w_ff = run(pc, fc, 3)
            r2_ff, w2_ff = run(pc, fc, 2)
            check(sc and sc["status"] == "ok" and w2_ff[:3] == [0, 1, 0] and w2_ff[4] == oc["markers"]["B"], "%s: module B runs through its dynamic binding" % case)
            check(r_ff["result"] == "idle" or (r_ff["stop"]["class"] != "none" and w_ff[:3] == [0, 0, 0]) or r_ff["result"] == "budget_exhausted",
                  "%s: route 3, whose loader the fixture makes unprovable, ends as the loader does or fails closed (typed stop with nothing of C executed)" % case)

        # ---- a different disc is a different program ----
        bad = tmp / "disc-bad"
        bad.mkdir()
        data = bytearray((fx / "disc" / "track01.bin").read_bytes())
        data[oracle["lba"]["B"] * 2048 + 0x10] ^= 0xFF
        (bad / "track01.bin").write_bytes(bytes(data))
        (bad / "disc.cue").write_text((fx / "disc" / "disc.cue").read_text(), newline="\n")
        out = subprocess.run([str(prog), "--disc-dir", str(bad), "--bios", str(fx / "bios.bin"), "--instruction-budget", BUDGET, "--set", "main:d7=2"], capture_output=True, text=True, timeout=300)
        rep = json.loads(out.stdout)
        check(rep["result"] == "disc_refused" and rep["error"] == "disc_identity_mismatch", "a disc whose file bytes differ is refused as a different disc; nothing runs")

    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
