# Consumer packaging

The release package is `cmake --install` of a Release build with `-DSEGARECOMP_BUILD_LAUNCHER=ON` plus the pinned
third-party inputs (`install.cmake`). `.github/workflows/release.yml` does exactly this per host (also on pull requests that touch packaging, without publishing; run it by hand with **Actions > release > Run workflow**), archives the result,
**extracts the archive into a clean directory and runs `smoke_test.py` against it**, then publishes archives and
`SHA256SUMS.txt` for a `v*` tag (a manual run is a dry run that only uploads artifacts).

## Pinned inputs

| Input | Version | Notes |
| --- | --- | --- |
| Zig (bundled `zig cc`) | 0.15.2 | official archives, SHA-256 in `release.yml`; MIT; used only to compile generated C |
| ymfm (vendored) | 81aec25 | YM2612 core, BSD-3-Clause; compiled with `zig c++` and linked without a C++ runtime (shim) |
| SDL3 | 3.4.16 | macOS/Linux built from the official source tarball (shared); Windows: official `VC` devel (launcher, DLL) and `mingw` devel (import lib for Zig) |
| Dear ImGui | 1.92.9b | fetched at configure time with a pinned hash (`apps/segarecomp-launcher/CMakeLists.txt`) |
| stb_image.h | pinned commit | fetched at configure time with a pinned hash; decodes the launcher's own PNG assets only |
| Silkscreen font | Google Fonts release | `apps/segarecomp-launcher/assets/fonts/`, SIL OFL 1.1 |

The generated program is always compiled with an explicit Zig `-target` (`aarch64-macos`, `x86_64-macos`,
`x86_64-linux-gnu.2.35`, `aarch64-linux-gnu.2.35`, `x86_64-windows-gnu`); see `apps/segarecomp-launcher/launcher_core.cpp`.

## Package size

`packaging/trim-zig.sh <zig-dir> <linux|macos|windows>` (run by `release.yml` right after fetching Zig, before
staging) removes the pinned Zig release's support for every OS/libc family the package's own single compilation
target will never use (a package only ever invokes `zig cc` for its own host). This was verified empirically, not
guessed: by compiling, linking and running this project's own generated C (including the SDL3 viewer variant, both
`-O0` and `-O2`) against the trimmed tree -- on Linux (215 MB -> 53 MB) and macOS (209 MB -> 41 MB) directly; the
same script's Windows behavior is verified by the release workflow's own Windows package smoke test (no local
Windows/Wine environment available). `lib/std` is deliberately never removed: Zig uses it to locate its own
installation directory and fails outright without it, even for a plain C compile.

The pinned `zig` executable itself (Zig's self-hosted compiler with LLVM statically linked in, ~180-190 MB
uncompressed) dominates both the installed footprint and the compressed archive size, and is not something this
project can safely trim: it is not a directory of removable per-target files, and empirically stripping its symbols
breaks it outright (verified locally: a stripped copy fails to compile anything). The `lib/` trim substantially
reduces the *extracted/installed* footprint (roughly halved); it has a smaller effect on the *compressed archive*
size specifically, because the removed header/source trees were already highly compressible text and the
irreducible `zig` binary was always the dominant compressed contributor.

SDL3's shared library is installed as two real file copies (never a symlink): the exact runtime dependency name the
built binaries embed (`libSDL3.so.0` / `libSDL3.0.dylib`) and the unversioned name the bundled linker searches for
to satisfy `-lSDL3` when it later builds each game (`libSDL3.so` / `libSDL3.dylib`). A previous `FILES_MATCHING`
directory copy staged SDL3's own symlink chain instead; some archivers/filesystems (observed: WSL's `DrvFs`)
extract a followed symlink as an empty or truncated file, producing "file too short" at load time.

Linux packages are built on Ubuntu 22.04 and need glibc >= 2.35 (the bundled SDL3 requires it; the generated program is linked against the same version).

The launcher's pixel-art (background, wordmark, icons) and the Silkscreen font live in
`apps/segarecomp-launcher/assets/` (see that directory's own `README.md`) and are installed into the
package's `assets/` directory (`packaging/install.cmake`); `apps/segarecomp-launcher/pixel_assets.*` loads
them at runtime, falling back to the source tree during development and to ImGui's built-in font/a flat
fill if an asset is missing.

## Layout

```text
Windows / Linux                       macOS
Segarecomp[.exe]  (launcher)          Segarecomp.app/Contents/MacOS/Segarecomp
SDL3.dll (Windows)                    Segarecomp.app/Contents/Resources/   (everything below)
bin/segarecomp[.exe]  (build driver)  bin/  toolchain/  runtime/  sdl3/  licenses/
toolchain/  (Zig)   sdl3/{include,lib}  runtime/{platforms,libs}  licenses/  README.txt
```

`runtime/` mirrors the repository paths the runtime headers include by relative path. It carries the Genesis runtime
(`platforms/genesis`) and the Master System runtime (`platforms/master-system/{runtime,headless,viewer}` plus
`libs/device/sega/psg` and the Z80 runtime ABI header under `libs/codegen/c11/include`); `segarecomp build --runtime-dir
<root>/platforms/master-system` derives the shared library sources from the same root. The Master System mapper is
declared (launcher selection control, `<rom>.mapper.json` sidecar, or `--mapper`), never inferred.
The Genesis build (SEG-032-T008) also derives `libs/device/sega/{psg,ym2612}` and the Z80 runtime ABI header from the same root: it
runs the build-time Z80 image materialization fixed point (several short headless runs of the program under the bundled compiler)
and compiles the vendored ymfm core with `<cc> c++` (derived from the `cc` argument, or given by `--cxx`/`--cxx-arg`).

## Local staging (macOS/Linux)

SDL3 comes from `packaging/build-sdl3.sh` (pinned source build; no Homebrew/vcpkg dependency, `@rpath` install name):

```sh
packaging/build-sdl3.sh /tmp/sdl-work /tmp/sdl3      # pinned source build, ~2 min
cmake -S . -B build/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release -DSEGARECOMP_BUILD_LAUNCHER=ON -DBUILD_TESTING=OFF \
  -DCMAKE_PREFIX_PATH=/tmp/sdl3 -DSEGARECOMP_PACKAGE_ZIG_DIR=<extracted zig> \
  -DSEGARECOMP_PACKAGE_SDL3_PREFIX=/tmp/sdl3 -DSEGARECOMP_PACKAGE_SDL3_LICENSE=/tmp/sdl3/LICENSE.txt
cmake --build build/launcher && cmake --install build/launcher --prefix stage
python3 packaging/smoke_test.py stage/Segarecomp.app/Contents/MacOS/Segarecomp
```

The launcher also has a non-interactive mode used by the smoke test:
`Segarecomp --build <rom> [--run] [--report <file>]`. `SEGARECOMP_CACHE_DIR` overrides the cache location.

## Deferred (deliberately out of the first release)

Signing/notarization and installers; prebuilt runtime objects (each game currently compiles the runtime sources);
further shrinking the pinned `zig` binary itself (verified unsafe, see "Package size" above); fullscreen/controller
settings; Windows arm64 and macOS x86_64 packages (no validated runner); cache eviction; `--discover-copy-aliases`
and other bridge-only analysis (consumer builds use the reset-entry immutable-ROM route only).
