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

One admission per pass: a pass ends at its first stop (an epoch capture or a frontier is a fail-closed stop - nothing after it runs),
so every later observation would depend on executing past an unadmitted image. Several admissions per pass would need evidence a
stopped pass cannot produce; the per-round cost is instead bounded by incremental compilation (SEG-064-T003):

  - the emitter partitions the program into deterministic processor/PC-window translation units (`--partition-window`) plus one
    shared dispatch unit; a unit's text depends only on its own window's images, variants and templates;
  - every object is cached under the SHA-256 of (cache version, compiler identity, flags, every runtime header the units can
    include, the unit's name and text): an unchanged unit is reused across rounds and builds, a changed unit or any changed input of
    its key is rebuilt, and the shared dispatch unit (entry tables, program identity) is rebuilt only when its text changes;
  - units compile in parallel; the link order is the emitter's unit order, never the host's completion or directory order.

The pass program and the final program are the same generated program: build-pass behaviour is only the run-time flag. Generated
C, objects and snapshots stay in --work (keep it in ignored storage when the image is a real BIOS). `--bios -` reads the BIOS once
from standard input and streams it to every emit and run through an anonymous pipe (POSIX), so a locally held image need not be
copied to disk.

usage: sega_cd_bios_build.py --segarecomp EXE --cc CC --cue DISC --bios IMAGE|- --work DIR [--copy-image SPEC]...
                             [--budget N] [--timeout S] [--cflags "..."] [--partition-window BYTES] [--jobs N]
prints the build summary JSON; exit 0 on a confirmed build, 1 on a typed build failure, 2 on usage errors.
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
CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-pedantic", "-Werror", "-Wno-type-limits", "-O1"]
MAX_EPOCHS = 4
MAX_TEMPLATES = 64
MAX_ROUNDS = 1 + MAX_EPOCHS + MAX_TEMPLATES  # ADR 0142 decision 8
TEMPLATE_OPCODES = ("4ef8", "4ef9")           # JMP (xxx).W / JMP (xxx).L: the only proposed shapes
DEFAULT_WINDOW = 0x2000                       # measured (ADR 0142 "Result (SEG-064-T003)"); 0 = one translation unit
CACHE_VERSION = "segarecomp.sega_cd_bios_build.objects.v1"
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
        self.cflags = CFLAGS + (args.cflags.split() if args.cflags else [])
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

    # ---- the object cache ----
    def object_key_base(self):
        """Everything except the unit's own name and text that can change an object: compiler identity, flags and every header the
        generated units and the runtime can include (both runtime include directories, by name and content)."""
        version = run([self.args.cc, "--version"], 60)
        machine = run([self.args.cc, "-dumpmachine"], 60)
        if version.returncode != 0:
            raise BuildFailure("compiler_unavailable", version.stderr[-500:])
        resolved = shutil.which(self.args.cc) or self.args.cc
        headers = []
        for directory in (SCD_RUNTIME, GENESIS_RUNTIME):
            for header in sorted(directory.glob("*.h")):
                headers += [directory.name + "/" + header.name, header.read_bytes()]
        return sha256_text(CACHE_VERSION, resolved, version.stdout, machine.stdout, json.dumps(self.cflags), *headers)

    def object_for(self, name, text, include_dirs):
        """Compile one source text (or reuse its cached object). Returns (object path, rebuilt?)."""
        key = sha256_text(self.key_base, name, text)
        obj = self.objects_dir / (key + ".o")
        if obj.exists():
            return obj, False
        with tempfile.TemporaryDirectory(dir=self.objects_dir) as tmp:
            # Compiled by its bare name from a private directory: no host path enters the object, so equal keys give equal bytes.
            (pathlib.Path(tmp) / name).write_bytes(text)
            partial = pathlib.Path(tmp) / "out.o"
            cmd = [self.args.cc, *self.cflags]
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
            sources = sorted(SCD_RUNTIME.glob("*.c")) + [GENESIS_RUNTIME / n for n in ("runtime.c", "vdp_render.c", "frame_export.c")]
            items = [(s.parent.parent.name + "_" + s.name, s.read_bytes(), (SCD_RUNTIME, GENESIS_RUNTIME)) for s in sources]
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
        compiled = time.monotonic()
        out = run([self.args.cc, *objects, *self.runtime_objects(), "-o", exe], 600)
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
            out = run([exe, "--disc-dir", pathlib.Path(self.args.cue).parent, "--bios", bios, "--instruction-budget", str(self.args.budget),
                       "--materialize-dir", pass_dir, "--report", pass_dir / "report.json"], self.args.timeout, pass_fds=fds)
        report_path = pass_dir / "report.json"
        if not report_path.exists():
            raise BuildFailure("pass_failed", "exit %d %s" % (out.returncode, out.stdout[-500:]))
        report = json.loads(report_path.read_text())
        capture = json.loads((pass_dir / "capture.json").read_text()) if (pass_dir / "capture.json").exists() else None
        return report, capture

    @staticmethod
    def outcome(report):
        return {k: report.get(k) for k in ("result", "stop", "cpus", "digests", "result_block", "bios_profile", "device_log", "ticks")}

    def build(self):
        started = time.monotonic()
        admits, history = [], []
        epochs = templates = 0
        exe = self.work / ("program" + EXE)
        for round_index in range(MAX_ROUNDS):
            t0 = time.monotonic()
            emitted, c_path = self.emit(admits, self.work / "src")
            t1 = time.monotonic()
            metrics = self.compile(emitted, c_path, exe)
            t2 = time.monotonic()
            report, capture = self.pass_run(exe, self.work / "pass")
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
            proposed = {(a.split(":")[2], int(a.split(":")[3], 0)) for a in admits if a.startswith("template:")}
            if (capture is not None and capture["kind"] == "frontier" and capture["bytes"][:4] in TEMPLATE_OPCODES and
                    ("main" if capture["cpu"] == 0 else "sub", capture["pc"]) not in proposed):
                templates += 1
                if templates > MAX_TEMPLATES:
                    raise BuildFailure("template_bound_exceeded")
                cpu = "main" if capture["cpu"] == 0 else "sub"
                admits.append("template:%s:%s:0x%x:%s" % (capture["pass_identity"], cpu, capture["pc"], capture["bytes"]))
                entry["admitted"] = {"kind": "operand_template", "cpu": cpu, "pc": capture["pc"]}
                history.append(entry)
                continue
            history.append(entry)
            confirm, confirm_capture = self.pass_run(exe, self.work / "confirm")
            if self.outcome(confirm) != self.outcome(report) or (confirm_capture is None) != (capture is None):
                raise BuildFailure("materialization_nondeterministic")
            return {"ok": True, "rounds": round_index + 1, "admissions": admits, "history": history,
                    "program_identity": emitted["program_identity"], "program": str(exe), "c_bytes": emitted["c_bytes"],
                    "emit": emitted, "compile_seconds": self.compile_seconds, "outcome": self.outcome(report),
                    "partition_window": self.window, "round_metrics": self.rounds, "peak_compile_kb": self.peak_compile_kb,
                    "peak_emit_kb": self.peak_emit_kb, "total_seconds": round(time.monotonic() - started, 3)}
        raise BuildFailure("round_bound_exceeded")


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--segarecomp", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--cue", required=True)
    parser.add_argument("--bios", required=True, help="the BIOS image, or - to read it once from standard input (streamed by pipe)")
    parser.add_argument("--work", required=True)
    parser.add_argument("--copy-image", action="append")
    parser.add_argument("--budget", type=int, default=5_000_000)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--cflags", default="")
    parser.add_argument("--partition-window", type=lambda v: int(v, 0), default=DEFAULT_WINDOW)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument("--reverse-compile-order", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
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
