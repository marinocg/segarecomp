# segarecomp

`segarecomp` is an experimental global static recompiler for Sega consoles. It translates a
complete target program ahead of time into portable C11 that runs against a native machine runtime.

```text
target program -> image model -> static CPU decode/lift/discovery -> generated portable C -> native machine runtime
```

The generated program contains no interpreter, no JIT, no runtime target-opcode decoder, and no
runtime code generation. Runtime state may only select among code that was statically generated and
compiled from build-time inputs.

## Status

Experimental. Stated without exaggeration:

- **Genesis / Mega Drive:** a generated-native MC68000 path, Genesis runtime and video (VDP), an
  optional SDL3 live viewer, and a Sonic the Hedgehog 1 title-screen checkpoint work. General and
  gameplay compatibility is incomplete.
- **Z80:** next after the current workspace work (SEG-008); not implemented yet.
- **Master System / Game Gear:** planned.
- **Sega CD / 32X / Saturn:** roadmap only; no code.

See `platforms/README.md` for the per-platform map.

## Legal and ROM policy

No commercial ROMs, firmware, keys, or vendor SDK material are distributed. Bring only software you
are legally entitled to analyze. Local commercial images belong only in the fully ignored `games/`
directory; tests and CI use project-authored synthetic inputs. See `docs/testing/commercial-games.md`.

## License

Licensed under the [Mozilla Public License 2.0](LICENSE). The repository vendors no third-party
source, so no separate third-party notices are currently required. SDL3 (optional viewer) and other
tools are external prerequisites under their own licenses. No security-reporting contact policy has
been established yet, so there is no `SECURITY.md`.

## Repository map

| Path | Contents |
| --- | --- |
| `libs/` | reusable libraries: core, CPU semantics (`cpu/m68k`), recompiler, C11 codegen, devices, media |
| `platforms/` | machine products (`genesis/{machine,runtime,viewer,compat}`); other platforms are docs only |
| `apps/segarecomp` | the CLI |
| `tools/` | Python developer tools (startup bridge, frontier debugging, Ghidra, baselines) |
| `tests/` | tests; the root `CMakeLists.txt` only orchestrates |
| `docs/` | architecture, decisions (ADRs), references, testing policy |

Further reading: [CONTRIBUTING.md](CONTRIBUTING.md), [platforms](platforms/README.md),
[pipeline](docs/architecture/pipeline.md).

## Quick start (all hosts)

Requirements: CMake >= 3.25, Ninja, Python >= 3.10, a C/C++ toolchain (see "Supported hosts").
Commands below use `python3` (macOS/Linux); on Windows use `python` or `py -3`.

```sh
cmake --workflow --preset dev-build   # configure + build
cmake --workflow --preset dev-fast    # + fast tests   (dev-full: full hermetic suite)
python3 -c "open('build/quickstart.bin','wb').write(bytes.fromhex('4AB9 00A1 0008 4E70'))"
python3 tools/genesis_startup_bridge.py --rom build/quickstart.bin --entry 00000B00 --mode synthetic --out-dir build/quickstart
```

The second-to-last line writes a tiny project-authored synthetic image (it stops at an unsupported
instruction; that sanitized JSON stop is the expected result). The bridge finds
`build/dev/apps/segarecomp/segarecomp` (`.exe` on Windows) and a C compiler (`$CC`, else
`cc`/`clang`/`gcc` on PATH) automatically; pass `--segarecomp`/`--cc` explicitly for reproducibility.
For the optional authorized-local Genesis viewer add `--viewer` (needs SDL3; see below and
`docs/development/genesis-viewer.md`; the tool fails clearly if SDL3 is missing).
`python3 tools/genesis_startup_bridge.py --help` lists examples. No canonical command needs `xcrun`.

## Supported hosts

`.github/workflows/ci.yml` builds and tests every change on all three hosts (configure with the
`dev` preset, build, `fast` tests, then the full hermetic suite); Ninja is the generator everywhere.

| Host | Primary toolchain (CI-tested) | Generated-C compiler (`$CC`) |
| --- | --- | --- |
| macOS | AppleClang + Ninja (`brew install ninja`) | `cc` (`xcrun --find cc` optional) |
| Linux | GCC 13 + Ninja (Ubuntu 24.04: `apt install cmake ninja-build g++`) | `/usr/bin/cc` |
| Windows | LLVM/Clang + Ninja inside a Visual Studio developer prompt (`CC=clang CXX=clang++`) | `clang` |

MSVC `cl` is not yet supported (not tested). On Windows, Python runs in UTF-8 mode under CTest, and
the private full/ephemeral report pipes pass a native inherited HANDLE that the generated child
converts to its own CRT fd. The ordinary `full` gate has the same product/generated-execution
surface on all three hosts; the developer measurement test `genesis_experiment_aligned_aot_report_failure_test`
(built on POSIX `getrusage`) is in the `extended` tier. Local commercial inputs, Ghidra, and
Docker tests are never part of the CI matrix.

Optional SDL3 viewer: install SDL3 development files (`vcpkg install sdl3` on every host (Linux needs `autoconf autoconf-archive automake libtool libltdl-dev` plus
the X11/Wayland/audio dev packages listed in the CI job; macOS may alternatively use `brew install sdl3`)), then configure with `-DSEGARECOMP_ENABLE_SDL3_VIEWER=ON` (plus
`-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake` when using vcpkg) and build the
`segarecomp_viewer_genesis_sdl3` target. CI compiles it on all three hosts.

## General-startup bridge

The bounded generated-native bridge has one project-owned compile/run entry point:

```sh
python3 tools/genesis_startup_bridge.py --segarecomp build/dev/apps/segarecomp/segarecomp --cc "$CC" \
  --rom synthetic.bin --entry 00000B00 --mode synthetic
```

`$CC` is the host's strict-C11-capable C compiler (see "Supported hosts"). The generated program
reports privately through an inherited writer, never a file: on POSIX an inherited file descriptor;
on Windows an explicitly inherited native HANDLE (only the requested handles are listed to the
child) that the generated program converts in the child with `_open_osfhandle` + `_fdopen`. Nothing
private is persisted, and the same bridge tests run on Windows as on the other hosts.

It writes generated C and its executable only to a Git-ignored output directory, compiles with
strict C11 flags, and prints a sanitized JSON result. `--compare-runs` runs the same generated
binary twice and reports whether its results match. Commercial mode never accepts a full-report path.

## Design Direction

```text
ROM -> image model -> decode -> lifted IR -> whole-program analysis -> C emitter -> runtime
```

See `docs/architecture/pipeline.md` and [CONTRIBUTING.md](CONTRIBUTING.md) before making changes.

## Optional Ghidra Analysis

A pinned, containerized headless Ghidra and Ghidra MCP bridge are available through
`python3 tools/ghidra.py`. See [docs/testing/ghidra-analysis.md](docs/testing/ghidra-analysis.md); no ROMs or host directories are mounted
unless explicitly placed in the ignored input directory.
