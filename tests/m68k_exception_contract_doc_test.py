#!/usr/bin/env python3
"""SEG-021-T017 / ADR 0043: guard for the MC68000 Group 1/2 exception frame contract.

Checks two things:

1. ADR 0043's machine-readable ``m68k-group12-frame`` block describes exactly the MC68000 six-byte
   frame: saved SR word at SP+0, saved PC long at SP+2, total 6 bytes. It must contain no
   MC68010-style format/vector-offset word, and every other mention of a format or vector-offset word
   in the ADR must be a negation.
2. The generated-native exception-entry and RTE implementations still build and consume that same layout.
   SEG-021-T018 moved the primitive out of the Genesis platform into the M68K-owned C11 exception core
   (``RUNTIME_FRAME_SOURCES``); the Genesis runtime must delegate to it rather than build frames itself.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ADR = ROOT / "docs" / "decisions" / "0043-mc68000-exception-privilege-and-machine-hook-contract.md"
# SEG-021-T018 moved the primitive out of the Genesis platform into the M68K-owned C11 exception core; the
# runtime half of this guard now checks that core (and that the Genesis runtime delegates to it).
RUNTIME_FRAME_SOURCES = [ROOT / "libs" / "cpu" / "m68k" / "include" / "segarecomp" / "cpu" / "m68k" / "exception_core.h"]
GENESIS_BINDING = ROOT / "platforms" / "genesis" / "runtime" / "runtime.c"
ENTRY_FUNCTION = "segarecomp_m68k_exception_enter"
RETURN_FUNCTION = "segarecomp_m68k_exception_return"

EXPECTED_GROUP12 = [(0, 2, "saved_sr"), (2, 4, "saved_pc")]
EXPECTED_TOTAL = 6
FORBIDDEN_FIELD = re.compile(r"format|vector[-_ ]?offset", re.IGNORECASE)
FORMAT_TERM = re.compile(r"vector[-_ ]?offset|format[-_/ ]?(vector|word|nibble|code|dispatch)", re.IGNORECASE)
NEGATION = re.compile(r"\b(no|not|without|never|excludes?)\b", re.IGNORECASE)


class ContractError(AssertionError):
    pass


def fenced_block(text: str, info: str) -> list[str]:
    matches = list(re.finditer(r"^```" + re.escape(info) + r"\n(.*?)^```", text, re.MULTILINE | re.DOTALL))
    if not matches:
        raise ContractError(f"missing ```{info} block")
    if len(matches) > 1:
        raise ContractError(f"more than one ```{info} block")
    match = matches[0]
    return [line.strip() for line in match.group(1).splitlines() if line.strip()]


def parse_frame(lines: list[str]) -> tuple[list[tuple[int, int, str]], int]:
    rows: list[tuple[int, int, str]] = []
    total = None
    for line in lines:
        if FORBIDDEN_FIELD.search(line):
            raise ContractError(f"frame block names a 68010+ format/vector-offset field: {line!r}")
        m = re.fullmatch(r"offset=(\d+) size=(\d+) field=([a-z_]+)", line)
        if m:
            rows.append((int(m.group(1)), int(m.group(2)), m.group(3)))
            continue
        m = re.fullmatch(r"total=(\d+)", line)
        if m and total is None:
            total = int(m.group(1))
            continue
        raise ContractError(f"unrecognized frame line: {line!r}")
    if total is None:
        raise ContractError("frame block has no total")
    return rows, total


def check_group12_frame(text: str) -> None:
    rows, total = parse_frame(fenced_block(text, "m68k-group12-frame"))
    if rows != EXPECTED_GROUP12 or total != EXPECTED_TOTAL:
        raise ContractError(f"Group 1/2 frame is {rows} total={total}; expected {EXPECTED_GROUP12} total=6")
    # Contiguity: the rows tile [0, total) exactly.
    cursor = 0
    for offset, size, _ in rows:
        if offset != cursor:
            raise ContractError("Group 1/2 frame rows are not contiguous")
        cursor += size
    if cursor != total:
        raise ContractError("Group 1/2 frame rows do not sum to the total")


def check_format_word_mentions_are_negations(text: str) -> None:
    for number, line in enumerate(text.splitlines(), 1):
        for term in FORMAT_TERM.finditer(line):
            # The negation must precede the term within the same clause (no clause break in between).
            prefix = re.split(r"[.;:]\s", line[:term.start()])[-1]
            if not NEGATION.search(prefix):
                raise ContractError(f"line {number} mentions a format/vector-offset word without a preceding negation")


def function_body(source: str, name: str) -> str:
    m = re.search(r"^(?:static\s+)?(?:inline\s+)?\w+\s+" + re.escape(name) + r"\s*\(", source, re.MULTILINE)
    if m is None:
        raise ContractError(f"{name} not found")
    start = source.index("{", source.index(")", m.end()))
    depth = 0
    for i in range(start, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
    raise ContractError(f"{name} body is unterminated")


def _compact(text: str) -> str:
    return re.sub(r"\s+", " ", text)


def check_runtime_frame(source: str) -> None:
    if not re.search(r"#define SEGARECOMP_M68K_EXCEPTION_FRAME_BYTES UINT32_C\(6\)", source):
        raise ContractError("the exception core no longer defines a six-byte frame")
    entry = _compact(function_body(source, ENTRY_FUNCTION))
    sr_write = re.search(r"hooks->stack_write\(hooks->context, frame_base, 2U, \(uint32_t\)(\w+)\)", entry)
    if sr_write is None or sr_write.group(1) != "saved_sr":
        raise ContractError("the SR word slot at frame_base is not written with the saved SR")
    pc_write = re.search(r"hooks->stack_write\(hooks->context, frame_base \+ 2U, 4U, (\w+)\)", entry)
    if pc_write is None or pc_write.group(1) != "stacked_pc":
        raise ContractError("the PC long slot at frame_base+2 is not written with the stacked PC")
    for needle in ("frame_base = ssp - SEGARECOMP_M68K_EXCEPTION_FRAME_BYTES;",
                   "*cpu->active_sp = frame_base;"):
        if needle not in entry:
            raise ContractError(f"exception entry no longer contains {needle!r}")
    if re.search(r"frame_base \+ (?!2U)\d+U", entry):
        raise ContractError("exception entry writes beyond the six-byte SR/PC frame")
    # validate-then-commit: the extent validation and vector resolution precede the first frame write.
    if not (entry.index("validate_stack_extent") < entry.index("resolve_vector") < entry.index("stack_write")):
        raise ContractError("exception entry writes the frame before validating it")
    rte = _compact(function_body(source, RETURN_FUNCTION))
    for needle in ("hooks->stack_read(hooks->context, sp, 2U, &saved_sr)",
                   "hooks->stack_read(hooks->context, sp + 2U, 4U, &saved_pc)",
                   "sp + SEGARECOMP_M68K_EXCEPTION_FRAME_BYTES"):
        if needle not in rte:
            raise ContractError(f"RTE no longer contains {needle!r}")


def check_genesis_delegates(source: str) -> None:
    """The Genesis runtime binds the core; it no longer builds or pops a frame itself."""
    for needle in ("segarecomp_m68k_exception_enter(", "segarecomp_m68k_exception_return("):
        if needle not in source:
            raise ContractError(f"the Genesis runtime no longer delegates to {needle}")
    for body_name in ("genesis_construct_exception_frame_and_transfer", "genesis_exception_return"):
        body = _compact(function_body(source, body_name))
        if "GENESIS_BUS_STACK" in body or "frame_base" in body:
            raise ContractError(f"{body_name} constructs or reads a frame itself instead of binding the core")


class Adr0043FrameContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.text = ADR.read_text(encoding="utf-8")

    def test_group12_frame_is_six_byte_sr_then_pc(self) -> None:
        check_group12_frame(self.text)

    def test_no_68010_format_word_in_contract(self) -> None:
        check_format_word_mentions_are_negations(self.text)

    def test_group0_frame_is_documented_as_deferred_only(self) -> None:
        rows, total = parse_frame(fenced_block(self.text, "m68k-group0-frame-deferred"))
        self.assertEqual(total, 14)
        self.assertEqual(rows[-2:], [(8, 2, "saved_sr"), (10, 4, "saved_pc")])

    def test_runtime_matches_contract(self) -> None:
        for path in RUNTIME_FRAME_SOURCES:
            check_runtime_frame(path.read_text(encoding="utf-8"))
        check_genesis_delegates(GENESIS_BINDING.read_text(encoding="utf-8"))

    # Negative controls: each checker must reject a drifted contract.
    def test_negative_format_word_in_frame(self) -> None:
        bad = self.text.replace("total=6", "offset=6 size=2 field=format_vector_offset\ntotal=8", 1)
        with self.assertRaises(ContractError):
            check_group12_frame(bad)

    def test_negative_swapped_frame(self) -> None:
        bad = self.text.replace("offset=0 size=2 field=saved_sr\noffset=2 size=4 field=saved_pc",
                                "offset=0 size=4 field=saved_pc\noffset=4 size=2 field=saved_sr", 1)
        with self.assertRaises(ContractError):
            check_group12_frame(bad)

    def test_negative_unnegated_format_word(self) -> None:
        with self.assertRaises(ContractError):
            check_format_word_mentions_are_negations(self.text + "\nThe frame carries a format word.\n")

    def test_negative_tightened_drifts(self) -> None:
        for sentence in ("The frame carries a format word, not a nibble.",
                         "A vector offset word follows the PC.",
                         "The frame format code is stored at SP+6."):
            with self.assertRaises(ContractError, msg=sentence):
                check_format_word_mentions_are_negations(self.text + "\n" + sentence + "\n")
        duplicate = self.text + "\n```m68k-group12-frame\noffset=0 size=2 field=saved_sr\ntotal=2\n```\n"
        with self.assertRaises(ContractError):
            check_group12_frame(duplicate)
        source = RUNTIME_FRAME_SOURCES[0].read_text(encoding="utf-8")
        swapped = source.replace("frame_base, 2U, (uint32_t)saved_sr)", "frame_base, 2U, (uint32_t)stacked_pc)", 1)
        self.assertNotEqual(swapped, source)
        with self.assertRaises(ContractError):
            check_runtime_frame(swapped)
        binding = GENESIS_BINDING.read_text(encoding="utf-8")
        inlined = binding.replace("status = segarecomp_m68k_exception_return(&hooks, &cpu, restored_pc_out);",
                                  "status = SEGARECOMP_M68K_EXCEPTION_OK; (void)GENESIS_BUS_STACK_READ;", 1)
        self.assertNotEqual(inlined, binding)
        with self.assertRaises(ContractError):
            check_genesis_delegates(inlined)

    def test_negative_runtime_drift(self) -> None:
        source = RUNTIME_FRAME_SOURCES[0].read_text(encoding="utf-8")
        with self.assertRaises(ContractError):
            check_runtime_frame(source.replace("UINT32_C(6)", "UINT32_C(8)", 1))
        with self.assertRaises(ContractError):
            check_runtime_frame(source.replace("frame_base = ssp - SEGARECOMP_M68K_EXCEPTION_FRAME_BYTES;",
                                               "frame_base = ssp - UINT32_C(8);", 1))
        with self.assertRaises(ContractError):
            check_runtime_frame(source.replace("hooks->stack_read(hooks->context, sp + 2U, 4U, &saved_pc)",
                                               "hooks->stack_read(hooks->context, sp + 4U, 4U, &saved_pc)", 1))


if __name__ == "__main__":
    unittest.main()
