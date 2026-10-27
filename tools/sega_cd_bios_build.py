#!/usr/bin/env python3
"""Bounded build-time materialization for the Sega CD BIOS-image profile (SEG-064-T002/T003, ADR 0142 decisions 3, 7 and 8).

The fixed point (ADR 0073's pattern): emit the program from the BIOS image, the declared copy images and the admissions so far ->
compile it (strict C11) -> run it once from reset as a build pass (`--materialize-dir`, no input, a finite instruction budget and a
wall-clock timeout) -> the pass either
  - captured an unknown sub runnable-epoch image (epoch.bin + its pass identity): admit it;
  - stopped fail-closed at a RAM PC whose live bytes are a JMP (xxx).W/.L (a frontier): propose it as an operand template (the
    emitter re-validates the shape with the CPU-owned decoder and the provenance chain);
  - ended otherwise (idle, budget, any other typed stop): the candidate.
A confirming run of the candidate must reproduce its outcome exactly. Every admission carries the identity of the program whose pass
observed it; `segarecomp sega-cd emit` refuses a broken chain. Bounds are constants; exhaustion is a typed failure.

SEG-079-T002 (ADR 0180): a frontier capture also reports the bounded contiguous run of live JMP (xxx).L slots containing the stopped PC;
the run is admitted as ONE `template-run` record (still one admission per pass). `--no-template-runs` restores one template per round.

One admission per pass: a pass ends at its first stop (an epoch capture or a frontier is a fail-closed stop - nothing after it runs),
so every later observation would depend on executing past an unadmitted image. Several admissions per pass would need evidence a
stopped pass cannot produce; the per-round cost is instead bounded by incremental compilation (SEG-064-T003):

  - the emitter partitions the program into deterministic processor/PC-window translation units (`--partition-window`) plus one
    shared dispatch unit; a unit's text depends only on its own window's images, variants and templates;
  - every object is cached under the SHA-256 of (cache version, compiler identity, flags, the content of every file the compiler
    reports as a dependency of the runtime ABI header and of the runtime sources, the unit's name and text): an unchanged unit is reused across rounds and builds, a changed unit or any changed input of
    its key is rebuilt, and the shared dispatch unit (entry tables, program identity) is rebuilt only when its text changes;
  - units compile in parallel; the link order is the emitter's unit order, never the host's completion or directory order.

The pass program and the final program are the same generated program: build-pass behaviour is only the run-time flag. Generated
C, objects and snapshots stay in --work (keep it in ignored storage when the image is a real BIOS). `--bios -` reads the BIOS once
from standard input and streams it to every emit and run through an anonymous pipe (POSIX), so a locally held image need not be
copied to disk.

usage: sega_cd_bios_build.py --segarecomp EXE --cc CC --cue DISC --bios IMAGE|- --work DIR [--copy-image SPEC]... [--disc-image SPEC]...
                             [--application-entry ADDRESS]
                             [--budget N] [--until-tick T] [--backup-ram FILE] [--input-script FILE] [--pass-set SPEC]... [--timeout S] [--cflags "..."] [--partition-window BYTES] [--jobs N]
                             [--viewer]
prints the build summary JSON; exit 0 on a confirmed build, 1 on a typed build failure, 2 on usage errors.

SEG-078-T001 (ADR 0204): `--viewer` additionally links the viewer variant of the confirmed program, `program-viewer` next to `program`: the
SAME generated C and objects (the program identity and the headless `program` are unchanged) with only its `main` entry renamed to
`scd_viewer_main_bios`, linked with the shared SDL3 viewer (platforms/sega-cd/viewer, platforms/genesis/viewer, libs/viewer). It needs SDL3
(SDL3_PREFIX or a standard prefix); a missing SDL3 is the typed failure `sdl3_unavailable`, never a silent headless fallback.
"""
import argparse
import concurrent.futures
import contextlib
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCD_RUNTIME = ROOT / "platforms" / "sega-cd" / "runtime"
GENESIS_RUNTIME = ROOT / "platforms" / "genesis" / "runtime"
SCD_VIEWER = ROOT / "platforms" / "sega-cd" / "viewer"
GENESIS_VIEWER = ROOT / "platforms" / "genesis" / "viewer"
SV_DIR = ROOT / "libs" / "viewer"
SV_SOURCES = ("sv_settings.c", "sv_layout.c", "sv_menu.c", "sv_input_state.c", "sv_input_diag.c", "sv_font.c", "sv_font_data.c", "sv_ui.c",
              "sv_host.c", "sv_pages.c", "sv_draw.c")
CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-pedantic", "-Werror", "-Wno-type-limits", "-O1"]
MAX_EPOCHS = 4
MAX_TEMPLATES = 64        # template ADMISSIONS (rounds) - ADR 0142 decision 8; one admission is one instruction or, SEG-079-T002, one slot run
MAX_TEMPLATE_SLOTS = 256  # template slots over all admissions (ADR 0180)
MAX_RUN_SLOTS = 128       # slots in one run admission (ADR 0180)
MAX_ROUNDS = 1 + MAX_EPOCHS + MAX_TEMPLATES  # ADR 0142 decision 8
TEMPLATE_OPCODES = ("4ef8", "4ef9")           # JMP (xxx).W / JMP (xxx).L: the only proposed shapes
DEFAULT_WINDOW = 0x2000                       # measured (ADR 0142 "Result (SEG-064-T003)"); 0 = one translation unit
CACHE_VERSION = "segarecomp.sega_cd_bios_build.objects.v2"
EXE = ".exe" if os.name == "nt" else ""


class BuildFailure(Exception):
    def __init__(self, kind, detail=""):
        super().__init__(kind)
        self.kind, self.detail = kind, detail


class Result:
    def __init__(self, returncode, stdout, stderr, maxrss_kb):
        self.returncode, self.stdout, self.stderr, self.maxrss_kb = returncode, stdout, stderr, maxrss_kb


def run(cmd, timeout, pass_fds=(), cwd=None):
    """Run a command with a wall-clock timeout; report its own peak resident set (KiB) where the host exposes it (wait4)."""
    cmd = [str(c) for c in cmd]
    if not hasattr(os, "wait4"):
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=cwd)
        except subprocess.TimeoutExpired:
            raise BuildFailure("wall_timeout", cmd[0])
        return Result(out.returncode, out.stdout, out.stderr, 0)
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        proc = subprocess.Popen(cmd, stdout=out, stderr=err, pass_fds=pass_fds, cwd=cwd)
        deadline = time.monotonic() + timeout
        delay = 0.001
        while True:
            pid, status, usage = os.wait4(proc.pid, os.WNOHANG)
            if pid == proc.pid:
                break
            if time.monotonic() > deadline:
                proc.kill()
                os.wait4(proc.pid, 0)
                proc.returncode = -9
                raise BuildFailure("wall_timeout", cmd[0])
            time.sleep(delay)
            delay = min(delay * 2, 0.05)
        proc.returncode = os.waitstatus_to_exitcode(status)
        out.seek(0)
        err.seek(0)
        return Result(proc.returncode, out.read().decode(errors="replace"), err.read().decode(errors="replace"), usage.ru_maxrss)


def find_sdl3():
    """(compile flags, link flags) of an installed SDL3, or None. SDL3_PREFIX first, then the common prefixes (the Genesis viewer's rule). A prefix
    with a pkg-config file (`lib/pkgconfig/sdl3.pc`) gives its own flags - `--static` when only a static library is installed (a vcpkg Linux
    triplet), whose private dependencies a plain `-lSDL3` would miss; otherwise `-L<lib> -lSDL3`."""
    for prefix in [p for p in (os.environ.get("SDL3_PREFIX"), "/opt/homebrew", "/usr/local", "/usr") if p]:
        base = pathlib.Path(prefix)
        libdir = base / "lib"
        if not ((base / "include" / "SDL3" / "SDL.h").is_file() and any(libdir.glob("libSDL3.*"))):
            continue
        shared = any(libdir.glob("libSDL3.so*")) or any(libdir.glob("libSDL3*.dylib"))
        pc = libdir / "pkgconfig" / "sdl3.pc"
        if pc.is_file() and shutil.which("pkg-config"):
            env = dict(os.environ, PKG_CONFIG_PATH=str(libdir / "pkgconfig") + os.pathsep + os.environ.get("PKG_CONFIG_PATH", ""))
            flags = subprocess.run(["pkg-config", "--cflags", "--libs", *([] if shared else ["--static"]), "sdl3"], capture_output=True, text=True, env=env)
            if flags.returncode == 0 and flags.stdout.strip():
                tokens = flags.stdout.split()
                compile_flags = [t for t in tokens if t.startswith(("-I", "-D"))]
                link_flags = [t for t in tokens if not t.startswith(("-I", "-D"))] + (["-Wl,-rpath," + str(libdir)] if shared else [])
                return compile_flags, link_flags
        return ["-I", str(base / "include")], ["-L", str(libdir), "-Wl,-rpath," + str(libdir), "-lSDL3"]
    return None


def sha256_text(*parts):
    h = hashlib.sha256()
    for part in parts:
        data = part if isinstance(part, bytes) else str(part).encode()
        h.update(len(data).to_bytes(8, "little"))
        h.update(data)
    return h.hexdigest()


class Driver:
    def __init__(self, args):
        self.args = args
        self.work = pathlib.Path(args.work)
        self.work.mkdir(parents=True, exist_ok=True)
        self.objects_dir = self.work / "objects"
        self.objects_dir.mkdir(exist_ok=True)
        self.cflags = CFLAGS + self.reproducible_flags(args.cc) + (args.cflags.split() if args.cflags else [])
        self.window = args.partition_window
        self.jobs = max(1, args.jobs)
        self.bios_bytes = None
        if args.bios == "-":
            if os.name == "nt":
                raise BuildFailure("bios_stream_unsupported")
            self.bios_bytes = sys.stdin.buffer.read(0x20001)
        self.runtime = None
        self.compile_seconds = []
        self.rounds = []
        self.peak_compile_kb = 0
        self.peak_emit_kb = 0
        self.lock = threading.Lock()
        self.key_base = self.object_key_base()

    @staticmethod
    def reproducible_flags(cc):
        """Clang targeting COFF stamps a time into every object unless asked not to (`/Brepro`); the flag is used only where the
        compiler accepts it under the strict flags (gcc rejects it, ELF/Mach-O clang ignores it without a warning or rejects it)."""
        flag = "-mno-incremental-linker-compatible"
        with tempfile.TemporaryDirectory() as tmp:
            probe = pathlib.Path(tmp) / "probe.c"
            probe.write_text("int segarecomp_probe(void);\nint segarecomp_probe(void) { return 0; }\n")
            try:
                out = run([cc, *CFLAGS, flag, "-c", "probe.c", "-o", "probe.o"], 60, cwd=tmp)
            except BuildFailure:
                return []
            return [flag] if out.returncode == 0 else []

    # ---- the object cache ----
    def object_key_base(self):
        """Everything except the unit's own name and text that can change an object: compiler identity, flags, and the content of every
        file the compiler reports as a dependency (`-M`, system headers included) of a probe unit that includes the runtime ABI header
        (what every generated unit includes) and of every runtime source - so headers reached through relative paths outside the
        runtime directories (the CPU-owned exception/timing cores, the Genesis device contracts) are part of the key."""
        version = run([self.args.cc, "--version"], 60)
        machine = run([self.args.cc, "-dumpmachine"], 60)
        if version.returncode != 0:
            raise BuildFailure("compiler_unavailable", version.stderr[-500:])
        resolved = shutil.which(self.args.cc) or self.args.cc
        dependencies = set()
        with tempfile.TemporaryDirectory(dir=self.objects_dir) as tmp:
            probe = pathlib.Path(tmp) / "probe.c"
            probe.write_text('#include <stddef.h>\n#include <stdint.h>\n#include "scd_runtime.h"\n')
            for source in [probe] + self.runtime_sources():
                out = run([self.args.cc, *self.cflags, "-I", SCD_RUNTIME.resolve(), "-I", GENESIS_RUNTIME.resolve(), "-M", source], 120)
                if out.returncode != 0:
                    raise BuildFailure("dependency_scan_failed", out.stderr[-1000:])
                text = out.stdout.replace("\\\n", " ").replace("\\ ", "\0")
                for token in text.split(":", 1)[1].split():
                    path = pathlib.Path(token.replace("\0", " ")).resolve()
                    if path != probe.resolve():
                        dependencies.add(path)
        files = []
        for path in sorted(dependencies):
            files += [str(path.relative_to(ROOT)) if ROOT in path.parents else str(path), path.read_bytes()]
        self.dependency_count = len(dependencies)
        return sha256_text(CACHE_VERSION, resolved, version.stdout, machine.stdout, json.dumps(self.cflags), *files)

    @staticmethod
    def runtime_sources():
        return sorted(SCD_RUNTIME.glob("*.c")) + [GENESIS_RUNTIME / n for n in ("runtime.c", "vdp_render.c", "frame_export.c")]

    def object_for(self, name, text, include_dirs, extra_flags=()):
        """Compile one source text (or reuse its cached object). Returns (object path, rebuilt?). `extra_flags` (the viewer variant's
        entry rename) are part of the key; with none the key is the historical one."""
        key = sha256_text(self.key_base, name, text, *([json.dumps(list(map(str, extra_flags)))] if extra_flags else []))
        obj = self.objects_dir / (key + ".o")
        if obj.exists():
            return obj, False
        with tempfile.TemporaryDirectory(dir=self.objects_dir) as tmp:
            # Compiled by its bare name from a private directory: no host path enters the object, so equal keys give equal bytes.
            (pathlib.Path(tmp) / name).write_bytes(text)
            partial = pathlib.Path(tmp) / "out.o"
            cmd = [self.args.cc, *self.cflags, *map(str, extra_flags)]
            for d in include_dirs:
                cmd += ["-I", pathlib.Path(d).resolve()]
            out = run(cmd + ["-c", name, "-o", "out.o"], 1800, cwd=tmp)
            with self.lock:
                self.peak_compile_kb = max(self.peak_compile_kb, out.maxrss_kb)
            if out.returncode != 0:
                raise BuildFailure("program_compile_failed", "%s: %s" % (name, out.stderr[-2000:]))
            os.replace(partial, obj)
        return obj, True

    def compile_all(self, items):
        """items: [(name, text bytes, include dirs)] in link order -> ([objects in the same order], rebuilt count)."""
        order = list(range(len(items)))
        if self.args.reverse_compile_order:  # test aid: a different host compile order must not change any output
            order.reverse()
        with concurrent.futures.ThreadPoolExecutor(max_workers=self.jobs) as pool:
            futures = {i: pool.submit(self.object_for, *items[i]) for i in order}
            results = [futures[i].result() for i in range(len(items))]
        return [obj for obj, _ in results], sum(1 for _, rebuilt in results if rebuilt)

    def runtime_objects(self):
        if self.runtime is None:
            items = [(s.parent.parent.name + "_" + s.name, s.read_bytes(), (SCD_RUNTIME, GENESIS_RUNTIME)) for s in self.runtime_sources()]
            self.runtime, _ = self.compile_all(items)
        return self.runtime

    # ---- the BIOS argument: a path, or a pipe fed from the bytes read once ----
    @contextlib.contextmanager
    def bios(self):
        if self.bios_bytes is None:
            yield self.args.bios, ()
            return
        r, w = os.pipe()
        data = self.bios_bytes

        def feed():
            view = memoryview(data)
            try:
                while view:
                    view = view[os.write(w, view):]
            except OSError:  # the reader exited without reading everything (it refused the image): nothing to feed
                pass
            finally:
                os.close(w)
        thread = threading.Thread(target=feed, daemon=True)
        thread.start()
        try:
            yield "/dev/fd/%d" % r, (r,)
        finally:
            os.close(r)
            thread.join()

    # ---- one round ----
    def emit(self, admits, src_dir):
        if src_dir.exists():
            shutil.rmtree(src_dir)
        src_dir.mkdir(parents=True)
        c_path = src_dir / "program.c"
        with self.bios() as (bios, fds):
            cmd = [self.args.segarecomp, "sega-cd", "emit", "--cue", self.args.cue, "--bios", bios, "--out", c_path]
            for spec in self.args.copy_image or []:
                cmd += ["--copy-image", spec]
            for spec in self.args.disc_image or []:
                cmd += ["--disc-image", spec]
            if self.args.application_entry:
                cmd += ["--application-entry", self.args.application_entry]
            for spec in admits:
                cmd += ["--admit", spec]
            if self.window:
                cmd += ["--partition-window", str(self.window)]
            out = run(cmd, 1800, pass_fds=fds)
        self.peak_emit_kb = max(self.peak_emit_kb, out.maxrss_kb)
        try:
            report = json.loads(out.stdout) if out.stdout.strip() else {}
        except ValueError:
            report = {}
        if out.returncode != 0 or not report.get("ok"):
            raise BuildFailure("emit_failed", json.dumps(report) if report else out.stderr[-2000:])
        return report, c_path

    def compile(self, report, c_path, exe):
        started = time.monotonic()
        units = report.get("units", [])
        inc = (SCD_RUNTIME, GENESIS_RUNTIME)
        items = [(u["name"], (c_path.parent / u["name"]).read_bytes(), inc) for u in units]  # emitter order = link order
        items.append(("program.c", c_path.read_bytes(), inc))
        objects, rebuilt = self.compile_all(items)
        self.last_items, self.last_objects = items, objects   # the viewer variant reuses every unit object
        compiled = time.monotonic()
        inputs = [*objects, *self.runtime_objects()]
        # The link inputs in link order: equal digests mean an equal program on every host (a Windows PE link also stamps a time).
        self.link_inputs = [hashlib.sha256(pathlib.Path(o).read_bytes()).hexdigest() for o in inputs]
        out = run([self.args.cc, *inputs, "-o", exe], 600)
        if out.returncode != 0:
            raise BuildFailure("program_link_failed", out.stderr[-2000:])
        seconds = round(time.monotonic() - started, 3)
        self.compile_seconds.append(seconds)
        return {"units": len(items), "rebuilt": rebuilt, "reused": len(items) - rebuilt, "compile_seconds": seconds,
                "link_seconds": round(time.monotonic() - compiled, 3)}

    def pass_run(self, exe, pass_dir):
        if pass_dir.exists():
            shutil.rmtree(pass_dir)
        pass_dir.mkdir(parents=True)
        with self.bios() as (bios, fds):
            until = ["--until-tick", str(self.args.until_tick)] if self.args.until_tick else []  # SEG-064-T005: a machine-time end
            if self.args.backup_ram:  # SEG-065-T002: the backup RAM machine input of every pass
                until += ["--backup-ram", self.args.backup_ram]
            if self.args.input_script:  # SEG-070-T002: the timed pad machine input of every pass (the frontiers of an input-driven path)
                until += ["--input-script", self.args.input_script]
            for spec in self.args.pass_set or []:   # SEG-064-T005: register presets of the build pass (project-authored fixtures select a case this way)
                until += ["--set", spec]
            out = run([exe, "--disc-dir", pathlib.Path(self.args.cue).parent, "--bios", bios, "--instruction-budget", str(self.args.budget),
                       "--materialize-dir", pass_dir, "--report", pass_dir / "report.json", *until], self.args.timeout, pass_fds=fds)
        report_path = pass_dir / "report.json"
        if not report_path.exists():
            raise BuildFailure("pass_failed", "exit %d %s" % (out.returncode, out.stdout[-500:]))
        report = json.loads(report_path.read_text())
        capture = json.loads((pass_dir / "capture.json").read_text()) if (pass_dir / "capture.json").exists() else None
        return report, capture

    @staticmethod
    def outcome(report):
        return {k: report.get(k) for k in ("result", "stop", "cpus", "digests", "result_block", "bios_profile", "device_log", "ticks")}

    def link_viewer(self):
        """SEG-078-T001 (ADR 0204): the viewer variant of the program just confirmed. Every unit object is reused; only `program.c` is
        recompiled with its entry renamed, and the shared SDL3 viewer sources are compiled and linked next to the runtime objects.
        Returns the summary entry. `--viewer-host-source` replaces the SDL3 viewer by a project-authored test host (no SDL3 needed)."""
        items, objects = self.last_items, self.last_objects
        name, text, include_dirs = items[-1]
        if name != "program.c":
            raise BuildFailure("viewer_link_failed", "the program unit is not the last item")
        program_object, _ = self.object_for(name, text, include_dirs, ["-Dscd_program_main_bios=scd_viewer_main_bios"])
        out = self.work / ("program-viewer" + EXE)
        host_source = self.args.viewer_host_source
        includes = [SCD_RUNTIME, GENESIS_RUNTIME, SCD_VIEWER, GENESIS_VIEWER, SV_DIR / "include", SV_DIR / "src"]
        if host_source:
            sdl, sources, host = ([], []), [pathlib.Path(host_source)], "test"
        else:
            sdl = find_sdl3()
            if sdl is None:
                raise BuildFailure("sdl3_unavailable", "SDL3 headers/library not found (install SDL3 or set SDL3_PREFIX)")
            sources = [SCD_VIEWER / "scd_viewer.c", GENESIS_VIEWER / "viewer.c", GENESIS_VIEWER / "viewer_sdl3.c"] + [SV_DIR / "src" / n for n in SV_SOURCES]
            host = "sdl3"
        inc_flags = [f for d in includes for f in ("-I", str(d))] + sdl[0]
        with tempfile.TemporaryDirectory(dir=self.objects_dir) as tmp:
            def compile_one(i_src):
                i, src = i_src
                obj = pathlib.Path(tmp) / ("v%02d.o" % i)
                result = run([self.args.cc, *self.cflags, *inc_flags, "-c", src, "-o", obj], 600)
                if result.returncode != 0:
                    raise BuildFailure("viewer_compile_failed", "%s: %s" % (src.name, result.stderr[-2000:]))
                return obj
            with concurrent.futures.ThreadPoolExecutor(max_workers=self.jobs) as pool:
                viewer_objects = list(pool.map(compile_one, enumerate(sources)))
            link = run([self.args.cc, *objects[:-1], program_object, *self.runtime_objects(), *viewer_objects, "-o", out, *sdl[1],
                        *([] if os.name == "nt" else ["-lm"])], 600)
            if link.returncode != 0:
                raise BuildFailure("viewer_link_failed", link.stderr[-2000:])
        return {"program": str(out), "host": host, "sources": len(sources)}

    def build(self):
        started = time.monotonic()
        admits, history = [], []
        epochs = templates = slot_total = 0
        exe = self.work / ("program" + EXE)
        if self.args.epoch_recipe:  # SEG-079 (ADR 0179): the epoch is a static producer: no discovery round, still verified whole at the epoch
            admits.append("epoch-recipe:" + self.args.epoch_recipe)
            epochs = 1
            history.append({"round": None, "admitted": {"kind": "epoch_recipe", "recipe": self.args.epoch_recipe}})
        for round_index in range(MAX_ROUNDS):
            t0 = time.monotonic()
            emitted, c_path = self.emit(admits, self.work / "src")
            t1 = time.monotonic()
            metrics = self.compile(emitted, c_path, exe)
            t2 = time.monotonic()
            report, capture = self.pass_run(exe, self.work / "pass")
            if self.args.epoch_recipe and (report["stop"]["class"] == "materialized_image_unverified" or (
                    capture is not None and capture["kind"] == "epoch" and report.get("bios_profile", {}).get("epochs", 0) == 0)):
                # a build pass writes a snapshot instead of stopping when the live image matches no admitted one (ADR 0142 decision 4): at the
                # first epoch that means the recipe image is not what the BIOS produced
                raise BuildFailure("epoch_recipe_unverified", "the live PRG RAM at the first epoch is not the reconstructed image")
            metrics.update({"round": round_index, "emit_seconds": round(t1 - t0, 3), "pass_seconds": round(time.monotonic() - t2, 3),
                            "functions": emitted["functions"], "c_bytes": emitted["c_bytes"]})
            self.rounds.append(metrics)
            entry = {"round": round_index, "program_identity": emitted["program_identity"], "result": report["result"],
                     "stop": report["stop"]["class"]}
            if capture is not None and capture["pass_identity"] != emitted["program_identity"]:
                raise BuildFailure("pass_identity_mismatch")
            if capture is not None and capture["kind"] == "epoch":
                epochs += 1
                if epochs > MAX_EPOCHS:
                    raise BuildFailure("epoch_bound_exceeded")
                snapshot = self.work / ("epoch%d.bin" % (epochs - 1))
                shutil.copyfile(self.work / "pass" / "epoch.bin", snapshot)
                admits.append("epoch:%s:%s" % (capture["pass_identity"], snapshot))
                entry["admitted"] = {"kind": "epoch", "length": capture["length"],
                                     "sha256": hashlib.sha256(snapshot.read_bytes()).hexdigest()}
                history.append(entry)
                continue
            proposed = set()
            for a in admits:
                if a.startswith("template:"):
                    proposed.add((a.split(":")[2], int(a.split(":")[3], 0)))
                elif a.startswith("template-run:"):
                    f = a.split(":")
                    proposed.update((f[2], int(f[3], 0) + 6 * k) for k in range(int(f[4])))
            if (capture is not None and capture["kind"] == "frontier" and capture["bytes"][:4] in TEMPLATE_OPCODES and
                    ("main" if capture["cpu"] == 0 else "sub", capture["pc"]) not in proposed):
                templates += 1
                if templates > MAX_TEMPLATES:
                    raise BuildFailure("template_bound_exceeded")
                cpu = "main" if capture["cpu"] == 0 else "sub"
                run = capture.get("run") if self.args.template_runs else None
                if run is not None:
                    # trim the run to the maximal stretch around the stopped PC that holds no already admitted slot (a table longer than
                    # one run is admitted run after run; the capture itself cannot know the admissions)
                    base, slots, pc = run["base"], run["slots"], capture["pc"]
                    if slots >= 1 and len(run["bytes"]) == 12 * slots and base <= pc < base + 6 * slots and (pc - base) % 6 == 0:
                        lo = hi = (pc - base) // 6
                        while lo > 0 and (cpu, base + 6 * (lo - 1)) not in proposed:
                            lo -= 1
                        while hi < slots - 1 and (cpu, base + 6 * (hi + 1)) not in proposed:
                            hi += 1
                        run = {"base": base + 6 * lo, "slots": hi - lo + 1, "bytes": run["bytes"][12 * lo:12 * (hi + 1)]}
                if run is not None and run["slots"] > 1:
                    # SEG-079-T002 (ADR 0180): the whole contiguous JMP (xxx).L slot run containing the stopped PC, as ONE admission. The
                    # emitter re-validates every slot; a malformed run is a typed failure, never a silent fallback.
                    if (run["slots"] > MAX_RUN_SLOTS or len(run["bytes"]) != 12 * run["slots"] or
                            not run["base"] <= capture["pc"] < run["base"] + 6 * run["slots"] or (capture["pc"] - run["base"]) % 6):
                        raise BuildFailure("template_run_invalid")
                    slot_total += run["slots"]
                    if slot_total > MAX_TEMPLATE_SLOTS:
                        raise BuildFailure("template_slot_bound_exceeded")
                    admits.append("template-run:%s:%s:0x%x:%d:%s" % (capture["pass_identity"], cpu, run["base"], run["slots"], run["bytes"]))
                    entry["admitted"] = {"kind": "operand_template_run", "cpu": cpu, "base": run["base"], "slots": run["slots"]}
                else:
                    slot_total += 1
                    admits.append("template:%s:%s:0x%x:%s" % (capture["pass_identity"], cpu, capture["pc"], capture["bytes"]))
                    entry["admitted"] = {"kind": "operand_template", "cpu": cpu, "pc": capture["pc"]}
                history.append(entry)
                continue
            history.append(entry)
            confirm, confirm_capture = self.pass_run(exe, self.work / "confirm")
            if self.outcome(confirm) != self.outcome(report) or (confirm_capture is None) != (capture is None):
                raise BuildFailure("materialization_nondeterministic")
            viewer = self.link_viewer() if self.args.viewer else None
            return {"ok": True, "viewer": viewer, "rounds": round_index + 1, "admissions": admits, "history": history,
                    "program_identity": emitted["program_identity"], "program": str(exe), "c_bytes": emitted["c_bytes"],
                    "emit": emitted, "compile_seconds": self.compile_seconds, "outcome": self.outcome(report),
                    "partition_window": self.window, "round_metrics": self.rounds, "peak_compile_kb": self.peak_compile_kb,
                    "peak_emit_kb": self.peak_emit_kb, "total_seconds": round(time.monotonic() - started, 3),
                    "link_inputs": self.link_inputs}
        raise BuildFailure("round_bound_exceeded")


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--segarecomp", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--cue", required=True)
    parser.add_argument("--bios", required=True, help="the BIOS image, or - to read it once from standard input (streamed by pipe)")
    parser.add_argument("--work", required=True)
    parser.add_argument("--copy-image", action="append")
    parser.add_argument("--disc-image", action="append", help="main|sub:<track-1 byte offset>:<base>:<length>: a disc extent placed in the processor's RAM (SEG-064-T004; main Word RAM SEG-071-T001)")
    parser.add_argument("--application-entry", help="the main address whose first execution ends the run (typed stop application_handoff)")
    parser.add_argument("--epoch-recipe", help="SEG-079: the epoch image as a static producer, <length>:<source>@<dest>[,...] (tools/sega_cd_epoch_recipe.py spec); no epoch discovery round")
    parser.add_argument("--no-template-runs", dest="template_runs", action="store_false",
                        help="SEG-079-T002: admit one operand template per round even when the capture reports a slot run (the conventional baseline)")
    parser.add_argument("--budget", type=int, default=5_000_000)
    parser.add_argument("--pass-set", action="append", help="main|sub:dN|aN=VALUE: a register preset of every build pass (fixture case selection)")
    parser.add_argument("--until-tick", type=int, default=0, help="end every pass at this machine-time instant (typed result deadline); 0 = none")
    parser.add_argument("--input-script", help="a timed standard-pad input script (ADR 0149) given to every pass (SEG-070-T002); default: none")
    parser.add_argument("--backup-ram", help="an 8 KiB internal backup RAM image given to every pass (SEG-065-T002); default: none")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--cflags", default="")
    parser.add_argument("--partition-window", type=lambda v: int(v, 0), default=DEFAULT_WINDOW)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument("--viewer", action="store_true", help="SEG-078-T001: also link the SDL3 viewer variant, program-viewer (the headless program is unchanged)")
    parser.add_argument("--viewer-host-source", help=argparse.SUPPRESS)  # test seam: a project-authored C host instead of the SDL3 viewer (implies --viewer)
    parser.add_argument("--reverse-compile-order", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.viewer_host_source:
        args.viewer = True
    if args.budget <= 0 or (args.partition_window and (args.partition_window < 0x100 or args.partition_window > 0x1000000 or
                                                       args.partition_window & (args.partition_window - 1))):
        return 2
    try:
        summary = Driver(args).build()
    except BuildFailure as failure:
        print(json.dumps({"ok": False, "failure": failure.kind, "detail": failure.detail[:2000]}))
        return 1
    print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
