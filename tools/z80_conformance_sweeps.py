"""Deterministic value sweeps for the Z80 conformance rows (SEG-008-T004).

A vector-table row may carry `"sweep": "<name>"` and `"sweep_v": "reg|imm|mem|half"`. The sweep expands one row into
many initial-state cases; `apply_case` writes a case into a vector's initial state. Nothing here is Z80 semantics: the
values are inputs, the pinned independent oracle decides the outcome.

Equivalence-class argument for the 8-bit ADD/ADC/SUB/SBC/CP sweeps (`alu8:*`). The 8-bit adder/subtractor is two
4-bit stages chained by one carry: the low stage maps (a & 15, v & 15, carry-in) to the low result nibble, the half-carry
bit H and the inter-stage carry k; the high stage maps (a >> 4, v >> 4, k) to the high result nibble, C and the signed
overflow V. S, Y and X are result bits 7, 5 and 3 (one nibble each) and Z is the conjunction of both nibbles being zero.
The sweep therefore contains (1) every (low nibble a, low nibble v, carry-in) combination, (2) every (high nibble a,
high nibble v) combination crossed with low nibbles that produce both values of k, and (3) for every a the operand that
makes the result zero, for each carry-in. That is a complete class sweep of the two stages and of the only cross-stage
predicate (Z). CP additionally takes X/Y from the operand, which is covered because every operand nibble pair appears.
The `alu8s:*` sweeps are the small cross product used on the register/memory/indexed operand classes: the operand class
only changes where the value comes from, never the flag code (one shared lowering function).
"""

V8_SMALL = [0x00, 0x01, 0x0F, 0x10, 0x7F, 0x80, 0xF0, 0xFF]
V8_LOGIC = [0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x7F, 0xFE, 0x55, 0xAA, 0x0F, 0xF0, 0xFF]
V8_VALUE = [0x00, 0x01, 0x0E, 0x0F, 0x10, 0x7E, 0x7F, 0x80, 0x81, 0xFE, 0xFF, 0x55, 0xAA]
V16 = [0x0000, 0x0001, 0x00FF, 0x0100, 0x07FF, 0x0800, 0x0FFF, 0x1000, 0x7FFF, 0x8000, 0x8001, 0xF000, 0xFF00, 0xFFFF,
       0x1234, 0xA5C3]
XYQ_A = [0x00, 0x08, 0x20, 0x28, 0xFF, 0xD7]
XYQ_F = [0x00, 0x01, 0x28, 0x29, 0xFF, 0xD7, 0xC4]
XYQ_Q = [0x00, 0x01, 0x28, 0xFF, 0x29]


def _dedupe(items):
    seen, out = set(), []
    for item in items:
        key = tuple(sorted(item.items()))
        if key not in seen:
            seen.add(key)
            out.append(item)
    return out


def _alu8(kind):
    with_carry = kind in ("adc", "sbc")
    subtract = kind in ("sub", "sbc", "cp")
    carries = [0, 1] if with_carry else [None]
    cases = []
    for c in carries:
        for a in range(16):  # low stage
            for v in range(16):
                cases.append({"a": a, "v": v, "c": c})
        highs_a = [(ah << 4) | al for ah in range(16) for al in (0x0, 0xF)]
        highs_v = [(vh << 4) | vl for vh in range(16) for vl in (0x0, 0x1)]
        for a in highs_a:  # high stage with both inter-stage carries
            for v in highs_v:
                cases.append({"a": a, "v": v, "c": c})
        for a in range(256):  # zero witnesses
            v = ((a - (c or 0)) if subtract else (-a - (c or 0))) & 0xFF
            cases.append({"a": a, "v": v, "c": c})
    return _dedupe(cases)


def _alu8_small(kind):
    carries = [0, 1] if kind in ("adc", "sbc") else [None]
    return [{"a": a, "v": v, "c": c} for c in carries for a in V8_SMALL for v in V8_SMALL]


def sweep_cases(name):
    """The ordered list of case dicts of a named sweep (empty list is never returned)."""
    kind, _, arg = name.partition(":")
    if kind == "alu8":
        return _alu8(arg)
    if kind == "alu8s":
        return _alu8_small(arg)
    if kind == "logic8":
        return [{"a": a, "v": v, "c": None} for a in V8_LOGIC for v in V8_LOGIC]
    if kind == "logic8s":
        return [{"a": a, "v": v, "c": None} for a in V8_SMALL for v in V8_SMALL]
    if kind == "value8s":
        return [{"v": t, "c": c} for t in V8_VALUE for c in (0, 1)]
    if kind == "daa":
        return [{"a": a, "f": f} for a in range(256) for f in (0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13)]
    if kind == "a8":
        return [{"a": a} for a in range(256)]
    if kind == "rot":
        return [{"a": a, "c": c} for a in range(256) for c in (0, 1)]
    if kind == "value8":  # INC/DEC: the operand value with the carry flag alternating (it must be preserved)
        return [{"v": t, "c": (t >> 3) & 1} for t in range(256)]
    if kind == "xyq":  # SCF/CCF: X/Y = ((Q xor F) or A)
        return [{"a": a, "f": f, "q": q} for a in XYQ_A for f in XYQ_F for q in XYQ_Q]
    if kind == "add16":
        return [{"x16": x, "y16": y, "c": 0 if arg == "add" else c} for x in V16 for y in V16
                for c in ((0,) if arg == "add" else (0, 1))]
    raise SystemExit("unknown sweep %r" % name)


def apply_case(case, state, mode):
    """Writes a sweep case into `state` (the profile state); returns the memory byte to fill around the touch points
    (mode `mem`) or None. The immediate mode is applied by the caller."""
    mem = None
    if case.get("a") is not None:
        state["a"] = case["a"]
    if case.get("f") is not None:
        state["f"] = case["f"]
    if case.get("q") is not None:
        state["q"] = case["q"]
    if case.get("c") is not None:
        state["f"] = (state["f"] & 0xFE) | case["c"]
    v = case.get("v")
    if v is not None:
        if mode == "reg":
            for reg in "bcdehl":
                state[reg] = v
            if case.get("a") is None:  # INC/DEC A and ALU forms that name A as the source
                state["a"] = v
        elif mode == "imm":
            pass
        elif mode == "mem":
            mem = v
        elif mode == "half":
            state["ix"] = state["iy"] = (v << 8) | v
        else:
            raise SystemExit("unknown sweep_v %r" % mode)
    if case.get("x16") is not None:
        x, y = case["x16"], case["y16"]
        state["h"], state["l"], state["ix"], state["iy"] = x >> 8, x & 0xFF, x, x
        state["b"], state["c"], state["d"], state["e"], state["sp"] = y >> 8, y & 0xFF, y >> 8, y & 0xFF, y
    return mem


def case_tag(case):
    return ".".join("%s%X" % (k, v) for k, v in sorted(case.items()) if v is not None)
