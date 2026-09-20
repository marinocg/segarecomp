#!/usr/bin/env python3
"""SEG-007-T007: synthetic, unconditional tests for the stage-1 semantic classifier.

Fully synthetic; requires no commercial ROM and no local Musashi checkout.
"""
import pathlib
import sys
import unittest

PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT))

from tools.inventory import stage1_classifier  # noqa: E402


class Stage1ClassifierTest(unittest.TestCase):
    def test_moveq_classifies_precisely(self) -> None:
        self.assertEqual(
            stage1_classifier.classify(0x7000),
            {"family": "MOVEQ", "size": "long", "addressingMode": "immediate",
             "sourceAddressingMode": None, "destinationAddressingMode": None},
        )

    def test_lea_absolute_long_classifies_precisely(self) -> None:
        # LEA (xxx).L,A0: 0100 000 111 111 001 == 0x41F9.
        self.assertEqual(
            stage1_classifier.classify(0x41F9),
            {"family": "LEA", "size": "long", "addressingMode": "absolute_long",
             "sourceAddressingMode": None, "destinationAddressingMode": None},
        )

    def test_jsr_absolute_long_classifies_precisely(self) -> None:
        # JSR (xxx).L: 0x4EB9, matching the shared decoder's selected form.
        self.assertEqual(
            stage1_classifier.classify(0x4EB9),
            {"family": "JSR", "size": None, "addressingMode": "absolute_long",
             "sourceAddressingMode": None, "destinationAddressingMode": None},
        )

    def test_move_l_d0_absolute_long_classifies_as_move(self) -> None:
        # MOVE.L D0,(xxx).L: 0x23C0. Destination mode field (bits 8-6) is
        # absolute-long (111), never address-register-direct, so family is MOVE. Source EA
        # (bits 5..0) is data-register-direct (D0); destination EA (bits 11..6) is absolute-long.
        self.assertEqual(
            stage1_classifier.classify(0x23C0),
            {"family": "MOVE", "size": "long", "addressingMode": None,
             "sourceAddressingMode": "data_register_direct",
             "destinationAddressingMode": "absolute_long"},
        )

    def test_move_destination_addressing_mode_distinguishes_otherwise_identical_source(self) -> None:
        # Two MOVE.L forms sharing the exact same source EA (D0, data_register_direct) but
        # differing destination EA must classify to two distinct normalized identities.
        # MOVE.L D0,(xxx).L: 0x23C0 (destination absolute-long, from the earlier test).
        store_to_absolute_long = stage1_classifier.classify(0x23C0)
        # MOVE.L D0,D1: 0010 001 000 000 000 == 0x2200 (destination data_register_direct, D1).
        store_to_d1 = stage1_classifier.classify(0x2200)
        self.assertEqual(store_to_d1["family"], "MOVE")
        self.assertEqual(store_to_d1["sourceAddressingMode"], "data_register_direct")
        self.assertEqual(store_to_d1["destinationAddressingMode"], "data_register_direct")
        self.assertNotEqual(store_to_absolute_long, store_to_d1)
        self.assertEqual(
            store_to_absolute_long["sourceAddressingMode"], store_to_d1["sourceAddressingMode"]
        )
        self.assertNotEqual(
            store_to_absolute_long["destinationAddressingMode"], store_to_d1["destinationAddressingMode"]
        )

    def test_rts_classifies_precisely(self) -> None:
        self.assertEqual(
            stage1_classifier.classify(0x4E75),
            {"family": "RTS", "size": None, "addressingMode": None,
             "sourceAddressingMode": None, "destinationAddressingMode": None},
        )

    def test_unrecognized_form_classifies_as_semantic_classifier_gap(self) -> None:
        # 0xD000 (ADD family, top nibble 0xD) is intentionally not precisely named.
        result = stage1_classifier.classify(0xD000)
        self.assertEqual(
            result,
            {"family": "semantic_classifier_gap", "size": None, "addressingMode": None,
             "sourceAddressingMode": None, "destinationAddressingMode": None},
        )

    def test_every_top_nibble_yields_a_non_empty_family(self) -> None:
        for line in range(16):
            word = line << 12
            result = stage1_classifier.classify(word)
            self.assertTrue(result["family"], (hex(word), result))

    def test_deterministic(self) -> None:
        for word in (0x7000, 0x41F9, 0x4EB9, 0x23C0, 0x4E75, 0xD000, 0x6000, 0x6100):
            first = stage1_classifier.classify(word)
            second = stage1_classifier.classify(word)
            self.assertEqual(first, second, hex(word))

    def test_bra_bsr_bcc_word_displacement(self) -> None:
        self.assertEqual(stage1_classifier.classify(0x6000)["family"], "BRA")
        self.assertEqual(stage1_classifier.classify(0x6100)["family"], "BSR")
        self.assertEqual(stage1_classifier.classify(0x6600)["family"], "Bcc")

    def test_illegal_word_classifies_precisely(self) -> None:
        # 0x4AFC is the well-known reserved "illegal instruction" encoding, not TST, despite
        # matching the TST mask (0xFF00 == 0x4A00) with size field 11.
        self.assertEqual(
            stage1_classifier.classify(0x4AFC),
            {"family": "ILLEGAL", "size": None, "addressingMode": None,
             "sourceAddressingMode": None, "destinationAddressingMode": None},
        )

    def test_tas_classifies_precisely(self) -> None:
        # TAS D0: 0100 1010 11 000 000 == 0x4AC0.
        self.assertEqual(
            stage1_classifier.classify(0x4AC0),
            {"family": "TAS", "size": "byte", "addressingMode": "data_register_direct",
             "sourceAddressingMode": None, "destinationAddressingMode": None},
        )

    def test_tst_byte_still_classifies_precisely(self) -> None:
        # TST.B D0: size field 00, real TST word not affected by the ILLEGAL/TAS carve-out.
        self.assertEqual(
            stage1_classifier.classify(0x4A00),
            {"family": "TST", "size": "byte", "addressingMode": "data_register_direct",
             "sourceAddressingMode": None, "destinationAddressingMode": None},
        )

    def test_no_word_classifies_as_tst_with_unknown_size(self) -> None:
        # Bounded, synthetic sweep of the entire TST-mask range: every word that classifies as
        # TST must carry a real size (byte/word/long); size field 11 (0x4AC0-0x4AFF) must never
        # produce family == "TST" with size is None.
        for word in range(0x4A00, 0x4B00):
            result = stage1_classifier.classify(word)
            if result["family"] == "TST":
                self.assertIn(result["size"], ("byte", "word", "long"), (hex(word), result))


if __name__ == "__main__":
    unittest.main()
