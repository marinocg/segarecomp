#!/usr/bin/env python3
"""SEG-063-T001: independent, spec-derived ISO9660 reader (the oracle for libs/media iso9660.cpp and the module producer).

Written from ECMA-119 only (volume descriptor set from logical block 16; Primary Volume Descriptor fields at byte offsets 80 volume
space, 128 logical block size, 156 root directory record; directory records: length, extended-attribute length, both-endian extent
and data length, flags at 25, identifier length at 32, identifier at 33) and shares no code with the C++ reader or the fixture
builder (tools/sega_cd_fixture_disc.py). It supports only what the fixtures use and raises on anything else; it is never used to
accept discs, only to compute expected values.

usage (module): tree(user_block) -> {"volume_space", "root", "entries": [{"path", "directory", "block", "size", "sha256"}]}
"""
import hashlib
import struct


def _both32(data, offset):
    little = struct.unpack_from("<I", data, offset)[0]
    big = struct.unpack_from(">I", data, offset + 4)[0]
    if little != big:
        raise ValueError("both-endian mismatch")
    return little


def _records(directory):
    out = []
    for sector in range(0, len(directory), 2048):
        at = 0
        while at < 2048 and directory[sector + at] != 0:
            length = directory[sector + at]
            rec = directory[sector + at:sector + at + length]
            if at + length > 2048 or len(rec) < 34:
                raise ValueError("bad record")
            name = rec[33:33 + rec[32]]
            out.append({"name": name, "block": _both32(rec, 2), "size": _both32(rec, 10), "directory": bool(rec[25] & 2),
                        "flags": rec[25], "xattr": rec[1]})
            at += length
    return out


def tree(user_block):
    """`user_block(block)` returns the 2048 user bytes of logical block `block` of the data track."""
    block = 16
    pvd = None
    while True:
        d = user_block(block)
        if d[1:6] != b"CD001":
            raise ValueError("not ISO9660")
        if d[0] == 255:
            break
        if d[0] == 1:
            pvd = d
        block += 1
    space = _both32(pvd, 80)
    root_rec = pvd[156:190]
    root = {"block": _both32(root_rec, 2), "size": _both32(root_rec, 10)}

    def read(extent_block, size):
        count = (size + 2047) // 2048
        return b"".join(user_block(extent_block + i) for i in range(count))[:size]

    entries = []
    pending = [("", root["block"], root["size"])]
    while pending:
        prefix, b, size = pending.pop(0)
        for rec in _records(read(b, size))[2:]:
            path = prefix + "/" + rec["name"].decode("ascii")
            item = {"path": path, "directory": rec["directory"], "block": rec["block"], "size": rec["size"]}
            if rec["directory"]:
                pending.append((path, rec["block"], rec["size"]))
            else:
                item["sha256"] = hashlib.sha256(read(rec["block"], rec["size"])).hexdigest()
            entries.append(item)
    return {"volume_space": space, "root": root, "entries": entries}
