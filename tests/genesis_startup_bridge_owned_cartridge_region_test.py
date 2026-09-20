#!/usr/bin/env python3
"""SEG-007-T077: generic immutable/generated cartridge-data region ownership.

Drives the real production compile-and-run contract (tools/genesis_startup_
bridge.py), never a private test-only reimplementation, proving genuine
bounds-checked runtime data access into a statically owned, generated
cartridge-data region -- distinct from T075's narrower scalar constant-fold
precedent, which requires the exact effective address to be known at
translation time.

Positive fixture: `LEA (addr).L,A1` loads A1 with a literal base address, then
two `MOVE.L (A1)+,Dn` instructions each read through A1 and postincrement it.
Unlike a LEA immediately followed by a MOVEM memory_to_registers transfer (the
one shape SEG-007-T075's own adjacent-LEA fold recognizes), a MOVE consumer is
never folded by that mechanism -- `retain_fact` never records a static memory
fact for a postincrement source EA at all (`m68k_is_statically_foldable_
control_ea` excludes it), so both reads are unconditionally routed through
`genesis_route_access` with a genuinely runtime-computed address (`runtime->
a[1]`, mutated by the CPU's own postincrement effect between the two reads,
not by any C4-time formula). This is the exact "runtime-variable, non-
constant-foldable effective address" shape this task's own Acceptance
requires: the two reads land at two distinct addresses (the base, and the
base plus four), each independently and correctly resolved against the
generated backing data for the sole `raw_cartridge_rom` mapping claim the
bridge route constructs for this ROM.

Negative fixture: byte-for-byte identical in shape, except the ROM image ends
exactly four bytes earlier, so the *first* read (still fully in-bounds) still
succeeds, but the *second* read's postincremented address lands exactly one
byte past the mapping claim's own end -- proving the bounds check is exact,
not approximate, even with a nearby in-region read succeeding in the same
execution.
"""
import json
import pathlib
import subprocess
import sys
import tempfile


ENTRY = "00000b00"


def lea_abs_l_a1(address: int) -> bytes:
    return bytes((0x43, 0xF9, (address >> 24) & 0xFF, (address >> 16) & 0xFF,
                  (address >> 8) & 0xFF, address & 0xFF))


def move_l_a1_postinc_to(destination_register: int) -> bytes:
    word = (0b00 << 14) | (0b10 << 12) | (destination_register << 9) | (0b000 << 6) | (0b011 << 3) | 1
    return bytes(((word >> 8) & 0xFF, word & 0xFF))


RESET = bytes((0x4E, 0x70))
DATA0 = bytes((0x12, 0x34, 0x56, 0x78))
DATA1 = bytes((0x9A, 0xBC, 0xDE, 0xF0))
PREFIX = lea_abs_l_a1(0x00000B0C) + move_l_a1_postinc_to(0) + move_l_a1_postinc_to(1) + RESET

# Positive: both postincrement reads (0x00000B0C, then 0x00000B10) land
# strictly inside the sole `raw_cartridge_rom` claim ([0x00000B00,
# 0x00000B14)), so both succeed against the generated backing data.
POSITIVE_IMAGE = PREFIX + DATA0 + DATA1
# Negative: the ROM (and therefore the claim) ends four bytes earlier
# ([0x00000B00, 0x00000B10)); the first read (0x00000B0C, width 4) still
# lands exactly at the claim's own upper boundary and succeeds, but the
# second read (0x00000B10) is one byte past the end and must fail closed.
NEGATIVE_IMAGE = PREFIX + DATA0


def run_once(driver: pathlib.Path, binary: pathlib.Path, compiler: pathlib.Path, rom: pathlib.Path,
             root: pathlib.Path, out_dir: pathlib.Path, full_path: pathlib.Path) -> tuple[dict, dict, str]:
    result = subprocess.run(
        [sys.executable, str(driver), "--segarecomp", str(binary), "--cc", str(compiler),
         "--rom", str(rom), "--entry", ENTRY, "--mode", "synthetic", "--out-dir", str(out_dir),
         "--full-report-path", str(full_path)],
        text=True, capture_output=True, cwd=root)
    if result.returncode != 0:
        raise RuntimeError(f"driver failed ({result.returncode}): {result.stderr}")
    sanitized = json.loads(result.stdout)
    full = json.loads(full_path.read_text())
    source = (out_dir / "bridge.generated.c").read_text()
    return sanitized, full, source


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    binary, compiler, root = (pathlib.Path(value).resolve() for value in sys.argv[1:])
    driver = root / "tools" / "genesis_startup_bridge.py"

    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)
        positive_rom = work / "owned-region-positive.bin"
        positive_rom.write_bytes(POSITIVE_IMAGE)
        negative_rom = work / "owned-region-negative.bin"
        negative_rom.write_bytes(NEGATIVE_IMAGE)

        # Positive: two independent end-to-end runs (own compile, own output
        # directory), proving determinism the same way genesis_startup_
        # bridge_c4_test.py's own two-independent-invocation pattern does.
        runs = []
        for index in range(2):
            with tempfile.TemporaryDirectory(dir=root / "build", prefix="c4-owned-region-positive-") as out:
                out_dir = pathlib.Path(out)
                full_path = work / f"positive-full-{index}.json"
                runs.append(run_once(driver, binary, compiler, positive_rom, root, out_dir, full_path))

        (first_sanitized, first_full, first_source), (second_sanitized, second_full, second_source) = runs
        require(first_sanitized == second_sanitized and first_full == second_full and
                first_source == second_source, "positive fixture: nondeterministic across runs")

        expected_sanitized = {
            "schema_version": 1, "report_kind": "sanitized", "result": "stop",
            "stop_class": "unsupported_cpu_form",
            "diagnostic_category": "valid_but_unsupported_instruction",
            "cpu_dimensions": {"family": "reset", "size": "none", "addressing_mode_class": "implied"},
            "c4_lowering_dimensions": None,
        }
        require(all(first_sanitized.get(key) == value for key, value in expected_sanitized.items()) and
                len(first_sanitized.get("rom_sha256", "")) == 64,
                f"positive fixture: unexpected sanitized report {first_sanitized}")

        runtime = first_full.get("runtime", {})
        # Each MOVE.L (A1)+,Dn independently and correctly resolved its own
        # runtime-computed address (the base, then the base plus four) against
        # the generated backing data for the sole owned cartridge-data region:
        # two distinct addresses, two distinct correctly-decoded values, both
        # reached by the same two static instructions relying purely on the
        # CPU's own runtime postincrement effect -- never a C4-time formula.
        require(runtime.get("d", [None, None])[0] == "0x12345678", f"positive fixture: D0 {runtime.get('d')}")
        require(runtime.get("d", [None, None])[1] == "0x9abcdef0", f"positive fixture: D1 {runtime.get('d')}")
        # A1 is fully postincremented across both reads: 0x00000B0C + 4 + 4.
        require(runtime.get("a", [None] * 8)[1] == "0x00000b14", f"positive fixture: A1 {runtime.get('a')}")

        # Belt-and-suspenders: this is genuine runtime routing (never a
        # disguised/broadened scalar fold) through a runtime-computed
        # address bound from A1 (never a compile-time literal) -- that the
        # new generic mechanism's own generated backing data actually served.
        # SEG-007-T105: each runtime-computed A1-derived address is bound once
        # through the MC68000 24-bit external-address-bus truncation seam
        # (`m68k_move_src_ea & 0x00FFFFFF`) and that single bus-address local is
        # what reaches genesis_route_access -- still genuine runtime routing, not
        # a compile-time fold.
        require(first_source.count("const uint32_t m68k_routed_addr_0 = (m68k_move_src_ea) & UINT32_C(0x00FFFFFF);") == 2 and
                first_source.count("genesis_route_access(runtime, m68k_routed_addr_0,") == 2,
                "positive fixture: expected two runtime-routed reads through a runtime-computed A1-derived address")
        require("GenesisOwnedCartridgeRegion genesis_owned_cartridge_regions[]" in first_source and
                "runtime.owned_regions = genesis_owned_cartridge_regions;" in first_source and
                "runtime.owned_region_count = UINT32_C(1);" in first_source,
                "positive fixture: missing generated owned-cartridge-region emission")

        # Negative: the second read's runtime-computed address (0x00000B10)
        # falls exactly one byte past the sole claim's own upper bound, even
        # though the first read (0x00000B0C, the same width, four bytes
        # earlier) is still fully in-bounds and succeeds in the same
        # execution -- proving the bounds check is exact, not approximate.
        with tempfile.TemporaryDirectory(dir=root / "build", prefix="c4-owned-region-negative-") as out:
            out_dir = pathlib.Path(out)
            full_path = work / "negative-full.json"
            negative_sanitized, negative_full, negative_source = run_once(
                driver, binary, compiler, negative_rom, root, out_dir, full_path)

        expected_negative_sanitized = {
            "schema_version": 1, "report_kind": "sanitized", "result": "stop",
            "stop_class": "internal_dispatch_inconsistency",
            "diagnostic_category": "internal_dispatch_inconsistency",
            "cpu_dimensions": None,
            "c4_lowering_dimensions": None,
        }
        require(all(negative_sanitized.get(key) == value for key, value in expected_negative_sanitized.items()),
                f"negative fixture: unexpected sanitized report {negative_sanitized}")
        negative_runtime = negative_full.get("runtime", {})
        # The first (in-bounds) read still completed and wrote D0 before the
        # second (out-of-bounds) read failed; D1 was never written, and A1
        # holds exactly the address that failed to resolve.
        require(negative_runtime.get("d", [None, None])[0] == "0x12345678",
                f"negative fixture: D0 {negative_runtime.get('d')}")
        require(negative_runtime.get("d", [None, None])[1] == "0x00000000",
                f"negative fixture: D1 {negative_runtime.get('d')}")
        require(negative_runtime.get("a", [None] * 8)[1] == "0x00000b10",
                f"negative fixture: A1 {negative_runtime.get('a')}")
        require(negative_source.count("const uint32_t m68k_routed_addr_0 = (m68k_move_src_ea) & UINT32_C(0x00FFFFFF);") == 2 and
                negative_source.count("genesis_route_access(runtime, m68k_routed_addr_0,") == 2,
                "negative fixture: expected two runtime-routed reads through a runtime-computed A1-derived address")

    print("genesis_startup_bridge_owned_cartridge_region_test: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
