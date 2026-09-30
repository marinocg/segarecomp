"""SEG-009-T003 (open fact U11): analysis of the `u11_probe` fixture results (tools/sms_fixture_rom.py).

The probe issues one V-counter read per line interrupt (every second line), at instruction-start offset 4n T after the dispatch, behind k
superseded DD prefixes. Result byte index = 4 n + position of k in U11_PREFIXES. Trial i reads line (line_0 + 2 i) or, once the counter has moved on, line_0 + 2 i + 1; `flip` = that increment. Under
instruction-start ordering the first n that flips (n*) cannot depend on k; a machine that orders each access at its real
bus cycle moves n* down by k (each prefix delays the bus read by 4 T = one NOP step)."""
import sys
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
import sms_fixture_rom as builder  # noqa: E402


def analyze(results):
    """Returns {'n_star': {k: first flipping n or None}, 'monotone': bool, 'trials': int}."""
    steps, prefixes = builder.U11_STEPS, builder.U11_PREFIXES
    trials = steps * len(prefixes)
    raw = list(results[:trials])
    # The line restarts every frame: split into maximal runs of rising values (one run per frame) and, inside a run, express
    # every trial relative to the lowest 'raw - 2 x position' (R10 = 1: one interrupt every second line).
    runs, start = [], 0
    for i in range(1, trials):
        if raw[i] < raw[i - 1]:
            runs.append((start, i))
            start = i
    runs.append((start, trials))
    flips = [0] * trials
    for lo, hi in runs:
        relative = [raw[i] - 2 * (i - lo) for i in range(lo, hi)]
        base = min(relative)
        for i in range(lo, hi):
            flips[i] = relative[i - lo] - base
    n_star, monotone = {}, True
    for ki, k in enumerate(prefixes):
        column = [flips[n * len(prefixes) + ki] for n in range(steps)]
        if any(f not in (0, 1) for f in column) or any(a > b for a, b in zip(column, column[1:])):
            monotone = False
        n_star[k] = next((n for n, f in enumerate(column) if f == 1), None)
    return {"n_star": n_star, "monotone": monotone, "trials": trials, "flips": flips}
