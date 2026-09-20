#!/usr/bin/env python3
"""Keep the discovery decode/traversal owner independent of scenario policy.

SEG-014-T003: the production static-graph traversal (the recursive DFS +
`switch (decoded.kind)` successor selector) physically moved from the Genesis
scenario adapter's own discover_m68k_general_startup into
discover_m68k_static_graph (libs/cpu/m68k/src/static_discovery.cpp). The frontend
now supplies only a narrow M68kStaticDiscoveryEnvironment fact adapter and
calls into that one CPU-owned traversal; it must never contain a second copy
of the successor-selection switch, and it must never invoke the decode cache
directly (that is now exclusively this CPU boundary's own responsibility).
"""

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]


def main() -> None:
    cpu = (ROOT / "libs/cpu/m68k/src/static_discovery.cpp").read_text(encoding="utf-8")
    header = (ROOT / "libs/cpu/m68k/include/segarecomp/cpu/m68k/static_discovery.hpp").read_text(encoding="utf-8")
    frontend_path = ROOT / "platforms/genesis/machine/src/frontend.cpp"
    frontend = frontend_path.read_text(encoding="utf-8")
    assert not (ROOT / "src/m68k_pipeline_frontend.cpp").exists()
    assert "decode_m68k_instruction" in cpu
    assert "M68kStaticDecodeCache" in header
    assert "discover_m68k_static_graph" in header
    # The CPU boundary itself owns the successor-selection switch.
    assert "switch (decoded.kind)" in cpu
    for forbidden in ("m68k_pipeline.hpp", "Genesis", "controller_io", "genesis_route_access"):
        assert forbidden not in cpu
        assert forbidden not in header
    general_start = frontend.index("FrontendResult discover_m68k_general_startup")
    general_end = frontend.index("namespace {", general_start)
    general = frontend[general_start:general_end]
    # The frontend now delegates the whole traversal to the CPU boundary...
    # ADR-0013 Decision §7 Phase B: the single call site is now made once per
    # seed inside a deterministic aggregation loop (round 1's fixed entry
    # plus any driver-supplied runtime_confirmed seeds), never duplicated.
    # SEG-007-T180 / ADR-0026: the inventory PARTITIONS validation. Phase 1
    # walks each offline code-entry candidate as its own bounded local unit
    # with every other candidate plus the reset entry as an independent unit
    # boundary; Phase 2 walks the reset entry, each admitted unit, and every
    # runtime-confirmed seed with `boundary = admitted_units \\ {own entry}`.
    # Every call still delegates the whole traversal to the one CPU-owned
    # `discover_m68k_static_graph`; the frontend only supplies the boundary
    # set as a narrow fact, never a traversal decision.
    assert "discover_m68k_static_graph(candidate_address, limits, environment, boundary)" in general
    # SEG-007-T181 / ADR-0027: Phase 2 and the continuation-root fixpoint
    # pre-pass pass a SEPARATE 5th-arg fallthrough-continuation boundary set
    # (kept distinct from the ADR-0026 independent-unit boundary set).
    assert "discover_m68k_static_graph(seed_address, limits, environment, boundary, cont_boundary)" in general
    assert "fallthrough-continuation" in general.lower() or "fallthrough_continuation" in general
    # The continuation-root FIXPOINT PRE-PASS mirrors the Phase-1 walk/inspect/
    # discard pattern and fails the build CLOSED at the normalized count ceiling
    # instead of looping or raising any instruction budget.
    assert "m68k_fallthrough_continuation_unit_ceiling" in general
    assert "fallthrough_continuation_frontier" in general
    assert "pre_pass_roots" in general
    # SEG-007-T047 / ADR-0020 Decision §6: one additional call site is the
    # bounded static hardware discovery root for the level-6 interrupt
    # autovector, walked once after the seed loop. It is an asynchronous root,
    # outside ADR-0013 §7's seed set S. SEG-007-T222 adds a separate
    # synchronous exception root; assert their distinct boundary facts and do
    # not conflate the exception with IRQ scheduling semantics.
    assert "discover_m68k_static_graph(handler_address, limits, environment, irq6_boundary," in general
    assert "discover_m68k_static_graph(handler_address, limits, environment, exception_boundary," in general
    assert "synthesized_control_roots" in general
    assert "resolved_control_target_frontier" in general
    # Phase 1 candidate probe, the ADR-0027 fixpoint pre-pass, the Phase-2 seed
    # loop, the IRQ6 asynchronous root, the vector-5 synchronous exception
    # root, (SEG-007-T190 / ADR-0031) the bounded late Tier-1 target
    # representation probe (Stage A) inside the closure fixed point, and
    # (SEG-007-T214 / ADR-0028 §9) the authoritative exact direct-control
    # target closure's own bounded fresh-materialization probe (Phase A's
    # "otherwise" branch -- a target with no decoded facts at all) -- still
    # the one CPU-owned traversal, with the frontend supplying only a boundary
    # fact.
    assert general.count("discover_m68k_static_graph(") == 7
    assert "discover_m68k_static_graph(late_target_address, limits, environment, boundary, cont_boundary)" in general
    assert "discover_m68k_static_graph(target_address, limits, environment, boundary, cont_boundary)" in general
    # ...and therefore no longer invokes the decode cache directly, nor owns
    # any of the CPU-owned recursive successor-selection walk's own state or
    # call/branch-target identity formulas. `general` still legitimately
    # contains build_analysis's own, unrelated, C4 static-memory-fact
    # `switch (decoded.kind)` (a fact-retention pass over an already-built
    # analysis, never a graph-traversal successor selector), so this checks
    # the walk's own distinguishing shapes instead of a bare switch search.
    assert "decode_cache.decode_or_get" not in general
    assert "decode_m68k_instruction(claim_bytes, source, M68kDecodeProfile::general_startup)" not in general
    assert "frame_stack" not in general
    assert "std::function<bool(Address" not in general
    assert "m68k_make_static_call(" not in general
    assert "m68k_make_static_call_edge(" not in general
    assert "m68k_make_static_return_edge(" not in general
    assert "case M68kInstructionKind::rts:" not in general
    assert "case M68kInstructionKind::jsr:" not in general
    assert "case M68kInstructionKind::bsr:" not in general
    print("cpu static-discovery decode/traversal boundary: ok")


if __name__ == "__main__":
    main()
