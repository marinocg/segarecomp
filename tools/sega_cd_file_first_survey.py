#!/usr/bin/env python3
"""SEG-100-T001 (ADR 0221): aggregate, privacy-safe survey of what a disc offers a file-first broad AOT build.

Inputs (all local, never copied into the output): the disc (`--cue`), the product CLI, and optionally the execution evidence of an independent
`segarecomp sega-cd build` run: its `status.json` (the extents the build pass's delivery ledger explained) and the `--coverage` file of the final
program's run (the instruction starts that run executed). Output: classes, counts and size classes only. No address, offset, LBA, name, hash or
byte of the disc is printed or written; the executed offsets are mapped in memory and handed to the CLI through a temporary probe file that is
deleted. Executed code is a falsification control for the candidate policy, never an input of it.

usage: sega_cd_file_first_survey.py --segarecomp CLI --cue DISC.cue [--status status.json --coverage coverage.txt] [--jobs N]
"""
import argparse
import json
import lzma
import os
import pathlib
import subprocess
import sys
import tempfile


def run_survey(cli, cue, jobs, *extra):
    done = subprocess.run([cli, "sega-cd", "file-survey", "--cue", cue, "--jobs", str(jobs), *extra], capture_output=True, text=True, timeout=7200)
    if not done.stdout.strip():
        raise SystemExit("file-survey produced no report: " + done.stderr[:200])
    return json.loads(done.stdout)


def size_class(n):
    return "lt_2k" if n < 2048 else "2k_to_32k" if n < 32768 else "32k_to_256k" if n < 262144 else "256k_to_512k" if n <= 524288 else "gt_512k"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--segarecomp", required=True)
    ap.add_argument("--cue", required=True)
    ap.add_argument("--status")
    ap.add_argument("--coverage")
    ap.add_argument("--jobs", default="8")
    ap.add_argument("--oracle-corpus", help="a private source-backed class-map snapshot (private-corpus/seg072-dev-v1): independent evaluation labels for the Sonic CD modules it names; read-only, never an input of the policy")
    ap.add_argument("--open", action="store_true", help="evaluation only: the closure in which a PC-relative branch or call leaving the file is a boundary (measured in ADR 0221; not a product policy)")
    ap.add_argument("--faults", action="store_true", help="evaluation only: the closure in which an always-faulting instruction is a valid terminating instruction (ADR 0221)")
    ap.add_argument("--skip-all-policy", action="store_true", help="do not lower every offset (the policy-all census is the expensive part)")
    args = ap.parse_args()
    out = {}
    iso = subprocess.run([args.segarecomp, "sega-cd", "inspect", "--cue", args.cue, "--iso-tree"], capture_output=True, text=True, timeout=600)
    tree = json.loads(iso.stdout)["iso9660"]
    files = [e for e in tree["entries"] if not e["directory"]]
    sizes = sorted(e["size"] for e in files)
    unique_bytes = None
    out["files"] = {"count": len(files), "bytes": sum(sizes), "min": sizes[0], "median": sizes[len(sizes) // 2], "max": sizes[-1], "by_size_class": {}}
    for e in files:
        c = size_class(e["size"])
        out["files"]["by_size_class"][c] = out["files"]["by_size_class"].get(c, 0) + 1
    # ---- the census under both policies (per-file records stay in memory) ----
    broad = run_survey(args.segarecomp, args.cue, args.jobs, "--per-file", *(["--no-census"] if args.skip_all_policy else []))
    probe_path = None
    if args.status and args.coverage:
        status = json.load(open(args.status))
        extents = []
        for text in status["extents"]:
            cpu, off, base, length = text.split(":")
            extents.append((cpu, int(off), int(base), int(length)))
        rows = {}
        for line in open(args.coverage):
            cpu, pc, count, first = line.split()
            if cpu not in ("0", "1"):
                continue
            rows.setdefault("main" if cpu == "0" else "sub", set()).add(int(pc, 16))
        groups, classes = set(), {"boot_rom": 0, "boot_area_extent": 0, "file_backed_attributable": 0, "file_backed_overlaid": 0, "no_extent": 0}
        for cpu, pcs in rows.items():
            mine = [e for e in extents if e[0] == cpu]
            for pc in pcs:
                if cpu == "main" and pc < 0x20000:
                    classes["boot_rom"] += 1
                    continue
                hits = [e for e in mine if e[2] <= pc < e[2] + e[3]]
                if not hits:
                    classes["no_extent"] += 1
                    continue
                offsets = sorted({e[1] + pc - e[2] for e in hits})
                if all(o < 32768 for o in offsets):
                    classes["boot_area_extent"] += 1
                    continue
                offsets = [o for o in offsets if o >= 32768]
                classes["file_backed_attributable" if len(offsets) == 1 else "file_backed_overlaid"] += 1
                groups.add(tuple(offsets))
        out["executed_instruction_starts"] = dict(classes, total=sum(rows[k].__len__() for k in rows))
        fd, probe_path = tempfile.mkstemp(prefix="scd-probes-")
        with os.fdopen(fd, "w") as f:
            f.write("\n".join(",".join(str(o) for o in g) for g in sorted(groups)) + "\n")
        # ---- execution evidence per extent, in aggregate ----
        by_class = {"main": 0, "sub": 0}
        in_file = {"whole_file": 0, "payload_after_header": 0, "in_system_area": 0}
        file_starts = {e["block"] * 2048: e["size"] for e in (dict(block=0, size=0),)}
        spans = [(e["block"] * 2048, e["block"] * 2048 + e["size"]) for e in broad["records"]]
        for cpu, off, base, length in extents:
            by_class[cpu] += 1
            if off < 32768:
                in_file["in_system_area"] += 1
                continue
            hit = [lo for lo, hi in spans if lo <= off < hi]
            if hit and off - hit[0] == 0:
                in_file["whole_file"] += 1
            elif hit:
                in_file["payload_after_header"] += 1
        out["execution_extents"] = {"total": len(extents), "by_processor": by_class, "placement": in_file}
        bases = {}
        for cpu, off, base, length in extents:
            bases.setdefault((cpu, off), set()).add(base)
        out["execution_extents"]["sources_at_more_than_one_base"] = sum(1 for v in bases.values() if len(v) > 1)
        ranges = {}
        for cpu, off, base, length in extents:
            ranges.setdefault(cpu, []).append((base, base + length, off))
        overlaid = 0
        for cpu, rs in ranges.items():
            for i, a in enumerate(rs):
                for b in rs[i + 1:]:
                    if a[0] < b[1] and b[0] < a[1] and a[2] != b[2]:
                        overlaid += 1
        out["execution_extents"]["different_sources_over_one_ram_range_pairs"] = overlaid
    oracle_dir = oracle_path = None
    if args.oracle_corpus:
        corpus = pathlib.Path(args.oracle_corpus)
        manifest = json.load(open(corpus / "MANIFEST.json"))
        images = manifest["images"]
        images = images if isinstance(images, list) else list(images.values())
        oracle_dir = tempfile.mkdtemp(prefix="scd-oracle-")
        lines = []
        for image in images:
            if image.get("kind") != "scd_module":
                continue
            raw = lzma.open(corpus / image["files"]["classes"]["path"]).read()
            path = os.path.join(oracle_dir, image["image_sha256"] + ".classes")
            open(path, "wb").write(raw)
            lines.append("%s %s" % (image["image_sha256"], path))
        oracle_path = os.path.join(oracle_dir, "oracle.txt")
        open(oracle_path, "w").write("\n".join(lines) + "\n")
        out["oracle_images_offered"] = len(lines)
    structural = run_survey(args.segarecomp, args.cue, args.jobs, "--structural-open" if args.open else "--structural-faults" if args.faults else "--structural", *(["--probe-offsets", probe_path] if probe_path else []), *(["--oracle", oracle_path] if oracle_path else []))
    if oracle_dir:
        for name in os.listdir(oracle_dir):
            os.unlink(os.path.join(oracle_dir, name))
        os.rmdir(oracle_dir)
    if probe_path:
        os.unlink(probe_path)
    totals = lambda r: {k: r[k] for k in ("lowered_files", "lowered_bytes", "oversize_files", "oversize_bytes")}
    if not args.skip_all_policy:
      out["policy_all"] = dict(totals(broad), even_offsets=broad["totals"]["even_offsets"], compiled_offsets=broad["totals"]["compiled"], source_relative_refused=
                             sum(broad["totals"][k] for k in ("refused_token_structure", "refused_literal_not_linear", "refused_literal_suffix", "refused_constant_context", "redecode")),
                             unique_encodings=broad["unique_encodings"], unique_body_text_bytes=broad["unique_text_bytes"], undecoded_offsets=broad["totals"]["not_decoded"])
    out["policy_structural"] = dict(totals(structural), decodable_offsets=structural["decodable_offsets"], candidate_offsets=structural["closure_valid_offsets"], compiled_offsets=structural["totals"]["compiled"],
                                    unique_encodings=structural["unique_encodings"], unique_body_text_bytes=structural["unique_text_bytes"])
    if structural.get("oracle", {}).get("modules"):
        out["policy_structural"]["oracle_recall"] = structural["oracle"]
    if "probes" in structural:
        out["policy_structural"]["executed_probe_recall"] = structural["probes"]
    print(json.dumps(out, indent=1, sort_keys=True))


if __name__ == "__main__":
    sys.exit(main())
