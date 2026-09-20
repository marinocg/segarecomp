#!/usr/bin/env python3
"""Black-box validation for the bounded, project-owned MOVEQ slice."""
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/fixtures/moveq-fixtures.json"
ORACLE_VECTORS = ROOT / "tests/fixtures/moveq-oracle-vectors.json"
DEFAULT_D = ["11111111", "22222222", "33333333", "44444444", "55555555", "66666666", "77777777", "88888888"]
ORACLE_ID = "musashi@313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
STATE_KEYS = {"schema", "d", "pc", "sr", "stop_reason"}
ORACLE_RESULT_KEYS = STATE_KEYS | {"oracle", "executed_instructions"}

def command(executable, image, entry):
    return [str(executable), "emit-moveq-c", str(image), entry["pc"], entry["offset"],
             entry.get("sr", "0000"), *entry.get("d", DEFAULT_D)]

def fixed_hex(value, width):
    return isinstance(value, str) and len(value) == width and value == value.upper() and all(character in "0123456789ABCDEF" for character in value)

def validate_oracle_vectors():
    manifest = json.loads(ORACLE_VECTORS.read_text())
    assert set(manifest) == {"ownership", "schema", "vectors"}
    assert manifest["schema"] == 1
    assert "project-authored" in manifest["ownership"].lower()
    assert isinstance(manifest["vectors"], list) and len(manifest["vectors"]) == 8
    destinations, immediates, states = set(), [], set()
    for vector in manifest["vectors"]:
        assert set(vector) == {"id", "image_hex", "sha256", "pc", "offset", "sr", "d"}
        assert isinstance(vector["id"], str) and vector["id"].replace("-", "").isalnum()
        assert fixed_hex(vector["image_hex"], 4)
        assert isinstance(vector["sha256"], str) and len(vector["sha256"]) == 64
        assert hashlib.sha256(bytes.fromhex(vector["image_hex"])).hexdigest() == vector["sha256"]
        assert fixed_hex(vector["pc"], 8) and fixed_hex(vector["offset"], 16) and fixed_hex(vector["sr"], 4)
        assert isinstance(vector["d"], list) and len(vector["d"]) == 8
        assert all(fixed_hex(value, 8) for value in vector["d"])
        word = int(vector["image_hex"], 16)
        assert (word & 0xF100) == 0x7000
        destinations.add((word >> 9) & 7)
        immediate = word & 0xFF
        immediates.append(immediate if immediate < 0x80 else immediate - 0x100)
        states.add(tuple(vector["d"]))
    assert destinations == set(range(8))
    assert any(value == 0 for value in immediates)
    assert any(value > 0 for value in immediates) and any(value < 0 for value in immediates)
    assert len(states) == len(manifest["vectors"])
    return manifest["vectors"]

def parse_generated_state(result, vector):
    assert result.returncode == 0 and result.stderr == "", (vector["id"], result.stderr)
    state = json.loads(result.stdout)
    assert set(state) == STATE_KEYS and state["schema"] == 1
    assert isinstance(state["d"], list) and len(state["d"]) == 8
    assert all(fixed_hex(value, 8) for value in state["d"])
    assert fixed_hex(state["pc"], 8) and fixed_hex(state["sr"], 4)
    assert state["stop_reason"] == "instruction_budget_exhausted"
    return state

def run_oracle(adapter, vector, temporary):
    source = temporary / f"{vector['id']}-oracle-input.json"
    destination = temporary / f"{vector['id']}-oracle-output.json"
    source.write_text(json.dumps({"schema": 1, "image_hex": vector["image_hex"], "image_sha256": vector["sha256"],
                                  "pc": vector["pc"], "d": vector["d"], "sr": vector["sr"]}, separators=(",", ":")) + "\n")
    result = subprocess.run([str(adapter), "--input", str(source), "--output", str(destination), "--instructions", "1"], text=True, capture_output=True, check=False)
    assert result.returncode == 0 and result.stdout == "" and result.stderr == "", (vector["id"], result)
    oracle = json.loads(destination.read_text())
    assert set(oracle) == ORACLE_RESULT_KEYS and oracle["schema"] == 1
    assert oracle["oracle"] == ORACLE_ID and oracle["executed_instructions"] == 1
    assert isinstance(oracle["d"], list) and len(oracle["d"]) == 8
    assert all(fixed_hex(value, 8) for value in oracle["d"])
    assert fixed_hex(oracle["pc"], 8) and fixed_hex(oracle["sr"], 4)
    assert oracle["stop_reason"] == "instruction_budget_exhausted"
    return {key: oracle[key] for key in STATE_KEYS}

def main():
    executable, compiler = map(Path, sys.argv[1:3])
    manifest = json.loads(FIXTURES.read_text())
    assert manifest["schema"] == 1 and "project-authored" in manifest["ownership"].lower()
    oracle_adapter = None
    if "SEGARECOMP_MOVEQ_MUSASHI_ORACLE" in os.environ:
        oracle_adapter = Path(os.environ["SEGARECOMP_MOVEQ_MUSASHI_ORACLE"])
        assert oracle_adapter.is_file() and os.access(oracle_adapter, os.X_OK), "invalid SEGARECOMP_MOVEQ_MUSASHI_ORACLE"
    oracle_vectors = validate_oracle_vectors()
    with tempfile.TemporaryDirectory() as temporary:
        temporary = Path(temporary)
        for entry in manifest["fixtures"]:
            image_bytes = bytes.fromhex(entry["image_hex"])
            assert hashlib.sha256(image_bytes).hexdigest() == entry["sha256"], entry["id"]
            image = temporary / f"{entry['id']}.bin"; image.write_bytes(image_bytes)
            result = subprocess.run(command(executable, image, entry), text=True, capture_output=True, check=False)
            if "expected_category" in entry:
                expected = {"category": entry["expected_category"], "cpu_variant": "mc68000",
                            "source_address": {"space": "m68k_program", "value": "0x" + entry["pc"][-6:]},
                            "image_offset": "0x" + f"{int(entry['offset'], 16):02X}",
                            "available_bytes": entry["expected_available"], "requested_length": 2,
                            "instruction_length": entry["expected_instruction_length"]}
                assert result.returncode == 1 and result.stdout == "" and json.loads(result.stderr) == expected, (entry["id"], result)
                continue
            assert result.returncode == 0 and result.stderr == "", (entry["id"], result.stderr)
            repeated = subprocess.run(command(executable, image, entry), text=True, capture_output=True, check=False)
            assert repeated.returncode == 0 and repeated.stdout == result.stdout and repeated.stderr == ""
            # The translator retains source provenance as metadata, but never embeds source bytes or a fetch path.
            assert entry["image_hex"] not in result.stdout and "fopen" not in result.stdout and "getchar" not in result.stdout
            assert "segarecomp_moveq_source_address" in result.stdout and "segarecomp_moveq_image_offset" in result.stdout
            source = temporary / f"{entry['id']}.c"; binary = temporary / entry["id"]
            source.write_text(result.stdout)
            compiled = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", source, "-o", binary], text=True, capture_output=True, check=False)
            assert compiled.returncode == 0, compiled.stderr
            executed = subprocess.run([binary], text=True, capture_output=True, check=False)
            expected = {"schema": 1, "d": entry["expected_d"], "pc": "00000102", "sr": entry["expected_sr"], "stop_reason": "instruction_budget_exhausted"}
            assert executed.returncode == 0 and executed.stderr == "" and json.loads(executed.stdout) == expected, (entry["id"], executed)
        for vector in oracle_vectors:
            image = temporary / f"{vector['id']}.bin"; image.write_bytes(bytes.fromhex(vector["image_hex"]))
            result = subprocess.run(command(executable, image, vector), text=True, capture_output=True, check=False)
            repeated_translation = subprocess.run(command(executable, image, vector), text=True, capture_output=True, check=False)
            assert result.returncode == 0 and result.stderr == "" and repeated_translation.returncode == 0
            assert repeated_translation.stdout == result.stdout and repeated_translation.stderr == ""
            assert vector["image_hex"] not in result.stdout
            assert all(token not in result.stdout for token in ("fopen", "fread", "open(", "read(", "getchar", "scanf"))
            assert "segarecomp_moveq_source_address" in result.stdout and "segarecomp_moveq_image_offset" in result.stdout
            source = temporary / f"{vector['id']}.c"; binary = temporary / vector["id"]
            source.write_text(result.stdout)
            compiled = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", source, "-o", binary], text=True, capture_output=True, check=False)
            assert compiled.returncode == 0, compiled.stderr
            generated = parse_generated_state(subprocess.run([binary], text=True, capture_output=True, check=False), vector)
            repeated_generated = parse_generated_state(subprocess.run([binary], text=True, capture_output=True, check=False), vector)
            assert repeated_generated == generated
            if oracle_adapter is not None:
                assert generated == run_oracle(oracle_adapter, vector, temporary), vector["id"]
    print("validated static MOVEQ C translation, execution, source-provenanced rejections, and optional Musashi oracle comparison")

if __name__ == "__main__": main()
