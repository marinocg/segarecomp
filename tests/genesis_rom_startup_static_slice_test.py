#!/usr/bin/env python3
"""Project-authored SEG-005 fixture: static C and ingress smoke coverage."""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ORACLE_REVISION = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
ORACLE_ID = f"musashi@{ORACLE_REVISION}"
INITIAL_D = ["11111111", "22222222", "33333333", "44444444",
             "55555555", "66666666", "77777777", "88888888"]
INITIAL_SR = "2700"
RAM_BEGIN = 0x00FF0000
RAM_LENGTH = 0x10000
EMPTY_OUTPUT_SHA256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

# Captured from the pre-SEG-007-T014 genesis-rom-startup command.  Keep the
# established public command's complete stdout/stderr bytes stable while the
# new general-startup command is added alongside it.
COMMAND_OUTPUT_SNAPSHOTS = {
    "accepted": ("0e466425c883a8c0ed1921b3f2727e22dd0680c5237c8a1b1a5124d63c8f07c9",
                 EMPTY_OUTPUT_SHA256),
    "primary-truncated": (EMPTY_OUTPUT_SHA256,
                          "d9ffbca7c0be9a793b48f192961fcb03d5d2774a046e74fbaccae3e8babe93cc"),
    "extension-truncated-store": (EMPTY_OUTPUT_SHA256,
                                  "13804498f6eb7d883ea14fa6ed0f1799a1503f6a8ea933a048beac5e85a162b8"),
    "extension-truncated-jsr": (EMPTY_OUTPUT_SHA256,
                                "aaa38f6248562e37ff2ce35d619e58d0a3727bd6e9205c0bc9b553d0408348d8"),
    "illegal-primary": (EMPTY_OUTPUT_SHA256,
                        "81320b331bc593dbc6ed50a43fb85b2213ddfbe52444fd8ea8585e5850190fd6"),
    "unsupported-primary": (EMPTY_OUTPUT_SHA256,
                            "f19105a193529c10193d9ffd52401d2299c06cc57e2b2cea25c7aa9a3e5b4312"),
    "five-moveq-trace": (EMPTY_OUTPUT_SHA256,
                         "13cc5453c677bfc8cc7953724ab1b180881f8240d7ddbb113951637f3efbc5b5"),
    "rom-write": (EMPTY_OUTPUT_SHA256,
                  "694e44af66966fc320d1ef72b8957fd16844316697396323e6907b6056c790ae"),
    "odd-data": (EMPTY_OUTPUT_SHA256,
                 "a688050fc5caebb65ac01150b31055f37f7d4691233d214fb00becaab865cfe3"),
    "unmapped-data": (EMPTY_OUTPUT_SHA256,
                      "cd2d95fa2435ed0d1b5b4cfcc31f96c4a883253030f9e248d51480ca82066e81"),
    "invalid-stack-alignment": (EMPTY_OUTPUT_SHA256,
                                "441e3fc109fcc9cb20169bae671f81440d52d3677a406e0e83a9cfc7738f112c"),
    "invalid-stack-range": (EMPTY_OUTPUT_SHA256,
                            "dbf0581f9b41aa333d8ce22f3586434ec3f5c189186179811ffddda7c0bd25c4"),
    "unsupported-form": (EMPTY_OUTPUT_SHA256,
                         "fb71b9f34fe08c76e7ff72e2b2e5da1ebcb8c79d2998d03f948b26aa04c4fd1c"),
}


def assert_command_output_snapshot(name, run):
    expected_stdout, expected_stderr = COMMAND_OUTPUT_SNAPSHOTS[name]
    actual = (hashlib.sha256(run.stdout).hexdigest(),
              hashlib.sha256(run.stderr).hexdigest())
    assert actual == (expected_stdout, expected_stderr), (name, actual)


def oracle_boundary_expected(boundary):
    """Expand the report's changed RAM words against the contract's zero RAM."""
    state = boundary["state"]
    ram = bytearray(RAM_LENGTH)
    for changed in boundary["ram_bytes"]:
        address = int(changed["address"], 16)
        raw = bytes.fromhex(changed["bytes"])
        offset = address - RAM_BEGIN
        assert len(raw) == 4 and 0 <= offset <= RAM_LENGTH - len(raw), changed
        ram[offset:offset + len(raw)] = raw
    return {"d": state["d"], "a7": state["a7"], "pc": state["pc"],
            "sr": state["sr"], "ram_hex": ram.hex().upper(),
            "executed_instructions": 1}


def compare_oracle(adapter, image, accepted, directory):
    checkout = adapter.parent / "musashi"
    pin = subprocess.run(["git", "-C", str(checkout), "rev-parse", "HEAD"],
                         text=True, capture_output=True, check=False)
    assert pin.returncode == 0 and pin.stdout.strip() == ORACLE_REVISION and pin.stderr == "", (
        "SEGARECOMP_STARTUP_MUSASHI_ORACLE must have the pinned adjacent Musashi checkout", pin)
    source = directory / "startup-oracle-input.json"
    destination = directory / "startup-oracle-output.json"
    source.write_text(json.dumps({"image_hex": image.hex().upper(),
                                  "image_sha256": hashlib.sha256(image).hexdigest().upper(),
                                  "d": INITIAL_D, "sr": INITIAL_SR,
                                  "ram_hex": "00" * RAM_LENGTH},
                                 separators=(",", ":")) + "\n")
    run = subprocess.run([str(adapter), "--input", str(source), "--output", str(destination),
                          "--instruction-counts", "1,1,1,1,1"],
                         text=True, capture_output=True, check=False)
    assert run.returncode == 0 and run.stdout == "" and run.stderr == "", run
    oracle = json.loads(destination.read_text())
    assert set(oracle) == {"oracle", "boundaries", "executed_instructions", "stop_reason"}
    assert oracle["oracle"] == ORACLE_ID
    assert oracle["executed_instructions"] == 5
    assert oracle["stop_reason"] == "instruction_budget_exhausted"
    expected_boundaries = [oracle_boundary_expected(boundary)
                           for boundary in accepted["boundaries"][1:]]
    assert len(expected_boundaries) == 5 and len(oracle["boundaries"]) == 5
    for ordinal, (expected, actual) in enumerate(zip(expected_boundaries, oracle["boundaries"])):
        assert set(actual) == set(expected), (ordinal, actual)
        assert actual == expected, (ordinal, actual, expected)


def main() -> None:
    executable, compiler = sys.argv[1:3]
    root = pathlib.Path(__file__).resolve().parent
    vector = json.loads((root / "fixtures/genesis-rom-startup-vectors.json").read_text())
    image = bytearray(vector["image_length"])
    for offset, data in vector["writes"].items():
        start = int(offset, 16)
        raw = bytes.fromhex(data)
        image[start:start + len(raw)] = raw
    image[0x120:0x150] = vector["title"].encode("ascii").ljust(48, b" ")
    assert hashlib.sha256(image).hexdigest() == vector["sha256"]
    oracle_adapter = None
    if "SEGARECOMP_STARTUP_MUSASHI_ORACLE" in os.environ:
        oracle_adapter = pathlib.Path(os.environ["SEGARECOMP_STARTUP_MUSASHI_ORACLE"])
        assert oracle_adapter.is_file() and os.access(oracle_adapter, os.X_OK), \
            "invalid SEGARECOMP_STARTUP_MUSASHI_ORACLE"
    with tempfile.TemporaryDirectory() as directory:
        directory = pathlib.Path(directory)
        rom = directory / "startup.bin"
        source = directory / "startup.c"
        output = directory / "startup"
        rom.write_bytes(image)
        report = subprocess.run([executable, "genesis-rom-startup", str(rom)], check=True,
                                capture_output=True)
        assert_command_output_snapshot("accepted", report)
        accepted = json.loads(report.stdout)
        assert accepted["result"] == "accepted"
        for key, value in vector["expected"].items():
            if key == "boundaries":
                assert len(accepted[key]) == value, (key, accepted)
            else:
                assert accepted[key] == value, (key, accepted)
        assert [[record["ordinal"], record["address"], record["bytes"]]
                for record in accepted["accesses"]] == vector["accepted_bus"]
        assert accepted["boundaries"][0]["bus_through_ordinal"] is None
        assert accepted["boundaries"][-1]["bus_through_ordinal"] == 8
        assert [boundary["ordinal"] for boundary in accepted["boundaries"]] == list(range(6))
        assert accepted["final_state"]["sr"] == "2700"
        assert all(boundary["state"]["sr"] == "2700" for boundary in accepted["boundaries"])
        # Every ROM-driven negative recipe uses the public shared frontend
        # command.  Isolated RTS frame cases are exercised by the C++ shared
        # execution-context test because they intentionally are not ROM input.
        def rejected(name, changed, category):
            candidate = directory / f"{name}.bin"
            candidate.write_bytes(changed)
            run = subprocess.run([executable, "genesis-rom-startup", str(candidate)],
                                 capture_output=True)
            assert run.returncode != 0, (name, run)
            assert_command_output_snapshot(name, run)
            report = json.loads(run.stderr)
            assert report["category"] == category, (name, report)
            recipe = vector["negative_recipes"].get(name)
            if recipe is not None:
                # "failing_bus_records" (contract lines 240-247) counts only the
                # bus records the failing attempt itself contributes: the shared
                # static rejection path (FrontendRejected) never accumulates
                # earlier instructions' records, so its "accesses" length is
                # already that count directly.  The runtime execution path
                # (StartupFailure) accumulates every already-processed
                # instruction's bus records too, so the failing attempt's own
                # count is the tail past the last completed boundary's
                # bus_through_ordinal.
                boundaries = report.get("boundaries") or []
                prior = boundaries[-1]["bus_through_ordinal"] + 1 if boundaries and boundaries[-1]["bus_through_ordinal"] is not None else 0
                failing_bus_records = len(report.get("accesses", [])) - prior
                assert failing_bus_records == recipe["failing_bus_records"], (name, failing_bus_records, report)
            # The shared structured-C backend must fail exactly as closed as the
            # plain report command for every contract negative recipe: no C is
            # ever emitted to stdout, and the same rejection category is
            # reported on stderr through the same pre-emission validation.
            emit_run = subprocess.run([executable, "emit-genesis-rom-startup-c", str(candidate)],
                                       text=True, capture_output=True)
            assert emit_run.returncode != 0 and emit_run.stdout == "", (name, emit_run)
            emit_report = json.loads(emit_run.stderr.strip())
            assert emit_report["category"] == category, (name, emit_report)
            return report

        report = rejected("primary-truncated", image[:0x161], "truncated_instruction")
        assert report["source_address"] == "0x00000160" and report["provenance"] is None
        report = rejected("extension-truncated-store", image[:0x166], "truncated_instruction")
        assert report["source_address"] == "0x00000162" and report["provenance"]["raw_bytes"] == "23C0"
        report = rejected("extension-truncated-jsr", image[:0x16C], "truncated_instruction")
        assert report["source_address"] == "0x00000168" and report["provenance"]["raw_bytes"] == "4EB9"
        changed = image[:]; changed[0x160:0x162] = bytes.fromhex("4AFC")
        rejected("illegal-primary", changed, "illegal_instruction")
        changed = image[:]; changed[0x160:0x162] = bytes.fromhex("4E71")
        rejected("unsupported-primary", changed, "valid_but_unsupported_instruction")
        # A sequence with five individually selected MOVEQs is not the startup
        # graph: the entry block's second node must be the selected RAM store.
        changed = image[:]
        for offset in range(0x160, 0x16A, 2):
            changed[offset:offset + 2] = bytes.fromhex("702A")
        report = rejected("five-moveq-trace", changed, "startup_graph_mismatch")
        assert report["source_address"] == "0x00000162" and report["provenance"]["raw_bytes"] == "702A"
        for name, extension, category in (
            ("rom-write", "00000100", "rom_write_prohibited"),
            ("odd-data", "00FF0001", "odd_effective_address"),
            ("unmapped-data", "00A00000", "unmapped_data_access"),
        ):
            changed = image[:]; changed[0x164:0x168] = bytes.fromhex(extension)
            report = rejected(name, changed, category)
            assert report["source_address"] == "0x00000162" and report["provenance"]["length"] == 6
        changed = image[:]; changed[0:4] = bytes.fromhex("00FF0101")
        rejected("invalid-stack-alignment", changed, "invalid_stack_alignment")
        changed = image[:]; changed[0:4] = bytes.fromhex("00FF0000")
        rejected("invalid-stack-range", changed, "invalid_stack_range")
        changed = image[:]; changed[0x168:0x16A] = bytes.fromhex("4E90")
        rejected("unsupported-form", changed, "unsupported_instruction_form")
        emitted = subprocess.run([executable, "emit-genesis-rom-startup-c", str(rom)], check=True, text=True, capture_output=True).stdout
        assert emitted == subprocess.run([executable, "emit-genesis-rom-startup-c", str(rom)], check=True, text=True, capture_output=True).stdout
        assert "target image" in emitted
        assert "#include <inttypes.h>" in emitted
        assert "PRIX32" in emitted
        assert "702A" not in emitted and "23C0" not in emitted
        assert "observed_return" in emitted
        assert "static_frame_depth == 0U" in emitted
        assert "pc = observed_return" in emitted
        source.write_text(emitted)
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(output)], check=True)
        generated = json.loads(subprocess.run([str(output)], check=True, text=True, capture_output=True).stdout)
        for key in ("d0", "d1", "a7", "pc"):
            assert generated[key] == vector["expected"][key]
        assert generated["sr"] == "2700"
        assert generated["stop_reason"] == "instruction_budget_exhausted"
        if oracle_adapter is not None:
            compare_oracle(oracle_adapter, image, accepted, directory)
    if oracle_adapter is None:
        print("genesis startup static slice: ok; Musashi differential comparison unavailable (SEGARECOMP_STARTUP_MUSASHI_ORACLE unset)")
    else:
        print("genesis startup static slice: ok; Musashi differential comparison passed")


if __name__ == "__main__":
    main()
