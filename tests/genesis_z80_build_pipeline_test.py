#!/usr/bin/env python3
"""SEG-032-T008 (ADR 0073): `segarecomp build` of a Genesis image runs the build-time Z80 materialization fixed point and links
one static executable with the generated-native Z80 sound program(s), with no manual step.

Project-authored synthetic ROMs only (tools/genesis_z80_fixture_rom.py). Proved through the real consumer route (analyze, generate,
compile, materialize, link):
  * a multi-epoch ROM converges with the exact image count (2 images, 4 epochs: A, B, a plain restart, A again) and the produced
    program runs the Z80 generated-native (PSG and YM2612 traffic from the Z80); a raw upload and a computed ("decompressed")
    upload of the same bytes are one image;
  * independent builds with different worker counts produce byte-identical generated Z80 C and registry, and equal aggregates;
  * fail closed with a typed outcome and NO executable: image bound exceeded, instruction-budget exhaustion (a guest that never
    advances virtual time);
  * a structural Z80 code mutation (self-modifying code, z80_code_mismatch) does NOT fail the build: the sound capability is classified
    structurally unsupported (status `genesis_audio = degraded`, `z80_audio_outcome = structural_code_mismatch`), Z80 discovery stops, and
    the final executable disables only its Z80 sound path (no later Z80 write, a later epoch neither runs nor stops it) while the 68K
    keeps running; a fully supported title is `genesis_audio = supported`;
  * an epoch after the observation window is not materialized and surfaces at run time as the typed z80_unknown_image;
  * falsification of the produced program: a registry with one image removed stops at that image's epoch (z80_unknown_image), one
    image's emitted code bytes mutated isolates the Z80 (typed fault, 68K continues), and a pipeline rebuild restores the full behaviour.
Sanitized aggregates (counts, ms, bytes) are printed as METRIC lines; no ROM byte, address or hash is.

Case selection (CI wall-clock): the cases are partitioned into independent groups, each with its own builds, so CTest can run them as
separate entries in parallel (tests/CMakeLists.txt registers one per group; the union is the whole matrix):
  epochs   multi-epoch convergence, raw/computed upload, determinism across repeats and worker counts, supported-audio status
  bound    the real image-bound failure (bound + 1 distinct images) and the instruction-budget exhaustion; the converging "exactly the
           bound" and the registry-level bound logic are proved by genesis_z80_materialization_test against scripted pass runners
  smc      self-modifying code, the sound-fault isolation and degraded status, and the epoch after the observation window
  falsify  removal / mutation / relink / rebuild falsification of the produced program
  prepare  prepare() failure diagnostics (POSIX wrapper compiler)

usage: genesis_z80_build_pipeline_test.py <segarecomp> <cc> <cxx> <source-root> [group ...]   (default: every group)
"""
import concurrent.futures
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

ALL_GROUPS = ("epochs", "bound", "smc", "falsify", "prepare")
GROUPS = tuple(sys.argv[5:]) or ALL_GROUPS
assert all(g in ALL_GROUPS for g in GROUPS), GROUPS
failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def build(rom_bytes, out, jobs=None, keep=False, compiler=None):
    out.mkdir(parents=True, exist_ok=True)
    rom = out.parent / (out.name + ".md")
    rom.write_bytes(rom_bytes)
    command = [cli, "build", "--rom", str(rom), "--output", str(out), "--cc", compiler or cc, "--cxx", cxx, "--runtime-dir",
               str(root / "platforms" / "genesis"), "--optimize", "0", "--runtime-optimize", "1"]
    if jobs:
        command += ["--jobs", str(jobs)]
    if keep:
        command += ["--keep-work", "1"]
    done = subprocess.run(command, text=True, capture_output=True, timeout=1800)
    status = json.loads((out / "status.json").read_text()) if (out / "status.json").exists() else {}
    return done, status


def executable(out):
    return next((p for p in out.iterdir() if p.stem == "game" and p.is_file()), None)


def run_program(exe, budget=3_000_000):
    done = subprocess.run([str(exe), "--instruction-budget", str(budget)], text=True, capture_output=True, timeout=300,
                          env={"SEGARECOMP_SOUND_SUMMARY": "1", "PATH": "/usr/bin:/bin"})
    summary = re.search(r"SOUND_SUMMARY (\{.*\})", done.stderr)
    return done, (json.loads(summary.group(1)) if summary else {})


def tree_digest(directory):
    digest = hashlib.sha256()
    for path in sorted(p for p in pathlib.Path(directory).rglob("*") if p.is_file()):
        digest.update(path.relative_to(directory).as_posix().encode() + b"\0" + path.read_bytes())
    return digest.hexdigest()


def stable(status):
    z = dict(status["z80"])
    for key in ("emit_ms", "compile_ms", "materialize_ms", "units_compiled", "units_reused"):
        z.pop(key, None)
    return z


def failing_compiler(directory, mode):
    """A POSIX wrapper around the real compiler that fails one operation with a synthetic diagnostic: `unit-compile` fails the
    compile of a generated Z80 unit, `pass-link` fails the link of the materialization pass program."""
    wrapper = directory / ("cc-fail-" + mode)
    pattern = "*/z80-*.o" if mode == "unit-compile" else "*/materialize-pass"
    wrapper.write_text('#!/bin/sh\nfor a in "$@"; do\n  case "$a" in %s) echo "synthetic-%s-diagnostic: scripted failure" >&2; exit 1;; esac\ndone\nexec "%s" "$@"\n' % (pattern, mode, cc))
    wrapper.chmod(0o755)
    return str(wrapper)


def main():
    with tempfile.TemporaryDirectory(prefix="segarecomp-z80-pipeline-") as directory:
        tmp = pathlib.Path(directory)
        multi, decoded, raw = fx.build("sound_multi_epoch"), fx.build("sound_decoded"), fx.build("sound_raw")
        late, smc = fx.build("sound_late_epoch"), fx.build("sound_smc")
        smc_fault = fx.build("sound_smc_fault")
        many_over = fx.sound_many(fx.MAX_IMAGES_FOR_TESTS + 1) if "bound" in GROUPS else None
        spin_no_time = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x60, 0xFE))  # never advances virtual time
        by_group = {
            "epochs": {"multi": (multi, None), "multi_j1": (multi, 1), "decoded": (decoded, None)},
            "bound": {"over": (many_over, None), "budget": (spin_no_time, None)},
            "smc": {"smc": (smc, None), "smc_fault": (smc_fault, None), "smc_fault2": (smc_fault, 1), "late": (late, None)},
            "falsify": {"raw": (raw, 2)},
            "prepare": {},
        }
        jobs = {name: job for group in GROUPS for name, job in by_group[group].items()}
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            futures = {name: pool.submit(build, rom, tmp / name, j, name == "raw") for name, (rom, j) in jobs.items()}
            results = {name: f.result() for name, f in futures.items()}

        if "epochs" in GROUPS:
            # ---- multi-epoch: exact image count, no manual step, the Z80 runs generated-native ----
            done, status = results["multi"]
            z = status.get("z80", {})
            check(done.returncode == 0 and status.get("status") == "ok", "multi-epoch ROM builds through the normal route")
            check(z.get("images") == 2 and z.get("epochs") == 4 and z.get("outcome") == "converged", "exactly 2 images materialized from 4 epochs (A, B, restart, A again)")
            check(z.get("runs") == z.get("discovery_runs", 0) + 1 and z.get("discovery_runs") == 3,
                  "convergence: 3 discovery runs (2 unknown images + the completing run) and one confirming run")
            check(z.get("window_frames", 0) >= 600 and z.get("frames_reached") == z.get("window_frames"), "the observation window covers at least 600 virtual frames and was fully observed")
            for stage in ("analyze", "generate", "compile", "z80-materialize", "link"):
                check("@stage %s begin" % stage in done.stdout and "@stage %s done" % stage in done.stdout, "stage %s reported" % stage)
            exe = executable(tmp / "multi")
            check(exe is not None, "the final static executable exists")
            ran, summary = run_program(exe)
            check(summary.get("epochs") == 4 and summary.get("bound_image") in (1, 2) and summary.get("psg_writes", 0) > 0 and summary.get("ym_writes", 0) > 0,
                  "the produced program executes the Z80 generated-native: epochs observed, PSG and YM2612 written by the Z80")
            check(not (tmp / "multi" / "obj").exists(), "intermediate objects removed")

            # ---- raw vs computed upload: identical activation signature ----
            done, status = results["decoded"]
            z = status.get("z80", {})
            check(done.returncode == 0 and z.get("images") == 1 and z.get("epochs") == 2, "a raw upload and a computed (decoded) upload of the same bytes are one image")

            # ---- determinism: repeats and worker counts ----
            digests = {name: tree_digest(tmp / name / "generated-z80") for name in ("multi", "multi_j1")}
            check(len(set(digests.values())) == 1, "generated Z80 C and the registry are byte-identical across independent builds with worker counts 1 and default (default is the host core count, capped at 8)")
            check(len({json.dumps(stable(results[n][1]), sort_keys=True) for n in digests}) == 1, "aggregates (images, epochs, runs, sizes) are identical across builds")
            check(len({tree_digest(tmp / n / "generated") for n in digests}) == 1, "the generated M68K C is byte-identical across builds as well")

            # ---- supported-audio status ----
            check(results["multi"][1].get("genesis_audio") == "supported" and "z80_audio_outcome" not in results["multi"][1] and "genesis_audio=degraded" not in results["multi"][0].stdout,
                  "a fully supported title is genesis_audio = supported, never degraded")

        if "bound" in GROUPS:
            # ---- fail closed ----
            done, status = results["over"]
            check(done.returncode != 0 and status.get("diagnostic") == "z80_image_bound_exceeded" and executable(tmp / "over") is None,
                  "bound + 1 distinct images: z80_image_bound_exceeded, no executable")
            check(status.get("z80", {}).get("images") == fx.MAX_IMAGES_FOR_TESTS, "the failure leaves exactly the bound registered")
            done, status = results["budget"]
            check(done.returncode == 4 and "stage=z80-materialize" in done.stdout and status.get("diagnostic") == "materialization_budget_exhausted" and
                  executable(tmp / "budget") is None,
                  "a guest that never advances virtual time exhausts the instruction budget: exit 4 in the z80-materialize stage, materialization_budget_exhausted, no executable")

        if "smc" in GROUPS:
            # ---- structural Z80 code mutation: degraded sound, never a failed build ----
            done, status = results["smc"]
            check(done.returncode == 0 and status.get("status") == "ok" and executable(tmp / "smc") is not None and status.get("genesis_audio") == "degraded" and
                  status.get("z80_audio_outcome") == "structural_code_mismatch" and "genesis_audio=degraded z80_audio_outcome=structural_code_mismatch" in done.stdout,
                  "self-modifying Z80 code (z80_code_mismatch): the build succeeds and reports genesis_audio = degraded / structural_code_mismatch")
            done, status = results["smc_fault"]
            z = status.get("z80", {})
            check(done.returncode == 0 and status.get("genesis_audio") == "degraded" and z.get("images") == 1 and z.get("epochs") == 2 and
                  z.get("sound_fault_epochs") == 1 and z.get("frames_reached") == z.get("window_frames"),
                  "the fault stops Z80 discovery: the later epoch is not materialized (1 image, 2 epochs) and the whole window is still observed")
            check(stable(status) == stable(results["smc_fault2"][1]) and tree_digest(tmp / "smc_fault" / "generated-z80") == tree_digest(tmp / "smc_fault2" / "generated-z80"),
                  "the degraded outcome is deterministic across builds and worker counts (same fault epoch, frame and registry)")
            ran, summary = run_program(executable(tmp / "smc_fault"), budget=6_000_000)
            check(summary.get("result_kind") == 3 and summary.get("sound_fault", 0) != 0 and summary.get("sound_fault_epoch") == 1 and summary.get("epochs") == 2 and
                  summary.get("bound_image") == 0,
                  "the degraded program runs the 68K to the instruction budget; the faulted Z80 is isolated (no stop, nothing bound, 2 epochs seen)")
            check(summary.get("psg_writes") == 1 and summary.get("ym_writes") == 0,
                  "no Z80-originated device write after the fault: the pre-fault PSG write only, the later driver never runs")
            ran2, summary2 = run_program(executable(tmp / "smc_fault"), budget=6_000_000)
            check(summary2 == summary, "the degraded program is deterministic across runs (summary, audio digest)")


            # ---- epochs after the window surface at run time as the typed unknown image ----
            done, status = results["late"]
            check(done.returncode == 0 and status.get("z80", {}).get("images") == 1 and status["z80"].get("epochs") == 1,
                  "an epoch after the observation window is not materialized (1 image)")
            ran, summary = run_program(executable(tmp / "late"), budget=12_000_000)  # past the 600-frame observation window
            check('"stop_class":"unsupported_z80_execution"' in ran.stdout and '"diagnostic_category":"z80_unknown_image"' in ran.stdout,
                  "the late epoch stops at run time with the typed z80_unknown_image (no decoding, no fallback)")

        if "falsify" in GROUPS:
            # ---- falsification of the produced program: removal, mutation, rebuild ----
            work = tmp / "raw" / "obj"
            done, status = results["raw"]
            check(done.returncode == 0 and work.is_dir(), "kept-work build available for the falsification")
            z80_dir = tmp / "raw" / "generated-z80"
            flags = ["-std=c11", "-O0", "-I", str(root / "platforms/genesis/runtime"), "-I", str(root / "libs/codegen/c11/include"), "-I", str(z80_dir)]

            def object_of(path):
                return work / ("z80-" + hashlib.sha256(path.read_bytes()).hexdigest()[:20] + ".o")

            def relink(replacements, name):
                out = tmp / name
                units = [z80_dir / line for line in (z80_dir / "genesis_z80.units").read_text().split()]
                final_z80 = {object_of(u).name for u in units}
                skip = {"hook-materialize.o"} | {object_of(p).name for p in replacements}
                # the stable objects plus the final emission's Z80 objects (earlier iterations' objects are left behind in obj/)
                objects = sorted(o for o in work.glob("*.o") if o.name not in skip and (not o.name.startswith("z80-") or o.name in final_z80))
                extra = []
                for source, text in replacements.items():
                    mutated = tmp / (name + "-" + source.name)
                    mutated.write_text(text)
                    obj = tmp / (name + "-" + source.name + ".o")
                    subprocess.run([cc, *flags, "-c", str(mutated), "-o", str(obj)], check=True, capture_output=True)
                    extra.append(obj)
                linked = subprocess.run([cc, "-o", str(out), *map(str, objects), *map(str, extra)], text=True, capture_output=True)
                assert linked.returncode == 0, linked.stderr[-2000:]
                return out

            registry = z80_dir / "genesis_z80_registry.c"
            removed = relink({registry: re.sub(r"index < 1u", "index < 0u", registry.read_text())}, "removed")
            ran, _ = run_program(removed)
            check('"diagnostic_category":"z80_unknown_image"' in ran.stdout, "a registry with the image removed stops at its epoch: z80_unknown_image")
            mutated_sources = {}
            for unit in sorted(z80_dir.glob("genesis_z80_owner_*.c")):
                text = unit.read_text()
                changed = text.replace("z80_live_guard(rt, 0x0000u, 3u, 1u, 49u,", "z80_live_guard(rt, 0x0000u, 3u, 1u, 50u,")
                if changed != text:
                    mutated_sources[unit] = changed
            check(len(mutated_sources) >= 1, "the first instruction's guarded bytes were found in the emitted image code")
            mutated = relink(mutated_sources, "mutated")
            ran, summary = run_program(mutated)
            check(summary.get("result_kind") == 3 and summary.get("sound_fault", 0) != 0,
                  "an image whose emitted bytes are mutated isolates the Z80 (typed fault) and the machine runs on")
            control = relink({}, "control")
            ran, summary = run_program(control)
            check(summary.get("epochs") == 1 and summary.get("result_kind") == 3 and summary.get("ym_writes", 0) > 0,
                  "control: the unmodified objects relinked run the Z80 to the budget (the comparison discriminates)")
            ran, summary = run_program(executable(tmp / "raw"))
            check(summary.get("epochs") == 1 and summary.get("result_kind") == 3 and summary.get("ym_writes", 0) > 0 and summary.get("sound_fault", 0) == 0,
                  "the pipeline's own (never relinked) executable shows the control behaviour, so the mutated/removed variants differ from it")

        for name in ("multi",) if "epochs" in GROUPS else ():
            z = results[name][1].get("z80", {})
            print("METRIC %s %s" % (name, json.dumps({k: z.get(k) for k in ("images", "epochs", "discovery_runs", "runs", "units", "generated_bytes",
                                                                        "object_bytes", "emit_ms", "compile_ms", "materialize_ms", "executable_bytes")}, sort_keys=True)))
    # ---- prepare() failure reports the failed operation and keeps the diagnostic (POSIX wrapper compiler) ----
    if "prepare" in GROUPS and sys.platform != "win32":
        with tempfile.TemporaryDirectory(prefix="segarecomp-z80-prepare-fail-") as directory:
            tmp = pathlib.Path(directory)
            for mode in ("unit-compile", "pass-link"):
                done, status = build(fx.build("sound_raw"), tmp / mode, compiler=failing_compiler(tmp, mode))
                log = (tmp / mode / "build.log").read_text(errors="replace")
                check(done.returncode != 0 and status.get("diagnostic") == "z80_image_compile_failed" and executable(tmp / mode) is None,
                      "%s failure: typed z80_image_compile_failed, no executable" % mode)
                check(("message=z80_image_compile_failed stage=%s" % mode) in done.stdout and ("(%s)" % mode) in status.get("message", ""),
                      "%s failure: the failed stage is in @result and status.json" % mode)
                check(("z80 prepare failure: stage=%s" % mode) in log and ("synthetic-%s-diagnostic" % mode) in log.split("z80 prepare failure:", 1)[-1],
                      "%s failure: the stage and the bounded compiler/link diagnostic are in build.log" % mode)

    print("genesis z80 build pipeline: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
