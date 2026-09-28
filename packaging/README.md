# Consumer packaging

The release package is `cmake --install` of a Release build with `-DSEGARECOMP_BUILD_LAUNCHER=ON` plus the pinned
third-party inputs (`install.cmake`). `.github/workflows/release.yml` does exactly this per host (also on pull requests that touch packaging, without publishing; run it by hand with **Actions > release > Run workflow**), archives the result,
**extracts the archive into a clean directory and runs `smoke_test.py` against it**, then publishes archives and
`SHA256SUMS.txt` for a `v*` tag (a manual run is a dry run that only uploads artifacts).

## Pinned inputs

| Input | Version | Notes |
| --- | --- | --- |
| Zig (bundled `zig cc`) | 0.15.2 | official archives, SHA-256 in `release.yml`; MIT; used only to compile generated C |
| SDL3 | 3.4.16 | macOS/Linux built from the official source tarball (shared); Windows: official `VC` devel (launcher, DLL) and `mingw` devel (import lib for Zig) |
| Dear ImGui | 1.92.9b | fetched at configure time with a pinned hash (`apps/segarecomp-launcher/CMakeLists.txt`) |

The generated program is always compiled with an explicit Zig `-target` (`aarch64-macos`, `x86_64-macos`,
`x86_64-linux-gnu.2.35`, `aarch64-linux-gnu.2.35`, `x86_64-windows-gnu`); see `apps/segarecomp-launcher/launcher_core.cpp`.

Linux packages are built on Ubuntu 22.04 and need glibc >= 2.35 (the bundled SDL3 requires it; the generated program is linked against the same version).

## Layout

```text
Windows / Linux                       macOS
Segarecomp[.exe]  (launcher)          Segarecomp.app/Contents/MacOS/Segarecomp
SDL3.dll (Windows)                    Segarecomp.app/Contents/Resources/   (everything below)
bin/segarecomp[.exe]  (build driver)  bin/  toolchain/  runtime/  sdl3/  licenses/
toolchain/  (Zig)   sdl3/{include,lib}  runtime/{platforms,libs}  licenses/  README.txt
```

`runtime/` mirrors the repository paths the Genesis runtime headers include by relative path.

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

Signing/notarization and installers; prebuilt runtime objects (each game currently compiles the runtime sources); trimming
Zig's `lib/` (large archive); embedded UI font (default ImGui font); fullscreen/controller settings; Windows arm64 and
macOS x86_64 packages (no validated runner); cache eviction; `--discover-copy-aliases` and other bridge-only
analysis (consumer builds use the reset-entry immutable-ROM route only).
