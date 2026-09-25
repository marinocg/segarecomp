#!/usr/bin/env python3
"""SEG-022-T001: deterministic generated-code scalability measurement.

Measures the size/time cost of the generated-native startup-bridge build and attributes every
byte of the generated C source to a (region, line-class) cell. Pure instrumentation: it invokes the
existing `emit-general-startup-bridge-c` command unchanged and post-processes its output, so
generated guest semantics and the admitted/compiled address sets cannot change.

Only aggregate counts/bytes/times are reported (no addresses, opcodes, or generated text), which
is the non-reconstructable form permitted as durable evidence. Raw generated C stays in the
caller-supplied ephemeral --out-dir.

Subcommands:
  attribute FILE.c            attribute an existing generated source (streaming, exact byte sum)
  measure --rom ... [--external-hints H] --out-dir D
                              emit + attribute + compile + link for one route; omitting
                              --external-hints selects the no-external-hints route
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import time

SCHEMA = 1

# Region: which top-level construct owns the line (first matching col-0 signature opens a region).
_REGION_STARTS = (
    ("provenance_fn", re.compile(rb"^void genesis_attach_route_provenance\(")),
    # SEG-022-T010: SEG-022-T003/T008 emit non-static functions across translation units and group AOT
    # entries in `genesis_aot_owner_*` functions; the optional `static ` keeps the old shapes matching.
    ("route_record_table", re.compile(rb"^static const GenesisRouteRecord genesis_route_records\[\]")),
    ("route_mapping_table", re.compile(rb"^static const GenesisRouteMapping genesis_route_mappings\[\]")),
    ("tier1_stop_fn", re.compile(rb"^(?:static )?GenesisControlTransfer genesis_tier1_indirect_stop_[0-9A-Fa-f]+\([^;]*\{\s*$")),
    ("frontier_stop_fn", re.compile(rb"^(?:static )?GenesisControlTransfer genesis_frontier_stop_[0-9A-Fa-f]+\([^;]*\{\s*$")),
    ("static_stop_fn", re.compile(rb"^GenesisControlTransfer genesis_static_stop\(")),
    ("ordinary_block", re.compile(rb"^(?:static )?GenesisControlTransfer genesis_block_[0-9A-Fa-f]+\([^;]*\{\s*$")),
    ("aot_function", re.compile(rb"^(?:static )?GenesisControlTransfer genesis_aot_(?:owner_)?[0-9A-Fa-f]+\([^;]*\{\s*$")),
    ("entry_table", re.compile(rb"^static const uint32_t genesis_compiled_entry_addresses\[\]")),
    ("dispatch", re.compile(rb"^(?:static )?GenesisCompiledEntry genesis_compiled_entry_lookup\([^;]*\{")),
    ("owned_literals", re.compile(rb"^static const uint8_t genesis_owned_region_data_")),
    ("main_glue", re.compile(rb"^int main\(")),
)
_FORWARD_DECL = re.compile(rb"^static [A-Za-z_ *]+\([^{]*\);\s*$")

_MAPPING = re.compile(rb"^\s*(?:frontier\.stop\.provenance\.mapping_|genesis_set_mapping_claim\()")
_ENTRY_DISPATCH = re.compile(rb"^\s*(?:switch \(runtime->pc\) \{|case UINT32_C\(0x[0-9A-Fa-f]+\): goto genesis_(?:aot_entry|instruction)_|"
                             rb"default: return genesis_internal_dispatch_inconsistency_stop)")
_ENTRY_LABEL = re.compile(rb"^genesis_(?:aot_entry|instruction)_[0-9A-Fa-f]+:")
_PROVENANCE = re.compile(
    rb"^\s*(?:GenesisInstructionProvenance source\b|source\.|genesis_set_fetch_access\(|frontier\.stop\.provenance\.|"
    rb"frontier\.stop\.(?:stop_class|diagnostic_category)|stop\.provenance)")
_FRONTIER = re.compile(rb"^\s*(?:GenesisControlTransfer frontier\b|frontier\.|return frontier|return genesis_runtime_retire_m68k_instruction_before_stop\()")
_MEMBERSHIP = re.compile(rb"^\s*(?:if \(runtime->pc == UINT32_C\(|\|\| runtime->pc == )")
_RETIRE = re.compile(rb"^\s*(?:\{ const uint32_t m68k_retirement_pc\b|if \(retired\.|return retired;|"
                     rb"runtime->pc = pc;|uint32_t pc = runtime->pc;|#define pc |#undef pc)")
_INDIRECT_ARRAY_START = re.compile(rb"static const uint32_t m68k_indirect_targets_\w+\[\]")
# SEG-022-T009: the compact table is three parallel arrays: sorted guest addresses, owner ids, owner symbols.
_ENTRY_ADDR_ROW = re.compile(rb"^\s*UINT32_C\(0x([0-9A-Fa-f]+)\),")
_ENTRY_ID_ROW = re.compile(rb"^\s*UINT(?:8|16|32)_C\(([0-9]+)\),")
_ENTRY_OWNER_ROW = re.compile(rb"^\s*(genesis_(aot|block)\w*),")
_ENTRY_ROW = re.compile(rb"^\s*(?:UINT32_C\(0x[0-9A-Fa-f]+\)|UINT(?:8|16|32)_C\([0-9]+\)|genesis_(?:aot|block)\w*),")

_FUNC_RE = {
    "aot_function": re.compile(rb"^static GenesisControlTransfer genesis_aot_"),
    "ordinary_block": re.compile(rb"^static GenesisControlTransfer genesis_block_"),
    "frontier_stop_fn": re.compile(rb"^static GenesisControlTransfer genesis_frontier_stop_"),
    "tier1_stop_fn": re.compile(rb"^static GenesisControlTransfer genesis_tier1_indirect_stop_"),
}


def split_entry_addresses(addresses, ids, owner_kinds) -> dict:
    """Resolve the compact table (address -> owner id -> owner kind) into per-kind address lists."""
    assert len(addresses) == len(ids), "entry table arrays disagree"
    result = {"aot": [], "block": []}
    for address, owner in zip(addresses, ids):
        result[owner_kinds[owner]].append(address)
    return result


def classify_line(region: str, line: bytes) -> str:
    """Return the line class inside a region."""
    if region == "entry_table":
        return "compiled_entry_table" if _ENTRY_ROW.match(line) else "table_scaffold"
    if region == "owned_literals":
        return "owned_rom_literal"
    if region in ("dispatch", "main_glue", "prelude", "static_stop_fn", "header"):
        return "glue"
    if region == "route_record_table":
        return "provenance"
    if region == "route_mapping_table":
        return "mapping_metadata"
    if region == "provenance_fn":
        return "mapping_metadata" if b"mapping" in line else "provenance"
    if _ENTRY_DISPATCH.match(line):
        return "entry_dispatch"
    if _ENTRY_LABEL.match(line):
        return "boilerplate"
    if _MAPPING.match(line):
        return "mapping_metadata"
    if _PROVENANCE.match(line):
        return "provenance"
    if _FRONTIER.match(line):
        return "frontier_code"
    if _MEMBERSHIP.match(line):
        return "target_membership"
    if _RETIRE.match(line):
        return "boilerplate"
    if region in ("frontier_stop_fn", "tier1_stop_fn"):
        return "frontier_code"
    if line[:1] not in b" \t" and (line.startswith(b"static ") or line.startswith(b"}")):
        return "boilerplate"
    return "body"


def set_fingerprint(addresses) -> dict:
    """Count + SHA-256 over the sorted, canonical `%08x\\n` serialization (no addresses retained)."""
    ordered = sorted(set(addresses))
    digest = hashlib.sha256("".join(f"{a:08x}\n" for a in ordered).encode("ascii")).hexdigest()
    return {"count": len(ordered), "sha256": digest}


def attribute(paths) -> dict:
    """Attribute every byte of one file or an ordered list of files (translation units + header)."""
    if isinstance(paths, (str, os.PathLike)):
        paths = [paths]
    cells: dict[str, list[int]] = {}
    counts = {"aot_function": 0, "ordinary_block": 0, "frontier_stop_fn": 0, "tier1_stop_fn": 0,
              "compiled_entry_rows": 0, "forward_declarations": 0, "aot_entry_label": 0}
    total_bytes = total_lines = 0
    region = "prelude"
    fn_sizes: dict[str, list[int]] = {"aot_function": [], "ordinary_block": []}
    fn_array_bytes: dict[str, list[int]] = {"aot_function": [], "ordinary_block": []}
    in_array = False
    array_count = array_elements = array_bytes = array_max = cur_elements = 0
    entry_addresses = {"aot": [], "block": []}
    table_addresses: list[int] = []
    table_ids: list[int] = []
    table_owner_kinds: list[str] = []
    for path in paths:
        region = "header" if pathlib.Path(path).suffix == ".h" else "prelude"
        in_array = False
        handle = pathlib.Path(path).open("rb")
        for line in handle:
            total_bytes += len(line)
            total_lines += 1
            if line[:1] not in b" \t}\n{":
                for name, pattern in _REGION_STARTS:
                    if pattern.match(line):
                        region = name
                        if name in fn_sizes:
                            fn_sizes[name].append(0)
                            fn_array_bytes[name].append(0)
                        if name in counts:
                            counts[name] += 1
                        break
                else:
                    if region in ("aot_function", "ordinary_block") and _FORWARD_DECL.match(line):
                        counts["forward_declarations"] += 1
            if region in fn_sizes:
                fn_sizes[region][-1] += len(line)
            cls = classify_line(region, line)
            if region == "entry_table":
                if (em := _ENTRY_ADDR_ROW.match(line)):
                    table_addresses.append(int(em.group(1), 16))
                elif (em := _ENTRY_ID_ROW.match(line)):
                    table_ids.append(int(em.group(1)))
                elif (em := _ENTRY_OWNER_ROW.match(line)):
                    table_owner_kinds.append(em.group(2).decode())
            # Site-local indirect-target membership arrays (possibly multi-line) are target
            # membership data wherever they are emitted, not instruction-lowering body.
            if not in_array and region not in ("entry_table", "owned_literals") \
                    and _INDIRECT_ARRAY_START.search(line):
                in_array = True
                array_count += 1
                cur_elements = 0
            if in_array:
                cls = "target_membership"
                n = line.count(b"UINT32_C(")
                cur_elements += n
                array_elements += n
                array_bytes += len(line)
                if region in fn_array_bytes:
                    fn_array_bytes[region][-1] += len(line)
                if b"};" in line:
                    in_array = False
                    array_max = max(array_max, cur_elements)
            if region == "entry_table" and _ENTRY_ADDR_ROW.match(line):
                counts["compiled_entry_rows"] += 1
            if region == "aot_function" and _ENTRY_LABEL.match(line):
                counts["aot_entry_label"] += 1
            cell = cells.setdefault(f"{region}.{cls}", [0, 0])
            cell[0] += len(line)
            cell[1] += 1
        handle.close()
    entry_addresses = split_entry_addresses(table_addresses, table_ids, table_owner_kinds)
    categories = {
        "ordinary_block_bodies": ("ordinary_block.body",),
        "ordinary_block_boilerplate": ("ordinary_block.boilerplate",),
        "immutable_rom_aot_bodies": ("aot_function.body",),
        "aot_boilerplate": ("aot_function.boilerplate",),
        # SEG-022-T010: per-owner `switch (runtime->pc)` entry dispatch introduced by grouped AOT owners.
        "owner_entry_dispatch": ("aot_function.entry_dispatch", "ordinary_block.entry_dispatch"),
        "dispatch_structures": ("dispatch.glue", "entry_table.table_scaffold", "main_glue.glue"),
        "compiled_entry_tables": ("entry_table.compiled_entry_table",),
        "target_membership_structures": ("aot_function.target_membership", "ordinary_block.target_membership",
                                         "tier1_stop_fn.target_membership", "frontier_stop_fn.target_membership"),
        "provenance": ("provenance_fn.provenance", "route_record_table.provenance", "aot_function.provenance", "ordinary_block.provenance",
                       "frontier_stop_fn.provenance", "tier1_stop_fn.provenance"),
        "mapping_metadata": ("provenance_fn.mapping_metadata", "route_mapping_table.mapping_metadata", "aot_function.mapping_metadata",
                             "ordinary_block.mapping_metadata", "frontier_stop_fn.mapping_metadata",
                             "tier1_stop_fn.mapping_metadata"),
        "frontier_code": ("aot_function.frontier_code", "ordinary_block.frontier_code",
                          "frontier_stop_fn.body", "frontier_stop_fn.frontier_code",
                          "frontier_stop_fn.boilerplate", "tier1_stop_fn.body", "tier1_stop_fn.frontier_code",
                          "tier1_stop_fn.boilerplate"),
        "owned_resolved_rom_literals": ("owned_literals.owned_rom_literal",),
        "runtime_prelude_glue": ("prelude.glue", "static_stop_fn.glue", "header.glue"),
    }
    assigned: set[str] = set()
    category_bytes: dict[str, int] = {}
    for name, members in categories.items():
        category_bytes[name] = sum(cells.get(m, [0, 0])[0] for m in members)
        assigned.update(members)
    residual_cells = {k: v[0] for k, v in cells.items() if k not in assigned}
    category_bytes["unattributed_residual"] = sum(residual_cells.values())
    def distribution(sizes: list[int], arrays: list[int]) -> dict:
        if not sizes:
            return {"count": 0}
        ordered = sorted(sizes)
        pick = lambda q: ordered[min(len(ordered) - 1, int(q * len(ordered)))]
        return {"count": len(ordered), "total_bytes": sum(ordered), "mean_bytes": sum(ordered) // len(ordered),
                "p50_bytes": pick(0.5), "p90_bytes": pick(0.9), "p99_bytes": pick(0.99), "max_bytes": ordered[-1],
                "functions_over_100kib": sum(1 for x in ordered if x > 102400),
                "bytes_in_functions_over_100kib": sum(x for x in ordered if x > 102400),
                "indirect_target_array_bytes_in_functions_over_100kib":
                    sum(a for x, a in zip(sizes, arrays) if x > 102400),
                "indirect_target_array_bytes_in_all_functions": sum(arrays)}

    return {
        "function_size_distribution": {k: distribution(v, fn_array_bytes[k]) for k, v in fn_sizes.items()},
        "total_bytes": total_bytes,
        "total_lines": total_lines,
        "category_bytes": category_bytes,
        "residual_cells_bytes": residual_cells,
        "category_sum_bytes": sum(category_bytes.values()),
        "cells": {k: {"bytes": v[0], "lines": v[1]} for k, v in sorted(cells.items())},
        "counts": counts,
        "indirect_target_arrays": {"array_count": array_count, "element_count": array_elements,
                                   "bytes": array_bytes, "max_elements_in_one_array": array_max},
        "compiled_entry_table": {"rows": len(entry_addresses["aot"]) + len(entry_addresses["block"]),
                                 "rows_owned_by_ordinary_blocks": len(entry_addresses["block"]),
                                 "rows_owned_by_immutable_rom_aot": len(entry_addresses["aot"])},
        "fingerprints": {
            "final_compiled_entry_address_set": set_fingerprint(entry_addresses["aot"] + entry_addresses["block"]),
            "aot_owned_entry_address_set": set_fingerprint(entry_addresses["aot"]),
            "ordinary_block_entry_address_set": set_fingerprint(entry_addresses["block"]),
        },
    }


_NUM = re.compile(r"(\w+)=(\d+)")


def parse_emitter_metrics(stderr: str) -> dict:
    marks = {
        "segarecomp: offline inventory stitch: ": "offline_stitch",
        "segarecomp: immutable-rom AOT enumeration: ": "immutable_rom_aot",
        "segarecomp: offline inventory emission: ": "emission",
        "segarecomp: offline inventory partition: ": "partition",
    }
    out: dict = {}
    for line in stderr.splitlines():
        for prefix, key in marks.items():
            if line.startswith(prefix):
                out[key] = {k: int(v) for k, v in _NUM.findall(line[len(prefix):])}
    out["external_hint_lines"] = sum(1 for l in stderr.splitlines() if l.startswith("segarecomp: external hint opted in"))
    return out


def timed(command: list[str], stdout=None, cwd=None) -> dict:
    """Run under the platform `time` for wall/user/sys/peak-RSS; return stats and stderr text."""
    if sys.platform == "darwin":
        wrapped = ["/usr/bin/time", "-l"] + command
    else:
        wrapped = ["/usr/bin/time", "-v"] + command
    start = time.monotonic()
    done = subprocess.run(wrapped, stdout=stdout or subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, cwd=cwd)
    wall = time.monotonic() - start
    rss = None
    text = done.stderr
    m = re.search(r"^\s*(\d+)\s+maximum resident set size", text, re.M)
    if m:
        rss = int(m.group(1))  # bytes on darwin
    m = re.search(r"Maximum resident set size \(kbytes\): (\d+)", text)
    if m:
        rss = int(m.group(1)) * 1024
    return {"returncode": done.returncode, "wall_seconds": round(wall, 2), "peak_rss_bytes": rss, "stderr": text}


def measure(args) -> dict:
    out = pathlib.Path(args.out_dir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    rom = pathlib.Path(args.rom).resolve()
    digest = hashlib.sha256(rom.read_bytes()).hexdigest()
    src = out / "generated.c"
    cmd = [args.segarecomp, "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry",
           "--rom-sha256", digest]
    if args.external_hints:
        cmd += ["--external-hints", args.external_hints]
    cmd += ["--immutable-rom-aot"]
    admitted_sink = out / "admitted_aot_addresses.txt"
    cmd += ["--immutable-aot-address-report", str(admitted_sink)]
    # SEG-022-T002: measure the canonical streaming path (never stdout redirection). No stale
    # artifact may be accepted: remove any prior output before the emitter runs.
    for stale in (src, pathlib.Path(str(src) + ".partial"), admitted_sink):
        stale.unlink(missing_ok=True)
    # SEG-022-T003: a large program is emitted as a bounded deterministic set of translation units in
    # `shard_dir` (a small one still yields the single `src`); both artifacts are cleared first.
    shard_dir = out / "generated"
    shutil.rmtree(shard_dir, ignore_errors=True)
    cmd += ["--generated-c-output", str(src), "--generated-c-shard-dir", str(shard_dir)]
    gen = timed(cmd)
    manifest = shard_dir / "bridge_generated.units"
    sharded = manifest.is_file()
    if gen["returncode"] == 0 and not sharded and (not src.is_file() or pathlib.Path(str(src) + ".partial").exists()):
        gen["returncode"] = 1  # success without a complete artifact is a failure
    report: dict = {
        "schema": SCHEMA,
        "route": "external_hints" if args.external_hints else "no_external_hints",
        "rom_sha256": digest,
        "generation": {k: gen[k] for k in ("returncode", "wall_seconds", "peak_rss_bytes")},
        "emitter_metrics": parse_emitter_metrics(gen["stderr"]),
    }
    if gen["returncode"] != 0:
        for leftover in (src, pathlib.Path(str(src) + ".partial"), admitted_sink):
            leftover.unlink(missing_ok=True)
        shutil.rmtree(shard_dir, ignore_errors=True)
        return report
    if sharded:
        units = [shard_dir / line for line in manifest.read_text().splitlines() if line]
        sizes = [u.stat().st_size for u in units]
        header = shard_dir / "bridge_generated.h"
        report["source"] = attribute(units + [header])
        report["source"]["translation_units"] = {"count": len(units), "total_bytes": sum(sizes),
                                                 "largest_bytes": max(sizes), "header_bytes": header.stat().st_size}
    else:
        report["source"] = attribute(src)
    addresses = [int(x, 16) for x in admitted_sink.read_text().split()]
    admitted_sink.unlink()  # ephemeral: only count + digest are retained
    report["fingerprints"] = {"admitted_immutable_rom_aot_address_set": set_fingerprint(addresses)}
    if sharded:
        # Final compiled-address authority = the sorted compiled-entry table (its own `entries` TU).
        t_addr: list[int] = []
        t_ids: list[int] = []
        t_kinds: list[str] = []
        with (shard_dir / "bridge_generated_entries_00.c").open("rb") as handle:
            for line in handle:
                if (match := _ENTRY_ADDR_ROW.match(line)):
                    t_addr.append(int(match.group(1), 16))
                elif (match := _ENTRY_ID_ROW.match(line)):
                    t_ids.append(int(match.group(1)))
                elif (match := _ENTRY_OWNER_ROW.match(line)):
                    t_kinds.append(match.group(2).decode())
        entry_addresses = split_entry_addresses(t_addr, t_ids, t_kinds)
        report["fingerprints"].update({
            "final_compiled_entry_address_set": set_fingerprint(entry_addresses["aot"] + entry_addresses["block"]),
            "aot_owned_entry_address_set": set_fingerprint(entry_addresses["aot"]),
            "ordinary_block_entry_address_set": set_fingerprint(entry_addresses["block"]),
        })
    runtime_dir = pathlib.Path(args.product_root) / "platforms" / "genesis" / "runtime"
    flags = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", args.opt, "-I", str(runtime_dir)]
    robj = out / "runtime.o"
    rcomp = timed(flags + ["-c", "-o", str(robj), str(runtime_dir / "runtime.c")])
    if sharded:
        # Per-TU compile (objects stay in the ephemeral out dir). `--jobs` > 1 only schedules the same
        # independent compiles concurrently (SEG-022-T004 owns the build-bridge scheduler); the sum of
        # per-TU wall times is reported next to the elapsed stage wall.
        import concurrent.futures
        objects = [out / f"generated_{i:02d}.o" for i in range(len(units))]
        stage_start = time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            results = list(pool.map(lambda pair: timed(flags + ["-I", str(shard_dir), "-c", "-o", str(pair[1]), str(pair[0])]),
                                    zip(units, objects)))
        stage_wall = round(time.monotonic() - stage_start, 2)
        comp = {"returncode": next((r["returncode"] for r in results if r["returncode"] != 0), 0),
                "wall_seconds": stage_wall, "peak_rss_bytes": max(r["peak_rss_bytes"] or 0 for r in results),
                "stderr": next((r["stderr"] for r in results if r["returncode"] != 0), "")}
        tu_stats = {"count": len(results), "jobs": max(1, args.jobs), "stage_wall_seconds": stage_wall,
                    "sum_of_tu_wall_seconds": round(sum(r["wall_seconds"] for r in results), 2),
                    "max_tu_wall_seconds": max(r["wall_seconds"] for r in results),
                    "max_tu_peak_rss_bytes": max(r["peak_rss_bytes"] or 0 for r in results)}
    else:
        obj = out / "generated.o"
        objects = [obj]
        comp = timed(flags + ["-c", "-o", str(obj), str(src)])
        tu_stats = None
    def failure_class(stats: dict) -> str | None:
        if stats["returncode"] == 0:
            return None
        if "ran out of source locations" in stats["stderr"]:
            return "compiler_source_location_limit"
        return "compile_error_other"

    report["compile"] = {
        "failure_class": failure_class(comp) or failure_class(rcomp),
        "opt": args.opt,
        "generated_tu": {k: comp[k] for k in ("returncode", "wall_seconds", "peak_rss_bytes")},
        "translation_units": tu_stats,
        "runtime_tu": {k: rcomp[k] for k in ("returncode", "wall_seconds", "peak_rss_bytes")},
    }
    if comp["returncode"] == 0 and rcomp["returncode"] == 0:
        exe = out / "bridge"
        link = timed([args.cc, "-o", str(exe)] + [str(o) for o in objects] + [str(robj)])
        report["link"] = {k: link[k] for k in ("returncode", "wall_seconds", "peak_rss_bytes")}
        report["artifact_bytes"] = {"generated_source": sum(u.stat().st_size for u in units) if sharded else src.stat().st_size,
                                    "generated_object": sum(o.stat().st_size for o in objects),
                                    "executable": exe.stat().st_size if exe.exists() else None}
    return report


def toolchain(cc: str) -> dict:
    ver = subprocess.run([cc, "--version"], capture_output=True, text=True).stdout.splitlines()
    return {"cc": ver[0] if ver else "unknown", "machine": platform.machine(), "system": platform.system(),
            "release": platform.release(), "cpu_count": os.cpu_count()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("attribute")
    a.add_argument("file")
    m = sub.add_parser("measure")
    m.add_argument("--segarecomp", required=True)
    m.add_argument("--product-root", default=str(pathlib.Path(__file__).resolve().parents[1]))
    m.add_argument("--cc", default="/usr/bin/cc")
    m.add_argument("--opt", default="-O0")
    m.add_argument("--rom", required=True)
    m.add_argument("--external-hints")
    m.add_argument("--out-dir", required=True)
    m.add_argument("--report")
    m.add_argument("--jobs", type=int, default=1, help="concurrent per-TU compiles (sharded output only)")
    args = parser.parse_args()
    if args.cmd == "attribute":
        result = attribute(pathlib.Path(args.file))
    else:
        result = measure(args)
        result["toolchain"] = toolchain(args.cc)
        if args.report:
            pathlib.Path(args.report).write_text(json.dumps(result, indent=1, sort_keys=True) + "\n")
    print(json.dumps(result, indent=1, sort_keys=True))
    if result.get("generation", {}).get("returncode", 0) != 0:
        return 1  # a failed/incomplete generation is a failed measurement
    return 0


if __name__ == "__main__":
    sys.exit(main())
