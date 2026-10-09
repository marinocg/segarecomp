#!/usr/bin/env python3
"""SEG-046 (ADR 0095): REPORT-ONLY, experiment-only ML executable-region PROPOSER. Never a production route; never runs in generated code.

Python owns features-from-bytes, labels, training, threshold selection, evaluation and report generation. It does NOT classify M68K: all
decoder/control columns come from the C++ `segarecomp ... --window-feature-report` export (`genesis_window_feature_report`).

Leakage contract (frozen in ADR 0095 section 1; enforced by `assert_feature_schema`):
  * a feature is a pure function of ROM bytes and the C++ window export; no address, offset, window ordinal, title, path, label, source
    symbol, runtime count or coverage may be a feature (the window start is only a row key and is dropped before modelling);
  * only Sonic 1 (SEG-044 exact source universe `C`) supplies labels; blind titles never do;
  * runtime coverage / oracle files are never read before the experiment is frozen (`require_frozen`, `refuse_oracle_inputs`).

Pure-Python parts (this file's top half) need no ML package. `train`/`propose` import numpy/scikit-learn lazily.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import re
import sys
import zlib

FEATURE_VERSION = "seg046-features-v1"
WINDOW_SIZES = (256, 512)
SHA = re.compile(r"[0-9a-f]{64}")
EXPECTED_DECODER_COLUMNS = [
    "ident", "len2", "len4", "len6", "len8p", "fam_move", "fam_arith", "fam_logic_bit", "fam_shift", "fam_control", "fam_other",
    "cond_branch", "uncond_direct", "call_direct", "call_any", "ret", "indirect", "terminator", "exception", "edge_out", "edge_same",
    "edge_adjacent", "edge_far_ident", "edge_dangling", "fall_dangling", "edge_in_local", "edge_in_external"]
BYTE_FEATURE_NAMES = ["b_entropy", "b_zero", "b_ff", "b_printable", "b_unique", "b_longest_run", "b_word_repeat", "b_zlib",
                      "b_hist0", "b_hist1", "b_hist2", "b_hist3", "b_hist4", "b_hist5", "b_hist6", "b_hist7"]
# Forbidden name fragments: a feature whose name contains one of these is rejected (positional / identity / label / runtime leakage).
FORBIDDEN_FRAGMENTS = ("addr", "offset", "ordinal", "index", "window_start", "position", "pos_", "title", "path", "name", "symbol", "label",
                       "source", "exec", "coverage", "oracle", "pc_", "frame", "rom_size", "sha")


def decoder_feature_names() -> list[str]:
    names = ["d_ident_density"]
    for column in EXPECTED_DECODER_COLUMNS[1:]:
        if column.startswith("edge_in_"):
            names.append(f"d_{column}_density")
        else:
            names.append(f"d_{column}_per_ident")
    names += ["d_local_edge_ratio", "d_adjacent_edge_ratio", "d_dangling_edge_ratio", "d_far_edge_ratio"]
    return names


def base_feature_names() -> list[str]:
    return BYTE_FEATURE_NAMES + decoder_feature_names()


def feature_names() -> list[str]:
    base = base_feature_names()
    return base + [f"prev_{n}" for n in base] + [f"next_{n}" for n in base]


def assert_feature_schema(names: list[str]) -> None:
    if len(set(names)) != len(names):
        raise SystemExit("forbidden feature schema: duplicate feature name")
    for name in names:
        lowered = name.lower()
        if any(fragment in lowered for fragment in FORBIDDEN_FRAGMENTS):
            raise SystemExit(f"forbidden feature (leakage risk): {name}")


def schema_hash(window_bytes: int, names: list[str] | None = None) -> str:
    names = feature_names() if names is None else names
    payload = json.dumps({"version": FEATURE_VERSION, "window_bytes": window_bytes, "features": names}, sort_keys=True)
    return hashlib.sha256(payload.encode()).hexdigest()


def sha256_file(path: str) -> str:
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def refuse_oracle_inputs(paths: list[str]) -> None:
    """Pre-freeze tooling never consumes a runtime coverage / oracle artifact."""
    for path in paths:
        lowered = pathlib.PurePath(path).name.lower()
        if "coverage" in lowered or "oracle" in lowered or lowered.endswith(".bitmap"):
            raise SystemExit("refused: runtime coverage/oracle artifacts are not accepted before the experiment is frozen")


# ---------------------------------------------------------------------------------------------------------------- byte features
def byte_features(chunk: bytes) -> list[float]:
    n = len(chunk)
    if n == 0:
        return [0.0] * len(BYTE_FEATURE_NAMES)
    counts = [0] * 256
    for byte in chunk:
        counts[byte] += 1
    entropy = -sum((c / n) * math.log2(c / n) for c in counts if c)
    printable = sum(counts[0x20:0x7F]) / n
    unique = sum(1 for c in counts if c) / 256.0
    longest = run = 1
    for i in range(1, n):
        run = run + 1 if chunk[i] == chunk[i - 1] else 1
        longest = max(longest, run)
    words = [(chunk[i] << 8) | chunk[i + 1] for i in range(0, n - 1, 2)]
    repeat = sum(1 for i in range(1, len(words)) if words[i] == words[i - 1]) / max(len(words) - 1, 1)
    compressed = len(zlib.compress(chunk, 6)) / n
    hist = [sum(counts[b * 32:(b + 1) * 32]) / n for b in range(8)]
    return [entropy / 8.0, counts[0] / n, counts[255] / n, printable, unique, longest / n, repeat, min(compressed, 2.0), *hist]


# ------------------------------------------------------------------------------------------------------------ C++ window export
def parse_window_report(text: str, window_bytes: int) -> dict[int, list[int]]:
    lines = text.split("\n")
    if len(lines) < 4 or lines[-1] != "" or lines[-2] != "end":
        raise SystemExit("invalid window feature report")
    if lines[0] != f"segarecomp.m68k_window_features.v1 window_bytes {window_bytes}":
        raise SystemExit("invalid window feature report header")
    if lines[1].split(" ")[0] != "columns" or lines[1].split(" ")[1:] != EXPECTED_DECODER_COLUMNS:
        raise SystemExit("window feature report column mismatch")
    rows: dict[int, list[int]] = {}
    for line in lines[2:-2]:
        parts = line.split(" ")
        if len(parts) != 1 + len(EXPECTED_DECODER_COLUMNS) or not re.fullmatch(r"[0-9a-f]{8}", parts[0]):
            raise SystemExit("invalid window feature row")
        start = int(parts[0], 16)
        if start % window_bytes or start in rows:
            raise SystemExit("invalid window feature row key")
        rows[start // window_bytes] = [int(v) for v in parts[1:]]
    return rows


def decoder_features(row: list[int] | None, window_bytes: int) -> list[float]:
    names = decoder_feature_names()
    if row is None:
        return [0.0] * len(names)
    words = window_bytes / 2.0
    ident = row[0]
    out = [ident / words]
    for column, value in zip(EXPECTED_DECODER_COLUMNS[1:], row[1:]):
        out.append(value / words if column.startswith("edge_in_") else (value / ident if ident else 0.0))
    edge_out = row[EXPECTED_DECODER_COLUMNS.index("edge_out")]
    for column in ("edge_same", "edge_adjacent", "edge_dangling", "edge_far_ident"):
        out.append(row[EXPECTED_DECODER_COLUMNS.index(column)] / edge_out if edge_out else 0.0)
    return out


def window_matrix(rom: bytes, report: dict[int, list[int]], window_bytes: int) -> tuple[list[list[float]], int]:
    """Feature rows for every window of the ROM (neighbour context = previous/next base vector, zeros beyond the ROM ends)."""
    count = (len(rom) + window_bytes - 1) // window_bytes
    base = [byte_features(rom[w * window_bytes:(w + 1) * window_bytes]) + decoder_features(report.get(w), window_bytes) for w in range(count)]
    zero = [0.0] * len(base_feature_names())
    return [base[w] + (base[w - 1] if w else zero) + (base[w + 1] if w + 1 < count else zero) for w in range(count)], count


# ----------------------------------------------------------------------------------------------------------------------- labels
def read_universe(text: str, rom_sha256: str, rom_size: int) -> set[int]:
    lines = text.split("\n")
    if (len(lines) < 8 or lines[0] != "segarecomp.m68k_source_universe.v1" or lines[1] != f"rom_sha256 {rom_sha256}"
            or lines[-1] != "" or lines[-2] != "end" or not lines[5].startswith("entries ")):
        raise SystemExit("invalid source universe (or wrong ROM hash)")
    entries = lines[6:-2]
    if int(lines[5].split(" ", 1)[1]) != len(entries) or not entries:
        raise SystemExit("invalid source universe")
    values = [int(e, 16) for e in entries if re.fullmatch(r"[0-9a-f]{8}", e)]
    if len(values) != len(entries) or values != sorted(set(values)) or any(v & 1 or v >= rom_size for v in values):
        raise SystemExit("invalid source universe")
    return set(values)


def window_labels(universe: set[int], rom_size: int, window_bytes: int) -> list[int]:
    count = (rom_size + window_bytes - 1) // window_bytes
    labels = [0] * count
    for address in universe:
        labels[address // window_bytes] = 1
    return labels


def uncovered_source_addresses(universe: set[int], selected_windows: set[int], window_bytes: int) -> list[int]:
    """Calibration containment check: source identities whose window is NOT selected (must be empty: C subset R)."""
    return sorted(a for a in universe if a // window_bytes not in selected_windows)


def seed_windows(addresses_text: str, rom_size: int, window_bytes: int) -> set[int]:
    """Certain-code windows: windows containing a precise direct-control-discovery identity (includes the machine roots)."""
    out = set()
    for token in addresses_text.split():
        address = int(token, 16)
        if 0 <= address < rom_size:
            out.add(address // window_bytes)
    return out


def regions_text(rom_sha256: str, rom_size: int, windows: set[int], window_bytes: int) -> str:
    if not SHA.fullmatch(rom_sha256) or rom_size <= 0 or rom_size % 2 or not windows:
        raise SystemExit("invalid region proposal inputs")
    runs: list[list[int]] = []
    for w in sorted(windows):
        begin, end = w * window_bytes, min((w + 1) * window_bytes, rom_size)
        if begin >= rom_size:
            raise SystemExit("window outside rom")
        if runs and runs[-1][1] == begin:
            runs[-1][1] = end
        else:
            runs.append([begin, end])
    return ("segarecomp.m68k_executable_regions.v1\n" f"rom_sha256 {rom_sha256}\n"
            + "".join(f"range {b:08x} {e:08x}\n" for b, e in runs) + "end\n")


# --------------------------------------------------------------------------------------------------------- blocked cross-validation
def blocked_folds(count: int, groups: int, purge: int) -> list[tuple[list[int], list[int]]]:
    """Contiguous groups; training excludes the held-out group plus `purge` windows on each side (neighbour-feature overlap)."""
    bounds = [count * g // groups for g in range(groups + 1)]
    folds = []
    for g in range(groups):
        lo, hi = bounds[g], bounds[g + 1]
        test = list(range(lo, hi))
        train = [w for w in range(count) if w < lo - purge or w >= hi + purge]
        folds.append((train, test))
    return folds


def threshold_at_full_recall(scores: list[float], labels: list[int]) -> float:
    positives = [s for s, y in zip(scores, labels) if y]
    if not positives:
        raise SystemExit("no positive windows")
    return min(positives)


# --------------------------------------------------------------------------------------------------------------------- model zoo
MODEL_RECIPES = {
    "logreg": {"family": "LogisticRegression", "C": 1.0, "max_iter": 2000, "solver": "lbfgs", "class_weight": "balanced", "scaler": "standard"},
    "hgb": {"family": "HistGradientBoostingClassifier", "max_iter": 60, "max_depth": 3, "learning_rate": 0.1, "min_samples_leaf": 20,
            "early_stopping": False, "class_weight": "balanced"},
}
SEED = 46


def make_model(name: str):
    from sklearn.ensemble import HistGradientBoostingClassifier
    from sklearn.linear_model import LogisticRegression
    from sklearn.pipeline import make_pipeline
    from sklearn.preprocessing import StandardScaler
    recipe = MODEL_RECIPES[name]
    if name == "logreg":
        return make_pipeline(StandardScaler(), LogisticRegression(C=recipe["C"], max_iter=recipe["max_iter"], solver=recipe["solver"],
                                                                     class_weight=recipe["class_weight"], random_state=SEED))
    return HistGradientBoostingClassifier(max_iter=recipe["max_iter"], max_depth=recipe["max_depth"], learning_rate=recipe["learning_rate"],
                                          min_samples_leaf=recipe["min_samples_leaf"], early_stopping=recipe["early_stopping"],
                                          class_weight=recipe["class_weight"], random_state=SEED)


def oof_scores(name: str, matrix, labels, folds):
    import numpy as np
    x, y = np.asarray(matrix, dtype=np.float64), np.asarray(labels)
    scores = np.zeros(len(y))
    for train, test in folds:
        model = make_model(name).fit(x[train], y[train])
        scores[test] = model.predict_proba(x[test])[:, 1]
    return scores


def auc(scores, labels) -> float:
    from sklearn.metrics import roc_auc_score
    return float(roc_auc_score(labels, scores))


def artifact_bytes(model) -> bytes:
    import pickle
    return pickle.dumps(model, protocol=4)


def load_artifact(path: str, expected_sha256: str):
    data = pathlib.Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != expected_sha256:
        raise SystemExit("model artifact digest mismatch: refusing to load")
    import pickle
    return pickle.loads(data)  # noqa: S301 - digest-pinned experiment artifact produced by this tool


def require_frozen(frozen: dict) -> None:
    if frozen.get("status") != "FROZEN" or not SHA.fullmatch(str(frozen.get("model_artifact_sha256", ""))):
        raise SystemExit("experiment is not frozen: refusing blind evaluation")
    if frozen.get("feature_schema_sha256") != schema_hash(frozen["window_bytes"]):
        raise SystemExit("feature schema hash mismatch")


# ------------------------------------------------------------------------------------------------------------------------ commands
def load_inputs(rom_path: str, window_report: str, window_bytes: int):
    rom = pathlib.Path(rom_path).read_bytes()
    report = parse_window_report(pathlib.Path(window_report).read_text(), window_bytes)
    matrix, count = window_matrix(rom, report, window_bytes)
    assert_feature_schema(feature_names())
    return rom, matrix, count


def cmd_train(args) -> int:
    import numpy as np
    refuse_oracle_inputs([args.rom, args.universe, args.window_report_256, args.window_report_512, args.direct_report_256])
    rom = pathlib.Path(args.rom).read_bytes()
    rom_sha = hashlib.sha256(rom).hexdigest()
    if rom_sha != args.rom_sha256:
        raise SystemExit("ROM hash mismatch: source labels are bound to the pinned Sonic 1 image")
    universe = read_universe(pathlib.Path(args.universe).read_text(), rom_sha, len(rom))
    direct_text = pathlib.Path(args.direct_report_256).read_text()
    results = []
    for window_bytes, report_path in ((256, args.window_report_256), (512, args.window_report_512)):
        _, matrix, count = load_inputs(args.rom, report_path, window_bytes)
        labels = window_labels(universe, len(rom), window_bytes)
        if args.shuffle_labels:  # mutation diagnostic: labels shuffled -> performance must collapse
            rng = np.random.RandomState(SEED)
            labels = list(rng.permutation(labels))
        seeds = seed_windows(direct_text, len(rom), window_bytes)
        folds = blocked_folds(count, args.groups, args.purge)
        for name in ("logreg", "hgb"):
            scores = oof_scores(name, matrix, labels, folds)
            threshold = threshold_at_full_recall(list(scores), labels)
            selected = {w for w in range(count) if scores[w] >= threshold} | seeds
            fraction = sum(min((w + 1) * window_bytes, len(rom)) - w * window_bytes for w in selected) / len(rom)
            record = {"window_bytes": window_bytes, "model": name, "windows": count, "positives": int(sum(labels)),
                      "oof_auc": round(auc(scores, labels), 4), "threshold": threshold, "oof_recall_at_threshold": 1.0,
                      "selected_rom_fraction": round(fraction, 4), "seed_windows": len(seeds)}
            results.append(record)
            print(json.dumps(record), file=sys.stderr)

    # Selection: lowest selected-ROM fraction at 100% OOF recall; candidates within 0.01 of the minimum are materially equal -> simpler wins.
    floor = min(r["selected_rom_fraction"] for r in results)
    near = [r for r in results if r["selected_rom_fraction"] <= floor + 0.01]
    chosen = sorted(near, key=lambda r: (0 if r["model"] == "logreg" else 1, r["window_bytes"]))[0]
    out = {"chosen": chosen, "all": results, "groups": args.groups, "purge_windows": args.purge, "shuffle_labels": args.shuffle_labels,
           "feature_schema_sha256": {str(w): schema_hash(w) for w in WINDOW_SIZES}, "seed": SEED, "recipes": MODEL_RECIPES}
    pathlib.Path(args.output).write_text(json.dumps(out, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"chosen": chosen}))
    return 0


def cmd_freeze(args) -> int:
    """Train the chosen recipe on ALL Sonic 1 windows; write the private artifact and the frozen-definition JSON (no ROM-derived data)."""
    import numpy as np
    refuse_oracle_inputs([args.rom, args.universe, args.window_report, args.direct_report, args.cv_json])
    cv = json.loads(pathlib.Path(args.cv_json).read_text())
    chosen = cv["chosen"]
    window_bytes = chosen["window_bytes"]
    rom = pathlib.Path(args.rom).read_bytes()
    rom_sha = hashlib.sha256(rom).hexdigest()
    if rom_sha != args.rom_sha256:
        raise SystemExit("ROM hash mismatch")
    universe = read_universe(pathlib.Path(args.universe).read_text(), rom_sha, len(rom))
    _, matrix, count = load_inputs(args.rom, args.window_report, window_bytes)
    labels = window_labels(universe, len(rom), window_bytes)
    x, y = np.asarray(matrix, dtype=np.float64), np.asarray(labels)
    digests = []
    for _ in range(2):  # determinism: two independent fits give the same artifact and predictions
        model = make_model(chosen["model"]).fit(x, y)
        digests.append((hashlib.sha256(artifact_bytes(model)).hexdigest(), hashlib.sha256(model.predict_proba(x).tobytes()).hexdigest()))
    if digests[0] != digests[1]:
        raise SystemExit("nondeterministic training")
    data = artifact_bytes(model)
    pathlib.Path(args.artifact).write_bytes(data)
    import sklearn
    frozen = {"status": "FROZEN", "experiment": "SEG-046", "window_bytes": window_bytes, "model": chosen["model"],
              "recipe": MODEL_RECIPES[chosen["model"]], "seed": SEED, "threshold": chosen["threshold"], "feature_version": FEATURE_VERSION,
              "feature_count": len(feature_names()), "feature_schema_sha256": schema_hash(window_bytes),
              "model_artifact_sha256": hashlib.sha256(data).hexdigest(), "prediction_digest_sha256": digests[0][1],
              "certain_code_union": "windows containing a precise direct-control-discovery identity (includes machine roots)",
              "blocked_cv": {"groups": cv["groups"], "purge_windows": cv["purge_windows"]},
              "training_title": "Sonic 1 REV00", "training_rom_sha256": rom_sha,
              "environment": {"python": sys.version.split()[0], "sklearn": sklearn.__version__, "numpy": np.__version__}}
    pathlib.Path(args.frozen_json).write_text(json.dumps(frozen, indent=1, sort_keys=True) + "\n")
    print(json.dumps({k: frozen[k] for k in ("window_bytes", "model", "threshold", "model_artifact_sha256", "feature_schema_sha256")}))
    return 0


def cmd_propose(args) -> int:
    """Frozen model -> regions file for any ROM. Consumes only ROM bytes + C++ window export + direct-control address report."""
    import numpy as np
    refuse_oracle_inputs([args.rom, args.window_report, args.direct_report])
    frozen = json.loads(pathlib.Path(args.frozen_json).read_text())
    require_frozen(frozen)
    window_bytes = frozen["window_bytes"]
    model = load_artifact(args.artifact, frozen["model_artifact_sha256"])
    rom = pathlib.Path(args.rom).read_bytes()
    rom_sha = hashlib.sha256(rom).hexdigest()
    _, matrix, count = load_inputs(args.rom, args.window_report, window_bytes)
    scores = model.predict_proba(np.asarray(matrix, dtype=np.float64))[:, 1]
    seeds = seed_windows(pathlib.Path(args.direct_report).read_text(), len(rom), window_bytes)
    selected = {w for w in range(count) if scores[w] >= frozen["threshold"]} | seeds
    pathlib.Path(args.regions).write_text(regions_text(rom_sha, len(rom), selected, window_bytes), newline="\n")
    if args.scores_out:  # private diagnostic artifact (ignored location): per-window probabilities
        pathlib.Path(args.scores_out).write_text("".join(f"{s:.9f}\n" for s in scores))
    ml_windows = {w for w in range(count) if scores[w] >= frozen["threshold"]}
    region_bytes = sum(min((w + 1) * window_bytes, len(rom)) - w * window_bytes for w in selected)
    print(json.dumps({"rom_bytes": len(rom), "windows": count, "ml_selected_windows": len(ml_windows), "seed_windows": len(seeds),
                      "selected_windows": len(selected), "region_bytes": region_bytes, "region_fraction": round(region_bytes / len(rom), 4)}))
    return 0


# ------------------------------------------------------------------------------------- canonical framework-independent model
CANONICAL_SCHEMA = "segarecomp.ml_region_model.v1"
CANONICAL_MODEL_ID = "SEG-046 M68K executable-region model v1"
FROZEN_SKLEARN_ARTIFACT_SHA256 = "b5ddae5aa0fe6571455803c244d2a6be4a2c3349e3436c8780bfc3d8aa8fa62b"


def canonical_model_from_arrays(frozen: dict, mean: list[float], scale: list[float], coef: list[float], intercept: float) -> dict:
    """Pure-Python derivation of the folded classifier and logit threshold from the exact fitted parameters."""
    n = len(feature_names())
    if not (len(mean) == len(scale) == len(coef) == n == frozen["feature_count"]):
        raise SystemExit("canonical model: dimension mismatch")
    if any(s_ == 0.0 or not math.isfinite(s_) for s_ in scale):
        raise SystemExit("canonical model: invalid scale")
    folded = [c / s_ for c, s_ in zip(coef, scale)]
    bias = intercept - math.fsum(c * m_ / s_ for c, m_, s_ in zip(coef, mean, scale))
    p = frozen["threshold"]
    return {
        "schema": CANONICAL_SCHEMA, "model_id": CANONICAL_MODEL_ID, "experiment": "SEG-046", "cpu": "MC68000",
        "owner": "Genesis executable-region producer (report-only experiment; not a production route)",
        "feature_version": frozen["feature_version"], "feature_schema_sha256": frozen["feature_schema_sha256"],
        "feature_count": n, "window_bytes": frozen["window_bytes"],
        "feature_names": feature_names(),
        "original_sklearn_artifact_sha256": frozen["model_artifact_sha256"],
        "training": {"model": frozen["model"], "recipe": frozen["recipe"], "seed": frozen["seed"], "training_title": frozen["training_title"],
                     "environment": frozen["environment"], "blocked_cv": frozen["blocked_cv"]},
        "probability_threshold": p, "logit_threshold": math.log(p / (1.0 - p)),
        "decision_rule": "select window iff folded_bias + sum(folded_weight[i] * x[i]) >= logit_threshold (equivalent to probability >= threshold)",
        "numeric_representation": "IEEE-754 binary64, shortest round-trip decimal (Python repr); folded_weight = coef/scale, "
                                  "folded_bias = intercept - fsum(coef*mean/scale)",
        "class_order": [0, 1], "positive_class": 1,
        "scaler_mean": mean, "scaler_scale": scale, "coef": coef, "intercept": intercept,
        "folded_weight": folded, "folded_bias": bias,
        "data_boundary": "Generic learned aggregate parameters only. No ROM bytes, address, offset, window start/ordinal, title, source label, "
                         "runtime coverage or per-ROM data is part of the model or its features.",
    }


def canonical_json(model: dict) -> str:
    return json.dumps(model, indent=1, sort_keys=True) + "\n"


def validate_canonical_model(model: dict, frozen: dict) -> None:
    """Fail closed on any schema/dimension/identity mismatch against the committed frozen definition."""
    required = {"schema", "model_id", "feature_version", "feature_schema_sha256", "feature_count", "window_bytes", "feature_names",
                "original_sklearn_artifact_sha256", "probability_threshold", "logit_threshold", "scaler_mean", "scaler_scale", "coef",
                "intercept", "folded_weight", "folded_bias", "class_order", "positive_class"}
    if not isinstance(model, dict) or required - set(model):
        raise SystemExit("canonical model: missing fields")
    if model["schema"] != CANONICAL_SCHEMA or model["feature_version"] != FEATURE_VERSION:
        raise SystemExit("canonical model: schema or feature version mismatch")
    n = model["feature_count"]
    if n != 141 or n != len(feature_names()) or model["feature_names"] != feature_names():
        raise SystemExit("canonical model: feature list mismatch")
    for key in ("scaler_mean", "scaler_scale", "coef", "folded_weight"):
        if not isinstance(model[key], list) or len(model[key]) != n or not all(isinstance(v, float) and math.isfinite(v) for v in model[key]):
            raise SystemExit(f"canonical model: bad {key}")
    for key in ("intercept", "folded_bias", "probability_threshold", "logit_threshold"):
        if not isinstance(model[key], float) or not math.isfinite(model[key]):
            raise SystemExit(f"canonical model: bad {key}")
    if (model["feature_schema_sha256"] != frozen["feature_schema_sha256"] or model["window_bytes"] != frozen["window_bytes"]
            or model["original_sklearn_artifact_sha256"] != frozen["model_artifact_sha256"]
            or model["probability_threshold"] != frozen["threshold"]):
        raise SystemExit("canonical model: does not match the frozen definition")
    if model["original_sklearn_artifact_sha256"] != FROZEN_SKLEARN_ARTIFACT_SHA256 or model["feature_schema_sha256"] != schema_hash(model["window_bytes"]):
        raise SystemExit("canonical model: identity digest mismatch")
    if model["logit_threshold"] != math.log(model["probability_threshold"] / (1.0 - model["probability_threshold"])):
        raise SystemExit("canonical model: logit threshold mismatch")
    if model["class_order"] != [0, 1] or model["positive_class"] != 1 or any(s_ <= 0.0 for s_ in model["scaler_scale"]):
        raise SystemExit("canonical model: class order or scale invalid")


def canonical_logit(model: dict, row: list[float], folded: bool = True) -> float:
    if len(row) != model["feature_count"]:
        raise SystemExit("canonical model: feature vector dimension mismatch")
    if folded:
        return model["folded_bias"] + math.fsum(w * x for w, x in zip(model["folded_weight"], row))
    return model["intercept"] + math.fsum(c * ((x - m_) / s_) for c, x, m_, s_ in zip(model["coef"], row, model["scaler_mean"], model["scaler_scale"]))


def canonical_selected(model: dict, rows: list[list[float]]) -> set[int]:
    return {w for w, row in enumerate(rows) if canonical_logit(model, row) >= model["logit_threshold"]}


def selection_digest(rom_sha256: str, rom_size: int, selected: set[int], window_bytes: int) -> str:
    """SHA-256 of the canonical regions serialization of the selected windows (nothing about the windows is stored)."""
    return hashlib.sha256(regions_text(rom_sha256, rom_size, selected, window_bytes).encode()).hexdigest()


def _pipeline_arrays(pipeline):
    scaler, logreg = pipeline.steps[0][1], pipeline.steps[-1][1]
    if list(logreg.classes_) != [0, 1] or logreg.coef_.shape != (1, 141) or scaler.mean_.shape != (141,):
        raise SystemExit("unexpected sklearn pipeline shape")
    return ([float(v) for v in scaler.mean_], [float(v) for v in scaler.scale_], [float(v) for v in logreg.coef_[0]], float(logreg.intercept_[0]))


def cmd_export_model(args) -> int:
    frozen = json.loads(pathlib.Path(args.frozen_json).read_text())
    require_frozen(frozen)
    if hashlib.sha256(pathlib.Path(args.artifact).read_bytes()).hexdigest() != FROZEN_SKLEARN_ARTIFACT_SHA256:
        raise SystemExit("original artifact digest mismatch: refusing to export")
    pipeline = load_artifact(args.artifact, frozen["model_artifact_sha256"])
    model = canonical_model_from_arrays(frozen, *_pipeline_arrays(pipeline))
    validate_canonical_model(model, frozen)
    text = canonical_json(model)
    if canonical_json(canonical_model_from_arrays(frozen, *_pipeline_arrays(pipeline))) != text:
        raise SystemExit("nondeterministic export")
    pathlib.Path(args.output).write_text(text, newline="\n")
    print(json.dumps({"canonical_sha256": hashlib.sha256(text.encode()).hexdigest(), "features": model["feature_count"],
                      "logit_threshold": model["logit_threshold"]}))
    return 0


def cmd_verify_parity(args) -> int:
    """Experiment-only preservation check: canonical scoring vs the sklearn pipeline on the locally available SEG-046 titles."""
    import numpy as np
    frozen = json.loads(pathlib.Path(args.frozen_json).read_text())
    require_frozen(frozen)
    model = json.loads(pathlib.Path(args.model).read_text())
    validate_canonical_model(model, frozen)
    pipeline = load_artifact(args.artifact, frozen["model_artifact_sha256"])
    report = {"max_abs_logit_diff_folded": 0.0, "max_abs_logit_diff_unfolded": 0.0, "titles": {}}
    for spec in args.title:  # label:rom:window_report:direct_report
        label, rom_path, window_report_path, direct_path = spec.split(":", 3)
        rom = pathlib.Path(rom_path).read_bytes()
        rom_sha = hashlib.sha256(rom).hexdigest()
        _, matrix, count = load_inputs(rom_path, window_report_path, frozen["window_bytes"])
        x = np.asarray(matrix, dtype=np.float64)
        proba = pipeline.predict_proba(x)[:, 1]
        reference = {w for w in range(count) if proba[w] >= frozen["threshold"]}
        decision = pipeline.decision_function(x)
        folded = [canonical_logit(model, row) for row in matrix]
        unfolded = [canonical_logit(model, row, folded=False) for row in matrix]
        diff_f = max(abs(a - float(b)) for a, b in zip(folded, decision))
        diff_u = max(abs(a - float(b)) for a, b in zip(unfolded, decision))
        canonical = canonical_selected(model, matrix)
        if canonical != reference:
            raise SystemExit(f"selected-window mismatch for {label}: refusing to record parity")
        seeds = seed_windows(pathlib.Path(direct_path).read_text(), len(rom), frozen["window_bytes"])
        final = reference | seeds
        region_bytes = sum(min((w + 1) * frozen["window_bytes"], len(rom)) - w * frozen["window_bytes"] for w in final)
        report["titles"][label] = {
            "rom_sha256": rom_sha, "rom_bytes": len(rom), "windows": count, "ml_selected_windows": len(reference), "seed_windows": len(seeds),
            "final_selected_windows": len(final), "region_bytes": region_bytes, "region_fraction": round(region_bytes / len(rom), 4),
            "selected_equal_sklearn_vs_canonical": True,
            "regions_sha256": selection_digest(rom_sha, len(rom), final, frozen["window_bytes"]),
            "ml_only_regions_sha256": selection_digest(rom_sha, len(rom), reference, frozen["window_bytes"]),
            "sklearn_proba_float64_le_sha256": hashlib.sha256(proba.astype("<f8").tobytes()).hexdigest()}
        report["max_abs_logit_diff_folded"] = max(report["max_abs_logit_diff_folded"], diff_f)
        report["max_abs_logit_diff_unfolded"] = max(report["max_abs_logit_diff_unfolded"], diff_u)
    pathlib.Path(args.output).write_text(json.dumps(report, indent=1, sort_keys=True) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "titles"}))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    train = sub.add_parser("train")
    for flag in ("--rom", "--universe", "--window-report-256", "--window-report-512", "--direct-report-256", "--rom-sha256", "--output"):
        train.add_argument(flag, required=True)
    train.add_argument("--groups", type=int, default=16)
    train.add_argument("--purge", type=int, default=2)
    train.add_argument("--shuffle-labels", action="store_true")
    train.set_defaults(func=cmd_train)
    freeze = sub.add_parser("freeze")
    for flag in ("--rom", "--universe", "--window-report", "--direct-report", "--rom-sha256", "--cv-json", "--artifact", "--frozen-json"):
        freeze.add_argument(flag, required=True)
    freeze.set_defaults(func=cmd_freeze)
    propose = sub.add_parser("propose")
    for flag in ("--rom", "--window-report", "--direct-report", "--frozen-json", "--artifact", "--regions"):
        propose.add_argument(flag, required=True)
    propose.add_argument("--scores-out")
    propose.set_defaults(func=cmd_propose)
    export = sub.add_parser("export-model")
    for flag in ("--frozen-json", "--artifact", "--output"):
        export.add_argument(flag, required=True)
    export.set_defaults(func=cmd_export_model)
    parity = sub.add_parser("verify-parity")
    for flag in ("--frozen-json", "--artifact", "--model", "--output"):
        parity.add_argument(flag, required=True)
    parity.add_argument("--title", action="append", required=True, help="label:rom:window_report:direct_report")
    parity.set_defaults(func=cmd_verify_parity)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
