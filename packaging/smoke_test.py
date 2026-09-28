#!/usr/bin/env python3
"""Package smoke test: exercise an EXTRACTED release package with a project-authored synthetic ROM.

usage: smoke_test.py <path-to-extracted-launcher-executable>

The package is driven through the launcher's non-interactive mode (`Segarecomp --build <rom> --run`), the same
core the GUI uses. The child environment is scrubbed: an empty PATH (no cc/clang/gcc/zig/python/cmake/ninja
can be found), no CC/CXX, a throw-away HOME and cache. Nothing from the build tree is referenced.
This script itself is CI glue and is not part of the shipped package.
"""
import hashlib
import os
import pathlib
import subprocess
import sys
import tempfile

# Reset SSP, reset PC=8, MOVEQ #0,D0 then RESET: a deterministic unsupported-instruction stop
# (the sanitized checkpoint the synthetic route is expected to end in).
IMAGE = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))


CACHE_FOR_DIAGNOSTICS = []


def require(condition, message):
    if not condition:
        print("SMOKE FAIL:", message, file=sys.stderr)
        for cache in CACHE_FOR_DIAGNOSTICS:  # show the preserved diagnostics of any failed build
            for log in sorted(cache.glob("games/*/build.log")):
                print(f"---- {log} ----\n" + log.read_text(encoding="utf-8", errors="replace")[-6000:], file=sys.stderr)
        sys.exit(1)


def parse(report):
    return dict(line.split("=", 1) for line in report.read_text(encoding="utf-8").splitlines() if "=" in line)


def main():
    launcher = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="segarecomp-smoke-") as directory:
        tmp = pathlib.Path(directory)
        (tmp / "emptybin").mkdir()
        (tmp / "home").mkdir()
        # Decoy host tools: if the build ever resolved a compiler through PATH it would trip these.
        decoys = ("cc", "gcc", "clang", "zig", "c99") if os.name != "nt" else ()
        for name in decoys:
            script = tmp / "emptybin" / name
            script.write_text(f"#!/bin/sh\necho {name} >> '{tmp}/decoy-used'\nexit 99\n")
            script.chmod(0o755)
        env = {"PATH": str(tmp / "emptybin"), "HOME": str(tmp / "home"), "SEGARECOMP_CACHE_DIR": str(tmp / "cache"),
               "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}
        if os.name == "nt":
            system = os.environ.get("SystemRoot", r"C:\Windows")
            env.update(SystemRoot=system, PATH=str(tmp / "emptybin") + ";" + system + r"\System32",
                       LOCALAPPDATA=str(tmp / "home"), TEMP=str(tmp), TMP=str(tmp))
            env["SEGARECOMP_CACHE_DIR"] = str(tmp / "cache")
        CACHE_FOR_DIAGNOSTICS.append(tmp / "cache")
        rom = tmp / "synthetic.bin"
        rom.write_bytes(IMAGE)
        digest = hashlib.sha256(IMAGE).hexdigest()

        def launch(rom_path, report_name, run=True):
            report = tmp / report_name
            command = [str(launcher), "--build", str(rom_path), "--report", str(report)] + (["--run"] if run else [])
            result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=900)
            return result.returncode, (parse(report) if report.exists() else {}), result

        # 1-3, 6-9: build with the bundled compiler, run the produced native program.
        code, first, result = launch(rom, "first.txt")
        require(code == 0, f"first build+run failed rc={code}: {first}")
        package_root = pathlib.Path(first["layout_root"]).resolve()
        compiler = pathlib.Path(first["compiler"]).resolve()
        require(package_root in compiler.parents, "compiler must live inside the package")
        require(first["cache"] == "miss" and first["build"] == "ok", f"unexpected first result {first}")
        require(first["rom_sha256"] == digest, "ROM digest mismatch")
        require(first["run"] == "exited code=0", f"native program did not run cleanly: {first['run']}")
        entry = pathlib.Path(first["entry"])
        build_log = (entry / "build.log").read_text(encoding="utf-8")
        require(f"cc={compiler}" in build_log, "build.log must show the bundled compiler was configured")
        compile_lines = [line for line in build_log.splitlines() if line.startswith("$ ")]
        require(compile_lines and all(line.startswith(f"$ {compiler}") or line.startswith("$ (link)") for line in compile_lines),
                "every compile command must invoke the bundled compiler")
        run_log = (entry / "run.log").read_text(encoding="utf-8")
        require("VIEWER_SUMMARY" in run_log and '"result":"stop"' in run_log and digest in run_log,
                "the launched program must reach the synthetic checkpoint (sanitized stop report)")
        require("rom_path" in (entry / "metadata.json").read_text() and str(rom) not in (entry / "metadata.json").read_text(),
                "the cache must not record or copy the ROM")
        require(not list(entry.rglob("synthetic.bin")), "the ROM must not be copied into the cache")

        # cache hit: no rebuild, entry untouched.
        stamp = (entry / "status.json").stat().st_mtime_ns
        code, second, _ = launch(rom, "second.txt")
        require(code == 0 and second["cache"] == "hit" and "build" not in second, f"expected a cache hit: {second}")
        require((entry / "status.json").stat().st_mtime_ns == stamp, "a cache hit must not rebuild")

        # stale/incomplete entry rebuilds safely.
        (entry / "status.json").unlink()
        code, third, _ = launch(rom, "third.txt", run=False)
        require(code == 0 and third["cache"] == "miss" and third["build"] == "ok", f"stale entry must rebuild: {third}")

        # 10: failure is reported (non-zero, human message) with preserved diagnostics.
        bad = tmp / "notarom.bin"
        bad.write_bytes(bytes(64))
        code, failed, _ = launch(bad, "failed.txt", run=False)
        require(code == 3 and failed.get("build") == "failed" and failed.get("message"), f"failure must be reported: {failed}")
        failed_log = pathlib.Path(failed["entry"] + ".failed") / "build.log"
        require(failed_log.is_file() and "FAILED" in failed_log.read_text(encoding="utf-8"), "diagnostics must be preserved")
        code, unreadable, _ = launch(tmp / "missing.bin", "missing.txt", run=False)
        require(code == 1 and "problem" in "".join(unreadable.values()) + str(unreadable), "unreadable ROM must fail cleanly")
        require(not (tmp / "decoy-used").exists(), "a host compiler on PATH was used")
    print("package smoke test: OK")


if __name__ == "__main__":
    main()
