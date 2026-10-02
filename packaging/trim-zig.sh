#!/usr/bin/env bash
# Strip the pinned Zig release down to only what one narrow, fixed use case needs: `zig cc -target
# <exact-host-target>` compiling/linking plain C11 (the generated game program, this project's own
# runtime, and the optional SDL3 viewer sources) -- never Zig source, never any other target triple.
#
# usage: trim-zig.sh <zig-dir> <linux|macos|windows>
#
# Zig's own lib/ ships complete cross-compilation support for every OS/libc/architecture it can target,
# which is most of a pinned release's size (lib/libc/include alone holds a full header set per target
# triple). A package only ever invokes zig for its own single host, so every other OS family's libc/
# headers, plus Zig's own C++ runtime support (never used: we compile no C++) and documentation, are
# dead weight. `lib/std` is the one directory that looks safe to remove but is not: zig uses it to find
# its own installation directory and fails outright without it, even for a plain C compile.
#
# This split (categorical OS-family removal only, never touching a kept OS's own per-architecture
# subdirectories) was verified empirically, not guessed: on Linux (215M -> 53M) and macOS (209M -> 41M)
# by compiling+linking+running this project's own generated C against the trimmed tree, including the
# optional SDL3 viewer variant, with both -O0 and -O2. The Windows shape (mingw + any-windows-any
# preserved, which are themselves large: Windows' own headers/CRT source do not shrink) is the same
# pattern applied symmetrically; it is verified by the release workflow's own Windows package smoke
# test, run on a real Windows CI runner, rather than locally (no Windows/Wine environment available
# here).
set -euo pipefail
dir="$1"; keep="$2"
lib="$dir/lib"
if [ ! -d "$lib/std" ]; then echo "trim-zig.sh: $lib does not look like a Zig release (no lib/std)" >&2; exit 2; fi

case "$keep" in
  linux)
    rm -rf "$lib/libc/mingw" "$lib/libc/darwin" "$lib/libc/wasi" "$lib/libc/freebsd" "$lib/libc/netbsd" "$lib/libc/musl"
    rm -rf "$lib"/libc/include/any-windows-any "$lib"/libc/include/any-macos-any \
           "$lib"/libc/include/generic-netbsd "$lib"/libc/include/generic-freebsd "$lib"/libc/include/generic-musl \
           "$lib"/libc/include/wasm-wasi-musl
    find "$lib/libc/include" -maxdepth 1 -type d \( -name "*-freebsd-*" -o -name "*-netbsd-*" -o -name "*-musl*" \) -exec rm -rf {} +
    ;;
  macos)
    rm -rf "$lib/libc/mingw" "$lib/libc/glibc" "$lib/libc/musl" "$lib/libc/freebsd" "$lib/libc/netbsd" "$lib/libc/wasi"
    rm -rf "$lib"/libc/include/any-windows-any "$lib"/libc/include/any-linux-any "$lib"/libc/include/generic-glibc \
           "$lib"/libc/include/generic-musl "$lib"/libc/include/generic-netbsd "$lib"/libc/include/generic-freebsd \
           "$lib"/libc/include/wasm-wasi-musl
    find "$lib/libc/include" -maxdepth 1 -type d \( -name "*-linux-*" -o -name "*-freebsd-*" -o -name "*-netbsd-*" -o -name "*-windows-*" \) -exec rm -rf {} +
    ;;
  windows)
    rm -rf "$lib/libc/darwin" "$lib/libc/glibc" "$lib/libc/musl" "$lib/libc/freebsd" "$lib/libc/netbsd" "$lib/libc/wasi"
    rm -rf "$lib"/libc/include/any-macos-any "$lib"/libc/include/any-linux-any "$lib"/libc/include/generic-glibc \
           "$lib"/libc/include/generic-musl "$lib"/libc/include/generic-netbsd "$lib"/libc/include/generic-freebsd \
           "$lib"/libc/include/wasm-wasi-musl
    find "$lib/libc/include" -maxdepth 1 -type d \( -name "*-linux-*" -o -name "*-freebsd-*" -o -name "*-netbsd-*" -o -name "*-macos-*" \) -exec rm -rf {} +
    ;;
  *)
    echo "usage: trim-zig.sh <zig-dir> <linux|macos|windows>" >&2
    exit 2
    ;;
esac
# Never used: neither the sanitizer runtime, the web playground tool, nor `zig init`'s templates are reachable from a
# plain `zig cc` invocation. SEG-032-T007 (ADR 0074): the vendored YM2612 core is C++ and is compiled by the package with
# `zig c++` WITHOUT a C++ runtime (the C-driver link uses src/cxx_runtime_shim.c), so ONLY the libc++/libc++abi HEADERS are
# kept (lib/libcxx/include, lib/libcxxabi/include); their sources, tests and the unwinder stay removed.
rm -rf "$lib/docs" "$lib/libtsan" "$lib/libunwind" "$lib/build-web" "$lib/init"
for d in libcxx libcxxabi; do
  if [ -d "$lib/$d" ]; then
    find "$lib/$d" -mindepth 1 -maxdepth 1 ! -name include -exec rm -rf {} +
  fi
done
du -sh "$lib"
