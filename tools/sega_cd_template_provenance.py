#!/usr/bin/env python3
"""Writer attribution of the Sega CD mutable `JMP (xxx).L/.W` operand templates (SEG-079, ADR 0178/0179).

A template discovered by a build pass is evidence, not authority: its recorded address says nothing about *why* the opcode word is
there. This tool answers, from the program's writer log (`--ram-write-log`, see sega_cd_epoch_provenance.py), who stored each
template's opcode word and what kind of code that writer is:

  static_writer         main: every store of the opcode word executes from the immutable BIOS image (PC below the image size); the stored
                        value being a constant is NOT checked (only that it is a JMP opcode word)
  reconstructed_writer  sub: every store executes from a byte range of the epoch image that a recipe reconstructs from immutable input
                        (class 4 decoded regions; the caller supplies the ranges)
  runtime_writer        some store executes from RAM that no immutable input or reconstruction explains
  unattributed          no store of the opcode word before the end of the log

It also groups all opcode-word stores into *slot runs* (consecutive 6-byte slots written by the same writer PCs): a template that is
a member of a run shares its producer with the other slots, which is the structural fact a batched admission would rest on.

This attributes templates from one recorded execution. It is NOT a static derivation: nothing here proves, before any guest code runs,
that the writer will execute. Output is counts only (no addresses). `--detail` writes the private per-template detail.
"""
import argparse
import collections
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sega_cd_epoch_provenance as P  # noqa: E402

JMP_OPCODES = (0x4EF8, 0x4EF9)
SLOT = 6
WORK_BASE = 0xFF0000


def opcode_stores(log):
    """-> [(record index, cpu, address, writer pc)] of stores whose first word is a JMP opcode (word, or the high half of a long)."""
    out = []
    for n, (kind, width, cpu, _, offset, value, pc) in enumerate(log):
        if kind not in (P.KIND_PRG, P.KIND_WORK):
            continue
        if width == 2 and value in JMP_OPCODES or width == 4 and (value >> 16) in JMP_OPCODES:
            address = (WORK_BASE | offset) if kind == P.KIND_WORK else offset
            out.append((n, cpu, address, pc))
    return out


def slot_runs(stores):
    """Group stores into runs of consecutive slots per processor: -> [(cpu, slots, distinct writer pcs)]."""
    by_cpu = collections.defaultdict(dict)
    for _, cpu, address, pc in stores:
        by_cpu[cpu].setdefault(address, set()).add(pc)
    runs = []
    for cpu, slots in sorted(by_cpu.items()):
        addresses = sorted(slots)
        run = [addresses[0]]
        for a in addresses[1:]:
            if a - run[-1] == SLOT:
                run.append(a)
            else:
                runs.append((cpu, run, set().union(*(slots[x] for x in run))))
                run = [a]
        runs.append((cpu, run, set().union(*(slots[x] for x in run))))
    return runs


def classify(templates, log, bios_size, reconstructed):
    """templates: [(cpu, address)] with cpu 0 = main, 1 = sub. reconstructed: [(lo, hi)] sub PC ranges explained by a recipe."""
    stores = opcode_stores(log)
    runs = slot_runs(stores)
    run_of = {}
    for cpu, slots, _ in runs:
        for a in slots:
            run_of[(cpu, a)] = len(slots)
    counts = collections.Counter()
    detail = []
    for cpu, address in templates:
        mine = [s for s in stores if s[1] == cpu and s[2] == address]
        writers = {s[3] for s in mine}
        if not mine:
            cls = "unattributed"
        elif cpu == 0 and all(pc < bios_size for pc in writers):
            cls = "static_writer"
        elif cpu == 1 and all(any(lo <= pc < hi for lo, hi in reconstructed) for pc in writers):
            cls = "reconstructed_writer"
        else:
            cls = "runtime_writer"
        counts[cls] += 1
        detail.append({"cpu": cpu, "address": address, "class": cls, "run_slots": run_of.get((cpu, address), 0)})
    run_sizes = sorted((len(slots) for _, slots, _ in runs), reverse=True)
    members = sum(1 for d in detail if d["run_slots"] > 1)
    report = {"templates": len(templates),
              "classes": {k: counts.get(k, 0) for k in ("static_writer", "reconstructed_writer", "runtime_writer", "unattributed")},
              "templates_in_multi_slot_runs": members,
              "slot_runs": {"count": len(runs), "multi_slot": sum(1 for s in run_sizes if s > 1), "largest": run_sizes[:4]},
              "statically_derived_before_execution": 0}  # a documented literal: this tool derives nothing, it attributes recorded stores
    return report, detail


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--write-log", required=True)
    ap.add_argument("--templates", required=True, help="lines 'main|sub 0xADDRESS' (the build driver's admissions)")
    ap.add_argument("--bios-size", type=lambda v: int(v, 0), required=True)
    ap.add_argument("--reconstructed", action="append", default=[], help="LO-HI hex sub PC range explained by a recipe (repeatable)")
    ap.add_argument("--detail")
    args = ap.parse_args(argv)
    templates = []
    for line in pathlib.Path(args.templates).read_text().splitlines():
        if line.strip():
            who, _, addr = line.partition(" ")
            templates.append((0 if who == "main" else 1, int(addr, 0)))
    ranges = []
    for spec in args.reconstructed:
        lo, _, hi = spec.partition("-")
        ranges.append((int(lo, 16), int(hi, 16)))
    report, detail = classify(templates, P.read_log(args.write_log), args.bios_size, ranges)
    if args.detail:
        pathlib.Path(args.detail).write_text(json.dumps(detail))
    json.dump(report, sys.stdout, indent=1)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
