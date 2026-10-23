#!/usr/bin/env python3
"""SEG-080-T002: the SEG-080-T001 experiment amended to the full movie replay (OFFLINE research tooling).

A thin wrapper: it imports the frozen T001 adapter unchanged and overrides only the window (all 115,665 movie frames), the five checkpoints
(20/40/60/80/100 % of the movie), the plan files and the Gate A progression rule (>= 3 level-mode checkpoints in >= 3 zones, in play order).
Thresholds, label pipeline, tiers, Gate B, Gate C and the decision logic are the T001 ones. Same subcommands as the T001 adapter.

Post-results disclosure: the T001 playback timeline and results were seen before this amendment (operator-directed re-analysis).
"""
from __future__ import annotations

import copy
import inspect
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_genesis_bizhawk as bz  # noqa: E402

MOVIE_FRAMES = 115665
bz.FRAMES = MOVIE_FRAMES
bz.CHECKPOINTS = [MOVIE_FRAMES * k // 5 for k in range(1, 6)]
bz.PLAN = HERE / "segarecomp_genesis_bizhawk_full.t002.plan.json"
bz.PLAN_DIGEST = HERE / "segarecomp_genesis_bizhawk_full.t002.plan.digest"
bz.BOUND = ["tools/segarecomp_genesis_bizhawk.py", "tools/segarecomp_genesis_bizhawk_full.py", "tools/segarecomp_genesis_evidence.py"]

P = copy.deepcopy(bz.PROTOCOL)
P["task"] = "SEG-080-T002"
P["amendment_F1"] = ("post-results, operator-directed: the window is the full movie (115,665 frames) instead of the first 15 minutes; the T001 result "
                     "(ADR 0181, REPLAY_INVALID by a gate-design artifact) stays as recorded. The T001 playback timeline and results were seen first. "
                     "Checkpoints move to 20/40/60/80/100 % of the movie; the Gate A progression rule needs >= 3 level-mode checkpoints in >= 3 "
                     "zones in play order (zone 0x05, the third Metropolis act, is added to the play order); thresholds, tiers, Gate B and C and the "
                     "decision logic are unchanged. The control runs the same number of frames with the same policy and seed.")
P["window"] = {"frames": MOVIE_FRAMES, "note": "the complete movie from power-on", "checkpoints": bz.CHECKPOINTS,
               "growth_every_frames": bz.GROWTH_EVERY}
P["control"]["seconds"] = MOVIE_FRAMES // 60
P["gate_a"]["zone_play_order"] = [0x00, 0x0D, 0x0F, 0x0C, 0x07, 0x0B, 0x0A, 0x04, 0x05, 0x10, 0x06, 0x0E]
P["gate_a"]["progression"] = ("all five checkpoints in level or special-stage mode; >= 3 level-mode checkpoints; their zones, in checkpoint order, "
                              "form a non-decreasing subsequence of zone_play_order with >= 3 distinct zones")
bz.PROTOCOL = P

_src = inspect.getsource(bz.gate_a)
assert "len(level) >= 2" in _src and "len(set(zones)) >= 2" in _src
exec(compile(_src.replace("len(level) >= 2", "len(level) >= 3").replace("len(set(zones)) >= 2", "len(set(zones)) >= 3"),
             "<gate_a full-window variant>", "exec"), bz.__dict__)

if __name__ == "__main__":
    sys.exit(bz.main())
