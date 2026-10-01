#!/usr/bin/env python3
"""SEG-032-T002 (ADR 0073): Z80 image-epoch materialization probe library and CLI.

Library: the canonical content hash, the activation signature S1* (the 68K-written extents of the hold window), the
image-spec rendering for the existing broad Z80 AOT (`z80_image_emitter`) and a measured compile.

CLI:  genesis_z80_materialization_probe.py derive --dir DIR --emitter z80_image_emitter [--cc CC] [--repeats N]
      reads the epoch snapshots the epoch probe (platforms/genesis/viewer/z80_epoch_probe_main_hook.c) left in DIR and
      prints ONE JSON object of sanitized aggregates (counts, byte-length classes, booleans, timings); it never prints
      a snapshot byte, an address or a hash.
"""
import argparse
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import host_cc  # noqa: E402

RAM_BYTES = 8192
WINDOW_BYTES = 0x4000  # two mirrors of the 8 KiB sound RAM
CONTENT_TAG = b"segarecomp.genesis.z80.image.v1"
SIGNATURE_TAG = b"segarecomp.genesis.z80.signature.v1.extents"


def content_hash(ram):
    """Names a compiled image and makes the registry reproducible; never runtime selection authority."""
    assert len(ram) == RAM_BYTES
    return hashlib.sha256(CONTENT_TAG + struct.pack("<IB", WINDOW_BYTES, WINDOW_BYTES // RAM_BYTES) + bytes(ram)).hexdigest()


def extents(written):
    """Maximal runs [(offset, length)] of the hold-window written bitmap (bit i of byte i/8)."""
    runs, start = [], None
    for offset in range(RAM_BYTES):
        bit = (written[offset >> 3] >> (offset & 7)) & 1
        if bit and start is None:
            start = offset
        if not bit and start is not None:
            runs.append((start, offset - start))
            start = None
    if start is not None:
        runs.append((start, RAM_BYTES - start))
    return runs


def signature(ram, written):
    """Activation signature S1*: the sequence of (u16le offset, u16le length, bytes) over the 68K-written extents."""
    digest = hashlib.sha256(SIGNATURE_TAG)
    for offset, length in extents(written):
        digest.update(struct.pack("<HH", offset, length) + bytes(ram[offset:offset + length]))
    return digest.hexdigest()


def image_spec(ram, group=None):
    """The `z80_image_emitter` spec of one materialized image: one invariant window `$0000-$3FFF` (two mirrors)."""
    text = "image 1 invariant\nwindow 1 0000 0 4000\n"
    if group:
        text = "group %d\n" % group + text
    doubled = bytes(ram) * (WINDOW_BYTES // RAM_BYTES)
    for start in range(0, len(doubled), 4096):
        text += "bytes 1 %s\n" % doubled[start:start + 4096].hex()
    return text


def emit_and_compile(emitter, ram, workdir, cc=None, jobs=None):
    """Broad Z80 AOT of one snapshot plus a strict-C11 compile of every generated unit. Returns aggregate metrics and the
    SHA-256 of the concatenated emitted units (for the determinism checks; callers must not persist it)."""
    cc = cc or os.environ.get("CC") or "cc"
    workdir = pathlib.Path(workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    spec = workdir / "image.spec"
    spec.write_text(image_spec(ram))
    started = time.monotonic()
    emitted = subprocess.run([str(emitter), str(spec), str(workdir), "image"], text=True, capture_output=True)
    emit_seconds = time.monotonic() - started
    if emitted.returncode != 0:
        return {"error": "emit"}
    units = [workdir / name for name in (workdir / "image.units").read_text().split()]
    include = HERE.parent / "libs" / "codegen" / "c11" / "include"
    flags = [*host_cc.STRICT_C11, "-O2", "-Wno-misleading-indentation", "-I", str(include), "-I", str(workdir)]
    started = time.monotonic()
    procs = [subprocess.Popen([cc, *flags, "-c", str(u), "-o", str(u.with_suffix(".o"))], stderr=subprocess.PIPE, text=True)
             for u in units]
    failed = sum(1 for p in procs if p.wait() != 0)
    compile_wall = time.monotonic() - started
    if failed:
        return {"error": "compile"}
    emitted_digest = hashlib.sha256()
    for u in units:
        emitted_digest.update(u.read_bytes())
    return {"units": len(units), "emit_seconds": round(emit_seconds, 2), "compile_wall_seconds": round(compile_wall, 2),
            "emitted_bytes": sum(u.stat().st_size for u in units), "object_bytes": sum(u.with_suffix(".o").stat().st_size for u in units),
            "_digest": emitted_digest.hexdigest()}


def load_epochs(directory):
    directory = pathlib.Path(directory)
    epochs = []
    for ram_path in sorted(directory.glob("epoch-*.ram")):
        written_path = ram_path.with_suffix(".written")
        epochs.append((ram_path.read_bytes(), written_path.read_bytes()))
    return epochs


def derive(args):
    epochs = load_epochs(args.dir)
    out = {"epochs": len(epochs), "signature_classes": None, "content_classes": None, "images": []}
    sigs = [signature(r, w) for r, w in epochs]
    hashes = [content_hash(r) for r, _ in epochs]
    out["signature_classes"] = len(set(sigs))
    out["content_classes"] = len(set(hashes))
    out["written_extent_counts"] = [len(extents(w)) for _, w in epochs]
    out["written_bytes"] = [sum(length for _, length in extents(w)) for _, w in epochs]
    out["nonzero_bytes"] = [sum(1 for b in r if b) for r, _ in epochs]
    seen = {}
    for index, (ram, written) in enumerate(epochs):
        sig = sigs[index]
        if sig in seen:
            out["images"].append({"epoch": index + 1, "reuses_image_of_epoch": seen[sig] + 1})
            continue
        seen[sig] = index
        digests = []
        metrics = None
        for repeat in range(args.repeats):
            with tempfile.TemporaryDirectory() as tmp:
                metrics = emit_and_compile(args.emitter, ram, tmp, args.cc)
            digests.append(metrics.pop("_digest", None))
            if "error" in metrics:
                break
        metrics["repeats"] = args.repeats
        metrics["deterministic"] = len(set(digests)) == 1
        metrics["epoch"] = index + 1
        out["images"].append(metrics)
    print(json.dumps(out, sort_keys=True))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    d = sub.add_parser("derive")
    d.add_argument("--dir", required=True)
    d.add_argument("--emitter", required=True)
    d.add_argument("--cc")
    d.add_argument("--repeats", type=int, default=3)
    d.set_defaults(func=derive)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
