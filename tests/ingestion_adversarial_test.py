#!/usr/bin/env python3
"""Independently adversarial validation of the public ingestion boundary."""

import hashlib
import json
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path


SIZE_LIMIT = 4 * 1024 * 1024
ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "tests/fixtures/header-classification-fixtures.json"


def inspection(size, outcome, diagnostic, candidates="", details=""):
    return (
        f"input-size: {size}\nsize-limit: {SIZE_LIMIT}\noutcome: {outcome}\n"
        f"diagnostic: {diagnostic}\ncandidates:\n{candidates}{details}"
    ).encode()


def genesis_candidate(status, system, title=b"", has_title_range=False):
    title_range = "  domestic-title-range: [0x120,0x150)\n" if has_title_range else ""
    return (
        f"- kind: genesis\n  offset: 0x100\n  status: {status}\n"
        f"  system-range: [0x100,0x110)\n{title_range}"
        f"  system-bytes: {system.hex().upper()}\n"
        f"  domestic-title-bytes: {title.hex().upper()}\n"
    )


def sms_candidate(offset, status):
    return (
        f"- kind: sms_gg\n  offset: 0x{offset:X}\n  status: {status}\n"
        f"  header-range: [0x{offset:X},0x{offset + 16:X})\n"
    )


def genesis_inspection(data, system_type, diagnostic):
    system = data[0x100:0x110]
    title = data[0x120:0x150]
    display = "".join(chr(value) if 0x20 <= value <= 0x7E else f"\\x{value:02X}"
                      for value in title.rstrip(b" "))
    candidate = genesis_candidate("valid", system, title, True)
    details = (
        f"family: genesis\nsystem-type: {system_type}\n"
        f"domestic-title-bytes: {title.hex().upper()}\n"
        f"domestic-title-display: {display}\n"
        "domestic-title-encoding: ascii_with_hex_escapes\n"
    )
    return inspection(len(data), "recognized", diagnostic, candidate, details)


def sms_inspection(data, offset, family, region_system, size_code, diagnostic):
    return inspection(
        len(data), "recognized", diagnostic, sms_candidate(offset, "valid"),
        f"family: {family}\nregion-system: {region_system}\nrom-size-code: {size_code}\n",
    )


def write_at(data, offset, value):
    data[offset:offset + len(value)] = value


def number(value):
    return int(value, 0) if isinstance(value, str) else value


def decode_title(value):
    return value.encode("utf-8").decode("unicode_escape").encode("latin1")


def materialize_manifest_fixture(entry):
    """Build a manifest image without sharing T002's test implementation."""
    size = number(entry["size"])
    data = bytearray(random.Random(entry["seed"]).randbytes(size) if "seed" in entry else size)

    def put(offset, value):
        start = number(offset)
        if start < size:
            data[start:min(size, start + len(value))] = value[:max(0, size - start)]

    systems = {
        "mega": b"SEGA MEGA DRIVE ",
        "genesis": b"SEGA GENESIS    ",
        "unsupported": b"SEGA 32X        ",
        "mega-prefix-15": b"SEGA MEGA DRIVE",
    }
    if "system" in entry:
        put("0x100", systems[entry["system"]])
    if "title" in entry:
        title = decode_title(entry["title"])
        put("0x120", title + b" " * (48 - len(title)))
    for offset, text in entry.get("writes", []):
        put(offset, text.encode("ascii"))
    sms = entry.get("sms", [])
    if sms and isinstance(sms[0], str):
        sms = [sms]
    for offset, region, low in sms:
        put(offset, b"TMR SEGA")
        region_offset = number(offset) + 15
        if region_offset < size:
            data[region_offset] = (region << 4) | low
    return bytes(data)


def assert_manifest_fixture_provenance_and_determinism(executable, directory):
    manifest = json.loads(MANIFEST.read_text())
    ownership = manifest.get("ownership")
    if not isinstance(ownership, str) or "synthetic" not in ownership.lower() or \
            "project-owned" not in ownership.lower():
        raise AssertionError("fixture manifest does not explicitly identify synthetic project-owned data")
    entries = manifest.get("fixtures")
    hashes = manifest.get("sha256")
    if not isinstance(entries, list) or not entries or not isinstance(hashes, dict) or not hashes:
        raise AssertionError("fixture manifest does not contain fixture data and SHA-256 values")

    recognized = 0
    for entry in entries:
        fixture_id = entry.get("id")
        if not isinstance(fixture_id, str) or not fixture_id:
            raise AssertionError("fixture manifest contains an unnamed fixture")
        data = materialize_manifest_fixture(entry)
        digest = hashlib.sha256(data).hexdigest()
        if hashes.get(fixture_id) != digest:
            raise AssertionError(f"{fixture_id}: SHA-256 mismatch: {digest}")

        expectation = entry.get("expect")
        if not isinstance(expectation, list) or not expectation or expectation[0] != "recognized":
            continue
        recognized += 1
        image = directory / f"manifest-{fixture_id}.bin"
        image.write_bytes(data)
        first_inspect = run([executable, "inspect", image])
        second_inspect = run([executable, "inspect", image])
        assert_result(f"{fixture_id} manifest inspect", first_inspect, 0, first_inspect.stdout,
                      first_inspect.stderr)
        assert_result(f"{fixture_id} manifest repeated inspect", second_inspect, 0,
                      first_inspect.stdout, first_inspect.stderr)
        first_emit = run([executable, "emit-c", image])
        second_emit = run([executable, "emit-c", image])
        assert_result(f"{fixture_id} manifest emit-c", first_emit, 0, first_emit.stdout,
                      first_emit.stderr)
        assert_result(f"{fixture_id} manifest repeated emit-c", second_emit, 0,
                      first_emit.stdout, first_emit.stderr)
    return len(entries), recognized


def accepted_genesis():
    data = bytearray(b"\xA5" * 0x400)
    write_at(data, 0x40, bytes(range(1, 33)))  # Deliberately non-metadata input payload.
    write_at(data, 0x100, b"SEGA GENESIS    ")
    write_at(data, 0x120, b"ADVERSARIAL GENESIS".ljust(48, b" "))
    return bytes(data)


def accepted_sms(region, size_code):
    data = bytearray(b"\x5A" * 0x2000)
    write_at(data, 0x80, bytes(range(33, 65)))  # Must not be embedded in generated C.
    write_at(data, 0x1FF0, b"TMR SEGA")
    data[0x1FFF] = (region << 4) | size_code
    return bytes(data)


def run(command):
    return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)


def assert_result(label, result, status, stdout, stderr=b""):
    if os.name == "nt" and "harness" in label:
        # A generated C harness prints through the Windows CRT text-mode stdout, which
        # translates "\n" to "\r\n"; the byte-exact CLI outputs above stay untranslated.
        result = subprocess.CompletedProcess(result.args, result.returncode,
                                             result.stdout.replace(b"\r\n", b"\n"),
                                             result.stderr.replace(b"\r\n", b"\n"))
    if result.returncode != status or result.stdout != stdout or result.stderr != stderr:
        raise AssertionError(
            f"{label}: expected status={status}, stdout={stdout!r}, stderr={stderr!r}; "
            f"got status={result.returncode}, stdout={result.stdout!r}, stderr={result.stderr!r}"
        )


def assert_rejected(executable, directory, label, data, diagnostic, candidates="", outcome="rejected"):
    image = directory / f"{label}.bin"
    image.write_bytes(data)
    expected = inspection(len(data), outcome, diagnostic, candidates)
    result = run([executable, "inspect", image])
    assert_result(label, result, 1, expected)


def assert_generated_text(label, emitted, input_payload):
    prohibited = (b"games", b"opcode", b"fetch", b"decode", b"uint8_t", b"unsigned char")
    lowered = emitted.lower()
    for token in prohibited:
        if token in lowered:
            raise AssertionError(f"{label}: generated C contains prohibited runtime/dependency token {token!r}")
    if input_payload in emitted or input_payload.hex().encode() in lowered:
        raise AssertionError(f"{label}: generated C embeds non-metadata input payload")
    lines = emitted.splitlines()
    if not lines or lines[0] != b"/* Metadata-only manifest generated by segarecomp; no image bytes or execution semantics. */":
        raise AssertionError(f"{label}: generated C does not declare its metadata-only envelope")
    if any(b"{" in line or b"}" in line for line in lines):
        raise AssertionError(f"{label}: generated C contains executable block syntax")


def assert_accepted(executable, compiler, directory, label, data, expected, payload):
    image = directory / f"{label}.bin"
    generated = directory / f"{label}.c"
    harness = directory / f"{label}_harness.c"
    image.write_bytes(data)

    first_inspect = run([executable, "inspect", image])
    second_inspect = run([executable, "inspect", image])
    assert_result(f"{label} inspect", first_inspect, 0, expected)
    assert_result(f"{label} repeated inspect", second_inspect, 0, expected)
    if first_inspect.stdout != second_inspect.stdout:
        raise AssertionError(f"{label}: inspect output is not byte-identical")

    first_emit = run([executable, "emit-c", image])
    second_emit = run([executable, "emit-c", image])
    assert_result(f"{label} emit-c", first_emit, 0, first_emit.stdout)
    assert_result(f"{label} repeated emit-c", second_emit, 0, first_emit.stdout)
    if first_emit.stdout != second_emit.stdout:
        raise AssertionError(f"{label}: emit-c output is not byte-identical")
    assert_generated_text(label, first_emit.stdout, payload)
    generated.write_bytes(first_emit.stdout)
    harness.write_text(
        "#include <stdio.h>\n"
        "extern const char segarecomp_inspection[];\n"
        "int main(void) { return fputs(segarecomp_inspection, stdout) == EOF; }\n"
    )
    for optimization in ("-O0", "-O2"):
        executable_path = directory / f"{label}{optimization[1:]}"
        command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", optimization,
                   generated, harness, "-o", executable_path]
        compiled = run(command)
        if compiled.returncode != 0:
            raise AssertionError(f"{label} {optimization}: strict C11 compile/link failed: "
                                 f"{compiled.stdout!r}{compiled.stderr!r}")
        executed = run([executable_path])
        assert_result(f"{label} {optimization} harness", executed, 0, expected)
        print("C11 command:", " ".join(str(argument) for argument in command))


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(f"usage: {sys.argv[0]} <segarecomp> [c-compiler]")
    executable = str(Path(sys.argv[1]).resolve())
    compiler = sys.argv[2] if len(sys.argv) == 3 else os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory(prefix="segarecomp-ingestion-adversarial-") as temporary:
        directory = Path(temporary)
        fixture_count, recognized_fixture_count = assert_manifest_fixture_provenance_and_determinism(
            executable, directory)
        assert_rejected(executable, directory, "empty", b"", "HDR_NOT_RECOGNIZED", outcome="unrecognized")

        partial_system = b"SEGA"
        partial_data = bytearray(0x104)
        write_at(partial_data, 0x100, partial_system)
        assert_rejected(executable, directory, "genesis_system_truncated", bytes(partial_data),
                        "HDR_GENESIS_SYSTEM_TRUNCATED",
                        genesis_candidate("system_truncated", partial_system))

        title_truncated = bytearray(0x120)
        write_at(title_truncated, 0x100, b"SEGA MEGA DRIVE ")
        assert_rejected(executable, directory, "genesis_title_truncated", bytes(title_truncated),
                        "HDR_GENESIS_DOMESTIC_TITLE_TRUNCATED",
                        genesis_candidate("title_truncated", b"SEGA MEGA DRIVE ", b"", True))

        unsupported = bytearray(0x150)
        write_at(unsupported, 0x100, b"SEGA 32X        ")
        assert_rejected(executable, directory, "genesis_unsupported", bytes(unsupported),
                        "HDR_GENESIS_SYSTEM_UNSUPPORTED",
                        genesis_candidate("unsupported_system", b"SEGA 32X        "))

        truncated_sms = bytearray(0x1FF8)
        write_at(truncated_sms, 0x1FF0, b"TMR SEGA")
        assert_rejected(executable, directory, "sms_header_truncated", bytes(truncated_sms),
                        "HDR_8BIT_HEADER_TRUNCATED", sms_candidate(0x1FF0, "truncated"))

        invalid_region = bytearray(0x2000)
        write_at(invalid_region, 0x1FF0, b"TMR SEGA")
        invalid_region[0x1FFF] = 0x2A
        assert_rejected(executable, directory, "sms_invalid_region", bytes(invalid_region),
                        "HDR_SMS_GG_REGION_INVALID", sms_candidate(0x1FF0, "invalid_region"))

        collision = bytearray(0x4000)
        for offset in (0x1FF0, 0x3FF0):
            write_at(collision, offset, b"TMR SEGA")
            collision[offset + 15] = 0x4C
        assert_rejected(executable, directory, "sms_collision", bytes(collision),
                        "HDR_HEADER_COLLISION", sms_candidate(0x1FF0, "valid") + sms_candidate(0x3FF0, "valid"))

        conflict = bytearray(0x2000)
        write_at(conflict, 0x100, b"SEGA GENESIS    ")
        write_at(conflict, 0x120, b"CONFLICT".ljust(48, b" "))
        write_at(conflict, 0x1FF0, b"TMR SEGA")
        conflict[0x1FFF] = 0x4D
        assert_rejected(executable, directory, "genesis_sms_conflict", bytes(conflict),
                        "HDR_HEADER_CONFLICT",
                        genesis_candidate("valid", b"SEGA GENESIS    ", b"CONFLICT".ljust(48, b" "), True) +
                        sms_candidate(0x1FF0, "valid"))

        assert_rejected(executable, directory, "size_limit", b"\0" * (SIZE_LIMIT + 1), "IMG_SIZE_LIMIT")

        genesis = accepted_genesis()
        sms = accepted_sms(4, 0xB)
        game_gear = accepted_sms(6, 0x7)
        assert_accepted(executable, compiler, directory, "genesis", genesis,
                        genesis_inspection(genesis, "genesis", "HDR_RECOGNIZED_GENESIS"), bytes(range(1, 33)))
        assert_accepted(executable, compiler, directory, "sms", sms,
                        sms_inspection(sms, 0x1FF0, "master_system", "sms_export", 0xB, "HDR_RECOGNIZED_SMS"),
                        bytes(range(33, 65)))
        assert_accepted(executable, compiler, directory, "game_gear", game_gear,
                        sms_inspection(game_gear, 0x1FF0, "game_gear", "gg_export", 0x7, "HDR_RECOGNIZED_GG"),
                        bytes(range(33, 65)))
    print(f"validated {fixture_count} manifest fixtures ({recognized_fixture_count} recognized with "
          "repeatable inspect/emit-c), 9 rejected boundary/signature cases and 3 accepted synthetic systems")


if __name__ == "__main__":
    main()
