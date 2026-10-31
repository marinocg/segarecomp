#!/usr/bin/env python3
"""SEG-049-T007 (ADR 0106): the extracted release package needs no Python, ML framework or research model file.

usage: dependency_check.py <extracted-package-root> <launcher-executable>

CI glue (not shipped). Checks, on the extracted package of the running platform:
  1. the native dynamic dependencies of the launcher and of `segarecomp` (ELF NEEDED via readelf, Mach-O via otool -L, PE import table parsed in
     pure Python) are each either packaged (found under the package) or an operating-system library, and none matches the forbidden list
     (Python, PyTorch/libtorch, NumPy, ONNX Runtime, TensorFlow, scikit-learn/SciPy, BLAS/MKL/CUDA runtimes, and a dynamic zlib: zlib is vendored and
     statically linked, ADR 0096);
  2. the package contains no Python source/bytecode/interpreter, no ML framework file and no research model/weights file (`*.model.json`, `*.onnx`,
     `*.pt`, `*.npz`, ...);
  3. no BIOS, disc image / CUE sheet, generated Sega CD program or Sega CD runtime source (SEG-078-T002);
  4. the frozen native models are embedded in `segarecomp`: the v1 feature-schema digest, the gen2 model digest and the SMS Z80 D512HP digests are present in the binary.
It prints an aggregate JSON report; exit 1 on any violation.
"""
import json
import os
import pathlib
import re
import struct
import subprocess
import sys

FORBIDDEN_LIB = re.compile(r"python|libtorch|torch_|libc10|(^|[/\\])c10\.dll|numpy|onnx|tensorflow|sklearn|scipy|libmkl|mkl_|openblas|libblas|liblapack|cudart|cublas|cudnn|(^|[/\\])libz[.\-]|zlib1|libz\.|(^|[/\\])z\.dll", re.I)
# Windows: a redistributable C/C++ runtime is never assumed on a clean machine (the CRT is linked statically; libusb is the MinGW build).
REDISTRIBUTABLE_DLL = re.compile(r"^(vcruntime|msvcp|msvcr\d|concrt|vccorlib|libgcc|libstdc|libwinpthread)", re.I)  # msvcrt.dll is an OS component
FORBIDDEN_FILE = re.compile(r"(\.pyc?$|\.pyd$|\.npz?$|\.onnx$|\.pt$|\.pth$|\.safetensors$|\.gguf$|\.tflite$|\.h5$|\.pkl$|\.joblib$|\.ckpt$|\.pb$|\.model\.json$|^python[0-9.]*(\.exe)?$|(^|[/\\])site-packages|segarecomp_ml_region|libpython|torch|numpy|sklearn)", re.I)
# SEG-078-T002 (ADR 0204): a Sega CD game is the user's BIOS and disc plus a program generated from them. None of it is ever bundled: no disc image or CUE sheet,
# no BIOS, no generated viewer/headless Sega CD program, no run report. SEG-078-T003 (ADR 0205): the project-authored Sega CD RUNTIME SOURCES
# (platforms/sega-cd/{runtime,runtime/z80,viewer}/*.c|*.h) ship, as the Genesis ones do, because the launcher compiles the user's program with them; any other file under a
# `sega-cd` folder (a binary, a table, a report, a program, a disc) is still refused.
FORBIDDEN_COMMERCIAL = re.compile(r"(\.cue$|\.iso$|\.chd$|\.mdf$|\.ccd$|\.mds$|bios[^/\\]*\.(bin|rom|md|gen|zip|img|7z)$|program-viewer|(^|[/\\])sega-cd([/\\]|$)(?!(runtime[/\\]z80|runtime|viewer)[/\\][^/\\]+\.[ch]$)|last-report\.json$|\.describe$)", re.I)
MODEL_DIGESTS = {  # embedded in `segarecomp` (the frozen native models), see ADR 0096 / 0102
    "gen2 model": b"11258708285e4427a3e38e02732e8f93b4c5d497883101f0629223d2366a98e1",
    "v1 feature schema": b"d2e7c82913139c29450511326d6de76e91579ec654edde81afae16b13cd1f570",
    # SEG-057-T002 (ADR 0119): the frozen SMS Z80 D512HP ensemble (digest of the original SEG-056 ensemble and of the embedded artifact).
    "sms z80 d512hp ensemble": b"6bee77726a93ad33504522c35b84aaa6d89ed3c5327ff5fae6bff1e9558a4f11",
    "sms z80 d512hp artifact": b"81434de0da4755654e14c370fc32de7898532193cbc5a514033268ba6669c2ac",
}


def pe_imports(path):
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit(f"{path}: not a PE file")
    n_sections = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)
    import_rva = struct.unpack_from("<I", data, dd + 8)[0]
    sections = [struct.unpack_from("<IIII", data, opt + opt_size + 40 * i + 8) for i in range(n_sections)]  # vsize, vaddr, rawsize, rawptr

    def rva(value):
        for vsize, vaddr, rawsize, rawptr in sections:
            if vaddr <= value < vaddr + max(vsize, rawsize):
                return value - vaddr + rawptr
        raise SystemExit(f"{path}: bad RVA")

    names = []
    if import_rva:
        at = rva(import_rva)
        while True:
            fields = struct.unpack_from("<IIIII", data, at)
            if fields[3] == 0:
                break
            name_at = rva(fields[3])
            names.append(data[name_at:data.index(b"\0", name_at)].decode("ascii", "replace"))
            at += 20
    return names


def dependencies(path, root):
    """[(name, kind)] with kind in {'package', 'system'}; raises on an unresolvable non-system dependency."""
    found = []
    pack_names = {p.name.lower() for p in root.rglob("*") if p.is_file()}
    if sys.platform.startswith("linux"):
        needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", subprocess.run(["readelf", "-d", str(path)], capture_output=True, text=True, check=True).stdout)
        allowed = re.compile(r"^(libc|libm|libdl|libpthread|librt|libutil|libstdc\+\+|libgcc_s)\.so(\.[0-9]+)*$|^ld-linux[\w.\-]*\.so(\.[0-9]+)*$|^linux-vdso")
        for name in needed:
            found.append((name, "package" if name.lower() in pack_names else "system" if allowed.match(name) else "unresolved"))
    elif sys.platform == "darwin":
        lines = subprocess.run(["otool", "-L", str(path)], capture_output=True, text=True, check=True).stdout.splitlines()[1:]
        for line in lines:
            name = line.strip().split(" (")[0]
            base = name.rsplit("/", 1)[-1].lower()
            if name.startswith("@rpath/") or name.startswith("@loader_path") or name.startswith("@executable_path"):
                found.append((name, "package" if base in pack_names else "unresolved"))
            else:
                found.append((name, "system" if name.startswith(("/usr/lib/", "/System/")) else "unresolved"))
    else:
        system32 = pathlib.Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
        for name in pe_imports(path):
            lower = name.lower()
            if lower in pack_names:
                found.append((name, "package"))
            elif lower.startswith(("api-ms-win-", "ext-ms-win-")) or (system32 / name).exists():
                found.append((name, "system"))
            else:
                found.append((name, "unresolved"))
    return found


def main():
    root, launcher = pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]).resolve()
    cli = next(p for p in root.rglob("*") if p.is_file() and p.name in ("segarecomp", "segarecomp.exe") and p.parent.name == "bin")
    problems, report = [], {"binaries": {}}
    binaries = [("launcher", launcher), ("cli", cli)]
    if os.name == "nt":  # the packaged SDL3 and libusb DLLs (beside the launcher and in sdl3/lib) obey the same rules
        binaries += [(p.relative_to(root).as_posix(), p) for p in sorted(list(root.glob("*.dll")) + list((root / "sdl3" / "lib").glob("*.dll")))]
    for label, path in binaries:
        deps = dependencies(path, root)
        report["binaries"][label] = [{"name": n, "kind": k} for n, k in deps]
        for name, kind in deps:
            if FORBIDDEN_LIB.search(name):
                problems.append(f"{label}: forbidden dependency {name}")
            if os.name == "nt" and REDISTRIBUTABLE_DLL.match(name):
                problems.append(f"{label}: depends on the redistributable runtime {name}")
            if kind == "unresolved":
                problems.append(f"{label}: dependency {name} is neither packaged nor an operating-system library")
    files = [p for p in root.rglob("*") if p.is_file() or p.is_symlink()]  # dangling symlinks count
    for p in files:
        rel = p.relative_to(root).as_posix()
        if FORBIDDEN_FILE.search(p.name) or FORBIDDEN_FILE.search(rel):
            problems.append(f"forbidden file in the package: {rel}")
        if FORBIDDEN_COMMERCIAL.search(rel):
            problems.append(f"commercial input or generated Sega CD program in the package: {rel}")
    report["files"] = len(files)
    blob = cli.read_bytes()
    report["embedded_models"] = {}
    for label, digest in MODEL_DIGESTS.items():
        present = digest in blob
        report["embedded_models"][label] = present
        if not present:
            problems.append(f"the {label} digest is not embedded in {cli.name}")
    for binary in (cli, launcher):
        content = blob if binary == cli else binary.read_bytes()
        for needle in (b"libpython", b"libtorch", b"onnxruntime", b"numpy.core"):
            if needle in content:
                problems.append(f"{binary.name} references {needle.decode()}")
    print(json.dumps(report, indent=1, sort_keys=True))
    if problems:
        print("\n".join(problems), file=sys.stderr)
        sys.exit(1)
    print("dependency check: OK")


if __name__ == "__main__":
    main()
