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
import subprocess
import sys
import time

SCHEMA = 1

# Region: which top-level construct owns the line (first matching col-0 signature opens a region).
_REGION_STARTS = (
    ("provenance_fn", re.compile(rb"^void genesis_attach_route_provenance\(")),
    ("tier1_stop_fn", re.compile(rb"^static GenesisControlTransfer genesis_tier1_indirect_stop_[0-9A-Fa-f]+\([^;]*\{\s*$")),
    ("frontier_stop_fn", re.compile(rb"^static GenesisControlTransfer genesis_frontier_stop_[0-9A-Fa-f]+\([^;]*\{\s*$")),
    ("static_stop_fn", re.compile(rb"^GenesisControlTransfer genesis_static_stop\(")),
    ("ordinary_block", re.compile(rb"^static GenesisControlTransfer genesis_block_[0-9A-Fa-f]+\([^;]*\{\s*$")),
    ("aot_function", re.compile(rb"^static GenesisControlTransfer genesis_aot_[0-9A-Fa-f]+\([^;]*\{\s*$")),
    ("entry_table", re.compile(rb"^static const GenesisCompiledEntryRecord genesis_compiled_entries\[\]")),
    ("dispatch", re.compile(rb"^static GenesisCompiledEntry genesis_compiled_entry_lookup\([^;]*\{")),
    ("owned_literals", re.compile(rb"^static const uint8_t genesis_owned_region_data_")),
    ("main_glue", re.compile(rb"^int main\(")),
)
_FORWARD_DECL = re.compile(rb"^static [A-Za-z_ *]+\([^{]*\);\s*$")

_MAPPING = re.compile(rb"^\s*frontier\.stop\.provenance\.mapping_")
_PROVENANCE = re.compile(
    rb"^\s*(?:GenesisInstructionProvenance source\b|source\.|frontier\.stop\.provenance\.|"
    rb"frontier\.stop\.(?:stop_class|diagnostic_category)|stop\.provenance)")
_FRONTIER = re.compile(rb"^\s*(?:GenesisControlTransfer frontier\b|frontier\.|return frontier)")
_MEMBERSHIP = re.compile(rb"^\s*(?:if \(runtime->pc == UINT32_C\(|\|\| runtime->pc == )")
_RETIRE = re.compile(rb"^\s*(?:\{ const uint32_t m68k_retirement_pc\b|if \(retired\.|return retired;|"
                     rb"runtime->pc = pc;|uint32_t pc = runtime->pc;|#define pc |#undef pc)")
_INDIRECT_ARRAY_START = re.compile(rb"static const uint32_t m68k_indirect_targets_\w+\[\]")
_ENTRY_ROW_ADDR = re.compile(rb"^\s*\{ UINT32_C\(0x([0-9A-Fa-f]+)\), genesis_(aot|block)_")
_ENTRY_ROW = re.compile(rb"^\s*\{ UINT32_C\(0x[0-9A-Fa-f]+\), genesis_(?:aot|block)_")

_FUNC_RE = {
    "aot_function": re.compile(rb"^static GenesisControlTransfer genesis_aot_"),
    "ordinary_block": re.compile(rb"^static GenesisControlTransfer genesis_block_"),
    "frontier_stop_fn": re.compile(rb"^static GenesisControlTransfer genesis_frontier_stop_"),
    "tier1_stop_fn": re.compile(rb"^static GenesisControlTransfer genesis_tier1_indirect_stop_"),
}


def classify_line(region: str, line: bytes) -> str:
    """Return the line class inside a region."""
    if region == "entry_table":
        return "compiled_entry_table" if _ENTRY_ROW.match(line) else "table_scaffold"
    if region == "owned_literals":
        return "owned_rom_literal"
    if region in ("dispatch", "main_glue", "prelude", "static_stop_fn"):
        return "glue"
    if region == "provenance_fn":
        return "mapping_metadata" if b"mapping" in line else "provenance"
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


def attribute(path: pathlib.Path) -> dict:
    cells: dict[str, list[int]] = {}
    counts = {"aot_function": 0, "ordinary_block": 0, "frontier_stop_fn": 0, "tier1_stop_fn": 0,
              "compiled_entry_rows": 0, "forward_declarations": 0}
    total_bytes = total_lines = 0
    region = "prelude"
    fn_sizes: dict[str, list[int]] = {"aot_function": [], "ordinary_block": []}
    fn_array_bytes: dict[str, list[int]] = {"aot_function": [], "ordinary_block": []}
    in_array = False
    array_count = array_elements = array_bytes = array_max = cur_elements = 0
    entry_addresses = {"aot": [], "block": []}
    with path.open("rb") as handle:
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
                em = _ENTRY_ROW_ADDR.match(line)
                if em:
                    entry_addresses[em.group(2).decode()].append(int(em.group(1), 16))
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
            if region == "entry_table" and cls == "compiled_entry_table":
                counts["compiled_entry_rows"] += 1
            cell = cells.setdefault(f"{region}.{cls}", [0, 0])
            cell[0] += len(line)
            cell[1] += 1
    categories = {
        "ordinary_block_bodies": ("ordinary_block.body",),
        "ordinary_block_boilerplate": ("ordinary_block.boilerplate",),
        "immutable_rom_aot_bodies": ("aot_function.body",),
        "aot_boilerplate": ("aot_function.boilerplate",),
        "dispatch_structures": ("dispatch.glue", "entry_table.table_scaffold", "main_glue.glue"),
        "compiled_entry_tables": ("entry_table.compiled_entry_table",),
        "target_membership_structures": ("aot_function.target_membership", "ordinary_block.target_membership",
                                         "tier1_stop_fn.target_membership", "frontier_stop_fn.target_membership"),
        "provenance": ("provenance_fn.provenance", "aot_function.provenance", "ordinary_block.provenance",
                       "frontier_stop_fn.provenance", "tier1_stop_fn.provenance"),
        "mapping_metadata": ("provenance_fn.mapping_metadata", "aot_function.mapping_metadata",
                             "ordinary_block.mapping_metadata", "frontier_stop_fn.mapping_metadata",
                             "tier1_stop_fn.mapping_metadata"),
        "frontier_code": ("aot_function.frontier_code", "ordinary_block.frontier_code",
                          "frontier_stop_fn.body", "frontier_stop_fn.frontier_code",
                          "frontier_stop_fn.boilerplate", "tier1_stop_fn.body", "tier1_stop_fn.frontier_code",
                          "tier1_stop_fn.boilerplate"),
        "owned_resolved_rom_literals": ("owned_literals.owned_rom_literal",),
        "runtime_prelude_glue": ("prelude.glue", "static_stop_fn.glue"),
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
    cmd += ["--generated-c-output", str(src)]
    gen = timed(cmd)
    if gen["returncode"] == 0 and (not src.is_file() or pathlib.Path(str(src) + ".partial").exists()):
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
        return report
    report["source"] = attribute(src)
    addresses = [int(x, 16) for x in admitted_sink.read_text().split()]
    admitted_sink.unlink()  # ephemeral: only count + digest are retained
    report["fingerprints"] = {"admitted_immutable_rom_aot_address_set": set_fingerprint(addresses)}
    runtime_dir = pathlib.Path(args.product_root) / "platforms" / "genesis" / "runtime"
    flags = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", args.opt, "-I", str(runtime_dir)]
    obj = out / "generated.o"
    comp = timed(flags + ["-c", "-o", str(obj), str(src)])
    robj = out / "runtime.o"
    rcomp = timed(flags + ["-c", "-o", str(robj), str(runtime_dir / "runtime.c")])
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
        "runtime_tu": {k: rcomp[k] for k in ("returncode", "wall_seconds", "peak_rss_bytes")},
    }
    if comp["returncode"] == 0 and rcomp["returncode"] == 0:
        exe = out / "bridge"
        link = timed([args.cc, "-o", str(exe), str(obj), str(robj)])
        report["link"] = {k: link[k] for k in ("returncode", "wall_seconds", "peak_rss_bytes")}
        report["artifact_bytes"] = {"generated_source": src.stat().st_size, "generated_object": obj.stat().st_size,
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
