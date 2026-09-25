"""SEG-022-T009 test helper: resolve the compact compiled-entry tables to legacy-style row strings."""
import re


def _body(text, name):
    match = re.search(r"\b" + name + r"\[\] = \{\n(.*?)\n\};", text, re.S)
    return match.group(1) if match else ""


def rows(text):
    """Text of `{ UINT32_C(0xADDR), owner }` lines, one per compiled entry, for substring assertions."""
    addresses = re.findall(r"UINT32_C\(0x[0-9A-Fa-f]{8}\)", _body(text, "genesis_compiled_entry_addresses"))
    ids = [int(i) for i in re.findall(r"UINT(?:8|16|32)_C\((\d+)\)", _body(text, "genesis_compiled_entry_owner_ids"))]
    owners = [o.strip().rstrip(",") for o in _body(text, "genesis_compiled_owners").splitlines()]
    return "\n".join("{ %s, %s }" % (a, owners[i]) for a, i in zip(addresses, ids))
