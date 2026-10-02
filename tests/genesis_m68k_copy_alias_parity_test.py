#!/usr/bin/env python3
"""SEG-028-T005: the C++ consumer-route alias derivation/merge (platforms/genesis/machine m68k_copy_alias) is result-identical to
tools/genesis_startup_bridge.py `derive_copy_alias` / `merge_copy_aliases` (the tooling-route reference).

Synthetic inputs only (seeded pseudo-random bytes, no image): several occurrences of the run (the longest wins, ties go to the
lowest), odd occurrences, back-extension by words, below-minimum runs, a run reaching the end of work RAM or of the image, odd and
non-work-RAM PCs, and merges of overlapping, adjacent, disjoint and different-delta aliases.

usage: genesis_m68k_copy_alias_parity_test.py <genesis_m68k_copy_alias_test> <source-root>
"""
import pathlib
import random
import subprocess
import sys
import tempfile

probe, root = sys.argv[1], pathlib.Path(sys.argv[2]).resolve()
sys.path.insert(0, str(root))
from tools import genesis_startup_bridge as b  # noqa: E402

WR = b.GENESIS_WORK_RAM_BEGIN


def derive_cases(rng):
    """(rom, ram, pc) tuples."""
    cases = []
    for n in range(150):
        rom = bytearray(rng.randrange(256) for _ in range(rng.choice((0x200, 0x800, 0x1000))))
        ram = bytearray(rng.randrange(4) for _ in range(b.GENESIS_WORK_RAM_SIZE))  # low entropy: partial matches happen
        length = rng.choice((8, 15, 16, 17, 31, 64, 200, 64, 200))
        source = rng.randrange(0, len(rom) - length) & ~1 | (1 if n % 4 == 1 else 0)  # every fourth case: an odd occurrence
        execution = rng.randrange(0, b.GENESIS_WORK_RAM_SIZE - length) if n % 7 else b.GENESIS_WORK_RAM_SIZE - length  # end of RAM
        ram[execution:execution + length] = rom[source:source + length]
        if n % 5 == 0:  # a second occurrence elsewhere in the image (shorter or longer than the first)
            other = rng.randrange(0, len(rom) - length)
            rom[other:other + length] = rom[source:source + length]
            if n % 10 == 0 and other + length < len(rom) and execution + length < len(ram):
                rom[other + length] = ram[execution + length]  # the second occurrence extends one byte further
        offset = rng.randrange(0, max(1, length - 16)) & ~1 if n % 3 else 0  # mostly inside the run, at least a minimum run left
        pc = WR + execution + offset
        if n % 11 == 0:
            pc |= 1  # odd pc
        if n % 13 == 0:
            pc = execution + offset  # not work RAM
        cases.append((bytes(rom), bytes(ram), pc))
    # A run that ends exactly at the end of the image, preceded by matching words (back-extension).
    rom = bytes(range(256)) * 2
    ram = bytearray(b.GENESIS_WORK_RAM_SIZE)
    ram[0x1000 - 40:0x1000 + 24] = rom[len(rom) - 64:]
    cases.append((rom, bytes(ram), WR + 0x1000))
    cases.append((rom, bytes(ram), WR + 0x1001))
    cases.append((rom, bytes(ram), WR + b.GENESIS_WORK_RAM_SIZE - 8))  # needle shorter than the minimum
    return cases


def merge_cases(rng):
    cases = [[], [(WR, 0x100, 16), (WR + 16, 0x110, 16)], [(WR + 16, 0x110, 16), (WR, 0x100, 32)], [(WR, 0x100, 16), (WR + 18, 0x112, 4)],
             [(WR, 0x100, 16), (WR + 8, 0x200, 16)], [(WR, 0x100, 16), (WR, 0x100, 16)]]
    for _ in range(80):
        aliases = []
        for _ in range(rng.randrange(1, 6)):
            execution = WR + 2 * rng.randrange(0, 64)
            delta = rng.choice((WR - 0x100, WR - 0x300))
            aliases.append((execution, execution - delta, 2 * rng.randrange(1, 24)))
        cases.append(aliases)
    return cases


def main():
    rng = random.Random(0x5E6028)
    failures = 0
    with tempfile.TemporaryDirectory(prefix="segarecomp-alias-parity-") as directory:
        tmp = pathlib.Path(directory)
        lines, expected = [], []
        derives = derive_cases(rng)
        for index, (rom, ram, pc) in enumerate(derives):
            (tmp / ("%d.rom" % index)).write_bytes(rom)
            (tmp / ("%d.ram" % index)).write_bytes(ram)
            lines.append("derive %s %s %x" % (tmp / ("%d.rom" % index), tmp / ("%d.ram" % index), pc))
            result = b.derive_copy_alias(rom, ram, pc)
            expected.append("none" if result is None else "%x:%x:%x" % result)
        merges = merge_cases(rng)
        for aliases in merges:
            lines.append("merge " + " ".join("%x:%x:%x" % a for a in aliases))
            expected.append(" ".join("%x:%x:%x" % a for a in b.merge_copy_aliases(list(aliases))))
        done = subprocess.run([probe, "--probe"], input="\n".join(lines) + "\n", text=True, capture_output=True, timeout=600)
        got = done.stdout.splitlines()
        if done.returncode != 0 or len(got) != len(expected):
            print("FAIL probe exited %d with %d of %d results" % (done.returncode, len(got), len(expected)))
            return 1
        for line, want, have in zip(lines, expected, got):
            if want != have:
                failures += 1
                print("FAIL %s: python=%s c++=%s" % (line.split(" ")[0], want, have))
        found = sum(1 for e in expected[:len(derives)] if e != "none")
        print("%-5s %d derive cases (%d with a proposal) and %d merge cases are result-identical" %
              ("ok" if failures == 0 else "FAIL", len(derives), found, len(merges)))
        if found < 20 or found == len(derives):
            print("FAIL the derive cases must cover both proposals and rejections")
            failures += 1
    print("genesis m68k copy alias parity: %s" % ("ok" if failures == 0 else "FAILED"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
