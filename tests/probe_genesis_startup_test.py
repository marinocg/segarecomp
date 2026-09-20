#!/usr/bin/env python3
"""SEG-007-T007: unconditional CLI tests for the two generic startup probes.

``probe-genesis-startup-decode`` and ``probe-genesis-startup-mapping`` are thin, profile-neutral
CLI wrappers over the already-existing shared ``decode_m68k_instruction`` (genesis_startup
profile) and the shared ``m68k_startup_ram_operand_in_range``/``raw_cartridge_rom`` mapping
facts. They take no file input, so this test never depends on the commercial ROM or a local
Musashi checkout and always runs under plain CI.
"""
import json
import subprocess
import sys


def run(executable: str, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run([executable, *args], text=True, capture_output=True, check=False)


def main() -> None:
    executable = sys.argv[1]

    # A known-supported word (MOVEQ #0,D0) decodes true and reports "supported": the real
    # decode -> lift -> IR-effect -> lowering pipeline, not decode success alone, decides this.
    result = run(executable, "probe-genesis-startup-decode", "7000", "-")
    assert result.returncode == 0, result
    payload = json.loads(result.stdout)
    assert payload == {"decoded": True, "kind": "moveq", "length": 2, "support": "supported"}, payload

    # A known-unsupported word decodes false with the shared rejection outcome and "unsupported".
    result = run(executable, "probe-genesis-startup-decode", "4E71", "-")
    assert result.returncode == 0, result
    payload = json.loads(result.stdout)
    assert payload["decoded"] is False, payload
    assert payload["outcome"] == "valid_but_unsupported_instruction", payload
    assert isinstance(payload["unsupported_instruction_form"], bool), payload
    assert payload["support"] == "unsupported", payload

    # A six-byte selected form (JSR (xxx).L) decodes true with length 6. The probe now supplies
    # a real (synthetic, project-owned) M68kMemoryEmissionContext instead of nullptr, so
    # emit_m68k_operation_c's call_absolute_long case (which emits its entire body only inside
    # `if (memory != nullptr)`, with no unconditional fallback) actually produces non-empty
    # text, and m68k_operation_effect already reports a complete PC-transition kind
    # (direct_target, never M68kPcEffectKind::none) for this form. Both signals agree, so this
    # now classifies "supported" -- correcting an earlier "partially_supported" finding that was
    # an artifact of probing with a null memory context (a false negative caused entirely by the
    # probe never exercising JSR/RTS's memory-gated lowering), not a genuine implementation gap.
    # See docs/testing/sonic-startup-inventory.md and the T007 Evidence section for the full
    # explanation.
    result = run(executable, "probe-genesis-startup-decode", "4EB9", "00FF0000")
    assert result.returncode == 0, result
    payload = json.loads(result.stdout)
    assert payload == {
        "decoded": True, "kind": "jsr_absolute_long", "length": 6, "support": "supported",
    }, payload

    # RTS: return_from_subroutine has the same shape as call_absolute_long above -- a complete
    # PC-transition kind (observed_stack_return) and an emit_m68k_operation_c case that is
    # likewise gated entirely on a non-null memory context. With the probe's real synthetic
    # context it also now classifies "supported", for the same reason as JSR above.
    result = run(executable, "probe-genesis-startup-decode", "4E75", "-")
    assert result.returncode == 0, result
    payload = json.loads(result.stdout)
    assert payload == {
        "decoded": True, "kind": "rts", "length": 2, "support": "supported",
    }, payload

    # The two absolute-long MOVE forms were already reported "supported" under the old null
    # context (their status-register-update line is emitted unconditionally, independent of the
    # memory context), and remain "supported" under the real context: the RAM store/load and
    # "pc +=" advance, previously untested, now also emit non-empty text and corroborate the
    # same result rather than changing it.
    result = run(executable, "probe-genesis-startup-decode", "23C0", "00FF0000")
    assert result.returncode == 0, result
    payload = json.loads(result.stdout)
    assert payload == {
        "decoded": True, "kind": "move_l_d0_absolute_long", "length": 6, "support": "supported",
    }, payload
    result = run(executable, "probe-genesis-startup-decode", "2239", "00FF0000")
    assert result.returncode == 0, result
    payload = json.loads(result.stdout)
    assert payload == {
        "decoded": True, "kind": "move_l_absolute_long_d1", "length": 6, "support": "supported",
    }, payload

    # TST.L (xxx).L (SEG-007-T008): added as a genesis_startup CPU decode capability of the same
    # shared decode_m68k_instruction predicate every other kind above already goes through -- not a
    # second/duplicated decoder or a per-instruction probe path. The probe's memory-emission context
    # now also supplies a harmless project-owned raw_cartridge_rom test_operand_region/
    # test_operand_rom_value (instead of leaving it unset), so
    # emit_m68k_operation_c's test_absolute_long case actually produces non-empty text and
    # m68k_operation_effect already reports a complete PC-transition kind for this form, exactly
    # like every other selected kind above. This regression is the entire point of SEG-007-T008:
    # before it, this exact probe reported "unsupported"; after it, "supported".
    result = run(executable, "probe-genesis-startup-decode", "4AB9", "00FF0000")
    assert result.returncode == 0, result
    payload = json.loads(result.stdout)
    assert payload == {
        "decoded": True, "kind": "tst_l_absolute_long", "length": 6, "support": "supported",
    }, payload

    # Bad argument shapes exit 2 with usage on stderr.
    result = run(executable, "probe-genesis-startup-decode", "700", "-")
    assert result.returncode == 2, result
    result = run(executable, "probe-genesis-startup-decode", "7000")
    assert result.returncode == 2, result

    # An address inside [0, length) is raw_cartridge_rom (1/2/4-byte accesses).
    result = run(executable, "probe-genesis-startup-mapping", "00000100", "2", "0000000000010000")
    assert result.returncode == 0, result
    assert json.loads(result.stdout) == {"category": "raw_cartridge_rom"}, result.stdout

    # An address inside [0x00FF0000, 0x01000000) is synthetic_work_ram.
    result = run(executable, "probe-genesis-startup-mapping", "00FF0010", "4", "0000000000010000")
    assert result.returncode == 0, result
    assert json.loads(result.stdout) == {"category": "synthetic_work_ram"}, result.stdout

    # An address outside both is hardware_frontier.
    result = run(executable, "probe-genesis-startup-mapping", "00C00000", "2", "0000000000010000")
    assert result.returncode == 0, result
    assert json.loads(result.stdout) == {"category": "hardware_frontier"}, result.stdout

    # Whole-range boundary cases: raw_cartridge_rom's [0, image_length) end boundary.
    image_length_hex = "0000000000010000"  # 0x10000
    image_length = 0x10000
    for width in (1, 2, 4):
        address = f"{image_length - width:08X}"
        result = run(executable, "probe-genesis-startup-mapping", address, str(width), image_length_hex)
        assert result.returncode == 0, result
        assert json.loads(result.stdout) == {"category": "raw_cartridge_rom"}, (width, result.stdout)

    # A 2-byte access starting one byte before the ROM mapping's end crosses out of it ->
    # hardware_frontier, never silently accepted as raw_cartridge_rom.
    crossing_address = f"{image_length - 1:08X}"
    result = run(executable, "probe-genesis-startup-mapping", crossing_address, "2", image_length_hex)
    assert result.returncode == 0, result
    assert json.loads(result.stdout) == {"category": "hardware_frontier"}, result.stdout

    # Whole-range boundary cases: synthetic_work_ram's [0x00FF0000, 0x01000000) end boundary.
    ram_end = 0x01000000
    for width in (1, 2, 4):
        address = f"{ram_end - width:08X}"
        result = run(executable, "probe-genesis-startup-mapping", address, str(width), "0000000000000000")
        assert result.returncode == 0, result
        assert json.loads(result.stdout) == {"category": "synthetic_work_ram"}, (width, result.stdout)

    # A 2-byte access starting one byte before the RAM window's end crosses out of it ->
    # hardware_frontier.
    ram_crossing_address = f"{ram_end - 1:08X}"
    result = run(executable, "probe-genesis-startup-mapping", ram_crossing_address, "2", "0000000000000000")
    assert result.returncode == 0, result
    assert json.loads(result.stdout) == {"category": "hardware_frontier"}, result.stdout

    # A huge decimal width (0xFFFFFFFF, larger than the entire synthetic work-RAM window) must
    # not overflow m68k_startup_ram_range_in_range's internal `end - width` subtraction and
    # false-accept; it must classify hardware_frontier for an address that would otherwise be a
    # valid small-width synthetic_work_ram address.
    result = run(executable, "probe-genesis-startup-mapping", "00FF0010", "4294967295", "0000000000010000")
    assert result.returncode == 0, result
    assert json.loads(result.stdout) == {"category": "hardware_frontier"}, result.stdout

    # Bad argument shapes exit 2 with usage on stderr.
    result = run(executable, "probe-genesis-startup-mapping", "00C00000")
    assert result.returncode == 2, result
    result = run(executable, "probe-genesis-startup-mapping", "00C00000", "0", "0000000000010000")
    assert result.returncode == 2, result
    result = run(executable, "probe-genesis-startup-mapping", "00C00000", "abc", "0000000000010000")
    assert result.returncode == 2, result

    print("probe genesis startup test: ok")


if __name__ == "__main__":
    main()
