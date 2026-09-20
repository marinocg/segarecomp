#!/usr/bin/env python3
"""Focused black-box checks for the bounded SEG-004 common frontend."""
import hashlib, json, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
manifest = json.loads((ROOT / "tests/fixtures/m68k-frontend-vectors.json").read_text())
assert hashlib.sha256(bytes.fromhex(manifest["image"]["bytes"])).hexdigest() == manifest["image"]["sha256"]
# The API-level test exercises these malformed inputs; retain their contract
# rows beside the black-box fixture without inventing an opcode-specific CLI.
assert [(row["id"], row["category"]) for row in manifest["frontend_pre_read_negatives"]] == [
    ("invalid-image-source-id", "invalid_frontend_image_source_id"),
    ("image-byte-length-mismatch", "frontend_image_byte_length_mismatch"),
    ("invalid-mapping-span", "invalid_mapping_claim"),
    ("odd-source", "odd_instruction_address"),
    ("conflicting-source", "conflicting_address_mapping"),
    ("unmapped-source", "unmapped_instruction_address"),
    ("truncated-primary", "truncated_instruction"),
    ("truncated-extension-adjacent-claim", "truncated_instruction"),
]
exe, cc = map(Path, sys.argv[1:3])
with tempfile.TemporaryDirectory() as name:
    temp = Path(name); image = temp / "fixture.bin"; image.write_bytes(bytes.fromhex(manifest["image"]["bytes"]))
    common = [str(image), manifest["image"]["source_id"], manifest["analysis_entries"][0]]
    for claim in manifest["claims"]:
        common += [claim["name"], claim["target_begin"], claim["target_end"], claim["image_begin"], claim["image_end"]]
    first = subprocess.run([exe, "m68k-frontend", *common], text=True, capture_output=True, check=True)
    again = subprocess.run([exe, "m68k-frontend", *common], text=True, capture_output=True, check=True)
    assert first.stdout == again.stdout
    report = json.loads(first.stdout)
    assert report == {"result":"accepted", "blocks":3, "edges":4, "units":[{"ordinal":0,"id":"m68k_unit_00000000","members":["0x00000100"]},{"ordinal":1,"id":"m68k_unit_00000001","members":["0x00000110","0x00000116"]}]}
    # The report is a JSON boundary, so arbitrary source identities must not
    # make the rejection report invalid or non-deterministic.
    escaped_source_id = 'synthetic/"backslash\\newline\n tab\t'
    malformed = [str(image), escaped_source_id, manifest["analysis_entries"][0],
                 "bad", "00000100", "00000103", "0000000000000000", "0000000000000002"]
    rejected = subprocess.run([exe, "m68k-frontend", *malformed], text=True, capture_output=True)
    rejected_again = subprocess.run([exe, "m68k-frontend", *malformed], text=True, capture_output=True)
    assert rejected.returncode == 1 and rejected.stderr == rejected_again.stderr
    escaped_report = json.loads(rejected.stderr)
    assert escaped_report == {"result":"rejected", "category":"invalid_mapping_claim",
                              "image_source_id":escaped_source_id}
    for vector in manifest["vectors"]:
        cargs = [str(image), manifest["image"]["source_id"], manifest["analysis_entries"][0], vector["entry"], vector["sr"], vector["budget"], vector["d0"], *["22222222","33333333","44444444","55555555","66666666","77777777","88888888"]]
        for claim in manifest["claims"]: cargs += [claim["name"], claim["target_begin"], claim["target_end"], claim["image_begin"], claim["image_end"]]
        generated = subprocess.run([exe, "emit-m68k-frontend-c", *cargs], text=True, capture_output=True, check=True).stdout
        assert generated == subprocess.run([exe, "emit-m68k-frontend-c", *cargs], text=True, capture_output=True, check=True).stdout
        assert "m68k_unit_00000000" in generated and "m68k_unit_00000001" in generated
        assert "static int m68k_unit_00000000" in generated and "static int m68k_unit_00000001" in generated
        assert "m68k_block_" not in generated
        assert generated.count("static int dispatch(") == 1
        # The unit bodies own the lifted arithmetic/CCR statements.  The outer
        # dispatcher may select only static unit identities.
        unit_zero = generated.split("static int m68k_unit_00000000", 1)[1].split("static int m68k_unit_00000001", 1)[0]
        unit_one = generated.split("static int m68k_unit_00000001", 1)[1].split("static int dispatch(", 1)[0]
        dispatch = generated.split("static int dispatch(", 1)[1].split("int main(void)", 1)[0]
        assert "d[0] = UINT32_C" in unit_zero and "d[0] = UINT32_C" in unit_one
        assert "m68k_unit_00000000" in dispatch and "m68k_unit_00000001" in dispatch
        assert "UINT32_C(0x00000100)" not in dispatch and "UINT32_C(0x00000110)" not in dispatch
        assert "7002600c" not in generated.lower()  # no embedded target image
        source=temp/f"{vector['id']}.c"; binary=temp/vector["id"]; source.write_text(generated)
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", source, "-o", binary], check=True)
        output=json.loads(subprocess.run([binary], text=True, capture_output=True, check=True).stdout)
        assert output["stop_reason"] == "instruction_budget_exhausted"
        expected = vector["boundaries"]
        assert len(output["boundaries"]) == len(expected)
        for actual, wanted in zip(output["boundaries"], expected):
            assert actual["state"]["d"][0] == wanted["d0"]
            assert actual["state"]["d"][1:] == ["22222222","33333333","44444444","55555555","66666666","77777777","88888888"]
            assert actual["state"]["sr"] == wanted["sr"] and actual["state"]["pc"] == wanted["pc"]
        assert output["boundaries"][0]["block"]["entry_instruction"]["source_address"]["value"] == "0x" + vector["entry"]
