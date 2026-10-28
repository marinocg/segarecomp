"""SEG-062 shared test plumbing: fixture disc -> `segarecomp sega-cd emit` -> strict-C11 compile -> run, and the pinned-Musashi
two-processor reference (tests/tools/sega_cd_musashi_reference.c).

Every generated program is compiled as strict C11 with warnings as errors and run with a finite --instruction-budget and a wall-
clock timeout. Runtime objects are compiled once per Builder."""
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILDER = ROOT / "tools" / "sega_cd_fixture_disc.py"
SCD_RUNTIME = ROOT / "platforms" / "sega-cd" / "runtime"
GENESIS_RUNTIME = ROOT / "platforms" / "genesis" / "runtime"
CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-pedantic", "-Werror", "-Wno-type-limits", "-O1"]
MUSASHI_PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
MUSASHI_ENVS = ("SEGARECOMP_SEGA_CD_MUSASHI_CHECKOUT", "SEGARECOMP_M68K_CONFORMANCE_MUSASHI_CHECKOUT",
                "SEGARECOMP_M68K_MULS_WORD_MUSASHI_CHECKOUT")
RUN_TIMEOUT = 120
EXE = ".exe" if os.name == "nt" else ""


def build_disc(directory, fixture, variant="cooked", *extra):
    out = subprocess.run([sys.executable, str(BUILDER), "build", "--fixture", fixture, "--variant", variant, "--out", str(directory),
                          *extra], check=True, capture_output=True, text=True)
    return json.loads(out.stdout)


class Builder:
    def __init__(self, segarecomp, cc, work):
        self.segarecomp, self.cc, self.work = segarecomp, cc, pathlib.Path(work)
        self.objects = None

    def runtime_objects(self):
        if self.objects is None:
            directory = self.work / "runtime-objects"
            directory.mkdir(parents=True, exist_ok=True)
            sources = sorted(SCD_RUNTIME.glob("*.c")) + [GENESIS_RUNTIME / n for n in ("runtime.c", "vdp_render.c", "frame_export.c")]
            self.objects = []
            for source in sources:
                obj = directory / (source.parent.parent.name + "_" + source.stem + ".o")
                subprocess.run([self.cc, *CFLAGS, "-I", str(SCD_RUNTIME), "-I", str(GENESIS_RUNTIME), "-c", str(source), "-o", str(obj)],
                               check=True)
                self.objects.append(obj)
        return self.objects

    def emit(self, cue, c_path, *extra):
        out = subprocess.run([self.segarecomp, "sega-cd", "emit", "--cue", str(cue), "--out", str(c_path), *extra],
                             capture_output=True, text=True, timeout=300)
        return out.returncode, json.loads(out.stdout) if out.stdout.strip() else {}

    def compile(self, c_path, exe):
        obj = pathlib.Path(str(exe) + ".o")
        subprocess.run([self.cc, *CFLAGS, "-I", str(SCD_RUNTIME), "-I", str(GENESIS_RUNTIME), "-c", str(c_path), "-o", str(obj)], check=True)
        subprocess.run([self.cc, str(obj), *[str(o) for o in self.runtime_objects()], "-o", str(exe)], check=True)
        return obj

    def program(self, name, fixture, variant="cooked", *extra, manifest=None):
        """Build the disc, emit and compile; returns (disc dir, executable, describe dict, emit report). `manifest`, when given, is
        a function (disc directory) -> module-list text passed to `emit --modules` (SEG-063)."""
        directory = self.work / name
        if directory.exists():
            shutil.rmtree(directory)
        expected = build_disc(directory / "disc", fixture, variant, *extra)
        emit_extra = []
        if manifest is not None:
            (directory / "modules.txt").write_text(manifest(directory / "disc"), newline="\n")
            emit_extra = ["--modules", str(directory / "modules.txt")]
        code, report = self.emit(directory / "disc" / "disc.cue", directory / "program.c", *emit_extra)
        if code != 0:
            raise RuntimeError("emit failed: %s" % report)
        exe = directory / ("program" + EXE)
        self.compile(directory / "program.c", exe)
        return directory / "disc", exe, expected, report


# --- SEG-063 module lists ---------------------------------------------------------------------------------------------------------

def disc_facts(disc_dir):
    """(disc identity, {ISO path: entry}) from the independent oracles (never from the C++ reader)."""
    sys.path.insert(0, str(ROOT / "tests"))
    import sega_cd_disc_oracle as oracle
    import sega_cd_iso9660_oracle as iso
    disc = oracle.read_disc(pathlib.Path(disc_dir) / "disc.cue")
    first = disc["tracks"][0]["first_lba"]
    tree = iso.tree(lambda block: disc["_user_block"](first + block))
    return disc["identity"], {entry["path"]: entry for entry in tree["entries"]}, tree


def module_list(identity, modules):
    """Module-list text. Each module: dict(name, processor, path?, extent?=(block, size), sha256, bases, entries, verify?)."""
    lines = ["segarecomp.sega_cd.modules.v1", "# synthetic fixture modules (SEG-063)", "disc " + identity]
    for m in modules:
        lines.append("module " + m["name"])
        lines.append("processor " + m.get("processor", "sub"))
        if m.get("path"):
            lines.append("file " + m["path"])
        if m.get("extent"):
            lines.append("extent %d %d" % tuple(m["extent"]))
        lines.append("sha256 " + m["sha256"])
        for base in m["bases"]:
            lines.append("map 0x%x" % base)
        for entry in m["entries"]:
            lines.append("entry 0x%x" % entry)
        lines.append("verify " + m.get("verify", "byte_identity"))
        lines.append("end")
    return "\n".join(lines) + "\n"


MODULE_BASES = (0x080000, 0x020000)


def standard_modules(files, names=("MODA.BIN;1", "MODB.BIN;1"), bases=MODULE_BASES):
    out = []
    for name in names:
        entry = files["/OVL/" + name]
        out.append({"name": name.split(".")[0].lower(), "path": "/OVL/" + name, "sha256": entry["sha256"], "bases": list(bases),
                    "entries": [0]})
    return out


def standard_manifest(disc_dir, names=("MODA.BIN;1", "MODB.BIN;1")):
    identity, files, _ = disc_facts(disc_dir)
    return module_list(identity, standard_modules(files, names))


def run(exe, disc_dir, budget=2000000, *extra, frame=None, timeout=RUN_TIMEOUT):
    args = [str(exe), "--disc-dir", str(disc_dir), "--instruction-budget", str(budget), *extra]
    if frame is not None:
        args += ["--frame", str(frame)]
    out = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    report = json.loads(out.stdout) if out.stdout.strip() else {}
    return out.returncode, report


def result_words(report, count):
    block = bytes.fromhex(report["result_block"])
    return [int.from_bytes(block[2 * i:2 * i + 2], "big") for i in range(count)]


# --- pinned Musashi reference -----------------------------------------------------------------------------------------------------

def musashi_checkout():
    for name in MUSASHI_ENVS:
        if os.environ.get(name):
            path = pathlib.Path(os.environ[name])
            head = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
            if head != MUSASHI_PIN:
                raise RuntimeError("Musashi checkout %s is not the pinned revision %s" % (path, MUSASHI_PIN))
            return path
    return None


def build_reference(checkout, cc, work):
    work = pathlib.Path(work)
    work.mkdir(parents=True, exist_ok=True)
    subprocess.run([cc, "-std=c11", str(checkout / "m68kmake.c"), "-o", str(work / "m68kmake")], check=True)
    shutil.copyfile(checkout / "m68k_in.c", work / "m68k_in.c")
    subprocess.run([str(work / "m68kmake")], cwd=work, check=True, capture_output=True)
    exe = work / ("sega_cd_reference" + EXE)
    subprocess.run([cc, "-std=c11", "-O1", "-w", "-DM68K_EMULATE_INT_ACK=M68K_OPT_ON", "-DM68K_INT_ACK_CALLBACK(A)=0",
                    "-I", str(work), "-I", str(checkout), "-I", str(checkout / "softfloat"),
                    str(ROOT / "tests" / "tools" / "sega_cd_musashi_reference.c"), str(checkout / "m68kcpu.c"), str(work / "m68kops.c"),
                    str(checkout / "softfloat" / "softfloat.c"), "-lm", "-o", str(exe)], check=True)
    return exe


def run_reference(exe, disc_dir, out_dir):
    sys.path.insert(0, str(ROOT / "tests"))
    import sega_cd_disc_oracle as oracle  # independent reader: the reference never uses the C++ ingestion
    disc = oracle.read_disc(pathlib.Path(disc_dir) / "disc.cue")
    first = disc["tracks"][0]["first_lba"]
    area = b"".join(disc["_user_block"](first + i) for i in range(16))
    out_dir = pathlib.Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "area.bin").write_bytes(area)
    ip, sp = disc["boot"]["ip"], disc["boot"]["sp"]
    proc = subprocess.run([str(exe), str(out_dir / "area.bin"), hex(ip["disc_address"]), hex(ip["load_size"]), hex(ip["entry_offset"]),
                           hex(sp["disc_address"]), hex(sp["load_size"]), hex(sp["entry_offset"]), str(out_dir)],
                          capture_output=True, text=True, timeout=RUN_TIMEOUT)
    if proc.returncode != 0:
        return proc.returncode, None
    state = json.loads((out_dir / "state.json").read_text())
    for name in ("prg_ram", "work_ram", "word_ram"):
        state[name + "_sha256"] = hashlib.sha256((out_dir / (name + ".bin")).read_bytes()).hexdigest()
    return 0, state


def stage_launcher_package(package, segarecomp, cc, sdl3_prefix):
    """SEG-078-T003: a launcher installation (SEGARECOMP_HOME) assembled from the source tree for a test: the product CLI, a `zig` stand-in that forwards
    `zig cc -target T <args>` to the host C compiler, the runtime sources (the tree mirrors the repository, as the release package does) and SDL3.
    Symlinks only; nothing is copied and no game content is involved."""
    package = pathlib.Path(package)
    (package / "bin").mkdir(parents=True)
    (package / "toolchain").mkdir()
    (package / "runtime").mkdir()
    (package / "bin" / ("segarecomp" + EXE)).symlink_to(pathlib.Path(segarecomp).resolve())
    zig = package / "toolchain" / "zig"
    zig.write_text('#!/bin/sh\nif [ "$1" = "--version" ]; then exec %s --version; fi\nshift 3\nexec %s "$@"\n' % (shlex_quote(cc), shlex_quote(cc)))
    zig.chmod(0o755)
    (package / "runtime" / "platforms").symlink_to(ROOT / "platforms")
    (package / "runtime" / "libs").symlink_to(ROOT / "libs")
    (package / "sdl3").mkdir()
    (package / "sdl3" / "include").symlink_to(pathlib.Path(sdl3_prefix) / "include")
    (package / "sdl3" / "lib").symlink_to(pathlib.Path(sdl3_prefix) / "lib")
    return package


def shlex_quote(text):
    import shlex
    return shlex.quote(str(text))
