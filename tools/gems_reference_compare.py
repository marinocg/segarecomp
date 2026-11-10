#!/usr/bin/env python3
"""Compares the chip-access log of the native GEMS replacement with the one an independent reference produced for the same commands.

Both logs use the line format "<Y|P|R> <port> <value hex> <guest master ticks>" (the native side: SEGARECOMP_AUDIO_WRITE_LOG of a built program, the
reference side: tools/gems_reference_harness.c). See docs/testing/genesis-native-gems-validation.md.

  gems_reference_compare.py <native log> <reference log> [--until TICKS] [--json]

The comparison is exact on the operation sequence (kind, port, value, in order) and reports the guest-time offset of every operation separately: the
native driver is scheduled on the real Z80 clock, the reference by a coarse harness, so time is a bounded-skew check, never an identity claim.
Exit status 0 only when the operation sequences are identical over the compared range (the one documented reference simplification excepted).
"""
import argparse
import json
import sys


def parse(path):
    events = []
    with open(path) as handle:
        for line in handle:
            parts = line.split()
            if len(parts) != 4:
                continue
            events.append((parts[0], int(parts[1]), int(parts[2], 16), int(parts[3])))
    return events


def native_driver_events(events):
    """The native driver's own accesses: from its boot (the four PSG silence writes at one instant; a Z80 stub image that silences the PSG does so at
    different instants) to the end. Chip resets are device effects, not driver operations. A /RESET restart of the driver is not modelled by the
    reference harness: compare a range that contains none (the build's `observed.activations` and the run summary say how many there were)."""
    starts = []
    for index in range(len(events) - 3):
        window = events[index:index + 4]
        if [e[0] for e in window] == ["P"] * 4 and [e[2] for e in window] == [0x9F, 0xBF, 0xDF, 0xFF] and len({e[3] for e in window}) == 1:
            starts.append(index)
    if not starts:
        return []
    return [e for e in events[starts[0]:] if e[0] in "YP"]


def command_script(events):
    """The commands the native driver consumed ('C' lines: one per command byte, same instant), as the reference harness's input script."""
    lines, current, when = [], [], None
    for kind, index, value, ticks in events:
        if kind != "C":
            continue
        if index == 0 and current:
            lines.append("%d %s" % (when, " ".join("%02x" % b for b in current)))
            current = []
        current.append(value)
        when = ticks
    if current:
        lines.append("%d %s" % (when, " ".join("%02x" % b for b in current)))
    return lines


def boot_ticks(events):
    native = [e for e in events if e[0] in "YP"]
    for index in range(len(native) - 3):
        window = native[index:index + 4]
        if [e[0] for e in window] == ["P"] * 4 and [e[2] for e in window] == [0x9F, 0xBF, 0xDF, 0xFF] and len({e[3] for e in window}) == 1:
            return window[0][3]
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("native")
    parser.add_argument("reference", nargs="?")
    parser.add_argument("--until", type=int, default=None, help="compare only operations before this guest master tick of the native log")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--emit-script", metavar="FILE", help="also write the reference harness input (consumed commands) and print the boot time to stderr")
    args = parser.parse_args()
    if args.emit_script:
        raw = parse(args.native)
        with open(args.emit_script, "w") as out:
            out.write("\n".join(command_script(raw)) + "\n")
        print("boot_ticks %s" % boot_ticks(raw), file=sys.stderr)
        return 0
    native = native_driver_events(parse(args.native))
    reference = [e for e in parse(args.reference) if e[0] != "R"]
    until = args.until if args.until is not None else (reference[-1][3] if reference else 0)   # default: the time range the reference covers
    native = [e for e in native if e[3] <= until]
    key = lambda e: e[:3]
    compared = min(len(native), len(reference))
    first_difference = None
    for index in range(compared):
        if key(native[index]) != key(reference[index]):
            first_difference = index
            break
    # the native log may legitimately be shorter (it faulted explicitly) or the reference longer; the comparison range is the native log
    identical = first_difference is None and len(native) <= len(reference)
    skews = [native[i][3] - reference[i][3] for i in range(compared if first_difference is None else first_difference)]
    dac = [e for e in native if e[0] == "Y" and e[1] == 1]
    result = {
        "native_operations": len(native),
        "reference_operations": len(reference),
        "compared_operations": compared,
        "sequence_identical": identical,
        "first_difference_index": first_difference,
        "skew_ticks_min": min(skews) if skews else None,
        "skew_ticks_max": max(skews) if skews else None,
        "dac_data_writes": sum(1 for i, e in enumerate(native) if e[0] == "Y" and e[1] == 1 and i > 0 and native[i - 1][:3] == ("Y", 0, 0x2A)),
    }
    if first_difference is not None:
        result["native_at_difference"] = native[first_difference]
        result["reference_at_difference"] = reference[first_difference]
    print(json.dumps(result, indent=None if args.json else 2))
    return 0 if identical else 1


if __name__ == "__main__":
    sys.exit(main())
