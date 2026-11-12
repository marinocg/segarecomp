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


def chip_events(events):
    """Everything except the 68000's stores into the driver's windows ('W') and the end-of-observation marker ('E')."""
    return [e for e in events if e[0] not in "WE"]


def observed_writes(events):
    """The 68000's own stores into the sound RAM windows the driver reads ('W <offset> <value> <ticks>'): what the game wrote, independent of the driver."""
    return [e for e in events if e[0] == "W"]


def observation_end(events):
    """The guest time at which the native observation ended ('E' line), or None for a log that has none."""
    ends = [e[3] for e in events if e[0] == "E"]
    return ends[-1] if ends else None


def normalize(events):
    """Native logs mark the accesses the replacement itself made with lower-case kinds ('y', 'p'); the 68000's own accesses to the chips are not part of the
    comparison. A log without lower-case kinds (an older program) is taken as it is."""
    if any(e[0] in "yp" for e in events):
        return [(e[0].upper(), e[1], e[2], e[3]) if e[0] in "yp" else e for e in events if e[0] in "ypCRSU"]
    return events


def lives(events):
    """One list per run of the driver. A /RESET restarts it: its boot is the four PSG silence writes (one instant) that directly follow a chip reset ('R');
    the same four bytes after a stop are the idle clipping, not a boot. The reference harness models one life, so each life is compared on its own."""
    events = normalize(chip_events(events))
    starts = []
    for index in range(1, len(events) - 3):
        window = events[index:index + 4]
        if events[index - 1][0] == "R" and [e[0] for e in window] == ["P"] * 4 and [e[2] for e in window] == [0x9F, 0xBF, 0xDF, 0xFF] and len({e[3] for e in window}) == 1:
            starts.append(index)
    return [events[start:(starts[i + 1] if i + 1 < len(starts) else len(events))] for i, start in enumerate(starts)]


def life_end(events, life, horizon):
    """The guest time at which a life ends: the /RESET that starts the next one (the last 'R' before its boot), or the common horizon for the last life."""
    chosen = lives(events)
    if life + 1 >= len(chosen):
        return horizon
    resets = [e[3] for e in chosen[life] if e[0] == "R"]
    return resets[-1] if resets else chosen[life + 1][0][3]


def life_events(events, life):
    chosen = lives(events)
    return chosen[life] if life < len(chosen) else []


def native_driver_events(events, life=0):
    """The native driver's own chip accesses in one life (resets are device effects, not driver operations)."""
    return [e for e in life_events(events, life) if e[0] in "YP"]


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


def hold_script(events):
    """Where the Z80 bus was taken from the running driver: 'S' (taken) up to the next 'U' (returned). The reference harness replays them as 'H <start> <end>'."""
    lines, start = [], None
    for kind, _, _, ticks in events:
        if kind == "S" and start is None:
            start = ticks
        elif kind == "U" and start is not None:
            lines.append("H %d %d" % (start, ticks))
            start = None
    return lines


def boot_ticks(events):
    native = [e for e in events if e[0] in "YP"]
    return native[0][3] if native else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("native")
    parser.add_argument("reference", nargs="?")
    parser.add_argument("--until", type=int, default=None, help="compare only operations before this guest master tick of the native log")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--life", type=int, default=0, help="which run of the driver to compare (0 = the first boot; a /RESET starts the next life)")
    parser.add_argument("--emit-script", metavar="FILE", help="also write the reference harness input (consumed commands) and print the boot time to stderr")
    args = parser.parse_args()
    if args.emit_script:
        raw = life_events(parse(args.native), args.life)
        with open(args.emit_script, "w") as out:
            out.write("\n".join(command_script(raw) + hold_script(raw)) + "\n")
        print("boot_ticks %s lives %d" % (boot_ticks(raw), len(lives(parse(args.native)))), file=sys.stderr)
        return 0
    native = native_driver_events(parse(args.native), args.life)
    reference = [e for e in parse(args.reference) if e[0] != "R"]
    until = args.until if args.until is not None else (reference[-1][3] if reference else 0)   # default: the time range the reference covers
    native = [e for e in native if e[3] <= until]
    native_end = native[-1][3] if native else 0
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
        # A native driver that stopped early (an explicit fault) while the reference went on playing is a coverage boundary ONLY IF the resource that caused the fault
        # really is outside the verified subset - inspect it; a premature stop is how a mapping defect shows (see the validation document, section 7).
        "reference_dac_data_writes_after_native_end": sum(1 for i, e in enumerate(reference) if e[3] > native_end and e[0] == "Y" and e[1] == 1 and i > 0 and reference[i - 1][:3] == ("Y", 0, 0x2A)),
        "dac_data_writes": sum(1 for i, e in enumerate(native) if e[0] == "Y" and e[1] == 1 and i > 0 and native[i - 1][:3] == ("Y", 0, 0x2A)),
    }
    if first_difference is not None:
        result["native_at_difference"] = native[first_difference]
        result["reference_at_difference"] = reference[first_difference]
    print(json.dumps(result, indent=None if args.json else 2))
    return 0 if identical else 1


if __name__ == "__main__":
    sys.exit(main())


# ---- independent command observation (docs/testing/genesis-native-gems-validation.md, section 4) --------------------------------------------------------------------------
# The reference is fed commands derived from what the 68000 itself wrote into the driver's FIFO ('W' lines), never from what the native driver says it consumed;
# the two are then compared. Argument counts are the reference implementation's (GEMSPlay's command switch); an id outside it is reported, never skipped silently.
KNOWN_ARGUMENTS = {0: 2, 1: 2, 2: 2, 3: 1, 4: 3, 5: 1, 6: 2, 7: 2, 11: 12, 12: 0, 13: 0, 14: 2, 16: 1, 18: 1, 20: 2, 22: 0, 23: 3, 26: 2, 27: 2, 28: 1, 29: 1, 30: 4, 31: 2, 32: 1}
FIFO_BASE, FIFO_SIZE, WRITE_POINTER, READ_POINTER = 0x1B40, 64, 0x36, 0x37
FRAME_TICKS = 896040
MAX_CONSUME_LAG = 2 * FRAME_TICKS   # the driver looks at its FIFO at every frame tick and whenever the Z80 gets its bus back


def committed_bytes(writes):
    """The byte stream the game committed to the driver: ring bytes between the previous pointer and each new write pointer, with the time of the commit. The 68000's write to the
    read pointer ($37) re-bases the unread data (a game initialising the driver)."""
    ring, base, stream, overruns = [0] * FIFO_SIZE, 0, [], 0
    for _, offset, value, ticks in writes:
        if FIFO_BASE <= offset < FIFO_BASE + FIFO_SIZE:
            ring[offset - FIFO_BASE] = value
        elif offset == READ_POINTER:
            base = value & (FIFO_SIZE - 1)
        elif offset == WRITE_POINTER:
            new = value & (FIFO_SIZE - 1)
            count = (new - base) % FIFO_SIZE
            stream.extend((ring[(base + k) % FIFO_SIZE], ticks) for k in range(count))
            base = new
    return stream, overruns


def derive_commands(writes):
    """Splits the committed stream into commands the way the driver does: a command starts with $FF; any other byte between commands is skipped. Returns
    (commands as (bytes, commit ticks of the last byte), stray byte count, unknown command ids)."""
    stream, _ = committed_bytes(writes)
    commands, stray, unknown, index = [], 0, [], 0
    while index < len(stream):
        if stream[index][0] != 0xFF:
            stray += 1
            index += 1
            continue
        if index + 1 >= len(stream):
            break   # an incomplete command at the end of the stream
        command_id = stream[index + 1][0]
        arguments = KNOWN_ARGUMENTS.get(command_id)
        if arguments is None:
            unknown.append(command_id)
            commands.append((bytes([0xFF, command_id]), stream[index + 1][1]))
            index += 2
            continue
        if index + 2 + arguments > len(stream):
            break
        chunk = stream[index:index + 2 + arguments]
        commands.append((bytes(b for b, _ in chunk), chunk[-1][1]))
        index += 2 + arguments
    return commands, stray, unknown


def consumed_commands(events):
    """The commands the native driver says it consumed: ('C' lines, one per byte, command start at index 0) as (bytes, ticks)."""
    commands, current, when = [], [], None
    for kind, index, value, ticks in chip_events(events):
        if kind != "C":
            continue
        if index == 0 and current:
            commands.append((bytes(current), when))
            current = []
        current.append(value)
        when = ticks
    if current:
        commands.append((bytes(current), when))
    return commands


def hold_windows(events):
    windows, start = [], None
    for kind, _, _, ticks in chip_events(events):
        if kind == "S" and start is None:
            start = ticks
        elif kind == "U" and start is not None:
            windows.append((start, ticks))
            start = None
    return windows


def held_ticks(windows, begin, end):
    return sum(max(0, min(end, b) - max(begin, a)) for a, b in windows)


def check_commands(events, horizon, max_lag=MAX_CONSUME_LAG, allow_unknown=False):
    """Compares the commands the game wrote with the commands the driver consumed. Returns (problems, summary). Every discrepancy is reported: a command the driver
    never consumed, one it consumed that the game did not write, a different order or content, a consumption before the write, a consumption later than the bound
    (excluding the time the 68000 held the Z80 bus), an unknown command id, and bytes the driver would skip."""
    problems = []
    derived, stray, unknown = derive_commands(observed_writes(events))
    consumed = consumed_commands(events)
    holds = hold_windows(events)
    if unknown and not allow_unknown:
        problems.append("unknown command ids written by the game: %s" % sorted(set(unknown)))
    if stray:
        problems.append("%d byte(s) written between commands that the driver skips" % stray)
    matched, worst = 0, 0
    for index, ((want, written), (got, taken)) in enumerate(zip(derived, consumed)):
        if want != got:
            problems.append("command %d differs: the game wrote %s, the driver consumed %s" % (index, want.hex(), got.hex()))
            break
        matched += 1
        if taken < written:
            problems.append("command %d was consumed at %d, before the game finished writing it at %d" % (index, taken, written))
        lag = taken - written - held_ticks(holds, written, taken)
        worst = max(worst, lag)
        if lag > max_lag:
            problems.append("command %d was consumed %d ticks after it was written (bound %d, bus holds excluded)" % (index, lag, max_lag))
    pending = [c for c in derived[len(consumed):]] if matched == len(consumed) else []
    if len(consumed) > len(derived):
        problems.append("the driver consumed %d command(s) the game never wrote" % (len(consumed) - len(derived)))
    for want, written in pending:
        if written + max_lag <= horizon:
            problems.append("a command written at %d (%s) was never consumed by the horizon %d: the driver missed it" % (written, want.hex(), horizon))
    summary = {"commands_written": len(derived), "commands_consumed": len(consumed), "matched": matched, "pending_at_horizon": sum(1 for _, w in pending if w + max_lag > horizon),
               "unknown_ids": sorted(set(unknown)), "stray_bytes": stray, "worst_consume_lag_ticks": worst}
    return problems, summary


def compare_to_horizon(native, reference, until):
    """Exact comparison of the driver's chip operations with the reference's up to a shared guest-time horizon: operation order, port and value must be identical, and
    neither side may have an extra operation or stop early inside the window. Returns (problems, result)."""
    native = [e for e in native if e[3] <= until]
    reference = [e for e in reference if e[3] <= until]
    problems = []
    compared = min(len(native), len(reference))
    first = next((i for i in range(compared) if native[i][:3] != reference[i][:3]), None)
    if first is not None:
        problems.append("operation %d differs: native %s, reference %s" % (first, native[first], reference[first]))
    elif len(native) < len(reference):
        problems.append("the native driver stopped early: %d operation(s) of the reference between its last (%s) and the horizon %d are missing" %
                        (len(reference) - len(native), native[-1][3] if native else "none", until))
    elif len(native) > len(reference):
        problems.append("the native driver made %d operation(s) the reference did not" % (len(native) - len(reference)))
    skews = [native[i][3] - reference[i][3] for i in range(compared if first is None else first)]
    return problems, {"native_operations": len(native), "reference_operations": len(reference), "first_difference_index": first,
                      "skew_ticks_min": min(skews) if skews else None, "skew_ticks_max": max(skews) if skews else None}
