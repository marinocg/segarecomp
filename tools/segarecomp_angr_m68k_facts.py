#!/usr/bin/env python3
"""SEG-041-T008: the smallest real external-analysis producer for `segarecomp.m68k_external_facts.v1`.

Uses angr's p-code M68K engine (SEG-041-T002: qualified for ordinary, non-RTE control-flow and data
semantics; `CPU32` is the least-bad available variant) to attempt an exact-target proof for one
dynamic-control site, writing a ROM-bound, bounded, deterministic fact file
`segarecomp.m68k_external_facts.v1` that `segarecomp-genesis-analysis-report --hybrid-plan
--external-m68k-facts <path>` independently re-verifies (ROM hash, CPU legality of every cited target,
mapping) before trusting anything -- this tool is never a correctness authority, only a proof producer.

This is deliberately narrow, not a general whole-program analysis: the caller supplies the exact
starting PC/register state to explore from and the one dynamic-control site PC/register to resolve.
A real caller obtains that starting context from segarecomp's own existing private report output
(computed-site PCs, their family/reason) plus whatever additional reachability premise a broader
(out-of-scope-for-this-tool) analysis pass established; this tool does not discover that context on
its own, and does not claim whole-program reachability.

Usage:
    python3 tools/segarecomp_angr_m68k_facts.py \
        --rom <path> --rom-sha256 <sha256> \
        --start-pc <hex> --target-pc <hex> --target-register a0 \
        [--ram-premise <hex-address>=<hex-value> ...] \
        --output <path> [--max-entries 64] [--max-steps 100000] [--producer <token>]

Exit status 0 with a written file means the exact-target set was proven and is non-empty and bounded.
Exit status 1 (nothing written) is an honest negative result: never force a speculative fact.
"""
from __future__ import annotations

import argparse
import hashlib
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", required=True)
    parser.add_argument("--rom-sha256", required=True)
    parser.add_argument("--start-pc", required=True, help="hex address to begin concrete/symbolic exploration from")
    parser.add_argument("--target-pc", required=True, help="hex address of the dynamic-control site to resolve")
    parser.add_argument("--target-register", default="a0", help="the address register read at --target-pc (default a0)")
    parser.add_argument("--ram-premise", action="append", default=[],
                         help="ADDR=VALUE (both hex): a concrete byte this tool assumes as a reachability premise, "
                              "not something it discovers itself -- see the module docstring")
    parser.add_argument("--output", required=True)
    parser.add_argument("--max-entries", type=int, default=64)
    parser.add_argument("--max-steps", type=int, default=100_000)
    parser.add_argument("--producer", default="segarecomp-angr-m68k-v1")
    parser.add_argument("--variant", default="68000:BE:32:CPU32",
                         help="pypcode M68K variant (SEG-041-T002: CPU32 has the narrowest false-acceptance footprint "
                              "of the available variants; none is bit-exact MC68000 -- the consumer re-verifies "
                              "every cited target independently regardless of this choice)")
    args = parser.parse_args()

    rom = open(args.rom, "rb").read()
    digest = hashlib.sha256(rom).hexdigest()
    if digest != args.rom_sha256.lower():
        print(f"segarecomp_angr_m68k_facts: --rom-sha256 does not match the ROM's actual digest "
              f"({digest} != {args.rom_sha256.lower()}); refusing to analyse a mismatched image", file=sys.stderr)
        return 2

    import angr
    import logging
    logging.getLogger("angr").setLevel(logging.ERROR)
    from archinfo import ArchPcode

    start_pc = int(args.start_pc, 16)
    target_pc = int(args.target_pc, 16)
    arch = ArchPcode(args.variant)
    proj = angr.Project(args.rom, main_opts={"backend": "blob", "arch": arch, "base_addr": 0, "entry_point": start_pc})
    state = proj.factory.blank_state(addr=start_pc)
    for premise in args.ram_premise:
        addr_text, value_text = premise.split("=", 1)
        state.memory.store(int(addr_text, 16), int(value_text, 16).to_bytes(4, "big"))

    simgr = proj.factory.simulation_manager(state)
    seen = set()
    steps = 0
    idle_since_new = 0
    idle_bound = 64  # bounded: stop once no new feasible value has appeared for this many consecutive steps (e.g. a reached
                      # self-loop island entry), never an unbounded wait for a naturally-terminating exploration that may not exist
    while simgr.active and steps < args.max_steps and idle_since_new < idle_bound:
        before = len(seen)
        for s in simgr.active:
            if s.solver.eval(s.regs.pc) == target_pc:
                reg = getattr(s.regs, args.target_register)
                for value in s.solver.eval_upto(reg, args.max_entries + 1):
                    seen.add(value)
        idle_since_new = 0 if len(seen) != before else (idle_since_new + 1 if seen else 0)
        simgr.step(num_inst=1)
        steps += 1
        if len(seen) > args.max_entries:
            break

    if not seen or len(seen) > args.max_entries:
        print(f"segarecomp_angr_m68k_facts: no bounded exact-target set proven at {args.target_pc} "
              f"within {steps} steps (honest negative result; nothing written)", file=sys.stderr)
        return 1

    entries = sorted(seen)
    lines = [
        "segarecomp.m68k_external_facts.v1",
        f"rom_sha256 {digest}",
        f"producer {args.producer}",
        f"fact {target_pc:08x} exact " + ",".join(f"{e:08x}" for e in entries),
        "end",
        "",
    ]
    with open(args.output, "w", encoding="ascii") as f:
        f.write("\n".join(lines))
    print(f"segarecomp_angr_m68k_facts: proved exact target set {{{', '.join(hex(e) for e in entries)}}} "
          f"at {hex(target_pc)} in {steps} steps; wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
