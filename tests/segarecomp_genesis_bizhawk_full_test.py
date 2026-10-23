#!/usr/bin/env python3
"""SEG-080-T002: tests of the full-movie wrapper (synthetic data only): window/checkpoints, the stricter Gate A progression rule, unchanged
thresholds and the frozen-plan invariants. The T001 adapter's own tests cover the shared logic (run them separately: module state differs)."""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_genesis_bizhawk_full as full  # noqa: E402
import segarecomp_genesis_bizhawk as bz  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)


def test_window_and_rule():
    check(bz.FRAMES == 115665 and bz.CHECKPOINTS == [23133, 46266, 69399, 92532, 115665], "full window and checkpoints")
    ident = {"rom_ok": True, "movie_rom_md5_matches": True, "movie": {"starts_from_savestate": False, "players_in_log": 2, "frames": 115665,
             "header": {"Core": "Genplus-gx", "emuVersion": "Version 2.9.1"}}}
    run = {"pcs_sha256": "a", "reads_sha256": "b", "edges_sha256": "c", "map_mismatch": 0, "odd_pcs": 0, "reset_pc_executed": True,
           "frames": bz.FRAMES, "movie_finished_at": 5, "movie_mode_end": "FINISHED", "read_self_fetch_fraction": 0.0, "vbl_pc_executed": True,
           "cdl_sha256": "d", "cdl": {"cdl_exec_bytes": 100, "cdl_exec_mismatch_bytes": 0}}
    lab = {"meta": {"typed": {}, "decoders": {"executed_rom_pcs": 100}, "tiers": {"E": 100}}}

    def ck(states):
        return [{"tag": "ck", "frame": c, "state": s} for c, s in zip(bz.CHECKPOINTS, states)]
    ss = "zone=0 act=1 mode=16 x=1 y=1"
    good = ck([ss, ss, "zone=13 act=1 mode=12 x=1", "zone=10 act=1 mode=12 x=1", "zone=14 act=0 mode=12 x=1"])
    two = ck([ss, ss, ss, "zone=10 act=1 mode=12 x=1", "zone=14 act=0 mode=12 x=1"])
    back = ck([ss, ss, "zone=10 act=1 mode=12 x=1", "zone=13 act=1 mode=12 x=1", "zone=14 act=0 mode=12 x=1"])
    end = "zone=14 act=0 mode=12"
    g = lambda c: bz.gate_a(ident, run, run, dict(run), c, c, lab, end, (run, dict(run)), True)
    check(g(good)["progression"], "three level checkpoints in play order pass")
    check(not g(two)["progression"], "two level checkpoints no longer suffice")
    check(not g(back)["progression"], "zones out of play order fail")


def test_plan():
    check(bz.PROTOCOL["gate_c"]["extra_code_positive_cells_min"] == 24 and bz.PROTOCOL["gate_c"]["extra_mixed_cells_min"] == 8, "thresholds unchanged")
    check(bz.PROTOCOL["gate_b"]["new_label_accuracy_min"] == 0.999, "gate B unchanged")
    check(bz.PROTOCOL["task"] == "SEG-080-T002" and bz.PROTOCOL["control"]["seconds"] == 1927, "control length matches the movie")
    if bz.PLAN.exists():
        check(bool(bz.verify_plan()), "frozen plan verifies")


def main():
    test_window_and_rule()
    test_plan()
    print("FAILURES:", len(FAILURES), *FAILURES)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
