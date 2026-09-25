#!/usr/bin/env python3
"""SEG-022-T003: the production emitter can emit the generated C as a bounded deterministic set of
translation units that compile under strict C11 (-Werror) and behave exactly like the single-file build.

Project-authored synthetic ROM, no commercial input. The set is forced with `--generated-c-shard-dir`
alone; given together with `--generated-c-output` a small program keeps the historical single file.
"""
import hashlib
import pathlib
import subprocess
import sys
import tempfile


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def emit(binary, rom, digest, *extra):
    return subprocess.run([str(binary), "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry",
                           "--rom-sha256", digest, *extra], text=True, capture_output=True)


def build_and_run(compiler, root, sources, include_dirs, executable):
    runtime = root / "platforms" / "genesis" / "runtime"
    objects = []
    for index, source in enumerate(list(sources) + [runtime / "runtime.c"]):
        obj = executable.parent / f"{executable.name}-{index}.o"
        built = subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O0",
                                "-I", str(runtime)] + [f for d in include_dirs for f in ("-I", str(d))]
                               + ["-c", "-o", str(obj), str(source)], text=True, capture_output=True)
        require(built.returncode == 0, built.stderr)
        objects.append(str(obj))
    linked = subprocess.run([str(compiler), "-o", str(executable)] + objects, text=True, capture_output=True)
    require(linked.returncode == 0, linked.stderr)
    return subprocess.run([str(executable), "--instruction-budget", "64"], text=True, capture_output=True)


def main():
    binary, compiler, root = (pathlib.Path(value).resolve() for value in sys.argv[1:4])
    image = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))
    digest = hashlib.sha256(image).hexdigest()
    with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-translation-units-") as directory:
        temporary = pathlib.Path(directory)
        rom = temporary / "tu.bin"
        rom.write_bytes(image)

        # Historical single-file artifact (reference behaviour).
        single = temporary / "single.c"
        done = emit(binary, rom, digest, "--generated-c-output", str(single))
        require(done.returncode == 0, done.stderr)
        reference = build_and_run(compiler, root, [single], [], temporary / "single-bin")

        # Forced sharding: two runs into different directories are byte-identical.
        sets = []
        for name in ("a", "b"):
            target = temporary / name
            done = emit(binary, rom, digest, "--generated-c-shard-dir", str(target))
            require(done.returncode == 0 and done.stdout == "", done.stderr)
            manifest = target / "bridge_generated.units"
            names = manifest.read_text().splitlines()
            require(names and names[0] == "bridge_generated_main.c", names)
            require(names[1:] == sorted(names[1:]), "manifest order must be main first, then sorted")
            require(len(names) <= 44, "translation-unit count must stay within the documented bound")
            require("translation units: count=" in done.stderr, done.stderr)
            require(sorted(p.name for p in target.iterdir()) == sorted(names + ["bridge_generated.h", "bridge_generated.units"]),
                    "no stray or partial files may remain")
            sets.append({p.name: p.read_bytes() for p in target.iterdir()})
        require(sets[0] == sets[1], "sharded output must be deterministic")
        header = (temporary / "a" / "bridge_generated.h").read_text()
        require("GenesisControlTransfer genesis_block_" in header, "block declarations belong in the shared header")
        main_tu = (temporary / "a" / "bridge_generated_main.c").read_text()
        require(main_tu.startswith('#define _POSIX_C_SOURCE 200809L\n#include "bridge_generated.h"\n'), main_tu[:120])
        require("int main(" in main_tu and sum("int main(" in (temporary / "a" / n).read_text() for n in names) == 1,
                "exactly one TU defines main")
        require(sum("genesis_block_" in (temporary / "a" / n).read_text() and "(GenesisRuntime *runtime) {" in (temporary / "a" / n).read_text()
                    for n in names) >= 1, "block definitions land in shard TUs")

        sharded = build_and_run(compiler, root, [temporary / "a" / n for n in names], [temporary / "a"], temporary / "sharded-bin")
        require(sharded.returncode == reference.returncode and sharded.stdout == reference.stdout,
                "sharded and single-file builds must produce the same sanitized result")

        # Both options: a small program stays a single file (the historical artifact), no shard directory.
        both_single = temporary / "both.c"
        both_dir = temporary / "both-dir"
        done = emit(binary, rom, digest, "--generated-c-output", str(both_single), "--generated-c-shard-dir", str(both_dir))
        require(done.returncode == 0 and both_single.is_file() and not both_dir.exists(), done.stderr)
        require(both_single.read_bytes() == single.read_bytes(), "small-program output is unchanged")
    print("genesis_generated_c_translation_units_test: OK")


if __name__ == "__main__":
    main()
