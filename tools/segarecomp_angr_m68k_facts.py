#!/usr/bin/env python3
"""SEG-041-T008/correction: the smallest real external-analysis producer for
`segarecomp.m68k_external_facts.v1` that genuinely proves exhaustiveness within its declared scope.

Uses angr's p-code M68K engine (SEG-041-T002: qualified for ordinary, non-RTE control-flow and data
semantics; `CPU32` is the least-bad available variant -- no available variant is bit-exact MC68000,
and this tool does NOT independently re-verify base-MC68000 legality of every instruction it traverses
during exploration; see "Proof-path CPU-legality" below) to attempt an EXACT-TARGET proof for one
dynamic-control site, writing a ROM-bound, bounded, deterministic fact file
`segarecomp.m68k_external_facts.v1` that `segarecomp-genesis-analysis-report --hybrid-plan
--external-m68k-facts <path>` independently re-verifies (ROM hash, CPU legality of every CITED target,
mapping) before trusting anything.

## Two-tier trust model (this tool's actual role)

segarecomp independently owns and re-derives: ROM identity, mapped address, alignment, MC68000
decode/legality of every CITED target, structural closure, island closure, admission, emission. This
tool is NOT "never a correctness authority" -- it IS the narrowly-scoped semantic-completeness
authority for the one claim segarecomp cannot structurally re-derive on its own: "there is no additional
feasible target beyond the ones named here." That claim is accepted only under this tool's explicit,
ROM-bound, producer-identified contract (SEG-041-T001's design), and only once this tool has itself
closed its own exploration soundly (see "Exhaustiveness" below) -- never from a resource-bound timeout,
never from a heuristic, never partially.

## Exhaustiveness (the correctness-critical part of this tool)

A claimed `exact` fact is sound only when ALL of the following hold for the declared starting scope:

  - every feasible path from `--start-pc` was explored to completion (no active state remains);
  - no path produced an unresolved/unconstrained program counter (an unconstrained PC invalidates the
    whole proof, even if another path already found a target -- "one bad branch poisons the proof");
  - no path errored (an angr lifting/execution error invalidates the whole proof, for the same reason);
  - the step/resource bound was never exhausted while a path remained unresolved (exhaustion means
    Unknown, never "assume the found set is complete");
  - the resulting target set is non-empty and no larger than `--max-entries`.

A state that reaches `--target-pc` is removed from further exploration the moment its target-register
value is queried (it is not stepped again) -- this is what lets a self-looping or otherwise
non-terminating target site be handled soundly without any timing-based heuristic standing in for proof.

## Proof-path opcode-class check (the smallest existing mechanism, not a new certificate framework)

Every opcode word any explored state actually fetches is checked against segarecomp's own
pre-existing, already-validated `tests/fixtures/m68k-legal-forms.json` primary-word partition
(`legal_user`/`legal_privileged` required; `line_a_reserved_exception`/`line_f_reserved_exception`/
`illegal_reserved_unassigned`/`illegal_post_68000_encoding` fail the proof). This is a per-opcode-word
class check reusing existing project data, not a per-instruction structural legality re-derivation (the
C++ consumer still independently re-verifies every CITED TARGET's full structural legality via
`image.decode()`; this check additionally screens the PATH angr traversed to reach that conclusion, at
the coarser word-class granularity SEG-041-T002 already used). Full proof-path structural requalification
against segarecomp's own decoder is deliberately NOT built here (that would be a new certificate
framework); it remains a required gate of the real-title successor milestone (SEG-042-T001).

## Scope and premises (deliberately narrow, deliberately not globally automatic)

This tool does not discover its own starting context. The caller supplies `--start-pc` and may supply
`--ram-premise ADDR=VALUE` (a 4-byte big-endian concrete value this tool ASSUMES at that address --
never something it discovers or verifies). A fact produced using any `--ram-premise` is globally valid
ONLY if the supplied starting scope already covers every relevant execution reaching the target site --
this tool cannot check that, and does not pretend to. Supplying `--ram-premise` therefore REQUIRES the
caller to also pass `--caller-asserts-premise-completeness`, an explicit, unambiguous, non-default
assertion that the caller (not this tool, not segarecomp) is vouching for that coverage; without it,
this tool refuses to write any fact file at all. The producer identity recorded in a premise-derived
fact is suffixed `+caller-asserted-premise` so the artifact itself discloses this.

Usage:
    python3 tools/segarecomp_angr_m68k_facts.py \
        --rom <path> --rom-sha256 <sha256> \
        --start-pc <hex> --target-pc <hex> --target-register a0 \
        [--ram-premise <hex-address>=<hex-value> --caller-asserts-premise-completeness] \
        --output <path> [--max-entries 64] [--max-steps 100000] [--producer <token>]

Exit status 0 with a written file means the exact-target set was soundly proven exhaustive within the
declared scope and is non-empty and bounded. Exit status 1 (nothing written) is an honest negative
result (Unknown) -- never force a speculative or partial fact. Exit status 2 is a usage/precondition
error (bad ROM hash, missing premise assertion).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct
import sys

LEGAL_FORMS_PATH = pathlib.Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "m68k-legal-forms.json"
LEGAL_CLASSES = {"L", "P"}  # legal_user, legal_privileged (see m68k-legal-forms.json's own legend)


def load_legal_word_classifier():
    data = json.loads(LEGAL_FORMS_PATH.read_text(encoding="utf-8"))
    rows = data["primary_word_partition"]["rows"]

    def classify(word: int) -> str:
        hi, lo = (word >> 8) & 0xFF, word & 0xFF
        return rows[hi][lo]

    return classify


def explore_exact_target(rom_path, start_pc, target_pc, target_register, ram_premises, max_entries, max_steps):
    """Returns (sorted_targets, steps, reason) on a sound exhaustive proof, or (None, steps, reason) otherwise."""
    import angr
    import logging
    logging.getLogger("angr").setLevel(logging.ERROR)
    from archinfo import ArchPcode

    classify_word = load_legal_word_classifier()
    rom_bytes = open(rom_path, "rb").read()

    arch = ArchPcode("68000:BE:32:CPU32")
    proj = angr.Project(rom_path, main_opts={"backend": "blob", "arch": arch, "base_addr": 0, "entry_point": start_pc})
    state = proj.factory.blank_state(addr=start_pc)
    for addr, value in ram_premises:
        state.memory.store(addr, value.to_bytes(4, "big"))

    simgr = proj.factory.simulation_manager(state)
    found = set()
    visited_pcs = set()
    steps = 0
    non_concrete = {"hit": False}

    def is_query_complete(s) -> bool:
        pc_candidates = s.solver.eval_upto(s.regs.pc, 2)
        if len(pc_candidates) != 1:
            # An active state must carry a concrete PC; angr's own stepping is what splits a genuinely
            # symbolic successor into multiple concretely-addressed states (or moves a truly
            # unconstrained one to the `unconstrained` stash, checked below). Seeing more than one
            # feasible PC value here would mean this tool is about to use a single arbitrary `eval()`
            # as if it were proof of a unique target -- flag it and refuse, rather than silently
            # picking one value.
            non_concrete["hit"] = True
            return False
        pc = pc_candidates[0]
        visited_pcs.add(pc)
        return pc == target_pc

    while simgr.active and steps < max_steps:
        # `simgr.move` (not a direct reassignment of `simgr.active`) is the API-correct way to remove a
        # query-complete state from further stepping while leaving every other active state's own
        # stepping schedule intact -- this is what makes a self-looping or otherwise non-terminating
        # target site safe without any idle/timing heuristic standing in for proof.
        simgr.move(from_stash="active", to_stash="query_complete", filter_func=is_query_complete)
        if non_concrete["hit"]:
            # Independent review could not make this branch fire under this angr version's actual
            # active-stash semantics (a genuinely non-unique/symbolic PC is routed straight to the
            # `unconstrained` stash before ever appearing in `active`) -- kept as a defensive, fail-closed
            # guard rather than removed, since relying on that routing behavior being permanent across
            # angr versions would itself be an unverified assumption.
            return None, steps, "non_concrete_active_pc"
        for s in simgr.stashes.get("query_complete", []):
            reg = getattr(s.regs, target_register)
            values = s.solver.eval_upto(reg, max_entries + 1)
            found.update(values)
            if len(found) > max_entries:
                return None, steps, "entry_bound_exceeded"
        simgr.drop(stash="query_complete")  # already folded into `found`; do not re-query on the next round
        if not simgr.active:
            break
        simgr.step(num_inst=1)
        steps += 1
        if simgr.errored:
            return None, steps, "errored_path"
        if simgr.unconstrained:
            return None, steps, "unconstrained_path"

    if simgr.active:
        return None, steps, "resource_exhausted"  # the step bound was hit with unresolved paths remaining
    if simgr.errored:
        return None, steps, "errored_path"
    if simgr.unconstrained:
        return None, steps, "unconstrained_path"
    if not found:
        return None, steps, "empty_target_set"
    if len(found) > max_entries:
        return None, steps, "entry_bound_exceeded"

    for pc in visited_pcs:
        if pc + 2 > len(rom_bytes):
            continue  # outside the backing image; not a traversed opcode fetch this check can classify
        word = struct.unpack(">H", rom_bytes[pc:pc + 2])[0]
        word_class = classify_word(word)
        if word_class not in LEGAL_CLASSES:
            return None, steps, f"proof_path_opcode_class_{word_class}_at_{pc:06x}"

    return sorted(found), steps, "closed"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rom", required=True)
    parser.add_argument("--rom-sha256", required=True)
    parser.add_argument("--start-pc", required=True, help="hex address to begin exploration from")
    parser.add_argument("--target-pc", required=True, help="hex address of the dynamic-control site to resolve")
    parser.add_argument("--target-register", default="a0", help="the address register read at --target-pc (default a0)")
    parser.add_argument("--ram-premise", action="append", default=[],
                         help="ADDR=VALUE (both hex): a concrete 4-byte big-endian value this tool ASSUMES as a "
                              "reachability premise at ADDR, never something it discovers or verifies itself -- "
                              "requires --caller-asserts-premise-completeness (see the module docstring)")
    parser.add_argument("--caller-asserts-premise-completeness", action="store_true",
                         help="required whenever --ram-premise is used: an explicit, non-default assertion that "
                              "the CALLER (not this tool, not segarecomp) vouches that the supplied starting scope "
                              "covers every relevant execution reaching --target-pc")
    parser.add_argument("--output", required=True)
    parser.add_argument("--max-entries", type=int, default=64,
                         help="keep this bounded (default 64): an unusually large value can let the solver spend "
                              "disproportionate time enumerating a wide/illegal-opcode-influenced value set before "
                              "this tool still correctly fails closed to entry_bound_exceeded -- a performance "
                              "footnote raised by independent review, not a soundness gap (it never emits a "
                              "partial/unsound result either way)")
    parser.add_argument("--max-steps", type=int, default=100_000)
    parser.add_argument("--producer", default="segarecomp-angr-m68k-v1")
    args = parser.parse_args()

    if args.ram_premise and not args.caller_asserts_premise_completeness:
        print("segarecomp_angr_m68k_facts: --ram-premise requires --caller-asserts-premise-completeness -- "
              "this tool cannot itself verify that a caller-supplied premise's starting scope covers every "
              "relevant execution reaching --target-pc; refusing to write a fact file without that explicit, "
              "caller-owned assertion (see the module docstring)", file=sys.stderr)
        return 2

    rom = open(args.rom, "rb").read()
    digest = hashlib.sha256(rom).hexdigest()
    if digest != args.rom_sha256.lower():
        print(f"segarecomp_angr_m68k_facts: --rom-sha256 does not match the ROM's actual digest "
              f"({digest} != {args.rom_sha256.lower()}); refusing to analyse a mismatched image", file=sys.stderr)
        return 2

    start_pc = int(args.start_pc, 16)
    target_pc = int(args.target_pc, 16)
    premises = []
    for premise in args.ram_premise:
        addr_text, value_text = premise.split("=", 1)
        premises.append((int(addr_text, 16), int(value_text, 16)))

    targets, steps, reason = explore_exact_target(
        args.rom, start_pc, target_pc, args.target_register, premises, args.max_entries, args.max_steps)

    if targets is None:
        print(f"segarecomp_angr_m68k_facts: no soundly-exhaustive exact-target set proven at "
              f"{hex(target_pc)} within {steps} steps (reason: {reason}; honest negative/Unknown result, "
              f"nothing written)", file=sys.stderr)
        return 1

    producer = args.producer + ("+caller-asserted-premise" if args.ram_premise else "")
    lines = [
        "segarecomp.m68k_external_facts.v1",
        f"rom_sha256 {digest}",
        f"producer {producer}",
        f"fact {target_pc:08x} exact " + ",".join(f"{e:08x}" for e in targets),
        "end",
        "",
    ]
    with open(args.output, "w", encoding="ascii") as f:
        f.write("\n".join(lines))
    print(f"segarecomp_angr_m68k_facts: soundly proved exact target set {{{', '.join(hex(e) for e in targets)}}} "
          f"at {hex(target_pc)} in {steps} steps (exploration closed with no unresolved/errored/unconstrained "
          f"path); wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
