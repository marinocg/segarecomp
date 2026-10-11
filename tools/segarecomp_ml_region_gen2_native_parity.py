#!/usr/bin/env python3
"""SEG-049-T003 (ADR 0102) gate: the NATIVE gen2 M3 + B35 route versus the frozen research reference.

Dev/CI verification tool only (never part of the product build). Output is aggregates and digests; no ROM-derived data is written anywhere
except ephemeral temp files.

  local      the five source-backed ROMs (matched by SHA-256 among locally held images): exact native/research selected-cell parity against the
             committed in-sample digests of `segarecomp_ml_region_gen2.parity.json`, an independent stdlib re-selection from the native logits,
             the proposed-region serialization, the existing structural prune + production validator (K stage) on the native region, repeated-run
             determinism and (optionally, with `--research-scores`) the region/K stage fed from the research PyTorch scores. Exit 77 = no parity
             title found (CTest skip), 1 = mismatch.
  reference  synthetic images (no ROM): the native logits versus the frozen model evaluated by PyTorch float32 (needs numpy + torch; exit 77
             when absent): reports the numerical difference, enforces the documented tolerance and exact selected-cell equality.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import segarecomp_ml_region_gen2 as g  # noqa: E402
import segarecomp_ml_region_gen2_rank as rk  # noqa: E402

PARITY = TOOLS / "segarecomp_ml_region_gen2.parity.json"
MODEL = TOOLS / "segarecomp_ml_region_gen2.model.json"
LOGIT_TOLERANCE_ABS = 1e-4  # documented numerical contract (ADR 0102 section 3): |native binary32 logit - reference| <= tolerance
LINE = re.compile(r"ml gen2 region proposal: cells=(\d+) ml_selected=(\d+) seed_cells=(\d+) final_selected=(\d+) region_bytes=(\d+) "
                  r"boundary_tie_cells=(\d+) boundary_gap=(\S+) selected_cells_sha256=([0-9a-f]{64}) logits_sha256=([0-9a-f]{64}) "
                  r"regions_sha256=([0-9a-f]{64}) ml_only_regions_sha256=([0-9a-f]{64})")
PRUNE = re.compile(r"region prune: universe=(\d+) k0=(\d+) k=(\d+) pruned=(\d+) rounds=(\d+) validator=accepted")


def lcg_rom(words: int, seed: int) -> bytes:
    """The private 64-bit LCG of tests/genesis_ml_gen2_test.cpp (identical sequence)."""
    state = (seed * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
    out = bytearray()
    for _ in range(words * 2):
        state = (state * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        out.append((state >> 33) % 256)
    return bytes(out)


def read_logits(path: pathlib.Path) -> list[float]:
    data = path.read_bytes()
    return [struct.unpack("<f", data[i:i + 4])[0] for i in range(0, len(data), 4)]


def selected_digest(cells) -> str:
    return g.sha256_bytes("".join(f"{c}\n" for c in sorted(cells)).encode())


def run_title(binary: str, rom: pathlib.Path, rom_sha: str, tmp: pathlib.Path, tag: str) -> dict:
    regions, logits = tmp / f"{tag}.regions", tmp / f"{tag}.logits"
    proc = subprocess.run([binary, "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry", "--rom-sha256", rom_sha, "--immutable-rom-aot",
                           "--ml-gen2-region-proposal-output", str(regions), "--ml-gen2-logits-output", str(logits)],
                          capture_output=True, text=True, timeout=3600, check=False)
    match = LINE.search(proc.stderr)
    if proc.returncode != 0 or not match:
        return {"error": f"cli_exit_{proc.returncode}"}
    c, ml, seeds, final, region_bytes, ties, gap, sel_sha, logit_sha, regions_sha, ml_regions_sha = match.groups()
    return {"cells": int(c), "ml_selected": int(ml), "seed_cells": int(seeds), "final_selected": int(final), "region_bytes": int(region_bytes),
            "boundary_tie_cells": int(ties), "boundary_gap": float(gap), "selected_cells_sha256": sel_sha, "logits_sha256": logit_sha,
            "regions_sha256": regions_sha, "ml_only_regions_sha256": ml_regions_sha, "regions_file": regions, "logits_file": logits,
            "file_sha256": hashlib.sha256(regions.read_bytes()).hexdigest()}


def prune_stage(binary: str, rom: pathlib.Path, rom_sha: str, regions: pathlib.Path, tmp: pathlib.Path, tag: str) -> dict:
    plan = tmp / f"{tag}.plan"
    proc = subprocess.run([binary, "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry", "--rom-sha256", rom_sha, "--immutable-rom-aot",
                           "--immutable-aot-region-proposal", str(regions), "--region-admission-plan-output", str(plan)],
                          capture_output=True, text=True, timeout=3600, check=False)
    match = PRUNE.search(proc.stderr)
    if proc.returncode != 0 or not match:
        return {"validator": "rejected", "detail": proc.stderr.strip().splitlines()[-1:] or ["no output"]}
    universe, k0, k, pruned, rounds = (int(x) for x in match.groups())
    return {"validator": "accepted", "universe": universe, "k0": k0, "k": k, "pruned": pruned, "rounds": rounds,
            "plan_sha256": hashlib.sha256(plan.read_bytes()).hexdigest()}


def cmd_local(args) -> int:
    parity = json.loads(PARITY.read_text())
    by_sha = {meta["rom_sha256"]: tid for tid, meta in g.CORPUS.items()}
    results, failures, scores = {}, 0, None
    if args.research_scores:
        import numpy as np
        scores = np.load(args.research_scores)
    games = pathlib.Path(args.games)
    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = pathlib.Path(tmp_name)
        for path in sorted(games.iterdir()) if games.is_dir() else []:
            if not path.is_file() or path.stat().st_size > (32 << 20):
                continue
            sha = hashlib.sha256(path.read_bytes()).hexdigest()
            if sha not in by_sha:
                continue
            tid = by_sha[sha]
            first = run_title(args.binary, path, sha, tmp, tid + "1")
            if "error" in first:
                results[tid] = {"exact": False, **first}
                failures += 1
                continue
            second = run_title(args.binary, path, sha, tmp, tid + "2")  # repeated-run determinism
            logits = read_logits(first["logits_file"])
            reselected = sorted(rk.rank_select(logits, 35))  # independent stdlib implementation of score-desc / ordinal-asc / ceil budget
            want_cells = parity["in_sample_selected_cells"][tid]
            want_digest = parity["in_sample_selected_cell_set_digest"][tid]
            rom_size = path.stat().st_size
            python_regions = g.regions_text(sha, rom_size, reselected)
            record = {
                "cells": first["cells"], "selected": first["ml_selected"], "boundary_tie_cells": first["boundary_tie_cells"],
                "boundary_gap": first["boundary_gap"],
                "selected_cell_parity": first["selected_cells_sha256"] == want_digest and first["ml_selected"] == want_cells,
                "stdlib_reselection_parity": selected_digest(reselected) == first["selected_cells_sha256"],
                "ml_only_region_parity": g.sha256_bytes(python_regions.encode()) == first["ml_only_regions_sha256"],
                "final_region_equals_ml_only": first["regions_sha256"] == first["ml_only_regions_sha256"],
                "repeat_identical": all(first[k] == second.get(k) for k in ("selected_cells_sha256", "logits_sha256", "regions_sha256")),
                "seed_cells": first["seed_cells"], "final_selected": first["final_selected"],
            }
            stage = prune_stage(args.binary, path, sha, first["regions_file"], tmp, tid)
            record["k_stage"] = {k: v for k, v in stage.items() if k != "plan_sha256"}
            if scores is not None and tid in scores.files:
                research = sorted(rk.rank_select(scores[tid].tolist(), 35))
                research_regions = tmp / f"{tid}.research.regions"
                research_regions.write_text(g.regions_text(sha, rom_size, research))
                research_stage = prune_stage(args.binary, path, sha, research_regions, tmp, tid + "r")
                record["research_scores_selected_parity"] = selected_digest(research) == first["selected_cells_sha256"]
                record["research_region_parity"] = g.sha256_bytes(research_regions.read_bytes()) == first["ml_only_regions_sha256"]
                record["research_admitted_set_parity"] = (research_stage.get("plan_sha256") == stage.get("plan_sha256")
                                                          and stage.get("validator") == "accepted")
            ok = all(v for k, v in record.items() if isinstance(v, bool)) and stage.get("validator") == "accepted"
            record["exact"] = ok
            failures += 0 if ok else 1
            results[tid] = record
    print(json.dumps({"titles": results, "expected_titles": len(by_sha), "logit_tolerance_abs": LOGIT_TOLERANCE_ABS}, indent=1, sort_keys=True))
    if not results:
        return 77
    return 1 if failures else 0


def torch_logits(rom: bytes, tokens: bytes):
    """Frozen M3 evaluated by PyTorch float32 (the research implementation) for every 128-byte cell of `rom`."""
    import numpy as np
    import torch
    sys.path.insert(0, str(TOOLS))
    import segarecomp_ml_region_gen2_train as tr
    weights = json.loads(MODEL.read_text())["weights"]
    model = tr.build_model(True)
    state = model.state_dict()
    model.load_state_dict({k: (torch.tensor(np.array(weights[k]), dtype=torch.float32) if k in weights else v) for k, v in state.items()})
    model.eval()
    words, cells = len(rom) // 2, g.cell_count(len(rom))
    idx = np.array([g.reflect_index(j - g.CONTEXT_PAD_WORDS, words) for j in range(cells * g.CELL_WORDS + 2 * g.CONTEXT_PAD_WORDS)], dtype=np.int64)
    arr = np.frombuffer(rom[:words * 2], dtype=np.uint8).reshape(words, 2)
    tok = np.frombuffer(tokens, dtype=np.uint8).reshape(words, 8)
    out = np.empty(cells, dtype=np.float64)
    with torch.no_grad():
        for start in range(0, cells, 64):
            n = min(64, cells - start)
            sl = idx[start * g.CELL_WORDS:start * g.CELL_WORDS + n * g.CELL_WORDS + 2 * g.CONTEXT_PAD_WORDS]
            h = torch.from_numpy(arr[sl, 0].astype(np.int64))[None]
            lo = torch.from_numpy(arr[sl, 1].astype(np.int64))[None]
            t = torch.from_numpy(tok[sl].astype(np.int64))[None]
            out[start:start + n] = model(h, lo, t).squeeze(0).double().numpy()
    return out.tolist()


def cmd_reference(args) -> int:
    try:
        import numpy  # noqa: F401
        import torch  # noqa: F401
    except ImportError:
        print("numpy/torch unavailable: skipped", file=sys.stderr)
        return 77
    images = [("lcg_24_cells", 64 * 24, 49), ("lcg_edge_65_words", 65, 7), ("lcg_150_cells_partial", 64 * 150 + 21, 123)]
    report, failures = {}, 0
    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = pathlib.Path(tmp_name)
        for name, words, seed in images:
            rom = lcg_rom(words, seed)
            rom_path, tok_path, logit_path = tmp / f"{name}.rom", tmp / f"{name}.tok", tmp / f"{name}.logits"
            rom_path.write_bytes(rom)
            subprocess.run([args.binary, "m68k-decode-token-report", "--rom", str(rom_path), "--output", str(tok_path)], check=True)
            subprocess.run([args.binary, "ml-gen2-cell-logits", "--rom", str(rom_path), "--output", str(logit_path)], check=True, capture_output=True)
            tokens = g.parse_token_report(tok_path.read_bytes(), len(rom))
            native = read_logits(logit_path)
            ref = torch_logits(rom, tokens)
            diffs = [abs(a - b) for a, b in zip(native, ref)]
            native_sel, ref_sel = sorted(rk.rank_select(native, 35)), sorted(rk.rank_select(ref, 35))
            ok = len(native) == len(ref) and max(diffs) <= LOGIT_TOLERANCE_ABS and native_sel == ref_sel
            report[name] = {"cells": len(native), "max_abs_diff_vs_torch_float32": max(diffs), "selected_equal": native_sel == ref_sel,
                            "selected_cells_sha256": selected_digest(native_sel), "tokens_sha256": g.sha256_bytes(tokens),
                            "logits_sha256": hashlib.sha256(logit_path.read_bytes()).hexdigest(), "ok": ok}
            failures += 0 if ok else 1
    print(json.dumps({"images": report, "logit_tolerance_abs": LOGIT_TOLERANCE_ABS}, indent=1, sort_keys=True))
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    local = sub.add_parser("local")
    local.add_argument("--binary", required=True)
    local.add_argument("--games", required=True, help="directory with locally held images (never committed)")
    local.add_argument("--research-scores", help="optional private npz of research PyTorch probabilities per title (numpy required)")
    local.set_defaults(func=cmd_local)
    ref = sub.add_parser("reference")
    ref.add_argument("--binary", required=True)
    ref.set_defaults(func=cmd_reference)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
