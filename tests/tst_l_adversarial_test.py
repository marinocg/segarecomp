#!/usr/bin/env python3
"""Adversarial validation of the TST.L (xxx).L capability
(SEG-007-T009, docs/references/tst-l-absolute-long-contract.md).

This test independently tries to falsify SEG-007-T008's delivered capability
rather than re-running its vectors: it is an adversarial superset focused on
region boundaries, step-8 precedence ties, determinism, and an independent
execution-oracle differential. It reads tests/fixtures/tst-l-adversarial-vectors.json
(project-authored synthetic recipes) and never reads or modifies
tests/fixtures/tst-l-fixtures.json.

Every expected post-state is derived here directly from the contract's
"Hardware contract" statement (N = bit31(operand), Z = (operand == 0), V = 0,
C = 0, X unaffected, PC += 6), bit by bit, rather than by reusing any
production mask expression, so a wrong stored expectation cannot silently
pass.

It drives two existing executables and adds no production API:
  * tests/tools/tst_l_test_harness.cpp (the SEG-007-T008 test-only harness)
    for the shared decode -> lift -> operand-resolution -> effect -> emit path;
  * the production `segarecomp` CLI, to prove the fixed five-operation
    genesis_rom_startup graph still rejects a leading TST.L.

The pinned Musashi differential is optional and off by default.  Set
 SEGARECOMP_TST_L_MUSASHI_CHECKOUT to a local ignored checkout at the exact
 pin.  This committed test builds the tracked Musashi generator, regenerates
 its opcode sources in a temporary directory, and builds its committed runner
 there.  It then checks the runner's revision report and observed data
 accesses.  No opaque adapter, prebuilt executable, or checkout-generated
 opcode source is accepted as an oracle provenance root.

Contract precedence coverage through this harness is deliberately partial.
Step 2 (odd_instruction_address) is reached by the one rejection vector that
overrides the manifest-level entry_pc/entry_image_offset with an odd source
address. Steps 1 (unsupported_cpu_variant, unsupported_address_space) and 3
(unmapped_instruction_address) are structurally unreachable here: the
test-only harness hardcodes CpuVariant::mc68000 and
TargetAddressSpace::m68k_program and performs no frontend address mapping, so
no command line can drive them. They are covered instead by
tests/moveq_test.cpp and tests/m68k_frontend_adversarial_test.py, and must not
be forced into this test.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VECTORS = ROOT / "tests/fixtures/tst-l-adversarial-vectors.json"
STARTUP_VECTORS = ROOT / "tests/fixtures/genesis-rom-startup-vectors.json"
ORACLE_REVISION = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
ORACLE_ID = f"musashi@{ORACLE_REVISION}"
ORACLE_CHECKOUT_ENVIRONMENT = "SEGARECOMP_TST_L_MUSASHI_CHECKOUT"
LEGACY_ORACLE_ENVIRONMENT = "SEGARECOMP_TST_L_MUSASHI_ORACLE"
MUSASHI_RUNNER_SOURCE = ROOT / "tests/tools/tst_l_musashi_runner.c"
RAM_BEGIN = 0x00FF0000
HEX = set("0123456789ABCDEF")
# A translated program must depend on nothing but itself: no source-image
# fetch, no runtime decode, no input.
FORBIDDEN_TOKENS = ("fopen", "fread", "freopen", "open(", "read(", "getchar", "scanf", "argv",
                    "decode", "opcode")
# The selected primary word must never survive into generated output as an
# emitted constant: translation happened once, statically.
FORBIDDEN_PRIMARY = "4ab9"
# The three accepted recipes docs/references/tst-l-absolute-long-contract.md
# records; reproduced here so a drift in either document or fixture is caught.
CONTRACT_RECORDED_SHA256 = (
    "32207f6f7540030671bdd53f5403f49cbaa42f1b906efb5a47be899fb590b0b1",
    "8cd33c30981199ad3b4269969d291650560a5c75fc423b4cf1590f8376d2317c",
    "5ac32f832b0a938696808e9488cfceb7441ab22374239d3d24a931f54604a714",
)
OVERLAPPING_OPERAND_ID = "rom-operand-overlaps-instruction"


def fixed_hex(value, width):
    return isinstance(value, str) and len(value) == width and set(value) <= HEX


def build_image(entry):
    """Expand a recipe into exactly its bytes: image_length zeros plus writes."""
    image = bytearray(int(entry["image_length"], 16))
    for offset, data in entry["writes"].items():
        assert fixed_hex(offset.upper(), 6) and fixed_hex(data, len(data)) and len(data) % 2 == 0, entry["id"]
        start = int(offset, 16)
        raw = bytes.fromhex(data)
        assert start + len(raw) <= len(image), (entry["id"], offset)
        image[start:start + len(raw)] = raw
    return bytes(image)


def entry_pc(manifest, entry):
    """The manifest-level entry PC unless the vector deliberately overrides it."""
    return entry.get("entry_pc", manifest["entry_pc"])


def entry_image_offset(manifest, entry):
    """The manifest-level image offset unless the vector deliberately overrides it."""
    return entry.get("entry_image_offset", manifest["entry_image_offset"])


def independent_sr(seed_sr, value):
    """The contract's CCR statement, transcribed bit by bit.

    N = bit31(operand); Z = (operand == 0); V = 0; C = 0; X unaffected; every
    other SR bit unaffected.
    """
    seed = int(seed_sr, 16)
    n = (value >> 31) & 1
    z = 1 if value == 0 else 0
    v = 0
    c = 0
    return f"{(seed & 0xFFE0) | (seed & 0x10) | (n << 3) | (z << 2) | (v << 1) | c:04X}"


def validate_manifest():
    manifest = json.loads(VECTORS.read_text())
    assert set(manifest) == {"schema", "ownership", "recipe", "oracle", "entry_pc",
                             "entry_image_offset", "expected_pc", "seed_d", "accepted", "rejections"}
    assert manifest["schema"] == 1
    ownership = manifest["ownership"].lower()
    assert "project-authored" in ownership and "no commercial content" in ownership
    assert manifest["oracle"] == ORACLE_ID
    assert fixed_hex(manifest["entry_pc"], 8) and fixed_hex(manifest["entry_image_offset"], 16)
    assert int(manifest["entry_image_offset"], 16) == 0x100 and int(manifest["entry_pc"], 16) == 0x00000100
    # PC + 6, derived from the contract's encoded length, not copied from output.
    assert manifest["expected_pc"] == f"{int(manifest['entry_pc'], 16) + 6:08X}"
    assert len(manifest["seed_d"]) == 8 and all(fixed_hex(value, 8) for value in manifest["seed_d"])

    identifiers = set()
    recorded = []
    for entry in manifest["accepted"]:
        expected_keys = {"id", "purpose", "image_length", "writes", "sha256", "seed_sr", "region",
                         "operand_address", "operand_value", "ram_seed", "expected_sr"}
        assert set(entry) - {"contract_recorded_sha256"} == expected_keys, entry["id"]
        assert entry["id"] not in identifiers
        identifiers.add(entry["id"])
        image = build_image(entry)
        assert hashlib.sha256(image).hexdigest() == entry["sha256"], entry["id"]
        assert fixed_hex(entry["seed_sr"], 4) and fixed_hex(entry["operand_address"], 8)
        assert fixed_hex(entry["operand_value"], 8) and entry["region"] in {"raw_cartridge_rom",
                                                                            "synthetic_work_ram"}
        # The stored expectation must equal the independently derived one.
        assert entry["expected_sr"] == independent_sr(entry["seed_sr"], int(entry["operand_value"], 16)), entry["id"]
        # The declared operand value must actually be the bytes the selected
        # region holds, not an unchecked annotation.
        address = int(entry["operand_address"], 16)
        if entry["region"] == "raw_cartridge_rom":
            assert entry["ram_seed"] == [], entry["id"]
            assert address + 4 <= len(image), entry["id"]
            assert image[address:address + 4].hex().upper() == entry["operand_value"], entry["id"]
        else:
            assert entry["ram_seed"] == [{"address": entry["operand_address"],
                                          "value": entry["operand_value"]}], entry["id"]
            assert RAM_BEGIN <= address <= 0x01000000 - 4, entry["id"]
        if "contract_recorded_sha256" in entry:
            assert entry["contract_recorded_sha256"] == entry["sha256"], entry["id"]
            recorded.append(entry["sha256"])
    assert tuple(recorded) == CONTRACT_RECORDED_SHA256
    overlap = [entry for entry in manifest["accepted"] if entry["id"] == OVERLAPPING_OPERAND_ID]
    assert len(overlap) == 1
    overlap = overlap[0]
    assert (overlap["region"], overlap["operand_address"], overlap["operand_value"],
            overlap["expected_sr"]) == ("raw_cartridge_rom", manifest["entry_pc"], "4AB90000", "2710")

    for entry in manifest["rejections"]:
        expected_keys = {"id", "purpose", "image_length", "writes", "sha256", "seed_sr",
                         "expected_stage", "expected_category", "expected_unsupported_instruction_form",
                         "expected_provenance_length"}
        if entry["expected_stage"] == "effective_address":
            expected_keys |= {"expected_complete_raw_bytes", "expected_effective_address"}
        # A rejection vector may override the entry address, and only as a
        # complete identity-mapped pair, to reach a precedence step the
        # manifest-level even entry address cannot reach.
        assert set(entry) - {"entry_pc", "entry_image_offset"} == expected_keys, entry["id"]
        assert ("entry_pc" in entry) == ("entry_image_offset" in entry), entry["id"]
        if "entry_pc" in entry:
            assert fixed_hex(entry["entry_pc"], 8) and fixed_hex(entry["entry_image_offset"], 16), entry["id"]
            assert int(entry["entry_pc"], 16) == int(entry["entry_image_offset"], 16), entry["id"]
            assert entry["entry_pc"] != manifest["entry_pc"], entry["id"]
        assert entry["id"] not in identifiers
        identifiers.add(entry["id"])
        image = build_image(entry)
        assert hashlib.sha256(image).hexdigest() == entry["sha256"], entry["id"]
        # Whatever the vector rejects, its instruction really is inside the
        # image at the address the harness is pointed at.
        assert int(entry_image_offset(manifest, entry), 16) < len(image), entry["id"]
        assert fixed_hex(entry["seed_sr"], 4)
        assert entry["expected_stage"] in {"decode", "effective_address"}
        assert isinstance(entry["expected_unsupported_instruction_form"], bool)
        assert entry["expected_provenance_length"] in {None, 2, 6}
    assert len(manifest["accepted"]) == 11 and len(manifest["rejections"]) == 24
    return manifest


def command(harness, image, manifest, entry):
    arguments = [str(harness), str(image), entry_pc(manifest, entry),
                 entry_image_offset(manifest, entry), entry["seed_sr"], *manifest["seed_d"]]
    for seed in entry.get("ram_seed", []):
        arguments += [seed["address"], seed["value"]]
    return arguments


def resolve_compiler(compiler):
    resolved = shutil.which(compiler)
    assert resolved is not None, f"compiler unavailable: {compiler}"
    return Path(resolved).resolve()


def compiler_environment():
    """Supply macOS's SDK to both generated-C and local-oracle compilations."""
    environment = os.environ.copy()
    if not environment.get("SDKROOT") and shutil.which("xcrun"):
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], text=True, capture_output=True, check=False)
        if sdk.returncode == 0:
            environment["SDKROOT"] = sdk.stdout.strip()
    return environment


def require_t012_shared_owner_consumption():
    """Inspect Batch-A's shared TST lowering independently of output state.

    A state-only test cannot distinguish consuming shared owners from
    independently reimplementing their current values. This inspection covers
    only T012's TST.L correction, not excluded MOVE lowering cases.
    """
    # SEG-014-T005 correction: emit_m68k_operation_c's shared switch (and this
    # TST.L case within it) physically moved from src/m68k_pipeline.cpp into
    # libs/codegen/c11/src/m68k.cpp; see
    # docs/architecture/seg-014-t001-symbol-migration-map.md.
    pipeline = (ROOT / "libs/codegen/c11/src/m68k.cpp").read_text()
    match = re.search(r"case M68kIrKind::test_operand: \{(?P<body>.*?)\n  \}\n  case ",
                      pipeline, re.DOTALL)
    assert match is not None, "shared Batch-A TST C-lowering case is unavailable for ownership inspection"
    body = match.group("body")
    assert "M68kMoveResultCcrSpecification::emit_c_update(output, status_register, \"test_value\")" in body
    assert 'output << "pc += UINT32_C(" << operation.provenance.length.value << ");\\n";' in body
    assert "pc += 6U" not in body


def pinned_oracle(compiler, temporary):
    """Build the only oracle executable from checked source and fresh generated code."""
    # Deliberately ignore the former opaque-adapter setting.  It cannot enable
    # a differential; only a checked-out source tree can do that.
    if os.environ.get(LEGACY_ORACLE_ENVIRONMENT):
        print(f"ignoring obsolete {LEGACY_ORACLE_ENVIRONMENT}; use {ORACLE_CHECKOUT_ENVIRONMENT}",
              file=sys.stderr)
    checkout_setting = os.environ.get(ORACLE_CHECKOUT_ENVIRONMENT)
    if not checkout_setting:
        return None
    checkout = Path(checkout_setting)
    assert checkout.is_dir(), f"TST.L Musashi checkout is not a directory: {checkout}"
    revision = subprocess.run(["git", "-C", str(checkout), "rev-parse", "HEAD"],
                              text=True, capture_output=True, check=False)
    assert revision.returncode == 0 and revision.stdout.strip() == ORACLE_REVISION, (
        f"TST.L Musashi checkout is not pinned at {ORACLE_REVISION}: {checkout}", revision.stderr)
    clean = subprocess.run(["git", "-C", str(checkout), "diff", "--quiet", "HEAD", "--"],
                            text=True, capture_output=True, check=False)
    assert clean.returncode == 0, "TST.L Musashi checkout has tracked source modifications"
    tracked_sources = (checkout / "m68kmake.c", checkout / "m68k_in.c", checkout / "m68kcpu.c",
                       checkout / "softfloat/softfloat.c")
    assert MUSASHI_RUNNER_SOURCE.is_file() and all(source.is_file() for source in tracked_sources), \
        f"TST.L pinned Musashi checkout lacks required build sources: {checkout}"
    runner = temporary / "tst-l-musashi-runner"
    generator = temporary / "m68kmake"
    generator_input = temporary / "m68k_in.c"
    generated_ops = temporary / "m68kops.c"
    generated_ops_header = temporary / "m68kops.h"
    # m68kcpu.c includes "m68kops.h" with quote-search semantics.  Compile a
    # temporary symlink to the checked tracked source so that it resolves only
    # the fresh temporary header, never an ignored checkout/m68kops.h.
    temporary_cpu_source = temporary / "m68kcpu.c"
    # The pinned generated Musashi sources have known unused locals under
    # current Clang; keep every other warning fatal, including this committed
    # runner's warnings, rather than editing ignored third-party sources.
    environment = compiler_environment()
    generator_build = subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                                      "-pedantic", str(checkout / "m68kmake.c"), "-o", str(generator)],
                                     text=True, capture_output=True, check=False, env=environment)
    assert generator_build.returncode == 0, (
        "cannot strictly build tracked Musashi opcode generator", generator_build.stderr)
    assert generator.is_file() and os.access(generator, os.X_OK), "Musashi generator build produced no executable"
    shutil.copyfile(checkout / "m68k_in.c", generator_input)
    assert generator_input.is_file() and generator_input.stat().st_size > 0, "cannot prepare Musashi generator input"
    generation = subprocess.run([str(generator)], cwd=temporary, text=True, capture_output=True, check=False,
                                env=environment)
    assert generation.returncode == 0, ("cannot generate fresh Musashi opcode sources", generation.stderr)
    assert all(source.is_file() and source.stat().st_size > 0 for source in (generated_ops, generated_ops_header)), \
        "Musashi generator did not produce both temporary opcode sources"
    temporary_cpu_source.symlink_to(checkout / "m68kcpu.c")
    assert temporary_cpu_source.is_symlink() and temporary_cpu_source.resolve() == checkout / "m68kcpu.c", \
        "cannot prepare checked Musashi CPU source"
    build = subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-error=unused-variable", "-pedantic",
                            f'-DMUSASHI_GIT_REVISION="{ORACLE_REVISION}"', "-I", str(temporary), "-I",
                            str(checkout), "-I", str(checkout / "softfloat"), str(MUSASHI_RUNNER_SOURCE),
                            str(temporary_cpu_source), str(generated_ops), str(checkout / "softfloat/softfloat.c"),
                            "-o", str(runner)], text=True, capture_output=True, check=False, env=environment)
    assert build.returncode == 0, ("cannot strictly build committed TST.L Musashi runner", build.stderr)
    assert runner.is_file() and os.access(runner, os.X_OK), "TST.L Musashi runner build produced no executable"
    return runner


def run_oracle(runner, manifest, entry, image, temporary, invocations):
    """Run the just-built checked runner and retain the legacy binding defense."""
    image_path = temporary / f"{entry['id']}-oracle-image.bin"
    image_path.write_bytes(image)
    ram_seeds = [field for seed in entry["ram_seed"] for field in (seed["address"], seed["value"])]
    invocations.append(entry["id"])
    result = subprocess.run([str(runner), str(image_path), manifest["entry_pc"], entry["seed_sr"],
                             *manifest["seed_d"], *ram_seeds], text=True, capture_output=True, check=False)
    assert result.returncode == 0 and result.stderr == "", (entry["id"], result)
    runner_state = json.loads(result.stdout)
    assert set(runner_state) == {"musashi_revision", "d", "pc", "sr", "writes", "accesses"}, (
        "committed TST.L Musashi runner did not emit the required state/access shape", entry["id"], runner_state)
    assert runner_state["musashi_revision"] == ORACLE_REVISION, (
        "TST.L runner did not report the revision supplied by the checked validator", entry["id"], runner_state)
    boundary = {key: value for key, value in runner_state.items() if key != "musashi_revision"}
    # Keep the established oracle-binding assertion as defense in depth.  It is
    # now derived locally from the checked runner result, not adapter testimony.
    oracle = {"oracle": ORACLE_ID,
              "oracle_binding": {"kind": "runner_reported_musashi_revision", "revision": runner_state["musashi_revision"]},
              "executed_instructions": 1, "boundary": boundary,
              "stop_reason": "instruction_budget_exhausted"}
    assert set(oracle) == {"oracle", "oracle_binding", "executed_instructions", "boundary", "stop_reason"}, (
        "TST.L oracle does not provide the required runner-attested pin binding; "
        "an adapter adjacent to a pinned checkout is insufficient", entry["id"], oracle)
    assert oracle["oracle"] == ORACLE_ID and oracle["executed_instructions"] == 1, (entry["id"], oracle)
    assert oracle["oracle_binding"] == {"kind": "runner_reported_musashi_revision",
                                        "revision": ORACLE_REVISION}, (
        "TST.L oracle pin binding is not the exact revision reported by its executing runner", entry["id"], oracle)
    assert oracle["stop_reason"] == "instruction_budget_exhausted", (entry["id"], oracle)
    boundary = oracle["boundary"]
    assert set(boundary) == {"d", "pc", "sr", "writes", "accesses"}, (
        "TST.L oracle does not expose operand access events required for attestation", entry["id"], oracle)
    # The access evidence comes from oracle output, never from fixture data
    # alone: exactly one selected operand read and no writes.
    assert boundary["writes"] == 0, (entry["id"], boundary)
    assert boundary["accesses"] == [{"kind": "operand_read", "address": entry["operand_address"],
                                     "width": 32, "value": entry["operand_value"]}], (entry["id"], boundary)
    return boundary


def require_no_runtime_dependence(manifest, entry, image, generated):
    lowered = generated.lower()
    assert all(token not in lowered for token in FORBIDDEN_TOKENS), entry["id"]
    # An overlapping ROM operand can itself begin with the selected primary.
    # That one occurrence is a statically folded data literal, which
    # require_region_lowering verifies below; it is not an emitted opcode.
    primary_in_operand = entry["region"] == "raw_cartridge_rom" and \
        entry["operand_value"].lower().startswith(FORBIDDEN_PRIMARY)
    assert lowered.count(FORBIDDEN_PRIMARY) == (1 if primary_in_operand else 0), entry["id"]
    # The instruction's own six raw bytes -- the only non-zero image content a
    # translation could plausibly carry -- must not survive anywhere in the
    # generated source as a contiguous hex run, in either letter case. The
    # span is taken from the vector's own entry image offset, so a vector that
    # overrides it is checked at the address it actually uses.
    start = int(entry_image_offset(manifest, entry), 16)
    raw = image[start:start + 6].hex()
    assert len(raw) == 12, entry["id"]
    assert raw not in lowered and raw.upper() not in generated, entry["id"]
    # Source provenance is retained as translation metadata, never as a
    # runtime fetch of the original image.
    assert "segarecomp_tst_l_source_address" in generated, entry["id"]
    assert "segarecomp_tst_l_image_offset" in generated, entry["id"]


def require_region_lowering(entry, generated):
    literal = f"UINT32_C(0x{entry['operand_value']})"
    test_lines = [line for line in generated.splitlines() if "const uint32_t test_value =" in line]
    assert len(test_lines) == 1, entry["id"]
    tested = test_lines[0]
    if entry["region"] == "raw_cartridge_rom":
        # The already-verified ROM fact is folded; no RAM slot is consulted.
        assert literal in tested, entry["id"]
        assert "ram[UINT32_C(" not in generated, entry["id"]
    else:
        # The tested value is read out of the generated RAM array itself.
        offset = int(entry["operand_address"], 16) - RAM_BEGIN
        assert all(f"ram[UINT32_C({offset + index})]" in tested for index in range(4)), entry["id"]
        assert literal not in generated, entry["id"]
        assert "raw_cartridge_rom" not in generated, entry["id"]


def require_accepted(harness, compiler, manifest, entry, temporary, runner, invocations):
    image_bytes = build_image(entry)
    image = temporary / f"{entry['id']}.bin"
    image.write_bytes(image_bytes)
    arguments = command(harness, image, manifest, entry)
    first = subprocess.run(arguments, text=True, capture_output=True, check=False)
    repeated = subprocess.run(arguments, text=True, capture_output=True, check=False)
    assert first.returncode == 0 and first.stderr == "", (entry["id"], first)
    assert repeated.returncode == 0 and repeated.stderr == "", (entry["id"], repeated)
    assert repeated.stdout == first.stdout, entry["id"]
    require_no_runtime_dependence(manifest, entry, image_bytes, first.stdout)
    require_region_lowering(entry, first.stdout)

    source = temporary / f"{entry['id']}.c"
    binary = temporary / entry["id"]
    source.write_text(first.stdout)
    compiled = subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                                str(source), "-o", str(binary)], text=True, capture_output=True, check=False,
                               env=compiler_environment())
    assert compiled.returncode == 0, (entry["id"], compiled.stderr)

    expected = {"schema": 1, "d": manifest["seed_d"], "pc": manifest["expected_pc"],
                "sr": independent_sr(entry["seed_sr"], int(entry["operand_value"], 16)),
                "stop_reason": "instruction_budget_exhausted"}
    executed = subprocess.run([str(binary)], text=True, capture_output=True, check=False)
    executed_again = subprocess.run([str(binary)], text=True, capture_output=True, check=False)
    assert executed.returncode == 0 and executed.stderr == "", (entry["id"], executed)
    assert executed_again.returncode == 0 and executed_again.stderr == "", (entry["id"], executed_again)
    assert executed_again.stdout == executed.stdout, entry["id"]
    observed = json.loads(executed.stdout)
    assert observed == expected, (entry["id"], observed, expected)

    if runner is not None:
        boundary = run_oracle(runner, manifest, entry, image_bytes, temporary, invocations)
        if entry["id"] == OVERLAPPING_OPERAND_ID:
            # The runner's access event, rather than fixture metadata alone,
            # must attest this overlapping code/data read.
            assert boundary["accesses"] == [{"kind": "operand_read", "address": "00000100",
                                             "width": 32, "value": "4AB90000"}], boundary
        # The oracle must agree with both the independent model and the state
        # the compiled translation actually printed.
        assert boundary["d"] == manifest["seed_d"] == observed["d"], (entry["id"], boundary)
        assert boundary["pc"] == manifest["expected_pc"] == observed["pc"], (entry["id"], boundary)
        assert boundary["sr"] == expected["sr"] == observed["sr"], (entry["id"], boundary, expected)


def require_rejected(harness, manifest, entry, temporary):
    image = temporary / f"{entry['id']}.bin"
    image.write_bytes(build_image(entry))
    source = temporary / f"{entry['id']}.c"
    result = subprocess.run(command(harness, image, manifest, entry), text=True, capture_output=True,
                            check=False)
    # A rejection must produce nothing that could be compiled: whatever the
    # harness did write to stdout is captured as a .c file, and the vector only
    # passes if there was nothing to capture.
    if result.stdout != "":
        source.write_text(result.stdout)
    assert result.returncode == 1 and result.stdout == "" and not source.exists(), (entry["id"], result)
    # The whole diagnostic is exactly one line of JSON on stderr, with no
    # trailing narration, and it parses back to the complete diagnostic shape.
    assert result.stderr.count("\n") == 1 and result.stderr.endswith("}\n"), (entry["id"], result)
    payload = json.loads(result.stderr)
    assert set(payload) == {"category", "stage", "unsupported_instruction_form", "provenance",
                            "complete_raw_bytes", "effective_address"}, (entry["id"], payload)
    assert payload["category"] == entry["expected_category"], (entry["id"], payload)
    assert payload["stage"] == entry["expected_stage"], (entry["id"], payload)
    assert payload["unsupported_instruction_form"] == entry["expected_unsupported_instruction_form"], (
        entry["id"], payload)
    if entry["expected_provenance_length"] is None:
        assert payload["provenance"] is None, (entry["id"], payload)
    else:
        assert payload["provenance"]["length"] == entry["expected_provenance_length"], (entry["id"], payload)
        assert payload["provenance"]["source_address"] == f"0x{int(entry_pc(manifest, entry), 16):06X}", (
            entry["id"], payload)
    if entry["expected_stage"] == "effective_address":
        # The instruction record is complete, but nothing beyond its own six
        # verified bytes was ever read: no partial or out-of-bounds data read.
        assert payload["complete_raw_bytes"] == entry["expected_complete_raw_bytes"], (entry["id"], payload)
        assert payload["effective_address"] == entry["expected_effective_address"], (entry["id"], payload)
    else:
        assert payload["complete_raw_bytes"] is None, (entry["id"], payload)
        assert payload["effective_address"] is None, (entry["id"], payload)


def require_startup_graph_still_rejects_tst_l(executable, temporary):
    """The fixed five-operation genesis_rom_startup graph is unchanged by TST.L."""
    vector = json.loads(STARTUP_VECTORS.read_text())
    image = bytearray(vector["image_length"])
    for offset, data in vector["writes"].items():
        start = int(offset, 16)
        raw = bytes.fromhex(data)
        image[start:start + len(raw)] = raw
    image[0x120:0x150] = vector["title"].encode("ascii").ljust(48, b" ")
    assert hashlib.sha256(image).hexdigest() == vector["sha256"]

    accepted_image = temporary / "startup-accepted.bin"
    accepted_image.write_bytes(bytes(image))
    accepted = subprocess.run([str(executable), "genesis-rom-startup", str(accepted_image)],
                              text=True, capture_output=True, check=False)
    # The recipe itself is a valid, recognized reset image: the rejection below
    # is therefore specific to TST.L and not to a malformed image.
    assert accepted.returncode == 0 and accepted.stderr == "", accepted
    assert json.loads(accepted.stdout)["result"] == "accepted", accepted.stdout

    entry_address = int.from_bytes(image[4:8], "big")
    patched = bytearray(image)
    patched[entry_address:entry_address + 6] = bytes.fromhex("4AB900000200")
    patched_image = temporary / "startup-leading-tst-l.bin"
    patched_image.write_bytes(bytes(patched))
    rejected = subprocess.run([str(executable), "genesis-rom-startup", str(patched_image)],
                              text=True, capture_output=True, check=False)
    assert rejected.returncode == 1 and rejected.stdout == "", rejected
    report = json.loads(rejected.stderr)
    assert report["category"] == "startup_graph_mismatch", report
    assert report["source_address"] == f"0x{entry_address:08X}", report


def main():
    harness = Path(sys.argv[1])
    executable = Path(sys.argv[2])
    compiler = resolve_compiler(sys.argv[3])
    manifest = validate_manifest()
    require_t012_shared_owner_consumption()
    invocations = []
    with tempfile.TemporaryDirectory() as temporary_name:
        temporary = Path(temporary_name)
        runner = pinned_oracle(compiler, temporary)
        for entry in manifest["accepted"]:
            require_accepted(harness, compiler, manifest, entry, temporary, runner, invocations)
        expected_invocations = len(manifest["accepted"]) if runner is not None else 0
        assert len(invocations) == expected_invocations
        for entry in manifest["rejections"]:
            require_rejected(harness, manifest, entry, temporary)
        # Project-policy rejections are not CPU exceptions: the oracle is never
        # consulted for them.
        assert len(invocations) == expected_invocations
        require_startup_graph_still_rejects_tst_l(executable, temporary)
    status = ("compared committed runner built from pinned Musashi checkout" if runner is not None
              else "pinned Musashi oracle unavailable; static evidence completed")
    print(f"adversarially validated {len(manifest['accepted'])} accepted and "
          f"{len(manifest['rejections'])} rejected TST.L (xxx).L vectors, region boundaries, "
          f"step-8 precedence ties, determinism, and the untouched genesis_rom_startup graph ({status})")


if __name__ == "__main__":
    main()
