#!/usr/bin/env python3
"""SEG-137-T001 (ADR 0294): the vendored, pinned Luau build.
Checks: the pin record and the digest of every vendored file; license notices and the package contents; no network in the build; native code generation (JIT) is not vendored, not built and
absent from the built archive and from every executable that links the VM; the disallowed standard libraries (os, debug, coroutine) are not built either.
usage: luau_build_test.py <source-root> <libsegarecomp_luau> <luau_host_test> <segarecomp-cli>"""
import hashlib
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
archive, host_test, cli_exe = (Path(a) for a in sys.argv[2:5])
failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL:", what)


luau = root / "third_party" / "luau"
pin = (luau / "PIN.txt").read_text()
check(re.search(r"^release:\s+0\.746$", pin, re.M) and re.search(r"^commit:\s+[0-9a-f]{40}$", pin, re.M) and re.search(r"^archive sha256:\s+[0-9a-f]{64}$", pin, re.M), "PIN.txt records the release, commit and archive digest")
check("12a6ba75883272c79b66b59815efef4c574196f6" in pin and "5bdfb1fb6f5bbd0af7056ba07f4e91043dd5615a7a9a61ed1498f8f67b991ac1" in pin, "the pinned commit and archive digest")

# every vendored file is exactly what was recorded; nothing else is vendored
sums = {}
for ln in (luau / "SHA256SUMS").read_text().splitlines():
    digest, name = ln.split("  ", 1)
    sums[name] = digest
files = {p.relative_to(luau).as_posix() for d in ("Common", "Ast", "Bytecode", "Compiler", "VM") for p in (luau / d).rglob("*") if p.is_file()} | {"LICENSE.txt", "lua_LICENSE.txt"}
check(set(sums) == files, f"SHA256SUMS lists exactly the vendored files (diff: {sorted(set(sums) ^ files)[:5]})")
bad = [n for n in sums if n in files and hashlib.sha256((luau / n).read_bytes()).hexdigest() != sums[n]]
check(not bad, f"vendored files are unmodified: {bad[:5]}")
own = {"CMakeLists.txt", "PIN.txt", "SHA256SUMS"}
extra = sorted(p.name for p in luau.iterdir() if p.is_file() and p.name not in own | {"LICENSE.txt", "lua_LICENSE.txt"})
check(not extra and sorted(p.name for p in luau.iterdir() if p.is_dir()) == ["Ast", "Bytecode", "Common", "Compiler", "VM"], "only the five retained directories are vendored")
check(not (luau / "CodeGen").exists() and not any("codegen" in str(p).lower() for p in luau.rglob("*") if p.is_file() and p.suffix in (".cpp", ".h") and "CodeGen" in p.parts), "no CodeGen directory is vendored")

# reproducible, offline build recipe
cm = (luau / "CMakeLists.txt").read_text()
recipe = "\n".join(l for l in cm.splitlines() if not l.lstrip().startswith("#"))
for forbidden in ("FetchContent", "ExternalProject", "DOWNLOAD", "git clone", "find_package"):
    check(forbidden not in recipe and forbidden not in (root / "libs" / "luau_host" / "CMakeLists.txt").read_text(), f"the build never downloads ({forbidden})")
check("CodeGen" not in recipe, "no CodeGen target or source in the build recipe")
check("linit|loslib|ldblib|lcorolib" in recipe, "the disallowed library sources are excluded from the build")
check("SEGARECOMP_LUAU_CODEGEN=0" in recipe, "the build defines SEGARECOMP_LUAU_CODEGEN=0")

# license files, notice and package contents
mit = (luau / "LICENSE.txt").read_text()
check("MIT License" in mit and "Roblox Corporation" in mit and (luau / "lua_LICENSE.txt").read_text().strip() != "", "the license files are present")
notices = (root / "packaging" / "THIRD-PARTY-NOTICES.txt").read_text()
check(re.search(r"Luau 0\.746", notices) and "Luau.txt" in notices and "Lua.txt" in notices and "third_party/luau" in notices, "THIRD-PARTY-NOTICES.txt attributes Luau 0.746 and names the shipped license files")
install = (root / "packaging" / "install.cmake").read_text()
check("third_party/luau/LICENSE.txt" in install and "RENAME Luau.txt" in install and "third_party/luau/lua_LICENSE.txt" in install and "RENAME Lua.txt" in install, "the product package installs both license texts")
readme = (root / "packaging" / "README.txt").read_text()
check("Luau" in readme, "the package README lists Luau")

# native code generation and the disallowed libraries are absent from the build outputs
# The archive holds only the pinned Luau: any CodeGen mention there would be Luau's native code generator. Executables also contain the product's own (unrelated) C code generator, so they are
# checked for Luau's specific names.
for needle in (b"CodeGen", b"codegen", b"luau_codegen", b"luacodegen", b"AssemblyBuilderX64", b"AssemblyBuilderA64"):
    check(needle not in archive.read_bytes(), f"the built archive contains no {needle.decode()}")
for needle in (b"Luau::CodeGen", b"luau_codegen", b"luacodegen", b"AssemblyBuilderX64", b"AssemblyBuilderA64", b"NativeContext", b"luau_codegen_create", b"CodeGenCompilationResult"):
    for exe in (host_test, cli_exe):
        check(needle not in exe.read_bytes(), f"{exe.name} contains no {needle.decode()}")
for needle in (b"luaopen_os", b"luaopen_debug", b"luaopen_coroutine", b"luaL_openlibs"):
    check(needle not in archive.read_bytes(), f"the built archive has no {needle.decode()}")
for needle in (b"luau_compile", b"luau_load", b"luaopen_base", b"luaopen_math", b"lua_newstate"):
    check(needle in archive.read_bytes(), f"the built archive contains {needle.decode()} (sanity)")

if failures:
    raise SystemExit(f"luau_build_test: {len(failures)} failure(s)")
print("luau_build_test: ok")
