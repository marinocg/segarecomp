#!/usr/bin/env python3
"""Adversarial black-box checks for the public Genesis reset-image report."""

import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "tests/fixtures/genesis-reset-image-fixtures.json"
SIZE_LIMIT = 0x400000
SOURCE_IDS = ["M1", "M2", "M3", "S1", "S2"]


def genesis_image(size, ssp_bytes, pc_bytes):
    """Build a project-authored recognized Genesis container with literal vector bytes."""
    data = bytearray(size)
    data[0:4] = ssp_bytes
    data[4:8] = pc_bytes
    data[0x100:0x110] = b"SEGA GENESIS    "
    data[0x120:0x150] = b"ADVERSARIAL RESET".ljust(48, b" ")
    return bytes(data)


def expected_report(image_size, ssp_bytes, pc_bytes, diagnostic, *, initial_pc=None,
                    alignment="not_checked", mapping="not_checked"):
    """Construct the public contract JSON, without calling project conversion code."""
    ssp_word = int.from_bytes(ssp_bytes, byteorder="big")
    pc_word = int.from_bytes(pc_bytes, byteorder="big")
    accepted = diagnostic == "GENESIS_RESET_IMAGE_ACCEPTED"
    entry = initial_pc if accepted else None
    return {
        "outcome": "accepted" if accepted else "rejected",
        "diagnostic": diagnostic,
        "exit_class": "success" if accepted else "input_rejected",
        "input_size": image_size,
        "size_limit": SIZE_LIMIT,
        "raw_region_status": "constructed",
        "raw_region": {
            "name": "raw_cartridge_rom",
            "cpu_address_range": [0, image_size],
            "image_range": [0, image_size],
            "permissions": ["read", "execute"],
            "mapping": "identity_non_mirrored",
        },
        "reset_ssp_range": {"start": 0, "end": 4, "status": "available"},
        "reset_pc_range": {"start": 4, "end": 8, "status": "available"},
        "initial_ssp": ssp_word,
        "initial_pc_word": pc_word,
        "initial_pc": initial_pc,
        "entry_address": entry,
        "entry_image_offset": entry,
        "pc_alignment": alignment,
        "pc_mapping": mapping,
        "source_provenance": {
            "input": "raw_image",
            "byte_order": "big_endian",
            "source_ids": SOURCE_IDS,
            "ssp": {"offset": 0, "length": 4, "status": "available",
                    "bytes": list(ssp_bytes), "word": ssp_word},
            "pc": {"offset": 4, "length": 4, "status": "available",
                   "bytes": list(pc_bytes), "word": pc_word},
        },
    }


def run(executable, image):
    return subprocess.run([executable, "analyze", image], text=True, capture_output=True,
                          check=False)


def assert_result(label, result, status, stdout, stderr=""):
    if (result.returncode, result.stdout, result.stderr) != (status, stdout, stderr):
        raise AssertionError(
            f"{label}: expected {(status, stdout, stderr)!r}; "
            f"got {(result.returncode, result.stdout, result.stderr)!r}")


def assert_analyze_case(executable, directory, label, data, report):
    image = directory / f"{label}.bin"
    image.write_bytes(data)
    expected = json.dumps(report, separators=(",", ":")) + "\n"
    first = run(executable, image)
    status = 0 if report["outcome"] == "accepted" else 2
    assert_result(label, first, status, expected)
    second = run(executable, image)
    assert_result(f"{label} repeat", second, status, expected)


def materialize_manifest_recipe(recipe):
    """Interpret only the documented synthetic fixture recipe notation."""
    parts = recipe.split("; ")
    match = re.fullmatch(r"zero_fill\((0x[0-9A-Fa-f]+|\d+)\)", parts[0])
    if not match:
        raise AssertionError(f"unrecognized zero-fill recipe: {recipe}")
    data = bytearray(int(match.group(1), 0))

    def put(offset, value):
        if offset < 0 or len(value) > len(data) - offset:
            raise AssertionError(f"fixture write [{offset}, {offset + len(value)}) exceeds image bounds")
        data[offset:offset + len(value)] = value

    for part in parts[1:]:
        if part.startswith("W32("):
            write = re.fullmatch(r"W32\((0x[0-9A-Fa-f]+|\d+),(0x[0-9A-Fa-f]+|\d+)\)", part)
            if not write:
                raise AssertionError(f"unrecognized vector write: {part}")
            offset, value = (int(value, 0) for value in write.groups())
            put(offset, value.to_bytes(4, "big"))
        elif part.startswith("byte("):
            write = re.fullmatch(r"byte\((0x[0-9A-Fa-f]+|\d+),(0x)?([0-9A-Fa-f]{2})\)", part)
            if not write:
                raise AssertionError(f"unrecognized byte write: {part}")
            put(int(write.group(1), 0), bytes((int(write.group(3), 16),)))
        elif part.startswith("bytes("):
            write = re.fullmatch(r"bytes\((0x[0-9A-Fa-f]+|\d+),([0-9A-Fa-f]+)\)", part)
            if not write:
                raise AssertionError(f"unrecognized byte sequence: {part}")
            offset, value = write.groups()
            put(int(offset, 0), bytes.fromhex(value))
        elif part.startswith("ASCII("):
            write = re.fullmatch(r"ASCII\((0x[0-9A-Fa-f]+),'([^']*)'\)( then space-pad title to 48)?", part)
            if not write:
                raise AssertionError(f"unrecognized ASCII write: {part}")
            offset, text, padded = write.groups()
            value = text.encode("ascii")
            if padded:
                value = value.ljust(48, b" ")
            start = int(offset, 0)
            put(start, value)
        else:
            raise AssertionError(f"unrecognized fixture recipe part: {part}")
    return bytes(data)


def literal_be32(vector):
    """Read four fixture bytes explicitly, independent of production conversion."""
    return ((vector[0] << 24) | (vector[1] << 16) |
            (vector[2] << 8) | vector[3])


def reset_report_oracle(data):
    """Derive the reset-layout report contract directly from literal image bytes."""
    image_size = len(data)
    provenance = {
        "input": "raw_image",
        "byte_order": "big_endian",
        "source_ids": SOURCE_IDS,
    }
    report = {
        "outcome": "rejected",
        "diagnostic": None,
        "exit_class": "input_rejected",
        "input_size": image_size,
        "size_limit": SIZE_LIMIT,
        "raw_region_status": None,
        "raw_region": None,
        "reset_ssp_range": None,
        "reset_pc_range": None,
        "initial_ssp": None,
        "initial_pc_word": None,
        "initial_pc": None,
        "entry_address": None,
        "entry_image_offset": None,
        "pc_alignment": None,
        "pc_mapping": None,
        "source_provenance": provenance,
    }

    def vector(offset, status, value=None):
        return {"offset": offset, "length": 4, "status": status,
                "bytes": list(data[offset:offset + 4]) if value is not None else None,
                "word": value}

    if image_size > SIZE_LIMIT:
        report.update({
            "diagnostic": "GENESIS_IMAGE_SIZE_LIMIT",
            "raw_region_status": "not_constructed",
            "reset_ssp_range": {"start": 0, "end": 4, "status": "unexamined"},
            "reset_pc_range": {"start": 4, "end": 8, "status": "unexamined"},
            "pc_alignment": "unexamined",
            "pc_mapping": "unexamined",
        })
        provenance["ssp"] = vector(0, "unexamined")
        provenance["pc"] = vector(4, "unexamined")
        return report

    report.update({
        "raw_region_status": "constructed",
        "raw_region": {
            "name": "raw_cartridge_rom",
            "cpu_address_range": [0, image_size],
            "image_range": [0, image_size],
            "permissions": ["read", "execute"],
            "mapping": "identity_non_mirrored",
        },
    })
    if image_size < 4:
        report.update({
            "diagnostic": "GENESIS_RESET_SSP_TRUNCATED",
            "reset_ssp_range": {"start": 0, "end": 4, "status": "unavailable"},
            "reset_pc_range": {"start": 4, "end": 8, "status": "unavailable"},
            "pc_alignment": "not_checked",
            "pc_mapping": "not_checked",
        })
        provenance["ssp"] = vector(0, "unavailable")
        provenance["pc"] = vector(4, "unavailable")
        return report

    ssp = literal_be32(data[0:4])
    report["initial_ssp"] = ssp
    report["reset_ssp_range"] = {"start": 0, "end": 4, "status": "available"}
    provenance["ssp"] = vector(0, "available", ssp)
    if image_size < 8:
        report.update({
            "diagnostic": "GENESIS_RESET_PC_TRUNCATED",
            "reset_pc_range": {"start": 4, "end": 8, "status": "unavailable"},
            "pc_alignment": "not_checked",
            "pc_mapping": "not_checked",
        })
        provenance["pc"] = vector(4, "unavailable")
        return report

    pc = literal_be32(data[4:8])
    report.update({
        "initial_pc_word": pc,
        "reset_pc_range": {"start": 4, "end": 8, "status": "available"},
    })
    provenance["pc"] = vector(4, "available", pc)
    if pc > 0xFFFFFF:
        report.update({
            "diagnostic": "GENESIS_RESET_PC_NOT_24BIT",
            "pc_alignment": "not_checked",
            "pc_mapping": "not_checked",
        })
        return report

    report["initial_pc"] = pc
    if pc & 1:
        report.update({
            "diagnostic": "GENESIS_RESET_PC_ODD",
            "pc_alignment": "odd",
            "pc_mapping": "not_checked",
        })
        return report
    if pc >= image_size:
        report.update({
            "diagnostic": "GENESIS_RESET_PC_UNMAPPED",
            "pc_alignment": "even",
            "pc_mapping": "unmapped",
        })
        return report

    report.update({
        "outcome": "accepted",
        "diagnostic": "GENESIS_RESET_IMAGE_ACCEPTED",
        "exit_class": "success",
        "entry_address": pc,
        "entry_image_offset": pc,
        "pc_alignment": "even",
        "pc_mapping": "mapped",
    })
    return report


def assert_manifest_provenance():
    manifest = json.loads(MANIFEST.read_text())
    if "project-owned" not in manifest.get("fixture_policy", "").lower() or \
            "synthetic" not in manifest.get("fixture_policy", "").lower():
        raise AssertionError("reset fixture manifest lacks synthetic project-owned provenance")
    valid = []
    for entry in manifest.get("fixtures", []):
        data = materialize_manifest_recipe(entry["construction"])
        if len(data) != entry["size"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
            raise AssertionError(f"{entry['id']}: manifest construction, size, or SHA-256 mismatch")
        report = entry["expected_report"]
        oracle = reset_report_oracle(data)
        if report != oracle:
            raise AssertionError(
                f"{entry['id']}: expected report disagrees with literal-byte reset oracle\n"
                f"expected: {report!r}\noracle: {oracle!r}")
        if list(report) != manifest["report_field_order"] or list(oracle) != list(report):
            raise AssertionError(f"{entry['id']}: reset report field order is not canonical")
        if entry["outcome"] != oracle["outcome"] or entry["diagnostic"] != oracle["diagnostic"]:
            raise AssertionError(f"{entry['id']}: fixture outcome metadata disagrees with literal-byte oracle")
        if report["source_provenance"]["source_ids"] != SOURCE_IDS:
            raise AssertionError(f"{entry['id']}: source IDs lost their contract provenance")
        if report["source_provenance"]["input"] != "raw_image" or \
                report["source_provenance"]["byte_order"] != "big_endian":
            raise AssertionError(f"{entry['id']}: vector provenance is not raw big-endian image data")
        if oracle["outcome"] == "accepted":
            valid.append(entry["id"])
    if valid != ["valid-be32-entry", "four-mebibyte-last-even", "recognized-genesis-composite"]:
        raise AssertionError(f"unexpected valid reset fixture coverage: {valid!r}")
    return manifest


def assert_bus_device_guard(production_texts):
    """Reject generic Bus/Device seams outside the bounded T021 declarations."""
    generic = re.compile(r"(?<![A-Za-z0-9_])(?:Bus|Device)\b|"
                         r"(?<![A-Za-z0-9])(?:bus_|device_)\w*")
    routing_declaration = re.compile(
        r"\[\[nodiscard\]\]\s+M68kGenesisDeviceRoutingResult\s+"
        r"(?P<routing>m68k_route_genesis_device_access)\s*\(\s*"
        r"const\s+M68kMemoryAccessRequest\s*&\s*request\s*\)\s*noexcept\s*;",
        re.MULTILINE)
    routing_definition = re.compile(
        r"M68kGenesisDeviceRoutingResult\s+"
        r"(?P<routing>m68k_route_genesis_device_access)\s*\(\s*"
        r"const\s+M68kMemoryAccessRequest\s*&\s*request\s*\)\s*noexcept\s*\{",
        re.MULTILINE)
    approved_route = re.compile(r"(?<![A-Za-z0-9_])"
                                 r"m68k_route_genesis_device_access(?![A-Za-z0-9_])")
    # T028 C1's host-only frontier taxonomy is not a bus/device seam. Keep
    # this exception exact so it cannot allow a generic device_access helper.
    approved_frontier_use = re.compile(
        r"(?<![A-Za-z0-9_])GenesisFrontierClass::unsupported_device_access"
        r"(?![A-Za-z0-9_])")
    approved_frontier_declaration = re.compile(
        r"enum\s+class\s+GenesisFrontierClass\s*\{"
        r"(?P<members>[^}]*)\};", re.DOTALL)
    # T028 C4 lowers the already retained StartupBusRecord provenance into the
    # fixed C11 GenesisProvenance ABI.  This is deliberately a function-body
    # span, not a global identifier exception: unrelated bus helpers remain
    # rejected by this reset-image scope guard.
    #
    # SEG-007-T064 factored the single-frontier stop-function body this span
    # already covered into its own free helper,
    # build_genesis_frontier_stop_function, called once per retained exit --
    # the same C4 provenance lowering, just reused per exit instead of
    # inlined once. The start anchor below moves to that helper's own
    # signature so the whitelisted span still opens before the first
    # bus_access_count/bus_accesses token it emits; the span still also
    # covers emit_m68k_general_startup_runtime_c(FrontendPartialProgram)/
    # emit_m68k_general_startup_bridge_c(FrontendPartialProgram, ...) exactly
    # as before.
    #
    # SEG-014-T005 correction: build_genesis_frontier_stop_function and every
    # bridge-emission function this span already covered physically moved
    # from platforms/genesis/machine/src/frontend.cpp into libs/codegen/c11/src/frontend.cpp
    # (see docs/architecture/seg-014-t001-symbol-migration-map.md), which
    # co-located them with no later format_m68k_frontend_result definition to
    # anchor against (that JSON report formatter stayed in
    # platforms/genesis/machine/src/frontend.cpp -- it is not C11 rendering). The end
    # anchor now matches through the physical end of this rendering-only
    # translation unit (`} // namespace segarecomp`, unique and always last
    # in the file), preserving the same "everything from this helper's
    # declared-signature start through the rest of the bridge-emission
    # functions" span the original anchor captured.
    # SEG-007-T174 / ADR-0024: `build_genesis_frontier_stop_function` gained a
    # 4th parameter, `emitted_code_addresses` (the Tier-2 EmittedCodeAddressSet
    # array, defaulted so every pre-existing call site stays source-compatible)
    # -- the start-anchor signature below is updated to match; the function's
    # own bus-provenance-lowering body and everything downstream of it is
    # otherwise unchanged and still fully covered by this same approved span.
    # SEG-007-T208 correction: a 5th parameter, `out_tier2_call_continuation`
    # (an optional out-parameter reporting a call-shaped Tier-2 site's own
    # already-computed continuation back to the caller -- see that
    # parameter's own doc comment in frontend.cpp), also defaulted so every
    # pre-existing call site stays source-compatible. Same disposition as the
    # 4th-parameter note directly above: only the start anchor changes.
    bridge_provenance_lowering = re.compile(
        r"std::optional<std::string>\s+build_genesis_frontier_stop_function\s*\(\s*"
        r"const\s+FrontendAnalysis\s*&\s*accepted_prefix\s*,\s*"
        r"const\s+UnresolvedFrontier\s*&\s*frontier\s*,\s*"
        r"const\s+std::set<Address>\s*&\s*sibling_frontier_addresses\s*,\s*"
        r"const\s+std::vector<std::uint32_t>\s*&\s*emitted_code_addresses\s*=\s*\{\}\s*,\s*"
        r"std::optional<std::uint32_t>\s*\*\s*out_tier2_call_continuation\s*=\s*nullptr\s*\)\s*\{(?P<body>.*?)"
        r"(?:\n\}\n\n#define\s+format_m68k_frontend_result|\n\}\s*//\s*namespace\s+segarecomp\s*\Z)",
        re.DOTALL)
    # The accepted no-completion bridge uses this exact private policy owner to
    # lower retained source/fetch provenance for routed failures.  Its body is
    # bounded by the anonymous namespace close; no other frontend symbol gets
    # a bus ABI exemption.
    bridge_policy_provenance_lowering = re.compile(
        r"std::string\s+emit_m68k_general_startup_runtime_c_with_policy\s*\(\s*"
        r"const\s+FrontendAnalysis\s*&\s*analysis\s*,\s*"
        r"M68kGeneralStartupBlockEmissionPolicy\s+policy\s*\)\s*\{(?P<body>.*?)\n\}\s*// namespace",
        re.DOTALL)

    for location, text in production_texts:
        routing_pattern = None
        if location == "include/segarecomp/m68k_pipeline.hpp":
            routing_pattern = routing_declaration
        elif location == "src/m68k_pipeline.cpp":
            routing_pattern = routing_definition
        approved_routing_components = [] if routing_pattern is None else [
            match.span("routing") for match in routing_pattern.finditer(text)]
        approved_route_components = [match.span() for match in approved_route.finditer(text)]
        approved_frontier_components = [match.span() for match in approved_frontier_use.finditer(text)]
        approved_frontier_components.extend(
            match.span("members") for match in approved_frontier_declaration.finditer(text))
        # SEG-014-T005 correction: build_genesis_frontier_stop_function and
        # emit_m68k_general_startup_runtime_c_with_policy physically moved
        # from platforms/genesis/machine/src/frontend.cpp (machine scenario ownership)
        # into libs/codegen/c11/src/frontend.cpp (C11 rendering ownership) with
        # identical bodies; see
        # docs/architecture/seg-014-t001-symbol-migration-map.md. Both
        # locations remain recognized so this guard keeps working regardless
        # of which side of that move a given checkout is on.
        bridge_provenance_locations = {"platforms/genesis/machine/src/frontend.cpp",
                                       "libs/codegen/c11/src/frontend.cpp"}
        approved_bridge_provenance_components = (
            [match.span("body") for match in bridge_provenance_lowering.finditer(text)]
            if location in bridge_provenance_locations else [])
        if location in bridge_provenance_locations:
            approved_bridge_provenance_components.extend(
                match.span("body") for match in bridge_policy_provenance_lowering.finditer(text))

        for match in generic.finditer(text):
            symbol = match.group(0)
            approved = symbol in {"bus_through_ordinal", "bus_records",
                                  "device_region_controller_io"}
            if symbol == "device_access":
                approved = any(start <= match.start() and match.end() <= end
                               for start, end in approved_routing_components)
                approved = approved or any(start <= match.start() and match.end() <= end
                                             for start, end in approved_route_components)
                approved = approved or any(start <= match.start() and match.end() <= end
                                             for start, end in approved_frontier_components)
            if symbol in {"bus_access_count", "bus_accesses"}:
                approved = any(start <= match.start() and match.end() <= end
                               for start, end in approved_bridge_provenance_components)
            if not approved:
                raise AssertionError(
                    f"unexpected generic bus or device framework in production tree: "
                    f"{location}: {symbol}")


def assert_bus_device_guard_self_test():
    """Keep the routing allowlist structural, not an adjacent-line escape hatch."""
    source = (
        "M68kGenesisDeviceRoutingResult m68k_route_genesis_device_access(\n"
        "    const M68kMemoryAccessRequest &request) noexcept {\n"
        "}\n")
    assert_bus_device_guard([("src/m68k_pipeline.cpp", source)])
    assert_bus_device_guard([("platforms/genesis/machine/src/frontend.cpp",
                             "GenesisFrontierClass::unsupported_device_access")])
    assert_bus_device_guard([("include/segarecomp/m68k_pipeline.hpp",
                              "enum class GenesisFrontierClass { unsupported_device_access, };")])
    assert_bus_device_guard([("platforms/genesis/machine/src/frontend.cpp",
                              "std::string emit_m68k_general_startup_runtime_c_with_policy(\n"
                              "  const FrontendAnalysis &analysis, M68kGeneralStartupBlockEmissionPolicy policy) {\n"
                              "  stop->provenance.bus_access_count = 1U;\n}\n} // namespace")])
    for forbidden in (
            "void device_access();\n",
            "void m68k_new_device_manager();\n",
            "void foo_bus_router();\n",
            "void another_device_access();\n"):
        try:
            assert_bus_device_guard([("src/m68k_pipeline.cpp", source + forbidden)])
        except AssertionError:
            continue
        raise AssertionError(f"generic framework identifier bypassed routing guard: {forbidden.strip()}")


def assert_scope_exclusions():
    tracked = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, check=True,
                             stdout=subprocess.PIPE).stdout.decode().split("\0")
    prohibited_suffixes = {".bin", ".rom", ".smd", ".md", ".iso", ".cue"}
    for name in filter(None, tracked):
        path = Path(name)
        if not path.parts or path.parts[0] not in {"src", "include", "tests"}:
            continue
        if "games" in path.parts or path.suffix.lower() in prohibited_suffixes:
            raise AssertionError(f"tracked source/test tree contains prohibited fixture path: {name}")
        if path.parts and path.parts[0] == "tests" and "commercial" in path.name.lower():
            raise AssertionError(f"commercial fixture path is tracked: {name}")

    production_files = [
        path for parent in (ROOT / "src", ROOT / "include")
        for path in parent.rglob("*") if path.is_file()]
    production = "\n".join(path.read_text(errors="ignore") for path in production_files)
    forbidden = {
        "host-pointer target encoding": r"reinterpret_cast\s*<[^>]*(?:ImageOffset|M68kAddress|uintptr_t)",
        "runtime target opcode fetch": r"(?:fetch|decode)\s*\([^)]*opcode|opcode[^\n]*(?:fetch|decode)",
    }
    for description, pattern in forbidden.items():
        for path in production_files:
            text = path.read_text(errors="ignore")
            for match in re.finditer(pattern, text, re.IGNORECASE):
                location = path.relative_to(ROOT).as_posix()
                raise AssertionError(
                    f"unexpected {description} in production tree: {location}: {match.group(0)}")

    # Generic Bus/Device abstractions and unscoped bus_/device_ helpers remain
    # excluded. Only exact bounded controller/routing declarations and existing
    # startup observation identifiers are approved.
    assert_bus_device_guard_self_test()
    assert_bus_device_guard([(path.relative_to(ROOT).as_posix(), path.read_text(errors="ignore"))
                             for path in production_files])


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <segarecomp>")
    executable = str(Path(sys.argv[1]).resolve())
    manifest = assert_manifest_provenance()
    assert_scope_exclusions()

    ssp = bytes((0x12, 0x34, 0x56, 0x78))
    cases = [
        ("valid_post_vector_entry", 0x402, bytes((0, 0, 4, 0)),
         "GENESIS_RESET_IMAGE_ACCEPTED", 0x400, "even", "mapped"),
        ("endian_sensitive_entry", 0x400, bytes((0, 0, 3, 0)),
         "GENESIS_RESET_IMAGE_ACCEPTED", 0x300, "even", "mapped"),
        ("pc_high_byte", 0x400, bytes((1, 0, 3, 0)),
         "GENESIS_RESET_PC_NOT_24BIT", None, "not_checked", "not_checked"),
        ("pc_odd", 0x400, bytes((0, 0, 3, 1)),
         "GENESIS_RESET_PC_ODD", 0x301, "odd", "not_checked"),
        ("pc_at_mapping_end", 0x400, bytes((0, 0, 4, 0)),
         "GENESIS_RESET_PC_UNMAPPED", 0x400, "even", "unmapped"),
        ("pc_past_mapping_end", 0x400, bytes((0, 0, 4, 2)),
         "GENESIS_RESET_PC_UNMAPPED", 0x402, "even", "unmapped"),
    ]
    with tempfile.TemporaryDirectory(prefix="segarecomp-reset-adversarial-") as temporary:
        directory = Path(temporary)
        # Public analyze classifies before reset validation.  These assertions cover only the
        # classification rejection; the literal-byte oracle above covers the unreachable reset
        # truncation report contract independently of the executable.
        for length in range(8):
            image = directory / f"vector-truncation-{length}.bin"
            image.write_bytes(bytes(length))
            first = run(executable, image)
            assert_result(f"vector truncation {length}", first, 1, "", "segarecomp: HDR_NOT_RECOGNIZED\n")
            repeated = run(executable, image)
            assert_result(f"vector truncation {length} repeat", repeated, 1, "", "segarecomp: HDR_NOT_RECOGNIZED\n")
        recognized = next(entry for entry in manifest["fixtures"]
                          if entry["id"] == "recognized-genesis-composite")
        recognized_data = materialize_manifest_recipe(recognized["construction"])
        assert_analyze_case(executable, directory, recognized["id"], recognized_data,
                            reset_report_oracle(recognized_data))
        for label, size, pc, diagnostic, initial_pc, alignment, mapping in cases:
            data = genesis_image(size, ssp, pc)
            report = expected_report(size, ssp, pc, diagnostic, initial_pc=initial_pc,
                                     alignment=alignment, mapping=mapping)
            assert_analyze_case(executable, directory, label, data, report)
    print("validated literal-byte reset-report oracle, scoped classification checks, and scope exclusions")


if __name__ == "__main__":
    main()
