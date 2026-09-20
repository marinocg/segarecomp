#!/usr/bin/env python3
"""Exercise public Genesis reset analysis with project-owned synthetic bytes."""
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "tests/fixtures/genesis-reset-image-fixtures.json"

def composite(pc=0x150):
    data = bytearray(0x200)
    data[0:4] = (0x00FF0000).to_bytes(4, "big")
    data[4:8] = pc.to_bytes(4, "big")
    data[0x100:0x110] = b"SEGA GENESIS    "
    title = b"SYNTHETIC RESET"
    data[0x120:0x150] = title + b" " * (48 - len(title))
    return bytes(data)

def reset_fixture(entry):
    if entry["id"] == "recognized-genesis-composite":
        return composite()
    data = bytearray(entry["size"])
    identifier = entry["id"]
    if identifier == "ssp-one-byte": data[0] = 0x12
    elif identifier == "ssp-two-bytes": data[0:2] = b"\x12\x34"
    elif identifier == "ssp-three-bytes": data[0:3] = b"\x12\x34\x56"
    if identifier in {"ssp-only", "pc-one-byte", "pc-two-bytes", "pc-three-bytes"}:
        data[0:4] = (0x12345678).to_bytes(4, "big")
    values = {"valid-be32-entry": (0x00FF0000, 6), "pc-high-byte": (0, 0x01000000),
              "pc-high-byte-and-odd": (0, 0x01000001), "pc-odd": (0, 7),
              "pc-odd-unmapped": (0, 9), "pc-at-image-end": (0, 8),
              "pc-even-unmapped": (0, 10), "four-mebibyte-last-even": (0, 0x003FFFFE),
              "four-mebibyte-window-end": (0, 0x00400000)}
    if identifier in values:
        ssp, pc = values[identifier]
        data[0:4] = ssp.to_bytes(4, "big"); data[4:8] = pc.to_bytes(4, "big")
    return bytes(data)

def canonical_expected(entry):
    assert list(entry["expected_report"]) == json.loads(MANIFEST.read_text())["report_field_order"]
    return json.dumps(entry["expected_report"], separators=(",", ":")) + "\n"

def run(executable, image):
    return subprocess.run([executable, "analyze", image], text=True, capture_output=True, check=False)

def main():
    executable = Path(sys.argv[1])
    entries = json.loads(MANIFEST.read_text())["fixtures"]
    for entry in entries:
        assert all(key in entry for key in ("id", "purpose", "construction", "size", "sha256", "outcome", "diagnostic", "expected_report"))
        assert hashlib.sha256(reset_fixture(entry)).hexdigest() == entry["sha256"]
    record = next(entry for entry in entries if entry["id"] == "recognized-genesis-composite")
    good = composite()
    assert hashlib.sha256(good).hexdigest() == record["sha256"]
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        good_path = directory / "good.bin"; good_path.write_bytes(good)
        accepted = run(executable, good_path)
        expected_text = canonical_expected(record)
        assert accepted.returncode == 0 and accepted.stderr == "" and accepted.stdout == expected_text
        repeated = run(executable, good_path)
        assert repeated.returncode == 0 and repeated.stdout == accepted.stdout and repeated.stderr == accepted.stderr
        odd = composite(0x151); odd_path = directory / "odd.bin"; odd_path.write_bytes(odd)
        rejected = run(executable, odd_path)
        expected_rejected = json.dumps({**record["expected_report"], "diagnostic": "GENESIS_RESET_PC_ODD", "outcome": "rejected", "exit_class": "input_rejected", "initial_pc_word": 337, "initial_pc": 337, "entry_address": None, "entry_image_offset": None, "pc_alignment": "odd", "pc_mapping": "not_checked", "source_provenance": {**record["expected_report"]["source_provenance"], "pc": {"offset": 4, "length": 4, "status": "available", "bytes": [0, 0, 1, 81], "word": 337}}}, separators=(",", ":")) + "\n"
        assert rejected.returncode == 2 and rejected.stderr == "" and rejected.stdout == expected_rejected
        sms = bytearray(0x2000); sms[0x1ff0:0x1ff8] = b"TMR SEGA"; sms[0x1fff] = 0x40
        sms_path = directory / "sms.bin"; sms_path.write_bytes(sms)
        non_genesis = run(executable, sms_path)
        assert non_genesis.returncode == 1 and non_genesis.stdout == "" and non_genesis.stderr == "segarecomp: HDR_RECOGNIZED_SMS\n"
        invalid = bytearray(0x110); invalid[0x100:0x110] = b"SEGA 32X        "
        invalid_path = directory / "invalid.bin"; invalid_path.write_bytes(invalid)
        classification_rejected = run(executable, invalid_path)
        assert classification_rejected.returncode == 1 and classification_rejected.stdout == "" and classification_rejected.stderr == "segarecomp: HDR_GENESIS_SYSTEM_UNSUPPORTED\n"
    print("validated Genesis analyze reports and classification gating")

if __name__ == "__main__": main()
