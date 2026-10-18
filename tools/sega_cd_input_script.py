#!/usr/bin/env python3
"""Author a Sega CD input script (ADR 0149) from a vertical-blank-onset schedule. Stdlib only; contains no game data.

The machine input's time is an absolute machine tick since reset. The vertical-blank onsets of the BIOS-image profile are exactly
periodic: onset number n (1-based) is at ONSET + (n - 1) * FRAME ticks (contract section 16), with 500,000 ticks per master clock,
3,420 master clocks per line, 262 lines per frame and the onset at the start of line 224. This tool converts "press at the onset
that follows `--base-onsets` + N onsets, hold H frames" into the two absolute ticks, so that a schedule fixed in frames (ADR 0148)
is expressed in the one canonical time the runtime accepts.

usage: sega_cd_input_script.py onset --base-onsets B --press N --hold H [--port 1|2] [--button NAME]
   prints the script: press at onset B + N, release at onset B + N + H.
"""
import argparse
import sys

MASTER_TICKS = 500_000
LINE_MASTER = 3420
FRAME_TICKS = LINE_MASTER * 262 * MASTER_TICKS         # 448,020,000,000
ONSET_TICKS = LINE_MASTER * 224 * MASTER_TICKS         # 382,968,000,000
BUTTONS = ("UP", "DOWN", "LEFT", "RIGHT", "A", "B", "C", "START")


def onset_tick(n):
    if n < 1:
        raise ValueError("onset numbers start at 1")
    return ONSET_TICKS + (n - 1) * FRAME_TICKS


def script(base, press, hold, port=1, button="START"):
    if hold < 1 or press < 1 or base < 0 or port not in (1, 2) or button not in BUTTONS:
        raise ValueError("invalid schedule")
    first = onset_tick(base + press)
    second = onset_tick(base + press + hold)
    return "segarecomp-input-script 1\n%d %d press %s\n%d %d release %s\n" % (first, port, button, second, port, button)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("command", choices=["onset"])
    parser.add_argument("--base-onsets", type=int, required=True)
    parser.add_argument("--press", type=int, required=True)
    parser.add_argument("--hold", type=int, required=True)
    parser.add_argument("--port", type=int, default=1)
    parser.add_argument("--button", default="START")
    args = parser.parse_args(argv)
    try:
        sys.stdout.write(script(args.base_onsets, args.press, args.hold, args.port, args.button))
    except ValueError as error:
        print("error: %s" % error, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
