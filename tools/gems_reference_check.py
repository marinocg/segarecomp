#!/usr/bin/env python3
"""Compares a native GEMS replacement run with the independent reference, life by life (ADR 0291, docs/testing/genesis-native-gems-validation.md).

  gems_reference_check.py --rom <game image> --native-log <SEGARECOMP_AUDIO_WRITE_LOG file> --harness <built tools/gems_reference_harness.c> [--work DIR]

For every run ("life") of the native driver in the log (a /RESET starts the next one) it writes the harness input (the commands the driver consumed and the windows in which the
68000 held the Z80 bus), runs the reference over the same guest time, and compares the two chip-access streams with tools/gems_reference_compare.py. It prints one JSON
object per life and exits 0 only when every life is identical in operation order, port and value.
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom", required=True)
    parser.add_argument("--native-log", required=True)
    parser.add_argument("--harness", required=True)
    parser.add_argument("--work", default=None)
    parser.add_argument("--end-ticks", type=int, default=6_000_000_000)
    args = parser.parse_args()
    events = compare.parse(args.native_log)
    count = len(compare.lives(events))
    work = pathlib.Path(args.work) if args.work else pathlib.Path(tempfile.mkdtemp(prefix="gems-reference-"))
    work.mkdir(parents=True, exist_ok=True)
    ok = count > 0
    for life in range(count):
        raw = compare.life_events(events, life)
        script, reference_log = work / ("life%d.script" % life), work / ("life%d.ref.log" % life)
        script.write_text("\n".join(compare.command_script(raw) + compare.hold_script(raw)) + "\n")
        boot = compare.boot_ticks(raw)
        subprocess.run([args.harness, args.rom, str(script), str(args.end_ticks), str(reference_log), str(boot)], check=True, stdout=subprocess.DEVNULL, timeout=3600)
        native = compare.native_driver_events(events, life)
        reference = [e for e in compare.parse(str(reference_log)) if e[0] in "YP"]
        until = native[-1][3] if native else 0
        reference = [e for e in reference if e[3] <= until]
        compared = min(len(native), len(reference))
        first = next((i for i in range(compared) if native[i][:3] != reference[i][:3]), None)
        identical = first is None and len(native) == len(reference)
        dac = sum(1 for i, e in enumerate(native) if e[0] == "Y" and e[1] == 1 and i > 0 and native[i - 1][:3] == ("Y", 0, 0x2A))
        result = {"life": life, "native_operations": len(native), "reference_operations": len(reference), "identical": identical, "dac_data_writes": dac,
                  "commands": sum(1 for line in compare.command_script(raw)), "bus_holds": len(compare.hold_script(raw)), "first_difference_index": first}
        if first is not None:
            result["native_at_difference"], result["reference_at_difference"] = native[first], reference[first]
        print(json.dumps(result))
        ok = ok and identical
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
