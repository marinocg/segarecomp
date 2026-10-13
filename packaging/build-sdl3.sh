#!/usr/bin/env bash
# Build the pinned SDL3 (shared library) from the official source tarball into a relocatable prefix.
# usage: packaging/build-sdl3.sh <work-dir> <prefix>      (macOS and Linux; Windows uses the official devel zips)
# The result has no dependency on Homebrew/vcpkg: the macOS install name is @rpath/libSDL3.0.dylib, the Linux
# soname is libSDL3.so.0; the launcher and generated games locate it through an rpath into the package.
set -euo pipefail
SDL_VERSION="${SDL_VERSION:-3.4.16}"
SDL_SOURCE_SHA256="${SDL_SOURCE_SHA256:-7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68}"
work="$1"; prefix="$2"
mkdir -p "$work"; cd "$work"
curl -fsSL -o sdl.tar.gz "https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL3-${SDL_VERSION}.tar.gz"
if command -v sha256sum >/dev/null; then echo "${SDL_SOURCE_SHA256}  sdl.tar.gz" | sha256sum -c -; else echo "${SDL_SOURCE_SHA256}  sdl.tar.gz" | shasum -a 256 -c -; fi
tar xzf sdl.tar.gz
extra=()
if [ "$(uname -s)" = "Darwin" ]; then extra+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-12.0}"); fi
# SEG-058 (input diagnostics): SDL compiles its USB driver for Nintendo Switch 2 controllers (SDL_JOYSTICK_HIDAPI_SWITCH2) only when libusb-1.0 is found at
# build time, and then loads libusb at run time (dlopen, so the package does not bundle or require it). CI installs the libusb development package for
# macOS/Linux and sets SDL_REQUIRE_LIBUSB=1 so a build without the driver fails loudly instead of silently shipping without it. SDL_EXTRA_CMAKE_ARGS lets a
# developer point at a libusb prefix (for example -DCMAKE_PREFIX_PATH=...).
if [ -n "${SDL_EXTRA_CMAKE_ARGS:-}" ]; then read -r -a user_extra <<<"${SDL_EXTRA_CMAKE_ARGS}"; extra+=("${user_extra[@]}"); fi
cmake -S "SDL3-${SDL_VERSION}" -B sdl-build -G Ninja -DCMAKE_BUILD_TYPE=Release "${extra[@]}" \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_INSTALL_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build sdl-build
cmake --install sdl-build
cp "SDL3-${SDL_VERSION}/LICENSE.txt" "$prefix/LICENSE.txt"
# Report (and optionally require) the Switch 2 driver: the hint name exists in the library only when the driver was compiled in.
sdl_lib="$(ls "$prefix"/lib/libSDL3.so.0.* "$prefix"/lib/libSDL3.0.dylib 2>/dev/null | head -1 || true)"
if [ -n "$sdl_lib" ] && [ "$(strings -a "$sdl_lib" | grep -c "SDL_JOYSTICK_HIDAPI_SWITCH2" || true)" -gt 0 ]; then
  echo "SDL3 build: Nintendo Switch 2 USB driver COMPILED IN (libusb is loaded at run time; USB only, Bluetooth is not supported by SDL)"
else
  echo "SDL3 build: Nintendo Switch 2 USB driver NOT compiled in (libusb development files were not found)"
  if [ "${SDL_REQUIRE_LIBUSB:-0}" = "1" ]; then echo "SDL_REQUIRE_LIBUSB=1: failing" >&2; exit 1; fi
fi
