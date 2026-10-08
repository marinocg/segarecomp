#!/usr/bin/env python3
"""SEG-042-T002: regression tests for tools/segarecomp_recomp_map_harvest.py, the automatic real-title
external-fact harvester. Project-authored synthetic MC68000 bytes/report fixtures only; no commercial
input.

Pure-logic tests (backward-walk starting-scope rule, external-entry check, eligible-site filtering, fact-
file formatting) run hermetically against a hand-built synthetic report dict -- no angr, no native
binary, no ROM. The end-to-end test additionally needs angr (SKIPPED, not failed, when unavailable,
matching this suite's existing optional-dependency convention) and the native primary-word classifier
CLI (its path is the first CLI argument; SKIPPED when not given/not found, since it is only built as part
of the full product build, not by this test file itself).
"""
from __future__ import annotations

import pathlib
import struct
import sys
import tempfile

TOOLS_DIR = pathlib.Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS_DIR))

import segarecomp_recomp_map_harvest as harvest_module  # noqa: E402

failures = 0


def expect(condition: bool, message: str) -> None:
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {message}", file=sys.stderr)


def test_eligible_sites_excludes_rte_and_unclassified() -> None:
    report = {
        "computed_sites": {
            "000100": {"family": "rte", "outcome": "unknown"},
            "000200": {"family": "unclassified", "outcome": "unknown"},
            "000300": {"family": "rts_computed", "outcome": "unknown"},
            "000400": {"family": "jsr_an", "outcome": "unknown"},
            "000500": {"family": "jsr_an", "outcome": "resolved"},  # already resolved: not eligible either
        },
    }
    sites = harvest_module.eligible_sites(report)
    pcs = [s["pc"] for s in sites]
    expect(pcs == [0x300, 0x400], f"eligible_sites must exclude rte/unclassified/resolved, got {[hex(p) for p in pcs]}")


def test_backward_walk_and_external_entry_check() -> None:
    # entry(0x200, 4 bytes) -> site(0x204); a second, unrelated function g(0x300, 4 bytes) has a static
    # successor landing INSIDE (entry, site] at 0x206 -- an external entry the inner candidate must reject,
    # forcing the walk to widen to the next (and here, only remaining) call-target entry: the program root.
    discovered_lengths = {"000200": 4, "000204": 4, "000300": 4}
    call_target_entries = {0x200}
    roots = {0x200}
    static_successors = {"000300": ["000204"]}  # g jumps into the middle of entry's span
    predecessor_index = harvest_module.build_predecessor_index(discovered_lengths)
    expect(predecessor_index == {0x204: 0x200, 0x208: 0x204, 0x304: 0x300},
           f"build_predecessor_index: unexpected index {predecessor_index}")

    # Without the external successor, 0x200 is immediately sound for a site at 0x204.
    clean_start, _ = harvest_module.find_sound_start_pc(0x204, predecessor_index, call_target_entries, roots, {}, set(),
                                                         {0x204: {0x200}, 0x208: {0x204}}, "span")
    expect(clean_start == 0x200, f"find_sound_start_pc: expected 0x200 with no external entries, got {clean_start}")

    # With the external successor landing inside (0x200, 0x208], a site at 0x208 must reject 0x200 and find
    # no further candidate (0x200 is the only call-target entry/root reachable by the backward walk here).
    violated = harvest_module.external_entry_violates(0x200, 0x208, static_successors)
    expect(violated, "external_entry_violates: must detect the edge from 0x300 into (0x200, 0x208]")
    for model in ("span", "cfg"):
        preds = harvest_module.build_predecessor_graph(discovered_lengths, static_successors)
        rejected, _ = harvest_module.find_sound_start_pc(0x208, predecessor_index, call_target_entries, roots,
                                                         static_successors, set(), preds, model)
        expect(rejected is None, f"find_sound_start_pc[{model}]: must reject the only candidate, got {rejected}")


def test_gap_termination_point_is_a_sound_candidate() -> None:
    """A real Sonic 1 finding: a site reached only through an ordinary branch (not a call) into a
    function with no JSR/BSR caller at all has an empty backward-walk candidate list under the
    call-target/root-only rule -- even though the branch target is itself a perfectly sound, proven-
    unique entry (segarecomp's own discovery could only have found it via that one edge). The point
    where the contiguous walk naturally runs out of predecessors (a gap) must be credited as a final-
    resort candidate when it is itself a confirmed jump target, still subject to the same external-entry
    check as every other candidate."""
    # g(0x300, 4 bytes) is reached only by a plain branch from 0x280; site(0x304) is g's second instruction.
    discovered_lengths = {"000300": 4, "000304": 4}
    call_target_entries: set[int] = set()
    roots: set[int] = set()
    static_successors = {"000280": ["000300"]}
    jump_targets = harvest_module.static_jump_targets(static_successors)
    expect(jump_targets == {0x300}, f"static_jump_targets: unexpected set {jump_targets}")
    predecessor_index = harvest_module.build_predecessor_index(discovered_lengths)

    preds = harvest_module.build_predecessor_graph(discovered_lengths, static_successors)
    found, _ = harvest_module.find_sound_start_pc(0x304, predecessor_index, call_target_entries, roots,
                                                  static_successors, jump_targets, preds)
    expect(found == 0x300, f"find_sound_start_pc: expected the gap-termination branch target 0x300, got {found}")

    # An UNCONFIRMED gap (nothing names it as a jump target at all) must never be credited: that would be
    # trusting an arbitrary address boundary, not a proven incoming edge.
    unconfirmed, _ = harvest_module.find_sound_start_pc(0x304, predecessor_index, call_target_entries, roots, {}, set(),
                                                        harvest_module.build_predecessor_graph(discovered_lengths, {}))
    expect(unconfirmed is None, f"find_sound_start_pc: an unconfirmed gap must never be credited, got {unconfirmed}")


def region(start, site, nodes, edges, roots, max_nodes=4096):
    """Build the predecessor graph for a synthetic layout of 2-byte instructions at `nodes` (hex ints) with
    explicit static `edges` {src: [dst]} and return entry_closed_region's result."""
    lengths = {format(n, "06x"): 2 for n in nodes}
    succ = {format(k, "06x"): [format(t, "06x") for t in v] for k, v in edges.items()}
    preds = harvest_module.build_predecessor_graph(lengths, succ)
    result = harvest_module.entry_closed_region(start, site, preds, set(roots), max_nodes)
    span = not harvest_module.external_entry_violates(start, site, succ)
    return result, span


def test_cfg_region_adversarial_cases() -> None:
    body = [0x300, 0x302, 0x304, 0x306, 0x308, 0x30A, 0x30C]  # contiguous 2-byte instructions
    # 1. internal backward edge whose source lies numerically PAST the site: old span rejects, region accepts.
    (reg, why), span = region(0x300, 0x306, body, {0x30A: [0x304]}, {0x300})
    expect(not span, "case 1: the linear span model must (conservatively) reject the backedge")
    expect(reg is not None and why == "ok" and 0x30A in reg, f"case 1: region must accept the internal backedge: {why}")
    # 2. a genuine second external entry into an interior node: both models reject.
    other = body + [0x500, 0x502]
    (reg, why), span = region(0x300, 0x306, other, {0x500: [0x304]}, {0x300, 0x500})
    expect(reg is None and why == "root_entry_into_region" and not span, f"case 2: external root entry must reject: {why}")
    # 2b. external entry from a node with no known predecessor at all (unaccounted): reject.
    (reg, why), _ = region(0x300, 0x306, other, {0x500: [0x304]}, {0x300})
    expect(reg is None and why == "unaccounted_entry", f"case 2b: unaccounted external entry must reject: {why}")
    # 3. multiple internal branches / nested loops: accepted.
    (reg, why), _ = region(0x300, 0x30A, body, {0x306: [0x302], 0x30C: [0x304], 0x308: [0x300]}, {0x300})
    expect(reg is not None, f"case 3: nested internal loops must be accepted: {why}")
    # 4. call-target entry + unrelated outside branch into the middle. The outside node has its own
    # independent entry (a second root) -> reject; if instead it is reachable ONLY through the region it is
    # an internal edge -> accept.
    far = body + [0x700, 0x702]
    (reg, why), _ = region(0x300, 0x306, far, {0x700: [0x304]}, {0x300, 0x700})
    expect(reg is None, "case 4a: unrelated outside branch with an independent entry must reject")
    (reg, why), _ = region(0x300, 0x306, far, {0x30C: [0x700], 0x700: [0x304]}, {0x300})
    expect(reg is not None, f"case 4b: an outside node reachable ONLY through the region is internal: {why}")
    # 5. no sound closed boundary: closure reaches a root other than start, or exceeds the fixed bound,
    # or never reaches `start` at all.
    (reg, why), _ = region(0x304, 0x30A, body, {}, {0x300, 0x306})
    expect(reg is None and why == "root_entry_into_region", f"case 5a: a root inside the region must reject: {why}")
    (reg, why), _ = region(0x304, 0x30A, body, {}, {0x300})
    expect(reg is not None, f"case 5a': a root BEFORE the start (behind the cut) is fine: {why}")
    (reg, why), _ = region(0x300, 0x30C, body, {}, {0x300}, max_nodes=3)
    expect(reg is None and why == "region_bound_exceeded", f"case 5b: bound exhaustion is Unknown: {why}")
    (reg, why), _ = region(0x700, 0x30A, far, {}, {0x300})
    expect(reg is None, "case 5c: a start that is not an ancestor of the site must reject")
    # A root equal to the site itself is a direct entry unless the site is the start.
    (reg, why), _ = region(0x300, 0x304, body, {}, {0x300, 0x304})
    expect(reg is None and why == "root_entry_into_region", f"site that is itself a root must reject: {why}")
    (reg, why), _ = region(0x304, 0x304, body, {}, {0x304})
    expect(reg == frozenset({0x304}), "degenerate start == site is accepted (matches the span model)")


def test_cfg_region_matches_bruteforce_dominance() -> None:
    """Independent oracle: with entries = roots + predecessor-less nodes, accept iff the site is unreachable
    from any entry (other than start) in the graph with `start` removed, and start reaches the site."""
    import random
    rng = random.Random(0x043)
    for _ in range(600):
        count = rng.randint(2, 9)
        nodes = [0x100 + 2 * i for i in range(count)]
        edges = {}
        for src in nodes:
            if rng.random() < 0.5:
                edges[src] = [rng.choice(nodes) for _ in range(rng.randint(1, 2))]
        roots = {n for n in nodes if rng.random() < 0.25}
        start, site = rng.choice(nodes), rng.choice(nodes)
        lengths = {format(n, "06x"): 2 for n in nodes}
        succ = {format(k, "06x"): [format(t, "06x") for t in v] for k, v in edges.items()}
        preds = harvest_module.build_predecessor_graph(lengths, succ)
        got, _ = harvest_module.entry_closed_region(start, site, preds, roots)
        forward = {n: set() for n in nodes}
        for dst, srcs in preds.items():
            for src in srcs:
                if dst in forward:
                    forward[src].add(dst)
        entries = {n for n in nodes if n in roots or not preds.get(n)}

        def reach(seeds, banned):
            seen, work = set(), [x for x in seeds if x != banned]
            while work:
                n = work.pop()
                if n in seen or n == banned:
                    continue
                seen.add(n)
                work.extend(forward.get(n, ()))
            return seen
        if start == site:
            expected = True  # closure is just {site}; accepted without looking behind the start
        else:
            expected = site not in reach(entries, start) and site in reach([start], None)
            expected = expected and start in reach([start], None)
        expect((got is not None) == expected,
               f"region != brute-force dominance (start={start:x} site={site:x} roots={roots} edges={edges}): "
               f"got={got is not None} expected={expected}")


def test_interrupt_unproven_sites_never_get_external_exact_facts() -> None:
    report = {
        "computed_sites": {"000204": {"family": "jmp_an", "outcome": "unknown", "reason": "unknown_input",
                                       "detail": "interrupt_resumption_unproven"}},
        "discovered_lengths": {"000200": 4, "000204": 2},
        "roots": ["000200"], "static_successors": {}, "call_target_continuations": {},
    }
    original = harvest_module.attempt_exact
    harvest_module.attempt_exact = lambda *a, **k: {"outcome": "exact", "targets": [0x300], "cost": {}}
    try:
        results = harvest_module.harvest(report, "/nonexistent-rom", "/nonexistent-classifier", 64, 10, "cfg", True)
        expect(results[0].get("uncredited_program_order_result", {}).get("targets") == [0x300],
               "diagnostic mode must record the uncredited program-order result")
    finally:
        harvest_module.attempt_exact = original
    for diag in (False,):
        results = harvest_module.harvest(report, "/nonexistent-rom", "/nonexistent-classifier", 64, 10, "cfg", diag)
        expect(len(results) == 1 and results[0]["outcome"] == "unsupported" and
               results[0]["reason"] == "external_exact_proof_ignores_interrupt_resumption",
               f"interrupt-unproven site must never be credited (diag={diag}): {results}")


def synth_report(nodes, edges, roots, no_fallthrough, calls, pushed=()):
    """Synthetic report: `nodes` {pc: length}, `edges` {src: [dst]} (non-fallthrough static successors),
    `calls` {call_site_pc: callee} (also static successors, and contribute call_target_continuations)."""
    succ = {}
    for src, dsts in edges.items():
        succ.setdefault(src, []).extend(dsts)
    cont = {}
    for site, callee in calls.items():
        succ.setdefault(site, []).append(callee)
        cont.setdefault(callee, set()).add(site + 2)  # call sites are 2 bytes wide in these layouts
    return {
        "discovered_lengths": {format(p, "06x"): n for p, n in nodes.items()},
        "static_successors": {format(k, "06x"): [format(d, "06x") for d in v] for k, v in succ.items()},
        "call_target_continuations": {format(k, "06x"): [format(c, "06x") for c in sorted(v)] for k, v in cont.items()},
        "roots": [format(r, "06x") for r in roots],
        "no_fallthrough": [format(n, "06x") for n in no_fallthrough],
        "pushed_code_addresses": [format(p, "06x") for p in pushed],
    }


def contain(report, site):
    ctx = harvest_module.RegionContext(report)
    start, _ = ctx.sound_start(site)
    if start is None:
        return {"outcome": "unresolved", "reason": "no_sound_starting_scope"}
    return harvest_module.attempt_containment(site, start, report["call_target_continuations"], ctx)


def epilogue_layout():
    # f1 @0x100 (call target of c1@0x10), f2 @0x200 (call target of c2@0x20); both end with a `bra E`;
    # E @0x300 is the shared epilogue: 0x300 -> 0x302 (`rts`, the site). c1/c2 are roots' callers (undiscovered).
    nodes = {0x100: 2, 0x102: 2, 0x104: 2, 0x200: 2, 0x202: 2, 0x300: 2, 0x302: 2, 0x10: 2, 0x20: 2}
    edges = {0x104: [0x300], 0x202: [0x300]}
    return nodes, edges, {0x10, 0x20}, {0x104, 0x202, 0x302, 0x10 - 0, 0x20 - 0} - {0x10, 0x20}


def test_rts_entry_accounting() -> None:
    nodes, edges, roots, nofall = epilogue_layout()
    calls = {0x10: 0x100, 0x20: 0x200}
    # shared epilogue with two valid callers: contained = union of both callers' continuations
    got = contain(synth_report(nodes, edges, roots, nofall | {0x302}, calls), 0x302)
    expect(got["outcome"] == "contained" and got["targets"] == [0x12, 0x22], f"shared epilogue: {got}")
    # the SEG-042 'callers' rule cannot see branch-entered epilogues at all
    rep = synth_report(nodes, edges, roots, nofall | {0x302}, calls)
    ctx = harvest_module.RegionContext(rep)
    expect(harvest_module.attempt_containment(0x302, 0x300, rep["call_target_continuations"], ctx, "callers")["outcome"] == "unresolved",
           "callers model must stay unresolved on a branch-entered epilogue")
    # branch-entered epilogue with a single branching function
    one = {k: v for k, v in edges.items() if k != 0x202}
    got = contain(synth_report(nodes, one, roots, nofall | {0x302}, {0x10: 0x100}), 0x302)
    expect(got["outcome"] == "contained" and got["targets"] == [0x12], f"branch-entered epilogue: {got}")
    # unaccounted external entry: a node entering the epilogue that is not itself provably entered (no preds)
    bad_nodes = dict(nodes); bad_nodes[0x400] = 2
    bad_edges = dict(edges); bad_edges[0x400] = [0x300]
    got = contain(synth_report(bad_nodes, bad_edges, roots, nofall | {0x302, 0x400}, calls), 0x302)
    expect(got["outcome"] == "unresolved", f"unaccounted external entry must not be contained: {got}")
    # the same entering node being a program root (stack is not a call frame) is also rejected
    got = contain(synth_report(bad_nodes, bad_edges, roots | {0x400}, nofall | {0x302, 0x400}, calls), 0x302)
    expect(got["outcome"] == "unresolved", f"root entry into the epilogue must not be contained: {got}")
    # manually manipulated return slot: the entry is a statically pushed code address (PEA ... RTS)
    got = contain(synth_report(nodes, edges, roots, nofall | {0x302}, calls, pushed=[0x300]), 0x302)
    expect(got["outcome"] == "unresolved", f"pushed-address entry must not be contained: {got}")
    # layout fall-in is a real entry: f1 (@0x100) runs into g (@0x106, also a call target of c2@0x20)
    fl_nodes = {0x100: 2, 0x102: 2, 0x104: 2, 0x106: 2, 0x108: 2, 0x10: 2, 0x20: 2}
    fl_calls = {0x10: 0x100, 0x20: 0x106}
    got = contain(synth_report(fl_nodes, {}, {0x10, 0x20}, {0x108}, fl_calls), 0x108)
    expect(got["outcome"] == "contained" and got["targets"] == [0x12, 0x22],
           f"a falling-through predecessor donates its frame to the next entry: {got}")
    got = contain(synth_report(fl_nodes, {}, {0x10, 0x20}, {0x104, 0x108}, fl_calls), 0x108)
    expect(got["outcome"] == "contained" and got["targets"] == [0x22],
           f"a terminating predecessor must not donate its frame: {got}")
    # a function whose last instruction does NOT fall through must not donate its frame to the next function
    plain = synth_report({0x100: 2, 0x102: 2, 0x104: 2, 0x10: 2}, {}, {0x10}, {0x102 + 0, 0x104}, {0x10: 0x100})
    got = contain(plain, 0x102)
    expect(got["outcome"] == "contained" and got["targets"] == [0x12], f"plain callee: {got}")
    # a no_fallthrough instruction directly before the entry contributes no layout predecessor
    adj = synth_report({0x100: 2, 0x102: 2, 0x10: 2, 0xfe: 2}, {}, {0x10}, {0x102, 0xfe}, {0x10: 0x100})
    got = contain(adj, 0x102)
    expect(got["outcome"] == "contained" and got["targets"] == [0x12], f"adjacent terminator: {got}")
    # mutation: adding one more unaccounted entry can only turn contained into unresolved, never shrink the set
    mut_edges = dict(edges); mut_edges[0x500] = [0x300]
    mut_nodes = dict(nodes); mut_nodes[0x500] = 2
    got = contain(synth_report(mut_nodes, mut_edges, roots, nofall | {0x302, 0x500}, calls), 0x302)
    expect(got["outcome"] == "unresolved", f"mutation adding an unaccounted entry must be rejected: {got}")
    # cyclic entry chain: two blocks that enter each other by branch, neither reached by a call
    cyc = synth_report({0x100: 2, 0x102: 2, 0x200: 2, 0x202: 2}, {0x102: [0x200], 0x202: [0x100]}, set(), {0x102, 0x202}, {})
    got = contain(cyc, 0x202)
    expect(got["outcome"] == "unresolved", f"cyclic chain must not be contained: {got}")


def test_rts_entry_depth_bound() -> None:
    # a chain of N branch-entered blocks, the first a call target: contained when within the bound, Unknown beyond
    def chain(n):
        nodes = {0x10: 2}
        edges = {}
        for i in range(n):
            nodes[0x100 + 0x10 * i] = 2
            nodes[0x102 + 0x10 * i] = 2
            if i:
                edges[0x102 + 0x10 * (i - 1)] = [0x100 + 0x10 * i]
        nodes_nf = {0x102 + 0x10 * i for i in range(n)}
        return synth_report(nodes, edges, {0x10}, nodes_nf, {0x10: 0x100}), 0x102 + 0x10 * (n - 1)
    rep, site = chain(3)
    got = contain(rep, site)
    expect(got["outcome"] == "contained" and got["targets"] == [0x12], f"short chain: {got}")
    rep, site = chain(harvest_module.MAX_ENTRY_CHAIN + 3)
    got = contain(rep, site)
    expect(got["outcome"] == "unresolved" and got["reason"] == "entry_chain_too_deep", f"long chain must be Unknown: {got}")



def test_write_facts_format() -> None:
    results = [
        {"pc": 0x300, "outcome": "exact", "targets": [0x400, 0x500]},
        {"pc": 0x200, "outcome": "contained", "targets": [0x206, 0x20c]},
        {"pc": 0x900, "outcome": "unresolved", "reason": "no_sound_starting_scope"},
    ]
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / "facts.txt"
        written = harvest_module.write_facts(results, "a" * 64, "test-producer", str(out))
        expect(written == 2, f"write_facts: must write exactly the exact+contained results, wrote {written}")
        text = out.read_text(encoding="ascii")
    lines = text.splitlines()
    expect(lines[0] == "segarecomp.m68k_external_facts.v1", f"write_facts: wrong schema line {lines[0]!r}")
    expect(lines[1] == "rom_sha256 " + "a" * 64, f"write_facts: wrong rom_sha256 line {lines[1]!r}")
    expect(lines[2] == "producer test-producer", f"write_facts: wrong producer line {lines[2]!r}")
    # Strictly ascending by PC (the format's own requirement: 0x200 before 0x300) and ascending entries.
    expect(lines[3] == "fact 00000200 contained 00000206,0000020c", f"write_facts: unexpected fact line {lines[3]!r}")
    expect(lines[4] == "fact 00000300 exact 00000400,00000500", f"write_facts: unexpected fact line {lines[4]!r}")
    expect(lines[5] == "end", f"write_facts: missing trailing end line, got {lines[5]!r}")


def w(*words: int) -> bytes:
    out = b""
    for word in words:
        out += struct.pack(">H", word & 0xFFFF)
    return out


def make_rom(code: bytes, entry: int = 0x400, size: int = 0x4000) -> bytes:
    data = bytearray(size)
    struct.pack_into(">I", data, 0x0, 0x00FFFE00)
    struct.pack_into(">I", data, 0x4, entry)
    data[entry:entry + len(code)] = code
    return bytes(data)


def movea_l_imm(a: int, value: int) -> bytes:
    return w(0x207C | (a << 9)) + struct.pack(">I", value)


def test_end_to_end_exact_and_contained(classifier_path: str) -> None:
    """One `jmp (a0)` exact site (entry2) reachable from a single root, and one `rts_computed` site
    (callee) reachable from two static call sites in a separate function -- a hand-built report dict
    (mirroring exactly what the real C++ tool emits) drives the real angr exploration and the real native
    classifier binary end to end, with no ROM/report generation shortcuts."""
    entry = 0x400
    callee = 0x440
    entry2 = 0x480

    jsr_callee = w(0x4EB9) + struct.pack(">I", callee)  # jsr.l callee (6 bytes)
    code_at_entry = jsr_callee + jsr_callee + w(0x4E71)  # two call sites, then a NOP (halts discovery cleanly)
    call_site_1 = entry
    call_site_2 = entry + 6
    continuation_1 = call_site_1 + 6
    continuation_2 = call_site_2 + 6

    code_at_callee = w(0x4E75)  # rts
    code_at_entry2 = movea_l_imm(0, 0x4C0) + w(0x4ED0)  # movea.l #0x4C0,a0 ; jmp (a0)
    jmp_site = entry2 + len(movea_l_imm(0, 0x4C0))

    rom = bytearray(make_rom(code_at_entry, entry=entry))
    rom[callee:callee + len(code_at_callee)] = code_at_callee
    rom[entry2:entry2 + len(code_at_entry2)] = code_at_entry2
    rom[0x4C0:0x4C0 + 2] = w(0x60FE)  # bra.s * at the jmp's landing pad (a harmless, decodable halt)

    with tempfile.TemporaryDirectory() as tmp:
        rom_path = pathlib.Path(tmp) / "synth.bin"
        rom_path.write_bytes(bytes(rom))

        report = {
            "roots": [f"{entry:06x}", f"{entry2:06x}"],
            "discovered_lengths": {
                f"{call_site_1:06x}": 6, f"{call_site_2:06x}": 6, f"{continuation_2:06x}": 2,
                f"{callee:06x}": 2,
                f"{entry2:06x}": 6, f"{jmp_site:06x}": 2,
            },
            "call_target_continuations": {f"{callee:06x}": [f"{continuation_1:06x}", f"{continuation_2:06x}"]},
            "static_successors": {f"{call_site_1:06x}": [f"{callee:06x}"], f"{call_site_2:06x}": [f"{callee:06x}"]},
            "computed_sites": {
                f"{callee + len(code_at_callee) - 2:06x}": {"family": "rts_computed", "outcome": "unknown"},
                f"{jmp_site:06x}": {"family": "jmp_an", "outcome": "unknown"},
            },
        }

        results = harvest_module.harvest(report, str(rom_path), classifier_path, max_entries=64, max_steps=10_000)
        by_pc = {r["pc"]: r for r in results}

        rts_pc = callee + len(code_at_callee) - 2
        expect(by_pc[rts_pc]["outcome"] == "contained", f"E2E: rts_computed site must be contained, got {by_pc[rts_pc]}")
        expect(by_pc[rts_pc].get("targets") == sorted([continuation_1, continuation_2]),
               f"E2E: contained entries must be both call continuations, got {by_pc[rts_pc].get('targets')}")

        expect(by_pc[jmp_site]["outcome"] == "exact", f"E2E: jmp (a0) site must be exact, got {by_pc[jmp_site]}")
        expect(by_pc[jmp_site].get("targets") == [0x4C0], f"E2E: exact target must be 0x4C0, got {by_pc[jmp_site].get('targets')}")

        out = pathlib.Path(tmp) / "facts.txt"
        written = harvest_module.write_facts(results, "0" * 64, "test-producer", str(out))
        expect(written == 2, f"E2E: both facts must be written, wrote {written}")


def main() -> int:
    test_eligible_sites_excludes_rte_and_unclassified()
    test_backward_walk_and_external_entry_check()
    test_gap_termination_point_is_a_sound_candidate()
    test_cfg_region_adversarial_cases()
    test_cfg_region_matches_bruteforce_dominance()
    test_interrupt_unproven_sites_never_get_external_exact_facts()
    test_rts_entry_accounting()
    test_rts_entry_depth_bound()
    test_write_facts_format()

    classifier_path = sys.argv[1] if len(sys.argv) > 1 else None
    if not classifier_path or not pathlib.Path(classifier_path).exists():
        print("segarecomp-m68k-primary-word-classify binary not given/found: end-to-end test SKIPPED "
              "(pure-logic tests above still ran)")
    else:
        try:
            import angr  # noqa: F401
            from archinfo import ArchPcode
            ArchPcode("68000:BE:32:CPU32")
        except Exception as exc:  # noqa: BLE001
            print(f"angr (or its M68K p-code support) unavailable in this interpreter ({exc!r}): "
                  f"end-to-end test SKIPPED (pure-logic tests above still ran)")
        else:
            test_end_to_end_exact_and_contained(classifier_path)

    if failures:
        print(f"{failures} failure(s)", file=sys.stderr)
        return 1
    print("segarecomp_recomp_map_harvest_test: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
