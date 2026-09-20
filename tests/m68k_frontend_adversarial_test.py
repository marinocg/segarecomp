#!/usr/bin/env python3
"""Independent black-box adversarial checks for the bounded SEG-004 frontend."""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "tests/fixtures/m68k-frontend-vectors.json"
ORACLE_REVISION = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
ORACLE_ID = f"musashi@{ORACLE_REVISION}"


def resolve_compiler(compiler):
    resolved = shutil.which(compiler)
    assert resolved is not None, f"compiler unavailable: {compiler}"
    return Path(resolved).resolve()


def claims(arguments, manifest):
    for claim in manifest["claims"]:
        arguments.extend((claim["name"], claim["target_begin"], claim["target_end"],
                          claim["image_begin"], claim["image_end"]))
    return arguments


def frontend_command(executable, image, source_id, entry, claim_arguments):
    return [str(executable), "m68k-frontend", str(image), source_id, entry, *claim_arguments]


def emit_command(executable, image, manifest, vector):
    arguments = [str(executable), "emit-m68k-frontend-c", str(image),
                 manifest["image"]["source_id"], manifest["analysis_entries"][0],
                 vector["entry"], vector["sr"], vector["budget"], vector["d0"],
                 "22222222", "33333333", "44444444", "55555555", "66666666",
                 "77777777", "88888888"]
    return claims(arguments, manifest)


def rejection(category, source_id):
    return {"result": "rejected", "category": category, "image_source_id": source_id}


def require_rejections(executable, temporary):
    # These are independent synthetic inputs.  Each assertion compares the full
    # externally visible rejection record, including the retained source ID.
    cases = (
        ("illegal-primary", "4afc", "00000100", [("mapped", "00000100", "00000102", "0000000000000000", "0000000000000002")], "illegal_instruction"),
        ("unsupported-primary", "7201", "00000100", [("mapped", "00000100", "00000102", "0000000000000000", "0000000000000002")], "valid_but_unsupported_instruction"),
        ("zero-displacement-extension", "66000002", "00000100", [("mapped", "00000100", "00000104", "0000000000000000", "0000000000000004")], "valid_but_unsupported_instruction"),
        ("truncated-primary", "70", "00000100", [("short", "00000100", "00000101", "0000000000000000", "0000000000000001")], "truncated_instruction"),
        # The adjacent claim deliberately contains the extension bytes.  A
        # claim-bounded read must still reject at the first claim.
        ("truncated-extension", "66000002", "00000100", [("primary", "00000100", "00000102", "0000000000000000", "0000000000000002"), ("adjacent", "00000102", "00000104", "0000000000000002", "0000000000000004")], "truncated_instruction"),
        ("odd-source-precedes-map", "7001", "00000101", [("mapped", "00000100", "00000102", "0000000000000000", "0000000000000002")], "odd_instruction_address"),
        ("conflicting-source", "7001", "00000100", [("first", "00000100", "00000102", "0000000000000000", "0000000000000002"), ("second", "00000100", "00000102", "0000000000000000", "0000000000000002")], "conflicting_address_mapping"),
        ("unmapped-source", "7001", "00000200", [("mapped", "00000100", "00000102", "0000000000000000", "0000000000000002")], "unmapped_instruction_address"),
        # An unequal claim is invalid before source resolution or opcode decode.
        ("invalid-claim-precedes-decode", "4afc", "00000100", [("bad", "00000100", "00000103", "0000000000000000", "0000000000000002")], "invalid_mapping_claim"),
        ("odd-direct-target", "6001", "00000100", [("mapped", "00000100", "00000102", "0000000000000000", "0000000000000002")], "odd_direct_target"),
        ("unmapped-direct-target", "607e", "00000100", [("mapped", "00000100", "00000102", "0000000000000000", "0000000000000002")], "unmapped_direct_target"),
    )
    source_id = "synthetic/SEG-004/adversarial"
    for case_id, image_hex, entry, mappings, category in cases:
        image = temporary / f"{case_id}.bin"
        image.write_bytes(bytes.fromhex(image_hex))
        mapping_arguments = [field for mapping in mappings for field in mapping]
        command = frontend_command(executable, image, source_id, entry, mapping_arguments)
        first = subprocess.run(command, text=True, capture_output=True, check=False)
        repeated = subprocess.run(command, text=True, capture_output=True, check=False)
        expected = json.dumps(rejection(category, source_id), separators=(",", ":")) + "\n"
        assert first.returncode == 1 and first.stdout == "" and first.stderr == expected, (case_id, first)
        assert repeated.returncode == 1 and repeated.stdout == "" and repeated.stderr == expected, (case_id, repeated)


def require_static_source(source, image_hex):
    lowered = source.lower()
    assert image_hex.lower() not in lowered
    # No input image, byte access, opcode decoder, or dynamic PC dispatch may
    # appear in generated execution code.  Unit names are the only dispatcher
    # targets accepted by this slice.
    assert all(token not in lowered for token in
               ("uint8_t", "fopen", "fread", " open(", " read(", "opcode", "decoder", "decode"))
    assert "switch(pc)" not in lowered and "m68k_block_" not in lowered
    assert lowered.count("static int dispatch(") == 1
    dispatch = lowered.split("static int dispatch(", 1)[1].split("int main(void)", 1)[0]
    assert "m68k_unit_00000000" in dispatch and "m68k_unit_00000001" in dispatch
    assert "0x00000100" not in dispatch and "0x00000110" not in dispatch


def require_execution(report, vector):
    expected = vector["boundaries"]
    assert set(report) == {"boundaries", "final_state", "executed_blocks", "stop_reason"}
    assert report["executed_blocks"] == int(vector["budget"], 16)
    assert report["stop_reason"] == "instruction_budget_exhausted"
    assert len(report["boundaries"]) == len(expected)
    for actual, wanted in zip(report["boundaries"], expected):
        state = actual["state"]
        assert state["d"] == [wanted["d0"], "22222222", "33333333", "44444444",
                              "55555555", "66666666", "77777777", "88888888"]
        assert state["sr"] == wanted["sr"] and state["pc"] == wanted["pc"]
    assert report["final_state"] == report["boundaries"][-1]["state"]


def pinned_oracle():
    adapter = os.environ.get("SEGARECOMP_DIRECT_FLOW_MUSASHI_ORACLE")
    if not adapter:
        return None
    path = Path(adapter)
    assert path.is_file() and os.access(path, os.X_OK), f"oracle adapter is not executable: {path}"
    checkout = path.parent / "musashi"
    revision = subprocess.run(["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True,
                             capture_output=True, check=False)
    assert checkout.is_dir() and revision.returncode == 0 and revision.stdout.strip() == ORACLE_REVISION, \
        f"oracle checkout is not pinned at {ORACLE_REVISION}"
    return path


def compare_oracle(adapter, manifest, vector, report, temporary):
    input_path = temporary / f"{vector['id']}-oracle-input.json"
    output_path = temporary / f"{vector['id']}-oracle-output.json"
    # The fixture's execution budget counts static blocks, while the oracle
    # receives the independently declared instruction count for each block.
    # Mapping claims are needed because this image occupies two discontinuous
    # target ranges; compact image offsets are not target addresses.
    assert len(vector["block_instruction_counts"]) == int(vector["budget"], 16)
    assert all(isinstance(count, int) and count > 0 for count in vector["block_instruction_counts"])
    payload = {"fixture_id": manifest["fixture_id"], "vector_id": vector["id"],
               "image_hex": manifest["image"]["bytes"].lower(),
                "image_sha256": manifest["image"]["sha256"].upper(),
                "reset_entry": "0x" + vector["entry"].lstrip("0").lower(),
                "d": ["0x" + value.lower() for value in [vector["d0"], "22222222", "33333333", "44444444", "55555555", "66666666", "77777777", "88888888"]],
                "sr": "0x" + vector["sr"].lower(),
                "block_instruction_counts": vector["block_instruction_counts"],
                "mapping_claims": manifest["claims"]}
    input_path.write_text(json.dumps(payload, separators=(",", ":")) + "\n")
    result = subprocess.run([str(adapter), "--input", str(input_path), "--output", str(output_path),
                             "--blocks", str(int(vector["budget"], 16))], text=True, capture_output=True,
                            check=False)
    if result.returncode != 0:
        raise AssertionError(
            "oracle adapter does not accept the required vector identity and "
            "discontinuous mapping claims; no pinned differential evidence: " + result.stderr.strip())
    assert result.stdout == "" and result.stderr == "", result
    oracle = json.loads(output_path.read_text())
    assert oracle["oracle"] == ORACLE_ID and oracle["executed_blocks"] == int(vector["budget"], 16)
    assert oracle["stop_reason"] == "instruction_budget_exhausted"
    expected_boundaries = report["boundaries"][1:]
    assert len(oracle["boundaries"]) == len(expected_boundaries)
    for ordinal, (generated, oracle_boundary) in enumerate(zip(expected_boundaries, oracle["boundaries"])):
        assert generated["state"] == oracle_boundary, (vector["id"], ordinal, generated, oracle_boundary)


def main():
    executable = Path(sys.argv[1])
    compiler = resolve_compiler(sys.argv[2])
    manifest = json.loads(MANIFEST.read_text())
    image_bytes = bytes.fromhex(manifest["image"]["bytes"])
    assert manifest["ownership"] == "project-authored synthetic fixture"
    assert len(image_bytes) == manifest["image"]["byte_length"]
    assert hashlib.sha256(image_bytes).hexdigest() == manifest["image"]["sha256"]
    oracle = pinned_oracle()
    with tempfile.TemporaryDirectory() as name:
        temporary = Path(name)
        require_rejections(executable, temporary)
        image = temporary / "seg-004-fixture.bin"
        image.write_bytes(image_bytes)
        common = claims([str(image), manifest["image"]["source_id"], manifest["analysis_entries"][0]], manifest)
        first = subprocess.run([str(executable), "m68k-frontend", *common], text=True, capture_output=True, check=False)
        repeated = subprocess.run([str(executable), "m68k-frontend", *common], text=True, capture_output=True, check=False)
        assert first.returncode == 0 and first.stderr == "" and repeated.stdout == first.stdout
        assert json.loads(first.stdout) == {"result": "accepted", "blocks": 3, "edges": 4,
                                            "units": [{"ordinal": 0, "id": "m68k_unit_00000000", "members": ["0x00000100"]}, {"ordinal": 1, "id": "m68k_unit_00000001", "members": ["0x00000110", "0x00000116"]}]}
        for vector in manifest["vectors"]:
            command = emit_command(executable, image, manifest, vector)
            translation = subprocess.run(command, text=True, capture_output=True, check=False)
            repeated_translation = subprocess.run(command, text=True, capture_output=True, check=False)
            assert translation.returncode == 0 and translation.stderr == "" and repeated_translation.stdout == translation.stdout
            require_static_source(translation.stdout, manifest["image"]["bytes"])
            c_source = temporary / f"{vector['id']}.c"
            binary = temporary / vector["id"]
            c_source.write_text(translation.stdout)
            compiled = subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                                        str(c_source), "-o", str(binary)], text=True, capture_output=True, check=False)
            assert compiled.returncode == 0, compiled.stderr
            execution = subprocess.run([str(binary)], text=True, capture_output=True, check=False)
            repeated_execution = subprocess.run([str(binary)], text=True, capture_output=True, check=False)
            assert execution.returncode == 0 and execution.stderr == "" and repeated_execution.stdout == execution.stdout
            report = json.loads(execution.stdout)
            require_execution(report, vector)
            if oracle is not None:
                compare_oracle(oracle, manifest, vector, report, temporary)
    status = "compared pinned Musashi oracle" if oracle else "pinned Musashi oracle unavailable; no differential claim"
    print(f"validated SEG-004 frontend rejection precedence, static C11 execution, and determinism ({status})")


if __name__ == "__main__":
    main()
