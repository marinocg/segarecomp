#!/usr/bin/env python3
"""SEG-049-T007 (ADR 0106): the package dependency/file policy of packaging/dependency_check.py, on synthetic names only (no package needed).

Forbidden: Python / ML framework / BLAS / CUDA / dynamic-zlib libraries, interpreter and bytecode files, research model/weights files. Allowed: ordinary system and
packaged names (libSDL3, libstdc++, KERNEL32.dll, libSystem.B.dylib, SDL3.dll ...). Also checks the PE import-table parser on a minimal synthetic PE image.
"""
import importlib.util
import pathlib
import struct
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("dependency_check", root / "packaging" / "dependency_check.py")
dc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dc)

bad_libs = ["libpython3.12.so.1.0", "python312.dll", "libtorch_cpu.so", "libc10.so", "numpy.core._multiarray_umath.so", "onnxruntime.dll", "libonnxruntime.so.1",
            "libtensorflow.so.2", "libmkl_rt.so", "libopenblas.so.0", "libcudart.so.12", "libcublas.so", "libz.so.1", "zlib1.dll", "/usr/lib/libz.1.dylib", "libz.dylib",
            "libscipy_openblas.so", "libsklearn.so"]
good_libs = ["libSDL3.so.0", "libstdc++.so.6", "libm.so.6", "libc.so.6", "libgcc_s.so.1", "KERNEL32.dll", "SDL3.dll", "/usr/lib/libSystem.B.dylib",
             "/usr/lib/libc++.1.dylib", "@rpath/libSDL3.0.dylib", "api-ms-win-crt-runtime-l1-1-0.dll", "ld-linux-x86-64.so.2", "libdl.so.2", "libpthread.so.0"]
for name in bad_libs:
    assert dc.FORBIDDEN_LIB.search(name), f"must be forbidden: {name}"
for name in good_libs:
    assert not dc.FORBIDDEN_LIB.search(name), f"must be allowed: {name}"
bad_files = ["a/b/model.py", "x.pyc", "ext.pyd", "tools/segarecomp_ml_region_gen2.model.json", "w.onnx", "w.pt", "w.npz", "python3", "python.exe",
             "lib/site-packages/x", "libpython3.so", "torch/lib.so", "numpy/__init__.py"]
good_files = ["Segarecomp", "bin/segarecomp", "bin/segarecomp.exe", "runtime/platforms/genesis/runtime/runtime.c", "compat/abc.json", "licenses/zlib.txt",
              "sdl3/lib/libSDL3.so.0", "toolchain/zig", "assets/fonts/Silkscreen.ttf", "README.txt", "SDL3.dll"]
for name in bad_files:
    assert dc.FORBIDDEN_FILE.search(name), f"must be forbidden: {name}"
for name in good_files:
    assert not dc.FORBIDDEN_FILE.search(name), f"must be allowed: {name}"


def tiny_pe(imports):
    """A minimal PE32+ with one section holding the import directory (names only)."""
    section_rva, raw = 0x1000, 0x200
    descriptor_size = 20 * (len(imports) + 1)
    name_offsets, blob = [], b""
    for n in imports:
        name_offsets.append(descriptor_size + len(blob))
        blob += n.encode() + b"\0"
    table = b"".join(struct.pack("<IIIII", 0, 0, 0, section_rva + off, 0) for off in name_offsets) + b"\0" * 20 + blob
    opt = bytearray(240)
    struct.pack_into("<H", opt, 0, 0x20B)
    struct.pack_into("<II", opt, 112 + 8, section_rva, len(table))
    coff = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, len(opt), 0x22)
    header = b"MZ" + b"\0" * 0x3A + struct.pack("<I", 0x40) + b"PE\0\0" + coff + bytes(opt)
    section = struct.pack("<8sIIIIIIHHI", b".idata", len(table), section_rva, len(table), raw, 0, 0, 0, 0, 0)
    image = header + section
    image += b"\0" * (raw - len(image)) + table
    return image


with tempfile.TemporaryDirectory() as tmp:
    path = pathlib.Path(tmp) / "x.exe"
    path.write_bytes(tiny_pe(["KERNEL32.dll", "SDL3.dll", "python312.dll"]))
    assert dc.pe_imports(path) == ["KERNEL32.dll", "SDL3.dll", "python312.dll"], dc.pe_imports(path)
    path.write_bytes(tiny_pe([]))
    assert dc.pe_imports(path) == []
print("packaging_dependency_check_test: ok")
