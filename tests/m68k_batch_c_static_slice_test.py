#!/usr/bin/env python3
"""Synthetic Batch-C (SEG-007-T025) generated-C checks: C1 (SWAP/EXT.W/EXT.L)
through C7b (ROL/ROR/ROXL/ROXR memory RMW forms), covering every selected
Batch-C family's generated-C execution, determinism, and adversarial
decode rejection."""
import json, subprocess, sys, tempfile
from pathlib import Path


def harness_args(d0="00000000", sr="0000", extra_d=None):
    d = ["00000000"] * 8
    d[0] = d0
    if extra_d:
        for index, value in extra_d.items():
            d[index] = value
    return ["00000100", sr, *d, *(["00000000"] * 7), "00FF0100", "0"]


def main():
    harness, cc = sys.argv[1:]
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)

        # Deterministic generated C: every SWAP/EXT.W/EXT.L register form
        # (all eight Dn) is byte-identical across two independent emissions.
        audit_codes = []
        for base in (0x4840, 0x4880, 0x48C0):
            for reg in range(8):
                audit_codes.append(f"{base | reg:04x}")
        base_args = ["00000100", "0010", *(["0"] * 16), "0"]
        for code in audit_codes:
            image = td / f"deterministic-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            first = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            second = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )

        # (code_hex, seed_d0, expected_d0, expected_pc, expected_sr) with a
        # fully poisoned seed SR (0xFFFF) so preserved-upper-byte/X behavior
        # and the exact N/Z/V/C pattern are all independently visible.
        vectors = [
            ("4840", "00000000", "00000000", "00000102", "FFF4"),  # SWAP zero
            ("4840", "1234ABCD", "ABCD1234", "00000102", "FFF8"),  # SWAP negative result
            ("4840", "00010002", "00020001", "00000102", "FFF0"),  # SWAP positive nonzero
            ("4840", "ABCD1234", "1234ABCD", "00000102", "FFF0"),  # SWAP half exchange round-trip
            ("4880", "12340142", "12340042", "00000102", "FFF0"),  # EXT.W positive, upper word preserved
            ("4880", "123401FF", "1234FFFF", "00000102", "FFF8"),  # EXT.W negative, upper word preserved
            ("4880", "12340100", "12340000", "00000102", "FFF4"),  # EXT.W zero word; Z from word only
            ("48C0", "FFFF7FFF", "00007FFF", "00000102", "FFF0"),  # EXT.L positive overwrites full register
            ("48C0", "00008000", "FFFF8000", "00000102", "FFF8"),  # EXT.L negative
            ("48C0", "12340000", "00000000", "00000102", "FFF4"),  # EXT.L zero
        ]
        for index, (code, d0, expected_d0, expected_pc, expected_sr) in enumerate(vectors):
            image = td / f"{index}.bin"
            image.write_bytes(bytes.fromhex(code))
            args = [harness, str(image), *harness_args(d0=d0, sr="FFFF")]
            generated = subprocess.run(args, text=True, capture_output=True)
            assert generated.returncode == 0, (code, generated.stderr)
            c = td / f"{index}.c"
            exe = td / f"{index}"
            c.write_text(generated.stdout)
            compiled = subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
                text=True, capture_output=True,
            )
            assert compiled.returncode == 0, compiled.stderr
            result = subprocess.run([str(exe)], text=True, capture_output=True)
            assert result.returncode == 0
            got = json.loads(result.stdout)
            assert got["d"][0] == expected_d0 and got["pc"] == expected_pc and got["sr"] == expected_sr, (
                code, got,
            )

        # EXT.W width preservation: the upper word must survive byte-identical
        # even when it is itself nonzero and would look like a plausible (but
        # wrong) sign-extension result if EXT.W ever touched it.
        image = td / "ext-w-preserve.bin"
        image.write_bytes(bytes.fromhex("4880"))
        args = [harness, str(image), *harness_args(d0="ABCD0001", sr="0000")]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "ext-w-preserve.c"
        exe = td / "ext-w-preserve"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["d"][0] == "ABCD0001", got  # low byte 0x01 sign-extends to 0x0001; upper word 0xABCD survives

        # Register-field decode: every Dn is independently addressable, not
        # only D0 (SWAP D5 is exercised, distinct from every vector above).
        image = td / "swap-d5.bin"
        image.write_bytes(bytes.fromhex("4845"))
        args = [harness, str(image), *harness_args(sr="0000", extra_d={5: "0000FFFF"})]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "swap-d5.c"
        exe = td / "swap-d5"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["d"][5] == "FFFF0000" and got["d"][0] == "00000000", got

        # Adversarial rejection: neighboring encodings in the same primary
        # byte belong to other, not-yet-selected (or out-of-scope) families
        # and must not be silently accepted by the SWAP/EXT masks.
        rejected_codes = (
            "4800",  # NBCD Dn -- different family, same primary byte
            "4808",  # NBCD (An) -- memory form of the same neighbor
            "4848",  # mode3=001 (An direct) at PEA's slot: illegal PEA, not SWAP
            "4888",  # MOVEM.W re,(An) shape (mode3=001) neighboring EXT.W's Dn slot
            "48C8",  # MOVEM.L re,(An) shape (mode3=001) neighboring EXT.L's Dn slot
            "49C0",  # EXTB.L (68020+) -- distinct top byte (0x49), out of scope
            "4AC0",  # TAS D0 -- unrelated family entirely
        )
        for code in rejected_codes:
            image = td / f"{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            rejected = subprocess.run(
                [harness, str(image), *base_args], text=True, capture_output=True,
            )
            assert rejected.returncode == 1 and rejected.stdout == "", (code, rejected)

        # ---------------------------------------------------------------
        # C2: PEA / LINK / UNLK
        # ---------------------------------------------------------------

        # PEA legal control-EA forms decode and generate deterministic C.
        # (An), d16(An), absolute.w, absolute.l, d16(PC) -- the exact T023
        # control-EA tranche, matching LEA/JMP/JSR's own ceiling.
        pea_legal_codes = (
            "4851",      # PEA (A1)
            "48690004",  # PEA 4(A1)
            "4878FF10",  # PEA absolute.w
            "487900FF0010",  # PEA absolute.l
            "487A0004",  # PEA 4(PC)
        )
        for code in pea_legal_codes:
            image = td / f"pea-legal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            first = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            second = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )

        # PEA rejects every illegal EA mode per the exact ISA legality table:
        # Dn is impossible to even reach PEA's decoder (that exact slot is
        # SWAP, tested above); An-direct, (An)+, -(An), and immediate all
        # remain outside PEA's legal control-EA set.
        pea_illegal_codes = (
            "4849",  # PEA A1 (An direct) -- illegal
            "4859",  # PEA (A1)+ -- illegal
            "4861",  # PEA -(A1) -- illegal
            "487C",  # PEA #imm -- illegal
        )
        for code in pea_illegal_codes:
            image = td / f"pea-illegal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            rejected = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert rejected.returncode == 1 and rejected.stdout == "", (code, rejected)

        # PEA extension-word truncation: d16(An)/absolute.w/absolute.l/
        # d16(PC) each require their extension bytes to be present; a primary
        # word with no (or a partial) extension must reject, reusing the
        # existing T023 truncation behavior rather than a parallel parser.
        pea_truncated_codes = (
            "4869",        # PEA d16(A1): missing the 2-byte displacement
            "4878",        # PEA absolute.w: missing the 2-byte address
            "4879000000",  # PEA absolute.l: only 3 of 4 address bytes present
            "487A",        # PEA d16(PC): missing the 2-byte displacement
        )
        for code in pea_truncated_codes:
            image = td / f"pea-truncated-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            rejected = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert rejected.returncode == 1 and rejected.stdout == "", (code, rejected)

        # LINK.W decodes (every register, deterministic C) and consumes
        # exactly one 16-bit extension word; a primary word with no
        # extension word present must reject as truncated.
        link_codes = tuple(f"4e5{reg:x}0010" for reg in range(8))
        for code in link_codes:
            image = td / f"link-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            first = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            second = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )
        image = td / "link-truncated.bin"
        image.write_bytes(bytes.fromhex("4e50"))  # LINK A0,#disp16 with no extension word present
        rejected = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
        assert rejected.returncode == 1 and rejected.stdout == "", rejected

        # UNLK decodes (every register, deterministic C); no extension word.
        unlk_codes = tuple(f"4e5{8 + reg:x}" for reg in range(8))
        for code in unlk_codes:
            image = td / f"unlk-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            first = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            second = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )

        # LINK's provenance length is exactly 4 bytes (primary + one 16-bit
        # displacement extension word): PC must advance by 4, not 2.
        image = td / "link-pc-advance.bin"
        image.write_bytes(bytes.fromhex("4e500000"))
        args = [harness, str(image), *harness_args(sr="0000")]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "link-pc-advance.c"
        exe = td / "link-pc-advance"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["pc"] == "00000104", got

        # UNLK's provenance length is exactly 2 bytes (primary word only).
        # UNLK A7 is used here (rather than A0) so the default harness_args
        # A7 seed (00FF0100, already inside the synthetic RAM window) is the
        # frame pointer read from, instead of an unseeded zero address.
        image = td / "unlk-pc-advance.bin"
        image.write_bytes(bytes.fromhex("4e5f"))
        args = [harness, str(image), *harness_args(sr="0000")]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "unlk-pc-advance.c"
        exe = td / "unlk-pc-advance"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["pc"] == "00000102", got

        # SEG-007-T114: NOP (0x4E71) is a selected no-operand System Control
        # Group form. Its lowering is a pure two-byte PC advance -- no
        # register, SR, or memory write of any kind. Seed nonzero D/A/SR and
        # confirm only pc changes (0x00000100 -> 0x00000102).
        image = td / "nop-pc-advance.bin"
        image.write_bytes(bytes.fromhex("4e71"))
        seeded = harness_args(sr="271F")
        seeded[2:18] = ["1234ABCD", "0F0F0F0F", "DEADBEEF", "00000001",
                        "80000000", "0000FFFF", "55555555", "AAAAAAAA",
                        "00000010", "00000020", "00000030", "00000040",
                        "00000050", "00000060", "00000070", "00FF0100"]
        generated = subprocess.run([harness, str(image), *seeded], text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        assert "/* NOP */" in generated.stdout
        assert "sr =" not in generated.stdout.split("/* NOP */", 1)[1]
        c = td / "nop-pc-advance.c"
        exe = td / "nop-pc-advance"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["pc"] == "00000102" and got["sr"] == "271F", got
        assert got["d"] == ["1234ABCD", "0F0F0F0F", "DEADBEEF", "00000001",
                            "80000000", "0000FFFF", "55555555", "AAAAAAAA"], got
        assert got["a"][:7] == ["00000010", "00000020", "00000030", "00000040",
                                "00000050", "00000060", "00000070"], got

        # SEG-007-T116: MOVE from SR (0x40C0-0x40FF), a word-operation System
        # Control Group form. MOVE SR,D3 (0x40C3) copies the 16-bit SR into the
        # low word of D3, preserves D3's upper word, and affects NO condition
        # codes (SR itself is unchanged). Seed a nonzero D3 upper word and a
        # nonzero SR and confirm exactly that.
        image = td / "move-from-sr.bin"
        image.write_bytes(bytes.fromhex("40c3"))
        seeded = harness_args(sr="271F", extra_d={3: "DEADBEEF"})
        first = subprocess.run([harness, str(image), *seeded], text=True, capture_output=True)
        second = subprocess.run([harness, str(image), *seeded], text=True, capture_output=True)
        assert first.returncode == 0, first.stderr
        assert first.stdout == second.stdout, (first.stdout, second.stdout)
        # The lowering must never emit an SR/CCR assignment (contrast MOVE to
        # SR). The harness declares `uint16_t sr=UINT16_C(...)` with no spaces,
        # so a spaced `sr = ` would be an operation-emitted write.
        assert "sr = " not in first.stdout
        c = td / "move-from-sr.c"
        exe = td / "move-from-sr"
        c.write_text(first.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["pc"] == "00000102" and got["sr"] == "271F", got
        assert got["d"][3] == "DEAD271F", got
        assert got["d"][:3] == ["00000000", "00000000", "00000000"] and got["d"][4:] == \
            ["00000000", "00000000", "00000000", "00000000"], got

        # Adversarial: every non-Dn MOVE from SR destination mode and the
        # reverse MOVE <ea>,SR encoding must not be accepted by this harness
        # (the decoder fails them closed as valid_but_unsupported_instruction).
        for code in ("40d0", "40d8", "40e0", "40e8", "40f9ff00", "40c8", "46c0"):
            (td / "adv.bin").write_bytes(bytes.fromhex(code))
            rejected = subprocess.run([harness, str(td / "adv.bin"), *harness_args()], text=True, capture_output=True)
            assert rejected.returncode == 1, (code, rejected.stdout, rejected.stderr)

        # SEG-007-T118: MOVE <ea>,CCR (0x44C0-0x44FF), a word-operation System
        # Control Group form. MOVE D2,CCR (0x44C2) reads D2 as a word and copies
        # only bits 4..0 of its low-order byte into the CCR sub-field (X/N/Z/V/C)
        # of SR; CCR bits 7..5 are unimplemented and always read 0, and the upper
        # (system) byte of SR is preserved and D2 is unchanged. Seed a D2 whose
        # low byte HAS bits 7..5 set (0xE0) so the 0x001F mask -- not 0x00FF -- is
        # exercised and regression-locked against the pinned-Musashi oracle
        # (m68ki_set_ccr consults only BIT_4..BIT_0).
        image = td / "move-to-ccr.bin"
        image.write_bytes(bytes.fromhex("44c2"))
        seeded = harness_args(sr="A700", extra_d={2: "DEADBEE0"})
        first = subprocess.run([harness, str(image), *seeded], text=True, capture_output=True)
        second = subprocess.run([harness, str(image), *seeded], text=True, capture_output=True)
        assert first.returncode == 0, first.stderr
        assert first.stdout == second.stdout, (first.stdout, second.stdout)
        # The lowering writes only the CCR sub-field: a spaced `sr = ` is emitted
        # (contrast MOVE from SR) but it must be the 0x001F-masked form.
        assert "sr = (uint16_t)((sr & UINT16_C(0xFF00)) | ((d[2]) & UINT16_C(0x001F)));" in first.stdout
        c = td / "move-to-ccr.c"
        exe = td / "move-to-ccr"
        c.write_text(first.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        # 0xA700 upper byte preserved; low byte replaced by (0xE0 & 0x1F) == 0x00
        # -> 0xA700 (a buggy 0x00FF mask would instead yield 0xA7E0).
        assert got["pc"] == "00000102" and got["sr"] == "A700", got
        assert got["d"][2] == "DEADBEE0", got
        assert got["d"][:2] == ["00000000", "00000000"] and got["d"][3:] == \
            ["00000000", "00000000", "00000000", "00000000", "00000000"], got

        # Adversarial: every non-Dn MOVE to CCR source mode (memory, An-direct,
        # immediate), the reverse MOVE <ea>,SR encoding, and MOVE from SR must
        # not be accepted by this harness (the decoder fails the excluded
        # sources closed as valid_but_unsupported_instruction; MOVE from SR is a
        # different accepted kind but its bytes 0x40Cx must not be misrouted to
        # move_to_ccr).
        for code in ("44d0", "44d8", "44e0", "44e8", "44f9ff00", "44c8", "44fcff00", "46c0"):
            (td / "adv.bin").write_bytes(bytes.fromhex(code))
            rejected = subprocess.run([harness, str(td / "adv.bin"), *harness_args()], text=True, capture_output=True)
            assert rejected.returncode == 1, (code, rejected.stdout, rejected.stderr)

        # Adversarial: LINK/UNLK's own opcode-space neighbors -- the 68020+
        # long-displacement LINK.L form (a different primary word entirely,
        # 0x4808|An), the surrounding TRAP/RESET/STOP/RTE family
        # (0x4E40-0x4E7F, excluding LINK/UNLK's own 0x4E50-0x4E5F slice and
        # SEG-007-T114's now-selected NOP 0x4E71), and a MOVEM.W
        # register-to-memory primary word with its mandatory register-mask
        # extension word missing -- must not be misclassified as LINK, UNLK,
        # or any C1/C2 kind.
        neighbor_rejected_codes = (
            "48080000",  # LINK.L A0,#disp32 (68020+) -- distinct primary word, unsupported
            "4E40",      # TRAP #0
            "4E70",      # RESET
            "4E72",      # STOP (also needs an extension word; rejected regardless)
            "4E73",      # RTE
            "4E76",      # TRAPV
            "4890",      # MOVEM.W list,(A0) -- truncated: mask extension word missing (C5a)
        )
        for code in neighbor_rejected_codes:
            image = td / f"neighbor-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            rejected = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert rejected.returncode == 1 and rejected.stdout == "", (code, rejected)

        # ---------------------------------------------------------------
        # C3: BTST / BCHG / BCLR / BSET
        # ---------------------------------------------------------------

        # Every dynamic (Dn bit-number) and static (immediate bit-number)
        # form, both Dn and memory destinations, for all four mnemonics:
        # deterministic generated C.
        bit_op_legal_codes = (
            "0300", "0312", "08000005", "08130000",   # BTST: dyn Dn, dyn mem, imm Dn, imm mem
            "0340", "035A", "08400005", "08530000",   # BCHG
            "0380", "03A2", "08800005", "08930000",   # BCLR
            "03C0", "03DA", "08C00005", "08D30000",   # BSET
        )
        for code in bit_op_legal_codes:
            image = td / f"bitop-legal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            # Memory-destination forms need A2/A3 seeded; register-only forms
            # tolerate the default zeroed A2/A3 harmlessly since they never
            # dereference them.
            a_seeded = ["00000000"] * 8
            a_seeded[2] = "00FF0050"
            a_seeded[3] = "00FF0060"
            a_seeded[7] = "00FF0100"
            full_args = [harness, str(image), "00000100", "0000", *(["00000000"] * 8), *a_seeded, "0",
                         "00FF0050", "00000000", "00FF0060", "00000000"]
            first = subprocess.run(full_args, text=True, capture_output=True)
            second = subprocess.run(full_args, text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )

        # Hand-verified register-result/CCR vectors (poisoned SR 0xFFFF so
        # the exact preserved-everything-except-Z rule is unambiguous: only
        # bit 2 may ever change, to 0xFFFB when the tested bit was
        # originally set, or unchanged at 0xFFFF when it was originally
        # clear).
        # (code_hex, seed_d0, seed_d1, expected_d0, expected_sr)
        register_vectors = (
            # BTST D1,D0: D0=0xFF, D1=3 -> bit3 of 0xFF is set -> Z=0; D0 unchanged (read-only).
            ("0300", "000000FF", "00000003", "000000FF", "FFFB"),
            # BCHG D1,D0: D0=0, D1=3 -> bit3 clear -> Z=1; toggled to 0x08.
            ("0340", "00000000", "00000003", "00000008", "FFFF"),
            # BCLR D1,D0: D0=0xFF, D1=3 -> bit3 set -> Z=0; cleared to 0xF7.
            ("0380", "000000FF", "00000003", "000000F7", "FFFB"),
            # BSET D1,D0: D0=0, D1=3 -> bit3 clear -> Z=1; set to 0x08.
            ("03C0", "00000000", "00000003", "00000008", "FFFF"),
        )
        for index, (code, d0, d1, expected_d0, expected_sr) in enumerate(register_vectors):
            image = td / f"bitop-reg-{index}.bin"
            image.write_bytes(bytes.fromhex(code))
            args = [harness, str(image), "00000100", "FFFF", d0, d1, *(["00000000"] * 6), *(["00000000"] * 7),
                    "00FF0100", "0"]
            generated = subprocess.run(args, text=True, capture_output=True)
            assert generated.returncode == 0, (code, generated.stderr)
            c = td / f"bitop-reg-{index}.c"
            exe = td / f"bitop-reg-{index}"
            c.write_text(generated.stdout)
            assert subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
                text=True, capture_output=True,
            ).returncode == 0
            got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
            assert got["d"][0] == expected_d0 and got["sr"] == expected_sr, (code, got)

        # BTST is read-only: register destination is provably NEVER
        # modified, across every tested bit position and both bit-number
        # classes.
        for code, d0 in (("0300", "80000000"), ("0300", "00000000"), ("08000005", "FFFFFFFF")):
            image = td / f"btst-readonly-{code}-{d0}.bin"
            image.write_bytes(bytes.fromhex(code))
            args = [harness, str(image), "00000100", "0000", d0, "00000000", *(["00000000"] * 6),
                    *(["00000000"] * 7), "00FF0100", "0"]
            generated = subprocess.run(args, text=True, capture_output=True)
            assert generated.returncode == 0, generated.stderr
            c = td / f"btst-ro-{code}-{d0}.c"
            exe = td / f"btst-ro-{code}-{d0}"
            c.write_text(generated.stdout)
            assert subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
                text=True, capture_output=True,
            ).returncode == 0
            got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
            assert got["d"][0] == d0, (code, d0, got)

        # Register destination is a full 32-bit write, never byte/word
        # partial: BSET on bit31 sets exactly bit31, leaving every other bit
        # of a fully poisoned D0 alone.
        image = td / "bset-full-width.bin"
        image.write_bytes(bytes.fromhex("03C0"))  # BSET D1,D0
        args = [harness, str(image), "00000100", "0000", "0000FFFF", "0000001F", *(["00000000"] * 6),
                *(["00000000"] * 7), "00FF0100", "0"]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "bset-full-width.c"
        exe = td / "bset-full-width"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["d"][0] == "8000FFFF", got  # bit31 (D1&31=31) set; every other bit untouched

        # Memory destination is exactly one byte: BSET must not disturb
        # either neighboring byte.
        image = td / "bset-one-byte.bin"
        image.write_bytes(bytes.fromhex("03DA"))  # BSET D1,(A2)+
        args = [harness, str(image), "00000100", "0000", "00000000", "00000000", *(["00000000"] * 6),
                "0", "0", "00FF0050", "0", "0", "0", "0", "00FF0100", "0",
                "00FF0050", "00FFFF00"]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "bset-one-byte.c"
        exe = td / "bset-one-byte"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        # Seeded long 00FFFF00: byte0=0x00 (target), byte1=0xFF, byte2=0xFF,
        # byte3=0x00 (neighbors). BSET D1(=0),(A2)+ sets bit0 of byte0 only.
        assert got["ram"] == ["01FFFF00"], got

        # Dn modulo-32 and memory modulo-8 bit-number behavior: an
        # out-of-width bit number is never rejected, only wrapped.
        image = td / "bset-modulo32.bin"
        image.write_bytes(bytes.fromhex("03C0"))  # BSET D1,D0
        args = [harness, str(image), "00000100", "0000", "00000000", "00000021", *(["00000000"] * 6),
                *(["00000000"] * 7), "00FF0100", "0"]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "bset-modulo32.c"
        exe = td / "bset-modulo32"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["d"][0] == "00000002", got  # bit (33 & 31) = 1 set

        image = td / "btst-modulo8.bin"
        image.write_bytes(bytes.fromhex("0312"))  # BTST D1,(A2)
        args = [harness, str(image), "00000100", "0000", "00000000", "00000009", *(["00000000"] * 6),
                "0", "0", "00FF0050", *(["00000000"] * 5), "0", "00FF0050", "02000000"]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "btst-modulo8.c"
        exe = td / "btst-modulo8"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        # byte 0x02 = 00000010, bit(9&7=1) is set -> Z=0.
        assert got["sr"] == "0000", got

        # Dynamic register alias (BCHG D0,D0): the bit number must come from
        # the ORIGINAL D0, never a value the destination write already
        # changed.
        image = td / "bchg-alias.bin"
        image.write_bytes(bytes.fromhex("0140"))  # BCHG D0,D0
        args = [harness, str(image), "00000100", "0000", "00000005", "00000000", *(["00000000"] * 6),
                *(["00000000"] * 7), "00FF0100", "0"]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "bchg-alias.c"
        exe = td / "bchg-alias"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["d"][0] == "00000025", got  # bit index=5 (from original D0=5), toggled: 5|0x20=0x25

        # A7 byte auto-update: (A7)+/-(A7) step by 2, not 1, for a byte
        # access -- the shared T023 EA machinery's existing A7-specific rule,
        # reused unmodified.
        image = td / "bset-a7-postinc.bin"
        image.write_bytes(bytes.fromhex("03DF"))  # BSET D1,(A7)+
        args = [harness, str(image), "00000100", "0000", *(["00000000"] * 8), *(["00000000"] * 7),
                "00FF0100", "0", "00FF0100", "22334455"]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "bset-a7-postinc.c"
        exe = td / "bset-a7-postinc"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["a"][7] == "00FF0102" and got["ram"] == ["23334455"], got

        image = td / "bclr-a7-predec.bin"
        image.write_bytes(bytes.fromhex("03A7"))  # BCLR D1,-(A7)
        args = [harness, str(image), "00000100", "0000", *(["00000000"] * 8), *(["00000000"] * 7),
                "00FF0104", "0", "00FF0102", "23445566"]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "bclr-a7-predec.c"
        exe = td / "bclr-a7-predec"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["a"][7] == "00FF0102" and got["ram"] == ["22445566"], got

        # BTST memory: exactly one read, zero writes (the (An)+/-(An)
        # address-register auto-update case for a read-only BTST is
        # separately proven against pinned Musashi by the
        # "btst-a7-postinc" Musashi-differential fixture vector, which
        # shows real A7 movement -- 0x00FF0100 to 0x00FF0102 -- with zero
        # writes).
        image = td / "btst-mem-readonly.bin"
        image.write_bytes(bytes.fromhex("0312"))  # BTST D1,(A2)
        args = [harness, str(image), "00000100", "0000", "00000000", "00000000", *(["00000000"] * 6),
                "0", "0", "00FF0050", *(["00000000"] * 5), "0", "00FF0050", "80000000"]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "btst-mem-readonly.c"
        exe = td / "btst-mem-readonly"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["ram"] == ["80000000"] and got["a"][2] == "00FF0050", got  # unchanged: (An), no postinc

        # Adversarial: BCHG/BCLR/BSET reject a PC-relative destination
        # (mutation forms only; BTST may legally read via d16(PC)).
        # d16(PC) mode3=7,reg3=2 -> EA=0x3A. BCHG static base 0x0840|0x3A=0x087A.
        for code in ("087A0000", "08BA0000", "08FA0000"):  # BCHG/BCLR/BSET #0,d16(PC): illegal destination
            image = td / f"bitop-pcrel-illegal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            rejected = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert rejected.returncode == 1 and rejected.stdout == "", (code, rejected)
        # BTST #0,d16(PC) is legal (read-only exception). Per the project's
        # established EA contract, d16(PC) folds against the address of its
        # OWN displacement extension word, not the primary word or the
        # instruction's start. For this instruction at 0x100: primary word
        # at 0x100, the bit-number immediate extension at 0x102, and the
        # destination's own displacement extension at 0x104. With
        # disp=0xFEFE (-258): target = 0x104 + (-258) = 0x104 - 0x102 =
        # 0x0002, landing inside this synthetic image itself (local offset
        # 2 is mapped) so the harness's shared static resolver can actually
        # resolve it as a ROM read; decode legality (not resolvability) is
        # what this test exists to prove.
        image = td / "btst-pcrel-legal.bin"
        image.write_bytes(bytes.fromhex("083A0000FEFE"))  # BTST #0,d16(PC): EA=(7<<3)|2=0x3A
        args = [harness, str(image), *base_args]
        accepted = subprocess.run(args, text=True, capture_output=True)
        assert accepted.returncode == 0, accepted.stderr

        # Load-bearing MOVEP collision (contract: "MOVEP collision is a
        # load-bearing decoder test"): one representative MOVEP encoding per
        # mnemonic's opmode slot, at destination mode3==001 (An-direct),
        # must NOT be accepted as BTST/BCHG/BCLR/BSET. T025 does not
        # implement MOVEP; these fail closed through the existing
        # unsupported-instruction/illegal-EA route.
        movep_shaped_codes = (
            "030A",  # movep.w er shape at BTST's opmode slot
            "034A",  # movep.l er shape at BCHG's opmode slot
            "038A",  # movep.w re shape at BCLR's opmode slot
            "03CA",  # movep.l re shape at BSET's opmode slot
        )
        for code in movep_shaped_codes:
            image = td / f"movep-shaped-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            rejected = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert rejected.returncode == 1 and rejected.stdout == "", (code, rejected)

        # Adversarial: T024's own ordinary immediate logical forms (ANDI/
        # ORI/EORI) remain unaffected -- the bit-op masks require bit8==1,
        # which these forms never set.
        for code in ("02010000", "00010000", "0A010000"):
            image = td / f"t024-neighbor-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            args = [harness, str(image), *base_args]
            result = subprocess.run(args, text=True, capture_output=True)
            # These are legal ANDI/ORI/EORI forms (not C1-C3 kinds), so the
            # harness's own accepted-kind filter rejects them with exit 1 --
            # this merely confirms they are not misclassified as any bit
            # operation kind (a genuine bit-op misclassification would still
            # produce exit 1 here, so this check only guards against a
            # crash/hang, with the real proof being the mask disjointness
            # argument in m68k_decode_general_bit_operation's doc comment).
            assert result.returncode == 1, (code, result)

        # ---------------------------------------------------------------
        # C4a: general BRA / Bcc
        # ---------------------------------------------------------------

        # Every selected branch form (BRA byte/word, all 14 Bcc conditions
        # byte form) decodes and generates deterministic C.
        branch_legal_codes = (
            "6010", "60F0", "60FF", "60000100", "6000FF00", "60000000",  # BRA byte/word forms
            "6210", "6310", "6410", "6510", "6610", "6710", "6810", "6910",
            "6A10", "6B10", "6C10", "6D10", "6E10", "6F10",  # every Bcc condition, byte form
        )
        for code in branch_legal_codes:
            image = td / f"branch-legal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            first = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            second = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )

        # BRA/Bcc touch ONLY pc: complete SR, every D/A register, and RAM
        # must remain byte-identical (contract: "C4a generated-C tests").
        # (code_hex, seed_sr, expected_pc)
        no_side_effect_vectors = (
            ("6010", "FFFF", "00000112"),          # BRA byte forward
            ("60F0", "FFFF", "000000F2"),          # BRA byte negative
            ("60FF", "FFFF", "00000101"),          # BRA byte -1
            ("60000100", "FFFF", "00000202"),      # BRA word positive
            ("6000FF00", "FFFF", "00000002"),      # BRA word negative
            ("6710", "2704", "00000112"),          # BEQ taken (Z=1)
            ("6710", "2700", "00000102"),          # BEQ not taken (Z=0)
            ("67000100", "2704", "00000202"),      # BEQ word taken
            ("67000100", "2700", "00000104"),      # BEQ word not taken
        )
        for index, (code, sr, expected_pc) in enumerate(no_side_effect_vectors):
            image = td / f"branch-noeffect-{index}.bin"
            image.write_bytes(bytes.fromhex(code))
            d = ["12345678"] * 8
            a = ["87654321"] * 7 + ["00FF0100"]
            args = [harness, str(image), "00000100", sr, *d, *a, "0", "00FF0100", "AABBCCDD"]
            generated = subprocess.run(args, text=True, capture_output=True)
            assert generated.returncode == 0, (code, generated.stderr)
            c = td / f"branch-noeffect-{index}.c"
            exe = td / f"branch-noeffect-{index}"
            c.write_text(generated.stdout)
            assert subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
                text=True, capture_output=True,
            ).returncode == 0
            got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
            assert got["pc"] == expected_pc, (code, got)
            assert got["sr"] == sr, (code, "SR must be completely unchanged", got)
            assert got["d"] == d, (code, "every D register must be unchanged", got)
            assert got["a"] == a, (code, "every A register must be unchanged", got)
            assert got["ram"] == ["AABBCCDD"], (code, "RAM must be unchanged", got)

        # Adversarial: a branch primary ending in 0xFF must consume only its
        # two-byte primary through the harness's full generated-C path too
        # (production-level proof is the m68k_pipeline_tests unit above;
        # this proves the SAME behavior survives all the way through
        # generated-C execution). Four recognizable trailing bytes are
        # present and must never be interpreted as a long-branch extension.
        for code, sr, expected_pc in (("60FF11223344", "0000", "00000101"), ("67FF11223344", "2704", "00000101")):
            image = td / f"branch-ff-adversarial-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            args = [harness, str(image), *harness_args(sr=sr)]
            generated = subprocess.run(args, text=True, capture_output=True)
            assert generated.returncode == 0, (code, generated.stderr)
            c = td / f"branch-ff-{code}.c"
            exe = td / f"branch-ff-{code}"
            c.write_text(generated.stdout)
            assert subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
                text=True, capture_output=True,
            ).returncode == 0
            got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
            assert got["pc"] == expected_pc, (code, got)

        # Adversarial: a truncated word-form branch (low byte 0x00, but no
        # extension word present) must reject as truncated, never silently
        # accepted with a garbage displacement.
        for code in ("6000", "6700"):
            image = td / f"branch-truncated-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            rejected = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert rejected.returncode == 1 and rejected.stdout == "", (code, rejected)

        # ---------------------------------------------------------------
        # C4b: BSR
        # ---------------------------------------------------------------

        # BSR byte/word forms decode and generate deterministic C.
        bsr_legal_codes = ("6110", "61F0", "61FF", "61000100", "6100FF00")
        for code in bsr_legal_codes:
            image = td / f"bsr-legal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            args = [harness, str(image), "00000100", "0000", *(["00000000"] * 8), *(["00000000"] * 7),
                    "00FF0100", "0", "00FF00FC", "00000000"]
            first = subprocess.run(args, text=True, capture_output=True)
            second = subprocess.run(args, text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )

        # BSR pushes a real LONG return address via A7 (contract: "preserve
        # CPU stack semantics"): exact final A7, exact pushed continuation,
        # exact target PC, SR unchanged, neighboring RAM unchanged.
        # (code_hex, expected_pc, expected_continuation)
        bsr_stack_vectors = (
            ("6110", "00000112", "00000102"),      # BSR byte forward
            ("61F0", "000000F2", "00000102"),      # BSR byte negative
            ("61FF", "00000101", "00000102"),      # BSR byte -1 (MC68000 regression)
            ("61000100", "00000202", "00000104"),  # BSR word positive
            ("6100FF00", "00000002", "00000104"),  # BSR word negative
        )
        for index, (code, expected_pc, expected_continuation) in enumerate(bsr_stack_vectors):
            image = td / f"bsr-stack-{index}.bin"
            image.write_bytes(bytes.fromhex(code))
            args = [harness, str(image), "00000100", "FFFF", *(["00000000"] * 8), *(["00000000"] * 7),
                    "00FF0100", "0", "00FF00FC", "AABBCCDD"]
            generated = subprocess.run(args, text=True, capture_output=True)
            assert generated.returncode == 0, (code, generated.stderr)
            c = td / f"bsr-stack-{index}.c"
            exe = td / f"bsr-stack-{index}"
            c.write_text(generated.stdout)
            assert subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
                text=True, capture_output=True,
            ).returncode == 0
            got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
            assert got["pc"] == expected_pc, (code, "target PC", got)
            assert got["a"][7] == "00FF00FC", (code, "A7 decremented by exactly 4", got)
            assert got["sr"] == "FFFF", (code, "SR must be completely unchanged", got)
            assert got["ram"] == [expected_continuation], (code, "the pushed continuation must match exactly", got)

        # Adversarial: a truncated word-form BSR (low byte 0x00, no extension
        # word present) must reject as truncated.
        image = td / "bsr-truncated.bin"
        image.write_bytes(bytes.fromhex("6100"))
        rejected = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
        assert rejected.returncode == 1 and rejected.stdout == "", rejected

        # ---------------------------------------------------------------
        # C4c: DBcc
        # ---------------------------------------------------------------

        # Every DBcc condition selector (all 16) decodes and generates
        # deterministic C, for both D0 and a non-D0 register field.
        dbcc_legal_codes = tuple(f"{0x50C8 | (cc << 8):04x}0010" for cc in range(16)) + ("56CB0010",)
        for code in dbcc_legal_codes:
            image = td / f"dbcc-legal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            first = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            second = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )

        # DBcc semantic order is load-bearing: condition-first. If the
        # condition is TRUE, do NOT decrement -- fall through unconditionally.
        # If FALSE, decrement the LOW WORD of Dn (preserving the upper 16
        # bits) and branch unless the result is exactly 0xFFFF (expired).
        # (code_hex, seed_sr, seed_d0, expected_d0, expected_pc)
        dbcc_vectors = (
            ("50C80010", "0000", "12340005", "12340005", "00000104"),  # DBT: always true, never decrements
            ("51C80010", "0000", "12340001", "12340000", "00000112"),  # DBF: decrement 1->0, branch taken
            ("51C80010", "0000", "12340000", "1234FFFF", "00000104"),  # DBF: decrement 0->FFFF, expired->fallthrough
            ("51C80010", "0000", "ABCD0001", "ABCD0000", "00000112"),  # DBF: upper word preserved through a branch
            ("51C8FFF0", "0000", "00000001", "00000000", "000000F2"),  # DBF: negative displacement, branch taken
            ("56C80010", "0000", "12340005", "12340005", "00000104"),  # DBNE, Z=0 (NE true): no decrement, fallthrough
            ("56C80010", "0004", "12340005", "12340004", "00000112"),  # DBNE, Z=1 (NE false): decrement, branch taken
        )
        for index, (code, sr, seed_d0, expected_d0, expected_pc) in enumerate(dbcc_vectors):
            image = td / f"dbcc-vector-{index}.bin"
            image.write_bytes(bytes.fromhex(code))
            args = [harness, str(image), *harness_args(d0=seed_d0, sr=sr)]
            generated = subprocess.run(args, text=True, capture_output=True)
            assert generated.returncode == 0, (code, generated.stderr)
            c = td / f"dbcc-vector-{index}.c"
            exe = td / f"dbcc-vector-{index}"
            c.write_text(generated.stdout)
            assert subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
                text=True, capture_output=True,
            ).returncode == 0
            got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
            assert got["d"][0] == expected_d0 and got["pc"] == expected_pc and got["sr"] == sr, (
                code, "DBcc condition-first semantics, upper-word preservation, and SR-unchanged", got,
            )

        # SEG-007-T025 (Batch C, C4c review correction) contract: "add direct
        # host / generated-C parity coverage" -- the generated-C half of that
        # parity. These six (seed_d0, expected_d0, expired) cases mirror
        # tests/m68k_pipeline_test.cpp's
        # dbcc_decrement_boundary_preserves_upper_word_and_reports_expiry
        # host-side parity cases exactly; both independently exercise the
        # ONE shared M68kDbccDecrementSpecification owner (generated C via
        # emit_c_update(), host via evaluate()) so neither side can silently
        # drift from the other. DBF (always false) is used so every case
        # always decrements; SR is poisoned (0xFFFF) to also prove it stays
        # completely unchanged.
        dbcc_parity_cases = (
            ("00000000", "0000FFFF", True),   # 0 -> FFFF, expired
            ("00000001", "00000000", False),  # 1 -> 0, not expired
            ("00000002", "00000001", False),  # 2 -> 1, not expired
            ("ABCD0000", "ABCDFFFF", True),   # upper word preserved; 0 -> FFFF, expired
            ("ABCD0001", "ABCD0000", False),  # upper word preserved; 1 -> 0, not expired
            ("ABCDFFFF", "ABCDFFFE", False),  # upper word preserved; FFFF -> FFFE, not expired
        )
        for index, (seed_d0, expected_d0, expired) in enumerate(dbcc_parity_cases):
            image = td / f"dbcc-parity-{index}.bin"
            image.write_bytes(bytes.fromhex("51C80010"))  # DBF D0,+0x10
            args = [harness, str(image), *harness_args(d0=seed_d0, sr="FFFF")]
            generated = subprocess.run(args, text=True, capture_output=True)
            assert generated.returncode == 0, generated.stderr
            c = td / f"dbcc-parity-{index}.c"
            exe = td / f"dbcc-parity-{index}"
            c.write_text(generated.stdout)
            assert subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
                text=True, capture_output=True,
            ).returncode == 0
            got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
            expected_pc = "00000104" if expired else "00000112"
            assert got["d"][0] == expected_d0 and got["pc"] == expected_pc and got["sr"] == "FFFF", (
                seed_d0, "host/generated-C DBcc decrement parity", got,
            )

        # DBcc performs no memory access at all: RAM must remain
        # byte-identical to its seed (reusing one representative vector with
        # a seeded RAM word to prove it).
        image = td / "dbcc-no-memory-access.bin"
        image.write_bytes(bytes.fromhex("51C80010"))  # DBF D0,+0x10
        args = [harness, str(image), "00000100", "0000", "00000001", *(["00000000"] * 7),
                *(["00000000"] * 7), "00FF0100", "0", "00FF0100", "AABBCCDD"]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "dbcc-no-memory-access.c"
        exe = td / "dbcc-no-memory-access"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["ram"] == ["AABBCCDD"], got

        # Adversarial: a truncated DBcc (primary word present, no 16-bit
        # displacement extension word) must reject as truncated -- DBcc
        # always requires the extension word, unlike Bcc's byte form.
        image = td / "dbcc-truncated.bin"
        image.write_bytes(bytes.fromhex("50C8"))
        rejected = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
        assert rejected.returncode == 1 and rejected.stdout == "", rejected

        # Adversarial: legal Scc encodings (bit3=0), immediately neighboring
        # DBcc's own bit3=1 slot in the same 0101-cccc opcode region, must
        # never be accepted by the harness's DBcc-only acceptance path (this
        # project does not implement Scc).
        for code in ("56C2", "57D0"):  # SNE D2 (register destination); SEQ (A0) (memory destination)
            image = td / f"scc-neighbor-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            rejected = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert rejected.returncode == 1 and rejected.stdout == "", (code, rejected)

        # ---------------------------------------------------------------
        # C5a: MOVEM.W/L ordinary (non-auto-update) forms
        # ---------------------------------------------------------------

        def movem_args(pc="00000100", sr="0000", d=None, a=None, ram=None):
            d = (list(d) if d else []) + ["00000000"] * 8
            a = (list(a) if a else []) + ["00000000"] * 8
            return [pc, sr, *d[:8], *a[:8], "0", *(value for pair in (ram or []) for value in pair)]

        # Every representative legal form across the confirmed C5a matrix
        # decodes and generates deterministic C: register->memory (An)/
        # d16(An)/absolute.w/absolute.l, and memory->register (An)/d16(An)/
        # absolute.w/absolute.l. d16(PC) is exercised separately below (its
        # resolved target depends on PC, unlike every other form here, so it
        # needs its own correctly laid-out image rather than this shared
        # zero-PC-independent determinism list).
        movem_legal_codes = (
            "48910001",          # MOVEM.W D0,(A1)
            "48D10003",          # MOVEM.L D0-D1,(A1)
            "48EA00030004",      # MOVEM.L D0-D1,4(A2)
            "48B80001FF10",      # MOVEM.W D0,(xxx).W
            "48F9000100FF0010",  # MOVEM.L D0,(xxx).L
            "4C910001",          # MOVEM.W (A1),D0
            "4CE900030004",      # MOVEM.L 4(A1),D0-D1
            "4CB80001FF10",      # MOVEM.W (xxx).W,D0
            "4CF9000100FF0010",  # MOVEM.L (xxx).L,D0
        )
        for code in movem_legal_codes:
            image = td / f"movem-legal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            first = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            second = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )

        # Transfer order + D/A boundary regression (register->memory): a
        # sparse mask crossing D0/D7/A0/A7 must write to ascending addresses
        # in exactly that ascending-mask-bit order -- an incorrect
        # contiguous-only/8-register-only implementation cannot pass. A1 (the
        # base register) is deliberately excluded from the mask; the base-
        # register-alias case is reserved for C5b/C5c.
        image = td / "movem-order-register-to-memory.bin"
        image.write_bytes(bytes.fromhex("48D18181"))  # MOVEM.L D0,D7,A0,A7,(A1)
        args = [harness, str(image), *movem_args(
            sr="FFFF",
            d=["11111111", "00000000", "00000000", "00000000", "00000000", "00000000", "00000000", "77777777"],
            a=["88888888", "00FF0300", "00000000", "00000000", "00000000", "00000000", "00000000", "AAAAAAAA"],
            ram=[("00FF0300", "00000000"), ("00FF0304", "00000000"), ("00FF0308", "00000000"), ("00FF030C", "00000000")],
        )]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-order-register-to-memory.c"
        exe = td / "movem-order-register-to-memory"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["ram"] == ["11111111", "77777777", "88888888", "AAAAAAAA"] and got["pc"] == "00000104" \
            and got["sr"] == "FFFF", got

        # Transfer order + D/A boundary + WORD sign extension regression
        # (memory->register): every loaded WORD sign-extends to 32 bits for
        # BOTH Dn and An.
        image = td / "movem-order-memory-to-register.bin"
        image.write_bytes(bytes.fromhex("4C918181"))  # MOVEM.W (A1),D0,D7,A0,A7
        args = [harness, str(image), *movem_args(
            sr="FFFF",
            a=["00000000", "00FF0400"],
            ram=[("00FF0400", "80007FFF"), ("00FF0404", "00018000")],
        )]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-order-memory-to-register.c"
        exe = td / "movem-order-memory-to-register"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["d"][0] == "FFFF8000" and got["d"][7] == "00007FFF" and got["a"][0] == "00000001" \
            and got["a"][7] == "FFFF8000" and got["pc"] == "00000104" and got["sr"] == "FFFF", got

        # ROM-resident d16(PC) memory->register read (contract: "ROM:
        # memory->register MOVEM reads may succeed"). PC is deliberately 0
        # here so the resolved absolute target coincides with this test's
        # own image byte offset (the shared resolver treats a synthetic
        # image span as beginning at program address 0, exactly like every
        # other direct-image ROM-read check in this harness).
        image = td / "movem-pc-relative-rom-read.bin"
        image.write_bytes(bytes.fromhex("4CBA00010004FFFF8000"))  # MOVEM.W 4(PC),D0 ; filler ; data word 0x8000
        args = [harness, str(image), *movem_args(pc="00000000", sr="FFFF")]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-pc-relative-rom-read.c"
        exe = td / "movem-pc-relative-rom-read"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["d"][0] == "FFFF8000" and got["pc"] == "00000006" and got["sr"] == "FFFF", got

        # Absolute.w/absolute.l representative forms round-trip through RAM.
        image = td / "movem-absolute-word-register-to-memory.bin"
        image.write_bytes(bytes.fromhex("48B80001FF10"))  # MOVEM.W D0,(xxx).W
        args = [harness, str(image), *movem_args(
            sr="0000", d=["0000ABCD"],
            ram=[("00FFFF10", "00000000")],
        )]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-absolute-word-register-to-memory.c"
        exe = td / "movem-absolute-word-register-to-memory"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["ram"] == ["ABCD0000"] and got["pc"] == "00000106", got

        image = td / "movem-absolute-long-memory-to-register.bin"
        image.write_bytes(bytes.fromhex("4CF9000100FF0010"))  # MOVEM.L (xxx).L,D0
        args = [harness, str(image), *movem_args(sr="0000", ram=[("00FF0010", "89ABCDEF")])]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-absolute-long-memory-to-register.c"
        exe = td / "movem-absolute-long-memory-to-register"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["d"][0] == "89ABCDEF" and got["pc"] == "00000108", got

        # d16(An) register->memory representative, two registers.
        image = td / "movem-d16an-register-to-memory.bin"
        image.write_bytes(bytes.fromhex("48EA00030004"))  # MOVEM.L D0-D1,4(A2)
        args = [harness, str(image), *movem_args(
            sr="0000", d=["11112222", "33334444"], a=["00000000", "00000000", "00FF0500"],
            ram=[("00FF0504", "00000000"), ("00FF0508", "00000000")],
        )]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-d16an-register-to-memory.c"
        exe = td / "movem-d16an-register-to-memory"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["ram"] == ["11112222", "33334444"] and got["pc"] == "00000106", got

        # Empty mask (section: "explicit oracle-backed empty-mask boundary"):
        # zero transfers, zero memory access, correct PC advancement, SR
        # unchanged.
        image = td / "movem-empty-mask.bin"
        image.write_bytes(bytes.fromhex("48D10000"))  # MOVEM.L (none),(A1)
        args = [harness, str(image), *movem_args(
            sr="FFFF", a=["00000000", "00FF0300"], ram=[("00FF0300", "AABBCCDD")],
        )]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-empty-mask.c"
        exe = td / "movem-empty-mask"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["ram"] == ["AABBCCDD"] and got["pc"] == "00000104" and got["sr"] == "FFFF", got

        # Adversarial: illegal/not-yet-legal EA modes for each direction.
        # -(An)/(An)+ are architecturally reserved for register->memory/
        # memory->register respectively; Dn/An-direct/immediate/d16(PC)-for-
        # writes are never legal at all.
        movem_illegal_codes = (
            "48990001",  # MOVEM.W D0,(A1)+ -- postinc illegal for register->memory (ever)
            "4CE10001",  # MOVEM.L -(A1),D0 -- predecrement illegal for memory->register (ever)
            "4C810001",  # MOVEM.W D1,D-list -- Dn direct illegal as a memory->register source
            "4C890001",  # MOVEM.W A1,D-list -- An direct illegal as a memory->register source
            "48FA00010004",  # MOVEM.L D0,4(PC) -- PC-relative illegal for register->memory
            "48BC0001",  # MOVEM.W D0,#imm -- immediate illegal
        )
        for code in movem_illegal_codes:
            image = td / f"movem-illegal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            rejected = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert rejected.returncode == 1 and rejected.stdout == "", (code, rejected)

        # Adversarial: truncation. Missing mask word; mask present but
        # missing EA extension; a partially-present absolute.l EA.
        movem_truncated_codes = (
            "4891",              # MOVEM.W D-list,(A1): mask word missing entirely
            "48A90001",          # MOVEM.W D0,4(A1): mask present, displacement extension missing
            "48F90001000001",    # MOVEM.L D0,(xxx).L: only 3 of 4 address bytes present
        )
        for code in movem_truncated_codes:
            image = td / f"movem-truncated-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            rejected = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert rejected.returncode == 1 and rejected.stdout == "", (code, rejected)

        # ---------------------------------------------------------------
        # C5a review correction: ordinary MOVEM working-EA base-alias
        # regressions (contract: "snapshot the ordinary MOVEM working EA
        # once"). Each case seeds the EA base register's own FIRST loaded
        # value with the high bit set, so a sign-extended reinterpretation
        # of that just-loaded value as the next transfer's base would
        # compute an address far outside the synthetic RAM window --
        # triggering the runtime guard's `return 1;` (no JSON at all) under
        # the OLD (unfixed) implementation, which re-embedded the live
        # `a[n]` expression per transfer instead of snapshotting the
        # original effective address once. The corrected implementation
        # must still read every transfer from the ORIGINAL base + slot *
        # width and complete successfully.
        # ---------------------------------------------------------------

        # A. WORD (An) alias: MOVEM.W (A0),A0-A1.
        image = td / "movem-alias-word-an.bin"
        image.write_bytes(bytes.fromhex("4C900300"))  # MOVEM.W (A0),A0-A1
        args = [harness, str(image), *movem_args(
            sr="FFFF", a=["00FF0600"], ram=[("00FF0600", "80001234")],
        )]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-alias-word-an.c"
        exe = td / "movem-alias-word-an"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        result = subprocess.run([str(exe)], text=True, capture_output=True)
        assert result.returncode == 0, (
            "old buggy sequencing would re-derive the second transfer from the newly-loaded A0, "
            "landing far outside the synthetic RAM window and failing the runtime guard", result,
        )
        got = json.loads(result.stdout)
        assert got["a"][0] == "FFFF8000" and got["a"][1] == "00001234" and got["pc"] == "00000104" \
            and got["sr"] == "FFFF" and got["ram"] == ["80001234"], got

        # B. LONG (An) alias: MOVEM.L (A0),A0-A1.
        image = td / "movem-alias-long-an.bin"
        image.write_bytes(bytes.fromhex("4CD00300"))  # MOVEM.L (A0),A0-A1
        args = [harness, str(image), *movem_args(
            sr="FFFF", a=["00FF0700"],
            ram=[("00FF0700", "11223344"), ("00FF0704", "55667788")],
        )]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-alias-long-an.c"
        exe = td / "movem-alias-long-an"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        result = subprocess.run([str(exe)], text=True, capture_output=True)
        assert result.returncode == 0, (
            "old buggy sequencing would re-derive the second transfer from the newly-loaded A0", result,
        )
        got = json.loads(result.stdout)
        assert got["a"][0] == "11223344" and got["a"][1] == "55667788" and got["pc"] == "00000104" \
            and got["sr"] == "FFFF", got

        # C. d16(An) alias: MOVEM.W disp(A0),A0-A1.
        image = td / "movem-alias-d16an.bin"
        image.write_bytes(bytes.fromhex("4CA803000010"))  # MOVEM.W 16(A0),A0-A1
        args = [harness, str(image), *movem_args(
            sr="FFFF", a=["00FF0600"], ram=[("00FF0610", "80012222")],
        )]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-alias-d16an.c"
        exe = td / "movem-alias-d16an"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        result = subprocess.run([str(exe)], text=True, capture_output=True)
        assert result.returncode == 0, (
            "old buggy sequencing would re-derive the second transfer's base+disp from the "
            "newly-loaded A0", result,
        )
        got = json.loads(result.stdout)
        assert got["a"][0] == "FFFF8001" and got["a"][1] == "00002222" and got["pc"] == "00000106" \
            and got["sr"] == "FFFF", got

        # D. A later (non-A0/A1) address register following the EA base,
        # proving this is not accidentally special-cased for a contiguous
        # A0-A1 list: source is (A1), destination mask includes A1 and A6.
        image = td / "movem-alias-later-register.bin"
        image.write_bytes(bytes.fromhex("4C914200"))  # MOVEM.W (A1),A1,A6
        args = [harness, str(image), *movem_args(
            sr="FFFF", a=["00000000", "00FF0620"], ram=[("00FF0620", "99990042")],
        )]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-alias-later-register.c"
        exe = td / "movem-alias-later-register"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        result = subprocess.run([str(exe)], text=True, capture_output=True)
        assert result.returncode == 0, (
            "old buggy sequencing would re-derive the A6 transfer from the newly-loaded A1", result,
        )
        got = json.loads(result.stdout)
        assert got["a"][1] == "FFFF9999" and got["a"][6] == "00000042" and got["pc"] == "00000104" \
            and got["sr"] == "FFFF", got

        # ---------------------------------------------------------------
        # C5b: MOVEM.W/L register->memory predecrement forms
        # ---------------------------------------------------------------

        # Legal predecrement forms decode and generate deterministic C.
        movem_predec_legal_codes = (
            "48A10001",  # MOVEM.W raw-mask-0001,-(A1): selects A7
            "48E10003",  # MOVEM.L raw-mask-0003,-(A1) (predecrement: selects A7,A6, not D0-D1)
        )
        for code in movem_predec_legal_codes:
            image = td / f"movem-predec-legal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            first = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            second = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )

        # Reversed transfer order + sparse A/D-boundary regression. Mask
        # 0x8181 (raw bits 0,7,8,15) reverses to transfer order
        # [A7, A0, D7, D0] (contract: "predecrement mask mapping is
        # reversed"); addresses decrement once per transfer, so an incorrect
        # "ordinary ascending block reversed afterward" implementation
        # cannot pass this seeding/readback layout.
        image = td / "movem-predec-order-da-boundary.bin"
        image.write_bytes(bytes.fromhex("48A18181"))  # MOVEM.W D0,D7,A0,A7,-(A1)
        args = [harness, str(image), *movem_args(
            sr="FFFF",
            d=["11111111", "00000000", "00000000", "00000000", "00000000", "00000000", "00000000", "77777777"],
            a=["88888888", "00FF0400", "00000000", "00000000", "00000000", "00000000", "00000000", "AAAAAAAA"],
            ram=[("00FF03F8", "00000000"), ("00FF03FC", "00000000")],
        )]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-predec-order-da-boundary.c"
        exe = td / "movem-predec-order-da-boundary"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["ram"] == ["11117777", "8888AAAA"] and got["a"][1] == "00FF03F8" \
            and got["pc"] == "00000104" and got["sr"] == "FFFF", got
        # Source registers themselves are never mutated by register->memory
        # MOVEM (only the base An updates, via its one final writeback).
        assert got["d"][0] == "11111111" and got["d"][7] == "77777777" and got["a"][0] == "88888888" \
            and got["a"][7] == "AAAAAAAA", got

        # Base-register-alias regression, WORD: the base An is itself the
        # sole selected register. The value stored must be the ORIGINAL
        # architectural An, never a partially-decremented working EA.
        image = td / "movem-predec-alias-word.bin"
        image.write_bytes(bytes.fromhex("48A00080"))  # MOVEM.W A0,-(A0)
        # A0 = 00FF0502 so the single WORD transfer address (A0 - 2 =
        # 00FF0500) is 4-byte aligned, matching the harness's ram_seed
        # readback requirement.
        args = [harness, str(image), *movem_args(sr="FFFF", a=["00FF0502"], ram=[("00FF0500", "00000000")])]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-predec-alias-word.c"
        exe = td / "movem-predec-alias-word"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["ram"] == ["05020000"] and got["a"][0] == "00FF0500" and got["pc"] == "00000104" \
            and got["sr"] == "FFFF", (
            "generated C must store the ORIGINAL A0 (low word 0502), never the "
            "already-decremented working EA", got,
        )

        # Base-register-alias regression, LONG.
        image = td / "movem-predec-alias-long.bin"
        image.write_bytes(bytes.fromhex("48E00080"))  # MOVEM.L A0,-(A0)
        args = [harness, str(image), *movem_args(sr="FFFF", a=["00FF0600"], ram=[("00FF05FC", "00000000")])]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-predec-alias-long.c"
        exe = td / "movem-predec-alias-long"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["ram"] == ["00FF0600"] and got["a"][0] == "00FF05FC" and got["pc"] == "00000104" \
            and got["sr"] == "FFFF", (
            "generated C must store the ORIGINAL A0 (00FF0600), never the already-decremented "
            "working EA", got,
        )

        # A7-as-base regression: base register in list, A7 as base, LONG,
        # reversed mask, plus one non-base register -- combines every
        # section-15 requirement in one vector.
        image = td / "movem-predec-alias-a7.bin"
        image.write_bytes(bytes.fromhex("48E78001"))  # MOVEM.L D0,A7,-(A7)
        args = [harness, str(image), *movem_args(
            sr="FFFF", d=["99999999"], a=["00000000", "00000000", "00000000", "00000000",
                                          "00000000", "00000000", "00000000", "00FF0700"],
            ram=[("00FF06F8", "00000000"), ("00FF06FC", "00000000")],
        )]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-predec-alias-a7.c"
        exe = td / "movem-predec-alias-a7"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["ram"] == ["99999999", "00FF0700"] and got["a"][7] == "00FF06F8" \
            and got["pc"] == "00000104" and got["sr"] == "FFFF", got

        # Empty mask: zero writes, SR unchanged, correct PC, and -- unlike
        # C5a's ordinary forms -- the base An still receives its one final
        # writeback, which for zero transfers leaves it exactly unchanged.
        image = td / "movem-predec-empty-mask.bin"
        image.write_bytes(bytes.fromhex("48A10000"))  # MOVEM.W (none),-(A1)
        args = [harness, str(image), *movem_args(
            sr="FFFF", a=["00000000", "00FF0300"], ram=[("00FF0300", "AABBCCDD")],
        )]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-predec-empty-mask.c"
        exe = td / "movem-predec-empty-mask"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["ram"] == ["AABBCCDD"] and got["a"][1] == "00FF0300" and got["pc"] == "00000104" \
            and got["sr"] == "FFFF", got

        # Adversarial: memory->register predecrement and register->memory
        # postincrement remain rejected (never legal / not yet selected);
        # EXT.W/EXT.L/SWAP/PEA/NBCD's own exact decode slots are unaffected
        # by widening the register->memory MOVEM legal set.
        movem_predec_neighbor_rejected_codes = (
            "4CE10001",  # MOVEM.L -(A1),D0 -- predecrement illegal for memory->register (ever)
            "48990001",  # MOVEM.W D0,(A1)+ -- postincrement illegal for register->memory (ever)
            "4840",      # SWAP D0
            "4880",      # EXT.W D0
            "48C0",      # EXT.L D0
            "4851",      # PEA (A1)
            "4800",      # NBCD D0
            "4808",      # NBCD (A0)
        )
        for code in movem_predec_neighbor_rejected_codes:
            image = td / f"movem-predec-neighbor-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            result = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            if code in ("4840", "4880", "48C0", "4851"):
                assert result.returncode == 0, (code, result.stderr)
            else:
                assert result.returncode == 1 and result.stdout == "", (code, result)

        # ---------------------------------------------------------------
        # C5c1: MOVEM.W/L memory->register postincrement forms
        # ---------------------------------------------------------------

        # Legal postincrement forms decode and generate deterministic C.
        movem_postinc_legal_codes = (
            "4C990001",  # MOVEM.W (A1)+,D0
            "4CD90003",  # MOVEM.L (A1)+,D0-D1
        )
        for code in movem_postinc_legal_codes:
            image = td / f"movem-postinc-legal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            first = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            second = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )

        # A/B/C: ordinary ascending order (never a third register-order
        # interpretation), sparse D/A-boundary mask, WORD, base register
        # (A1) NOT in the list.
        image = td / "movem-postinc-order-da-boundary.bin"
        image.write_bytes(bytes.fromhex("4C998181"))  # MOVEM.W (A1)+,D0,D7,A0,A7
        args = [harness, str(image), *movem_args(
            sr="FFFF", a=["00000000", "00FF0600"],
            ram=[("00FF0600", "11112222"), ("00FF0604", "33334444")],
        )]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-postinc-order-da-boundary.c"
        exe = td / "movem-postinc-order-da-boundary"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["d"][0] == "00001111" and got["d"][7] == "00002222" and got["a"][0] == "00003333" \
            and got["a"][7] == "00004444" and got["a"][1] == "00FF0608" and got["pc"] == "00000104" \
            and got["sr"] == "FFFF", got

        # D: strong base-register-alias regression, WORD (contract: "strong
        # alias test"). The base A0 is itself the first selected transfer;
        # its loaded, sign-extended value (0xFFFF8000) is deliberately far
        # outside the synthetic RAM window, so a buggy implementation that
        # re-derives the SECOND transfer's address from the newly-loaded
        # live A0 (instead of the original working EA + width) would fail
        # its runtime guard entirely (no JSON output at all) instead of
        # merely producing a silently wrong value.
        image = td / "movem-postinc-alias-word.bin"
        image.write_bytes(bytes.fromhex("4C980300"))  # MOVEM.W (A0)+,A0-A1
        args = [harness, str(image), *movem_args(sr="FFFF", a=["00FF0700"], ram=[("00FF0700", "80001234")])]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-postinc-alias-word.c"
        exe = td / "movem-postinc-alias-word"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        result = subprocess.run([str(exe)], text=True, capture_output=True)
        assert result.returncode == 0, (
            "old buggy sequencing would re-derive the second transfer from the newly-loaded A0, "
            "landing far outside the synthetic RAM window and failing the runtime guard", result,
        )
        got = json.loads(result.stdout)
        assert got["a"][0] == "00FF0704" and got["a"][1] == "00001234" and got["pc"] == "00000104" \
            and got["sr"] == "FFFF", (
            "final A0 must be original_base + 2*width, never the loaded (sign-extended) value", got,
        )

        # E: base-register-alias regression, LONG.
        image = td / "movem-postinc-alias-long.bin"
        image.write_bytes(bytes.fromhex("4CD80300"))  # MOVEM.L (A0)+,A0-A1
        args = [harness, str(image), *movem_args(
            sr="FFFF", a=["00FF0800"], ram=[("00FF0800", "11223344"), ("00FF0804", "55667788")],
        )]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-postinc-alias-long.c"
        exe = td / "movem-postinc-alias-long"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["a"][0] == "00FF0808" and got["a"][1] == "55667788" and got["pc"] == "00000104" \
            and got["sr"] == "FFFF", got

        # F: A7-as-base regression: base register in list, A7 as base, LONG.
        image = td / "movem-postinc-alias-a7.bin"
        image.write_bytes(bytes.fromhex("4CDF8001"))  # MOVEM.L (A7)+,D0,A7
        args = [harness, str(image), *movem_args(
            sr="FFFF", a=["00000000", "00000000", "00000000", "00000000",
                          "00000000", "00000000", "00000000", "00FF0900"],
            ram=[("00FF0900", "99999999"), ("00FF0904", "12345678")],
        )]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-postinc-alias-a7.c"
        exe = td / "movem-postinc-alias-a7"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["d"][0] == "99999999" and got["a"][7] == "00FF0908" and got["pc"] == "00000104" \
            and got["sr"] == "FFFF", got

        # G: empty mask. Zero reads, working EA unchanged, final An ==
        # original An, correct PC, SR unchanged.
        image = td / "movem-postinc-empty-mask.bin"
        image.write_bytes(bytes.fromhex("4C990000"))  # MOVEM.W (A1)+,(none)
        args = [harness, str(image), *movem_args(
            sr="FFFF", a=["00000000", "00FF0300"], ram=[("00FF0300", "AABBCCDD")],
        )]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "movem-postinc-empty-mask.c"
        exe = td / "movem-postinc-empty-mask"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["ram"] == ["AABBCCDD"] and got["a"][1] == "00FF0300" and got["pc"] == "00000104" \
            and got["sr"] == "FFFF", got

        # Collision/legality regression: (An)+ is accepted only for
        # memory->register; register->memory postincrement and
        # memory->register predecrement remain rejected; EXT.W/EXT.L/SWAP/
        # PEA/NBCD's own exact decode slots are unaffected.
        movem_postinc_neighbor_rejected_codes = (
            "48990001",  # MOVEM.W D0,(A1)+ -- postincrement illegal for register->memory (ever)
            "4CE10001",  # MOVEM.L -(A1),D0 -- predecrement illegal for memory->register (ever)
            "4840",      # SWAP D0
            "4880",      # EXT.W D0
            "48C0",      # EXT.L D0
            "4851",      # PEA (A1)
            "4800",      # NBCD D0
            "4808",      # NBCD (A0)
        )
        for code in movem_postinc_neighbor_rejected_codes:
            image = td / f"movem-postinc-neighbor-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            result = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            if code in ("4840", "4880", "48C0", "4851"):
                assert result.returncode == 0, (code, result.stderr)
            else:
                assert result.returncode == 1 and result.stdout == "", (code, result)

        # ---------------------------------------------------------------
        # C6a: LSL / LSR register shift forms
        # ---------------------------------------------------------------

        # Legal forms decode and generate deterministic C: immediate and
        # register count sources, all three widths, both directions.
        shift_legal_codes = (
            "E308",  # LSL.B #1,D0
            "E208",  # LSR.B #1,D0
            "E368",  # LSL.W D1,D0
            "E4AB",  # LSR.L D2,D3
        )
        for code in shift_legal_codes:
            image = td / f"shift-legal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            first = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            second = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )

        # LSL.B #1,D0: ordinary 1<count<width case, upper Dn bits preserved,
        # poisoned unrelated SR bits survive unchanged.
        image = td / "shift-lsl-byte-boundary.bin"
        image.write_bytes(bytes.fromhex("E308"))
        args = [harness, str(image), *movem_args(sr="A700", d=["AAAAAA81"])]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "shift-lsl-byte-boundary.c"
        exe = td / "shift-lsl-byte-boundary"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["d"][0] == "AAAAAA02" and got["sr"] == "A711" and got["pc"] == "00000102", got

        # LSR.B with immediate encoded field 0 -> count 8 (== width): result
        # 0, C=X= the single remaining edge bit, upper Dn bits preserved.
        image = td / "shift-lsr-byte-count-eq-width.bin"
        image.write_bytes(bytes.fromhex("E008"))  # LSR.B #8,D0 (encoded field 0)
        args = [harness, str(image), *movem_args(sr="A700", d=["AAAAAA81"])]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "shift-lsr-byte-count-eq-width.c"
        exe = td / "shift-lsr-byte-count-eq-width"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["d"][0] == "AAAAAA00" and got["sr"] == "A715" and got["pc"] == "00000102", got

        # Immediate encoded field 0 -> count 8 for LSL too, execution level
        # (not merely decode level).
        image = td / "shift-lsl-byte-immediate-zero-is-eight.bin"
        image.write_bytes(bytes.fromhex("E108"))  # LSL.B #8,D0 (encoded field 0)
        args = [harness, str(image), *movem_args(sr="0000", d=["ABCDEFFF"])]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "shift-lsl-byte-immediate-zero-is-eight.c"
        exe = td / "shift-lsl-byte-immediate-zero-is-eight"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["d"][0] == "ABCDEF00" and got["sr"] == "0015" and got["pc"] == "00000102", got

        # Register count exceeding width (17 > 16 for WORD): result 0, C and
        # X both CLEARED, distinct from the immediate form (which can never
        # exceed 8).
        image = td / "shift-lsr-word-register-count-beyond-width.bin"
        image.write_bytes(bytes.fromhex("E26A"))  # LSR.W D1,D2
        args = [harness, str(image), *movem_args(sr="A700", d=["00000000", "00000011", "12348001"])]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "shift-lsr-word-register-count-beyond-width.c"
        exe = td / "shift-lsr-word-register-count-beyond-width"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["d"][2] == "12340000" and got["sr"] == "A704" and got["pc"] == "00000102", got

        # Register-count zero (contract: "count-zero is a first-class
        # boundary"): destination unchanged, C cleared, X PRESERVED (seeded
        # set), N/Z from the unchanged sized destination.
        image = td / "shift-lsl-word-register-count-zero.bin"
        image.write_bytes(bytes.fromhex("E768"))  # LSL.W D3,D0
        args = [harness, str(image), *movem_args(sr="A710", d=["1234ABCD", "00000000", "00000000", "00000000"])]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "shift-lsl-word-register-count-zero.c"
        exe = td / "shift-lsl-word-register-count-zero"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["d"][0] == "1234ABCD" and got["sr"] == "A718" and got["pc"] == "00000102", (
            "register count 0 must leave the destination unchanged, clear C, and PRESERVE (not "
            "clear) the already-set X", got,
        )

        # Source-count/destination alias (contract: "source count must be
        # materialized first"): LSL.L D0,D0 -- the count source and
        # destination are the SAME register. Must not crash and must produce
        # the architecturally correct result.
        image = td / "shift-lsl-long-alias-d0-d0.bin"
        image.write_bytes(bytes.fromhex("E1A8"))  # LSL.L D0,D0
        args = [harness, str(image), *movem_args(sr="A700", d=["00000001"])]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "shift-lsl-long-alias-d0-d0.c"
        exe = td / "shift-lsl-long-alias-d0-d0"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["d"][0] == "00000002" and got["sr"] == "A700" and got["pc"] == "00000102", got

        # No data memory access at all for register shift/rotate forms.
        image = td / "shift-no-memory-access.bin"
        image.write_bytes(bytes.fromhex("E308"))  # LSL.B #1,D0
        args = [harness, str(image), "00000100", "0000", "00000001", *(["00000000"] * 7),
                *(["00000000"] * 7), "00FF0100", "0", "00FF0100", "AABBCCDD"]
        generated = subprocess.run(args, text=True, capture_output=True)
        assert generated.returncode == 0, generated.stderr
        c = td / "shift-no-memory-access.c"
        exe = td / "shift-no-memory-access"
        c.write_text(generated.stdout)
        assert subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
            text=True, capture_output=True,
        ).returncode == 0
        got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
        assert got["ram"] == ["AABBCCDD"], got

        # ---------------------------------------------------------------
        # C6b: ASL / ASR register shift forms
        # ---------------------------------------------------------------

        shift_b_legal_codes = (
            "E200",  # ASR.B #1,D0
            "E300",  # ASL.B #1,D0
            "E660",  # ASR.W D3,D0
            "E3A0",  # ASL.L D1,D0
        )
        for code in shift_b_legal_codes:
            image = td / f"shift-b-legal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            first = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            second = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )

        def run_shift(name, code_hex, sr, d, expected_d_index, expected_d, expected_sr, expected_pc="00000102"):
            image = td / f"{name}.bin"
            image.write_bytes(bytes.fromhex(code_hex))
            args = [harness, str(image), *movem_args(sr=sr, d=d)]
            generated = subprocess.run(args, text=True, capture_output=True)
            assert generated.returncode == 0, generated.stderr
            c = td / f"{name}.c"
            exe = td / name
            c.write_text(generated.stdout)
            assert subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
                text=True, capture_output=True,
            ).returncode == 0
            got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
            assert got["d"][expected_d_index] == expected_d and got["sr"] == expected_sr \
                and got["pc"] == expected_pc, (name, got)
            return got

        # A: ASR.B #1,D0, negative source.
        run_shift("shift-asr-byte-negative", "E200", "A700", ["AAAAAA81"], 0, "AAAAAAC0", "A719")

        # B: ASR.W D3,D0, register count zero, X initially set (must be
        # PRESERVED, never cleared).
        run_shift("shift-asr-word-count-zero-preserves-x", "E660", "A710",
                  ["1234ABCD", "00000000", "00000000", "00000000"], 0, "1234ABCD", "A718")

        # C: ASR.L D1,D0, register count == 32, negative source (must not
        # execute a native >>32; saturates to all-ones).
        run_shift("shift-asr-long-count-eq-32-negative", "E2A0", "A700",
                  ["80000001", "00000020"], 0, "FFFFFFFF", "A719")

        # D: ASR.L D2,D0, register count > 32, positive source (saturates to
        # zero).
        run_shift("shift-asr-long-count-gt-32-positive", "E4A0", "A700",
                  ["40000001", "00000000", "00000021"], 0, "00000000", "A704")

        # E: ASL.B #1,D0, ordinary (V=0).
        run_shift("shift-asl-byte-ordinary", "E300", "A700", ["AAAAAA10"], 0, "AAAAAA20", "A700")

        # F: ASL.B #8,D0 (encoded field 0 -> count 8), full-width boundary:
        # V=1 even though the result is 0.
        run_shift("shift-asl-byte-count-eq-width", "E100", "A700", ["AAAAAA01"], 0, "AAAAAA00", "A717")

        # G: ASL.B D1,D0, register count 9 (> width).
        run_shift("shift-asl-byte-register-count-gt-width", "E320", "A700",
                  ["AAAAAA01", "00000009"], 0, "AAAAAA00", "A706")

        # H: ASL.L D1,D0, register count == 32 (must not execute a native
        # <<32).
        run_shift("shift-asl-long-count-eq-32", "E3A0", "A700", ["00000001", "00000020"], 0, "00000000", "A717")

        # I: ASL.L D2,D0, register count == 33.
        run_shift("shift-asl-long-count-eq-33", "E5A0", "A700", ["00000001", "00000000", "00000021"], 0,
                  "00000000", "A706")

        # J: the mandatory multi-bit endpoint-sign-equal-but-V=1 case:
        # ASL.B #2,D0 on 0x50 (0x50 -> 0xA0 -> 0x40; initial and final signs
        # match, but V must still be 1).
        run_shift("shift-asl-byte-multibit-overflow", "E500", "A700", ["AAAAAA50"], 0, "AAAAAA40", "A713")

        # K: source/destination alias, ASL.L D0,D0.
        run_shift("shift-asl-long-alias-d0-d0", "E1A0", "A700", ["00000001"], 0, "00000002", "A700")

        # ---------------------------------------------------------------
        # C6c: ROL / ROR register shift forms
        # ---------------------------------------------------------------

        shift_c_legal_codes = (
            "E318",  # ROL.B #1,D0
            "E218",  # ROR.B #1,D0
            "E378",  # ROL.W D1,D0
            "E2B8",  # ROR.L D1,D0
        )
        for code in shift_c_legal_codes:
            image = td / f"shift-c-legal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            first = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            second = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )

        # A: ROL.B #1,D0, source with both LSB and MSB set (contract:
        # "carry strong regressions").
        run_shift("shift-rol-byte-1", "E318", "A700", ["AAAAAA81"], 0, "AAAAAA03", "A701")

        # B: ROR.B #1,D0, same source.
        run_shift("shift-ror-byte-1", "E218", "A700", ["AAAAAA81"], 0, "AAAAAAC0", "A709")

        # C: ROL.B #8,D0 (encoded field 0 -> count 8), full-width nonzero
        # rotation (contract: "immediate BYTE #8 is load-bearing"): data
        # unchanged but C must reflect the genuine rotated-out bit, never
        # collapse into the true-zero-count C=0 path.
        run_shift("shift-rol-byte-full-cycle-8", "E118", "A700", ["AAAAAA81"], 0, "AAAAAA81", "A709")

        # D: ROR.B #8,D0, same load-bearing full-width case.
        run_shift("shift-ror-byte-full-cycle-8", "E018", "A700", ["AAAAAA81"], 0, "AAAAAA81", "A709")

        # E: ROL.W D1,D0, register count TRUE zero (D1's low six bits are
        # all zero), X initially SET -- must be PRESERVED, never cleared or
        # treated as a full-width rotate.
        run_shift("shift-rol-word-count-zero-x-set", "E378", "A710",
                  ["1234ABCD", "00000000"], 0, "1234ABCD", "A718")

        # F: ROR.W D1,D0, register count TRUE zero, with BOTH X initially
        # clear and X initially set -- X must independently track its own
        # initial state in each case, never the data/carry result.
        run_shift("shift-ror-word-count-zero-x-clear", "E278", "A700",
                  ["1234ABCD", "00000000"], 0, "1234ABCD", "A708")
        run_shift("shift-ror-word-count-zero-x-set", "E278", "A710",
                  ["1234ABCD", "00000000"], 0, "1234ABCD", "A718")

        # G: ROL.W D1,D0, register count == width(16): unchanged data, but a
        # genuine rotated-out C, distinct from the true-zero-count path.
        run_shift("shift-rol-word-count-eq-width", "E378", "A710",
                  ["AAAA8001", "00000010"], 0, "AAAA8001", "A719")

        # H: ROR.W D1,D0, register count == 2*width(32).
        run_shift("shift-ror-word-count-eq-2width", "E278", "A710",
                  ["AAAA8001", "00000020"], 0, "AAAA8001", "A719")

        # I: ROL.L D1,D0, register count == width(32): must not execute a
        # native shift by 32.
        run_shift("shift-rol-long-count-eq-width", "E3B8", "A700",
                  ["80000001", "00000020"], 0, "80000001", "A709")

        # J: ROR.L D1,D0, register count == 33 (beyond width, nonmultiple).
        run_shift("shift-ror-long-count-eq-33", "E2B8", "A710",
                  ["80000001", "00000021"], 0, "C0000000", "A719")

        # K: source/destination alias, ROL.B D0,D0 -- the count must come
        # from the ORIGINAL D0 (materialized before any destination write),
        # never re-read after the sized write has already contaminated the
        # low bits the count is drawn from. D0=0x41: original low-six-bits
        # count is 1; the post-write byte (0x82) would give a DIFFERENT
        # (wrong) count of 2 if the count were incorrectly re-read.
        run_shift("shift-rol-byte-alias-d0-d0", "E138", "A700", ["00000041"], 0, "00000082", "A708")

        # L: BYTE/WORD partial-register preservation -- ROL.W #1,D0 with a
        # poisoned upper word that must survive the sized (WORD) write
        # unchanged; BYTE partial-preservation is already exercised by A-D
        # above (their poisoned "AAAAAA" upper 24 bits survive every BYTE
        # write untouched).
        run_shift("shift-rol-word-partial-preserve", "E358", "A710", ["FFFF8001"], 0, "FFFF0003", "A711")

        # ---------------------------------------------------------------
        # C6d: ROXL / ROXR register shift forms
        # ---------------------------------------------------------------

        shift_d_legal_codes = (
            "E310",  # ROXL.B #1,D0
            "E210",  # ROXR.B #1,D0
            "E370",  # ROXL.W D1,D0
            "E2B0",  # ROXR.L D1,D0
        )
        for code in shift_d_legal_codes:
            image = td / f"shift-d-legal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            first = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            second = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )

        # A: ROXL.B #1,D0, X=0 initial
        run_shift("shift-roxl-byte-imm1", "E310", "A700", ["AAAAAA81"], 0, "AAAAAA02", "A711", "00000102")
        # B: ROXR.B #1,D0, X=0 initial
        run_shift("shift-roxr-byte-imm1", "E210", "A700", ["AAAAAA81"], 0, "AAAAAA40", "A711", "00000102")
        # C: ROXL.B #8,D0 -- must NOT equal ordinary ROL.B #8 full-cycle identity
        # (contract: "immediate BYTE #8 regression").
        run_shift("shift-roxl-byte-imm8-not-identity", "E110", "A700", ["AAAAAA81"], 0, "AAAAAA40", "A711", "00000102")
        # D: ROXR.B #8,D0
        run_shift("shift-roxr-byte-imm8-not-identity", "E010", "A700", ["AAAAAA81"], 0, "AAAAAA02", "A711", "00000102")
        # E: ROXL.W D1,D0, register count TRUE zero, X=0 -> C=X=0 (contract:
        # "true-zero C=X vectors").
        run_shift("shift-roxl-word-count-zero-x-clear", "E370", "A700", ["AAAA8001", "00000000"], 0, "AAAA8001", "A708", "00000102")
        # F: ROXL.W D1,D0, register count TRUE zero, X=1 -> C=X=1
        run_shift("shift-roxl-word-count-zero-x-set", "E370", "A710", ["AAAA8001", "00000000"], 0, "AAAA8001", "A719", "00000102")
        # G: ROXR.W D1,D0, register count TRUE zero, X=0 -> C=X=0
        run_shift("shift-roxr-word-count-zero-x-clear", "E270", "A700", ["AAAA8001", "00000000"], 0, "AAAA8001", "A708", "00000102")
        # H: ROXR.W D1,D0, register count TRUE zero, X=1 -> C=X=1
        run_shift("shift-roxr-word-count-zero-x-set", "E270", "A710", ["AAAA8001", "00000000"], 0, "AAAA8001", "A719", "00000102")
        # I: ROXL.B D1,D0, register count == ring width (9): unchanged data,
        # C == existing X (contract: "ring-multiple vectors").
        run_shift("shift-roxl-byte-count-eq-ring", "E330", "A700", ["AAAAAA81", "00000009"], 0, "AAAAAA81", "A708", "00000102")
        # J: ROXR.B D1,D0, register count == 2*ring width (18), X=1
        run_shift("shift-roxr-byte-count-eq-2ring", "E230", "A710", ["AAAAAA81", "00000012"], 0, "AAAAAA81", "A719", "00000102")
        # K: ROXL.W D1,D0, register count == ring width (17)
        run_shift("shift-roxl-word-count-eq-ring", "E370", "A700", ["AAAA8001", "00000011"], 0, "AAAA8001", "A708", "00000102")
        # L: ROXR.W D1,D0, register count == 2*ring width (34), X=1
        run_shift("shift-roxr-word-count-eq-2ring", "E270", "A710", ["AAAA8001", "00000022"], 0, "AAAA8001", "A719", "00000102")
        # M: ROXL.L D1,D0, register count 32 (LONG, one short of the 33-bit
        # ring -- contract: "LONG 33-bit regressions").
        run_shift("shift-roxl-long-count-32", "E3B0", "A700", ["80000001", "00000020"], 0, "40000000", "A711", "00000102")
        # N: ROXR.L D1,D0, register count == ring width (33), X=1
        run_shift("shift-roxr-long-count-eq-ring", "E2B0", "A710", ["80000001", "00000021"], 0, "80000001", "A719", "00000102")
        # O: ROXL.L D1,D0, register count 34 (beyond the 33-bit ring, nonmultiple)
        run_shift("shift-roxl-long-count-34", "E3B0", "A700", ["80000001", "00000022"], 0, "00000002", "A711", "00000102")
        # P: ROXR.L D1,D0, register count 63 (deepest)
        run_shift("shift-roxr-long-count-63", "E2B0", "A700", ["80000001", "0000003F"], 0, "0000000A", "A700", "00000102")
        # Q: source/destination alias, ROXL.B D0,D0 (contract: "source/
        # destination alias") -- D0=0x41's original low-six-bits count is 1.
        run_shift("shift-roxl-byte-alias-d0-d0", "E130", "A700", ["00000041"], 0, "00000082", "A708", "00000102")
        # R: source/destination alias, ROXR.B D0,D0, X=1
        run_shift("shift-roxr-byte-alias-d0-d0", "E030", "A710", ["00000041"], 0, "000000A0", "A719", "00000102")

        # ---------------------------------------------------------------
        # C7a: memory-WORD ASL/ASR/LSL/LSR RMW forms
        # ---------------------------------------------------------------

        def run_shift_memory(name, code_hex, sr, a, ram, expected_ram, expected_sr,
                              expected_a_index=None, expected_a=None, expected_pc="00000102"):
            image = td / f"{name}.bin"
            image.write_bytes(bytes.fromhex(code_hex))
            args = [harness, str(image), *movem_args(sr=sr, a=a, ram=ram)]
            generated = subprocess.run(args, text=True, capture_output=True)
            assert generated.returncode == 0, generated.stderr
            c = td / f"{name}.c"
            exe = td / name
            c.write_text(generated.stdout)
            assert subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(c), "-o", str(exe)],
                text=True, capture_output=True,
            ).returncode == 0
            got = json.loads(subprocess.run([str(exe)], text=True, capture_output=True).stdout)
            assert got["ram"] == expected_ram and got["sr"] == expected_sr and got["pc"] == expected_pc, (name, got)
            # Every D register is untouched by any memory-form shift/rotate
            # (contract: "for every RMW assert ... D registers untouched").
            assert got["d"] == ["00000000"] * 8, (name, got)
            # The touched A register (if any) matches its expected side
            # effect; every OTHER A register remains exactly as seeded
            # (contract: "unrelated A registers untouched").
            seeded_a = (list(a) if a else []) + ["00000000"] * 8
            for index in range(8):
                if expected_a_index is not None and index == expected_a_index:
                    assert got["a"][index] == expected_a, (name, got)
                else:
                    assert got["a"][index] == seeded_a[index], (name, got)
            return got

        # A: ASR.W (A0), negative source, sentinel at +2 proves unrelated RAM
        # untouched.
        run_shift_memory("shift-mem-asr-word-a0", "E0D0", "A700", ["00FF0200"],
                          [("00FF0200", "80019ABC")], ["C0009ABC"], "A719")

        # B: ASL.W (A0), overflow case (0x4001<<1 sign-changes -> V=1).
        run_shift_memory("shift-mem-asl-word-a0", "E1D0", "A700", ["00FF0210"],
                          [("00FF0210", "40011234")], ["80021234"], "A70A")

        # C: LSR.W (A0), bit0=1 -> zero result.
        run_shift_memory("shift-mem-lsr-word-a0", "E2D0", "A700", ["00FF0220"],
                          [("00FF0220", "00015678")], ["00005678"], "A715")

        # D: LSL.W (A0), bit15=1.
        run_shift_memory("shift-mem-lsl-word-a0", "E3D0", "A700", ["00FF0230"],
                          [("00FF0230", "80019999")], ["00029999"], "A711")

        # E: ASR.W (A1)+ -- postincrement, ordinary (non-strong) case.
        run_shift_memory("shift-mem-asr-word-a1-postinc", "E0D9", "A700",
                          ["00000000", "00FF0240"], [("00FF0240", "00509ABC")], ["00289ABC"], "A700",
                          expected_a_index=1, expected_a="00FF0242")

        # F: ASL.W -(A1) -- predecrement, ordinary case.
        run_shift_memory("shift-mem-asl-word-a1-predec", "E1E1", "A700",
                          ["00000000", "00FF0252"], [("00FF0250", "00109ABC")], ["00209ABC"], "A700",
                          expected_a_index=1, expected_a="00FF0250")

        # G: LSR.W 4(A2) -- d16(An), no An side effect. 4-byte instruction
        # (one displacement extension word) -> PC advances by 4.
        run_shift_memory("shift-mem-lsr-word-a2-disp16", "E2EA0004", "A700",
                          ["00000000", "00000000", "00FF0260"], [("00FF0264", "80001111")], ["40001111"], "A700",
                          expected_a_index=2, expected_a="00FF0260", expected_pc="00000104")

        # H: LSL.W (xxx).W -- absolute.w (canonicalized to 0x00FFFF10). 4-byte
        # instruction (one absolute-word extension word) -> PC advances by 4.
        run_shift_memory("shift-mem-lsl-word-absw", "E3F8FF10", "A700", None,
                          [("00FFFF10", "80012222")], ["00022222"], "A711", expected_pc="00000104")

        # I: ASR.W (xxx).L -- absolute.l, negative source. 6-byte instruction
        # (two absolute-long extension words) -> PC advances by 6.
        run_shift_memory("shift-mem-asr-word-absl", "E0F900FF0400", "A700", None,
                          [("00FF0400", "C0013333")], ["E0003333"], "A719", expected_pc="00000106")

        # Strong postincrement regression (contract: "strong postinc
        # regression"): distinct words at base and base+2 catch a
        # read-base/write-base+2, double-increment, increment-before-read,
        # or separate-read/write-EA implementation.
        run_shift_memory("shift-mem-lsl-word-postinc-strong", "E3D8", "A700", ["00FF0500"],
                          [("00FF0500", "80011234")], ["00021234"], "A711",
                          expected_a_index=0, expected_a="00FF0502")

        # Strong predecrement regression (contract: "strong predec
        # regression"): A0 seeded at base+2; both read and write must land
        # at base, sentinel at base+2 must remain untouched.
        run_shift_memory("shift-mem-lsr-word-predec-strong", "E2E0", "A700", ["00FF0512"],
                          [("00FF0510", "00019ABC")], ["00009ABC"], "A715",
                          expected_a_index=0, expected_a="00FF0510")

        shift_e_legal_codes = (
            "E0D0",      # ASR.W (A0)
            "E3D8",      # LSL.W (A0)+
            "E2EA0004",  # LSR.W 4(A2)
            "E3F8FF10",  # LSL.W (xxx).W
        )
        for code in shift_e_legal_codes:
            image = td / f"shift-e-legal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            first = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            second = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )

        # ---------------------------------------------------------------
        # C7b: memory-WORD ROL/ROR/ROXL/ROXR RMW forms
        # ---------------------------------------------------------------

        # A: ROR.W (A0), C=0 result, X initially SET -- must be PRESERVED
        # (contract: "ROL/ROR X-preservation strong test").
        run_shift_memory("shift-mem-ror-word-a0-c0-x-set", "E6D0", "A710", ["00FF0300"],
                          [("00FF0300", "00509ABC")], ["00289ABC"], "A710")

        # B: ROR.W (A0), C=1 result, X initially CLEAR -- opposite polarity.
        run_shift_memory("shift-mem-ror-word-a0-c1-x-clear", "E6D0", "A700", ["00FF0310"],
                          [("00FF0310", "00519ABC")], ["80289ABC"], "A709")

        # C: ROL.W (A0), C=0 result, X initially SET.
        run_shift_memory("shift-mem-rol-word-a0-c0-x-set", "E7D0", "A710", ["00FF0320"],
                          [("00FF0320", "00509ABC")], ["00A09ABC"], "A710")

        # D: ROL.W (A0), C=1 result, X initially CLEAR -- opposite polarity.
        run_shift_memory("shift-mem-rol-word-a0-c1-x-clear", "E7D0", "A700", ["00FF0330"],
                          [("00FF0330", "80509ABC")], ["00A19ABC"], "A701")

        # E/F: ROXL.W (A0), SAME source word, X=0 vs X=1 -- must produce
        # DIFFERENT memory results (contract: "ROX X-as-data strong test" --
        # stronger than merely checking C/X, proves X genuinely participates
        # in the rotation, not merely copied afterward).
        run_shift_memory("shift-mem-roxl-word-a0-x-clear", "E5D0", "A700", ["00FF0340"],
                          [("00FF0340", "80019ABC")], ["00029ABC"], "A711")
        run_shift_memory("shift-mem-roxl-word-a0-x-set", "E5D0", "A710", ["00FF0340"],
                          [("00FF0340", "80019ABC")], ["00039ABC"], "A711")

        # G/H: ROXR.W (A0), same X-as-data strong pair, opposite direction.
        run_shift_memory("shift-mem-roxr-word-a0-x-clear", "E4D0", "A700", ["00FF0350"],
                          [("00FF0350", "80019ABC")], ["40009ABC"], "A711")
        run_shift_memory("shift-mem-roxr-word-a0-x-set", "E4D0", "A710", ["00FF0350"],
                          [("00FF0350", "80019ABC")], ["C0009ABC"], "A719")

        # I: ROR.W (A1)+ -- postincrement coverage for the RO family
        # (contract: "reuse C7a RMW shape unchanged" -- proves C7a's
        # sequencing was not accidentally family-specific).
        run_shift_memory("shift-mem-ror-word-a1-postinc", "E6D9", "A700",
                          ["00000000", "00FF0360"], [("00FF0360", "00519ABC")], ["80289ABC"], "A709",
                          expected_a_index=1, expected_a="00FF0362")

        # J: ROXL.W -(A1) -- predecrement coverage for the ROX family.
        run_shift_memory("shift-mem-roxl-word-a1-predec", "E5E1", "A710",
                          ["00000000", "00FF0372"], [("00FF0370", "80019ABC")], ["00039ABC"], "A711",
                          expected_a_index=1, expected_a="00FF0370")

        # K: ROL.W 4(A2) -- d16(An) coverage (contract: "all six EA forms
        # across final C7" -- rotate families must not rely exclusively on
        # (An)).
        run_shift_memory("shift-mem-rol-word-a2-disp16", "E7EA0004", "A710",
                          ["00000000", "00000000", "00FF0380"], [("00FF0384", "00509ABC")], ["00A09ABC"], "A710",
                          expected_a_index=2, expected_a="00FF0380", expected_pc="00000104")

        # L: ROXR.W (xxx).W -- absolute.w coverage.
        run_shift_memory("shift-mem-roxr-word-absw", "E4F8FF30", "A700", None,
                          [("00FFFF30", "80019ABC")], ["40009ABC"], "A711", expected_pc="00000104")

        shift_f_legal_codes = (
            "E6D0",      # ROR.W (A0)
            "E7D0",      # ROL.W (A0)
            "E5E1",      # ROXL.W -(A1)
            "E4F8FF30",  # ROXR.W (xxx).W
        )
        for code in shift_f_legal_codes:
            image = td / f"shift-f-legal-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            first = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            second = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (
                code, first.stderr, second.stderr,
            )

        # Adversarial: 68020+ bitfield family values (4-7 shape, verified
        # against representative words above the 8-family memory matrix)
        # remain permanently unsupported; illegal EA modes (Dn/An-direct/
        # PC-relative/immediate/indexed) remain unsupported for a memory
        # shift/rotate; every C6 register form remains unselected by the
        # memory decoder. Every one of the 8 base-MC68000 memory-WORD
        # families is now selected -- nothing remains "not yet selected"
        # through this primary opcode family.
        shift_rejected_codes = (
            "E8D0",  # family 4 (68020+ bitfield, e.g. BFTST shape)
            "EAD0",  # family 5 (68020+ bitfield)
            "ECD0",  # family 6 (68020+ bitfield)
            "EED0",  # family 7 (68020+ bitfield)
            "E7C0",  # ROL Dn direct -- illegal memory EA
            "E7C8",  # ROL An direct -- illegal memory EA
            "E7FC",  # ROL immediate -- illegal memory EA
            "E7F0",  # ROL d8(An,Xn) indexed -- illegal memory EA
        )
        for code in shift_rejected_codes:
            image = td / f"shift-rejected-{code}.bin"
            image.write_bytes(bytes.fromhex(code))
            rejected = subprocess.run([harness, str(image), *base_args], text=True, capture_output=True)
            assert rejected.returncode == 1 and rejected.stdout == "", (code, rejected)

    print("validated synthetic Batch C C1 (SWAP/EXT.W/EXT.L) + C2 (PEA/LINK/UNLK) + "
          "C3 (BTST/BCHG/BCLR/BSET) + C4a (BRA/Bcc) + C4b (BSR) + C4c (DBcc) + "
          "C5a (MOVEM ordinary forms) + C5b (MOVEM predecrement forms) + "
          "C5c1 (MOVEM postincrement forms) + C6a (LSL/LSR register forms) + "
          "C6b (ASL/ASR register forms) + C6c (ROL/ROR register forms) + "
          "C6d (ROXL/ROXR register forms, completing all 8 base-MC68000 register-form "
          "shift/rotate families) + C7a (ASL/ASR/LSL/LSR memory RMW forms) + "
          "C7b (ROL/ROR/ROXL/ROXR memory RMW forms, completing all 8 base-MC68000 "
          "memory-WORD shift/rotate families) generated-C vectors and adversarial rejections")


if __name__ == "__main__":
    main()
