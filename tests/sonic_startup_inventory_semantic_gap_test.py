#!/usr/bin/env python3
"""SEG-007-T007: synthetic, unconditional tests for the semantic_classifier_gap exit-code path.

Fully synthetic: never touches a real ROM or a real Musashi adapter. Exercises
``tool.decide_exit_code`` directly against synthetic normalized inventories (the post-scan
exit-code decision is factored out of ``main()`` specifically so it is unit-testable without
driving the whole --scan CLI path), and confirms two distinct synthetic MOVE forms sharing a
source EA but differing destination EA never collapse into one aggregated entry via the stage-1
classifier that feeds this aggregation.
"""
import pathlib
import sys

PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT))

from tools import sonic_startup_inventory as tool  # noqa: E402
from tools.inventory import stage1_classifier  # noqa: E402


def _normalized(instructions: list) -> dict:
    return {
        "instructions": instructions,
        "accesses": [],
        "stopReason": "max_executed_instructions",
        "bounds": {
            "maxExecutedInstructions": tool.MAX_EXECUTED_INSTRUCTIONS,
            "maxUniqueVisitedPcs": tool.MAX_UNIQUE_VISITED_PCS,
            "maxControlFlowDepth": tool.MAX_CONTROL_FLOW_DEPTH,
        },
    }


def _instruction_entry(family: str, support: str = "supported") -> dict:
    return {
        "family": family,
        "size": None,
        "addressingMode": None,
        "sourceAddressingMode": None,
        "destinationAddressingMode": None,
        "support": support,
        "firstObservedOrdinal": 0,
        "observationCount": 1,
    }


def main() -> None:
    # A scan with no semantic_classifier_gap entries returns EXIT_SUCCESS.
    clean = _normalized([_instruction_entry("MOVEQ"), _instruction_entry("RTS")])
    assert tool.decide_exit_code(clean) == tool.EXIT_SUCCESS, tool.decide_exit_code(clean)

    # A scan with at least one semantic_classifier_gap entry returns EXIT_SEMANTIC_GAP, even when
    # every other entry is fine (a mixed scan is still backlog-generation-unsafe as a whole).
    with_gap = _normalized([
        _instruction_entry("MOVEQ"),
        _instruction_entry("semantic_classifier_gap", support="unsupported"),
    ])
    assert tool.decide_exit_code(with_gap) == tool.EXIT_SEMANTIC_GAP, tool.decide_exit_code(with_gap)
    assert tool.EXIT_SEMANTIC_GAP == 30, tool.EXIT_SEMANTIC_GAP

    # A synthetic word from a genuinely unclassified family returns the exact
    # semantic_classifier_gap marker, all four detail fields None.
    gap_result = stage1_classifier.classify(0xD000)
    assert gap_result == {
        "family": "semantic_classifier_gap", "size": None, "addressingMode": None,
        "sourceAddressingMode": None, "destinationAddressingMode": None,
    }, gap_result

    # Two synthetic MOVE words sharing the same source EA but differing destination EA classify
    # to two distinct entries (never collapsed) -- exercised at the stage-1 classifier level,
    # which is exactly what sonic_startup_inventory.run_scan's aggregation key is built from.
    move_to_absolute_long = stage1_classifier.classify(0x23C0)  # MOVE.L D0,(xxx).L
    move_to_d1 = stage1_classifier.classify(0x2200)  # MOVE.L D0,D1
    assert move_to_absolute_long["sourceAddressingMode"] == move_to_d1["sourceAddressingMode"]
    assert (
        move_to_absolute_long["destinationAddressingMode"] != move_to_d1["destinationAddressingMode"]
    )
    key_absolute_long = (
        move_to_absolute_long["family"], move_to_absolute_long["size"],
        move_to_absolute_long["addressingMode"], move_to_absolute_long["sourceAddressingMode"],
        move_to_absolute_long["destinationAddressingMode"], "supported",
    )
    key_to_d1 = (
        move_to_d1["family"], move_to_d1["size"], move_to_d1["addressingMode"],
        move_to_d1["sourceAddressingMode"], move_to_d1["destinationAddressingMode"], "supported",
    )
    assert key_absolute_long != key_to_d1, (key_absolute_long, key_to_d1)

    print("sonic startup inventory semantic gap test: ok")


if __name__ == "__main__":
    main()
