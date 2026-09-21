#!/usr/bin/env python3
"""SEG-020-T006: first-divergence diagnosis separating CPU and Genesis device divergence.

Compares a generated boundary stream against an expected (reference) stream, one boundary per
retired instruction, using two line-oriented inputs that the opt-in diagnostics-enabled binary
emits per boundary (``genesis_m68k_checkpoint_write_detail`` and
``genesis_device_checkpoint_write_detail``; the two line kinds may share one file):

  {"m68k_checkpoint": {...}}    M68k-owned state and effects (schema: tools/m68k_first_divergence.py)
  {"device_checkpoint": {...}}  ``boundary``, ``unsupported``, ``components`` (per-component FNV-1a
                                digests) and ``events`` (bounded device command/interrupt events:
                                ``{"k":1,"r":region,"w":width,"a":addr,"v":value}`` device write,
                                ``{"k":2,...,"v":count}`` VBLANK rising edge, ``{"k":3,...,"v":6}``
                                IRQ6 admission).

Rules (ADR-0042 sections 3, 5, 10): sequential lockstep with a mandatory positive boundary limit;
at each boundary CPU presence/ordinal/unsupported checks come first (unsupported fails closed, domain ``none``), then the CPU fields are compared, and only when every CPU field matches are
device records compared. First differing domain is therefore ``cpu`` when any CPU field differs,
else ``device``, else ``none``. A device difference is classified ``device_command`` (the event
lists differ: a command or interrupt event happened, or not, or differently) or ``device_state``
(events agree but a named state component digest differs: device evolution). A boundary flagged
unsupported on either side is never equal. Device knowledge stays in the Genesis runtime that
produces the digests; this tool only names components. Output is deterministic; nothing is
persisted and nothing here can affect generated code. Values are ephemeral diagnostics.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import sys

_spec = importlib.util.spec_from_file_location(
    "m68k_first_divergence", pathlib.Path(__file__).resolve().with_name("m68k_first_divergence.py"))
cpu_tool = importlib.util.module_from_spec(_spec)
sys.modules.setdefault("m68k_first_divergence", cpu_tool)
_spec.loader.exec_module(cpu_tool)

SCHEMA = 1
REGION_NAMES = {1: "controller_io", 2: "psg", 3: "ym2612", 4: "vdp", 5: "z80_bus", 6: "z80_ram_window"}
EVENT_NAMES = {1: "write", 2: "vblank_raise", 3: "irq_admit"}


def parse_device_stream(text: str) -> list[dict]:
    records = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        record = json.loads(line)
        if "device_checkpoint" not in record or record["device_checkpoint"] is None:
            continue
        records.append(record["device_checkpoint"])
    return records


def _event_key(event: dict) -> str:
    name = EVENT_NAMES.get(event["k"], "kind%d" % event["k"])
    if event["k"] == 1:
        return "event:write@%s/%08X/w%d" % (REGION_NAMES.get(event["r"], "region%d" % event["r"]),
                                            event["a"], event["w"])
    return "event:" + name


def _event_view(event: dict | None) -> dict | None:
    return None if event is None else {"value": event["v"]}


def _event_map(record: dict) -> dict[str, dict]:
    """Ordered events keyed by kind/destination; a repeated key keeps its sequence (``#n``)."""
    counts: dict[str, int] = {}
    mapped: dict[str, dict] = {}
    for event in record.get("events", []):
        base = _event_key(event)
        n = counts.get(base, 0)
        counts[base] = n + 1
        mapped[base if n == 0 else "%s#%d" % (base, n)] = event
    return mapped


def device_differences(generated: dict, expected: dict) -> tuple[str, list[dict]]:
    """Return (``device_command`` | ``device_state`` | ``none``, field-level differences)."""
    gm, em = _event_map(generated), _event_map(expected)
    ordered_g = [_event_key(e) for e in generated.get("events", [])]
    ordered_e = [_event_key(e) for e in expected.get("events", [])]
    fields = []
    for key in sorted(set(gm) | set(em)):
        g, e = _event_view(gm.get(key)), _event_view(em.get(key))
        if g != e:
            fields.append({"field": key, "generated": g, "expected": e})
    if not fields and ordered_g != ordered_e:
        fields.append({"field": "event:order", "generated": ordered_g, "expected": ordered_e})
    if fields:
        return "device_command", fields
    gc, ec = generated.get("components", {}), expected.get("components", {})
    for name in sorted(set(gc) | set(ec)):
        if gc.get(name) != ec.get(name):
            fields.append({"field": "state:" + name, "generated": gc.get(name), "expected": ec.get(name)})
    return ("device_state" if fields else "none"), fields


def compare(cpu_generated: list[dict], cpu_expected: list[dict], device_generated: list[dict],
            device_expected: list[dict], limit: int, initial_pc: int, image: str | None = None) -> dict:
    if limit <= 0:
        raise ValueError("limit must be a positive boundary count")
    report: dict = {"schema": SCHEMA, "boundary_limit": limit, "domain": "none", "result": "no_divergence"}
    if image is not None:
        report["image"] = image
    last_pc = initial_pc
    compared = 0
    for index in range(limit):
        streams = (cpu_generated, cpu_expected, device_generated, device_expected)
        if all(index >= len(s) for s in streams):
            break
        boundary = index + 1
        failure = {"schema": SCHEMA, "boundary_limit": limit, "last_matching_boundary": compared,
                   "first_differing_boundary": boundary, "pc": last_pc}
        if image is not None:
            failure["image"] = image
        cg = cpu_generated[index] if index < len(cpu_generated) else None
        ce = cpu_expected[index] if index < len(cpu_expected) else None
        dg = device_generated[index] if index < len(device_generated) else None
        de = device_expected[index] if index < len(device_expected) else None
        if cg is None or ce is None:
            failure.update(domain="cpu", result="diverged", fields=[{
                "field": "boundary_presence", "generated": cg is not None, "expected": ce is not None}])
            return failure
        if cg.get("boundary") != boundary or ce.get("boundary") != boundary:
            failure.update(domain="cpu", result="diverged", fields=[{
                "field": "boundary_ordinal", "generated": cg.get("boundary"), "expected": ce.get("boundary")}])
            return failure
        if cg.get("unsupported") or ce.get("unsupported"):
            # Fail closed (ADR-0042 section 5): incomplete CPU effect evidence is never a confident divergence.
            failure.update(domain="none", result="unsupported_for_comparison", fields=[{
                "field": "unsupported", "generated": bool(cg.get("unsupported")),
                "expected": bool(ce.get("unsupported"))}])
            return failure
        cpu_fields = [{"field": f["field"], "generated": f["generated"], "expected": f["oracle"]}
                      for f in cpu_tool.field_differences(cg, ce)]
        if cpu_fields:
            failure.update(domain="cpu", result="diverged", fields=cpu_fields)
            return failure
        if dg is None or de is None:
            failure.update(domain="device", classification="device_state", result="diverged", fields=[{
                "field": "device_boundary_presence", "generated": dg is not None, "expected": de is not None}])
            return failure
        if dg.get("boundary") != boundary or de.get("boundary") != boundary:
            failure.update(domain="device", classification="device_state", result="diverged", fields=[{
                "field": "device_boundary_ordinal", "generated": dg.get("boundary"),
                "expected": de.get("boundary")}])
            return failure
        if dg.get("unsupported") or de.get("unsupported"):
            failure.update(domain="none", result="unsupported_for_comparison", fields=[{
                "field": "device_unsupported", "generated": bool(dg.get("unsupported")),
                "expected": bool(de.get("unsupported"))}])
            return failure
        classification, fields = device_differences(dg, de)
        if fields:
            failure.update(domain="device", classification=classification, result="diverged", fields=fields)
            return failure
        compared += 1
        last_pc = cg["pc"]
    report["compared_boundaries"] = compared
    return report


def render(report: dict) -> str:
    return json.dumps(report, sort_keys=True, indent=2) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--generated", required=True, help="generated stream (m68k + device lines)")
    parser.add_argument("--expected", required=True, help="expected stream (m68k + device lines)")
    parser.add_argument("--limit", type=int, required=True, help="mandatory boundary limit")
    parser.add_argument("--initial-pc", type=lambda v: int(v, 0), required=True)
    parser.add_argument("--image", default=None, help="platform-owned image identity to report")
    args = parser.parse_args(argv)
    generated = pathlib.Path(args.generated).read_text()
    expected = pathlib.Path(args.expected).read_text()
    report = compare(cpu_tool.parse_stream(generated), cpu_tool.parse_stream(expected),
                     parse_device_stream(generated), parse_device_stream(expected),
                     args.limit, args.initial_pc, args.image)
    sys.stdout.write(render(report))
    return 0 if report["result"] == "no_divergence" else 1


if __name__ == "__main__":
    sys.exit(main())
