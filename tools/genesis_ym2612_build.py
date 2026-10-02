#!/usr/bin/env python3
"""SEG-032-T007 (ADR 0074): builds the objects of the Genesis YM2612 device (vendored ymfm + the C ABI wrapper + the C++-runtime shim)
for test programs that link a Genesis runtime with sound. The result is linkable by a C driver alone (no C++ runtime library) - the shape
`segarecomp build` uses with the bundled Zig toolchain. Objects are cached per source/flag hash in `cache_dir` (default: `$SEGARECOMP_Z80_BUILD_CACHE/ym2612`, the build
tree's shared test cache, when that variable is set, else workdir): a test process that finds an object another test of the same build tree already compiled (identical sources, headers, compiler and flags) links it
instead of compiling the vendored core again.

  build_objects(cc, cxx, root, workdir, cache_dir=None) -> [object paths]
"""
import hashlib
import os
import pathlib
import subprocess

SOURCES = ("third_party/ymfm/ymfm_opn.cpp", "third_party/ymfm/ymfm_adpcm.cpp", "third_party/ymfm/ymfm_ssg.cpp", "src/ym2612.cpp", "src/cxx_runtime_shim.c")


def build_objects(cc, cxx, root, workdir, cache_dir=None, opt="-O1"):
    root, workdir = pathlib.Path(root), pathlib.Path(workdir)
    device = root / "libs/device/sega/ym2612"
    vendor = device / "third_party/ymfm"
    shared = os.environ.get("SEGARECOMP_Z80_BUILD_CACHE")
    cache = pathlib.Path(cache_dir) if cache_dir else (pathlib.Path(shared) / "ym2612" if shared else workdir)
    cache.mkdir(parents=True, exist_ok=True)
    headers = b"".join(p.read_bytes() for p in sorted((device / "include").rglob("*.h")) + sorted(vendor.glob("*.h")) + sorted(vendor.glob("*.ipp")))
    objects = []
    for relative in SOURCES:
        source = device / relative
        is_cxx = source.suffix == ".cpp"
        compiler = cxx if is_cxx else cc
        flags = ["-std=c++14", "-fno-exceptions", "-fno-rtti", "-w"] if (is_cxx and "third_party" in relative) else (
            ["-std=c++14", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-isystem", str(vendor)] if is_cxx else ["-std=c11", "-Wall", "-Wextra"])
        key = hashlib.sha256(source.read_bytes() + headers + " ".join([compiler, opt, *flags]).encode()).hexdigest()[:16]
        obj = cache / ("ym2612_%s_%s.o" % (source.stem, key))
        if not obj.exists():
            partial = obj.with_suffix(".tmp%d.o" % os.getpid())  # per process: concurrent tests may fill the same shared entry
            done = subprocess.run([compiler, opt, *flags, "-I", str(device / "include"), "-I", str(vendor), "-c", str(source), "-o", str(partial)],
                                  text=True, capture_output=True)
            if done.returncode != 0:
                raise RuntimeError("ym2612 build failed: " + done.stderr[:2000])
            partial.replace(obj)
        objects.append(obj)
    return objects
