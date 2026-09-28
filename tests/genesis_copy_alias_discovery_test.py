#!/usr/bin/env python3
"""SEG-021-T041 / ADR 0049: the pure derivation of an immutable-copy alias proposal (tool side).

Project-authored bytes only. `derive_copy_alias` proposes (execution, source, length) for the maximal verbatim
run of the ROM image found around a work-RAM stop PC; `merge_copy_aliases` unions same-delta neighbours. Both are
proposals only: the emitter re-validates them and every alias body carries the runtime byte-identity guard.
"""
import pathlib
import random
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import genesis_startup_bridge as b  # noqa: E402

random.seed(7)
rom = bytes(random.randrange(256) for _ in range(0x4000))
ram_begin = b.GENESIS_WORK_RAM_BEGIN

# A verbatim 0x60-byte copy of rom[0x1230:0x1290] at RAM offset 0x0400, surrounded by unrelated bytes.
ram = bytearray(random.randrange(256) for _ in range(b.GENESIS_WORK_RAM_SIZE))
ram[0x0400:0x0460] = rom[0x1230:0x1290]
ram = bytes(ram)

# Stop at the copy start: the proposal is exactly the run.
assert b.derive_copy_alias(rom, ram, ram_begin + 0x0400) == (ram_begin + 0x0400, 0x1230, 0x60)
# Stop in the middle of the copy: the run is extended backward and forward with a constant delta.
assert b.derive_copy_alias(rom, ram, ram_begin + 0x0420) == (ram_begin + 0x0400, 0x1230, 0x60)
# An odd PC, a PC outside work RAM, and a stop with no verbatim run (random RAM) yield no proposal.
assert b.derive_copy_alias(rom, ram, ram_begin + 0x0401) is None
assert b.derive_copy_alias(rom, ram, 0x00001000) is None
assert b.derive_copy_alias(rom, ram, ram_begin + 0x2000) is None
# A run shorter than the minimum is not proposed (avoids aliasing incidental matches).
short = bytearray(ram)
short[0x0810:0x0810 + b.GENESIS_ALIAS_MIN_RUN - 2] = rom[0x0100:0x0100 + b.GENESIS_ALIAS_MIN_RUN - 2]
assert b.derive_copy_alias(rom, bytes(short), ram_begin + 0x0810) is None

# Merge: overlapping/adjacent aliases with the same delta union; a different delta stays separate.
delta = ram_begin + 0x0400 - 0x1230
merged = b.merge_copy_aliases([(ram_begin + 0x0400, 0x1230, 0x20), (ram_begin + 0x0420, 0x1250, 0x40),
                               (ram_begin + 0x0800, 0x0100, 0x10)])
assert merged == [(ram_begin + 0x0400, 0x1230, 0x60), (ram_begin + 0x0800, 0x0100, 0x10)], merged
assert b.merge_copy_aliases([]) == []
assert b.merge_copy_aliases([(ram_begin + 0x10, 0x20, 0x8), (ram_begin + 0x40, 0x50, 0x8)]) == \
    [(ram_begin + 0x10, 0x20, 0x8), (ram_begin + 0x40, 0x50, 0x8)]
print("genesis_copy_alias_discovery_test: OK")
