#!/usr/bin/env python3
"""Deterministic .segapatch authoring and an INDEPENDENT reference reader (ADR 0266).

A package is a ZIP of `manifest.json` and its declared payload file(s) (v1: one BPS `rom` payload). This tool shares no code with the C++ importer
(libs/media/src/patch_*.cpp): the BPS encoder/decoder, CRC use and ZIP writer are separate implementations so the tests can
cross-check both. It never reads or writes anything outside the paths it is given.

  segapatch.py create --source ROM --target PATCHED --id ID --name NAME --version V --out FILE [--width-max N]
  segapatch.py apply  PACKAGE --source ROM --out FILE      (reference implementation; verifies everything)
  segapatch.py bps-create SOURCE TARGET OUT.bps
  segapatch.py bps-apply  PATCH SOURCE OUT
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zipfile
import zlib
from pathlib import Path

FIXED_TIME = (1980, 1, 1, 0, 0, 0)


def _num(n: int) -> bytes:
    out = bytearray()
    while True:
        x = n & 0x7F
        n >>= 7
        if n == 0:
            out.append(0x80 | x)
            return bytes(out)
        out.append(x)
        n -= 1


def _read_num(buf: bytes, at: int, end: int) -> tuple[int, int]:
    data, shift = 0, 1
    while True:
        if at >= end:
            raise ValueError("truncated number")
        x = buf[at]
        at += 1
        data += (x & 0x7F) * shift
        if x & 0x80:
            return data, at
        shift <<= 7
        data += shift


def bps_create(source: bytes, target: bytes) -> bytes:
    """Linear-time BPS: SourceRead for identical runs at the same offset, TargetRead for the rest."""
    body = bytearray(b"BPS1" + _num(len(source)) + _num(len(target)) + _num(0))
    i, n = 0, len(target)
    while i < n:
        j = i
        if i < len(source) and target[i] == source[i]:
            while j < n and j < len(source) and target[j] == source[j]:
                j += 1
            body += _num(((j - i - 1) << 2) | 0)
        else:
            while j < n and not (j < len(source) and target[j] == source[j] and j + 4 <= min(n, len(source)) and target[j:j + 4] == source[j:j + 4]):
                j += 1
            body += _num(((j - i - 1) << 2) | 1) + target[i:j]
        i = j
    body += struct.pack("<I", zlib.crc32(source)) + struct.pack("<I", zlib.crc32(target))
    body += struct.pack("<I", zlib.crc32(bytes(body)))
    return bytes(body)


def bps_apply(patch: bytes, source: bytes) -> bytes:
    if patch[:4] != b"BPS1" or len(patch) < 19:
        raise ValueError("not BPS1")
    if zlib.crc32(patch[:-4]) != struct.unpack("<I", patch[-4:])[0]:
        raise ValueError("patch crc")
    end = len(patch) - 12
    at = 4
    ssize, at = _read_num(patch, at, end)
    tsize, at = _read_num(patch, at, end)
    msize, at = _read_num(patch, at, end)
    at += msize
    if ssize != len(source) or zlib.crc32(source) != struct.unpack("<I", patch[end:end + 4])[0]:
        raise ValueError("source mismatch")
    out = bytearray()
    srel = trel = 0
    while len(out) < tsize:
        word, at = _read_num(patch, at, end)
        length, action = (word >> 2) + 1, word & 3
        if action == 0:
            out += source[len(out):len(out) + length]
        elif action == 1:
            out += patch[at:at + length]
            at += length
        else:
            raw, at = _read_num(patch, at, end)
            delta = -(raw >> 1) if raw & 1 else raw >> 1
            if action == 2:
                srel += delta
                out += source[srel:srel + length]
                srel += length
            else:
                trel += delta
                for k in range(length):
                    out.append(out[trel + k])
                trel += length
    if len(out) != tsize or at != end or zlib.crc32(bytes(out)) != struct.unpack("<I", patch[end + 4:end + 8])[0]:
        raise ValueError("target mismatch")
    return bytes(out)


def manifest_bytes(source: bytes, target: bytes, payload: bytes, *, id_: str, name: str, version: str, platform: str = "genesis",
                   width_max: int | None = None, requires: list[str] | None = None) -> bytes:
    manifest = {
        "format": "segarecomp-patch",
        "schema_version": 1,
        "id": id_,
        "version": version,
        "name": name,
        "platform": platform,
        "purpose": "widescreen",
        "source_sha256": hashlib.sha256(source).hexdigest(),
        "payloads": {"rom": {"format": "bps", "file": "game.bps", "sha256": hashlib.sha256(payload).hexdigest(),
                             "target_sha256": hashlib.sha256(target).hexdigest()}},
    }
    if requires:
        manifest["requires"] = requires
    if width_max is not None:
        manifest["widescreen_width_max"] = width_max
    return (json.dumps(manifest, indent=2) + "\n").encode("ascii")


def write_zip(path: Path, files: list[tuple[str, bytes]], *, method: int = zipfile.ZIP_DEFLATED) -> None:
    with zipfile.ZipFile(path, "w") as z:
        for name, data in files:
            info = zipfile.ZipInfo(name, FIXED_TIME)
            info.compress_type = method
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            z.writestr(info, data, compresslevel=9)


def create(source: bytes, target: bytes, out: Path, *, id_: str, name: str, version: str, width_max: int | None = None) -> None:
    payload = bps_create(source, target)
    write_zip(out, [("manifest.json", manifest_bytes(source, target, payload, id_=id_, name=name, version=version, width_max=width_max)), ("game.bps", payload)])


def apply_package(package: Path, source: bytes) -> bytes:
    with zipfile.ZipFile(package) as z:
        names = [i.filename for i in z.infolist()]
        manifest = json.loads(z.read("manifest.json"))
        rom = manifest["payloads"]["rom"]
        if sorted(names) != sorted(["manifest.json", rom["file"]]):
            raise ValueError("unexpected entries")
        payload = z.read(rom["file"])
    if hashlib.sha256(payload).hexdigest() != rom["sha256"]:
        raise ValueError("payload sha256")
    if hashlib.sha256(source).hexdigest() != manifest["source_sha256"]:
        raise ValueError("source sha256")
    out = bps_apply(payload, source)
    if hashlib.sha256(out).hexdigest() != rom["target_sha256"]:
        raise ValueError("target sha256")
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("create")
    c.add_argument("--source", type=Path, required=True)
    c.add_argument("--target", type=Path, required=True)
    c.add_argument("--id", required=True)
    c.add_argument("--name", required=True)
    c.add_argument("--version", required=True)
    c.add_argument("--out", type=Path, required=True)
    c.add_argument("--width-max", type=int)
    a = sub.add_parser("apply")
    a.add_argument("package", type=Path)
    a.add_argument("--source", type=Path, required=True)
    a.add_argument("--out", type=Path, required=True)
    bc = sub.add_parser("bps-create")
    bc.add_argument("source", type=Path); bc.add_argument("target", type=Path); bc.add_argument("out", type=Path)
    ba = sub.add_parser("bps-apply")
    ba.add_argument("patch", type=Path); ba.add_argument("source", type=Path); ba.add_argument("out", type=Path)
    args = ap.parse_args(argv)
    if args.cmd == "create":
        create(args.source.read_bytes(), args.target.read_bytes(), args.out, id_=args.id, name=args.name, version=args.version, width_max=args.width_max)
    elif args.cmd == "apply":
        args.out.write_bytes(apply_package(args.package, args.source.read_bytes()))
    elif args.cmd == "bps-create":
        args.out.write_bytes(bps_create(args.source.read_bytes(), args.target.read_bytes()))
    else:
        args.out.write_bytes(bps_apply(args.patch.read_bytes(), args.source.read_bytes()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
