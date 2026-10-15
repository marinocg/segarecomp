#!/usr/bin/env bash
# Build the pinned SDL3 (shared library) from the official source tarball into a relocatable prefix, with SDL's Nintendo Switch 2 USB driver.
# usage: packaging/build-sdl3.sh <work-dir> <prefix>      (macOS, Linux and Windows)
#
# macOS/Linux: no dependency on Homebrew/vcpkg at run time: the macOS install name is @rpath/libSDL3.0.dylib, the Linux soname is libSDL3.so.0; the launcher
#   and generated games locate it through an rpath into the package. libusb comes from the OS (development files at build time, dlopen at run time).
# Windows (needs the MSVC environment, e.g. ilammy/msvc-dev-cmd, plus Zig for the MinGW import library):
#   <prefix>/bin/SDL3.dll and <prefix>/lib/SDL3.lib (launcher, MSVC), <prefix>/lib/libSDL3.dll.a (bundled Zig, generated from the SAME DLL's exports),
#   <prefix>/libusb/ (the runtime libusb-1.0.dll, its license and its corresponding source, all SHA-256 pinned). SDL loads libusb-1.0.dll lazily by name.
#
# Environment: SDL_REQUIRE_LIBUSB=1 fails the build when the Switch 2 driver is not compiled in (always on Windows), SDL_CONFIGURE_ONLY=1 stops after the
# configure-time gate (used to prove the gate rejects a build without libusb), SDL_EXTRA_CMAKE_ARGS adds CMake arguments, ZIG=<zig executable> (Windows).
set -euo pipefail
SDL_VERSION="${SDL_VERSION:-3.4.16}"
SDL_SOURCE_SHA256="${SDL_SOURCE_SHA256:-7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68}"
# Windows only: official libusb 1.0.29 release assets (https://github.com/libusb/libusb/releases/tag/v1.0.29).
LIBUSB_VERSION="${LIBUSB_VERSION:-1.0.29}"
LIBUSB_BINARY_SHA256="${LIBUSB_BINARY_SHA256:-964a38152ca9a104cd00ec8d2f0617b89cd814f9b635e29763c68563d951521d}"
LIBUSB_SOURCE_SHA256="${LIBUSB_SOURCE_SHA256:-5977fc950f8d1395ccea9bd48c06b3f808fd3c2c961b44b0c2e6e29fc3a70a85}"
work="$1"; prefix="$2"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) windows=1; py=python ;; *) windows=0; py=python3 ;; esac
if [ "$windows" = 1 ]; then SDL_REQUIRE_LIBUSB=1; fi

sha_check() { # <sha256> <file>
  if command -v sha256sum >/dev/null; then echo "$1  $2" | sha256sum -c -; else echo "$1  $2" | shasum -a 256 -c -; fi
}
mkdir -p "$work"; cd "$work"
curl -fsSL -o sdl.tar.gz "https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL3-${SDL_VERSION}.tar.gz"
sha_check "${SDL_SOURCE_SHA256}" sdl.tar.gz
tar xzf sdl.tar.gz
extra=()
if [ "$(uname -s)" = "Darwin" ]; then extra+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-12.0}"); fi

if [ "$windows" = 1 ]; then
  # SDL compiles its Switch 2 driver (SDL_JOYSTICK_HIDAPI_SWITCH2) only when CMake finds libusb at configure time. The official prebuilt libusb provides the
  # headers and the MSVC import library needed to configure/compile against it; the DLL SDL loads at run time is the MinGW build, which needs only
  # KERNEL32/msvcrt/USER32 (the Visual C++ build needs VCRUNTIME140.dll, which a clean machine may lack). Both are the same API; sdl_check.py proves SDL's
  # lookups against the shipped DLL. Passing the FULL path of the .lib (not a directory) lets SDL identify the DLL name and load it lazily (upstream issue 14742).
  curl -fsSL -o libusb.7z "https://github.com/libusb/libusb/releases/download/v${LIBUSB_VERSION}/libusb-${LIBUSB_VERSION}.7z"
  sha_check "${LIBUSB_BINARY_SHA256}" libusb.7z
  curl -fsSL -o libusb-source.tar.bz2 "https://github.com/libusb/libusb/releases/download/v${LIBUSB_VERSION}/libusb-${LIBUSB_VERSION}.tar.bz2"
  sha_check "${LIBUSB_SOURCE_SHA256}" libusb-source.tar.bz2
  # The release asset is a 7z archive that CMake's bundled libarchive cannot read on the Windows runners ("Lzma library error"); 7-Zip is preinstalled there.
  sevenzip=""
  for candidate in 7z 7zz 7za "/c/Program Files/7-Zip/7z.exe" "C:/Program Files/7-Zip/7z.exe"; do
    if command -v "$candidate" >/dev/null 2>&1; then sevenzip="$candidate"; break; fi
  done
  [ -n "$sevenzip" ] || { echo "7-Zip is required to unpack the pinned libusb release asset" >&2; exit 1; }
  rm -rf libusb-bin && mkdir libusb-bin && "$sevenzip" x -y -olibusb-bin libusb.7z >/dev/null
  extra+=("-DLibUSB_INCLUDE_PATH=$work/libusb-bin/include" "-DLibUSB_LIBRARY=$work/libusb-bin/VS2022/MS64/dll/libusb-1.0.lib")
  # Static CRT: no Visual C++ redistributable on a clean machine (same rule as the launcher). cl, not clang: the toolchain SDL's official Windows builds use.
  extra+=("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded")
  export CC=cl CXX=cl
fi
# SDL_EXTRA_CMAKE_ARGS lets a developer point at a libusb prefix (for example -DCMAKE_PREFIX_PATH=...).
if [ -n "${SDL_EXTRA_CMAKE_ARGS:-}" ]; then read -r -a user_extra <<<"${SDL_EXTRA_CMAKE_ARGS}"; extra+=("${user_extra[@]}"); fi
configure() { cmake -S "SDL3-${SDL_VERSION}" -B sdl-build -G Ninja -DCMAKE_BUILD_TYPE=Release "${extra[@]}" "$@" \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_INSTALL_TESTS=OFF -DCMAKE_INSTALL_PREFIX="$prefix"; }
configure

# Configure-time gate. SDL_build_config.h is generated from what CMake actually found: HAVE_LIBUSB is the condition under which the driver is compiled
# (SDL_hidapijoystick_c.h), SDL_LIBUSB_DYNAMIC the name SDL loads at run time.
config_header="$(find sdl-build -name SDL_build_config.h | head -1)"
if grep -q '^#define HAVE_LIBUSB 1' "$config_header"; then
  echo "SDL3 configure: libusb found; the Nintendo Switch 2 USB driver will be compiled ($(grep '^#define SDL_LIBUSB_DYNAMIC' "$config_header" || echo 'libusb linked statically, not loaded lazily'))"
else
  echo "SDL3 configure: libusb NOT found; the Nintendo Switch 2 USB driver would NOT be compiled in (libusb development files were not found)"
  if [ "${SDL_REQUIRE_LIBUSB:-0}" = "1" ]; then echo "SDL_REQUIRE_LIBUSB=1: failing" >&2; exit 1; fi
fi
if [ "$windows" = 1 ] && ! grep -q '^#define SDL_LIBUSB_DYNAMIC "libusb-1.0.dll"' "$config_header"; then
  echo "SDL3 configure: SDL would link libusb statically instead of loading libusb-1.0.dll (check that LibUSB_LIBRARY is the full path of the .lib)" >&2; exit 1
fi
if [ "${SDL_CONFIGURE_ONLY:-0}" = "1" ]; then exit 0; fi
have_libusb=0
if grep -q '^#define HAVE_LIBUSB 1' "$config_header"; then have_libusb=1; fi
# A build that carries the driver says so in SDL_GetRevision(), which the input diagnostics read to tell "driver missing from this SDL" from other causes.
if [ "$have_libusb" = 1 ]; then configure "-DSDL_VENDOR_INFO=segarecomp-switch2-usb"; fi
cmake --build sdl-build
cmake --install sdl-build
cp "SDL3-${SDL_VERSION}/LICENSE.txt" "$prefix/LICENSE.txt"

# Post-build gate on the real binary (never a configure-time claim alone).
if [ "$windows" = 1 ]; then
  sdl_lib="$prefix/bin/SDL3.dll"
  "$py" "$here/sdl_check.py" driver "$sdl_lib" --dynamic-libusb libusb-1.0.dll
  # The MinGW import library for the bundled Zig, generated from the exports of the DLL that ships (never from a different build).
  "$py" "$here/sdl_check.py" mkdef "$sdl_lib" "$work/SDL3.def"
  "${ZIG:?ZIG must name the pinned zig executable}" dlltool -m i386:x86-64 -D SDL3.dll -d "$work/SDL3.def" -l "$prefix/lib/libSDL3.dll.a"
  mkdir -p "$prefix/libusb"
  cp libusb-bin/MinGW64/dll/libusb-1.0.dll "$prefix/libusb/libusb-1.0.dll"
  rm -rf libusb-src && mkdir libusb-src && (cd libusb-src && cmake -E tar xf ../libusb-source.tar.bz2)
  cp "libusb-src/libusb-${LIBUSB_VERSION}/COPYING" "$prefix/libusb/COPYING.txt"
  cp libusb-source.tar.bz2 "$prefix/libusb/libusb-${LIBUSB_VERSION}-source.tar.bz2"
  "$py" "$here/sdl_check.py" windows "$sdl_lib" "$prefix/libusb/libusb-1.0.dll" "$prefix/lib/SDL3.lib" "$prefix/lib/libSDL3.dll.a"
else
  sdl_lib="$(ls "$prefix"/lib/libSDL3.so.0.* "$prefix"/lib/libSDL3.0.dylib 2>/dev/null | head -1 || true)"
  if [ -n "$sdl_lib" ] && "$py" "$here/sdl_check.py" driver "$sdl_lib"; then
    echo "SDL3 build: Nintendo Switch 2 USB driver COMPILED IN (libusb is loaded at run time; USB only, Bluetooth is not supported by SDL)"
  else
    echo "SDL3 build: Nintendo Switch 2 USB driver NOT compiled in"
    if [ "${SDL_REQUIRE_LIBUSB:-0}" = "1" ]; then echo "SDL_REQUIRE_LIBUSB=1: failing" >&2; exit 1; fi
  fi
fi
