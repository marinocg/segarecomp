#!/usr/bin/env python3
"""SEG-076-T004 (exploratory research tooling): every committed SEG-076 class re-derives from its committed result document with the
frozen pure rule functions (T001 pre-registered + both ROCm amendment views, T003 transfer / stability / A / M1, T005 C-v2 vs A).
The M1 bootstrap needs numpy; without it (CI without the research environment) the test is skipped."""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
# The SEG-076 research runners are Linux research tooling and import the Unix-only `resource` module (peak-RSS records). They are
# digest-bound by their frozen plans, so they stay unchanged; on platforms without `resource` (Windows CI) a stand-in lets these tests
# exercise the pure parts (rules, barrier, job lists), which never call it.
try:
    import resource  # noqa: F401
except ImportError:
    import types
    sys.modules["resource"] = types.ModuleType("resource")


def main() -> int:
    try:
        import numpy  # noqa: F401
    except ImportError:
        print("skip: numpy not installed (research-environment check)")
        return 77
    import segarecomp_xplat_seg076_final as f
    res = f.decisions()
    bad = {k: v for k, v in res.items() if not v["consistent"]}
    expected = {"t001": "NEGATIVE_STOP", "t003_m3r_transfer": "EVIDENCE_INSUFFICIENT", "t003_seed_stability": "NO_MEANINGFUL_ADVANTAGE",
                "t003_a_transfer": "NO_MEANINGFUL_ADVANTAGE", "t003_m1": "DESCRIPTIVE_EFFECT_NOT_ROBUST", "t005_cv2_vs_a": "EVIDENCE_INSUFFICIENT"}
    wrong = {k: res[k]["recorded"] for k, v in expected.items() if res[k]["recorded"] != v}
    if bad or wrong or len(res) != 8:
        print("FAIL:", bad, wrong, len(res))
        return 1
    print("segarecomp_xplat_seg076_final_test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
