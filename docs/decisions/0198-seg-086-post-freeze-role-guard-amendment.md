# ADR 0198: SEG-086 - post-freeze sample-role guard amendment (corrective, not part of the pre-registered experiment)

- Status: Accepted. Follows ADRs 0195-0197; amends none of their results, decisions or numbers. This record is a **post-freeze security/correctness amendment** made during pre-merge review; it is not part of the frozen protocol.

## Why

ADR 0195 disclosed that the SEG-086 development loaders (`segarecomp_seg086_cal.load_records`, `segarecomp_seg086_diag._load`) apply only a lineage-name guard and SEG-085's reserved-identity check, not the per-sample role check. Review required the per-sample boundary to be enforced at runtime.

## Frozen-artifact implications (what was and was not touched)

- **Untouched, byte-identical:** `segarecomp_seg086_cal.py`, `segarecomp_seg086_norm.py`, `segarecomp_seg086_diag.py`, `segarecomp_sample_roles.py`, `segarecomp_seg086_cal.selection.json`, `segarecomp_seg086_cal.plan.json`/`.plan.digest`, `segarecomp_seg086_cal.results.json`, `segarecomp_seg086_diag.results.json`, `segarecomp_ml_v3_train.py` and every file bound by older plans. No digest was rewritten; `cal.verify_plan()` still returns `d5b2d43d...` and the frozen decision `CALIBRATION_INEFFECTIVE` is unchanged.
- **New, unbound, non-fitting:** `tools/segarecomp_sample_guard.py` (protection layer), `tools/segarecomp_seg086_guarded.py` (sanctioned entry point, `verify-frozen`), `tests/segarecomp_seg086_guard_test.py`.
- **Edited, unbound post-freeze analysis code:** `segarecomp_seg086_report.py` and `segarecomp_seg086_h01.py` now install the guard (H01 is constructed with `evaluation=ctx`). Their outputs are byte-identical to the committed `segarecomp_seg086_report.json` and `segarecomp_seg086_cal.h01.json`.

## Mechanism

Every sample the frozen tools load is constructed through `segarecomp_ml_v3_train.Image` (directly or via `xplat_transfer.load`). `segarecomp_sample_guard.install()` wraps `Image.__init__` and `registry()`: the metadata (`meta.json` only) is read, the reserved/sealed identity list is checked first (fail-closed if unreadable), then `require_trainable(meta)` (evaluation-only, exploratory, contradictory or unknown roles and frozen-evaluation identities/lineages are refused) - or, with `evaluation=ctx`, `ctx.admit(meta)` (exact frozen-manifest membership) - and only then does the original constructor read the ROM, tokens or class map. Existing `segarecomp_sample_roles` functions are reused unchanged.

## Limit

Calling `segarecomp_seg086_cal.py` or `segarecomp_seg086_diag.py` **directly** still uses the unguarded loaders (they could not be edited without invalidating the frozen plan). The sanctioned route is `segarecomp_seg086_guarded.py` (and the report/H01 modules). A future prospective experiment must start from a new, versioned pipeline that installs the guard itself.

## Reproducibility check

`python3 tools/segarecomp_seg086_guarded.py verify-frozen` (research environment): the frozen plan verifies; the 50 development records are identical with and without the guard (sha256 `e4d471de...ebfe1`); the nested evaluation recomputed under the guard reproduces the stored determinism digest `2957098525e6...`; the report and H01 modules regenerate their committed JSON byte-for-byte. The guard therefore changes neither experimental inputs nor outputs.

## Unrelated CI fix

The macOS CI failure on product main (`segarecomp_genesis_tasbatch5_test`, `ctl_a = all-controller primary`) is addressed in a separate PR (ZIP member timestamps; the SEG-083 test and tool are digest-bound and unchanged).
