"""Structural build-shape budget of an emitted Z80/SMS image (SEG-033-T006).

One predicate shared by the production gate (`sms_emission_budget_test.py`) and its negative controls
(`z80_owner_group_mutation_test.py`): given what the emitter reports and what is on disk, return every violated invariant.
Host-independent by construction: counts and sizes, never time.
"""

MAX_GROUP_ENTRIES = 128
MIN_ENTRIES_PER_OWNER = 100


def violations(shape, budget, largest_tu_mib, total_mib, defined_owners):
    """`shape`: the emitter's `shape` line as ints (entries, owners, max_group, shared_bodies); `budget`: the per-size budget
    (entries, owners, functions, max_tu_mib, total_mib); `defined_owners`: owner functions found in the C."""
    out = []
    if shape["entries"] != budget["entries"]:
        out.append("exact entry count changed: %d, expected %d (every start keeps its entry)" % (shape["entries"], budget["entries"]))
    if shape["owners"] > budget["owners"]:
        out.append("host owner count %d exceeds %d (owner grouping regressed)" % (shape["owners"], budget["owners"]))
    if shape["max_group"] > MAX_GROUP_ENTRIES:
        out.append("an owner holds %d entries (bound %d)" % (shape["max_group"], MAX_GROUP_ENTRIES))
    if shape["entries"] / max(1, shape["owners"]) < MIN_ENTRIES_PER_OWNER:
        out.append("owner grouping ratio below %d entries per owner" % MIN_ENTRIES_PER_OWNER)
    functions = shape["owners"] + shape["shared_bodies"]
    if functions > budget["functions"]:
        out.append("host function count %d exceeds %d" % (functions, budget["functions"]))
    if largest_tu_mib > budget["max_tu_mib"]:
        out.append("largest translation unit %.1f MiB exceeds %.1f MiB" % (largest_tu_mib, budget["max_tu_mib"]))
    if total_mib > budget["total_mib"]:
        out.append("generated C %.1f MiB exceeds the %.1f MiB shape budget" % (total_mib, budget["total_mib"]))
    if defined_owners != shape["owners"]:
        out.append("the C defines %d owner functions, the emitter reported %d" % (defined_owners, shape["owners"]))
    return out
