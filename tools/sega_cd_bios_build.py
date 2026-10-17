#!/usr/bin/env python3
"""Bounded build-time materialization for the Sega CD BIOS-image profile (SEG-064-T002, ADR 0142 decisions 3, 7 and 8).

The fixed point (ADR 0073's pattern): emit the program from the BIOS image, the declared copy images and the admissions so far ->
compile it (strict C11) -> run it once from reset as a build pass (`--materialize-dir`, no input, a finite instruction budget and a
wall-clock timeout) -> the pass either
  - captured an unknown sub runnable-epoch image (epoch.bin + its pass identity): admit it;
  - stopped fail-closed at a RAM PC whose live bytes are a JMP (xxx).W/.L (a frontier): propose it as an operand template (the
    emitter re-validates the shape with the CPU-owned decoder and the provenance chain);
  - ended otherwise (idle, budget, any other typed stop): the candidate.
A confirming run of the candidate must reproduce its outcome exactly. Every admission carries the identity of the program whose pass
observed it; `segarecomp sega-cd emit` refuses a broken chain. Bounds are constants; exhaustion is a typed failure.

The pass program and the final program are the same generated program: build-pass behaviour is only the run-time flag. Generated
C, objects and snapshots stay in --work (keep it in ignored storage when the image is a real BIOS).

usage: sega_cd_bios_build.py --segarecomp EXE --cc CC --cue DISC --bios IMAGE --work DIR [--copy-image SPEC]...
                             [--budget N] [--timeout S] [--cflags "..."]
prints the build summary JSON; exit 0 on a confirmed build, 1 on a typed build failure, 2 on usage errors.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCD_RUNTIME = ROOT / "platforms" / "sega-cd" / "runtime"
GENESIS_RUNTIME = ROOT / "platforms" / "genesis" / "runtime"
CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-pedantic", "-Werror", "-Wno-type-limits", "-O1"]
MAX_EPOCHS = 4
MAX_TEMPLATES = 64
MAX_ROUNDS = 1 + MAX_EPOCHS + MAX_TEMPLATES  # ADR 0142 decision 8
TEMPLATE_OPCODES = ("4ef8", "4ef9")           # JMP (xxx).W / JMP (xxx).L: the only proposed shapes
EXE = ".exe" if os.name == "nt" else ""


class BuildFailure(Exception):
    def __init__(self, kind, detail=""):
        super().__init__(kind)
        self.kind, self.detail = kind, detail


def run(cmd, timeout, **kw):
    try:
        return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout, **kw)
    except subprocess.TimeoutExpired:
        raise BuildFailure("wall_timeout", str(cmd[0]))


class Driver:
    def __init__(self, args):
        self.args = args
        self.work = pathlib.Path(args.work)
        self.work.mkdir(parents=True, exist_ok=True)
        self.cflags = CFLAGS + (args.cflags.split() if args.cflags else [])
        self.objects = None
        self.compile_seconds = []

    def runtime_objects(self):
        if self.objects is None:
            directory = self.work / "runtime-objects"
            directory.mkdir(exist_ok=True)
            sources = sorted(SCD_RUNTIME.glob("*.c")) + [GENESIS_RUNTIME / n for n in ("runtime.c", "vdp_render.c", "frame_export.c")]
            self.objects = []
            for source in sources:
                obj = directory / (source.parent.parent.name + "_" + source.stem + ".o")
                out = run([self.args.cc, *self.cflags, "-I", SCD_RUNTIME, "-I", GENESIS_RUNTIME, "-c", source, "-o", obj], 600)
                if out.returncode != 0:
                    raise BuildFailure("runtime_compile_failed", out.stderr[-2000:])
                self.objects.append(obj)
        return self.objects

    def emit(self, admits, c_path):
        cmd = [self.args.segarecomp, "sega-cd", "emit", "--cue", self.args.cue, "--bios", self.args.bios, "--out", c_path]
        for spec in self.args.copy_image or []:
            cmd += ["--copy-image", spec]
        for spec in admits:
            cmd += ["--admit", spec]
        out = run(cmd, 600)
        report = json.loads(out.stdout) if out.stdout.strip() else {}
        if out.returncode != 0 or not report.get("ok"):
            raise BuildFailure("emit_failed", json.dumps(report) or out.stderr[-2000:])
        return report

    def compile(self, c_path, exe):
        started = time.monotonic()
        obj = pathlib.Path(str(exe) + ".o")
        out = run([self.args.cc, *self.cflags, "-I", SCD_RUNTIME, "-I", GENESIS_RUNTIME, "-c", c_path, "-o", obj], 1800)
        if out.returncode != 0:
            raise BuildFailure("program_compile_failed", out.stderr[-2000:])
        out = run([self.args.cc, obj, *self.runtime_objects(), "-o", exe], 600)
        if out.returncode != 0:
            raise BuildFailure("program_link_failed", out.stderr[-2000:])
        self.compile_seconds.append(round(time.monotonic() - started, 3))

    def pass_run(self, exe, pass_dir):
        if pass_dir.exists():
            shutil.rmtree(pass_dir)
        pass_dir.mkdir(parents=True)
        out = run([exe, "--disc-dir", pathlib.Path(self.args.cue).parent, "--bios", self.args.bios, "--instruction-budget",
                   str(self.args.budget), "--materialize-dir", pass_dir, "--report", pass_dir / "report.json"], self.args.timeout)
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
        admits, history = [], []
        epochs = templates = 0
        for round_index in range(MAX_ROUNDS):
            c_path = self.work / "program.c"
            exe = self.work / ("program" + EXE)
            emitted = self.emit(admits, c_path)
            self.compile(c_path, exe)
            report, capture = self.pass_run(exe, self.work / "pass")
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
            if capture is not None and capture["kind"] == "frontier" and capture["bytes"][:4] in TEMPLATE_OPCODES:
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
                    "program_identity": emitted["program_identity"], "program": str(exe), "c_bytes": c_path.stat().st_size,
                    "emit": emitted, "compile_seconds": self.compile_seconds, "outcome": self.outcome(report)}
        raise BuildFailure("round_bound_exceeded")


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--segarecomp", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--cue", required=True)
    parser.add_argument("--bios", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--copy-image", action="append")
    parser.add_argument("--budget", type=int, default=5_000_000)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--cflags", default="")
    args = parser.parse_args(argv)
    if args.budget <= 0:
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
