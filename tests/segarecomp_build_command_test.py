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
                           "--runtime-dir", str(root / "platforms" / "genesis"), "--optimize", "0", *extra],
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
        executable = next(p for p in out.iterdir() if p.stem == "game")
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        require('"result":"stop"' in ran.stdout and digest in ran.stdout, "the native program must emit the sanitized stop report")

        # Instruction budget: an explicit budget is one finite run; with no flag the program is unbounded
        # (it must still be running after a couple of seconds on a guest that never stops).
        loop = tmp / "loop.bin"
        loop.write_bytes(bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x60, 0xFE)))
        result = build(cli, compiler, root, loop, tmp / "loop-out")
        require(result.returncode == 0, "loop build must succeed: " + result.stdout + result.stderr)
        loop_exe = next(p for p in (tmp / "loop-out").iterdir() if p.stem == "game")
        bounded = subprocess.run([str(loop_exe), "--instruction-budget", "1000"], text=True, capture_output=True, timeout=60)
        require('"result":"runner_resource_limit"' in bounded.stdout, "an explicit budget must end in runner_resource_limit")
        try:
            subprocess.run([str(loop_exe)], text=True, capture_output=True, timeout=3)
            require(False, "without --instruction-budget the program must keep running")
        except subprocess.TimeoutExpired:
            pass

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
