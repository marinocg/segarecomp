#!/usr/bin/env python3
"""`segarecomp build`: native consumer route ROM -> generated C -> C compiler -> executable, no Python tooling.

Project-authored synthetic Genesis image only. Proves the success path (machine-readable @stage/@result lines,
status.json, build.log, runnable output with the sanitized stop report), and that failures exit non-zero with a
stage-attributed result and a preserved diagnostic log.
"""
import hashlib
import pathlib
import subprocess
import sys
import tempfile

# Reset SSP, reset PC=8, MOVEQ #0,D0 then RESET: a deterministic unsupported-instruction stop.
IMAGE = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def build(cli, compiler, root, rom, out, cc=None, extra=()):
    return subprocess.run([cli, "build", "--rom", str(rom), "--output", str(out), "--cc", cc or compiler,
                           "--runtime-dir", str(root / "platforms" / "genesis"), "--optimize", "0", "--runtime-optimize", "1", *extra],
                          text=True, capture_output=True)


def main():
    cli, compiler, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
    digest = hashlib.sha256(IMAGE).hexdigest()
    with tempfile.TemporaryDirectory(prefix="segarecomp-build-command-") as directory:
        tmp = pathlib.Path(directory)
        rom = tmp / "synthetic.bin"
        rom.write_bytes(IMAGE)

        out = tmp / "ok"
        result = build(cli, compiler, root, rom, out)
        require(result.returncode == 0, "build must succeed: " + result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        for stage in ("analyze", "generate", "compile", "link"):
            require(f"@stage {stage} begin" in lines and f"@stage {stage} done" in lines, f"missing stage {stage}")
        require(lines[-1].startswith("@result ok executable="), "final line must be the result")
        require('"status":"ok"' in (out / "status.json").read_text(), "status.json must record success")
        require(digest in (out / "status.json").read_text(), "status.json must carry the ROM digest")
        require("cc=" in (out / "build.log").read_text(), "build.log must record the compiler")
        require(not (out / "obj").exists(), "intermediate objects must be removed")
        # SEG-047 (ADR 0096): the machine-readable AOT policy member is always present; Compatibility is the default.
        import json
        compat = json.loads((out / "status.json").read_text())["aot_policy"]
        require(compat["requested"] == "compatibility" and compat["effective"] == "broad" and compat["fallback"] is False and
                compat["reason"] == "none" and len(compat["identity"]) == 64, "default build must report the Compatibility policy: " + str(compat))
        opt = build(cli, compiler, root, rom, tmp / "opt", extra=("--aot-policy", "optimized"))
        require(opt.returncode == 0, "optimized build must succeed: " + opt.stdout + opt.stderr)
        policy = json.loads((tmp / "opt" / "status.json").read_text())["aot_policy"]
        require(policy["requested"] == "optimized" and policy["effective"] in ("ml_region", "broad"), "optimized status: " + str(policy))
        require(policy["fallback"] == (policy["effective"] == "broad") and policy["identity"] != compat["identity"],
                "Compatibility and Optimized identities must never alias, a fallback must be visible: " + str(policy))
        if policy["effective"] == "ml_region":
            require(policy["model"] == "seg046-features-v1" and policy["validator"] == "accepted" and policy["k"] <= policy["universe"] and
                    len(policy["k_sha256"]) == 64 and len(policy["schema_sha256"]) == 64, "ML metrics: " + str(policy))
        else:
            require(policy["reason"] in ("model_identity", "rom_size", "empty_proposal", "prune_rejected", "validator_rejected", "no_analysis"),
                    "fallback reason must be a stable code: " + str(policy))
        require(subprocess.run([cli, "build", "--rom", str(rom), "--output", str(tmp / "bad"), "--cc", compiler, "--runtime-dir",
                                str(root / "platforms" / "genesis"), "--aot-policy", "turbo"], text=True, capture_output=True).returncode == 2,
                "an unknown policy is a usage error")
        # Exact-map precedence: an explicit admission plan outranks the optimized producer and is reported as such.
        regions = tmp / "x.regions"
        regions.write_bytes(f"segarecomp.m68k_executable_regions.v1\nrom_sha256 {digest}\nrange 00000000 0000000c\nend\n".encode())
        plan = tmp / "x.plan"
        planned = subprocess.run([cli, "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry", "--rom-sha256", digest,
                                  "--immutable-rom-aot", "--immutable-aot-region-proposal", str(regions), "--region-admission-plan-output", str(plan)],
                                 text=True, capture_output=True)
        require(planned.returncode == 0, "region plan for the precedence check: " + planned.stderr)
        precedence = build(cli, compiler, root, rom, tmp / "prec", extra=("--aot-policy", "optimized", "--admission-plan", str(plan)))
        require(precedence.returncode == 0, "plan + optimized build: " + precedence.stdout + precedence.stderr)
        won = json.loads((tmp / "prec" / "status.json").read_text())["aot_policy"]
        require(won["effective"] == "admission_plan" and won["reason"] == "exact_plan_precedence" and won["fallback"] is False,
                "an explicit plan must outrank the ML producer: " + str(won))
        executable = next(p for p in out.iterdir() if p.stem == "game")
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        require('"result":"stop"' in ran.stdout and digest in ran.stdout, "the native program must emit the sanitized stop report")

        # Instruction budget: an explicit budget is one finite run; with no flag the program is unbounded
        # (it must still be running after a moment (1.5 s) on a guest that never stops).
        # A real spin loop (every retired instruction advances virtual time), so the build-time Z80 materialization pass ends at its
        # observation window like any program that never stops. The prologue (MOVEQ, BEQ, RESET) keeps the 68K translator's startup
        # prefix to the operations it admits; the loop itself is compiled by the immutable-ROM AOT.
        sys.path.insert(0, str(root / "tools"))
        import genesis_z80_fixture_rom as fx
        spin = fx.M68k()
        spin.w(0x7000)
        spin.branch(0x67, "real")
        spin.w(0x4E70)
        spin.label("real")
        spin.label("spin")
        spin.branch(0x60, "spin")
        loop = tmp / "loop.bin"
        loop.write_bytes(fx.build_rom(spin.resolve()))
        result = build(cli, compiler, root, loop, tmp / "loop-out")
        require(result.returncode == 0, "loop build must succeed: " + result.stdout + result.stderr)
        loop_exe = next(p for p in (tmp / "loop-out").iterdir() if p.stem == "game")
        # (The program has an unmodelled-instruction frontier, so its sanitized stdout report is not written for a resource limit -- an
        # existing property of partial programs; the sound hook's opt-in counter line names the runner result instead.)
        bounded = subprocess.run([str(loop_exe), "--instruction-budget", "1000"], text=True, capture_output=True, timeout=60,
                                 env={"SEGARECOMP_SOUND_SUMMARY": "1", "PATH": "/usr/bin:/bin"})
        require('"result_kind":3' in bounded.stderr, "an explicit budget must end in the runner resource limit: " + bounded.stderr)
        try:
            subprocess.run([str(loop_exe)], text=True, capture_output=True, timeout=1.5)
            require(False, "without --instruction-budget the program must keep running")
        except subprocess.TimeoutExpired:
            pass

        # A guest that never advances virtual time can never finish the Z80 observation window: that build-time failure (exit 4, the
        # z80-materialize stage, materialization_budget_exhausted, no executable) is proved end to end by genesis_z80_build_pipeline_test
        # (group `bound`); only the ROM is needed here, for the hook regression below.
        stuck = tmp / "stuck.bin"
        stuck.write_bytes(bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x60, 0xFE)))

        # Regression: a viewer window close / finished capture reports a runner resource limit with a count below
        # UINT32_MAX and must END the unbounded default run, never re-enter the runner (which reopened the window).
        # No window is involved: the runner is replaced by a stub that fails if it is called a second time.
        gen = tmp / "hook-gen.c"
        emit = subprocess.run([cli, "emit-general-startup-bridge-c", "--rom", str(stuck), "--reset-entry", "--rom-sha256",
                               hashlib.sha256(stuck.read_bytes()).hexdigest(), "--immutable-rom-aot",
                               "--generated-c-output", str(gen)], text=True, capture_output=True)
        require(emit.returncode == 0 and gen.is_file(), "emit for the hook regression must succeed: " + emit.stderr)
        runtime_dir = root / "platforms" / "genesis" / "runtime"
        (tmp / "proto.h").write_text('#include "runtime.h"\nGenesisControlTransfer test_hook(GenesisRuntime *, GenesisDispatchFunction, uint32_t);\n')
        (tmp / "hook.c").write_text(
            '#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include "proto.h"\nstatic int calls;\n'
            'GenesisControlTransfer test_hook(GenesisRuntime *r, GenesisDispatchFunction d, uint32_t a) {\n'
            '  GenesisControlTransfer t; (void)d; (void)a; memset(&t, 0, sizeof t); calls++;\n'
            '  if (calls > 1) { fputs("RESTARTED\\n", stderr); exit(9); }\n'
            '  t.kind = GENESIS_RUNNER_RESOURCE_LIMIT; t.next_pc = r->pc; t.runner_dispatch_count = 7U; return t; }\n')
        hooked = tmp / "hooked"
        # Only the generated main is compiled with the rename; runtime.c and the stub keep their own definitions.
        objects = []
        for name, src, extra in (("gen", gen, ["-include", str(tmp / "proto.h"), "-Dgenesis_runtime_run=test_hook"]),
                                 ("hook", tmp / "hook.c", []), ("rt", runtime_dir / "runtime.c", [])):
            obj = tmp / (name + ".o")
            built = subprocess.run([compiler, "-std=c11", "-I", str(runtime_dir), "-I", str(tmp), *extra, "-c", str(src),
                                    "-o", str(obj)], text=True, capture_output=True)
            require(built.returncode == 0, f"hook build ({name}) must succeed: " + built.stderr)
            objects.append(str(obj))
        linked = subprocess.run([compiler, *objects, "-o", str(hooked)], text=True, capture_output=True)
        require(linked.returncode == 0, "hook link must succeed: " + linked.stderr)
        hooked_run = subprocess.run([str(hooked)], text=True, capture_output=True, timeout=30)
        require(hooked_run.returncode == 0 and "RESTARTED" not in hooked_run.stderr,
                "a hook-ended run must not restart the runner: " + hooked_run.stderr)

        # Not a Genesis image: fail closed at analyze/generate with exit 1 and a diagnostic log.
        bad = tmp / "bad.bin"
        bad.write_bytes(bytes(64))
        result = build(cli, compiler, root, bad, tmp / "bad-out")
        require(result.returncode == 1 and "@result failed stage=" in result.stdout, "invalid ROM must exit 1")
        require('"status":"failed"' in (tmp / "bad-out" / "status.json").read_text(), "failure must be recorded")
        require((tmp / "bad-out" / "build.log").is_file(), "failure must keep build.log")

        # Unusable compiler: exit 3 at the compile stage.
        result = build(cli, compiler, root, rom, tmp / "nocc", cc=str(tmp / "no-such-compiler"))
        require(result.returncode == 3 and "@result failed stage=compile" in result.stdout, "missing compiler must exit 3")

        # Missing ROM and bad usage are reported without a crash.
        result = build(cli, compiler, root, tmp / "missing.bin", tmp / "missing-out")
        require(result.returncode == 1 and "@result failed" in result.stdout, "missing ROM must exit 1")
        usage = subprocess.run([cli, "build", "--rom"], text=True, capture_output=True)
        require(usage.returncode == 2, "bad usage must exit 2")
    print("segarecomp_build_command_test: OK")


if __name__ == "__main__":
    main()
