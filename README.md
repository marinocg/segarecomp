# segarecomp

**Experimental static recompilation for Sega consoles.**

segarecomp explores a way to run Sega console software on modern computers without
interpreting the original CPU instructions while the program runs. It discovers and
translates supported instructions **ahead of time** into portable C11, then compiles
that C and links it with a native implementation of the console's hardware.

This is an open-source engineering project **and an experimental launcher**, not yet
a general-purpose replacement for a Sega emulator. Unsupported code or behavior
is reported explicitly rather than silently treated as working.

## How it works

```text
ROM / disc image
      |
      v
Image model -> static decode, lift and code discovery
      |
      v
Generated portable C11 -> native compilation
      |
      v
Generated-native program + console hardware runtime
```

The CPU code in a generated program is statically compiled: **no interpreter, no JIT,
no runtime target-opcode decoder, and no runtime code generation**. Runtime state
may select only among code generated and compiled from build-time inputs.
The runtime supplies machine behavior such as memory, devices and video. CPU-specific
semantics and machine/device behavior remain separate, so the same foundations can
be reused across platforms without assuming their hardware is identical.

For the detailed pipeline, see [the architecture overview](docs/architecture/pipeline.md).

## Project status

segarecomp has **fully playable commercial games** on Genesis / Mega Drive and
working Master System titles. Compatibility is not universal: games not listed
below may reach gameplay, encounter unsupported behavior or fail to build.

| Target | What exists today | Important limits |
| --- | --- | --- |
| **Genesis / Mega Drive** | Generated-native MC68000, Genesis runtime and video (VDP), device work and an optional SDL3 viewer. Sonic 1 is beatable end-to-end; Sonic 2, Sonic 3 and Streets of Rage 1 and 3 are fully playable. Many more games reach gameplay. | Compatibility is still incomplete for other titles. |
| **Z80 CPU** | Z80 decode/semantics and generated-C lowering components exist and are used by ongoing platform work. | CPU coverage, hardware integration and game compatibility remain experimental. |
| **Master System** | Generated-native Z80 path, C11 runtime, VDP/PSG/input components, headless support and optional viewer. Sonic 1 and 2, Wonder Boy III and Alex Kidd in Miracle World play well. | Compatibility varies by title; mapper selection is explicit. |
| **Game Gear** | Planned platform work. | No general compatibility claim. |
| **Sega CD** | **Synthetic** project-authored CUE/BIN discs with two generated-native MC68000 CPUs, CD/CDC and Word RAM behavior; **authentic BIOS execution** (your own BIOS image) with the generated-native boot and hand-off; **Sonic CD** is checked from the BIOS through the title, two START presses and the first level, with a timed RIGHT input, against an independent reference (checkpoint evidence, not a compatibility claim); a second title has only **experimental observations**. A program can be played **interactively in the shared viewer** (keyboard/gamepad, pause, frame step, input recording and exact replay) and the launcher can **build a program from your own BIOS and disc** (experimental, ADR 0205: the build discovers the disc spans the console loads and executes by itself) or play a **verified prebuilt program** that you import. | **No audio** (the Z80 does not execute; no PCM or CD-DA output). **Complete CUE-to-play compilation is not claimed**: the build reaches only the code it proved to come from the disc by running the console from reset with no input (generated, decompressed or later-input code stops with a typed stop, see [the Sega CD notes](docs/product/sega-cd-play.md)). BIOS, disc and programs are yours and are never bundled. |
| **32X / Saturn** | Roadmap targets. | No working platform implementation claimed. |

### Known working games

The following are concrete examples of games running through segarecomp, not
just synthetic tests or title-screen demonstrations:

- **Genesis / Mega Drive:** **Sonic the Hedgehog** is beatable from start
  to finish. **Sonic the Hedgehog 2**, **Sonic the Hedgehog 3**,
  **Streets of Rage** and **Streets of Rage 3** are fully playable.
  Many additional titles reach gameplay.
- **Master System:** **Sonic the Hedgehog**, **Sonic the Hedgehog 2**,
  **Wonder Boy III: The Dragon's Trap** and **Alex Kidd in Miracle World**
  run well.

These examples do not imply universal compatibility across all games or ROM
revisions. See the [platform-by-platform documentation](platforms/README.md)
for architecture and development details.

## Download and try segarecomp

The [Releases](../../releases) page provides experimental desktop launcher archives
for Windows (`segarecomp-<version>-windows-x86_64.zip`), Linux
(`-linux-x86_64.tar.gz`), and Apple Silicon macOS (`-macos-arm64.zip`).
Check the downloaded archive against `SHA256SUMS.txt`.

1. Extract the archive and open **Segarecomp** (`Segarecomp.exe`,
   `Segarecomp.app` or `./Segarecomp`, depending on your platform).
2. Drop a ROM into the window or select **Browse...**.
3. On the game's page, choose the build settings below, then select
   **Recompile & Play**. The first build can take several minutes for larger
   images; cached builds are faster to launch.

**Recommended first try (based on maintainer testing):**

- **Genesis / Mega Drive:** **Optimized** is the launcher's default **Build
  mode** for new games (it uses the model that earlier versions offered as
  *Experimental model*; there is no separate checkbox any more). It has worked
  well in the maintainer's game tests. For headless builds, the corresponding
  options are `--aot-policy optimized --aot-model gen2`.
- **Master System:** **Optimized** is also the default when the launcher's
  bundled product offers it (otherwise the game builds as Compatibility). This
  mode is still experimental. For headless builds, use `--aot-policy optimized`.

**Compatibility** is an explicit choice under **Build mode** and remains the
conservative fallback if an optimized build fails or a game behaves incorrectly.
The `segarecomp build` command-line default is unchanged (Compatibility). An Optimized
request can also fall back to Compatibility automatically; Master System may
restart in Compatibility for its supported unadmitted-code stop. These are
recommendations for what to try first, not guarantees for every game.

The packages include the compiler (a pinned Zig toolchain, used to compile generated
C) and SDL3. **No ROMs are included**; use only software you are legally entitled
to analyze. These are experimental builds: the launcher recognizes Genesis/Mega
Drive and Master System images. Several games already work well (see
[Known working games](#known-working-games)), while others may reach gameplay,
stop at unsupported behavior or fail to build.
An image without compatibility metadata may be labelled **Compatibility unknown**;
**Try anyway** is not a promise that it works.

### Launcher controls and useful options

- **Controls:** arrow keys, Z/X/C for A/B/C, Return for Start and Escape to quit.
  There is no key remapping yet.
- **Genesis region:** USA NTSC, Japan NTSC or Europe PAL. This currently changes
  the version register only; execution remains **60 Hz** even when Europe is
  selected, and the launcher indicates that 50 Hz is unsupported. The default
  follows the ROM header (a lone `E` is ambiguous and retains USA). The choice is
  applied at launch via `SEGARECOMP_GENESIS_REGION` without rebuilding. Headless:
  `--run --region <usa|japan|europe>`.
- **Advanced Genesis HInt alias discovery:** the optional, default-off
  **HInt alias discovery (strict build pass)** toggle applies a strict
  H-interrupt policy during build-time preparation. It can resolve a
  RAM-resident IRQ4 vector through the ADR 0049 copy-alias/RAM-thunk preparation.
  Because it changes generated code, it has a separate cache entry.
  Headless: `--hint-alias-discovery`.
- **Master System mapper:** mapper identity must be explicitly selected or
  supplied through the supported sidecar/manifest route; it is not guessed.
  See [Master System details](platforms/master-system/README.md).
- **Build cache and diagnostics:** generated builds live in
  `%LOCALAPPDATA%\Segarecomp\` (Windows),
  `~/Library/Application Support/Segarecomp/` (macOS) or
  `~/.local/share/Segarecomp/` (Linux). Directly selected ROM files are
  referenced rather than copied; library ZIP imports can extract their ROM
  into the managed library. On failure, **View diagnostics** shows the log,
  also saved as `build.log` under `games/<id>.failed/`, which is useful for
  bug reports.
- **macOS:** current packages are not notarized. Gatekeeper may require
  right-clicking the app and choosing **Open** the first time (the original
  workaround is `xattr -dr com.apple.quarantine Segarecomp.app`).
- **Linux:** release archives require glibc 2.35 or newer.
- **Third-party components:** bundled Zig, SDL3, Dear ImGui and other shipped
  dependencies have notices in the package's `licenses/` directory.

To build the launcher and distribution archives from source, see
[packaging/README.md](packaging/README.md). The developer build below is separate.

## Repository map

| Path | Contents |
| --- | --- |
| `libs/` | reusable libraries: core, MC68000 and Z80 CPU semantics, recompiler, C11 code generation, devices and media |
| `platforms/` | machine/runtime/viewer composition for Genesis, Master System and Sega CD (synthetic and authentic-BIOS); other targets have roadmap documentation |
| `apps/segarecomp` | the CLI (including `build`, the native ROM-to-executable route) |
| `apps/segarecomp-launcher` | opt-in consumer GUI launcher (SDL3 + Dear ImGui) |
| `packaging/` | release staging, package smoke test, pinned third-party inputs |
| `tools/` | Python developer tools (startup bridge, frontier debugging, Ghidra, baselines) |
| `tests/` | tests; the root `CMakeLists.txt` only orchestrates |
| `docs/` | architecture, decisions (ADRs), references, testing policy |

Further reading: [CONTRIBUTING.md](CONTRIBUTING.md), [platforms](platforms/README.md),
[pipeline](docs/architecture/pipeline.md).

## Build from source (developers)

### Quick start (all hosts)

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

### Supported hosts

`.github/workflows/ci.yml` builds and tests every change on all three hosts (configure with the
`dev` preset, build, then one cost-balanced shard of the hermetic suite per matrix job: six shards per host, labelled
`shard-1`..`shard-6` by `tests/ci_shards.cmake`, whose union is the `full` tier that contains `fast`; run one locally with
`ctest --preset dev -L '^shard-3$'`, and the whole partition is checked by `ci_shard_guard_test`); Ninja is the generator everywhere.

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

### General-startup bridge

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

### Architecture and contribution guide

The recompilation pipeline is intentionally deterministic and fail-closed:
unsupported instructions or unresolved executable paths must not turn into
silent no-ops. CPU-specific semantics belong to their CPU implementations;
machine/device behavior lives in the appropriate runtime and device libraries.
The aim is reusable foundations, not a speculative universal CPU abstraction.

For the complete design and validation contracts, see
[the pipeline documentation](docs/architecture/pipeline.md) and
[CONTRIBUTING.md](CONTRIBUTING.md) before making changes.

### Optional Ghidra debugging

**Ghidra is optional developer tooling for debugging and investigating
recompilation issues. It is not required to build segarecomp, recompile a game,
or launch and play games.** Normal game execution does not use Ghidra.

For developers investigating code discovery or other tricky failures, a pinned,
containerized headless Ghidra and Ghidra MCP bridge are available through
`python3 tools/ghidra.py`. See
[docs/testing/ghidra-analysis.md](docs/testing/ghidra-analysis.md).
No ROMs or host directories are mounted unless explicitly placed in the
ignored input directory.

## Legal and ROM policy

No commercial ROMs, firmware, keys, or vendor SDK material are distributed.
Bring only software you are legally entitled to analyze. Local commercial
images belong in the fully ignored `games/` directory; tests and CI use
project-authored synthetic inputs. See
[the commercial-game testing policy](docs/testing/commercial-games.md).

## License and third-party notices

segarecomp's own code is licensed under the
[Mozilla Public License 2.0](LICENSE). Third-party components retain their
respective licenses: for example, the repository includes vendored `ymfm`
code for YM2612 support under the BSD 3-Clause License. Consult the
upstream license in `libs/device/sega/ym2612/third_party/ymfm/` and
[packaging/README.md](packaging/README.md) for distribution details.

Release packages also redistribute components including SDL3 (zlib), Zig
(MIT, the bundled C compiler) and Dear ImGui (MIT); applicable notices ship
in their `licenses/` directory. No security-reporting contact policy has been
established yet, so there is no `SECURITY.md`.
