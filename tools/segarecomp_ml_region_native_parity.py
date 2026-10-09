#!/usr/bin/env python3
"""SEG-047 (ADR 0096) T004 gate: the NATIVE `--ml-region-proposal-output` digests must equal the committed SEG-046 parity digests.

Dev/CI verification tool only (never part of the product build). It scans a directory of locally held images, selects those whose
SHA-256 is listed in tools/segarecomp_ml_region.parity.json and runs the native CLI on each; it compares selected-window counts and
the SHA-256 of the final and ML-only canonical regions serialization. No ROM-derived data is written anywhere except an ephemeral temp
file; the output is aggregates and digests. Exit 0 = every found title matches exactly; 77 = no parity title found (CTest skip); 1 = mismatch.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
PARITY = ROOT / "tools" / "segarecomp_ml_region.parity.json"
LINE = re.compile(r"ml region proposal: windows=(\d+) ml_selected=(\d+) seed_windows=(\d+) final_selected=(\d+) region_bytes=(\d+) "
                  r"min_logit_margin=(\S+) regions_sha256=([0-9a-f]{64}) ml_only_regions_sha256=([0-9a-f]{64})")


def run_title(binary: str, rom: pathlib.Path, rom_sha: str, extra: list[str]) -> dict:
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp, "r.regions")
        proc = subprocess.run([binary, "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry", "--rom-sha256", rom_sha,
                               "--immutable-rom-aot", *extra, "--ml-region-proposal-output", str(out)],
                              capture_output=True, text=True, timeout=1800, check=False)
        match = LINE.search(proc.stderr)
        if proc.returncode != 0 or not match:
            return {"error": f"cli_exit_{proc.returncode}"}
        file_digest = hashlib.sha256(out.read_bytes()).hexdigest()
    w, ml, seeds, final, region_bytes, margin, regions_sha, ml_sha = match.groups()
    return {"windows": int(w), "ml_selected_windows": int(ml), "seed_windows": int(seeds), "final_selected_windows": int(final),
            "region_bytes": int(region_bytes), "min_logit_margin": float(margin), "regions_sha256": regions_sha,
            "ml_only_regions_sha256": ml_sha, "file_sha256": file_digest}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--binary", required=True)
    parser.add_argument("--games", required=True, help="directory with locally held images (never committed)")
    parser.add_argument("--extra", action="append", default=[], help="extra CLI argument (repeatable), e.g. --external-hints")
    args = parser.parse_args()
    expected = {t["rom_sha256"]: (label, t) for label, t in json.loads(PARITY.read_text())["titles"].items()}
    found, failures = {}, 0
    games = pathlib.Path(args.games)
    for path in sorted(games.iterdir()) if games.is_dir() else []:
        if not path.is_file() or path.stat().st_size > (32 << 20):
            continue
        sha = hashlib.sha256(path.read_bytes()).hexdigest()
        if sha not in expected:
            continue
        label, want = expected[sha]
        got = run_title(args.binary, path, sha, args.extra)
        ok = (got.get("regions_sha256") == want["regions_sha256"] and got.get("ml_only_regions_sha256") == want["ml_only_regions_sha256"]
              and got.get("file_sha256") == want["regions_sha256"] and got.get("ml_selected_windows") == want["ml_selected_windows"]
              and got.get("seed_windows") == want["seed_windows"] and got.get("final_selected_windows") == want["final_selected_windows"]
              and got.get("region_bytes") == want["region_bytes"] and got.get("windows") == want["windows"])
        found[label] = {"exact": ok, **{k: v for k, v in got.items() if k in ("min_logit_margin", "error")}}
        failures += 0 if ok else 1
    print(json.dumps({"titles": found, "expected_titles": len(expected)}, indent=1, sort_keys=True))
    if not found:
        return 77
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
