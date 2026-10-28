#!/usr/bin/env python3
"""SEG-097-T003 (ADR 0208): the pre-registered three-arm comparison of the experimental Sega CD M3 path. Stdlib only; project-authored fixtures; aggregate output.

Arms: A = per-mapping broad emission (ADR 0132 reference); B = broad with position-independent bodies (ADR 0207); C = experimental frozen M3 + B35 with
position-independent bodies. Subjects: S1 = the `unvisited` disc (case `pi`) with the module list discovered by T001 (synthetic profile; independent labels
exist: the fixture encoder's own instruction starts); S2 = the BIOS-image profile fixture `direct` built end to end by `segarecomp sega-cd build` (A vs the
experimental policy; no labels).

usage: sega_cd_m3_compare.py <segarecomp> <c-compiler> <source-root>   ->   JSON on stdout
"""
import json
import pathlib
import subprocess
import sys
import tempfile

# A = full Compatibility (per-mapping, the reference); B = Compatibility with source-relative bodies; H = the HISTORICAL M1 experimental scope (every image scored);
# C = the experimental policy as shipped (only independently admitted disc game modules / extents are scored; firmware, BIOS-derived and boot code stay broad).
ARMS = {"A": [], "B": ["--position-independent"],
        "H": ["--position-independent", "--aot-policy", "optimized", "--aot-model", "gen2", "--aot-scope", "all-images"],
        "C": ["--position-independent", "--aot-policy", "optimized", "--aot-model", "gen2"]}
ROUTES = {"default": None, "B": 2, "C": 3}


def main():
    segarecomp, cc, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
    sys.path.insert(0, str(root / "tests"))
    import sega_cd_common as common
    result = {"schema": 1, "subjects": {}}
    with tempfile.TemporaryDirectory(prefix="scd-m3-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(segarecomp, cc, tmp)
        builder.runtime_objects()

        # ---------------- S1 ----------------------------------------------------------------------------------------------------------
        disc = tmp / "s1" / "disc"
        oracle = common.build_disc(disc, "unvisited", "cooked", "--unvisited-case", "pi")
        found = json.loads(subprocess.run([segarecomp, "sega-cd", "discover-modules", "--cue", str(disc / "disc.cue"), "--out", str(tmp / "s1" / "m.txt")],
                                          capture_output=True, text=True).stdout)
        ranges = {m["name"]: m["range"]["offset"] for m in found["modules"]}
        labels = {name: set(offsets) for name, offsets in oracle["labels"].items()}
        arms = {}
        for arm, flags in ARMS.items():
            entry = {}
            outs = []
            for repeat in range(2):
                c_path = tmp / "s1" / ("%s%d.c" % (arm, repeat))
                rc, out, cpu, rss, wall, err = common.measured([segarecomp, "sega-cd", "emit", "--cue", disc / "disc.cue", "--out", c_path, "--modules",
                                                                tmp / "s1" / "m.txt", "--report-offsets", *flags])
                assert rc == 0, out
                outs.append((json.loads(out), c_path.read_bytes()))
            report, text = outs[0]
            entry["deterministic"] = outs[0][1] == outs[1][1] and outs[0][0] == outs[1][0]
            entry["program_identity"] = report["program_identity"]
            entry["inventory"] = [{k: im[k] for k in ("processor", "image_id", "name", "producer", "mapping", "start", "length", "roots")} for im in report["images"]]
            admitted = {}
            for im in report["images"]:
                admitted.setdefault((im["processor"], im["name"]), set()).update(im["source_offsets"])
            entry["admitted_offsets"] = {"%s:%s" % k: len(v) for k, v in sorted(admitted.items())}
            import hashlib
            entry["admitted_digests"] = {"%s:%s" % k: hashlib.sha256(json.dumps(sorted(v)).encode()).hexdigest()[:16] for k, v in sorted(admitted.items())}
            entry["admitted_total"] = sum(len(v) for v in admitted.values())
            entry["bodies"] = sum(p["bodies"] for p in report["processors"])
            entry["c_bytes"] = len(text)
            recall = {}
            for name, label_set in labels.items():
                got = [v for (proc, n), v in admitted.items() if n == name]
                union = set().union(*got) if got else set()
                recall[name] = {"labels": len(label_set), "admitted": len(label_set & union), "missed": len(label_set - union)}
            entry["recall"] = recall
            obj = tmp / "s1" / (arm + ".o")
            crc, _, ccpu, crss, _, err = common.measured([cc, *common.CFLAGS, "-I", common.SCD_RUNTIME, "-I", common.GENESIS_RUNTIME, "-c", tmp / "s1" / (arm + "0.c"), "-o", obj])
            assert crc == 0, err
            exe = tmp / "s1" / (arm + common.EXE)
            subprocess.run([cc, str(obj), *[str(o) for o in builder.runtime_objects()], "-o", str(exe)], check=True)
            entry["compile_cpu_s"], entry["compile_peak_kib"], entry["object_bytes"] = round(ccpu, 3), crss, obj.stat().st_size
            if "aot_policy" in report:
                a = report["aot_policy"]
                entry["aot_policy"] = {k: a[k] for k in ("requested", "experimental", "effective", "fallback", "reason", "model", "rank_policy", "rank_plan", "weights", "k_sha256",
                                                          "policy_identity", "broad_entries", "admitted_entries", "m3_images", "m3_broad_entries", "m3_admitted_entries", "scope") if k in a}
                entry["images_policy"] = [{k: i[k] for k in ("name", "selection", "selection_reason", "units", "selected", "broad_entries", "admitted_entries")} for i in a.get("images", [])]
            routes = {}
            executed = set()
            for route, d7 in ROUTES.items():
                coverage = tmp / "s1" / ("cov-%s-%s.txt" % (arm, route))
                rc, rep = common.run(exe, disc, 3_000_000, *(["--set", "main:d7=%d" % d7] if d7 else []), "--coverage", str(coverage))
                routes[route] = {"exit": rc, "result": rep["result"], "stop": rep["stop"]["class"], "stop_pc": rep["stop"]["pc"], "retired": rep["cpus"][1]["retired"] + rep["cpus"][0]["retired"],
                                 "words": common.result_words(rep, 8) if "result_block" in rep else None}
                for line in coverage.read_text().splitlines():
                    cpu_index, pc = line.split()[0], int(line.split()[1], 16)
                    executed.add((int(cpu_index), pc))
            entry["routes"] = routes
            entry["executed"] = executed
            arms[arm] = entry
        # executed starts under the reference arm, mapped to (image, offset); retention of every arm
        ref_images = arms["A"]["inventory"]
        def to_source(cpu_index, pc):
            for im in ref_images:
                if (0 if im["processor"] == "main" else 1) == cpu_index and im["start"] <= pc < im["start"] + im["length"]:
                    return (im["name"], pc - im["start"] + (ranges.get(im["name"], 0)))
            return None
        executed_ref = {s for s in (to_source(c, p) for c, p in arms["A"]["executed"]) if s}
        for arm in arms:
            admitted_by_name = {}
            # recompute admitted per image name from the emitted report is not kept; use recall inputs via a second small pass
        for arm, flags in ARMS.items():
            rc, out, *_ = common.measured([segarecomp, "sega-cd", "emit", "--cue", disc / "disc.cue", "--out", tmp / "s1" / ("r-%s.c" % arm), "--modules", tmp / "s1" / "m.txt",
                                           "--report-offsets", *flags])
            admitted = {}
            for im in json.loads(out)["images"]:
                admitted.setdefault(im["name"], set()).update(im["source_offsets"])
            kept = sum(1 for name, off in executed_ref if off in admitted.get(name, set()))
            arms[arm]["retention"] = {"executed_starts": len(executed_ref), "retained": kept}
            arms[arm].pop("executed")
        result["subjects"]["S1"] = {"fixture": "unvisited/pi (discovered module list)", "arms": arms}

        # ---------------- S2 ----------------------------------------------------------------------------------------------------------
        fx = tmp / "s2"
        subprocess.run([sys.executable, str(root / "tools" / "sega_cd_bios_loader_fixture.py"), "build", "--case", "direct", "--out", str(fx)], check=True, capture_output=True)
        s2 = {}
        for arm, flags in (("A", []), ("H", ["--aot-policy", "optimized", "--aot-model", "gen2", "--aot-scope", "all-images"]), ("C", ["--aot-policy", "optimized", "--aot-model", "gen2"])):
            out_dir = tmp / ("s2-" + arm)
            rc, out, cpu, rss, wall, err = common.measured([segarecomp, "sega-cd", "build", "--cue", fx / "disc" / "disc.cue", "--bios", fx / "bios.bin", "--output", out_dir,
                                                            "--runtime-dir", root, "--cc", cc, "--budget", "3000000", "--jobs", "4", *flags])
            status = json.loads((out_dir / "status.json").read_text())
            c_bytes = sum(p.stat().st_size for p in (out_dir / "src").glob("*.c"))
            a = status["aot_policy"]
            s2[arm] = {"status": status["status"], "build_cpu_s": round(cpu, 2), "build_peak_kib": rss, "rounds": status["rounds"], "result": status["result"], "stop": status["stop"],
                       "extents": len(status["extents"]), "frontier": (status["frontier"] or {}).get("refusal"), "c_bytes": c_bytes,
                       "program_identity": status["program_identity"],
                       "aot_policy": {k: a[k] for k in ("requested", "effective", "fallback", "reason", "broad_entries", "admitted_entries", "k_sha256", "m3_images", "m3_broad_entries", "m3_admitted_entries") if k in a},
                       "images": [{k: i[k] for k in ("name", "selection", "selection_reason", "broad_entries", "admitted_entries")} for i in a.get("images", [])]}
        result["subjects"]["S2"] = {"fixture": "bios_loader_fixture/direct (BIOS-image profile, end-to-end build)", "arms": s2}
    json.dump(result, sys.stdout, indent=1, sort_keys=True, default=sorted)
    print()


if __name__ == "__main__":
    main()
