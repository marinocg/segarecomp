#!/usr/bin/env python3
"""ADR 0277: `segarecomp build --z80-static 1` against the unchanged materialization fixed point, through the real consumer route
(analyze, generate, compile, Z80 production/materialize, link) on project-authored fixture ROMs (tools/genesis_z80_fixture_rom.py).

For every fixture the build is run twice, `--z80-static 0` (legacy) and `--z80-static 1`, and the FINAL RESULTS must be identical: the
generated Z80 C and registry (byte for byte), the linked program, the sound/execution summary of a bounded run and the audio status. What the
static producer is allowed to change is only the amount of work, asserted separately and honestly (Z80 compilation generations, discovery
runs), and only where the producer proved something. Groups (separate CTest entries for CI wall-clock):

  equivalence   fully provable shapes: single contiguous upload, A/B/restart/A multi-epoch, the same driver twice, a stub followed by a
                Kosinski-compressed driver, a Kosinski driver; exactly ONE Z80 compilation generation, one discovery run, the verification run;
                a partial proof (A proven, a computed B refused): the materializer discovers B, order preserved
  fallback      predictions the verification run contradicts (an image never activated inside the observation window, a region-gated upload
                the observation never reaches, a structural Z80 mutation, whose static snapshot cannot be verified outside the written extents) fall back to the unchanged fixed point
                with a typed reason; refused shapes (unknown length/pointer, beyond-RAM output, unrecognised decompressor, a valid stream that is
                not executable authority, computed upload) leave the fixed point untouched; a structural Z80 mutation keeps its typed degraded
                audio outcome
  determinism   repeated static builds with different worker counts are byte-identical

usage: genesis_z80_static_first_build_test.py <segarecomp> <cc> <cxx> <source-root> [group ...]
"""
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile

cli, cc, cxx, root = sys.argv[1], sys.argv[2], sys.argv[3], pathlib.Path(sys.argv[4]).resolve()
sys.path.insert(0, str(root / "tools"))
import genesis_z80_fixture_rom as fx  # noqa: E402

ALL_GROUPS = ("equivalence", "fallback", "determinism")
GROUPS = tuple(sys.argv[5:]) or ALL_GROUPS
assert all(g in ALL_GROUPS for g in GROUPS), GROUPS
failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def build(rom_bytes, out, static, jobs=None):
    out.mkdir(parents=True, exist_ok=True)
    rom = out.parent / (out.name + ".md")
    rom.write_bytes(rom_bytes)
    command = [cli, "build", "--rom", str(rom), "--output", str(out), "--cc", cc, "--cxx", cxx, "--runtime-dir", str(root / "platforms" / "genesis"),
               "--optimize", "0", "--runtime-optimize", "1", "--z80-static", "1" if static else "0"]
    if jobs:
        command += ["--jobs", str(jobs)]
    done = subprocess.run(command, text=True, capture_output=True, timeout=1800)
    status = json.loads((out / "status.json").read_text()) if (out / "status.json").exists() else {}
    return done, status


def tree_digest(directory):
    digest = hashlib.sha256()
    for path in sorted(p for p in pathlib.Path(directory).rglob("*") if p.is_file()):
        digest.update(path.relative_to(directory).as_posix().encode() + b"\0" + path.read_bytes())
    return digest.hexdigest()


def program(out):
    return next((p for p in out.iterdir() if p.stem == "game" and p.is_file()), None)


def run_summary(exe):
    env = {"SEGARECOMP_SOUND_SUMMARY": "1", "PATH": "/usr/bin:/bin"}
    done = subprocess.run([str(exe), "--instruction-budget", "3000000"], text=True, capture_output=True, timeout=300, env=env)
    summary = re.search(r"SOUND_SUMMARY (\{.*\})", done.stderr)
    return summary.group(1) if summary else None


# name -> (static outcome, fallback, images the static registry keeps, expected Z80 generations of the static build or None)
EQUIVALENCE = {
    "sound_raw": ("complete", "none", 1, 1),
    "sound_multi_epoch": ("complete", "none", 2, 1),     # A, B, restart (empty hold window), A again: stable ordinals
    "static_two_phase": ("complete", "none", 2, 1),      # A, B, A, restart
    "static_kos": ("complete", "none", 1, 1),            # the recognised reference decompressor, stream decoded at generation time
    "static_kos_staged": ("complete", "none", 1, 1),     # the Sega-family two-stage loader: decode into a work-RAM buffer, counted copy to the Z80 RAM
    "static_stub_driver": ("complete", "none", 2, 1),    # stub, then the driver over it, then the driver again
    "static_partial": ("partial", "none", 1, 2),         # A proven; the computed B is discovered by the unchanged fixed point
    "static_ram_count": ("partial", "none", 1, 2),       # the data-dependent second upload is refused and discovered
}
FALLBACK = {
    "sound_late_epoch": ("complete", "image_never_activated"),                         # the second driver is uploaded after the window
    "sound_domestic_gated_overseas_header": ("complete", "image_never_activated"),     # the region-gated upload is not reached as USA
    "sound_smc_fault": ("complete", "sound_fault_requires_observed_registry"),    # an isolated sound CPU is only verifiable against the observed registry
    "static_kos_unrecognised": ("none", "none"),                                       # unrecognised decompressor: nothing proven, nothing changed
    "static_kos_staged_clobbered": ("none", "none"),                                   # the staged buffer is not a proven source after an unresolved store
    "static_ram_source": ("none", "none"),
    "static_ram_destination": ("none", "none"),
    "static_raw_beyond_ram": ("none", "none"),
    "static_kos_beyond_ram": ("none", "none"),
    "sound_decoded": ("partial", "none"),                                              # computed upload of the same bytes: same signature
    "static_kos_work_ram": ("complete", "none"),                                       # a valid stream in WORK RAM is not executable authority
    "sound_smc": ("complete", "sound_fault_requires_observed_registry"),                # structural mutation: legacy registry is the reference; typed degraded outcome kept
}


def compare(name, tmp, expected_outcome, expected_fallback, expected_kept=None, expected_generations=None):
    rom = fx.build(name)
    legacy_done, legacy = build(rom, tmp / (name + "-legacy"), False)
    static_done, static = build(rom, tmp / (name + "-static"), True)
    check(legacy_done.returncode == 0 and static_done.returncode == 0, "%s: both builds succeed" % name)
    if legacy_done.returncode != 0 or static_done.returncode != 0:
        return
    lz, sz = legacy["z80"], static["z80"]
    info = sz.get("static")
    check(info is not None and "static" not in lz, "%s: the static block is present only with --z80-static 1" % name)
    check(info["outcome"] == expected_outcome, "%s: producer outcome %s (got %s)" % (name, expected_outcome, info["outcome"]))
    check(info["fallback"] == expected_fallback, "%s: fallback %s (got %s)" % (name, expected_fallback, info["fallback"]))
    log = (tmp / (name + "-static") / "build.log").read_text()
    check(("CONTRADICTED" in log) == (expected_fallback != "none"), "%s: a contradiction is logged iff the static registry was discarded" % name)
    if expected_kept is not None:
        check(info["images_kept"] == expected_kept, "%s: %d image(s) kept (got %d)" % (name, expected_kept, info["images_kept"]))
    # the results are the legacy results
    check(sz["images"] == lz["images"] and sz["epochs"] == lz["epochs"] and sz["end"] == lz["end"], "%s: same images/epochs/end as the legacy fixed point" % name)
    check(static.get("genesis_audio") == legacy.get("genesis_audio") and static.get("z80_audio_outcome") == legacy.get("z80_audio_outcome"),
          "%s: same audio capability status (%s)" % (name, static.get("genesis_audio")))
    check(tree_digest(tmp / (name + "-legacy") / "generated-z80") == tree_digest(tmp / (name + "-static") / "generated-z80"),
          "%s: generated Z80 C and registry are byte-identical" % name)
    check(program(tmp / (name + "-legacy")).read_bytes() == program(tmp / (name + "-static")).read_bytes(), "%s: the linked program is byte-identical" % name)
    a, b = run_summary(program(tmp / (name + "-legacy"))), run_summary(program(tmp / (name + "-static")))
    check(a is not None and a == b, "%s: sound/execution summary of a bounded run is identical" % name)
    # the work done
    if expected_generations is not None:
        check(sz["prepare_generations"] == expected_generations, "%s: Z80 compilation generations %d (got %d)" % (name, expected_generations, sz["prepare_generations"]))
        check(sz["prepare_generations"] < lz["prepare_generations"], "%s: fewer Z80 generations than legacy (%d < %d)" % (name, sz["prepare_generations"], lz["prepare_generations"]))
    if expected_fallback != "none":
        check(sz["prepare_generations"] > lz["prepare_generations"], "%s: a contradicted prediction costs extra work (honestly reported: %d vs %d)" % (name, sz["prepare_generations"], lz["prepare_generations"]))
    elif info["outcome"] == "none":
        check(sz["prepare_generations"] == lz["prepare_generations"], "%s: nothing proven: the same Z80 work as legacy" % name)
    # provenance of what was kept
    images = static["executable_images"]["z80"]
    kept = info["images_kept"]
    check(images["authority"]["static_proof"] == kept and images["authority"]["bounded_build_time_materialization"] == sz["images"] - kept,
          "%s: provenance counts: %d static_proof, %d observed" % (name, kept, sz["images"] - kept))
    print("METRIC %s outcome=%s fallback=%s kept=%d images=%d gens_static=%d gens_legacy=%d disc_static=%d disc_legacy=%d" %
          (name, info["outcome"], info["fallback"], kept, sz["images"], sz["prepare_generations"], lz["prepare_generations"], sz["discovery_runs"], lz["discovery_runs"]))
    return legacy, static


def main():
    with tempfile.TemporaryDirectory(prefix="segarecomp-z80-static-first-") as directory:
        tmp = pathlib.Path(directory)
        if "equivalence" in GROUPS:
            for name, (outcome, fallback, kept, generations) in sorted(EQUIVALENCE.items()):
                result = compare(name, tmp, outcome, fallback, kept, generations)
                if result and outcome == "complete":
                    _, static = result
                    z = static["z80"]
                    check(z["discovery_runs"] == 1 and z["verification_runs"] == 1 and z["units_reused"] == 0,
                          "%s: no discovery recompilation: one discovery run, one verification run, no reused unit" % name)
        if "fallback" in GROUPS:
            for name, (outcome, fallback) in sorted(FALLBACK.items()):
                compare(name, tmp, outcome, fallback)
            smc = build(fx.build("sound_smc"), tmp / "smc-check", True)[1]
            check(smc.get("genesis_audio") == "degraded" and smc.get("z80_audio_outcome") == "structural_code_mismatch",
                  "a structural Z80 mutation still isolates only the sound CPU (typed degraded outcome, build succeeds)")
        if "determinism" in GROUPS:
            digests = []
            for jobs in (1, 4, 8, 4):
                out = tmp / ("det-%d-%d" % (jobs, len(digests)))
                done, status = build(fx.build("static_stub_driver"), out, True, jobs)
                check(done.returncode == 0, "determinism: jobs=%d builds" % jobs)
                z = dict(status["z80"])
                for key in ("emit_ms", "compile_ms", "materialize_ms", "units_compiled", "units_reused"):
                    z.pop(key, None)
                z["static"] = {k: v for k, v in z["static"].items() if k != "ms"}
                digests.append((tree_digest(out / "generated-z80"), hashlib.sha256(program(out).read_bytes()).hexdigest(), json.dumps(z, sort_keys=True)))
            check(len(set(digests)) == 1, "determinism: byte-identical generated Z80 C, program and sanitized aggregates for jobs 1/4/8/4")
    if failures:
        print("FAILED: %d check(s)" % len(failures))
        return 1
    print("all static-first build checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
