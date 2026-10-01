#!/usr/bin/env python3
"""SEG-009-T012: every typed fail-closed outcome reachable through the Master System has a hermetic test.

usage: sms_fail_closed_inventory_test.py <product-root>

Two inventories are parsed from the production headers (so a new value cannot be added without an entry here):
  * every `SmsError` (platforms/master-system/runtime/sms_error.h) names a registered CTest test and a needle (the
    error's own name) that really appears in that test's source;
  * every Z80 outcome of libs/codegen/c11/.../z80_runtime.h that is an error is classified for the SMS mapping as
    `tested` (a generated-native or runtime test drives it) or `unreachable` with the reason and a witness test whose
    source contains the witness needle (the property that makes it unreachable).
Also checked: the contract's error table lists exactly the SmsError classes, and `sms_error_name` names each value.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1]).resolve()
TESTS = ROOT / "tests"
CMAKE = (TESTS / "CMakeLists.txt").read_text(encoding="utf-8")
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)


SOURCES = {
    "sms_machine_tests": "sms_machine_test.cpp",
    "sms_bank_crossing_test": "sms_bank_crossing_test.py",
    "sms_runtime_tests": "sms_runtime_test.cpp",
    "sms_machine_scheduler_test": "sms_machine_scheduler_test.py",
    "sms_vdp_tests": "sms_vdp_test.cpp",
    "sms_psg_native_test": "sms_psg_native_test.py",
}

SMS_ERRORS = {  # SmsError -> (ctest, hermetic evidence)
    "SMS_ERROR_PROFILE_UNSUPPORTED": ("sms_machine_tests", "ingestion and generation-time stop"),
    "SMS_ERROR_MAPPER_UNDECLARED": ("sms_bank_crossing_test", "generation route refuses, nothing emitted"),
    "SMS_ERROR_MAPPER_UNSUPPORTED": ("sms_bank_crossing_test", "generation route refuses, nothing emitted"),
    "SMS_ERROR_ROM_SIZE_UNSUPPORTED": ("sms_bank_crossing_test", "generation route refuses, nothing emitted"),
    "SMS_ERROR_CONTROL_BIT_UNSUPPORTED": ("sms_runtime_tests", "latched by the runtime, sticky"),
    "SMS_ERROR_UNMAPPED_READ": ("sms_machine_tests", "rom_only data read"),
    "SMS_ERROR_BIOS_UNSUPPORTED": ("sms_machine_scheduler_test", "headless driver --bios"),
    "SMS_ERROR_PORT_UNIMPLEMENTED": ("sms_runtime_tests", "decoded port class without a device"),
    "SMS_ERROR_VDP_MODE_UNSUPPORTED": ("sms_vdp_tests", "excluded modes at the check point"),
    "SMS_ERROR_HCOUNTER_UNRESOLVED": ("sms_vdp_tests", "H counter read before any latch"),
    "SMS_ERROR_PSG_DATA_BEFORE_LATCH": ("sms_psg_native_test", "data byte before any latch byte"),
}
# Z80 outcome -> ("tested"|"unreachable", ctest, needle, note)
Z80_ERRORS = {
    "Z80_ERROR_NO_OWNER": ("unreachable", "sms_bank_crossing_test", "no_owner unreachable through the SMS mapping",
                           "every mappable ROM offset of every declared image is an owner start (full owner or typed stub)"),
    "Z80_ERROR_MUTABLE_CODE": ("tested", "sms_bank_crossing_test", "mutable_code", "jump into RAM, cartridge RAM, rom_only $8000"),
    "Z80_ERROR_UNRESOLVED_FETCH_MAPPING": ("tested", "sms_bank_crossing_test", "unresolved_fetch_mapping", "slot-straddling instruction"),
    "Z80_ERROR_UNKNOWN_IMAGE_IDENTITY": ("unreachable", "sms_machine_tests", "is not declared",
                                         "code_image only answers identities declared in the ImageSet (enumerated contract check)"),
    "Z80_ERROR_EXCLUDED_FORM": ("unreachable", "z80_legal_forms_test", "T001 scope decision: no exclusions",
                                "reserved by ADR 0056: the Z80 scope excludes no form (tests/z80_legal_forms_test.py)"),
    "Z80_ERROR_IM0_UNSUPPORTED_ACKNOWLEDGE_BYTE": ("unreachable", "sms_runtime_tests", "IM0_UNSUPPORTED_ACKNOWLEDGE_BYTE",
                                                   "the SMS acknowledge byte is always $FF (RST 38h); IM0/IM1/IM2 accepted in the runtime test"),
}


def source(test):
    name = SOURCES.get(test, test + ".py")
    return (TESTS / name).read_text(encoding="utf-8", errors="ignore")


def ctest_registered(test):
    return re.search(r"add_test\(NAME %s\b" % re.escape(test), CMAKE) is not None


def main():
    header = (ROOT / "platforms/master-system/runtime/sms_error.h").read_text(encoding="utf-8")
    enum = header.split("typedef enum SmsError", 1)[1].split("} SmsError", 1)[0]
    names = re.findall(r"\b(SMS_ERROR_[A-Z_]+)\s*=", enum)
    check(len(names) == 11, "SmsError surface changed (%d values): update the inventory" % len(names))
    check(set(names) == set(SMS_ERRORS), "SmsError inventory mismatch: %s" % sorted(set(names) ^ set(SMS_ERRORS)))
    for name in names:
        check('case %s: return "%s"' % (name, name) in header, "%s is not named by sms_error_name" % name)
        test, _why = SMS_ERRORS.get(name, (None, None))
        if test is None:
            continue
        check(ctest_registered(test), "%s: %s is not a registered CTest test" % (name, test))
        check(name in source(test), "%s: its hermetic test %s never mentions it" % (name, test))
    contract = (ROOT / "docs/architecture/master-system-machine-contract.md").read_text(encoding="utf-8")
    table = contract.split("## 13.", 1)[1].split("## 14.", 1)[0]
    check(set(re.findall(r"`(SMS_ERROR_[A-Z_]+)`", table)) == set(names), "contract error table does not list exactly the SmsError classes")

    runtime = (ROOT / "libs/codegen/c11/include/segarecomp/codegen/c11/runtime/z80_runtime.h").read_text(encoding="utf-8")
    z80_enum = runtime.split("typedef enum Z80Outcome", 1)[1].split("} Z80Outcome", 1)[0]
    z80_errors = re.findall(r"\b(Z80_ERROR_[A-Z0-9_]+)\s*=", z80_enum)
    check(set(z80_errors) == set(Z80_ERRORS), "Z80 error inventory mismatch: %s" % sorted(set(z80_errors) ^ set(Z80_ERRORS)))
    for name, (kind, test, needle, _note) in Z80_ERRORS.items():
        check(ctest_registered(test), "%s: %s is not registered" % (name, test))
        check(needle in source(test), "%s: witness needle %r missing from %s" % (name, needle, test))
    if FAILED:
        print("\n".join(FAILED[:20]))
        return 1
    print("sms fail-closed inventory: %d SmsError values and %d Z80 errors accounted" % (len(names), len(z80_errors)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
