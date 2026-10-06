#!/usr/bin/env bash
# vcpkg x-script asset source: when a GNU tarball URL is requested, try alternative GNU mirrors
# first. vcpkg verifies the SHA-512 itself, so a bad mirror can never inject content. A non-zero
# exit makes vcpkg fall back to the original URL.
# usage: vcpkg_gnu_mirror.sh <url> <dst>
set -u
url="$1"; dst="$2"
case "$url" in
  https://ftpmirror.gnu.org/*|https://ftp.gnu.org/*) ;;
  *) exit 1 ;;
esac
path="${url#https://ftpmirror.gnu.org/}"
path="${path#https://ftp.gnu.org/pub/}"
path="${path#https://ftp.gnu.org/}"
for base in https://mirrors.kernel.org https://ftp.nluug.nl/pub https://mirrors.ocf.berkeley.edu; do
  if curl -fsSL --retry 2 --connect-timeout 10 --max-time 120 -o "$dst" "$base/$path"; then exit 0; fi
done
rm -f "$dst"
exit 1
