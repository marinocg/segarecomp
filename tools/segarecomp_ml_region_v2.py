#!/usr/bin/env python3
"""SEG-048 (ADR 0099): OFFLINE multi-title hardening experiment for the M68K executable-region producer. Never a production route.

Builds on `segarecomp_ml_region.py` (SEG-046/047; unchanged, v1 artifacts are never touched). This module adds only what the milestone needs:
the pre-registered feature candidates F0..F5, four-way leave-one-title-out (LOTO) evaluation with the unchanged C++ prune + production validator,
the threshold/margin policy, the final v2 candidate freeze, and the HARD BLIND BARRIER (a sealed title's truth cannot be materialized before a
valid freeze record). Pure-Python helpers need no ML package; `fit`/`freeze` import scikit-learn lazily.

Leakage contract (ADR 0099 section 6): a feature is a pure function of ROM bytes and the C++ window export; a held-out title contributes zero
rows to the fit, scaler, threshold or any selection of its fold; the window start is only a row key and is never a feature.
"""
from __future__ import annotations

import argparse
import bisect
import concurrent.futures
import hashlib
import json
import math
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import segarecomp_ml_region as v1  # noqa: E402

WINDOW_BYTES = 512
SEED = v1.SEED
V2_FORBIDDEN = v1.FORBIDDEN_FRAGMENTS + ("hash", "game", "rom", "truth", "runtime", "annotation", "filename", "page", "ordinal")
SEALED_ROM_SHA256 = {  # formal blind titles E, F and the post-freeze control G (ADR 0099 section 3)
    "2754fa0ddf8a3f71998f4b7f5294fda7fe94cf452513d51c41d2ff8d3ae0f2d1": "E",
    "497958caf4e357f6862e438b1306e8a124a336b797a5dbe0ff50e09600162004": "F",
    "6e12e6b33c26ebfcd0be433251d21cf6284eafe9f71b027bda3767ae59affec1": "G",
}
FROZEN_FILES = ("segarecomp_ml_region_v2.model.json", "segarecomp_ml_region_v2.frozen.json", "segarecomp_ml_region_v2.parity.json")
FREEZE_SCHEMA = "segarecomp.ml_region_freeze.v2"
V1_ARTIFACTS = ("segarecomp_ml_region.py", "segarecomp_ml_region.frozen.json", "segarecomp_ml_region.model.json",
                "segarecomp_ml_region.parity.json", "segarecomp_ml_region_native_data.py", "segarecomp_ml_region_native_parity.py")


# ------------------------------------------------------------------------------------------------------------ feature candidates
CANDIDATES = {
    "F0": {"version": v1.FEATURE_VERSION, "drop": (), "neighbors": "zero", "families": ("b", "d")},
    "F1": {"version": "seg048-features-v2-F1", "drop": ("b_zlib",), "neighbors": "zero", "families": ("b", "d")},
    "F2": {"version": "seg048-features-v2-F2", "drop": ("b_zlib",), "neighbors": "replicate", "families": ("b", "d")},
    "F3": {"version": "seg048-features-v2-F3", "drop": ("b_zlib",), "neighbors": "none", "families": ("b", "d")},
    "F4": {"version": "seg048-features-v2-F4", "drop": ("b_zlib",), "neighbors": "replicate", "families": ("d",)},
    "F5": {"version": "seg048-features-v2-F5", "drop": ("b_zlib",), "neighbors": "replicate", "families": ("b",)},
}


def _spec(candidate: str) -> dict:
    if candidate not in CANDIDATES:
        raise SystemExit(f"unknown feature candidate: {candidate}")
    return CANDIDATES[candidate]


def base_columns(candidate: str) -> list[int]:
    """Indexes into the 47-wide v1 base vector kept by this candidate."""
    spec = _spec(candidate)
    names = v1.base_feature_names()
    return [i for i, name in enumerate(names) if name not in spec["drop"] and name.split("_", 1)[0] in spec["families"]]


def schema_names(candidate: str) -> list[str]:
    spec = _spec(candidate)
    base = v1.base_feature_names()
    kept = [base[i] for i in base_columns(candidate)]
    names = list(kept)
    if spec["neighbors"] != "none":
        names += [f"prev_{n}" for n in kept] + [f"next_{n}" for n in kept]
    return names


def assert_schema(names: list[str]) -> None:
    v1.assert_feature_schema(names)
    for name in names:
        if any(fragment in name.lower() for fragment in V2_FORBIDDEN):
            raise SystemExit(f"forbidden feature (leakage risk): {name}")


def schema_hash(candidate: str, window_bytes: int = WINDOW_BYTES) -> str:
    payload = json.dumps({"version": _spec(candidate)["version"], "window_bytes": window_bytes, "features": schema_names(candidate)},
                         sort_keys=True)
    return hashlib.sha256(payload.encode()).hexdigest()


def build_rows(base: list[list[float]], candidate: str) -> list[list[float]]:
    """Feature rows for every window of ONE ROM. Neighbour context never crosses ROMs; a missing neighbour is zero (F0/F1) or the
    window's own base vector (F2/F4/F5, no first/last-window sentinel) or absent (F3)."""
    spec = _spec(candidate)
    cols = base_columns(candidate)
    kept = [[row[i] for i in cols] for row in base]
    count = len(kept)
    zero = [0.0] * len(cols)
    rows = []
    for w, cur in enumerate(kept):
        if spec["neighbors"] == "none":
            rows.append(list(cur))
            continue
        edge = cur if spec["neighbors"] == "replicate" else zero
        rows.append(list(cur) + (kept[w - 1] if w else edge) + (kept[w + 1] if w + 1 < count else edge))
    return rows


# ---------------------------------------------------------------------------------------------------------------------- titles
class Title:
    """One ROM with everything a fold needs. `truth` is None for a title whose truth is (still) sealed."""

    def __init__(self, tid, rom_path, window_report, direct_report, address_report, truth_path=None):
        self.id = tid
        self.rom = pathlib.Path(rom_path).read_bytes()
        self.sha = hashlib.sha256(self.rom).hexdigest()
        self.rom_path = str(rom_path)
        report = v1.parse_window_report(pathlib.Path(window_report).read_text(), WINDOW_BYTES)
        count = (len(self.rom) + WINDOW_BYTES - 1) // WINDOW_BYTES
        self.count = count
        self.base = [v1.byte_features(self.rom[w * WINDOW_BYTES:(w + 1) * WINDOW_BYTES]) + v1.decoder_features(report.get(w), WINDOW_BYTES)
                     for w in range(count)]
        self.seeds = v1.seed_windows(pathlib.Path(direct_report).read_text(), len(self.rom), WINDOW_BYTES)
        self.universe = sorted(int(t, 16) for t in pathlib.Path(address_report).read_text().split())
        self.truth = None
        self.labels = None
        self.truth_sha = None
        if truth_path is not None:
            text = pathlib.Path(truth_path).read_text()
            self.truth = v1.read_universe(text, self.sha, len(self.rom))
            self.truth_sha = hashlib.sha256(text.encode()).hexdigest()
            self.labels = v1.window_labels(self.truth, len(self.rom), WINDOW_BYTES)
            if not self.truth <= set(self.universe):
                raise SystemExit(f"truth sanity failed for {tid}: C is not contained in the broad universe U")


def parse_title_spec(spec: str, truth: bool = True) -> Title:
    """id:rom:window_report:direct_report:address_report[:truth]"""
    parts = spec.split(":")
    v1.refuse_oracle_inputs(parts[2:])  # runtime coverage/oracle artifacts are never inputs here (pre- or post-freeze)
    if len(parts) not in (5, 6):
        raise SystemExit("title spec: id:rom:window_report:direct_report:address_report[:truth]")
    if len(parts) == 6 and not truth:
        raise SystemExit("truth refused for this command")
    return Title(*parts)


# ---------------------------------------------------------------------------------------------------------------- models and policy
MODELS = {"logreg": {"family": "LogisticRegression", "max_iter": 2000, "solver": "lbfgs", "class_weight": "balanced", "scaler": "standard"},
          "hgb": {"family": "HistGradientBoostingClassifier", "max_iter": 60, "max_depth": 3, "learning_rate": 0.1, "min_samples_leaf": 20,
                  "early_stopping": False, "class_weight": "balanced"}}
LOGREG_C = (0.25, 1.0, 4.0)
POLICIES = {"P0": 1.0, "P1": 0.5, "P2": 0.25}


def make_estimator(family: str, c: float = 1.0):
    from sklearn.ensemble import HistGradientBoostingClassifier
    from sklearn.linear_model import LogisticRegression
    from sklearn.pipeline import make_pipeline
    from sklearn.preprocessing import StandardScaler
    if family == "logreg":
        recipe = MODELS["logreg"]
        return make_pipeline(StandardScaler(), LogisticRegression(C=c, max_iter=recipe["max_iter"], solver=recipe["solver"],
                                                                     class_weight=recipe["class_weight"], random_state=SEED))
    if family == "hgb":
        r = MODELS["hgb"]
        return HistGradientBoostingClassifier(max_iter=r["max_iter"], max_depth=r["max_depth"], learning_rate=r["learning_rate"],
                                              min_samples_leaf=r["min_samples_leaf"], early_stopping=r["early_stopping"],
                                              class_weight=r["class_weight"], random_state=SEED)
    raise SystemExit(f"unknown model family: {family}")


def sklearn_fit(family: str, c: float = 1.0):
    """fit(X, y) -> proba(X) callable; the estimator, scaler and every statistic see ONLY the rows passed in."""
    def fit(rows, labels):
        import numpy as np
        model = make_estimator(family, c).fit(np.asarray(rows, dtype=np.float64), np.asarray(labels))
        return lambda x: [float(p) for p in model.predict_proba(np.asarray(x, dtype=np.float64))[:, 1]]
    return fit


def derive_threshold(train_probabilities: list[float], train_labels: list[int], factor: float) -> tuple[float, float]:
    """(threshold, min_positive): factor x the minimum probability of any positive TRAINING window (training titles only)."""
    positives = [p for p, y in zip(train_probabilities, train_labels) if y]
    if not positives:
        raise SystemExit("no positive training windows")
    minimum = min(positives)
    return factor * minimum, minimum


# ------------------------------------------------------------------------------------------------------------------ C++ prune bridge
class PruneRunner:
    """Runs the unchanged production CLI prune + validator for a regions file; results are cached by (ROM, regions) digest."""

    def __init__(self, cli: str, workdir: str):
        self.cli = cli
        self.workdir = pathlib.Path(workdir)
        self.workdir.mkdir(parents=True, exist_ok=True)

    def run(self, title: Title, regions: str) -> dict:
        key = hashlib.sha256((title.sha + regions).encode()).hexdigest()
        cache = self.workdir / f"{key}.json"
        if cache.exists():
            return json.loads(cache.read_text())
        regions_path, plan_path = self.workdir / f"{key}.regions", self.workdir / f"{key}.plan"
        regions_path.write_text(regions, newline="\n")
        result = subprocess.run([self.cli, "emit-general-startup-bridge-c", "--rom", title.rom_path, "--reset-entry", "--rom-sha256", title.sha,
                                 "--immutable-rom-aot", "--immutable-aot-region-proposal", str(regions_path), "--region-admission-plan-output",
                                 str(plan_path)], capture_output=True, text=True, check=False)
        stats = {"returncode": result.returncode, "validator": "rejected"}
        if result.returncode != 0:  # stable, content-free failure line (class + counts only)
            stats["reason"] = next((ln.split("segarecomp:", 1)[-1].strip() for ln in result.stderr.splitlines() if "rejected" in ln.lower()), "")
        line = next((ln for ln in result.stderr.splitlines() if "region prune:" in ln), "")
        for token in line.split():
            if "=" in token:
                k, v = token.split("=", 1)
                stats[k] = v
        if result.returncode == 0 and plan_path.exists():
            ranges = [tuple(int(x, 16) for x in ln.split()[1:3]) for ln in plan_path.read_text().splitlines() if ln.startswith("range ")]
            stats["ranges"] = ranges
        cache.write_text(json.dumps(stats))
        for path in (regions_path, plan_path):
            path.unlink(missing_ok=True)
        return stats


def admitted_set(universe: list[int], ranges) -> set[int]:
    starts = [r[0] for r in ranges]
    out = set()
    for address in universe:
        i = bisect.bisect_right(starts, address) - 1
        if i >= 0 and address < ranges[i][1]:
            out.add(address)
    return out


# ------------------------------------------------------------------------------------------------------------------------ evaluation
def evaluate(title: Title, probabilities: list[float], threshold: float, min_positive_train: float | None, runner: PruneRunner) -> dict:
    """Static gate metrics for one held-out/blind title. `title.truth` must be materialized."""
    if title.truth is None:
        raise SystemExit("evaluate needs materialized truth")
    selected = {w for w, p in enumerate(probabilities) if p >= threshold}
    region = selected | title.seeds
    outside_r = sorted(a for a in title.truth if a // WINDOW_BYTES not in region)
    positives = [probabilities[w] for w, y in enumerate(title.labels) if y]
    min_pos = min(positives)
    record = {"title": title.id, "rom_sha256": title.sha, "truth_c": len(title.truth), "windows": title.count, "universe_u": len(title.universe),
              "threshold": threshold, "ml_selected_windows": len(selected), "seed_windows": len(title.seeds), "r_windows": len(region),
              "r_bytes": sum(min((w + 1) * WINDOW_BYTES, len(title.rom)) - w * WINDOW_BYTES for w in region),
              "rom_bytes": len(title.rom), "c_outside_r": len(outside_r), "min_positive_probability": min_pos,
              "margin_factor": min_pos / threshold if threshold > 0 else math.inf}
    record["r_over_rom"] = record["r_bytes"] / record["rom_bytes"]
    stats = (runner.run(title, v1.regions_text(title.sha, len(title.rom), region, WINDOW_BYTES)) if region
             else {"returncode": -1, "validator": "rejected", "reason": "empty_region"})
    record["validator"] = "accepted" if stats.get("returncode") == 0 and stats.get("validator") == "accepted" else "rejected"
    if record["validator"] == "accepted":
        k = admitted_set(title.universe, stats["ranges"])
        if int(stats["k"]) != len(k):
            raise SystemExit("K cardinality mismatch between CLI report and plan ranges")
        record.update({"k0": int(stats["k0"]), "k": len(k), "k_over_u": len(k) / len(title.universe), "pruned": int(stats["pruned"]),
                       "rounds": int(stats["rounds"]), "c_outside_k": len(title.truth - k)})
    else:
        record.update({"k": None, "k_over_u": None, "c_outside_k": None, "failure": stats.get("returncode"), "reason": stats.get("reason", "")})
    return record


def gates_pass(record: dict, margin: float = 2.0, selectivity: float = 0.60) -> bool:
    return (record["c_outside_r"] == 0 and record["c_outside_k"] == 0 and record["validator"] == "accepted"
            and record["margin_factor"] >= margin and record["k_over_u"] <= selectivity)


def run_fold(titles: dict, held_out: str, candidate: str, fit, factor: float, runner: PruneRunner) -> dict:
    """Train on every title except `held_out`; threshold from the TRAINING titles only; then evaluate the held-out title."""
    train_ids = sorted(t for t in titles if t != held_out)
    if held_out in train_ids or not train_ids:
        raise SystemExit("fold construction failed")
    rows, labels = [], []
    for tid in train_ids:
        rows += build_rows(titles[tid].base, candidate)
        labels += titles[tid].labels
    scorer = fit(rows, labels)
    train_p = scorer(rows)
    threshold, min_positive = derive_threshold(train_p, labels, factor)
    held = titles[held_out]
    p = scorer(build_rows(held.base, candidate))
    record = evaluate(held, p, threshold, min_positive, runner)
    record.update({"train_titles": train_ids, "train_windows": len(rows), "train_positive_windows": sum(labels), "train_min_positive": min_positive,
                   "policy_factor": factor})
    return record


def run_loto(titles: dict, candidate: str, fit, factor: float, runner: PruneRunner, workers: int = 4) -> dict:
    ids = sorted(titles)
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        folds = list(pool.map(lambda h: run_fold(titles, h, candidate, fit, factor, runner), ids))
    ratios = [f["k_over_u"] for f in folds]
    complete = all(f["validator"] == "accepted" for f in folds)
    return {"candidate": candidate, "feature_count": len(schema_names(candidate)), "schema_sha256": schema_hash(candidate), "policy_factor": factor,
            "folds": folds, "all_contained_r": all(f["c_outside_r"] == 0 for f in folds),
            "all_contained_k": complete and all(f["c_outside_k"] == 0 for f in folds),
            "mean_k_over_u": (sum(ratios) / len(ratios)) if complete else None}


# ---------------------------------------------------------------------------------------------------------------- v1 zero-shot comparator
def canonical_probabilities(model: dict, rows: list[list[float]]) -> list[float]:
    out = []
    for row in rows:
        logit = v1.canonical_logit(model, row)
        out.append(1.0 / (1.0 + math.exp(-logit)) if logit > -700 else 0.0)
    return out


def zero_shot(model_path: str, frozen_path: str, titles: dict, runner: PruneRunner) -> list[dict]:
    frozen = json.loads(pathlib.Path(frozen_path).read_text())
    model = json.loads(pathlib.Path(model_path).read_text())
    v1.require_frozen(frozen)
    v1.validate_canonical_model(model, frozen)
    records = []
    for tid in sorted(titles):
        t = titles[tid]
        rows = build_rows(t.base, "F0")
        records.append(evaluate(t, canonical_probabilities(model, rows), frozen["threshold"], None, runner))
    return records


# --------------------------------------------------------------------------------------------------------- freeze and blind barrier
def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def seal_check(store: str) -> list[str]:
    """Names of artifacts under `store` that are (or contain) a source universe bound to a sealed title's ROM digest."""
    violations = []
    root = pathlib.Path(store)
    if not root.exists():
        return violations
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.stat().st_size > (64 << 20):
            continue
        head = path.read_bytes()[:200].decode("ascii", errors="ignore")
        lines = head.split("\n")
        if len(lines) > 1 and lines[0] == "segarecomp.m68k_source_universe.v1" and lines[1].startswith("rom_sha256 "):
            if lines[1].split(" ", 1)[1] in SEALED_ROM_SHA256:
                violations.append(str(path))
    return violations


def verify_freeze(tools_dir: str, freeze_commit: str | None = None, repo: str | None = None) -> dict:
    """Fail closed unless the committed candidate is intact: frozen record FROZEN, digests match the files; optionally the same bytes live
    in `freeze_commit`, an ancestor of HEAD."""
    tools = pathlib.Path(tools_dir)
    frozen_path = tools / "segarecomp_ml_region_v2.frozen.json"
    if not frozen_path.exists():
        raise SystemExit("blind barrier: no freeze record")
    frozen = json.loads(frozen_path.read_text())
    if frozen.get("schema") != FREEZE_SCHEMA or frozen.get("status") not in ("FROZEN", "NO_QUALIFYING_CANDIDATE"):
        raise SystemExit("blind barrier: freeze record is not FROZEN")
    if frozen["status"] == "NO_QUALIFYING_CANDIDATE":
        return verify_no_candidate(tools, frozen, freeze_commit, repo)
    for name, key in (("segarecomp_ml_region_v2.model.json", "model_json_sha256"), ("segarecomp_ml_region_v2.parity.json", "parity_json_sha256")):
        if sha256_bytes((tools / name).read_bytes()) != frozen.get(key):
            raise SystemExit(f"blind barrier: {name} differs from the frozen digest")
    model = json.loads((tools / "segarecomp_ml_region_v2.model.json").read_text())
    for field, expected in (("feature_schema_sha256", frozen["feature_schema_sha256"]), ("probability_threshold", frozen["threshold"]),
                            ("feature_version", frozen["feature_version"])):
        if model.get(field) != expected:
            raise SystemExit(f"blind barrier: model {field} differs from the frozen record")
    if schema_hash(frozen["candidate"]) != frozen["feature_schema_sha256"] or schema_names(frozen["candidate"]) != model.get("feature_names"):
        raise SystemExit("blind barrier: feature schema differs from the frozen candidate")
    if freeze_commit:
        if not re.fullmatch(r"[0-9a-f]{40}", freeze_commit):
            raise SystemExit("blind barrier: freeze commit must be a full SHA")
        git = ["git", "-C", repo or str(tools.parent)]
        if subprocess.run(git + ["merge-base", "--is-ancestor", freeze_commit, "HEAD"], capture_output=True).returncode != 0:
            raise SystemExit("blind barrier: freeze commit is not an ancestor of HEAD")
        for name in FROZEN_FILES:
            shown = subprocess.run(git + ["show", f"{freeze_commit}:tools/{name}"], capture_output=True)
            if shown.returncode != 0 or shown.stdout != (tools / name).read_bytes():
                raise SystemExit(f"blind barrier: {name} differs from the freeze commit")
    return frozen


def verify_no_candidate(tools: pathlib.Path, frozen: dict, freeze_commit: str | None, repo: str | None) -> dict:
    """The pre-registered negative decision (no policy satisfied every fold gate): v1 is retained, no v2 artifact exists and the v1
    artifacts must still match the digests recorded in the record. The barrier is equally binding: nothing may change afterwards."""
    for name, digest in frozen.get("v1_artifact_sha256", {}).items():
        if name not in V1_ARTIFACTS or sha256_bytes((tools / name).read_bytes()) != digest:
            raise SystemExit(f"blind barrier: v1 artifact {name} differs from the recorded digest")
    if sorted(frozen.get("v1_artifact_sha256", {})) != sorted(V1_ARTIFACTS):
        raise SystemExit("blind barrier: incomplete v1 artifact digests")
    if any((tools / name).exists() for name in ("segarecomp_ml_region_v2.model.json", "segarecomp_ml_region_v2.parity.json")):
        raise SystemExit("blind barrier: a v2 candidate artifact exists next to a no-candidate record")
    if freeze_commit:
        git = ["git", "-C", repo or str(tools.parent)]
        if not re.fullmatch(r"[0-9a-f]{40}", freeze_commit) or subprocess.run(
                git + ["merge-base", "--is-ancestor", freeze_commit, "HEAD"], capture_output=True).returncode != 0:
            raise SystemExit("blind barrier: freeze commit is not an ancestor of HEAD")
        shown = subprocess.run(git + ["show", f"{freeze_commit}:tools/segarecomp_ml_region_v2.frozen.json"], capture_output=True)
        if shown.returncode != 0 or shown.stdout != (tools / "segarecomp_ml_region_v2.frozen.json").read_bytes():
            raise SystemExit("blind barrier: the record differs from the freeze commit")
    return frozen


def v1_untouched(tools_dir: str, base_ref: str, repo: str | None = None) -> list[str]:
    """Names of v1 artifacts whose bytes differ from `base_ref` (must be empty)."""
    git = ["git", "-C", repo or str(pathlib.Path(tools_dir).parent)]
    changed = []
    for name in V1_ARTIFACTS:
        shown = subprocess.run(git + ["show", f"{base_ref}:tools/{name}"], capture_output=True)
        if shown.returncode != 0 or shown.stdout != (pathlib.Path(tools_dir) / name).read_bytes():
            changed.append(name)
    return changed


def materialize_truth(listing: str, rom_path: str, output: str, extractor_args: list[str], tools_dir: str,
                      freeze_commit: str | None, repo: str | None = None) -> int:
    """The only entry that may write truth for a SEALED title: it requires a valid freeze record first."""
    sha = hashlib.sha256(pathlib.Path(rom_path).read_bytes()).hexdigest()
    if sha in SEALED_ROM_SHA256:
        verify_freeze(tools_dir, freeze_commit, repo)
    extractor = str(pathlib.Path(__file__).resolve().parent / "segarecomp_source_map_extract.py")
    return subprocess.run([sys.executable, "-I", extractor, "--listing", listing, "--rom", rom_path, "--output", output] + extractor_args,
                          check=False).returncode


# ----------------------------------------------------------------------------------------------------------------- canonical v2 model
def canonical_v2(frozen_core: dict, mean, scale, coef, intercept) -> dict:
    n = len(frozen_core["feature_names"])
    if not (len(mean) == len(scale) == len(coef) == n) or any(s_ == 0.0 or not math.isfinite(s_) for s_ in scale):
        raise SystemExit("canonical v2: dimension or scale mismatch")
    folded = [c / s_ for c, s_ in zip(coef, scale)]
    bias = intercept - math.fsum(c * m_ / s_ for c, m_, s_ in zip(coef, mean, scale))
    p = frozen_core["threshold"]
    return {
        "schema": v1.CANONICAL_SCHEMA, "model_id": "SEG-048 M68K executable-region model v2 candidate (offline; not a production route)",
        "experiment": "SEG-048", "cpu": "MC68000", "feature_version": frozen_core["feature_version"],
        "feature_schema_sha256": frozen_core["feature_schema_sha256"], "feature_count": n, "window_bytes": WINDOW_BYTES,
        "feature_names": frozen_core["feature_names"], "training": frozen_core["training"],
        "probability_threshold": p, "logit_threshold": math.log(p / (1.0 - p)),
        "decision_rule": "select window iff folded_bias + sum(folded_weight[i] * x[i]) >= logit_threshold (equivalent to probability >= threshold)",
        "numeric_representation": "IEEE-754 binary64, shortest round-trip decimal (Python repr); folded_weight = coef/scale, "
                                  "folded_bias = intercept - fsum(coef*mean/scale)",
        "class_order": [0, 1], "positive_class": 1, "scaler_mean": mean, "scaler_scale": scale, "coef": coef, "intercept": intercept,
        "folded_weight": folded, "folded_bias": bias,
        "data_boundary": "Generic learned aggregate parameters only. No ROM bytes, address, offset, window start/ordinal, title feature, source label, "
                         "runtime coverage or per-ROM data is part of the model or its features.",
    }


# ------------------------------------------------------------------------------------------------------------------------ commands
def _titles(args) -> dict:
    titles = {}
    for spec in args.title:
        t = parse_title_spec(spec)
        if t.sha in SEALED_ROM_SHA256:
            raise SystemExit("refused: sealed title cannot enter a LOTO/training run")
        titles[t.id] = t
    return titles


def _fit_from_args(args):
    return sklearn_fit(args.model, args.c)


def cmd_loto(args) -> int:
    titles = _titles(args)
    runner = PruneRunner(args.cli, args.workdir)
    result = run_loto(titles, args.candidate, _fit_from_args(args), POLICIES[args.policy] if args.policy in POLICIES else float(args.policy), runner)
    result.update({"model": args.model, "c": args.c if args.model == "logreg" else None, "policy": args.policy})
    text = json.dumps(result, indent=1, sort_keys=True) + "\n"
    if args.output:
        pathlib.Path(args.output).write_text(text)
    for f in result["folds"]:
        print(json.dumps({k: f[k] for k in ("title", "c_outside_r", "c_outside_k", "k_over_u", "margin_factor", "validator")}))
    print(json.dumps({k: result[k] for k in ("candidate", "all_contained_r", "all_contained_k", "mean_k_over_u")}))
    return 0


def cmd_zero_shot(args) -> int:
    titles = _titles(args)
    records = zero_shot(args.model_json, args.frozen_json, titles, PruneRunner(args.cli, args.workdir))
    text = json.dumps(records, indent=1, sort_keys=True) + "\n"
    if args.output:
        pathlib.Path(args.output).write_text(text)
    for r in records:
        print(json.dumps({k: r[k] for k in ("title", "c_outside_r", "c_outside_k", "k_over_u", "margin_factor", "validator")}))
    return 0


def cmd_seal_check(args) -> int:
    violations = seal_check(args.store)
    if violations:
        print(json.dumps({"seal": "VIOLATED", "count": len(violations)}))
        return 1
    print(json.dumps({"seal": "INTACT"}))
    return 0


def cmd_verify_freeze(args) -> int:
    frozen = verify_freeze(args.tools_dir, args.freeze_commit, args.repo)
    print(json.dumps({"freeze": "VALID", "status": frozen["status"], "candidate": frozen.get("candidate")}))
    return 0


def cmd_materialize_truth(args) -> int:
    return materialize_truth(args.listing, args.rom, args.output, args.extractor_args, args.tools_dir, args.freeze_commit, args.repo)


def cmd_freeze(args) -> int:
    """Train the final candidate on all training titles; two independent fits must agree; write the v2 artifacts (no blind data)."""
    import numpy as np
    import sklearn
    titles = _titles(args)
    ids = sorted(titles)
    factor = POLICIES[args.policy]
    rows, labels = [], []
    for tid in ids:
        rows += build_rows(titles[tid].base, args.candidate)
        labels += titles[tid].labels
    x, y = np.asarray(rows, dtype=np.float64), np.asarray(labels)
    digests, models = [], []
    for _ in range(2):
        model = make_estimator("logreg", args.c).fit(x, y)
        models.append(model)
        scaler, lr = model.steps[0][1], model.steps[-1][1]
        digests.append((sha256_bytes(json.dumps([[float(v) for v in scaler.mean_], [float(v) for v in scaler.scale_],
                                                  [float(v) for v in lr.coef_[0]], float(lr.intercept_[0])]).encode()),
                        sha256_bytes(model.predict_proba(x).tobytes())))
    if digests[0] != digests[1]:
        raise SystemExit("nondeterministic training")
    model = models[0]
    scaler, lr = model.steps[0][1], model.steps[-1][1]
    probs = [float(p) for p in model.predict_proba(x)[:, 1]]
    threshold, min_positive = derive_threshold(probs, labels, factor)
    training = {"model": "logreg", "recipe": dict(MODELS["logreg"], C=args.c), "seed": SEED, "policy": args.policy, "policy_factor": factor,
                "min_positive_training_probability": min_positive,
                "titles": [{"id": tid, "rom_sha256": titles[tid].sha, "truth_sha256": titles[tid].truth_sha, "windows": titles[tid].count,
                            "positive_windows": sum(titles[tid].labels)} for tid in ids],
                "environment": {"python": sys.version.split()[0], "sklearn": sklearn.__version__, "numpy": np.__version__}}
    core = {"feature_names": schema_names(args.candidate), "feature_version": _spec(args.candidate)["version"],
            "feature_schema_sha256": schema_hash(args.candidate), "threshold": threshold, "training": training}
    canonical = canonical_v2(core, [float(v) for v in scaler.mean_], [float(v) for v in scaler.scale_], [float(v) for v in lr.coef_[0]],
                             float(lr.intercept_[0]))
    # canonical scoring must reproduce the sklearn selection on every training title before anything is written
    for tid in ids:
        sk = model.predict_proba(np.asarray(build_rows(titles[tid].base, args.candidate)))[:, 1]
        want = {w for w, p in enumerate(sk) if p >= threshold}
        got = v1.canonical_selected(canonical, build_rows(titles[tid].base, args.candidate))
        if want != got:
            raise SystemExit(f"canonical/sklearn selected-window mismatch for {tid}")
    out = pathlib.Path(args.out_dir)
    model_text = v1.canonical_json(canonical)
    (out / "segarecomp_ml_region_v2.model.json").write_text(model_text, newline="\n")
    parity = json.loads(pathlib.Path(args.parity_input).read_text())
    parity_text = json.dumps(parity, indent=1, sort_keys=True) + "\n"
    (out / "segarecomp_ml_region_v2.parity.json").write_text(parity_text, newline="\n")
    frozen = {"schema": FREEZE_SCHEMA, "status": "FROZEN", "experiment": "SEG-048", "candidate": args.candidate,
              "feature_version": core["feature_version"], "feature_count": len(core["feature_names"]),
              "feature_schema_sha256": core["feature_schema_sha256"], "window_bytes": WINDOW_BYTES, "model": "logreg", "c": args.c,
              "policy": args.policy, "policy_factor": factor, "threshold": threshold, "seed": SEED,
              "model_json_sha256": sha256_bytes(model_text.encode()), "parity_json_sha256": sha256_bytes(parity_text.encode()),
              "prediction_digest_sha256": digests[0][1], "fit_digest_sha256": digests[0][0], "training": training,
              "certain_code_union": "windows containing a precise direct-control-discovery identity (unchanged from v1)",
              "blind_barrier": "No feature, schema, window, family, hyperparameter, coefficient, scaler, threshold, margin, union, prune or "
                               "validator change is permitted after this record is committed (ADR 0099 section 9)."}
    (out / "segarecomp_ml_region_v2.frozen.json").write_text(json.dumps(frozen, indent=1, sort_keys=True) + "\n", newline="\n")
    print(json.dumps({k: frozen[k] for k in ("candidate", "feature_count", "threshold", "model_json_sha256", "prediction_digest_sha256")}))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def titles_arg(p):
        p.add_argument("--title", action="append", required=True, help="id:rom:window_report:direct_report:address_report:truth")
        p.add_argument("--cli", required=False, default="segarecomp")
        p.add_argument("--workdir", required=False, default=".")

    loto = sub.add_parser("loto")
    titles_arg(loto)
    loto.add_argument("--candidate", default="F0", choices=sorted(CANDIDATES))
    loto.add_argument("--model", default="logreg", choices=sorted(MODELS))
    loto.add_argument("--c", type=float, default=1.0)
    loto.add_argument("--policy", default="P0")
    loto.add_argument("--output")
    loto.set_defaults(func=cmd_loto)
    zs = sub.add_parser("zero-shot")
    titles_arg(zs)
    zs.add_argument("--model-json", required=True)
    zs.add_argument("--frozen-json", required=True)
    zs.add_argument("--output")
    zs.set_defaults(func=cmd_zero_shot)
    seal = sub.add_parser("seal-check")
    seal.add_argument("--store", required=True)
    seal.set_defaults(func=cmd_seal_check)
    vf = sub.add_parser("verify-freeze")
    vf.add_argument("--tools-dir", required=True)
    vf.add_argument("--freeze-commit")
    vf.add_argument("--repo")
    vf.set_defaults(func=cmd_verify_freeze)
    mt = sub.add_parser("materialize-truth")
    for flag in ("--listing", "--rom", "--output", "--tools-dir"):
        mt.add_argument(flag, required=True)
    mt.add_argument("--freeze-commit")
    mt.add_argument("--repo")
    mt.add_argument("extractor_args", nargs=argparse.REMAINDER)
    mt.set_defaults(func=cmd_materialize_truth)
    fz = sub.add_parser("freeze")
    titles_arg(fz)
    fz.add_argument("--candidate", required=True, choices=sorted(CANDIDATES))
    fz.add_argument("--c", type=float, default=1.0)
    fz.add_argument("--policy", required=True, choices=sorted(POLICIES))
    fz.add_argument("--out-dir", required=True)
    fz.add_argument("--parity-input", required=True)
    fz.set_defaults(func=cmd_freeze)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
