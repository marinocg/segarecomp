#!/usr/bin/env python3
"""SEG-137-T002: independent reference for the deterministic math.random of the Luau policy (section 3): seed = first 8 bytes (LE) of SHA-256(identity || frame u64 LE || callback index u32 LE), then
splitmix64. The golden values compiled into tests/luau_host_test.cpp must be exactly what this implementation produces. usage: luau_host_golden_test.py"""
import hashlib
import math
import re
import struct
from pathlib import Path

M = (1 << 64) - 1


def sequence(identity_hex, frame, index):
    digest = hashlib.sha256(bytes.fromhex(identity_hex) + struct.pack("<Q", frame) + struct.pack("<I", index)).digest()
    state = int.from_bytes(digest[:8], "little")

    def next64():
        nonlocal state
        state = (state + 0x9E3779B97F4A7C15) & M
        z = state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M
        return z ^ (z >> 31)

    def unit():
        return (next64() >> 11) / 2 ** 53

    return [math.floor(unit() * 100) + 1, math.floor(unit() * 100) + 1, math.floor(unit() * 100) + 1, math.floor(unit() * 10) + 1, unit()]


source = (Path(__file__).parent / "luau_host_test.cpp").read_text()
IDENT = "ab" * 32
failures = []
for frame, index in ((7, 3), (8, 3), (7, 4)):
    a, b, c, d, e = sequence(IDENT, frame, index)
    want = f'"number:{a}" && vm->describe_global("b") == "number:{b}" && vm->describe_global("c") == "number:{c}" && vm->describe_global("d") == "number:{d}"'
    if want not in source:
        failures.append(f"golden values for frame {frame} callback {index}: {a},{b},{c},{d} not in luau_host_test.cpp")
e = sequence(IDENT, 7, 3)[4]
if repr(e) not in source:
    failures.append(f"golden float {e!r} not in luau_host_test.cpp")
# the stream is a function of (identity, frame, callback): the three seeds differ
seeds = {tuple(sequence(IDENT, f, i)) for f, i in ((7, 3), (8, 3), (7, 4), (7, 3))}
if len(seeds) != 3:
    failures.append("distinct (frame, callback) pairs must give distinct streams")
if failures:
    raise SystemExit("luau_host_golden_test: " + "; ".join(failures))
print("luau_host_golden_test: ok")
