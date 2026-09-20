"""SEG-007-T007 stage 1: bounded, inspection-only MC68000 semantic classifier.

Purpose and scope boundary
---------------------------
This module's sole contract is "private 16-bit MC68000 primary word (and, where the caller
supplies one, a private 32-bit extension word) in -> a sanitized public ISA classification
5-key dict out": ``{"family": str, "size": "byte"|"word"|"long"|None, "addressingMode": str|None,
"sourceAddressingMode": str|None, "destinationAddressingMode": str|None}``. Single-operand forms
(``LEA``, ``JSR``, ``JMP``, ``TST``, ``TAS``) and every operand-free/single-simple-operand form
(``MOVEQ``, ``RTS``, ``RTE``, ``NOP``, ``ILLEGAL``, ``BRA``/``BSR``/``Bcc``) populate only
``addressingMode`` (or leave it ``None`` where there is no effective-address field) and leave
``sourceAddressingMode``/``destinationAddressingMode`` ``None``. ``MOVE``/``MOVEA`` are the only
recognized families that populate ``sourceAddressingMode``/``destinationAddressingMode`` instead
(see the ``MOVE``/``MOVEA`` bullet below) and leave ``addressingMode`` ``None``.

It exists strictly to *name* an observed instruction form using public Motorola MC68000
encoding rules (see "Encoding source" below). It never determines whether segarecomp actually
*supports* that form -- that is decided only by the real shared
``segarecomp::decode_m68k_instruction`` pipeline (stage 2a), driven from the CLI via
``segarecomp probe-genesis-startup-decode``. This module is tooling-only: it is never imported
by, or exercised as part of, any production/runtime or generated-code build target (enforced by
``tests/stage1_classifier_tooling_only_test.py``, which greps ``src/`` and ``include/`` for any
reference to this package).

Precisely recognized families (public MC68000 encoding, MSB-first primary word bits 15..0)
--------------------------------------------------------------------------------------------
- ``RTS``  -- exact word ``0100 1110 0111 0101`` (0x4E75).
- ``RTE``  -- exact word ``0100 1110 0111 0011`` (0x4E73).
- ``NOP``  -- exact word ``0100 1110 0111 0001`` (0x4E71).
- ``JSR``  -- ``0100 1110 10 mmm rrr`` (mask 0xFFC0 == 0x4E80); addressing mode from the
  effective-address field, bits 5..0 (3-bit mode, bits 5..3; 3-bit register, bits 2..0).
- ``JMP``  -- ``0100 1110 11 mmm rrr`` (mask 0xFFC0 == 0x4EC0); addressing mode as above.
- ``LEA``  -- ``0100 aaa 111 mmm rrr`` (mask 0xF1C0 == 0x41C0); size is always "long";
  addressing mode from the source effective-address field, bits 5..0.
- ``TST``  -- ``0100 1010 ss mmm rrr`` (mask 0xFF00 == 0x4A00); size from bits 7..6
  (00=byte, 01=word, 10=long); addressing mode from bits 5..0. Size field 11 is not TST -- see
  ``ILLEGAL`` and ``TAS`` below, which take the size-field-11 sub-range of this same mask.
- ``ILLEGAL`` -- exact word ``0100 1010 1111 1100`` (0x4AFC); the well-known reserved
  "illegal instruction" encoding (also special-cased identically in
  ``src/m68k_pipeline.cpp``).
- ``TAS``    -- ``0100 1010 11 mmm rrr`` (mask 0xFFC0 == 0x4AC0); size is always "byte";
  addressing mode from the effective-address field, bits 5..0.
- ``MOVEQ``-- ``0111 ddd 0 iiiiiiii`` (mask 0xF100 == 0x7000); size "long", addressing mode
  "immediate" (the 8-bit sign-extended immediate field, bits 7..0).
- ``MOVE`` / ``MOVEA`` -- general MOVE encoding ``00 SS ddd DDD mmm rrr``: bits 15..14 == 00,
  bits 13..12 select size (01=byte, 11=word, 10=long; 00 is not a MOVE size and falls through to
  the ``semantic_classifier_gap`` marker), bits 11..9 destination register, bits 8..6 destination
  mode, bits 5..0 the *source* effective-address field (3-bit mode, bits 5..3; 3-bit register,
  bits 2..0). ``sourceAddressingMode`` is derived from the source field (bits 5..0, register-field
  bits 2..0 + mode-field bits 5..3) and ``destinationAddressingMode`` is *independently* derived
  from the full destination EA (the opposite field order from the source EA: register-field bits
  11..9 + mode-field bits 8..6, i.e. ``mode = (word >> 6) & 0x7``, ``register = (word >> 9) & 0x7``
  passed to the same ``_ea_mode_name`` helper). ``addressingMode`` is left ``None`` for
  ``MOVE``/``MOVEA``. When the destination mode field (bits 8..6) equals 1
  (address-register-direct), the family is ``MOVEA``; otherwise ``MOVE``. Two MOVE forms sharing
  the same source EA but differing destination EA (for example ``MOVE.L D0,(xxx).L`` vs.
  ``MOVE.L D0,D1``) classify to *different* normalized identities, since
  ``destinationAddressingMode`` differs between them even though every other field matches.
- ``Bcc`` / ``BRA`` / ``BSR`` (word-displacement form only) -- ``0110 cccc dddddddd``
  (top nibble 0110); recognized only when the low byte is exactly 0x00 (the classic word
  -displacement encoding: the low byte's zero value signals a following 16-bit displacement
  word rather than an embedded 8-bit displacement). Condition field bits 11..8: 0000 -> ``BRA``,
  0001 -> ``BSR``, anything else -> ``Bcc``. Size is reported "word"; addressing mode is ``None``
  (branches have no effective-address field).

Explicit gap marker (deliberate, fail-closed scope boundary)
--------------------------------------------------------------
Every other primary word -- including every MC68000 form this module does not attempt to name
precisely (e.g. ADD/SUB/AND/OR/CMP families, bit/shift instructions, ANDI/ORI/EORI-to-CCR/SR,
Scc, DBcc, byte/long-displacement Bcc, MOVEM, MOVEP, privileged/exception forms, and any other
form not listed above) -- classifies to the explicit marker
``{"family": "semantic_classifier_gap", "size": None, "addressingMode": None,
"sourceAddressingMode": None, "destinationAddressingMode": None}``. This is a deliberate,
fail-closed gap marker, not a coarse bucket: it never guesses a name the encoding does not
establish, and a ``semantic_classifier_gap`` entry is *not* backlog-actionable on its own -- it
signals that the classifier itself needs precise extension for that observed form before a
compatibility batch can be authored from it. (An earlier version of this module used a coarse
``line_<N>`` top-nibble bucket instead; that bucket falsely implied every unrecognized form in a
given "line" was one interchangeable capability, which is not true and is not actionable.)

Encoding source
----------------
All rules above are drawn from the publicly documented MC68000 instruction-word layout (the
well-known 4-bit "line" grouping of the top nibble, and the standard 6-bit effective-address
field: 3-bit mode + 3-bit register at bits 5..0, with mode 7 subdivided by register into
absolute-short (0), absolute-long (1), pc-relative-displacement (2), pc-relative-indexed (3),
and immediate (4)). No proprietary or copyrighted text is reproduced; only public bit-level
encoding facts are used.
"""
from typing import Dict, Optional

# Effective-address mode field (bits 5..3 of the EA byte) named per the public MC68000
# addressing-mode table; mode 7 is further subdivided by the register field (bits 2..0).
_EA_MODE_NAMES = {
    0: "data_register_direct",
    1: "address_register_direct",
    2: "address_register_indirect",
    3: "address_register_indirect_postincrement",
    4: "address_register_indirect_predecrement",
    5: "address_register_indirect_displacement",
    6: "address_register_indirect_indexed",
}
_EA_MODE_7_REGISTER_NAMES = {
    0: "absolute_short",
    1: "absolute_long",
    2: "pc_displacement",
    3: "pc_indexed",
    4: "immediate",
}

_MOVE_SIZE_BITS = {1: "byte", 3: "word", 2: "long"}
_TST_SIZE_BITS = {0: "byte", 1: "word", 2: "long"}


def _ea_mode_name(mode: int, register: int) -> Optional[str]:
    if mode == 7:
        return _EA_MODE_7_REGISTER_NAMES.get(register)
    return _EA_MODE_NAMES.get(mode)


def _triple(family: str, size: Optional[str] = None,
            addressing_mode: Optional[str] = None) -> Dict[str, Optional[str]]:
    return {
        "family": family,
        "size": size,
        "addressingMode": addressing_mode,
        "sourceAddressingMode": None,
        "destinationAddressingMode": None,
    }


def _move_entry(family: str, size: Optional[str], source_addressing_mode: Optional[str],
                 destination_addressing_mode: Optional[str]) -> Dict[str, Optional[str]]:
    return {
        "family": family,
        "size": size,
        "addressingMode": None,
        "sourceAddressingMode": source_addressing_mode,
        "destinationAddressingMode": destination_addressing_mode,
    }


def _gap() -> Dict[str, Optional[str]]:
    return {
        "family": "semantic_classifier_gap",
        "size": None,
        "addressingMode": None,
        "sourceAddressingMode": None,
        "destinationAddressingMode": None,
    }


def classify(primary: int, extension: Optional[int] = None) -> Dict[str, Optional[str]]:
    """Classify one MC68000 primary word (plus optional extension) to a public ISA 5-key dict.

    ``extension`` is accepted for contract symmetry with the caller's traced access log but is
    not currently required by any recognized family; it is deliberately unused rather than
    consulted for a family this module does not attempt to name from the extension word alone.
    """
    del extension  # Reserved for contract symmetry; see docstring.
    word = primary & 0xFFFF
    line = (word >> 12) & 0xF

    if word == 0x4E75:
        return _triple("RTS")
    if word == 0x4E73:
        return _triple("RTE")
    if word == 0x4E71:
        return _triple("NOP")
    if (word & 0xFFC0) == 0x4E80:
        return _triple("JSR", None, _ea_mode_name((word >> 3) & 0x7, word & 0x7))
    if (word & 0xFFC0) == 0x4EC0:
        return _triple("JMP", None, _ea_mode_name((word >> 3) & 0x7, word & 0x7))
    if (word & 0xF1C0) == 0x41C0:
        return _triple("LEA", "long", _ea_mode_name((word >> 3) & 0x7, word & 0x7))
    if word == 0x4AFC:
        return _triple("ILLEGAL")
    if (word & 0xFFC0) == 0x4AC0:
        return _triple("TAS", "byte", _ea_mode_name((word >> 3) & 0x7, word & 0x7))
    if (word & 0xFF00) == 0x4A00:
        size = _TST_SIZE_BITS.get((word >> 6) & 0x3)
        if size is not None:
            return _triple("TST", size, _ea_mode_name((word >> 3) & 0x7, word & 0x7))
    if (word & 0xF100) == 0x7000:
        return _triple("MOVEQ", "long", "immediate")
    if (word & 0xC000) == 0x0000:
        size = _MOVE_SIZE_BITS.get((word >> 12) & 0x3)
        if size is not None:
            destination_mode_field = (word >> 6) & 0x7
            destination_register_field = (word >> 9) & 0x7
            source_mode = (word >> 3) & 0x7
            source_register = word & 0x7
            family = "MOVEA" if destination_mode_field == 1 else "MOVE"
            return _move_entry(
                family, size,
                _ea_mode_name(source_mode, source_register),
                _ea_mode_name(destination_mode_field, destination_register_field),
            )
    if line == 0x6:
        condition = (word >> 8) & 0xF
        displacement = word & 0xFF
        if displacement == 0x00:
            if condition == 0x0:
                family = "BRA"
            elif condition == 0x1:
                family = "BSR"
            else:
                family = "Bcc"
            return _triple(family, "word", None)

    return _gap()
