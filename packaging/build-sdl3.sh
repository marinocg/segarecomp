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
cmake -S "SDL3-${SDL_VERSION}" -B sdl-build -G Ninja -DCMAKE_BUILD_TYPE=Release "${extra[@]}" \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_INSTALL_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build sdl-build
cmake --install sdl-build
cp "SDL3-${SDL_VERSION}/LICENSE.txt" "$prefix/LICENSE.txt"
