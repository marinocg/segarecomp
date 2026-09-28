# ADR 0050: Consumer Launcher and Bundled C Toolchain

- Status: Accepted
- Date: 2026-09-28
- Related, unchanged: the generated-native/AOT architecture (no interpreter, JIT, runtime opcode decoder or host
  machine-code emitter); ADR 0046 (opt-in generated object cache, a developer tool, unrelated to this cache).

## Context

The only supported way to run a ROM was the developer bridge (`tools/genesis_startup_bridge.py`), which needs Python, a
host C compiler and, for the viewer, an installed SDL3. A normal user has none of these.

## Decision

1. **`segarecomp build`** (in the existing CLI) is the native counterpart of the bridge's `--viewer` route: it runs
   the same `emit-general-startup-bridge-c --reset-entry --immutable-rom-aot` emit route in-process, compiles the
   sharded output plus the runtime (and, with `--sdl3-*`, the viewer) with a caller-supplied compiler, and links. It
   prints `@stage`/`@result` lines, writes `status.json` and `build.log`, and exits non-zero on failure (1 ROM/translation,
   2 usage/runtime files, 3 compiler/link). It is orchestration only. The bridge remains the developer tool (it has the
   diagnosis, expansion and copy-alias features the consumer path deliberately does not).
2. **Launcher** (`apps/segarecomp-launcher`, opt-in `SEGARECOMP_BUILD_LAUNCHER`): SDL3 + Dear ImGui (SDL renderer backend
   only). `launcher_core` (paths, cache, spawn build/game via SDL's process API) is UI-free and drives a non-interactive
   `--build <rom> [--run] [--report file]` mode used by the package smoke test. No library depends on it.
3. **Bundled compiler: Zig 0.15.2 used only as `zig cc`**, invoked with an explicit `-target` (native detection lags new OS
   releases: on macOS 26 it fails to find libSystem). Chosen because one small, MIT-licensed, relocatable archive per host
   compiles and links C11 without any system SDK, headers or linker, including for `windows-gnu`. Alternatives (shipping
   clang+lld+sysroots, or MinGW/GCC per host) are larger and need per-host sysroot assembly. Nothing else is written in Zig.
4. **Runtime is compiled from packaged sources per game** (mirroring the repo paths the runtime headers include), not
   prebuilt: a prebuilt-object/ABI seam is a larger contract than the first release justifies. Recorded as future work.
5. **Per-user cache** `games/<key>/` with key = SHA-256 of (ROM SHA-256, launcher version, platform, host arch/OS,
   Zig version, package location, optimization). The ROM is referenced, never copied. Builds go to `<key>.partial` and are
   renamed on success; a failed attempt is kept as `<key>.failed` for diagnostics. A changed input selects a new entry.
6. **Packaging** is `cmake --install` (`packaging/install.cmake`); SDL3 ships as a shared library in `sdl3/lib`, found by
   the launcher through an rpath and by the game through an rpath (POSIX) or `PATH` (Windows). Releases are built by
   `.github/workflows/release.yml` from pinned, checksum-verified inputs and smoke-tested from the extracted archive.

## Consequences

Large commercial ROMs generate very large C; the first build takes minutes (documented). The package is large
(Zig's `lib/`); trimming it is future work. Windows paths must fit the system code page (the CLI is ANSI-argv based).
