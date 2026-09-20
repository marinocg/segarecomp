#!/usr/bin/env python3
"""SEG-006-T002 / SEG-007-T018: hash-gated, locally reproducible legacy
``genesis-rom-startup`` fixed-route regression.

This test never depends on committed ROM content and is never required by CI: unless the
operator sets ``SEGARECOMP_SONIC_ROM`` to a local Genesis Sonic the Hedgehog image whose SHA-256
matches the digest pinned by SEG-006-T001
(``docs/references/sonic-first-unsupported-inspection-contract.md``), it prints a skip message and
exits with ``SKIP_EXIT_CODE`` (registered with CTest's ``SKIP_RETURN_CODE`` property) without
touching any local file. CTest then reports this test as *skipped*, not passed, whenever the local
corpus is absent.

When the pinned input is present, it translates that ROM through the exact shared SEG-005/SEG-012
static route (``genesis-rom-startup`` and ``emit-genesis-rom-startup-c``) and asserts that both
commands deterministically fail closed with the sanitized category ``startup_graph_mismatch`` and
a structurally complete rejection report. Each command is run twice and the two runs are required
to produce byte-identical return code, stdout, and stderr, so the finding is verified
deterministic, not merely observed once. It uses only the existing public CLI commands; it adds no
new pipeline stage, decoder, or capability. Because the input fails closed during static analysis,
no structured-C emission ever happens for it, so stdout must remain empty on both commands,
including ``emit-genesis-rom-startup-c``; no Sonic-derived C artifact is ever produced by this
test.

History (SEG-007-T018): this test originally pinned the exact prior category
(``valid_but_unsupported_instruction``) and the exact numeric ``source_address``/``image_offset``
values SEG-006-T001 recorded for this route's boundary. SEG-007-T008 (`done`) later implemented
shared ``TST.L`` absolute-long decode/lift/effect/lowering capability through the common pipeline;
SEG-007-T009 (`done`) only independently validated that implementation and recorded (without
causing) a separate emitter shared-effect ownership defect, later corrected and independently
revalidated by SEG-007-T012/T013 (`done`) -- SEG-007-T009 itself made no production-behavior
change. Once SEG-007-T008 made ``TST.L`` absolute-long capability available to the shared pipeline,
the separate, still-hardcoded ``genesis_rom_startup`` five-operation-sequence graph (unchanged by
T008/T009/T010/T012/T013/T014/T015) began reaching its own ``startup_graph_mismatch`` boundary at
that point instead of the prior unsupported-instruction boundary -- a route reclassification, not a
defect in any of those tasks' own diffs. SEG-007-T016 observed this test's resulting staleness
against the real route as an explicit out-of-scope aside (not a fix). SEG-007-T018 is the
deliberate, independent follow-up that applies the fix: rather than refreshing the old exact-value
pins with newly observed private numeric values (reintroducing exactly the kind of committed
private numeric provenance this milestone's newer hygiene convention avoids), retiring this test
(it remains the only real-input regression coverage for the still-live ``genesis-rom-startup``
route, distinct from SEG-007-T016's separate ``genesis-general-startup`` coverage), or freezing it
indefinitely with a known-stale assertion, this test's assertions are modernized to match the
sanitized category/provenance-presence convention SEG-007-T016 already established for this
milestone.

No ROM byte, opcode/extension word, disassembly, local filesystem path, raw report, or newly
observed numeric address/offset value is ever printed, logged, or written to any committed
artifact by this test. Numeric provenance values are not individually asserted, pinned, printed,
logged, persisted, or committed. They may be present inside the complete ``stderr`` strings
compared opaquely in memory (see ``run_twice``) solely to establish byte-for-byte two-run
determinism. After that determinism check, field-specific assertions inspect only the parsed
report's ``category`` and the *presence*/non-null-ness of a bounded set of non-content provenance
fields -- never the numeric values themselves -- mirroring the provenance-separation policy already
established by SEG-006-T001's inspection contract and SEG-007-T016's own evidence. The SHA-256
check establishes identity against the one pinned digest only; it is not, and does not claim to be,
a determination of legal authorization to use the local file (see
``docs/testing/commercial-games.md`` for that policy).
"""
import hashlib
import json
import os
import pathlib
import subprocess
import sys

# Pinned by SEG-006-T001; see
# docs/references/sonic-first-unsupported-inspection-contract.md.
PINNED_SHA256 = "46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6"

# SEG-007-T018: the fixed genesis_rom_startup graph's current, real deterministic boundary
# category, established once SEG-007-T008 made shared TST.L absolute-long capability available to
# the pipeline (superseding the prior valid_but_unsupported_instruction observation SEG-006-T001
# recorded before that capability existed). See the module docstring's "History" section.
EXPECTED_CATEGORY = "startup_graph_mismatch"

# Bounded, non-content field-presence contract this regression records: a source address and a
# resolved provenance record must be present for this category (both are structural facts about
# the report shape). These numeric values are not individually asserted, pinned, printed, logged,
# persisted, or committed; they may be present inside the complete stderr strings compared opaquely
# in memory solely to establish byte-for-byte two-run determinism (see run_twice), but the
# field-specific checks below inspect only presence/non-null, never the numeric values themselves.
# Matches the convention SEG-007-T016 already established for this milestone.
REQUIRED_NON_NULL_FIELDS = ("source_address", "image_offset", "provenance")

# Matches this test's CMakeLists.txt SKIP_RETURN_CODE registration.
SKIP_EXIT_CODE = 77


def run_twice(executable: str, command: list, rom: pathlib.Path):
    """Run one CLI command against ``rom`` twice and require identical results.

    The complete ``stderr`` string of each run -- which may contain private numeric provenance
    values such as ``source_address``/``image_offset`` -- participates opaquely in memory in this
    byte-for-byte equality comparison; that full-stderr comparison is required and intentional
    (it is how two-run determinism is established), and numeric provenance values are never
    individually asserted, pinned, printed, logged, persisted, or committed anywhere as a result of
    it. The failure message deliberately carries no field of either run (not the ROM path, not
    returncode/stdout/stderr, and therefore never the private source address, image offset, or
    provenance values embedded in a rejection report): only the byte-for-byte fact of a mismatch
    is ever raised, so a determinism failure can never itself become a privacy leak.
    """
    runs = [
        subprocess.run([executable, *command, str(rom)], text=True, capture_output=True, check=False)
        for _ in range(2)
    ]
    first, second = runs
    first_fingerprint = (first.returncode, first.stdout, first.stderr)
    second_fingerprint = (second.returncode, second.stdout, second.stderr)
    if first_fingerprint != second_fingerprint:
        raise AssertionError(f"{command} repeated runs were not byte-identical")
    return first


def main() -> None:
    executable = sys.argv[1]
    env_path = os.environ.get("SEGARECOMP_SONIC_ROM")
    if not env_path:
        print("sonic first-unsupported local test: skipped (SEGARECOMP_SONIC_ROM unset)")
        sys.exit(SKIP_EXIT_CODE)

    rom = pathlib.Path(env_path)
    assert rom.is_file(), "SEGARECOMP_SONIC_ROM must name a readable local file"
    digest = hashlib.sha256(rom.read_bytes()).hexdigest()
    assert digest == PINNED_SHA256, (
        "local file at SEGARECOMP_SONIC_ROM is a mismatched/unpinned input "
        "(its SHA-256 does not equal the SEG-006-T001 pinned digest)"
    )

    for command in (["genesis-rom-startup"], ["emit-genesis-rom-startup-c"]):
        run = run_twice(executable, command, rom)
        if run.returncode == 0 or run.stdout != "":
            raise AssertionError(
                f"{command}: expected the rejected/fail-closed exit class (nonzero return code, "
                "empty stdout); production route did not fail closed as required"
            )

        report = json.loads(run.stderr.strip())
        assert report["category"] == EXPECTED_CATEGORY, (
            command,
            "expected the fixed genesis_rom_startup graph's current startup_graph_mismatch "
            f"boundary, got {report['category']!r}",
        )

        # Non-content provenance-field *presence* only: these field-specific checks never assert,
        # print, or persist the numeric values themselves (the earlier run_twice determinism
        # comparison already consumed the complete stderr strings opaquely in memory; see its
        # docstring and the module docstring's privacy paragraph).
        for field in REQUIRED_NON_NULL_FIELDS:
            assert report.get(field) is not None, (command, f"expected non-null {field!r} in the rejection report")

    print(
        "sonic first-unsupported local test: ok "
        "(hash-verified pinned local input reproduces the fixed genesis_rom_startup graph's "
        "current startup_graph_mismatch boundary deterministically across two runs of each "
        "command)"
    )


if __name__ == "__main__":
    main()
