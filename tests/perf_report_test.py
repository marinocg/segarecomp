#!/usr/bin/env python3
"""SEG-125 (ADR 0273): tools/perf_report.py on synthetic reports - validation, malformed-input rejection, summaries, comparisons and comparability warnings,
offline HTML, and the sanitize allow-list. No build, no ROM, no network."""
import copy
import json
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import perf_report as pr  # noqa: E402


def require(cond, message):
    if not cond:
        raise AssertionError(message)


def hist(values_ns):
    v = sorted(values_ns)
    n = len(v)
    q = lambda p: v[min(n - 1, max(0, -(-int(p * n * 100) // 100) - 1))]  # noqa: E731
    return {"count": n, "min_ns": v[0], "avg_ns": sum(v) // n, "p50_ns": q(0.5), "p95_ns": q(0.95), "p99_ns": q(0.99), "max_ns": v[-1], "resolution_ns": 50000}


def build_report(scale=1.0, **over):
    wall = 1000.0 * scale
    d = {"schema": pr.SCHEMA, "schema_version": 1, "kind": "build", "status": "complete", "privacy": "private", "platform": "genesis",
         "config": {"optimize": "2", "runtime_optimize": "1", "aot_policy": "compatibility", "jobs": 4, "hardware_concurrency": 8, "viewer": False,
                    "compiler": {"name": "gcc", "version": "gcc (secret/path) 13"}, "host_os": "linux"},
         "build": {"wall_ms": wall,
                   "stages": [{"name": "analyze", "count": 1, "inclusive_ms": 10.0, "exclusive_ms": 10.0},
                              {"name": "compile", "count": 1, "inclusive_ms": 900.0 * scale, "exclusive_ms": 100.0 * scale}],
                   "operations": [{"name": "compile_batch", "count": 2, "inclusive_ms": 700.0 * scale, "exclusive_ms": 700.0 * scale},
                                  {"name": "link_exec", "count": 1, "inclusive_ms": 90.0, "exclusive_ms": 90.0}],
                   "accounting": {"exclusive_sum_ms": 910.0 * scale, "unattributed_ms": 5.0},
                   "counts": {"m68k_emissions": 2, "alias_rounds": 1}, "sizes": {"generated_c_bytes": 1000, "executable_bytes": 5000},
                   "compile": {"batches": 2, "max_workers": 4, "units_total": 10, "compiled": 8, "reused": 2, "final": 6, "superseded": 1, "pass_only": 1, "final_marked": True,
                               "batch_wall_ms": 700.0 * scale, "aggregate_job_ms": 2000.0 * scale, "wasted_ms": 200.0 * scale, "reuse_saved_ms": 150.0,
                               "phases": {"initial": {"compiled": 6, "job_ms": 1500.0}, "preparation": {"compiled": 2, "job_ms": 500.0}},
                               "duration_ms": {"count": 8, "min": 1.0, "p50": 100.0, "p95": 600.0, "max": 600.0},
                               "categories": [{"category": "m68k_generated", "units": 6, "compiled": 4, "reused": 2, "job_ms": 1200.0 * scale, "wasted_ms": 200.0 * scale,
                                               "reuse_saved_ms": 150.0, "source_bytes": 900, "object_bytes": 400},
                                              {"category": "runtime", "units": 4, "compiled": 4, "reused": 0, "job_ms": 800.0, "wasted_ms": 0.0, "reuse_saved_ms": 0.0,
                                               "source_bytes": 100, "object_bytes": 80}],
                               "resources": {"compiler_cpu_ms": 1900.0, "compiler_peak_rss_kib": 100000, "link_cpu_ms": None, "link_wall_ms": 90.0},
                               "slowest_units": [{"label": "genesis_secret_unit.c", "category": "m68k_generated", "wall_ms": 600.0, "source_bytes": 500, "object_bytes": 200,
                                                  "round": 1, "disposition": "superseded"}],
                               "largest_units": []}},
         "unavailable": [{"metric": "link_cpu_ms", "reason": "the platform did not report child CPU time for the linker"}]}
    d.update(over)
    return d


def runtime_report(factor=1.0, frames=600, paced=False, **over):
    iv = [int(16_000_000 * factor) + (i % 7) * 100_000 for i in range(frames)]
    wk = [x - 3_000_000 if paced else x for x in iv]
    tl = [[1000 + i, iv[i] // 1000, wk[i] // 1000, int(iv[i] * 0.6) // 1000, int(iv[i] * 0.2) // 1000, 1 if i % 100 == 99 else 0] for i in range(min(frames, 300))]
    total_iv = sum(iv)
    d = {"schema": pr.SCHEMA, "schema_version": 1, "kind": "runtime", "status": "complete", "privacy": "private", "platform": "genesis",
         "config": {"entry": "viewer", "mode": "detailed", "paced": paced, "slice_dispatches": 500, "target_frame_ns": 16683333, "renderer": "software", "host_os": "linux"},
         "runtime": {"outcome": "window_closed", "wall_ns": total_iv, "paused_ns": 0, "pauses": 0, "frames": {"virtual": frames, "presented": frames - 3, "coalesced": 0},
                     "frame_interval": hist(iv), "frame_work": hist(wk),
                     "pacing": {"waits": frames if paced else 0, "wait_ns": 3_000_000 * frames if paced else 0, "missed_deadlines": 6, "resyncs": 0},
                     "spans": [{"name": "emulation", "class": "inclusive", "parent": "", "count": frames, "total_ns": int(total_iv * 0.6), "max_ns": 9_000_000},
                               {"name": "vdp_compose", "class": "inclusive", "parent": "emulation", "count": frames, "total_ns": int(total_iv * 0.1), "max_ns": 2_000_000},
                               {"name": "present", "class": "inclusive", "parent": "", "count": frames - 3, "total_ns": int(total_iv * 0.2), "max_ns": 4_000_000},
                               {"name": "pacing_wait", "class": "exclusive", "parent": "", "count": frames, "total_ns": 3_000_000 * frames if paced else 0, "max_ns": 3_000_000}],
                     "guest": {"master_ticks": 123456789}, "counters": {"m68k_dispatches": 5_000_000, "runner_slices": 1000, "audio_frames": 99999},
                     "timeline": {"capacity": 1024, "recorded": len(tl), "dropped_oldest": 0, "frames": tl,
                                  "columns": ["frame", "interval_us", "work_us", "emulation_us", "present_us", "missed"]}},
         "unavailable": [{"metric": "gpu_time", "reason": "present is host submission time, not GPU execution time"}]}
    d.update(over)
    return d


def rejects(d, why):
    try:
        pr.validate(d)
    except pr.ReportError:
        return
    raise AssertionError("must be rejected: " + why)


def test_validate_accepts_and_rejects():
    pr.validate(build_report())
    pr.validate(runtime_report())
    rejects({}, "empty")
    rejects([], "not an object")
    d = build_report(); d["schema_version"] = 2; rejects(d, "future version")
    d = build_report(); d["schema"] = "other"; rejects(d, "wrong schema")
    d = build_report(); d["kind"] = "x"; rejects(d, "unknown kind")
    d = build_report(); d["build"]["wall_ms"] = -1; rejects(d, "negative time")
    d = build_report(); d["build"]["wall_ms"] = "fast"; rejects(d, "string time")
    d = build_report(); d["build"]["wall_ms"] = True; rejects(d, "bool as number")
    d = build_report(); d["build"]["accounting"]["exclusive_sum_ms"] = 99999; rejects(d, "double counting")
    d = build_report(); d["build"]["compile"]["compiled"] = 99; rejects(d, "compiled + reused != total")
    d = build_report(); d["build"]["compile"]["wasted_ms"] = 99999; rejects(d, "wasted > aggregate")
    d = build_report(); d["build"]["stages"][1]["exclusive_ms"] = 5000; rejects(d, "exclusive > inclusive")
    d = build_report(); d["build"]["compile"]["slowest_units"] = [{}] * 11; rejects(d, "unbounded unit list")
    d = build_report(status="failed"); rejects(d, "failed build without failure.stage")
    d = runtime_report(); d["runtime"]["frame_interval"]["p95_ns"] = 1; rejects(d, "unordered percentiles")
    d = runtime_report(); d["runtime"]["frames"]["virtual"] = 5; rejects(d, "histogram count vs frames")
    d = runtime_report(); d["runtime"]["timeline"]["frames"] = [[0, 0, 0, 0, 0, 0]] * 1025; rejects(d, "oversized timeline")
    d = runtime_report(); d["runtime"]["timeline"]["frames"][0] = [1, 2]; rejects(d, "malformed timeline row")
    d = runtime_report(privacy="shareable"); rejects(d, "shareable report with a timeline")
    d = runtime_report(); d["runtime"]["spans"][0]["name"] = "mystery"; rejects(d, "unknown span")
    d = runtime_report(); d["config"]["target_frame_ns"] = -5; rejects(d, "negative target")


def test_load_rejects_bad_files(tmp):
    cases = {"empty.json": "", "garbage.json": "{not json", "nan.json": '{"schema":"x","v":NaN}', "inf.json": '{"a":Infinity}', "deep.json": "[" * 5000 + "]" * 5000, "bin.json": None}
    for name, text in cases.items():
        p = tmp / name
        p.write_bytes(b"\xff\xfe\x00\x01" if text is None else text.encode())
        try:
            pr.load(p)
        except pr.ReportError:
            continue
        raise AssertionError(name + " must be rejected")
    big = tmp / "big.json"
    big.write_bytes(b" " * (pr.MAX_REPORT_BYTES + 1))
    try:
        pr.load(big)
        raise AssertionError("oversized file")
    except pr.ReportError:
        pass
    try:
        pr.load(tmp / "missing.json")
        raise AssertionError("missing file")
    except pr.ReportError:
        pass


def test_summary_labels_and_content():
    b = pr.summary_text(build_report())
    require("[measured]" in b and "[derived]" in b and "effective parallelism 2.86x" in b, "build summary labels / parallelism:\n" + b)
    require("discarded work (superseded) 200.0 ms = 10% of job time [derived]" in b, "wasted share")
    require("[unavailable] link_cpu_ms" in b and "genesis_secret_unit.c" in b, "unavailable + private units")
    r = pr.summary_text(runtime_report(paced=True))
    require("emulation speed" in r and "[derived]" in r and "vdp_compose is" in r and "not additive" in r, "runtime summary:\n" + r)
    require("worst frames" in r and "[unavailable] gpu_time" in r, "timeline + unavailable")
    failed = build_report(status="failed", failure={"stage": "link"})
    require("failure in stage link" in pr.summary_text(failed), "failed build is called out")
    inc = runtime_report(status="incomplete")
    inc["runtime"]["outcome"] = "guest_stop"
    require("incomplete" in pr.summary_text(inc), "incomplete session is called out")


def test_compare_build():
    a, b = build_report(), build_report(scale=1.5)
    text = pr.compare_text(a, b)
    require("build wall time" in text and "+50.0%" in text and "SLOWER/WORSE" in text, "slowdown flagged:\n" + text)
    same = pr.compare_text(a, a)
    require("SLOWER" not in same and "better" not in same and "WARNING" not in same, "identical reports: no flags")
    # a 5% change on a large number is not significant; a 20% change under the absolute floor is not either
    small = build_report(scale=1.05)
    require("SLOWER" not in pr.compare_text(a, small), "5% is within the noise floor")
    c = build_report(); c["config"]["jobs"] = 1; c["config"]["optimize"] = "0"
    warn = pr.comparability(a, c)
    require(any("jobs" in w for w in warn) and any("optimize" in w for w in warn), "config differences warned: " + str(warn))
    d = build_report(); d["build"]["compile"]["units_total"] = 20; d["build"]["compile"]["reused"] = 12
    require(any("different number of units" in w and not w.startswith("INFO") for w in pr.comparability(a, d)), "different program")
    modded = pr.comparability(a, d, mod=True)
    require(any(w.startswith("INFO modded") for w in modded) and "WARNING the builds compile" not in pr.compare_text(a, d, mod=True), "a vanilla vs modded comparison notes the expected difference")
    f = build_report(status="failed", failure={"stage": "compile"})
    require(any("failed" in w for w in pr.comparability(a, f)), "failed side warned")
    require(pr.comparability(a, runtime_report())[0].startswith("INCOMPARABLE"), "build vs runtime")
    other = build_report(platform="master_system")
    require(any(w.startswith("INCOMPARABLE") for w in pr.comparability(a, other)), "platform mismatch")


def test_compare_runtime():
    a, b = runtime_report(), runtime_report(factor=1.4)
    text = pr.compare_text(a, b)
    require("frame interval p95" in text and "SLOWER/WORSE" in text and "frame-indexed comparison" in text, "runtime slowdown + timeline:\n" + text)
    require("emulation speed (derived)" in text, "speed row")
    c = runtime_report(); c["config"]["renderer"] = "opengl"; c["config"]["paced"] = True
    require(any("renderer" in w for w in pr.comparability(a, c)) and any("paced" in w for w in pr.comparability(a, c)), "runtime config warnings")
    d = runtime_report(frames=100)
    require(any("frame counts differ" in w for w in pr.comparability(a, d)), "workload size")
    ex = pr.compare_text(a, build_report())
    require("INCOMPARABLE" in ex and "->" not in ex, "no numbers across kinds")


def test_html_is_offline_and_escaped():
    for reports in ([build_report()], [runtime_report()], [build_report(), build_report(scale=1.4)], [runtime_report(), runtime_report(factor=1.3)]):
        page = pr.html_report(reports, ["a.json", "b.json"][:len(reports)])
        require(page.startswith("<!doctype html>"), "document")
        require(not re.search(r"https?://|<script|<link|@import|src=|url\(", page), "the page must be fully self-contained")
        require("<svg" in page and "aria-label" in page, "charts are inline SVG with labels")
    evil = build_report()
    evil["build"]["compile"]["slowest_units"][0]["label"] = "<img src=x onerror=alert(1)>"
    page = pr.html_report([evil], ["x"])
    require("<img src=x" not in page and "&lt;img" in page, "report text is escaped")
    page = pr.html_report([runtime_report(paced=True)], ["r"])
    require("Runtime subsystem breakdown" in page and "Frame interval timeline" in page and "inside emulation" in page, "runtime sections")
    page = pr.html_report([build_report()], ["b"])
    for title in ("Build stages", "Compilation cost by category", "Slowest generated translation units", "kept versus discarded", "compiled versus reused"):
        require(title in page, "missing chart: " + title)
    cmp_page = pr.html_report([build_report(), build_report(scale=1.5)], ["a", "b"])
    require("Before / after" in cmp_page and "Largest changes" in cmp_page, "comparison section")
    bad = pr.html_report([build_report(), runtime_report().copy()], ["a", "b"]) if False else None  # kinds are refused by the CLI, tested below


def test_sanitize_allow_list():
    d = build_report()
    d["extra_private"] = "/home/user/game.bin"
    d["build"]["secret_path"] = "/home/user/roms/x.bin"
    d["config"]["compiler"]["version"] = "gcc (/opt/secret/bin) 13"
    d["unavailable"].append({"metric": "x/y", "reason": "/home/user/path"})
    s = pr.sanitize(d)
    text = json.dumps(s)
    for leak in ("genesis_secret_unit", "/home", "secret", "slowest_units", "largest_units", "extra_private", "(secret/path)", "\"version\""):
        require(leak not in text, f"sanitized report leaks {leak!r}")
    require(s["privacy"] == "shareable" and s["build"]["compile"]["units_total"] == 10, "aggregates survive")
    require(pr.sanitize(s) == s, "sanitize is idempotent")
    r = runtime_report()
    r["config"]["renderer"] = "/dev/dri/card0"
    rs = pr.sanitize(r)
    require("timeline" not in rs["runtime"] and rs["config"]["renderer"] == "redacted", "timeline dropped, path-like text redacted")
    require(rs["runtime"]["frame_interval"]["p95_ns"] == r["runtime"]["frame_interval"]["p95_ns"], "percentiles survive")
    try:
        pr.sanitize({"schema": "bad"})
        raise AssertionError("sanitize must validate its input")
    except pr.ReportError:
        pass


def test_cli(tmp):
    script = str(ROOT / "tools" / "perf_report.py")
    run = lambda *a: subprocess.run([sys.executable, "-I", script, *a], text=True, capture_output=True)  # noqa: E731
    b1, b2, r1, r2 = (tmp / n for n in ("b1.json", "b2.json", "r1.json", "r2.json"))
    b1.write_text(json.dumps(build_report())); b2.write_text(json.dumps(build_report(scale=1.5)))
    r1.write_text(json.dumps(runtime_report())); r2.write_text(json.dumps(runtime_report(factor=1.3)))
    require(run("validate", str(b1), str(r1)).returncode == 0, "validate")
    bad = tmp / "bad.json"; bad.write_text("{}")
    res = run("validate", str(b1), str(bad))
    require(res.returncode == 1 and "perf_report:" in res.stderr and "Traceback" not in res.stderr, "malformed report: clean error, no traceback: " + res.stderr)
    require(run("summary", str(b1)).returncode == 0, "summary")
    out = tmp / "cmp.html"
    res = run("compare", str(b1), str(b2), "--html", str(out))
    require(res.returncode == 0 and out.exists() and "SLOWER/WORSE" in res.stdout, "compare + html")
    res = run("compare", str(b1), str(r1))
    require(res.returncode == 3 and "INCOMPARABLE" in res.stdout, "incomparable exit code")
    res = run("html", str(b1), str(r1), "-o", str(tmp / "x.html"))
    require(res.returncode == 1 and "cannot compare" in res.stderr, "html refuses mixed kinds")
    res = run("sanitize", str(r1), "-o", str(tmp / "shared.json"))
    require(res.returncode == 0 and run("check-shareable", str(tmp / "shared.json")).returncode == 0, "sanitize + check")
    require(run("check-shareable", str(r1)).returncode == 1, "a private report is not shareable")
    tampered = json.loads((tmp / "shared.json").read_text())
    tampered["runtime"]["leak"] = "/home/x"
    (tmp / "t.json").write_text(json.dumps(tampered))
    require(run("check-shareable", str(tmp / "t.json")).returncode == 1, "an extra field breaks shareability")


def main():
    test_validate_accepts_and_rejects()
    test_summary_labels_and_content()
    test_compare_build()
    test_compare_runtime()
    test_html_is_offline_and_escaped()
    test_sanitize_allow_list()
    with tempfile.TemporaryDirectory(prefix="perf-report-") as d:
        tmp = pathlib.Path(d)
        test_load_rejects_bad_files(tmp)
        test_cli(tmp)
    print("perf_report_test: ok")


if __name__ == "__main__":
    main()
