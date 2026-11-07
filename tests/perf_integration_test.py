#!/usr/bin/env python3
"""SEG-125 (ADR 0273): the profilers on the real `segarecomp build` pipeline and a generated headless program (project-authored synthetic Genesis images only).

Build: a report is written and valid for a successful, a failing and a parallel build; the profiled build's generated C, status.json, @stage lines and program are identical to the
unprofiled build's. Runtime: the headless report is valid, bounded, and the program's sound summary (audio digest, virtual frames, result kind, stop class) is identical with the
collector on and off; a typed guest stop is `incomplete`; malformed options and an unwritable report path never change the guest's result.
"""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import genesis_z80_fixture_rom as fx  # noqa: E402
import perf_report as pr  # noqa: E402

STOP_IMAGE = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))


def require(cond, message):
    if not cond:
        raise RuntimeError(message)


def spin_rom():
    spin = fx.M68k()
    spin.w(0x7000)
    spin.branch(0x67, "real")
    spin.w(0x4E70)
    spin.label("real")
    spin.label("spin")
    spin.branch(0x60, "spin")
    return fx.build_rom(spin.resolve())


def build(cli, cc, rom, out, extra=(), report=None):
    args = [cli, "build", "--rom", str(rom), "--output", str(out), "--cc", cc, "--runtime-dir", str(ROOT / "platforms" / "genesis"), "--optimize", "0", "--runtime-optimize", "1", *extra]
    if report is not None:
        args += ["--performance-report", str(report)]
    return subprocess.run(args, text=True, capture_output=True)


def tree_digest(directory):
    h = hashlib.sha256()
    for p in sorted(pathlib.Path(directory).rglob("*")):
        if p.is_file():
            h.update(str(p.relative_to(directory)).encode())
            h.update(p.read_bytes())
    return h.hexdigest()


def run_game(exe, budget, env_extra=None):
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "SEGARECOMP_SOUND_SUMMARY": "1", **(env_extra or {})}
    return subprocess.run([str(exe), "--instruction-budget", str(budget)], text=True, capture_output=True, env=env, timeout=120)


def sound_summary(stderr):
    line = next(l for l in stderr.splitlines() if l.startswith("SOUND_SUMMARY"))
    return line


def main():
    cli, cc = sys.argv[1], sys.argv[2]
    with tempfile.TemporaryDirectory(prefix="segarecomp-perf-") as directory:
        tmp = pathlib.Path(directory)
        stop_rom, spin = tmp / "stop.bin", tmp / "spin.bin"
        stop_rom.write_bytes(STOP_IMAGE)
        spin.write_bytes(spin_rom())

        # ---- build report: successful build
        report = tmp / "build.json"
        ok = build(cli, cc, stop_rom, tmp / "on", report=report)
        require(ok.returncode == 0, "profiled build must succeed: " + ok.stdout + ok.stderr)
        d = pr.load(report)
        require(d["kind"] == "build" and d["status"] == "complete" and d["platform"] == "genesis", "build report identity")
        names = [s["name"] for s in d["build"]["stages"]]
        for stage in ("analyze", "generate", "compile", "link"):
            require(stage in names, f"stage {stage} missing from {names}")
        c = d["build"]["compile"]
        require(c["compiled"] > 5 and c["final_marked"] and c["final"] > 0 and c["pass_only"] >= 1 and c["superseded"] == 0, "compile accounting: " + json.dumps(c)[:400])
        require(d["build"]["sizes"]["executable_bytes"] > 0 and d["build"]["sizes"]["generated_c_bytes"] > 0, "sizes")
        require(d["build"]["counts"]["m68k_emissions"] == 1, "one emission")
        require(abs(d["build"]["accounting"]["exclusive_sum_ms"] + d["build"]["accounting"]["unattributed_ms"] - d["build"]["wall_ms"]) < 5.0, "exclusive + unattributed == wall (no double counting)")
        cats = {x["category"] for x in c["categories"]}
        require({"m68k_generated", "runtime", "sound_device"} <= cats, "categories: " + str(cats))
        if sys.platform != "win32":
            require(c["resources"]["compiler_cpu_ms"] is not None and c["resources"]["compiler_peak_rss_kib"] is not None, "POSIX reports compiler CPU and RSS")
        require(str(tmp) not in report.read_text(), "no local path inside a build report")

        # ---- profiling off is the same pipeline
        off = build(cli, cc, stop_rom, tmp / "off")
        require(off.returncode == 0 and not (tmp / "off" / "performance.json").exists(), "unprofiled build")
        require([l for l in off.stdout.splitlines() if l.startswith("@stage")] == [l for l in ok.stdout.splitlines() if l.startswith("@stage")], "launcher progress lines are unchanged")
        def status_without_timing(path):   # the existing status.json carries its own wall-clock members (emit/compile/materialize ms): everything else must match
            data = json.loads((path / "status.json").read_text())
            for key in ("emit_ms", "compile_ms", "materialize_ms"):
                data.get("z80", {}).pop(key, None)
            return data
        require(status_without_timing(tmp / "off") == status_without_timing(tmp / "on"), "status.json unchanged apart from its own timings")
        gen_off = tree_digest(tmp / "off" / "generated") if (tmp / "off" / "generated").is_dir() else None
        gen_on = tree_digest(tmp / "on" / "generated") if (tmp / "on" / "generated").is_dir() else None
        require(gen_off == gen_on, "generated C is byte-identical with and without profiling")
        r_off, r_on = run_game(tmp / "off" / "game", 1000), run_game(tmp / "on" / "game", 1000)
        require(r_off.stdout == r_on.stdout and r_off.returncode == r_on.returncode, "the program behaves identically")

        # ---- privacy: the sanitized forms of REAL reports hold no unit name, path or compiler version line
        script = str(ROOT / "tools" / "perf_report.py")
        for name, source in (("build", report), ("runtime", tmp / "none")):
            if name == "runtime":
                source = tmp / "runtime-for-privacy.json"
                run_game(tmp / "off" / "game", 1000, {"SEGARECOMP_PERF_REPORT": str(source), "SEGARECOMP_PERF_MODE": "detailed"})
            shared = tmp / f"{name}-shared.json"
            res = subprocess.run([sys.executable, "-I", script, "sanitize", str(source), "-o", str(shared)], text=True, capture_output=True)
            require(res.returncode == 0, f"sanitize {name}: " + res.stderr)
            text = shared.read_text()
            require(str(tmp) not in text and "/tmp" not in text and "/home" not in text, f"shareable {name} report holds no path")
            require(".c\"" not in text and "label" not in text and "timeline" not in text and "slowest_units" not in text, f"shareable {name} report holds no unit names or timeline")
            require(subprocess.run([sys.executable, "-I", script, "check-shareable", str(shared)], capture_output=True).returncode == 0, f"{name} shareable check")
            require(subprocess.run([sys.executable, "-I", script, "check-shareable", str(source)], capture_output=True).returncode == 1, f"the private {name} report is not shareable")

        # ---- build report: failure is marked, status.json and exit code preserved
        failed_report = tmp / "failed.json"
        bad = build(cli, str(tmp / "no-such-compiler"), stop_rom, tmp / "bad", report=failed_report)
        require(bad.returncode != 0 and '"status":"failed"' in (tmp / "bad" / "status.json").read_text(), "failure behavior preserved")
        fd = pr.load(failed_report)
        require(fd["status"] == "failed" and fd["failure"]["stage"] == "compile", "failed report names the stage: " + json.dumps(fd.get("failure")))
        require(any(u["metric"] == "unit_disposition" for u in fd["unavailable"]), "unknown dispositions are explicit")
        if sys.platform != "win32":   # a compiler that really runs and fails on one source: the failed job is attempted work, the jobs behind it never start
            wrapper = tmp / "failing-cc.sh"
            wrapper.write_text('#!/bin/sh\ncase "$*" in *runtime.c*) echo "synthetic compile error" >&2; sleep 0.2; exit 1;; esac\nexec %s "$@"\n' % cc)
            wrapper.chmod(0o755)
            part_report = tmp / "partial.json"
            part = build(cli, str(wrapper), stop_rom, tmp / "part", extra=("--jobs", "1"), report=part_report)
            require(part.returncode != 0, "the synthetic compiler failure fails the build")
            pd = pr.load(part_report)
            pc = pd["build"]["compile"]
            require(pd["status"] == "failed" and pd["failure"]["stage"] == "compile", "failed build report")
            require(pc["failed"] == 1 and pc["not_started"] >= 1 and pc["compiled"] >= 1, "failed and never-started jobs are separate from successes: " + json.dumps(pc)[:300])
            require(pc["attempted"] == pc["compiled"] + pc["failed"] and pc["failed_ms"] >= 150 and pc["aggregate_job_ms"] >= pc["failed_ms"], "the failed job's time is attempted work: " + json.dumps(pc)[:300])
            require(pc["final_marked"] is False and pc["final"] == 0 and pc["superseded"] == 0, "no disposition is claimed before the final link")
            require(any(u["disposition"] == "failed" for u in pc["slowest_units"]) and all(u["label"] != "" for u in pc["slowest_units"]), "the failed job is visible among the units")
        unwritable = build(cli, cc, stop_rom, tmp / "unw", report=tmp / "no-dir" / "x" / "r.json")
        require(unwritable.returncode == ok.returncode, "an unwritable report path does not change the build result")

        # ---- parallelism: serial jobs cannot overlap, parallel jobs overlap
        serial, parallel = tmp / "serial.json", tmp / "parallel.json"
        require(build(cli, cc, stop_rom, tmp / "j1", extra=("--jobs", "1"), report=serial).returncode == 0, "jobs=1 build")
        require(build(cli, cc, stop_rom, tmp / "j4", extra=("--jobs", "4"), report=parallel).returncode == 0, "jobs=4 build")
        s, p = pr.load(serial), pr.load(parallel)
        sc, pc = s["build"]["compile"], p["build"]["compile"]
        require(s["config"]["jobs"] == 1 and p["config"]["jobs"] == 4, "jobs recorded")
        require(sc["aggregate_job_ms"] <= sc["batch_wall_ms"] * 1.15 + 5, "serial jobs: aggregate ~ wall (not treated as parallel): " + json.dumps({k: sc[k] for k in ("aggregate_job_ms", "batch_wall_ms")}))
        require(pc["max_workers"] >= 2 and pc["aggregate_job_ms"] >= pc["batch_wall_ms"], "parallel jobs: aggregate >= wall")
        require(any("jobs" in w for w in pr.comparability(s, p)), "differing concurrency is a comparability warning")

        # ---- runtime: headless report + on/off equivalence
        require(build(cli, cc, spin, tmp / "spin-out", extra=("--optimize", "1")).returncode == 0, "spin build")
        game = tmp / "spin-out" / "game"
        off_run = run_game(game, 3_000_000)
        runtime_report = tmp / "runtime.json"
        on_run = run_game(game, 3_000_000, {"SEGARECOMP_PERF_REPORT": str(runtime_report), "SEGARECOMP_PERF_MODE": "detailed"})
        require(off_run.returncode == on_run.returncode and off_run.stdout == on_run.stdout, "profiled run: same exit code and stdout")
        require(sound_summary(off_run.stderr) == sound_summary(on_run.stderr), "profiled run: sound summary (audio digest, frames, result and stop class) identical")
        rd = pr.load(runtime_report)
        r = rd["runtime"]
        require(rd["status"] == "complete" and r["outcome"] == "runner_resource_limit" and rd["config"]["entry"] == "headless" and rd["config"]["paced"] is False, "runtime identity")
        guest_frames = json.loads(sound_summary(on_run.stderr).split(" ", 1)[1])["virtual_frames"]
        require(r["frames"]["virtual"] == guest_frames > 100, f"every guest frame of the session is counted, the first slice included ({r['frames']} vs {guest_frames})")
        require(r["frame_interval"]["count"] + r["frames"]["estimated"] == r["frames"]["virtual"], "histogram + estimated frames == virtual frames: " + json.dumps(r["frames"]))
        require(r["counters"]["m68k_dispatches"] == 3_000_000, "guest dispatch count equals the budget")
        require(r["guest"]["master_ticks"] > 0 and r["wall_ns"] > 0, "guest time reported separately from host time")
        require(len(r["timeline"]["frames"]) <= 1024 and runtime_report.stat().st_size < 64 * 1024, "bounded report")
        require(str(tmp) not in runtime_report.read_text(), "no local path inside a runtime report")
        require(any(u["metric"] == "m68k_time" for u in rd["unavailable"]), "unmeasurable metrics are explicit")
        # long sessions do not grow the report
        longer, longest = tmp / "runtime-long.json", tmp / "runtime-longest.json"
        run_game(game, 14_000_000, {"SEGARECOMP_PERF_REPORT": str(longer), "SEGARECOMP_PERF_MODE": "detailed"})
        run_game(game, 28_000_000, {"SEGARECOMP_PERF_REPORT": str(longest), "SEGARECOMP_PERF_MODE": "detailed"})
        require(pr.load(longer)["runtime"]["frames"]["virtual"] > 1024 and pr.load(longest)["runtime"]["frames"]["virtual"] > 2 * 1024 - 300, "sessions longer than the history")
        require(len(pr.load(longest)["runtime"]["timeline"]["frames"]) == 1024 and pr.load(longest)["runtime"]["timeline"]["dropped_oldest"] > 0, "the frame history is a bounded ring")
        require(abs(longest.stat().st_size - longer.stat().st_size) < 4 * 1024, "report size is independent of session length")
        # summary mode + shareable form
        shareable = tmp / "shareable.json"
        run_game(game, 1_000_000, {"SEGARECOMP_PERF_REPORT": str(shareable), "SEGARECOMP_PERF_PRIVACY": "shareable", "SEGARECOMP_PERF_MODE": "detailed"})
        sd = pr.load(shareable)
        require(sd["privacy"] == "shareable" and "timeline" not in sd["runtime"] and pr.sanitize(sd) == sd, "env-requested shareable report is exactly its sanitized form")
        summary = tmp / "summary.json"
        run_game(game, 1_000_000, {"SEGARECOMP_PERF_REPORT": str(summary)})
        require("timeline" not in pr.load(summary)["runtime"] and pr.load(summary)["config"]["mode"] == "summary", "summary mode keeps no timeline")
        # failure/robustness: malformed option, unwritable path, typed guest stop
        malformed = run_game(game, 1000, {"SEGARECOMP_PERF_REPORT": str(tmp / "m.json"), "SEGARECOMP_PERF_MODE": "turbo"})
        require(malformed.returncode == 3, "a malformed option fails closed before the run")
        unwritable_run = run_game(game, 1_000_000, {"SEGARECOMP_PERF_REPORT": str(tmp / "no-dir" / "r.json")})
        require(sound_summary(unwritable_run.stderr) == sound_summary(run_game(game, 1_000_000).stderr), "an unwritable report path never changes the guest result")
        stop_game = tmp / "on" / "game"
        stop_report = tmp / "stop.json"
        stopped = run_game(stop_game, 1000, {"SEGARECOMP_PERF_REPORT": str(stop_report)})
        plain = run_game(stop_game, 1000)
        require(stopped.stdout == plain.stdout and stopped.returncode == plain.returncode, "a typed guest stop is unchanged by the collector")
        st = pr.load(stop_report)
        require(st["status"] == "incomplete" and st["runtime"]["outcome"] == "guest_stop", "a guest stop is an incomplete session: " + st["status"])
        print("perf_integration_test: ok")


if __name__ == "__main__":
    main()
