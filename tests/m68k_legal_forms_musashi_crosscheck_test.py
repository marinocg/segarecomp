#!/usr/bin/env python3
"""SEG-021-T001: pinned-Musashi 68000 primary-word sweep vs the manual dataset partition.
Skips cleanly (exit 0) unless SEGARECOMP_M68K_MULS_WORD_MUSASHI_CHECKOUT is the pinned checkout.
Musashi is used only as an overlap/hole cross-check of the 65,536 primary words."""
import pathlib
import sys

ROOT = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "tools"))
import m68k_word_sweep as sweep  # noqa: E402


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else "cc"
    chk = sweep.checkout()
    if chk is None:
        print("skipped: pinned Musashi checkout unavailable (%s unset)" % sweep.CHECKOUT_ENV)
        return 0
    result = sweep.sweep(compiler, chk)
    fixture = sweep.build_fixture(result)  # raises on any unexplained disagreement
    committed = sweep.DISAGREEMENTS.read_bytes()
    if sweep.render(fixture).encode("utf-8") != committed:
        print("FAIL: recorded sweep fixture differs from the fresh Musashi sweep; disagreements not fully recorded")
        return 1
    for entry in fixture["disagreements"]:
        if not entry["resolution"].strip():
            print("FAIL: disagreement without resolution")
            return 1
    # 68010+ words must trap (vector 4) on the 68000 core: encoded in the matrix.
    if fixture["outcome_matrix"].get("illegal_post_68000_encoding|4|4") != 2249:
        print("FAIL: a post-68000 encoding did not trap on the 68000 core")
        return 1
    print("Musashi 68000 sweep matches manual partition: %d explained range(s), 0 unexplained" % len(fixture["disagreements"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
