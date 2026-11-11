#!/usr/bin/env python3
"""Compares a native GEMS replacement run with the independent reference, life by life, to one shared guest-time horizon (ADR 0291, docs/testing/genesis-native-gems-validation.md).

  gems_reference_check.py --rom <game image> --native-log <SEGARECOMP_AUDIO_WRITE_LOG file> --harness <built tools/gems_reference_harness.c> [--work DIR]
                          [--horizon TICKS] [--allow-unknown-commands]

What is checked (every item is a failure, not a tolerance):
  * The commands. The reference is fed the commands the GAME wrote (derived from the 68000's own stores into the driver's FIFO, 'W' lines), not what the native driver says it
    consumed; the two are compared one by one: a command the driver never consumed, one it consumed that was not written, a different content or order, a consumption before the
    write or later than a bound (bus-hold time excluded), an unknown command id and bytes the driver would skip are all reported.
  * The horizon. Native and reference chip operations are compared up to ONE explicit guest time (the end of the observation, written by the program as an 'E' line, or --horizon):
    exact operation order, port and value, with no extra or missing operation on either side, so a native driver that stops early or plays on after the reference is a failure.
    A /RESET ends a life at the reset; the last life ends at the horizon.
  * Time is reported separately (the bounded skew per operation); it is not part of the identity.
It prints one JSON object per life and one for the commands, and exits 0 only when everything is clean.
"""
import argparse
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gems_reference_compare as compare  # noqa: E402


def reference_script(commands, windows):
    return ["%d %s" % (when, data.hex(" ")) for data, when in commands] + ["H %d %d" % window for window in windows]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom", required=True)
    parser.add_argument("--native-log", required=True)
    parser.add_argument("--harness", required=True)
    parser.add_argument("--work", default=None)
    parser.add_argument("--horizon", type=int, default=None, help="explicit guest-time horizon (default: the log's own end-of-observation marker)")
    parser.add_argument("--allow-unknown-commands", action="store_true", help="report, but do not fail on, command ids outside the reference's command set")
    parser.add_argument("--max-consume-lag", type=int, default=compare.MAX_CONSUME_LAG)
    args = parser.parse_args()
    events = compare.parse(args.native_log)
    horizon = args.horizon if args.horizon is not None else compare.observation_end(events)
    if horizon is None:
        print(json.dumps({"error": "the log has no end-of-observation marker; pass --horizon explicitly"}))
        return 2
    chips = compare.chip_events(events)
    count = len(compare.lives(events))
    work = pathlib.Path(args.work) if args.work else pathlib.Path(tempfile.mkdtemp(prefix="gems-reference-"))
    work.mkdir(parents=True, exist_ok=True)
    ok = count > 0
    if count == 0:
        print(json.dumps({"error": "no life of the native driver found in the log"}))

    problems, summary = compare.check_commands(events, horizon, args.max_consume_lag, args.allow_unknown_commands)
    print(json.dumps({"commands": summary, "problems": problems, "horizon": horizon}))
    ok = ok and not problems
    late = [e for e in chips if e[0] in "YP" and e[3] > horizon]
    if late:
        print(json.dumps({"error": "the native log has %d chip operation(s) after the horizon" % len(late)}))
        ok = False

    derived, _, _ = compare.derive_commands(compare.observed_writes(events))
    for life in range(count):
        raw = compare.life_events(events, life)
        until = compare.life_end(events, life, horizon)
        # the commands the game wrote, timed at the moment the native driver took them (the reference models no Z80 polling): the life's share of the global order
        consumed = compare.consumed_commands(raw)
        offset = sum(len(compare.consumed_commands(compare.life_events(events, earlier))) for earlier in range(life))
        commands = [(derived[offset + k][0], consumed[k][1]) for k in range(len(consumed)) if offset + k < len(derived)]
        script, reference_log = work / ("life%d.script" % life), work / ("life%d.ref.log" % life)
        script.write_text("\n".join(reference_script(commands, [tuple(map(int, line.split()[1:3])) for line in compare.hold_script(raw)])) + "\n")
        subprocess.run([*([sys.executable] if args.harness.endswith(".py") else []), args.harness, args.rom, str(script), str(until), str(reference_log), str(compare.boot_ticks(raw))], check=True, stdout=subprocess.DEVNULL, timeout=3600)
        native = compare.native_driver_events(events, life)
        reference = [e for e in compare.parse(str(reference_log)) if e[0] in "YP"]
        life_problems, result = compare.compare_to_horizon(native, reference, until)
        dac = sum(1 for i, e in enumerate(native) if e[0] == "Y" and e[1] == 1 and i > 0 and native[i - 1][:3] == ("Y", 0, 0x2A))
        result.update({"life": life, "until": until, "identical": not life_problems, "dac_data_writes": dac, "commands": len(commands), "bus_holds": len(compare.hold_script(raw)),
                       "problems": life_problems})
        print(json.dumps(result))
        ok = ok and not life_problems
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
