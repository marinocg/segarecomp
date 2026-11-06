#!/usr/bin/env python3
"""Local reader for segarecomp performance reports (SEG-125, ADR 0273). Python standard library only; no network access.

  perf_report.py validate  REPORT...                 schema/consistency check (exit 1 on a malformed report)
  perf_report.py summary   REPORT                    console summary (build or runtime)
  perf_report.py compare   BEFORE AFTER [--html F]   absolute and percentage changes, comparability warnings
  perf_report.py html      REPORT [REPORT] -o F.html self-contained offline report (one report, or before/after)
  perf_report.py sanitize  REPORT -o OUT.json        allow-list rewrite into the shareable form
  perf_report.py check-shareable REPORT              exit 1 unless the report is already exactly its sanitized form

Every number is labelled: [measured] taken from the report, [derived] computed here from measured values by the stated rule, [unavailable] the producer said
it cannot measure it. Nothing here guesses a cause: bottleneck lines state the measured shares they are based on.
"""
from __future__ import annotations

import argparse
import html
import json
import math
import re
import sys
from pathlib import Path

SCHEMA = "segarecomp.performance"
VERSION = 1
MAX_REPORT_BYTES = 4 * 1024 * 1024
KINDS = ("build", "runtime")
STATUSES = ("complete", "incomplete", "failed")
PRIVACIES = ("private", "shareable")
PLATFORMS = ("genesis", "master_system", "sega_cd", "auto")
# A change is called significant only when it clears both a relative and an absolute floor (so noise on tiny numbers is not flagged).
SIGNIFICANT_PCT = 10.0
SIGNIFICANT_BUILD_MS = 50.0
SIGNIFICANT_FRAME_NS = 100_000
SPAN_ORDER = ("emulation", "vdp_compose", "rgb_convert", "texture_upload", "present", "audio_present", "pacing_wait")

BUILD_COMPARE_KEYS = ("optimize", "runtime_optimize", "aot_policy", "jobs", "viewer", "host_os")
RUNTIME_COMPARE_KEYS = ("entry", "mode", "paced", "slice_dispatches", "target_frame_ns", "renderer", "host_os")


class ReportError(Exception):
    """A report that must be rejected."""


# ---------------------------------------------------------------------------------------------------------------------------------- loading

def _no_constants(name: str):
    raise ReportError(f"non-finite number {name} in report")


def load(path: str | Path) -> dict:
    p = Path(path)
    try:
        size = p.stat().st_size
    except OSError as exc:
        raise ReportError(f"{p}: cannot read ({exc.strerror})") from None
    if size > MAX_REPORT_BYTES:
        raise ReportError(f"{p}: report is larger than {MAX_REPORT_BYTES} bytes")
    try:
        data = json.loads(p.read_text(encoding="utf-8"), parse_constant=_no_constants)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as exc:
        raise ReportError(f"{p}: not valid JSON ({exc})") from None
    validate(data, str(p))
    return data


def _need(cond: bool, where: str, msg: str):
    if not cond:
        raise ReportError(f"{where}: {msg}")


def _num(v, where: str, name: str, *, integer=False, null=False):
    if v is None and null:
        return
    ok = isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) and v >= 0
    if integer:
        ok = ok and float(v).is_integer()
    _need(ok, where, f"{name} must be a non-negative {'integer' if integer else 'number'}" + (" or null" if null else ""))


def _hist(h, where: str, name: str):
    _need(isinstance(h, dict), where, f"{name} must be an object")
    for k in ("count", "min_ns", "avg_ns", "p50_ns", "p95_ns", "p99_ns", "max_ns"):
        _num(h.get(k), where, f"{name}.{k}", integer=True)
    if h["count"]:
        _need(h["min_ns"] <= h["p50_ns"] <= h["p95_ns"] <= h["p99_ns"] <= h["max_ns"], where, f"{name} percentiles are not ordered")
        _need(h["min_ns"] <= h["avg_ns"] <= h["max_ns"], where, f"{name}.avg_ns outside [min, max]")


def validate(d, where: str = "report") -> None:
    _need(isinstance(d, dict), where, "top level must be an object")
    _need(d.get("schema") == SCHEMA, where, f"schema must be {SCHEMA!r}")
    _need(d.get("schema_version") == VERSION, where, f"unsupported schema_version {d.get('schema_version')!r} (this reader understands {VERSION})")
    _need(d.get("kind") in KINDS, where, "kind must be build or runtime")
    _need(d.get("status") in STATUSES, where, "status must be complete, incomplete or failed")
    _need(d.get("privacy") in PRIVACIES, where, "privacy must be private or shareable")
    _need(d.get("platform") in PLATFORMS, where, "unknown platform")
    _need(isinstance(d.get("config"), dict), where, "config must be an object")
    if "unavailable" in d:
        _need(isinstance(d["unavailable"], list) and all(isinstance(u, dict) and isinstance(u.get("metric"), str) and isinstance(u.get("reason"), str)
                                                            for u in d["unavailable"]), where, "unavailable must be a list of {metric, reason}")
    if d["status"] == "failed" and d["kind"] == "build":
        _need(isinstance(d.get("failure"), dict), where, "a failed build report must name failure.stage")
    if d["kind"] == "build":
        b = d.get("build")
        _need(isinstance(b, dict), where, "build section missing")
        _num(b.get("wall_ms"), where, "build.wall_ms")
        for key in ("stages", "operations"):
            _need(isinstance(b.get(key), list), where, f"build.{key} must be a list")
            for row in b[key]:
                _need(isinstance(row, dict) and isinstance(row.get("name"), str), where, f"build.{key} row needs a name")
                for k in ("inclusive_ms", "exclusive_ms"):
                    _num(row.get(k), where, f"{key}.{row['name']}.{k}")
                _need(row["exclusive_ms"] <= row["inclusive_ms"] + 0.01, where, f"{key}.{row['name']}: exclusive exceeds inclusive")
        acc = b.get("accounting")
        _need(isinstance(acc, dict), where, "build.accounting missing")
        _need(acc["exclusive_sum_ms"] <= b["wall_ms"] + 1.0, where, "exclusive times exceed the wall time (double counting)")
        c = b.get("compile")
        _need(isinstance(c, dict), where, "build.compile missing")
        for k in ("units_total", "compiled", "reused", "final", "superseded", "pass_only"):
            _num(c.get(k), where, f"compile.{k}", integer=True)
        for k in ("batch_wall_ms", "aggregate_job_ms", "wasted_ms", "reuse_saved_ms"):
            _num(c.get(k), where, f"compile.{k}")
        _need(c["compiled"] + c["reused"] == c["units_total"], where, "compiled + reused must equal units_total")
        _need(c["final"] + c["superseded"] + c["pass_only"] <= c["compiled"], where, "dispositions exceed the compiled count")
        _need(c["wasted_ms"] <= c["aggregate_job_ms"] + 0.01, where, "wasted_ms exceeds the aggregate job time")
        _need(isinstance(c.get("categories"), list), where, "compile.categories must be a list")
        for cat in c["categories"]:
            _need(isinstance(cat, dict) and isinstance(cat.get("category"), str), where, "bad compile category")
        res = c.get("resources")
        _need(isinstance(res, dict), where, "compile.resources missing")
        for k in ("compiler_cpu_ms", "compiler_peak_rss_kib", "link_cpu_ms"):
            _num(res.get(k), where, f"resources.{k}", null=True)
        for key in ("slowest_units", "largest_units"):
            if key in c:
                _need(isinstance(c[key], list) and len(c[key]) <= 10, where, f"{key} must be a list of at most 10")
        _need(isinstance(b.get("counts"), dict) and isinstance(b.get("sizes"), dict), where, "build.counts / build.sizes must be objects")
        for v in list(b["counts"].values()) + list(b["sizes"].values()):
            _num(v, where, "count/size value", integer=True)
    else:
        r = d.get("runtime")
        _need(isinstance(r, dict), where, "runtime section missing")
        _need(isinstance(r.get("outcome"), str), where, "runtime.outcome must be a string")
        for k in ("wall_ns", "paused_ns", "pauses"):
            _num(r.get(k), where, f"runtime.{k}", integer=True)
        f = r.get("frames")
        _need(isinstance(f, dict), where, "runtime.frames missing")
        for k in ("virtual", "presented", "estimated"):
            _num(f.get(k), where, f"frames.{k}", integer=True)
        eo = r.get("estimated_observations")
        _need(isinstance(eo, dict), where, "runtime.estimated_observations missing")
        for k in ("count", "wall_ns", "max_wall_ns"):
            _num(eo.get(k), where, f"estimated_observations.{k}", integer=True)
        _need(eo["count"] <= f["estimated"] and (eo["count"] > 0) == (f["estimated"] > 0), where, "estimated frames and observations are inconsistent")
        _hist(r.get("frame_interval"), where, "frame_interval")
        _hist(r.get("frame_work"), where, "frame_work")
        _need(r["frame_interval"]["count"] + f["estimated"] == f["virtual"], where, "frames.virtual must equal the exactly-timed frames (frame_interval.count) plus frames.estimated")
        _need(r["frame_work"]["count"] == r["frame_interval"]["count"], where, "frame_work and frame_interval must cover the same frames")
        p = r.get("pacing")
        _need(isinstance(p, dict), where, "runtime.pacing missing")
        for k in ("waits", "wait_ns", "missed_deadlines", "resyncs"):
            _num(p.get(k), where, f"pacing.{k}", integer=True)
        _need(isinstance(r.get("spans"), list) and len(r["spans"]) <= 16, where, "runtime.spans must be a short list")
        for s in r["spans"]:
            _need(isinstance(s, dict) and s.get("name") in SPAN_ORDER and s.get("class") in ("inclusive", "exclusive"), where, "bad span row")
            for k in ("count", "total_ns", "max_ns"):
                _num(s.get(k), where, f"span.{s.get('name')}.{k}", integer=True)
        _need(isinstance(r.get("guest"), dict), where, "runtime.guest missing")
        _num(r["guest"].get("master_ticks"), where, "guest.master_ticks", integer=True)
        _need(isinstance(r.get("counters"), dict) and len(r["counters"]) <= 16, where, "runtime.counters must be a small object")
        for v in r["counters"].values():
            _num(v, where, "counter", integer=True)
        _num(d["config"].get("target_frame_ns"), where, "config.target_frame_ns", integer=True)
        tl = r.get("timeline")
        if tl is not None:
            _need(isinstance(tl, dict) and isinstance(tl.get("frames"), list) and len(tl["frames"]) <= 1024, where, "timeline must hold at most 1024 frames")
            _need(d["privacy"] == "private", where, "a shareable report must not carry a timeline")
            for row in tl["frames"]:
                _need(isinstance(row, list) and len(row) == 8 and all(isinstance(x, int) and x >= 0 for x in row) and row[5] >= 1 and row[6] <= row[5] and row[7] in (0, 1),
                  where, "malformed timeline row (frame, interval_us, work_us, emulation_us, present_us, frames, missed, partial)")


# ------------------------------------------------------------------------------------------------------------------------------ derivations

def runtime_derived(d: dict) -> dict:
    """Values computed from measured ones. `speed` = virtual frames x native frame period / wall time excluding pauses."""
    r, cfg = d["runtime"], d["config"]
    wall = max(r["wall_ns"] - r["paused_ns"], 0)
    frames = r["frames"]["virtual"]
    out = {"wall_ns": wall}
    out["speed"] = frames * cfg["target_frame_ns"] / wall if wall and frames else None
    out["fps_virtual"] = frames * 1e9 / wall if wall and frames else None
    out["fps_presented"] = r["frames"]["presented"] * 1e9 / wall if wall and r["frames"]["presented"] else None
    spans = {s["name"]: s for s in r["spans"]}
    out["per_frame_ns"] = {n: s["total_ns"] / frames for n, s in spans.items() if frames and s["count"]}
    # shares of the observed wall time (pauses excluded): independent of how many frames could be timed individually
    out["share_of_interval"] = {n: s["total_ns"] / wall for n, s in spans.items() if wall and s["count"]}
    out["exact_share"] = r["frame_interval"]["count"] / frames if frames else None
    return out


def build_derived(d: dict) -> dict:
    b = d["build"]
    c = b["compile"]
    out = {}
    out["parallelism"] = c["aggregate_job_ms"] / c["batch_wall_ms"] if c["batch_wall_ms"] else None
    out["wasted_share"] = c["wasted_ms"] / c["aggregate_job_ms"] if c["aggregate_job_ms"] else None
    out["compile_wall_share"] = c["batch_wall_ms"] / b["wall_ms"] if b["wall_ms"] else None
    out["reuse_ratio"] = c["reused"] / c["units_total"] if c["units_total"] else None
    return out


def bottleneck_lines(d: dict) -> list[str]:
    """Plain statements of the measured shares; no cause is asserted."""
    lines = []
    if d["kind"] == "build":
        b = d["build"]
        top = sorted(b["stages"], key=lambda s: -s["exclusive_ms"])[:3]
        ops = sorted(b["operations"], key=lambda s: -s["exclusive_ms"])[:3]
        for s in top:
            lines.append(f"stage {s['name']}: {s['exclusive_ms']:.0f} ms exclusive = {100 * s['exclusive_ms'] / b['wall_ms']:.0f}% of wall [derived]" if b["wall_ms"] else "")
        for s in ops:
            lines.append(f"operation {s['name']}: {s['exclusive_ms']:.0f} ms exclusive = {100 * s['exclusive_ms'] / b['wall_ms']:.0f}% of wall [derived]" if b["wall_ms"] else "")
        k = build_derived(d)
        if k["wasted_share"] is not None and b["compile"]["wasted_ms"] > 0:
            lines.append(f"compile work discarded (superseded objects): {b['compile']['wasted_ms']:.0f} ms of {b['compile']['aggregate_job_ms']:.0f} ms job time = {100 * k['wasted_share']:.0f}% [derived]")
    else:
        k = runtime_derived(d)
        shares = k["share_of_interval"]
        parts = [(n, v) for n, v in shares.items() if n in ("emulation", "present", "audio_present", "pacing_wait", "rgb_convert")]
        parts.sort(key=lambda x: -x[1])
        if parts:
            lines.append("share of the average frame interval (exclusive categories): " + ", ".join(f"{n} {100 * v:.0f}%" for n, v in parts) + " [derived]")
        if "vdp_compose" in shares and "emulation" in shares:
            lines.append(f"vdp_compose is {100 * shares['vdp_compose']:.0f}% of the interval and is INSIDE emulation ({100 * shares['emulation']:.0f}%) - not additive [derived]")
        if k["speed"] is not None:
            lines.append(f"emulation speed {100 * k['speed']:.0f}% of the console's native rate [derived]" + ("" if d["config"].get("paced") else " (unpaced: this is host capacity, not a pacing result)"))
        r = d["runtime"]
        if r["pacing"]["missed_deadlines"]:
            lines.append(f"{r['pacing']['missed_deadlines']} of {r['frames']['virtual']} frame deadlines were already past when the pacer looked [measured]")
    return [x for x in lines if x]


# ---------------------------------------------------------------------------------------------------------------------------------- summary

def fmt_ms(v) -> str:
    return "n/a" if v is None else f"{v:,.1f} ms"


def fmt_ns(v) -> str:
    return f"{v / 1e6:,.2f} ms"


def fmt_bytes(v) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if v < 1024 or unit == "GiB":
            return f"{v:,.0f} {unit}" if unit == "B" else f"{v:,.1f} {unit}"
        v /= 1024
    return str(v)


def summary_text(d: dict) -> str:
    out = []
    w = out.append
    cfg = d["config"]
    w(f"{d['kind']} report  platform={d['platform']}  status={d['status']}  privacy={d['privacy']}")
    if d["kind"] == "build":
        b = d["build"]
        w("config: " + ", ".join(f"{k}={cfg.get(k)}" for k in ("optimize", "runtime_optimize", "aot_policy", "jobs", "viewer", "host_os")) + f", compiler={cfg.get('compiler', {}).get('name')}")
        if d["status"] != "complete":
            w(f"!! {d['status']}: failure in stage {d.get('failure', {}).get('stage') or 'unknown'}; figures cover the work done before it")
        w(f"\nwall time {fmt_ms(b['wall_ms'])} [measured]   unattributed {fmt_ms(b['accounting']['unattributed_ms'])} [measured]")
        w("\nstages (exclusive time excludes nested stages) [measured]")
        for s in b["stages"]:
            w(f"  {s['name']:<22} exclusive {s['exclusive_ms']:>10,.1f} ms   inclusive {s['inclusive_ms']:>10,.1f} ms   x{s['count']}")
        w("\noperations [measured]")
        for s in sorted(b["operations"], key=lambda s: -s["exclusive_ms"]):
            w(f"  {s['name']:<22} exclusive {s['exclusive_ms']:>10,.1f} ms   inclusive {s['inclusive_ms']:>10,.1f} ms   x{s['count']}")
        c = b["compile"]
        k = build_derived(d)
        w("\ncompilation")
        w(f"  units {c['units_total']}: compiled {c['compiled']}, reused {c['reused']} [measured]; of the compiled: final {c['final']}, pass-only {c['pass_only']}, superseded {c['superseded']}"
          + ("" if c["final_marked"] else "  [unavailable: build ended before the final link]"))
        w(f"  wall time of compile batches {fmt_ms(c['batch_wall_ms'])} vs aggregate job time {fmt_ms(c['aggregate_job_ms'])} [measured]"
          + (f"  -> effective parallelism {k['parallelism']:.2f}x [derived]" if k["parallelism"] else ""))
        w(f"  discarded work (superseded) {fmt_ms(c['wasted_ms'])}" + (f" = {100 * k['wasted_share']:.0f}% of job time [derived]" if k["wasted_share"] is not None else "")
          + f";  saved by reuse {fmt_ms(c['reuse_saved_ms'])} [measured]")
        ph = c["phases"]
        w(f"  initial build {ph['initial']['compiled']} units / {fmt_ms(ph['initial']['job_ms'])};  preparation rounds {ph['preparation']['compiled']} units / {fmt_ms(ph['preparation']['job_ms'])} [measured]")
        res = c["resources"]
        w("  compiler CPU " + (fmt_ms(res["compiler_cpu_ms"]) + " [measured]" if res["compiler_cpu_ms"] is not None else "[unavailable]") +
          ", peak RSS " + (f"{res['compiler_peak_rss_kib'] / 1024:,.0f} MiB [measured]" if res["compiler_peak_rss_kib"] is not None else "[unavailable]"))
        w("  by category (job time, wasted, units)")
        for cat in sorted(c["categories"], key=lambda x: -x["job_ms"]):
            w(f"    {cat['category']:<16} {cat['job_ms']:>10,.1f} ms  wasted {cat['wasted_ms']:>8,.1f} ms  compiled {cat['compiled']} reused {cat['reused']}  src {fmt_bytes(cat['source_bytes'])}")
        if "slowest_units" in c:
            w("  slowest units [measured, private]")
            for u in c["slowest_units"][:5]:
                w(f"    {u['wall_ms']:>9,.1f} ms  {u['category']:<14} {u['label']}  ({u['disposition']}, round {u['round']})")
        w("\nsizes [measured]: " + ", ".join(f"{k}={fmt_bytes(v)}" for k, v in sorted(b["sizes"].items())))
        w("counts [measured]: " + ", ".join(f"{k}={v}" for k, v in sorted(b["counts"].items())))
    else:
        r, k = d["runtime"], runtime_derived(d)
        w("config: " + ", ".join(f"{k2}={cfg.get(k2)}" for k2 in RUNTIME_COMPARE_KEYS))
        if d["status"] != "complete":
            w(f"!! {d['status']}: outcome '{r['outcome']}'; figures cover the session up to it")
        f = r["frames"]
        w(f"\nsession {fmt_ns(r['wall_ns'])} (paused {fmt_ns(r['paused_ns'])}, {r['pauses']}x) outcome={r['outcome']} [measured]")
        w(f"frames: virtual {f['virtual']}, presented {f['presented']}, estimated {f['estimated']} [measured]")
        if f["estimated"]:
            eo = r["estimated_observations"]
            w(f"  {f['estimated']} of {f['virtual']} frames lie in {eo['count']} multi-frame or partial observations (wall {fmt_ns(eo['wall_ns'])}, longest {fmt_ns(eo['max_wall_ns'])}): "
              "they are counted but not individually timed; the percentiles below cover the other " + str(r["frame_interval"]["count"]) + " frames only")
        if k["speed"] is not None:
            w(f"emulation speed {100 * k['speed']:.1f}% of native ({k['fps_virtual']:.1f} virtual FPS" + (f", {k['fps_presented']:.1f} presented FPS" if k["fps_presented"] else "") + ") [derived]")
        for name, key in (("frame interval", "frame_interval"), ("frame work (interval minus pacing wait)", "frame_work")):
            h = r[key]
            w(f"{name}: min {fmt_ns(h['min_ns'])}  avg {fmt_ns(h['avg_ns'])}  p50 {fmt_ns(h['p50_ns'])}  p95 {fmt_ns(h['p95_ns'])}  p99 {fmt_ns(h['p99_ns'])}  max {fmt_ns(h['max_ns'])} [measured, bucket {50} us]")
        p = r["pacing"]
        w(f"pacing: {p['waits']} waits totalling {fmt_ns(p['wait_ns'])}, {p['missed_deadlines']} missed deadlines, {p['resyncs']} resyncs [measured]")
        w("subsystem spans (inclusive spans are contained in their parent; do not add them) [measured]")
        for s in sorted(r["spans"], key=lambda s: SPAN_ORDER.index(s["name"])):
            if not s["count"]:
                continue
            per = k["per_frame_ns"].get(s["name"])
            w(f"  {s['name']:<15} {s['class']:<9} total {fmt_ns(s['total_ns'])}  per frame {fmt_ns(per) if per is not None else 'n/a'}  max {fmt_ns(s['max_ns'])}  x{s['count']}")
        w(f"guest (separate from host time) [measured]: master_ticks={r['guest']['master_ticks']}  " + "  ".join(f"{a}={b}" for a, b in r["counters"].items()))
        if "timeline" in r:
            worst = sorted(r["timeline"]["frames"], key=lambda x: -x[1] / x[5])[:5]
            w(f"worst of the last {len(r['timeline']['frames'])} observations, by time per frame [measured]")
            for row in worst:
                what = f"{row[5]} frames, {row[1] / 1000:.2f} ms total = {row[1] / row[5] / 1000:.2f} ms average (not a per-frame measurement)" if row[5] > 1 or row[7] else f"{row[1] / 1000:.2f} ms"
                w(f"  frame {row[0]}: {what}, work {row[2] / 1000:.2f} ms, emulation {row[3] / 1000:.2f} ms, present {row[4] / 1000:.2f} ms" + (f", {row[6]} missed deadline(s)" if row[6] else ""))
    for line in bottleneck_lines(d):
        w("* " + line)
    for u in d.get("unavailable", []):
        w(f"[unavailable] {u['metric']}: {u['reason']}")
    return "\n".join(out)


# --------------------------------------------------------------------------------------------------------------------------------- compare

def comparability(a: dict, b: dict, mod: bool = False) -> list[str]:
    """`mod`: the two builds are meant to differ in program (vanilla vs patched/modded), so a different unit count is expected and noted instead of warned."""
    warn = []
    if a["kind"] != b["kind"]:
        return [f"INCOMPARABLE: kinds differ ({a['kind']} vs {b['kind']})"]
    if a["platform"] != b["platform"]:
        warn.append(f"INCOMPARABLE: platforms differ ({a['platform']} vs {b['platform']})")
    for r, name in ((a, "before"), (b, "after")):
        if r["status"] != "complete":
            warn.append(f"{name} report is {r['status']}: its figures cover a partial {'build' if a['kind'] == 'build' else 'session'}")
    keys = BUILD_COMPARE_KEYS if a["kind"] == "build" else RUNTIME_COMPARE_KEYS
    for k in keys:
        if a["config"].get(k) != b["config"].get(k):
            warn.append(f"config differs: {k}: {a['config'].get(k)!r} vs {b['config'].get(k)!r}")
    if a["kind"] == "build":
        if a["config"].get("compiler", {}).get("name") != b["config"].get("compiler", {}).get("name"):
            warn.append("config differs: compiler")
        elif a["config"].get("compiler", {}).get("version") != b["config"].get("compiler", {}).get("version") and "version" in a["config"].get("compiler", {}):
            warn.append("compiler version differs")
        if a["build"]["compile"]["units_total"] != b["build"]["compile"]["units_total"]:
            ua, ub = a["build"]["compile"]["units_total"], b["build"]["compile"]["units_total"]
            warn.append(f"INFO modded/patched comparison: the builds compile a different number of units ({ua} vs {ub}), as expected" if mod
                        else f"the builds compile a different number of units ({ua} vs {ub}): not the same program (use --mod to compare a vanilla and a modded build)")
    else:
        fa, fb = a["runtime"]["frames"]["virtual"], b["runtime"]["frames"]["virtual"]
        if min(fa, fb) == 0 or max(fa, fb) / max(min(fa, fb), 1) > 1.25:
            warn.append(f"frame counts differ markedly ({fa} vs {fb}): different workloads or windows")
        for side, r in (("before", a), ("after", b)):
            est = r["runtime"]["frames"]["estimated"]
            if est:
                warn.append(f"INFO {side}: {est} of {r['runtime']['frames']['virtual']} frames were not individually timed (coalesced slices); its percentiles cover the others only")
        if a["runtime"]["outcome"] != b["runtime"]["outcome"]:
            warn.append(f"session outcomes differ ({a['runtime']['outcome']} vs {b['runtime']['outcome']})")
    return warn


def delta_row(label: str, x, y, unit_fmt, floor: float, lower_is_better=True):
    if x is None or y is None:
        return label, None
    diff = y - x
    pct = (diff / x * 100.0) if x else (math.inf if diff else 0.0)
    significant = abs(pct) >= SIGNIFICANT_PCT and abs(diff) >= floor
    worse = (diff > 0) == lower_is_better and diff != 0
    return label, {"before": x, "after": y, "diff": diff, "pct": pct, "significant": significant, "worse": worse, "fmt": unit_fmt}


def compare_rows(a: dict, b: dict) -> list[tuple[str, dict | None]]:
    rows = []
    if a["kind"] == "build":
        A, B = a["build"], b["build"]
        rows.append(delta_row("build wall time", A["wall_ms"], B["wall_ms"], fmt_ms, SIGNIFICANT_BUILD_MS))
        sa = {s["name"]: s for s in A["stages"]}
        sb = {s["name"]: s for s in B["stages"]}
        for n in dict.fromkeys(list(sa) + list(sb)):
            rows.append(delta_row(f"stage {n} (exclusive)", sa.get(n, {}).get("exclusive_ms", 0.0), sb.get(n, {}).get("exclusive_ms", 0.0), fmt_ms, SIGNIFICANT_BUILD_MS))
        oa = {s["name"]: s for s in A["operations"]}
        ob = {s["name"]: s for s in B["operations"]}
        for n in dict.fromkeys(list(oa) + list(ob)):
            rows.append(delta_row(f"operation {n} (exclusive)", oa.get(n, {}).get("exclusive_ms", 0.0), ob.get(n, {}).get("exclusive_ms", 0.0), fmt_ms, SIGNIFICANT_BUILD_MS))
        ca, cb = A["compile"], B["compile"]
        for key, label in (("batch_wall_ms", "compile batch wall time"), ("aggregate_job_ms", "aggregate compile job time"), ("wasted_ms", "discarded compile work"),
                           ("reuse_saved_ms", "time saved by reuse")):
            rows.append(delta_row(label, ca[key], cb[key], fmt_ms, SIGNIFICANT_BUILD_MS, lower_is_better=key != "reuse_saved_ms"))
        for key, label in (("compiled", "units compiled"), ("reused", "units reused"), ("superseded", "units superseded")):
            rows.append(delta_row(label, ca[key], cb[key], lambda v: f"{v:,.0f}", 1, lower_is_better=key != "reused"))
        for key in sorted(set(A["sizes"]) | set(B["sizes"])):
            rows.append(delta_row(f"size {key}", A["sizes"].get(key), B["sizes"].get(key), fmt_bytes, 1024))
    else:
        ra, rb = a["runtime"], b["runtime"]
        ka, kb = runtime_derived(a), runtime_derived(b)
        rows.append(delta_row("emulation speed (derived)", ka["speed"], kb["speed"], lambda v: f"{100 * v:.1f}%", 0.01, lower_is_better=False))
        for key in ("avg_ns", "p50_ns", "p95_ns", "p99_ns", "max_ns"):
            rows.append(delta_row(f"frame interval {key[:-3]}", ra["frame_interval"][key], rb["frame_interval"][key], fmt_ns, SIGNIFICANT_FRAME_NS))
        for key in ("avg_ns", "p95_ns", "p99_ns"):
            rows.append(delta_row(f"frame work {key[:-3]}", ra["frame_work"][key], rb["frame_work"][key], fmt_ns, SIGNIFICANT_FRAME_NS))
        for n in SPAN_ORDER:
            if n in ka["per_frame_ns"] or n in kb["per_frame_ns"]:
                rows.append(delta_row(f"{n} per frame", ka["per_frame_ns"].get(n), kb["per_frame_ns"].get(n), fmt_ns, SIGNIFICANT_FRAME_NS))
        rows.append(delta_row("missed deadlines (share of frames)", ra["pacing"]["missed_deadlines"] / max(ra["frames"]["virtual"], 1),
                              rb["pacing"]["missed_deadlines"] / max(rb["frames"]["virtual"], 1), lambda v: f"{100 * v:.1f}%", 0.005))
        for n in sorted(set(ra["counters"]) & set(rb["counters"])):
            fa, fb = max(ra["frames"]["virtual"], 1), max(rb["frames"]["virtual"], 1)
            rows.append(delta_row(f"counter {n} per frame", ra["counters"][n] / fa, rb["counters"][n] / fb, lambda v: f"{v:,.1f}", 1.0))
    return rows


def compare_text(a: dict, b: dict, mod: bool = False) -> str:
    out = []
    warn = comparability(a, b, mod)
    out.append(f"compare {a['kind']} reports: before={a['status']} after={b['status']}")
    for w in warn:
        out.append(w if w.startswith("INFO") else f"WARNING {w}")
    if any(w.startswith("INCOMPARABLE") for w in warn):
        return "\n".join(out)
    out.append("(all differences are [derived] from the two reports' measured values; 'significant' = both >= "
               f"{SIGNIFICANT_PCT:.0f}% and above the absolute floor)")
    for label, row in compare_rows(a, b):
        if row is None:
            out.append(f"  {label:<40} [unavailable on one side]")
            continue
        flag = ""
        if row["significant"]:
            flag = "  << SLOWER/WORSE" if row["worse"] else "  >> better"
        sign = "+" if row["diff"] >= 0 else "-"
        pct = "n/a" if math.isinf(row["pct"]) else f"{row['pct']:+.1f}%"
        out.append(f"  {label:<40} {row['fmt'](row['before']):>14} -> {row['fmt'](row['after']):>14}   {sign}{row['fmt'](abs(row['diff']))} ({pct}){flag}")
    if a["kind"] == "runtime" and "timeline" in a["runtime"] and "timeline" in b["runtime"]:
        out.append(timeline_regions(a, b))
    return "\n".join(out)


def timeline_regions(a: dict, b: dict) -> str:
    """Frame-indexed comparison of the two recorded windows: the five frame offsets with the largest interval increase."""
    fa, fb = a["runtime"]["timeline"]["frames"], b["runtime"]["timeline"]["frames"]
    exact = lambda rows: [r for r in rows if r[5] == 1 and not r[7]]   # noqa: E731  only individually timed frames are comparable position by position
    fa, fb = exact(fa), exact(fb)
    n = min(len(fa), len(fb))
    if n == 0:
        return ""
    diffs = sorted(((fb[i][1] - fa[i][1], i) for i in range(n)), reverse=True)[:5]
    lines = [f"comparison of the first {n} individually timed frames of each recorded window, by position (multi-frame observations skipped) [derived]"]
    for d, i in diffs:
        lines.append(f"  offset {i}: {fa[i][1] / 1000:.2f} ms -> {fb[i][1] / 1000:.2f} ms ({d / 1000:+.2f} ms)")
    return "\n".join(lines)


# ------------------------------------------------------------------------------------------------------------------------------ sanitize

_SAFE_TEXT = re.compile(r"^[A-Za-z0-9_.+ \-]{0,48}$")
_CATEGORIES = ("m68k_generated", "z80_generated", "sega_cd_generated", "sms_generated", "sound_device", "viewer", "runtime", "other")
_COUNTER_NAMES = ("m68k_dispatches", "runner_slices", "z80_epochs", "frames_published", "render_rejections", "vram_journal_overflows", "audio_frames", "audio_underruns",
                  "audio_overrun_dropped", "main_dispatches", "sub_dispatches")


def _text(v, default="redacted") -> str:
    return v if isinstance(v, str) and _SAFE_TEXT.match(v) and "/" not in v and "\\" not in v else default


def sanitize(d: dict) -> dict:
    """Rebuilds a report from allow-listed fields only. Free text is kept only if it is short and path-free; unit labels, the compiler version line and the frame
    timeline are never copied. Unknown keys are dropped, so a future private field cannot leak through this function."""
    validate(d)
    cfg = d["config"]
    out = {"schema": SCHEMA, "schema_version": VERSION, "kind": d["kind"], "status": d["status"], "privacy": "shareable", "platform": d["platform"]}
    if d["kind"] == "build":
        out["config"] = {"optimize": _text(cfg.get("optimize")), "runtime_optimize": _text(cfg.get("runtime_optimize")), "aot_policy": _text(cfg.get("aot_policy")),
                         "jobs": int(cfg.get("jobs", 0)), "hardware_concurrency": int(cfg.get("hardware_concurrency", 0)), "viewer": bool(cfg.get("viewer")),
                         "compiler": {"name": _text(cfg.get("compiler", {}).get("name"))}, "host_os": _text(cfg.get("host_os"))}
        if d["status"] != "complete":
            out["failure"] = {"stage": _text(d.get("failure", {}).get("stage"))}
        b = d["build"]
        c = b["compile"]
        row = lambda s: {"name": _text(s["name"]), "count": int(s.get("count", 0)), "inclusive_ms": s["inclusive_ms"], "exclusive_ms": s["exclusive_ms"]}  # noqa: E731
        out["build"] = {"wall_ms": b["wall_ms"], "stages": [row(s) for s in b["stages"]], "operations": [row(s) for s in b["operations"]],
                        "accounting": {k: b["accounting"][k] for k in ("exclusive_sum_ms", "unattributed_ms")},
                        "counts": {_text(k): v for k, v in b["counts"].items()}, "sizes": {_text(k): v for k, v in b["sizes"].items()},
                        "compile": {k: c[k] for k in ("batches", "max_workers", "units_total", "compiled", "reused", "final", "superseded", "pass_only", "final_marked", "batch_wall_ms",
                                                      "aggregate_job_ms", "wasted_ms", "reuse_saved_ms", "phases", "duration_ms") if k in c}}
        out["build"]["compile"]["categories"] = [{k: v for k, v in cat.items() if k in ("units", "compiled", "reused", "job_ms", "wasted_ms", "reuse_saved_ms", "source_bytes", "object_bytes")}
                                                 | {"category": cat["category"] if cat["category"] in _CATEGORIES else "other"} for cat in c["categories"]]
        out["build"]["compile"]["resources"] = {k: c["resources"].get(k) for k in ("compiler_cpu_ms", "compiler_peak_rss_kib", "link_cpu_ms", "link_wall_ms")}
    else:
        out["config"] = {"entry": _text(cfg.get("entry")), "mode": _text(cfg.get("mode")), "paced": bool(cfg.get("paced")), "slice_dispatches": int(cfg.get("slice_dispatches", 0)),
                         "target_frame_ns": int(cfg["target_frame_ns"]), "renderer": _text(cfg.get("renderer")) if cfg.get("renderer") else None, "host_os": _text(cfg.get("host_os"))}
        r = d["runtime"]
        out["runtime"] = {"outcome": _text(r["outcome"]), "wall_ns": r["wall_ns"], "paused_ns": r["paused_ns"], "pauses": r["pauses"], "frames": dict(r["frames"]), "estimated_observations": dict(r["estimated_observations"]),
                          "frame_interval": {k: r["frame_interval"][k] for k in r["frame_interval"] if k.endswith("_ns") or k in ("count", "resolution_ns")},
                          "frame_work": {k: r["frame_work"][k] for k in r["frame_work"] if k.endswith("_ns") or k in ("count", "resolution_ns")},
                          "pacing": dict(r["pacing"]), "spans": [{k: s[k] for k in ("name", "class", "parent", "count", "total_ns", "max_ns") if k in s} for s in r["spans"]],
                          "guest": {"master_ticks": r["guest"]["master_ticks"]}, "counters": {k: v for k, v in r["counters"].items() if k in _COUNTER_NAMES}}
    out["unavailable"] = [{"metric": _text(u["metric"]), "reason": u["reason"] if isinstance(u["reason"], str) and "/" not in u["reason"] and "\\" not in u["reason"] and len(u["reason"]) <= 160 else "redacted"}
                          for u in d.get("unavailable", [])]
    validate(out)
    return out


# ------------------------------------------------------------------------------------------------------------------------------------ HTML

CSS = """
:root{--bg:#fff;--fg:#1b2330;--mut:#5b6678;--card:#f4f6fa;--line:#d5dbe6;--a:#2a6fdb;--b:#d9822b;--c:#2f9e6b;--bad:#c0392b;--good:#1f8a52}
@media (prefers-color-scheme:dark){:root{--bg:#10151f;--fg:#e4e9f2;--mut:#9aa6bb;--card:#18202e;--line:#2c3547;--a:#6ea3f5;--b:#f0a45a;--c:#58c796;--bad:#ff7b6b;--good:#5fd098}}
body{margin:0;padding:24px;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif;max-width:1100px}
h1{font-size:22px;margin:0 0 4px}h2{font-size:16px;margin:28px 0 8px;border-bottom:1px solid var(--line);padding-bottom:4px}
.meta{color:var(--mut)}.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px 16px;margin:10px 0}
.warn{border-left:4px solid var(--b);padding:6px 12px;background:var(--card);margin:6px 0}.bad{color:var(--bad)}.good{color:var(--good)}
table{border-collapse:collapse;width:100%}th,td{text-align:left;padding:3px 8px;border-bottom:1px solid var(--line);font-variant-numeric:tabular-nums}th{color:var(--mut);font-weight:600}
td.n{text-align:right}.tag{font-size:11px;color:var(--mut);border:1px solid var(--line);border-radius:4px;padding:0 4px;margin-left:4px}
svg text{fill:var(--fg);font-size:11px}svg .mut{fill:var(--mut)}svg .axis{stroke:var(--line)}
.sa{fill:var(--a)}.sb{fill:var(--b)}.sc{fill:var(--c)}.sbad{fill:var(--bad)}.sg{fill:var(--mut)}
"""
PALETTE = ["sa", "sb", "sc", "sbad", "sg"]


def e(x) -> str:
    return html.escape(str(x), quote=True)


def bars_svg(rows: list[tuple[str, float, str]], unit: str, title: str, width=760, note=None) -> str:
    """Horizontal bars: (label, value, css class). Value is printed beside each bar so colour is never the only encoding."""
    if not rows:
        return f"<p class='meta'>{e(title)}: no data in this report</p>"
    top = max(v for _, v, _ in rows) or 1.0
    h = 22 * len(rows) + 8
    lab = 210
    parts = [f"<svg role='img' aria-label='{e(title)}' width='{width}' height='{h}' viewBox='0 0 {width} {h}'><title>{e(title)}</title>"]
    for i, (label, v, cls) in enumerate(rows):
        y = 4 + 22 * i
        w = max(1.0, (width - lab - 120) * v / top) if v > 0 else 0
        parts.append(f"<text x='{lab - 6}' y='{y + 13}' text-anchor='end'>{e(label[:34])}</text><rect class='{cls}' x='{lab}' y='{y}' width='{w:.1f}' height='16' rx='2'><title>{e(label)}: {v:,.1f} {e(unit)}</title></rect>"
                     f"<text x='{lab + w + 6:.1f}' y='{y + 13}' class='mut'>{v:,.1f} {e(unit)}</text>")
    parts.append("</svg>")
    return f"<div class='card'><b>{e(title)}</b>" + (f" <span class='meta'>{e(note)}</span>" if note else "") + "".join(parts) + "</div>"


def stacked_svg(rows: list[tuple[str, float, str]], unit: str, title: str, width=760) -> str:
    total = sum(v for _, v, _ in rows)
    if total <= 0:
        return f"<p class='meta'>{e(title)}: nothing to show</p>"
    x = 0.0
    seg, leg = [], []
    for i, (label, v, cls) in enumerate(rows):
        w = (width - 4) * v / total
        seg.append(f"<rect class='{cls}' x='{2 + x:.1f}' y='4' width='{w:.1f}' height='26'><title>{e(label)}: {v:,.1f} {e(unit)} ({100 * v / total:.0f}%)</title></rect>")
        leg.append(f"<tr><td><svg width='12' height='12'><rect class='{cls}' width='12' height='12'/></svg></td><td>{e(label)}</td><td class='n'>{v:,.1f} {e(unit)}</td><td class='n'>{100 * v / total:.0f}%</td></tr>")
        x += w
    return (f"<div class='card'><b>{e(title)}</b><svg role='img' aria-label='{e(title)}' width='{width}' height='34'>{''.join(seg)}</svg>"
            f"<table>{''.join(leg)}</table></div>")


def timeline_svg(frames: list[list[int]], target_us: float, title: str, width=760, height=150) -> str:
    if not frames:
        return f"<p class='meta'>{e(title)}: this report carries no frame timeline (summary mode or shareable form)</p>"
    per = lambda r: r[1] / r[5]   # noqa: E731  a multi-frame observation is drawn at its AVERAGE per frame, in a different colour, with that said in its tooltip
    top = max(max(per(r) for r in frames), target_us * 2) or 1
    n = len(frames)
    bw = (width - 50) / n
    parts = [f"<svg role='img' aria-label='{e(title)}' width='{width}' height='{height + 24}'><title>{e(title)}</title>"]
    y0 = height
    parts.append(f"<line class='axis' x1='40' x2='{width}' y1='{y0}' y2='{y0}'/>")
    ty = y0 - height * target_us / top
    parts.append(f"<line x1='40' x2='{width}' y1='{ty:.1f}' y2='{ty:.1f}' stroke='currentColor' stroke-dasharray='4 3' opacity='.5'/><text class='mut' x='2' y='{ty + 4:.1f}'>native</text>")
    for i, r in enumerate(frames):
        h = height * per(r) / top
        est = r[5] > 1 or r[7]
        cls = "sbad" if r[6] else ("sa" if est else ("sbad" if r[1] > target_us * 1.125 else "sc"))
        what = (f"{r[5]} frames, {r[1] / 1000:.2f} ms total = {r[1] / r[5] / 1000:.2f} ms average (NOT a per-frame measurement)" if est else f"interval {r[1] / 1000:.2f} ms")
        parts.append(f"<rect class='{cls}' x='{40 + i * bw:.2f}' y='{y0 - h:.1f}' width='{max(bw - 0.3, 0.5):.2f}' height='{h:.1f}'><title>frame {r[0]}: {what}, work {r[2] / 1000:.2f} ms, emulation {r[3] / 1000:.2f} ms, present {r[4] / 1000:.2f} ms"
                     + (f", {r[6]} missed deadline(s)" if r[6] else "") + "</title></rect>")
    parts.append(f"<text class='mut' x='40' y='{y0 + 16}'>frame {frames[0][0]}</text><text class='mut' x='{width}' y='{y0 + 16}' text-anchor='end'>frame {frames[-1][0]}</text>"
                 f"<text class='mut' x='2' y='12'>{top / 1000:.1f} ms</text></svg>")
    return f"<div class='card'><b>{e(title)}</b> <span class='meta'>green = one frame, individually timed; red = missed deadline or &gt;12.5% over native frame time; blue = several frames in one observation (average per frame, not a measurement); one bar per observation</span>{''.join(parts)}</div>"


def compare_svg(rows, title: str, width=760) -> str:
    items = [(lab, r) for lab, r in rows if r is not None and r["before"] is not None]
    items = [(lab, r) for lab, r in items if max(abs(r["before"]), abs(r["after"])) > 0][:14]
    if not items:
        return ""
    h = 40 * len(items) + 24
    parts = [f"<svg role='img' aria-label='{e(title)}' width='{width}' height='{h}'><title>{e(title)}</title>"]
    for i, (lab, r) in enumerate(items):
        y = 6 + 40 * i
        top = max(r["before"], r["after"]) or 1
        for j, (val, cls) in enumerate(((r["before"], "sg"), (r["after"], "sbad" if r["worse"] and r["significant"] else ("sc" if r["significant"] else "sa")))):
            w = (width - 300) * val / top
            parts.append(f"<rect class='{cls}' x='210' y='{y + 16 * j}' width='{max(w, 1):.1f}' height='13' rx='2'><title>{'before' if j == 0 else 'after'}: {e(r['fmt'](val))}</title></rect>"
                         f"<text class='mut' x='{216 + max(w, 1):.1f}' y='{y + 16 * j + 11}'>{e(r['fmt'](val))}</text>")
        parts.append(f"<text x='204' y='{y + 20}' text-anchor='end'>{e(lab[:32])}</text>")
    parts.append("</svg>")
    return f"<div class='card'><b>{e(title)}</b> <span class='meta'>grey = before; green = significantly better, red = significantly worse, blue = within noise floor</span>{''.join(parts)}</div>"


def report_section(d: dict, heading: str) -> str:
    out = [f"<h2>{e(heading)}</h2>", f"<p class='meta'>{e(d['kind'])} report, platform {e(d['platform'])}, status <b>{e(d['status'])}</b>, privacy {e(d['privacy'])}; "
           "<span class='tag'>measured</span> <span class='tag'>derived</span> <span class='tag'>unavailable</span> labels as in the console summary</p>"]
    if d["status"] != "complete":
        out.append(f"<div class='warn bad'>This report is {e(d['status'])}; its figures cover only what ran before it ended.</div>")
    if d["kind"] == "build":
        b = d["build"]
        c = b["compile"]
        k = build_derived(d)
        out.append(f"<div class='card'>wall <b>{b['wall_ms']:,.0f} ms</b> <span class='tag'>measured</span> &nbsp; compile batches {c['batch_wall_ms']:,.0f} ms vs aggregate jobs {c['aggregate_job_ms']:,.0f} ms"
                   + (f" &rarr; parallelism <b>{k['parallelism']:.2f}x</b> <span class='tag'>derived</span>" if k["parallelism"] else "") + "</div>")
        out.append(bars_svg([(s["name"], s["exclusive_ms"], "sa") for s in sorted(b["stages"], key=lambda s: -s["exclusive_ms"])], "ms", "Build stages (exclusive time)", note="nested stages are not double counted"))
        out.append(bars_svg([(s["name"], s["exclusive_ms"], "sb") for s in sorted(b["operations"], key=lambda s: -s["exclusive_ms"])], "ms", "Operations (exclusive time)"))
        out.append(bars_svg([(x["category"], x["job_ms"], "sa") for x in sorted(c["categories"], key=lambda x: -x["job_ms"]) if x["job_ms"] > 0], "ms", "Compilation cost by category (aggregate job time)"))
        if "slowest_units" in c:
            out.append(bars_svg([(u["label"], u["wall_ms"], "sbad" if u["disposition"] == "superseded" else "sb") for u in c["slowest_units"]], "ms", "Slowest generated translation units",
                                note="red = superseded (discarded)"))
        else:
            out.append("<p class='meta'>Slowest units: not part of the shareable form.</p>")
        used = c["aggregate_job_ms"] - c["wasted_ms"]
        out.append(stacked_svg([("used by the executable or the build-time observation program", used, "sc"), ("discarded (superseded by a later round)", c["wasted_ms"], "sbad")], "ms",
                               "Compilation work: kept versus discarded"))
        out.append(stacked_svg([("compiled", c["compiled"], "sa"), ("reused", c["reused"], "sc")], "units", "Units: compiled versus reused from cache"))
    else:
        r, k = d["runtime"], runtime_derived(d)
        f = r["frames"]
        out.append(f"<div class='card'>{f['virtual']:,} virtual frames, {f['presented']:,} presented; speed "
                   + (f"<b>{100 * k['speed']:.1f}%</b> of native <span class='tag'>derived</span>" if k["speed"] is not None else "<span class='tag'>unavailable</span>")
                   + f"; interval p50 {r['frame_interval']['p50_ns'] / 1e6:.2f} / p95 {r['frame_interval']['p95_ns'] / 1e6:.2f} / p99 {r['frame_interval']['p99_ns'] / 1e6:.2f} / max {r['frame_interval']['max_ns'] / 1e6:.2f} ms</div>")
        out.append(timeline_svg(r.get("timeline", {}).get("frames", []), d["config"]["target_frame_ns"] / 1000, "Frame interval timeline (last recorded frames)"))
        rows = [(s["name"] + (" (inside " + s["parent"] + ")" if s.get("parent") else ""), k["per_frame_ns"][s["name"]] / 1e6, "sb" if s["class"] == "inclusive" else "sa")
                for s in r["spans"] if s["name"] in k["per_frame_ns"]]
        out.append(bars_svg(rows, "ms/frame", "Runtime subsystem breakdown (average per virtual frame)", note="orange = inclusive span (contained in another), blue = exclusive; do not add inclusive rows to their parent"))
    if d.get("unavailable"):
        out.append("<div class='card'><b>Unavailable measurements</b><table>" + "".join(f"<tr><td>{e(u['metric'])}</td><td>{e(u['reason'])}</td></tr>" for u in d["unavailable"]) + "</table></div>")
    out.append("<div class='card'><b>Measured shares</b><ul>" + "".join(f"<li>{e(x)}</li>" for x in bottleneck_lines(d)) + "</ul></div>")
    return "\n".join(out)


def html_report(reports: list[dict], names: list[str], mod: bool = False) -> str:
    body = ["<h1>segarecomp performance report</h1>",
            "<p class='meta'>Self-contained: no network, no external script or font. Values come only from the report file(s); derived values state their rule in the console summary.</p>"]
    if len(reports) == 2:
        warn = comparability(reports[0], reports[1], mod)
        for w in warn:
            body.append(f"<div class='warn {'bad' if w.startswith('INCOMPARABLE') else ''}'>{e(w)}</div>")
        if not any(w.startswith("INCOMPARABLE") for w in warn):
            rows = compare_rows(*reports)
            body.append("<h2>Before / after</h2>")
            body.append(compare_svg(rows, "Largest changes"))
            body.append("<table><tr><th>metric</th><th>before</th><th>after</th><th>change</th><th>%</th></tr>" + "".join(
                f"<tr><td>{e(lab)}</td><td class='n'>{e(r['fmt'](r['before']))}</td><td class='n'>{e(r['fmt'](r['after']))}</td>"
                f"<td class='n {'bad' if r['significant'] and r['worse'] else ('good' if r['significant'] else '')}'>{'+' if r['diff'] >= 0 else '-'}{e(r['fmt'](abs(r['diff'])))}</td>"
                f"<td class='n'>{'n/a' if math.isinf(r['pct']) else format(r['pct'], '+.1f') + '%'}</td></tr>"
                for lab, r in rows if r is not None) + "</table>")
        for rep, name in zip(reports, ("Before", "After")):
            body.append(report_section(rep, f"{name}: {names[0 if name == 'Before' else 1]}"))
    else:
        body.append(report_section(reports[0], names[0]))
    return f"<!doctype html><html lang='en'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>segarecomp performance</title><style>{CSS}</style></head><body>{''.join(body)}</body></html>\n"


# --------------------------------------------------------------------------------------------------------------------------------------- CLI

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="perf_report.py", description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("validate"); s.add_argument("reports", nargs="+")
    s = sub.add_parser("summary"); s.add_argument("report")
    s = sub.add_parser("compare"); s.add_argument("before"); s.add_argument("after"); s.add_argument("--html"); s.add_argument("--mod", action="store_true", help="vanilla vs modded/patched builds: a different program is expected")
    s = sub.add_parser("html"); s.add_argument("reports", nargs="+"); s.add_argument("-o", "--output", required=True)
    s = sub.add_parser("sanitize"); s.add_argument("report"); s.add_argument("-o", "--output", required=True)
    s = sub.add_parser("check-shareable"); s.add_argument("report")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "validate":
            for p in args.reports:
                load(p)
                print(f"ok  {p}")
        elif args.cmd == "summary":
            print(summary_text(load(args.report)))
        elif args.cmd == "compare":
            a, b = load(args.before), load(args.after)
            print(compare_text(a, b, args.mod))
            if args.html:
                Path(args.html).write_text(html_report([a, b], [args.before, args.after], args.mod), encoding="utf-8")
                print(f"wrote {args.html}")
            if any(w.startswith("INCOMPARABLE") for w in comparability(a, b, args.mod)):
                return 3
        elif args.cmd == "html":
            if len(args.reports) > 2:
                raise ReportError("html takes one report, or two for before/after")
            reps = [load(p) for p in args.reports]
            if len(reps) == 2 and reps[0]["kind"] != reps[1]["kind"]:
                raise ReportError("cannot compare a build report with a runtime report")
            Path(args.output).write_text(html_report(reps, [Path(p).name for p in args.reports]), encoding="utf-8")
            print(f"wrote {args.output}")
        elif args.cmd == "sanitize":
            d = load(args.report)
            Path(args.output).write_text(json.dumps(sanitize(d), indent=1, sort_keys=False) + "\n", encoding="utf-8")
            print(f"wrote {args.output} (shareable)")
        elif args.cmd == "check-shareable":
            d = load(args.report)
            if d["privacy"] != "shareable" or sanitize(d) != d:
                print(f"{args.report}: NOT shareable (privacy={d['privacy']}; contains fields outside the allow-list)", file=sys.stderr)
                return 1
            print(f"ok  {args.report} is shareable")
    except ReportError as exc:
        print(f"perf_report: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
