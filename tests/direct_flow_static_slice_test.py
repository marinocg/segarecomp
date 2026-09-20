#!/usr/bin/env python3
"""Black-box validation for the bounded project-authored SEG-003 slice."""
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
VECTORS = ROOT / "tests/fixtures/direct-flow-oracle-vectors.json"
ORACLE_REVISION = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
ORACLE_ID = f"musashi@{ORACLE_REVISION}"
HEX = set("0123456789ABCDEF")


def fixed_hex(value, width):
    return isinstance(value, str) and len(value) == width and set(value) <= HEX


def require_state(actual, expected):
    assert set(actual) == {"d", "pc", "sr"}
    assert actual == expected


def require_provenance(provenance):
    assert set(provenance) == {"cpu_variant", "source_address", "image_offset", "raw_bytes", "length"}
    assert provenance["cpu_variant"] == "mc68000"
    assert provenance["source_address"]["space"] == "m68k_program"
    assert fixed_hex(provenance["source_address"]["value"][2:].upper(), 8)
    assert isinstance(provenance["image_offset"], int) and provenance["image_offset"] >= 0
    assert fixed_hex(provenance["raw_bytes"], provenance["length"] * 2)


def validate_manifest():
    manifest = json.loads(VECTORS.read_text())
    assert set(manifest) == {"schema", "ownership", "oracle", "vectors"}
    assert manifest["schema"] == 1 and "project-authored" in manifest["ownership"].lower()
    assert manifest["oracle"] == ORACLE_ID and isinstance(manifest["vectors"], list)
    assert len(manifest["vectors"]) == 2
    expected_keys = {"id", "provenance", "image_hex", "sha256", "pc", "offset", "sr", "budget", "d",
                     "block_instruction_counts", "expected_blocks", "expected_edges", "expected_boundary_blocks",
                     "expected_boundary_states"}
    seen_ids = set()
    for vector in manifest["vectors"]:
        assert set(vector) == expected_keys
        assert vector["id"] not in seen_ids and vector["id"].replace("-", "").isalnum()
        seen_ids.add(vector["id"])
        assert vector["provenance"] == "synthetic/SEG-003/direct-loop-v1"
        assert fixed_hex(vector["image_hex"], 16)
        image = bytes.fromhex(vector["image_hex"])
        assert hashlib.sha256(image).hexdigest() == vector["sha256"]
        assert all(fixed_hex(vector[key], width) for key, width in (("pc", 8), ("offset", 16), ("sr", 4), ("budget", 16)))
        assert int(vector["budget"], 16) > 0
        assert isinstance(vector["d"], list) and len(vector["d"]) == 8 and all(fixed_hex(value, 8) for value in vector["d"])
        assert isinstance(vector["block_instruction_counts"], list) and len(vector["block_instruction_counts"]) == int(vector["budget"], 16)
        assert all(isinstance(count, int) and count > 0 for count in vector["block_instruction_counts"])
        assert all(count <= len(image) // 2 for count in vector["block_instruction_counts"])
        expected_blocks = vector["expected_blocks"]
        assert isinstance(expected_blocks, list) and expected_blocks
        block_entries = set()
        for block in expected_blocks:
            assert set(block) == {"entry", "raw_instructions"} and fixed_hex(block["entry"], 8)
            assert block["entry"] not in block_entries and block["raw_instructions"]
            block_entries.add(block["entry"])
            assert all(fixed_hex(raw, 4) for raw in block["raw_instructions"])
        assert len(vector["expected_edges"]) == int(vector["budget"], 16)
        for edge in vector["expected_edges"]:
            assert set(edge) == {"kind", "condition", "source_block", "source_address", "target"}
            assert edge["kind"] in {"fallthrough", "bne_taken", "bne_fallthrough", "bra_taken"}
            assert edge["condition"] in {"always", "z_clear", "z_set"}
            assert edge["source_block"] in block_entries
            assert all(fixed_hex(edge[key], 8) for key in ("source_block", "source_address", "target"))
        assert len(vector["expected_boundary_states"]) == int(vector["budget"], 16) + 1
        assert len(vector["expected_boundary_blocks"]) == len(vector["expected_boundary_states"])
        assert all(entry in block_entries for entry in vector["expected_boundary_blocks"])
        for state in vector["expected_boundary_states"]:
            require_state(state, state)
            assert len(state["d"]) == 8 and all(fixed_hex(value, 8) for value in state["d"])
            assert fixed_hex(state["pc"], 8) and fixed_hex(state["sr"], 4)
    assert seen_ids == {"loop-bne-taken-and-fallthrough", "bne-fallthrough"}
    full_loop = next(vector for vector in manifest["vectors"] if vector["id"] == "loop-bne-taken-and-fallthrough")
    assert {edge["kind"] for edge in full_loop["expected_edges"]} >= {"bne_taken", "bne_fallthrough"}
    return manifest


def command(executable, image, vector):
    return [str(executable), "emit-direct-flow-c", str(image), vector["pc"], vector["offset"], vector["sr"],
            vector["budget"], *vector["d"]]


def resolve_compiler(compiler):
    resolved = shutil.which(compiler)
    assert resolved is not None, f"compiler unavailable: {compiler}"
    return Path(resolved).resolve()


def rejection_record(category, source_address, available_bytes, requested_length, instruction_length, raw_bytes, target):
    return {
        "category": category,
        "cpu_variant": "mc68000",
        "block_entry": "0x00000100",
        "source_address": source_address,
        "image_offset": 0,
        "available_bytes": available_bytes,
        "requested_length": requested_length,
        "instruction_length": instruction_length,
        "raw_bytes": raw_bytes,
        "target": target,
        "edge": True if target is not None else None,
        "mapping_claims": [],
        "unresolved_reason": None,
    }


def require_cli_rejections(executable, compiler, temporary):
    address = lambda value: {"space": "m68k_program", "value": value}
    cases = (
        ("truncated_instruction", bytes.fromhex("6600"), rejection_record(
            "truncated_instruction", address("0x00000100"), 2, 4, 2, "6600", None)),
        ("odd_direct_target", bytes.fromhex("6001"), rejection_record(
            "odd_direct_target", address("0x00000100"), 0, 0, None, "6001", address("0x00000103"))),
        ("unmapped_direct_target", bytes.fromhex("607E"), rejection_record(
            "unmapped_direct_target", address("0x00000100"), 0, 0, None, "607E", address("0x00000180"))),
        ("valid_but_unsupported_instruction", bytes.fromhex("4E71"), rejection_record(
            "valid_but_unsupported_instruction", address("0x00000100"), 2, 2, 2, "4E71", None)),
    )
    vector = {"pc": "00000100", "offset": "0000000000000000", "sr": "2700", "budget": "0000000000000001",
              "d": ["00000000"] * 8}
    for case_id, image_bytes, expected in cases:
        image = temporary / f"{case_id}.bin"
        source = temporary / f"{case_id}.c"
        binary = temporary / case_id
        image.write_bytes(image_bytes)
        first = subprocess.run(command(executable, image, vector), text=True, capture_output=True, check=False)
        repeated = subprocess.run(command(executable, image, vector), text=True, capture_output=True, check=False)
        expected_stderr = json.dumps(expected, separators=(",", ":")) + "\n"
        assert first.returncode == 1 and first.stdout == "" and first.stderr == expected_stderr, (case_id, first)
        assert repeated.returncode == 1 and repeated.stdout == "" and repeated.stderr == expected_stderr, (case_id, repeated)
        # A rejection produces no C translation: there is no source artifact to compile or execute.
        assert not source.exists() and not binary.exists(), case_id


def require_edge(edge, expected, expected_raw, mapping_begin):
    assert set(edge) == {"source_block", "source_instruction", "kind", "condition", "target"}
    assert edge["kind"] == expected["kind"] and edge["condition"] == expected["condition"]
    assert edge["source_block"] == {"space": "m68k_program", "value": "0x" + expected["source_block"]}
    assert edge["target"] == {"space": "m68k_program", "value": "0x" + expected["target"]}
    require_provenance(edge["source_instruction"])
    source = edge["source_instruction"]
    assert source["source_address"] == {"space": "m68k_program", "value": "0x" + expected["source_address"]}
    assert source["image_offset"] == int(expected["source_address"], 16) - mapping_begin
    assert source["raw_bytes"] == expected_raw[expected["source_block"]][-1]
    assert source["length"] == 2


def require_instruction(instruction, address, raw, mapping_begin):
    require_provenance(instruction)
    assert instruction["source_address"] == {"space": "m68k_program", "value": f"0x{address:08X}"}
    assert instruction["image_offset"] == address - mapping_begin
    assert instruction["raw_bytes"] == raw
    assert instruction["length"] == len(raw) // 2


def require_report(report, vector):
    budget = int(vector["budget"], 16)
    assert set(report) == {"boundaries", "final_state", "executed_blocks", "stop_reason"}
    assert report["executed_blocks"] == budget and report["stop_reason"] == "instruction_budget_exhausted"
    assert len(report["boundaries"]) == budget + 1
    require_state(report["final_state"], vector["expected_boundary_states"][-1])
    expected_raw = {block["entry"]: block["raw_instructions"] for block in vector["expected_blocks"]}
    mapping_begin = int(vector["pc"], 16) - int(vector["offset"], 16)
    for ordinal, (boundary, state, entry) in enumerate(zip(report["boundaries"], vector["expected_boundary_states"], vector["expected_boundary_blocks"])):
        assert set(boundary) == {"ordinal", "block", "incoming_edge", "outgoing_edge", "edge", "state", "executed_blocks", "budget", "stop_reason"}
        assert boundary["ordinal"] == ordinal and boundary["executed_blocks"] == ordinal and boundary["budget"] == budget
        assert boundary["stop_reason"] == ("instruction_budget_exhausted" if ordinal == budget else "continue")
        require_state(boundary["state"], state)
        block = boundary["block"]
        assert set(block) == {"id", "entry_instruction", "instructions"}
        assert block["id"] == {"space": "m68k_program", "value": "0x" + entry}
        expected_instructions = expected_raw[entry]
        assert len(block["instructions"]) == len(expected_instructions)
        address = int(entry, 16)
        require_instruction(block["entry_instruction"], address, expected_instructions[0], mapping_begin)
        for instruction, raw in zip(block["instructions"], expected_instructions):
            require_instruction(instruction, address, raw, mapping_begin)
            address += len(raw) // 2
        if ordinal == 0:
            assert boundary["incoming_edge"] == "reset_seed" and boundary["outgoing_edge"] is None and boundary["edge"] is None
        else:
            assert boundary["incoming_edge"] == ("reset_seed" if ordinal == 1 else report["boundaries"][ordinal - 1]["outgoing_edge"])
            require_edge(boundary["outgoing_edge"], vector["expected_edges"][ordinal - 1], expected_raw, mapping_begin)
            assert boundary["edge"] == boundary["outgoing_edge"]


def pinned_oracle():
    adapter = os.environ.get("SEGARECOMP_DIRECT_FLOW_MUSASHI_ORACLE")
    if not adapter:
        return None
    adapter_path = Path(adapter)
    assert adapter_path.is_file() and os.access(adapter_path, os.X_OK), \
        f"direct-flow Musashi adapter is not executable: {adapter_path}"
    checkout = adapter_path.parent / "musashi"
    assert checkout.is_dir(), f"Musashi checkout adjacent to adapter is unavailable: {checkout}"
    revision = subprocess.run(["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True, capture_output=True, check=False)
    assert revision.returncode == 0 and revision.stdout.strip() == ORACLE_REVISION, \
        f"Musashi checkout is not pinned at {ORACLE_REVISION}: {checkout}"
    return adapter_path


def run_oracle(adapter, vector, temporary):
    source = temporary / f"{vector['id']}-oracle-input.json"
    destination = temporary / f"{vector['id']}-oracle-output.json"
    payload = {"image_hex": vector["image_hex"].lower(), "image_sha256": vector["sha256"].upper(),
               "reset_entry": "0x" + vector["pc"].lstrip("0").lower(),
               "d": ["0x" + value.lower() for value in vector["d"]], "sr": "0x" + vector["sr"].lower(),
               "block_instruction_counts": vector["block_instruction_counts"]}
    source.write_text(json.dumps(payload, separators=(",", ":")) + "\n")
    result = subprocess.run([str(adapter), "--input", str(source), "--output", str(destination), "--blocks", str(int(vector["budget"], 16))], text=True, capture_output=True, check=False)
    assert result.returncode == 0 and result.stdout == "" and result.stderr == "", (vector["id"], result)
    oracle = json.loads(destination.read_text())
    assert set(oracle) == {"oracle", "executed_blocks", "boundaries", "stop_reason"}
    assert oracle["oracle"] == ORACLE_ID and oracle["executed_blocks"] == int(vector["budget"], 16)
    assert oracle["stop_reason"] == "instruction_budget_exhausted"
    assert oracle["boundaries"] == vector["expected_boundary_states"][1:]
    return oracle


def main():
    executable = Path(sys.argv[1])
    compiler = resolve_compiler(sys.argv[2])
    manifest = validate_manifest()
    adapter = pinned_oracle()
    with tempfile.TemporaryDirectory() as temporary_name:
        temporary = Path(temporary_name)
        require_cli_rejections(executable, compiler, temporary)
        for vector in manifest["vectors"]:
            image = temporary / f"{vector['id']}.bin"
            image.write_bytes(bytes.fromhex(vector["image_hex"]))
            translation = subprocess.run(command(executable, image, vector), text=True, capture_output=True, check=False)
            repeated_translation = subprocess.run(command(executable, image, vector), text=True, capture_output=True, check=False)
            assert translation.returncode == 0 and translation.stderr == "", (vector["id"], translation.stderr)
            assert repeated_translation.returncode == 0 and repeated_translation.stderr == "" and repeated_translation.stdout == translation.stdout
            source_lower = translation.stdout.lower()
            assert vector["image_hex"].lower() not in source_lower
            assert all(token not in source_lower for token in ("fopen", "fread", "open(", "read(", "getchar", "scanf", "decoder", "opcode", "uint8_t"))
            assert "switch(pc)" not in source_lower and re.search(r"static void m68k_block_[0-9a-f]{8}\(", source_lower)
            source = temporary / f"{vector['id']}.c"
            binary = temporary / vector["id"]
            source.write_text(translation.stdout)
            compiled = subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(binary)], text=True, capture_output=True, check=False)
            assert compiled.returncode == 0, compiled.stderr
            executed = subprocess.run([str(binary)], text=True, capture_output=True, check=False)
            repeated_execution = subprocess.run([str(binary)], text=True, capture_output=True, check=False)
            assert executed.returncode == 0 and executed.stderr == "" and repeated_execution.returncode == 0 and repeated_execution.stderr == ""
            assert repeated_execution.stdout == executed.stdout
            require_report(json.loads(executed.stdout), vector)
            if adapter is not None:
                oracle = run_oracle(adapter, vector, temporary)
                generated_boundaries = json.loads(executed.stdout)["boundaries"][1:]
                assert [boundary["state"] for boundary in generated_boundaries] == oracle["boundaries"]
    status = "compared pinned Musashi oracle" if adapter is not None else "pinned Musashi oracle unavailable; static evidence completed"
    print(f"validated static direct-flow boundaries, BNE outcomes, determinism, and C11 compilation ({status})")


if __name__ == "__main__":
    main()
