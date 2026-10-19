#!/usr/bin/env python3
"""SEG-074-T001 (ADR 0156/0157): exact-code oracle and compilation-cost floor benchmark (offline research tool).

Measures real generated-native compilation of Genesis MC68000 immutable-ROM images under five admission strategies, using the unchanged
CPU-owned decoder, lowering, emitter, structural prune and admission validator:

  BROAD          broad immutable-ROM AOT (Compatibility)
  INC            production M3 + B35 as deployed (native gen2 route; its own validator fallback to broad)
  M3RB50         saved SEG-072 M3-R + B50 out-of-fold regions -> unchanged prune + validator (rejection => broad fallback)
  ORACLE_REGION  every 128-byte cell not proven pure data (any I/c/U byte) + seed cells -> unchanged prune + validator
  ORACLE_INSTR   every even I (labelled start) or U (unknown) position as 2-byte ranges -> report-only least closed superset (`close`)
                 -> unchanged validator

The two oracles read source-backed truth: they are diagnostic floors, never producers and never evidence that an arbitrary image's code
can be completely identified. Nothing here changes production behaviour.

Subcommands:
  fixture    synthetic exact-map MC68000 image end to end (project-authored bytes; used by the CTest)
  measure    measure the private corpus images (requires the SEG-072 handoff bundle and an ignored work directory)
  density    count-only instruction/region density over the whole qualified development registry
  aggregate  per-lineage aggregation and the pre-registered G1/G2/G3 gate -> public aggregate record

Privacy: plans, generated C, objects, executables and per-image work files hold exact addresses or commercial-derived code; they stay in an
ignored work directory. The public record holds aggregates, identities/digests and classes only.
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import hashlib
import json
import math
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

SCHEMA = "segarecomp.oracle_bench.v1"
CELL = 128
STRATEGIES = ("BROAD", "INC", "M3RB50", "ORACLE_REGION", "ORACLE_INSTR")
# Non-gating attribution diagnostic added after the first measurement (ADR 0157, not pre-registered): labelled I starts only (unknown U bytes
# excluded - an optimistic floor that is NOT safe where U holds code), closed upward. Never part of the gate.
DIAGNOSTICS = ("DIAG_INSTR_LABELLED",)
# ADR 0156: the gate set (development Genesis images whose broad AOT is accepted) and its lineages - fixed before measurement.
GATE_IMAGES = {"s1": "D01", "s2": "D01", "sk": "D01", "flicky": "D02", "sf1": "D03", "sf2": "D03", "ps2": "D04",
               "d05": "D05", "d06": "D06", "d07": "D07", "d09": "D09"}
# ADR 0156 gate thresholds (absolute; applied unchanged).
G1_G = 0.20            # instruction vs region oracle, primary cost (total cold build CPU)
G1_GC = 0.20           # instruction vs region oracle, generated C bytes
G1_H = 0.30            # instruction oracle vs incumbent as deployed, primary cost
G1_PER_LINEAGE = 0.10  # per-lineage instruction-vs-region gain ...
G1_MIN_LINEAGES = 5    # ... on at least this many lineages
G1_MIN_ACCEPTED = 6    # ORACLE_INSTR validator-accepted on at least this many lineages (all images of the lineage)
G2_RHO = 0.20          # representation headroom
OPT_FLAGS = {"O2": ["-O2"], "O0": ["-O0"]}
BASE_FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic"]
# SEG-074-T002 / ADR 0158: opt-in emitter switches applied to every emission and runtime build of a run (set by --emit-flag).
EMIT_FLAGS: list = []
RUNTIME_KEYS = ("outcome", "frames", "final_state", "coverage", "frame_stream")


# ================================================================================================================== pure helpers
def merge_ranges(ranges) -> list[tuple[int, int]]:
    """Sorted, disjoint, NON-ADJACENT half-open ranges (the region format rejects touching ranges)."""
    out: list[list[int]] = []
    for b, e in sorted(ranges):
        if b >= e or b & 1 or e & 1:
            raise ValueError("ranges must be non-empty and even")
        if out and b <= out[-1][1]:
            out[-1][1] = max(out[-1][1], e)
        else:
            out.append([b, e])
    return [(b, e) for b, e in out]


def regions_text(sha: str, ranges) -> str:
    merged = merge_ranges(ranges)
    if not merged:
        raise ValueError("empty proposal")
    return ("segarecomp.m68k_executable_regions.v1\n" f"rom_sha256 {sha}\n" + "".join(f"range {b:08x} {e:08x}\n" for b, e in merged) + "end\n")


def cell_ranges(cells, size: int, cell: int = CELL) -> list[tuple[int, int]]:
    out = []
    for c in sorted(set(cells)):
        b, e = c * cell, min((c + 1) * cell, size)
        if c < 0 or b >= size:
            raise ValueError("cell outside the image")
        out.append((b, e + (e & 1)))
    return out


def oracle_region_cells(classes: bytes, seeds, cell: int = CELL) -> set[int]:
    """Every cell that is not proven pure data (holds any I, c or U byte; an all-D cell is the only exclusion) plus the seed cells."""
    n = (len(classes) + cell - 1) // cell
    keep = {c for c in range(n) if classes[c * cell:(c + 1) * cell].count(b"D") != len(classes[c * cell:(c + 1) * cell])}
    return keep | {int(s) for s in seeds if 0 <= int(s) < n}


def oracle_instr_ranges(classes: bytes) -> list[tuple[int, int]]:
    """Every even position classed I (labelled start) or U (unknown: never assumed non-executable), as merged 2-byte ranges."""
    return merge_ranges((p, p + 2) for p in range(0, len(classes) - 1, 2) if classes[p] in (0x49, 0x55))


def labelled_starts(classes: bytes) -> list[int]:
    return [p for p in range(0, len(classes), 2) if classes[p] == 0x49]


def admitted_from_plan(plan_text: str, universe) -> set[int]:
    import bisect
    ranges = [tuple(int(x, 16) for x in ln.split()[1:3]) for ln in plan_text.splitlines() if ln.startswith("range ")]
    if not ranges:  # a broad plan admits everything
        return set(universe)
    starts = [r[0] for r in ranges]
    out = set()
    for a in universe:
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and a < ranges[i][1]:
            out.add(a)
    return out


_LABEL = re.compile(r"^genesis_aot_entry_[0-9A-Fa-f]+:")
_HEX = re.compile(r"0[xX][0-9A-Fa-f]+")
_DEC = re.compile(r"\b\d{4,}\b")


def normalize_segment(text: str) -> str:
    text = _LABEL.sub("L:", text, count=1)
    return _DEC.sub("#", _HEX.sub("0x#", text))


def aot_segments(text: str):
    """Yield the text of each per-identity entry segment: from a `genesis_aot_entry_*:` label line to the first column-0 `}` line."""
    lines = text.splitlines(keepends=True)
    i = 0
    while i < len(lines):
        if _LABEL.match(lines[i]):
            j = i
            while j + 1 < len(lines) and lines[j].rstrip("\n") != "}":
                j += 1
            yield "".join(lines[i:j + 1])
            i = j + 1
        else:
            i += 1


def compiled_entry_rows(text: str) -> int:
    """Rows of the generated `genesis_compiled_entry_addresses[]` dispatch table (sharded `0x...u,` or single-file `UINT32_C(0x...),` form)."""
    m = re.search(r"genesis_compiled_entry_addresses\[\] = \{\n(.*?)\n\};", text, re.S)
    if not m:
        return 0
    return sum(1 for ln in m.group(1).splitlines() if re.fullmatch(r"\s*(?:0x[0-9A-Fa-f]{8}u|UINT32_C\(0x[0-9A-Fa-f]{8}\)),", ln))


def generated_counts(directory: pathlib.Path) -> dict:
    """Counts from the generated C (labels, owners, dispatch cases, compiled-entry table rows, block functions) and the ADR 0156 rho
    numerator: bytes of entry segments whose normalized text duplicates an earlier segment of the same build."""
    files = sorted(p for p in directory.iterdir() if p.is_file() and p.suffix in (".c", ".h"))
    total = 0
    counts = {"entry_segments": 0, "aot_owner_functions": 0, "aot_shared_helpers": 0, "dispatch_cases": 0, "compiled_entry_rows": 0,
              "block_functions": 0, "duplicate_segment_bytes": 0, "segment_bytes": 0}
    seen: set[str] = set()
    for p in files:
        text = p.read_text(encoding="utf-8", errors="replace")
        total += len(text.encode("utf-8", errors="replace"))
        counts["aot_owner_functions"] += len(re.findall(r"^GenesisControlTransfer genesis_aot_owner_\w+\(", text, re.M))
        counts["block_functions"] += len(re.findall(r"^(?:static )?GenesisControlTransfer genesis_block_\w+\(", text, re.M))
        counts["dispatch_cases"] += len(re.findall(r"^\s*case UINT32_C\(", text, re.M))
        counts["aot_shared_helpers"] += len(re.findall(r"^GenesisControlTransfer genesis_aot_shared_\w+\(", text, re.M))
        counts["compiled_entry_rows"] += compiled_entry_rows(text)
        for seg in aot_segments(text):
            counts["entry_segments"] += 1
            n = len(seg.encode())
            counts["segment_bytes"] += n
            key = hashlib.sha256(normalize_segment(seg).encode()).hexdigest()
            if key in seen:
                counts["duplicate_segment_bytes"] += n
            else:
                seen.add(key)
    counts["generated_bytes"] = total
    counts["rho"] = counts["duplicate_segment_bytes"] / total if total else 0.0
    return counts


def _segment_address(seg: str) -> int:
    return int(re.match(r"genesis_aot_entry_([0-9A-Fa-f]+):", seg).group(1), 16)


def _literals(text: str) -> list[int]:
    return [int(x, 16) for x in _HEX.findall(text)] + [int(x) for x in _DEC.findall(text)]


def rho_decomposition(directory: pathlib.Path) -> dict:
    """Non-gating (ADR 0157) refinement of rho: of the duplicate entry-segment bytes, the share whose body (provenance line excluded - existing
    shared helpers already take it as a parameter) differs only in PC-derived literals (every differing literal shifts by the address delta),
    and the distribution of non-PC literal differences per duplicate (an operand parameter would be needed for each)."""
    first: dict[str, str] = {}
    total = dup = pc_only = 0
    by_operands = {"0": 0, "1": 0, "2": 0, "3+": 0}
    body = lambda seg: "\n".join(ln for ln in seg.splitlines()[1:] if "GenesisInstructionProvenance" not in ln)  # noqa: E731
    for p in sorted(x for x in directory.rglob("*") if x.is_file() and x.suffix in (".c", ".h")):
        text = p.read_text(encoding="utf-8", errors="replace")
        total += len(text.encode("utf-8", errors="replace"))
        for seg in aot_segments(text):
            key = hashlib.sha256(normalize_segment(seg).encode()).hexdigest()
            if key not in first:
                first[key] = seg
                continue
            n = len(seg.encode())
            dup += n
            a = first[key]
            delta = _segment_address(seg) - _segment_address(a)
            diffs = [y - x for x, y in zip(_literals(body(a)), _literals(body(seg))) if y != x]
            operands = sum(1 for d in diffs if d != delta)
            pc_only += n if operands == 0 else 0
            by_operands[str(operands) if operands < 3 else "3+"] += n
    return {"rho": dup / total if total else 0.0, "rho_pc_only": pc_only / total if total else 0.0,
            "rho_by_operand_literals": {k: v / total if total else 0.0 for k, v in by_operands.items()}}


def deployed(strategy_rec: dict, broad_rec: dict) -> dict:
    """The deployed cost of a strategy: its own build when accepted, else the broad build plus the cost of the rejected attempt."""
    if strategy_rec.get("accepted"):
        return {"cpu": strategy_rec["total_cpu"], "generated_bytes": strategy_rec["generated_bytes"], "fallback": False}
    if "total_cpu" in strategy_rec:  # a route that fell back inside its own build (INC): its measured build already contains the attempt
        return {"cpu": strategy_rec["total_cpu"], "generated_bytes": strategy_rec["generated_bytes"], "fallback": True}
    return {"cpu": broad_rec["total_cpu"] + strategy_rec.get("plan_cpu", 0.0), "generated_bytes": broad_rec["generated_bytes"], "fallback": True}


def lineage_means(per_image: dict, fn) -> dict:
    """{lineage: mean over its images of fn(image record)} for images where fn returns a number."""
    acc: dict[str, list[float]] = {}
    for iid, rec in per_image.items():
        v = fn(rec)
        if v is not None:
            acc.setdefault(GATE_IMAGES.get(iid, rec.get("lineage", "?")), []).append(v)
    return {L: sum(v) / len(v) for L, v in sorted(acc.items())}


def evaluate_gate(per_image: dict) -> dict:
    """ADR 0156 gate over per-image records {id: {"strategies": {...}, "lineage": L}}; returns the figures and the selected class."""
    def ratio(a, b, key):
        def f(rec):
            s = rec.get("strategies", {})
            if "BROAD" not in s or a not in s or b not in s or not s["BROAD"].get("accepted"):
                return None
            x, y = deployed(s[a], s["BROAD"]), deployed(s[b], s["BROAD"])
            return x[key] / y[key] if y[key] else None
        return f
    instr_region_cpu = lineage_means(per_image, ratio("ORACLE_INSTR", "ORACLE_REGION", "cpu"))
    instr_region_c = lineage_means(per_image, ratio("ORACLE_INSTR", "ORACLE_REGION", "generated_bytes"))
    instr_inc_cpu = lineage_means(per_image, ratio("ORACLE_INSTR", "INC", "cpu"))
    lineages = sorted(set(instr_region_cpu) | set(instr_inc_cpu))
    mean = lambda d: (sum(d.values()) / len(d)) if d else None  # noqa: E731
    g = 1 - mean(instr_region_cpu) if instr_region_cpu else None
    g_c = 1 - mean(instr_region_c) if instr_region_c else None
    h = 1 - mean(instr_inc_cpu) if instr_inc_cpu else None
    per_lineage_gain = {L: 1 - v for L, v in instr_region_cpu.items()}
    accepted = {}
    for iid, rec in per_image.items():
        L = GATE_IMAGES.get(iid, rec.get("lineage", "?"))
        ok = bool(rec.get("strategies", {}).get("ORACLE_INSTR", {}).get("accepted"))
        accepted[L] = accepted.get(L, True) and ok
    runtime_ok = {}
    for iid, rec in per_image.items():
        for name in ("ORACLE_INSTR", "ORACLE_REGION"):
            s = rec.get("strategies", {}).get(name, {})
            if s.get("accepted") and s.get("runtime_equal") is not None:
                runtime_ok.setdefault(name, []).append(bool(s["runtime_equal"]))
    rho_vals = lineage_means(per_image, lambda r: r.get("strategies", {}).get("ORACLE_REGION", {}).get("counts", {}).get("rho")
                             if r.get("strategies", {}).get("ORACLE_REGION", {}).get("accepted")
                             else r.get("strategies", {}).get("INC", {}).get("counts", {}).get("rho"))
    rho = mean(rho_vals)
    g1_parts = {
        "g": g is not None and g >= G1_G,
        "g_c": g_c is not None and g_c >= G1_GC,
        "h": h is not None and h >= G1_H,
        "per_lineage": sum(1 for v in per_lineage_gain.values() if v >= G1_PER_LINEAGE) >= G1_MIN_LINEAGES,
        "accepted": sum(1 for v in accepted.values() if v) >= G1_MIN_ACCEPTED,
    }
    runtime_blocker = not all(runtime_ok.get("ORACLE_INSTR", [True]))
    if all(g1_parts.values()) and not runtime_blocker:
        decision = "G1"
    elif rho is not None and rho >= G2_RHO:
        decision = "G2"
    else:
        decision = "G3"
    return {"lineages": lineages, "g": g, "g_c": g_c, "h": h, "per_lineage_instr_vs_region_gain": per_lineage_gain,
            "lineages_gain_ge_threshold": sum(1 for v in per_lineage_gain.values() if v >= G1_PER_LINEAGE),
            "oracle_instr_accepted_lineages": sum(1 for v in accepted.values() if v), "rho_lineage": rho_vals, "rho": rho,
            "g1_conditions": g1_parts, "runtime_blocker": runtime_blocker, "runtime_equal": {k: f"{sum(v)}/{len(v)}" for k, v in runtime_ok.items()},
            "decision": decision}


# ================================================================================================================ process measurement
def run_timed(cmd, cwd=None, stdout=subprocess.DEVNULL) -> dict:
    """Run one build-time process (CLI or compiler; never a generated program - those go through the startup bridge with an explicit
    instruction budget and wall timeout); wall, user+system CPU and peak RSS from wait4. `ru_maxrss` is KiB on Linux (bytes on macOS)."""
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, cwd=cwd, stdout=stdout, stderr=subprocess.PIPE)
    err = b""
    try:
        err = proc.stderr.read() if proc.stderr else b""
    finally:
        if proc.stderr:
            proc.stderr.close()
    while True:
        pid, status, ru = os.wait4(proc.pid, 0)
        if pid == proc.pid:
            break
    proc.returncode = os.waitstatus_to_exitcode(status)
    wall = time.monotonic() - t0
    return {"rc": proc.returncode, "wall": wall, "cpu": ru.ru_utime + ru.ru_stime, "rss_kib": ru.ru_maxrss, "stderr": err.decode(errors="replace")}


def tu_family(path) -> str:
    """Translation-unit family of a generated/runtime source (`bridge_generated_<family>_NN.c`, main, single file, runtime)."""
    name = pathlib.Path(path).name
    if name == "runtime.c":
        return "runtime"
    m = re.fullmatch(r"bridge_generated_([a-z]+)(?:_\d+)?\.c", name)
    return m.group(1) if m else "single"


def compile_cold(cc: str, flags: list, units: list, include: list, jobs: int, objdir: pathlib.Path) -> dict:
    """Compile every TU (no object cache) with `jobs` concurrent processes; per-process CPU/RSS from wait4. Objects follow unit order."""
    objdir.mkdir(parents=True, exist_ok=True)
    base = [cc] + flags + include
    objects = [objdir / f"{i}.o" for i in range(len(units))]
    t0 = time.monotonic()
    with cf.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        recs = list(pool.map(lambda i: run_timed(base + ["-c", "-o", str(objects[i]), str(units[i])]), range(len(units))))
    wall = time.monotonic() - t0
    bad = [r for r in recs if r["rc"] != 0]
    if bad:
        raise RuntimeError("compile failed: " + bad[0]["stderr"][-2000:])
    families: dict[str, float] = {}
    for u, r in zip(units, recs):
        families[tu_family(u)] = families.get(tu_family(u), 0.0) + r["cpu"]
    return {"base": base, "objects": objects, "compile_cpu": sum(r["cpu"] for r in recs), "compile_wall": wall, "family_cpu": families,
            "peak_compiler_rss_kib": max(r["rss_kib"] for r in recs), "object_bytes": sum(o.stat().st_size for o in objects), "units": len(units)}


def cache_rebuild(base: list, units: list, objects: list, cwd: pathlib.Path, cache: pathlib.Path) -> dict:
    """Cache-assisted rebuild through the production content-addressed object cache: store the cold objects under their production keys,
    then rebuild with `compile_objects` (hits verified; a miss compiles)."""
    import genesis_startup_bridge as gsb
    shutil.rmtree(cache, ignore_errors=True)
    cache.mkdir(parents=True)
    t0 = time.monotonic()
    keys = 0
    for u, o in zip(units, objects):
        key = gsb.object_cache_key(base, pathlib.Path(u), cwd)
        if key is not None:
            gsb.object_cache_store(cache, key, str(o))
            keys += 1
    key_wall = time.monotonic() - t0
    gsb._object_cache_dir_override = cache
    before = dict(gsb._object_cache_stats)
    try:
        with tempfile.TemporaryDirectory(dir=str(cache.parent)) as tmp:
            t1 = time.monotonic()
            objs, failure = gsb.compile_objects([(base, pathlib.Path(u)) for u in units], cwd, pathlib.Path(tmp))
            warm_wall = time.monotonic() - t1
    finally:
        gsb._object_cache_dir_override = None
    delta = {k: v - before.get(k, 0) for k, v in gsb._object_cache_stats.items()}
    shutil.rmtree(cache, ignore_errors=True)
    return {"keyed_units": keys, "populate_key_wall": key_wall, "warm_wall": warm_wall, "hits": delta.get("hits", 0), "misses": delta.get("misses", 0),
            "failed": failure is not None}


def parse_kv(line: str) -> dict:
    return dict(tok.split("=", 1) for tok in line.split() if "=" in tok)


# ===================================================================================================================== one strategy
def make_plan(cli: str, rom: pathlib.Path, sha: str, proposal: str, work: pathlib.Path, closure: str) -> dict:
    work.mkdir(parents=True, exist_ok=True)
    rp, pp = work / "proposal.regions", work / "admission.plan"
    rp.write_text(proposal, newline="\n")
    pp.unlink(missing_ok=True)
    r = run_timed([cli, "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry", "--rom-sha256", sha, "--immutable-rom-aot",
                   "--immutable-aot-region-proposal", str(rp), "--region-admission-plan-output", str(pp), "--region-proposal-closure", closure])
    line = next((ln for ln in r["stderr"].splitlines() if "region prune:" in ln or "region close:" in ln), "")
    rejected = next((ln.split("segarecomp:", 1)[-1].strip() for ln in r["stderr"].splitlines() if "REJECTED" in ln or "rejected" in ln), "")
    ok = r["rc"] == 0 and pp.exists()
    stats = {k: v for k, v in parse_kv(line).items() if k in ("universe", "k0", "k", "pruned", "added", "roots_added", "rounds", "ranges")}
    reason = re.sub(r"\b(universe|k0|rounds)=\d+", "", rejected).strip() if not ok else ""
    return {"accepted": ok, "plan": pp if ok else None, "plan_cpu": r["cpu"], "plan_wall": r["wall"], "plan_rss_kib": r["rss_kib"],
            "stats": stats, "reason": reason}


def emit(cli: str, rom: pathlib.Path, sha: str, extra: list, out: pathlib.Path) -> dict:
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    shard = out / "generated"
    r = run_timed([cli, "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry", "--rom-sha256", sha, "--immutable-rom-aot"] + extra + EMIT_FLAGS +
                  ["--generated-c-output", str(out / "bridge.generated.c"), "--generated-c-shard-dir", str(shard)])
    if r["rc"] != 0:
        raise RuntimeError("emission failed: " + r["stderr"][-1500:])
    units = [shard / ln for ln in (shard / "bridge_generated.units").read_text().splitlines() if ln] if (shard / "bridge_generated.units").is_file() \
        else [out / "bridge.generated.c"]
    admission = next((ln for ln in r["stderr"].splitlines() if "m68k admission:" in ln), "")
    enum = next((ln for ln in r["stderr"].splitlines() if "immutable-rom AOT enumeration:" in ln), "")
    return {"emit_cpu": r["cpu"], "emit_wall": r["wall"], "emit_rss_kib": r["rss_kib"], "units": units,
            "dir": shard if shard.is_dir() else out, "admission": {k: v for k, v in parse_kv(admission.split("m68k admission:", 1)[-1]).items()
                                                                   if not k.endswith("sha256")},
            "enumeration": parse_kv(enum.split("enumeration:", 1)[-1])}


def runtime_check(cli: str, rom: pathlib.Path, extra: list, out: pathlib.Path, frames: int, cc: str, timeout: int) -> dict:
    """Bounded generated-native execution through the startup bridge (explicit instruction budget + wall timeout; quick profile)."""
    import genesis_ml_gen2_differential as diff
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    budget = max(10_000_000, frames * 100_000)
    cmd = [sys.executable, str(ROOT / "tools/genesis_startup_bridge.py"), "--segarecomp", cli, "--rom", str(rom), "--mode", "commercial",
           "--immutable-rom-aot", "--execution-coverage", str(frames), "--coverage-epoch-frames", str(frames), "--coverage-no-render",
           "--instruction-budget", str(budget), "--out-dir", str(out), "--cc", cc, "--build-profile", "quick"] + extra
    for flag in EMIT_FLAGS:
        cmd += ["--emitter-flag", flag]
    done = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    line = next((ln for ln in done.stderr.splitlines() if ln.startswith("COVERAGE_SUMMARY ")), None)
    rec: dict = {"ok": line is not None, "instruction_budget": budget}
    if line:
        d = json.loads(line.split(" ", 1)[1])
        rec.update({"outcome": d["outcome"], "frames": d["frames_published"], "final_state": d["final_state_digest"],
                    "coverage": d["coverage_digest"], "frame_stream": d["frame_stream_digest"]})
        try:
            rec["_pcs"] = sorted(diff.executed_pcs(out, rom.stat().st_size))
        except OSError:
            rec["_pcs"] = None
    else:
        rec["_stderr_tail"] = done.stderr[-800:]
    shutil.rmtree(out / "generated", ignore_errors=True)
    for p in out.glob("*.generated.c"):
        p.unlink()
    return rec


def build_strategy(cli: str, rom: pathlib.Path, sha: str, extra: list, work: pathlib.Path, cc: str, opt: str, jobs: int, cache: bool) -> dict:
    e = emit(cli, rom, sha, extra, work / "emit")
    counts = generated_counts(e["dir"])
    runtime_dir = ROOT / "platforms" / "genesis" / "runtime"
    units = [str(u) for u in e["units"]] + [str(runtime_dir / "runtime.c")]
    include = ["-I", str(runtime_dir), "-I", str(e["dir"])]
    flags = BASE_FLAGS + OPT_FLAGS[opt]
    c = compile_cold(cc, flags, units, include, jobs, work / "obj")
    exe = work / "bridge"
    link = run_timed([cc, "-o", str(exe)] + [str(o) for o in c["objects"]], cwd=str(ROOT))
    if link["rc"] != 0:
        raise RuntimeError("link failed: " + link["stderr"][-1500:])
    rec = {"emit_cpu": e["emit_cpu"], "emit_wall": e["emit_wall"], "emit_rss_kib": e["emit_rss_kib"], "admission": e["admission"],
           "enumeration": e["enumeration"], "counts": counts, "generated_bytes": counts["generated_bytes"], "tu_count": len(e["units"]),
           "compile_cpu": c["compile_cpu"], "compile_wall": c["compile_wall"], "peak_compiler_rss_kib": c["peak_compiler_rss_kib"],
           "family_cpu": c["family_cpu"], "object_bytes": c["object_bytes"], "link_cpu": link["cpu"], "link_wall": link["wall"], "executable_bytes": exe.stat().st_size,
           "flags": flags, "jobs": jobs, "compiler": cc}
    if cache:
        rec["cache"] = cache_rebuild(c["base"], units, c["objects"], ROOT, work / "cache")
    shutil.rmtree(work / "obj", ignore_errors=True)
    shutil.rmtree(work / "emit", ignore_errors=True)
    exe.unlink(missing_ok=True)
    return rec


def measure_image(cli: str, rom: pathlib.Path, sha: str, classes: bytes | None, seeds, m3r_cells, work: pathlib.Path, cc: str, opt: str,
                  jobs: int, frames: int, timeout: int, strategies=STRATEGIES, cache: bool = True, runtime: bool = True) -> dict:
    """All strategies for one image. classes=None (no truth) skips the oracles; m3r_cells=None skips M3RB50."""
    size = rom.stat().st_size
    work.mkdir(parents=True, exist_ok=True)
    out: dict = {"rom_bytes": size, "strategies": {}}
    # the current CLI's broad universe (identity addresses), for C outside K
    with tempfile.TemporaryDirectory(dir=str(work)) as tmp:
        rep = pathlib.Path(tmp) / "u.txt"
        r = run_timed([cli, "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry", "--rom-sha256", sha, "--immutable-rom-aot",
                       "--immutable-aot-address-report", str(rep), "--generated-c-output", str(pathlib.Path(tmp) / "u.c")])
        universe = sorted(int(t, 16) for t in rep.read_text().split()) if r["rc"] == 0 else []
    starts = labelled_starts(classes) if classes is not None else []
    uset = set(universe)
    out["universe"] = len(universe)
    out["labelled_starts"] = len(starts)
    out["labelled_starts_in_universe"] = sum(1 for s in starts if s in uset)
    reference = None
    for name in strategies:
        sw = work / name
        rec: dict = {}
        extra: list = []
        plan = None
        if name == "BROAD":
            rec.update({"accepted": True, "plan_cpu": 0.0})
        elif name == "INC":
            extra = ["--immutable-rom-aot-ml-gen2-admission"]
            rec.update({"plan_cpu": 0.0})  # inference and prune run inside the emission (counted in emit_cpu)
        else:
            if name == "M3RB50":
                if m3r_cells is None:
                    continue
                proposal, closure = regions_text(sha, cell_ranges(m3r_cells, size)), "prune"
            elif classes is None:
                continue
            elif name == "ORACLE_REGION":
                proposal, closure = regions_text(sha, cell_ranges(oracle_region_cells(classes, seeds), size)), "prune"
            elif name == "DIAG_INSTR_LABELLED":
                proposal, closure = regions_text(sha, merge_ranges((p, p + 2) for p in labelled_starts(classes))), "close"
            else:
                proposal, closure = regions_text(sha, oracle_instr_ranges(classes)), "close"
            p = make_plan(cli, rom, sha, proposal, sw / "plan", closure)
            rec.update({k: p[k] for k in ("accepted", "plan_cpu", "plan_wall", "plan_rss_kib", "stats", "reason")})
            if p["accepted"]:
                plan = p["plan"]
                extra = ["--immutable-rom-aot-admission", str(plan)]
                k = admitted_from_plan(plan.read_text(), universe)
                rec["k"] = len(k)
                rec["c_outside_k"] = sum(1 for s in starts if s in uset and s not in k)
            else:
                out["strategies"][name] = rec
                print(f"{name}: rejected {rec['reason']}", file=sys.stderr, flush=True)
                continue
        b = build_strategy(cli, rom, sha, extra, sw, cc, opt, jobs, cache)
        rec.update(b)
        if name == "INC":
            adm = b["admission"]
            rec["accepted"] = adm.get("fallback") == "0"  # the deployed route's own sanitized report line (ADR 0103)
            rec["reason"] = adm.get("reason", "")
            if "k" in adm:
                rec["k"] = int(adm["k"])
        rec["total_cpu"] = rec["plan_cpu"] + b["emit_cpu"] + b["compile_cpu"] + b["link_cpu"]
        rec["total_wall"] = rec.get("plan_wall", 0.0) + b["emit_wall"] + b["compile_wall"] + b["link_wall"]
        if runtime:
            rt_extra = [] if name == "BROAD" else (["--ml-gen2-admission"] if name == "INC" else ["--admission-plan", str(plan)])
            rt = runtime_check(cli, rom, rt_extra, sw / "run", frames, cc, timeout)
            if name == "BROAD":
                reference = rt
            elif reference is not None and reference.get("ok") and rt.get("ok"):
                rec["runtime_equal"] = all(rt.get(k) == reference.get(k) for k in RUNTIME_KEYS)
                if rt.get("_pcs") is not None and reference.get("_pcs") is not None:
                    rec["executed_pcs_outside_broad"] = len(set(rt["_pcs"]) - set(reference["_pcs"]))
                    rec["broad_pcs_not_executed"] = len(set(reference["_pcs"]) - set(rt["_pcs"]))
            elif reference is not None and reference.get("ok"):
                rec["runtime_equal"] = False
            rec["runtime"] = {k: v for k, v in rt.items() if not k.startswith("_")}
        out["strategies"][name] = rec
        print(f"{name}: cpu={rec['total_cpu']:.1f} c={rec['generated_bytes']} eq={rec.get('runtime_equal')}", file=sys.stderr, flush=True)
    return out


# ===================================================================================================================== synthetic fixture
def fixture_image(drop_dynamic_target_label: bool = False) -> tuple[bytes, bytes]:
    """Project-authored synthetic Genesis image with an exact per-byte instruction map (I start, c continuation, D data).

    reset $200: LEA $300.L,A0; MOVEQ #0,D0; loop: JSR $280.L; ADDQ.W #1,D0; CMPI.W #3,D0; BNE.S loop; MOVEA.L (A0),A1; JMP (A1)
    $240: NOP; BRA.S *   (reached ONLY through the dynamic JMP via the data table at $300 - no static edge)
    $280: ADDQ.W #1,D1; RTS
    $300: jump table (long $00000240) followed by code-like data words (D).
    With drop_dynamic_target_label the $240 NOP is labelled data, so an exact-start admission omits a dynamically reached instruction."""
    import genesis_z80_fixture_rom as fx
    size = 0x2000
    instructions = [(0x200, [0x41F9, 0x0000, 0x0300]), (0x206, [0x7000]), (0x208, [0x4EB9, 0x0000, 0x0280]), (0x20E, [0x5240]),
                    (0x210, [0x0C40, 0x0003]), (0x214, [0x66F2]), (0x216, [0x2250]), (0x218, [0x4ED1]),
                    (0x240, [0x4E71]), (0x242, [0x60FE]), (0x280, [0x5241]), (0x282, [0x4E75])]
    data = [(0x300, [0x0000, 0x0240, 0x4E75, 0x4E71, 0x6000, 0x0010, 0x4EB9, 0x0000, 0x0280, 0x60FE])]
    body = bytearray([0xFF]) * (0x400 - 0x200)
    for at, words in instructions + data:
        for i, w in enumerate(words):
            body[at - 0x200 + 2 * i:at - 0x200 + 2 * i + 2] = struct.pack(">H", w)
    rom = bytearray(fx.build_rom(bytes(body), size))
    rom[0:4] = struct.pack(">I", 0x00FFFE00)  # a valid supervisor stack inside work RAM (the subroutine call pushes)
    classes = bytearray(b"D") * size
    for at, words in instructions:
        if drop_dynamic_target_label and at == 0x240:
            continue
        classes[at] = ord("I")
        for j in range(at + 1, at + 2 * len(words)):
            classes[j] = ord("c")
    return bytes(rom), bytes(classes)


def cmd_fixture(a) -> int:
    work = pathlib.Path(a.work).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    results = {}
    for variant, drop in (("exact", False), ("dropped_dynamic_target", True)):
        rom, classes = fixture_image(drop)
        rp = work / f"{variant}.bin"
        rp.write_bytes(rom)
        sha = hashlib.sha256(rom).hexdigest()
        res = measure_image(a.cli, rp, sha, classes, [], None, work / variant, a.cc, a.opt, a.jobs, a.frames, a.timeout,
                            strategies=("BROAD", "ORACLE_REGION", "ORACLE_INSTR"), cache=not a.no_cache)
        results[variant] = res
    print(json.dumps(public(results), indent=1, sort_keys=True))
    return 0


# ========================================================================================================================= corpus
def public(obj):
    if isinstance(obj, dict):
        return {k: public(v) for k, v in obj.items() if not str(k).startswith("_") and not isinstance(v, pathlib.Path)}
    if isinstance(obj, list):
        return [public(v) for v in obj]
    if isinstance(obj, float):
        return round(obj, 4)
    return obj


def _bundle(a):
    import segarecomp_ml_v3_diagnose as dg
    return dg, dg.Bundle(a.bundle, dg.parse_rebase(getattr(a, "rebase", None)))


def cmd_measure(a) -> int:
    dg, bundle = _bundle(a)
    work = pathlib.Path(a.work).resolve()
    dg.require_private_dir(work)
    reg = dg.registry(bundle)
    m3r = json.loads(bundle.file("seg072/results/table.M3_R.json").read_text())
    m3r_regions = {r["id"]: r["policies"]["B50"]["_region"] for r in m3r["records"] if "B50" in r.get("policies", {})}
    for iid in a.images:
        m = reg[iid]
        out = work / iid / a.result_name
        if out.exists() and not a.force:
            print(f"{iid}: cached", file=sys.stderr)
            continue
        if m.get("broad_aot") != "accepted":
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(json.dumps({"lineage": m["lineage"], "status": "broad_aot_" + str(m.get("broad_aot"))}))
            continue
        rom = bundle.resolve_image(m)
        d = pathlib.Path(m["dir"])
        classes = bundle.file(str((d / "classes.bin").relative_to(bundle.payload))).read_bytes()
        seeds = json.loads(bundle.file(str((d / "seeds.json").relative_to(bundle.payload))).read_text())
        print(f"== {iid}", file=sys.stderr, flush=True)
        res = measure_image(a.cli, rom, m["sha256"], classes, seeds, m3r_regions.get(iid), work / iid / ("w" + a.result_name), a.cc, a.opt, a.jobs, a.frames,
                            a.timeout, strategies=tuple(a.strategies), cache=not a.no_cache)
        res.update({"lineage": m["lineage"], "status": "measured", "rom_sha256": m["sha256"]})
        shutil.rmtree(work / iid / ("w" + a.result_name), ignore_errors=True)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(public(res), indent=1, sort_keys=True))
    return 0


def cmd_diag_repr(a) -> int:
    """Non-gating representation diagnostic (ADR 0157): re-plan and re-emit one strategy's build (ORACLE_REGION by default), decompose rho, and
    compile each `block` TU separately (same flags, one process at a time) for the distribution of the precise-discovery block compile cost."""
    dg, bundle = _bundle(a)
    work = pathlib.Path(a.work).resolve()
    dg.require_private_dir(work)
    reg = dg.registry(bundle)
    runtime_dir = ROOT / "platforms" / "genesis" / "runtime"
    out = {}
    for iid in a.images:
        m = reg[iid]
        rom = bundle.resolve_image(m)
        d = pathlib.Path(m["dir"])
        classes = bundle.file(str((d / "classes.bin").relative_to(bundle.payload))).read_bytes()
        seeds = json.loads(bundle.file(str((d / "seeds.json").relative_to(bundle.payload))).read_text())
        w = work / iid / "repr"
        shutil.rmtree(w, ignore_errors=True)
        p = make_plan(a.cli, rom, m["sha256"], regions_text(m["sha256"], cell_ranges(oracle_region_cells(classes, seeds), rom.stat().st_size)), w / "plan", "prune")
        if not p["accepted"]:
            out[iid] = {"status": "rejected"}
            continue
        e = emit(a.cli, rom, m["sha256"], ["--immutable-rom-aot-admission", str(p["plan"])], w / "emit")
        rec = {"lineage": m["lineage"], "strategy": "ORACLE_REGION", **rho_decomposition(e["dir"])}
        blocks = [u for u in e["units"] if tu_family(u) == "block"]
        cpus = []
        for u in blocks:
            r = run_timed([a.cc] + BASE_FLAGS + OPT_FLAGS[a.opt] + ["-I", str(runtime_dir), "-I", str(e["dir"]), "-c", "-o", str(w / "b.o"), str(u)])
            if r["rc"] != 0:
                raise RuntimeError("block compile failed")
            text = pathlib.Path(u).read_text(errors="replace")
            fn_lines = [len(f.splitlines()) for f in re.split(r"\n(?=(?:static )?GenesisControlTransfer genesis_block_)", text)[1:]]
            cpus.append((r["cpu"], max(fn_lines) if fn_lines else 0, len(fn_lines)))
        cpus.sort(reverse=True)
        tot = sum(c for c, _, _ in cpus)
        rec["block"] = {"tus": len(cpus), "cpu": tot, "top1_tu_share": cpus[0][0] / tot if tot else 0.0,
                        "top5_tu_share": sum(c for c, _, _ in cpus[:5]) / tot if tot else 0.0,
                        "largest_function_lines": max((l for _, l, _ in cpus), default=0), "functions": sum(n for _, _, n in cpus)}
        out[iid] = rec
        shutil.rmtree(w, ignore_errors=True)
        print(iid, json.dumps(public(rec)), file=sys.stderr, flush=True)
    pathlib.Path(a.out).write_text(json.dumps(public(out), indent=1, sort_keys=True))
    return 0


def cmd_density(a) -> int:
    """Count-only density over every qualified development registry image (no compilation)."""
    dg, bundle = _bundle(a)
    reg = dg.registry(bundle)
    rows = {}
    for iid, m in sorted(reg.items()):
        d = pathlib.Path(m["dir"])
        classes = bundle.file(str((d / "classes.bin").relative_to(bundle.payload))).read_bytes()
        tokens = bundle.file(str((d / "tokens.bin").relative_to(bundle.payload))).read_bytes()
        seeds = json.loads(bundle.file(str((d / "seeds.json").relative_to(bundle.payload))).read_text())
        size = len(classes)
        n = (size + CELL - 1) // CELL
        labels = dg.cell_labels(classes)
        decodable = sum(1 for w in range(len(tokens) // 8) if tokens[w * 8] == 2)
        region = oracle_region_cells(classes, seeds)
        instr = oracle_instr_ranges(classes)
        rows[iid] = {"lineage": m["lineage"], "kind": m["kind"], "broad_aot": m.get("broad_aot"), "bytes": size, "cells": n,
                     "labelled_starts": classes.count(b"I"), "instruction_bytes": classes.count(b"I") + classes.count(b"c"),
                     "unknown_bytes": classes.count(b"U"), "data_bytes": classes.count(b"D"),
                     "ordinary_decodable_positions": decodable, "positive_cells": labels.count(1), "masked_cells": labels.count(-1),
                     "negative_cells": labels.count(0), "seed_cells": len(set(seeds)),
                     "oracle_region_cells": len(region), "oracle_region_fraction": len(region) / n,
                     "oracle_instr_positions": sum((e - b) // 2 for b, e in instr),
                     "oracle_instr_fraction_of_decodable": (sum((e - b) // 2 for b, e in instr) / decodable) if decodable else None,
                     "decodable_positions_in_oracle_region": sum(1 for c in region for w in range(c * CELL // 2, min((c + 1) * CELL, size) // 2)
                                                                 if w * 8 < len(tokens) and tokens[w * 8] == 2)}
    out = pathlib.Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"schema": SCHEMA + ".density", "images": rows}, indent=1, sort_keys=True))
    return 0


def cmd_aggregate(a) -> int:
    work = pathlib.Path(a.work).resolve()
    per_image = {}
    for iid in GATE_IMAGES:
        p = work / iid / "result.json"
        if p.exists():
            per_image[iid] = json.loads(p.read_text())
    measured = {k: v for k, v in per_image.items() if v.get("status") == "measured"}
    gate = evaluate_gate(measured)
    record = {"schema": SCHEMA, "adr": "0156 (pre-registration) / 0157 (results)", "gate": gate,
              "thresholds": {"g": G1_G, "g_c": G1_GC, "h": G1_H, "per_lineage": G1_PER_LINEAGE, "min_lineages": G1_MIN_LINEAGES,
                             "min_accepted_lineages": G1_MIN_ACCEPTED, "rho": G2_RHO},
              "images": {}}
    for iid, rec in sorted(per_image.items()):
        row = {"lineage": rec.get("lineage"), "status": rec.get("status")}
        if rec.get("status") == "measured":
            row.update({k: rec[k] for k in ("rom_bytes", "universe", "labelled_starts", "labelled_starts_in_universe")})
            row["strategies"] = {}
            broad = rec["strategies"].get("BROAD")
            for name, s in rec["strategies"].items():
                keep = {k: s.get(k) for k in ("accepted", "reason", "stats", "k", "c_outside_k", "plan_cpu", "emit_cpu", "compile_cpu",
                                              "compile_wall", "link_cpu", "total_cpu", "total_wall", "generated_bytes", "object_bytes",
                                              "executable_bytes", "peak_compiler_rss_kib", "tu_count", "runtime_equal",
                                              "executed_pcs_outside_broad", "broad_pcs_not_executed") if k in s}
                if "counts" in s:
                    keep["counts"] = {k: s["counts"][k] for k in ("entry_segments", "aot_owner_functions", "aot_shared_helpers", "dispatch_cases",
                                                                  "compiled_entry_rows", "block_functions", "rho")}
                    keep["aot_identities"] = int(s.get("enumeration", {}).get("accepted_count", 0) or 0)
                if "cache" in s:
                    keep["cache"] = s["cache"]
                if "family_cpu" in s:
                    keep["family_cpu"] = s["family_cpu"]
                if "runtime" in s:
                    keep["runtime"] = {k: s["runtime"].get(k) for k in ("ok", "outcome", "frames")}
                if broad and broad.get("accepted") and (s.get("accepted") or name != "BROAD"):
                    dep = deployed(s, broad)
                    keep["deployed"] = {"fallback": dep["fallback"], "cpu_vs_broad": dep["cpu"] / broad["total_cpu"],
                                        "c_vs_broad": dep["generated_bytes"] / broad["generated_bytes"]}
                row["strategies"][name] = keep
        record["images"][iid] = row
    # Non-gating diagnostics (ADR 0157; not pre-registered): labelled-only instruction admission (unknown bytes excluded), the representation
    # decomposition, and count-only corpus density (Sonic CD modules aggregated into one row).
    diag: dict = {"note": "non-gating attribution diagnostics; never part of the ADR 0156 gate"}
    labelled = {}
    for iid in GATE_IMAGES:
        p = work / iid / "diag.json"
        if p.exists():
            d = json.loads(p.read_text())
            s, bro = d["strategies"].get("DIAG_INSTR_LABELLED", {}), d["strategies"].get("BROAD", {})
            if s.get("accepted"):
                labelled[iid] = {"k": s.get("k"), "stats": s.get("stats"), "cpu_vs_broad": s["total_cpu"] / bro["total_cpu"],
                                 "c_vs_broad": s["generated_bytes"] / bro["generated_bytes"], "runtime_equal": s.get("runtime_equal"),
                                 "runtime": {k: s.get("runtime", {}).get(k) for k in ("outcome", "frames")},
                                 "broad_runtime": {k: bro.get("runtime", {}).get(k) for k in ("outcome", "frames")}}
    if labelled:
        diag["instr_labelled_only"] = labelled
    if a.diag_repr and pathlib.Path(a.diag_repr).exists():
        diag["representation"] = json.loads(pathlib.Path(a.diag_repr).read_text())
    if a.density and pathlib.Path(a.density).exists():
        rows = json.loads(pathlib.Path(a.density).read_text())["images"]
        dens = {k: v for k, v in rows.items() if v["kind"] == "genesis_rom"}
        scd = [v for v in rows.values() if v["kind"] != "genesis_rom"]
        if scd:
            keys = ("bytes", "labelled_starts", "instruction_bytes", "unknown_bytes", "data_bytes", "ordinary_decodable_positions", "positive_cells",
                    "masked_cells", "negative_cells", "seed_cells", "oracle_region_cells", "oracle_instr_positions", "decodable_positions_in_oracle_region", "cells")
            agg = {k: sum(v[k] for v in scd) for k in keys}
            agg.update({"modules": len(scd), "lineage": "D01", "kind": "scd_module (aggregate)",
                        "oracle_instr_fraction_of_decodable": agg["oracle_instr_positions"] / agg["ordinary_decodable_positions"],
                        "oracle_region_fraction": agg["oracle_region_cells"] / agg["cells"]})
            dens["scd_modules"] = agg
        diag["density"] = dens
    record["diagnostics"] = diag
    out = pathlib.Path(a.out)
    out.write_text(json.dumps(public(record), indent=1, sort_keys=True) + "\n")
    print(json.dumps(public(gate), indent=1, sort_keys=True))
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("--cli", required=True)
        sp.add_argument("--cc", default="clang")
        sp.add_argument("--opt", choices=sorted(OPT_FLAGS), default="O2")
        sp.add_argument("--jobs", type=int, default=4)
        sp.add_argument("--frames", type=int, default=120)
        sp.add_argument("--timeout", type=int, default=3600)
        sp.add_argument("--work", required=True)
        sp.add_argument("--no-cache", action="store_true")
        sp.add_argument("--emit-flag", action="append", default=[], choices=["--shared-return-targets", "--aot-operand-sharing"],
                        help="SEG-074-T002: opt-in emitter switch applied to every emission (repeatable); the result name should record the variant")
    f = sub.add_parser("fixture")
    common(f)
    f.set_defaults(fn=cmd_fixture)
    m = sub.add_parser("measure")
    common(m)
    m.add_argument("--bundle", required=True)
    m.add_argument("--rebase", action="append")
    m.add_argument("--images", nargs="+", default=list(GATE_IMAGES))
    m.add_argument("--strategies", nargs="+", default=list(STRATEGIES), choices=STRATEGIES + DIAGNOSTICS)
    m.add_argument("--result-name", default="result.json", help="per-image result file (diagnostic runs use a separate name)")
    m.add_argument("--force", action="store_true")
    m.set_defaults(fn=cmd_measure)
    r = sub.add_parser("diag-repr")
    common(r)
    r.add_argument("--bundle", required=True)
    r.add_argument("--rebase", action="append")
    r.add_argument("--images", nargs="+", default=list(GATE_IMAGES))
    r.add_argument("--out", required=True)
    r.set_defaults(fn=cmd_diag_repr)
    d = sub.add_parser("density")
    d.add_argument("--bundle", required=True)
    d.add_argument("--rebase", action="append")
    d.add_argument("--out", required=True)
    d.set_defaults(fn=cmd_density)
    g = sub.add_parser("aggregate")
    g.add_argument("--work", required=True)
    g.add_argument("--out", required=True)
    g.add_argument("--diag-repr", help="diag-repr output (non-gating)")
    g.add_argument("--density", help="density output (non-gating)")
    g.set_defaults(fn=cmd_aggregate)
    a = p.parse_args(argv)
    EMIT_FLAGS[:] = list(getattr(a, "emit_flag", []) or [])
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
