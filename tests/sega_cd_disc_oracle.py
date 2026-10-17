#!/usr/bin/env python3
"""SEG-062-T002: independent, spec-derived Sega CD disc reader (the oracle for libs/media DiscImage and the boot producer).

Written from the documents only - the CUE sheet format, ECMA-130 sector layout (12-byte sync, BCD MSF header, mode byte, 2048
user bytes; MSF = LBA + 150) and docs/architecture/sega-cd-machine-contract.md section 9 - and shares no code with the C++ reader
(libs/media/src/disc.cpp) or the fixture builder (tools/sega_cd_fixture_disc.py). It supports only what the fixtures use and
raises on anything else; it is never used to accept discs, only to compute expected values.

The normalized identity text follows ADR 0129 / docs/architecture/sega-cd-machine-contract.md section 9a.

SEG-063-T003 (CUE sheet semantics): `INDEX 00 mm:ss:ff` opens a track's pregap whose sectors are stored in the BIN; `INDEX 01`
opens the track's program area; `PREGAP` declares a gap that is NOT stored in the BIN. On the disc a pregap is addressable time
between the previous track's last sector and this track's first program sector, so every pregap sector (stored or generated)
advances the LBA counter, and the previous track's data stops where this track's pregap begins.

usage (module): read_disc(cue_path) -> dict ; command line: sega_cd_disc_oracle.py <disc.cue> [--sector-digests] -> JSON
"""
import hashlib
import json
import pathlib
import re
import sys

SYNC = b"\x00" + b"\xff" * 10 + b"\x00"


def _bcd_to_int(value):
    return (value >> 4) * 10 + (value & 0xF)


def _time_to_frames(text):
    minutes, seconds, frames = (int(part) for part in text.split(":"))
    return (minutes * 60 + seconds) * 75 + frames


def read_disc(cue_path, sector_digests=False):
    cue_path = pathlib.Path(cue_path)
    folder = cue_path.parent
    entries = []  # [{'name', 'tracks': [{'number', 'mode', 'index', 'pregap'}]}]
    for raw_line in cue_path.read_text().splitlines():
        line = raw_line.strip()
        if not line:
            continue
        m = re.match(r'FILE\s+"([^"]+)"\s+BINARY$', line)
        if m:
            entries.append({"name": m.group(1), "tracks": []})
            continue
        m = re.match(r"TRACK\s+(\d\d)\s+(MODE1/2048|MODE1/2352|AUDIO)$", line)
        if m:
            entries[-1]["tracks"].append({"number": int(m.group(1)), "mode": m.group(2), "index": None, "index0": None, "pregap": 0})
            continue
        m = re.match(r"INDEX\s+(00|01)\s+(\d\d:\d\d:\d\d)$", line)
        if m:
            entries[-1]["tracks"][-1]["index" if m.group(1) == "01" else "index0"] = _time_to_frames(m.group(2))
            continue
        m = re.match(r"PREGAP\s+(\d\d:\d\d:\d\d)$", line)
        if m:
            entries[-1]["tracks"][-1]["pregap"] = _time_to_frames(m.group(1))
            continue
        raise ValueError("oracle does not understand: " + line)

    files, tracks = [], []
    position = 0
    for file_number, entry in enumerate(entries):
        data = (folder / entry["name"]).read_bytes()
        files.append({"name": entry["name"], "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "data": data})
        size = 2048 if entry["tracks"][0]["mode"] == "MODE1/2048" else 2352
        count_in_file = len(data) // size
        def begins(track):  # first BIN sector the track owns: its stored pregap if it has one, else its program area
            return track["index"] if track["index0"] is None else track["index0"]

        for k, track in enumerate(entry["tracks"]):
            following = begins(entry["tracks"][k + 1]) if k + 1 < len(entry["tracks"]) else count_in_file
            stored_gap = track["index"] - begins(track)
            position += track["pregap"] + stored_gap
            kind = {"MODE1/2048": "mode1_2048", "MODE1/2352": "mode1_2352", "AUDIO": "audio"}[track["mode"]]
            tracks.append({"number": track["number"], "type": kind, "file": file_number, "file_offset": track["index"] * size,
                           "sector_size": size, "pregap": track["pregap"], "index0": stored_gap, "first_lba": position,
                           "sector_count": following - track["index"]})
            position += following - track["index"]

    def user_block(lba):
        for track in tracks:
            if track["first_lba"] <= lba < track["first_lba"] + track["sector_count"] and track["type"] != "audio":
                data = files[track["file"]]["data"]
                start = track["file_offset"] + (lba - track["first_lba"]) * track["sector_size"]
                if track["type"] == "mode1_2352":
                    sector = data[start:start + 2352]
                    assert sector[:12] == SYNC, "raw sync"
                    msf = _bcd_to_int(sector[12]) * 4500 + _bcd_to_int(sector[13]) * 75 + _bcd_to_int(sector[14])
                    assert msf == lba + 150 and sector[15] == 1, "raw header"
                    return sector[16:16 + 2048]
                return data[start:start + 2048]
        raise KeyError(lba)

    table = "segarecomp.disc.v1\n"
    for number, f in enumerate(files):
        table += "file %d size %d sha256 %s name %s\n" % (number, f["size"], f["sha256"], f["name"])
    for t in tracks:
        table += "track %d %s file %d offset %d sector %d pregap %d lba %d count %d\n" % (
            t["number"], t["type"], t["file"], t["file_offset"], t["sector_size"], t["pregap"], t["first_lba"], t["sector_count"])
        if t["index0"]:
            table = table[:-1] + " index0 %d\n" % t["index0"]
    table += "total %d\n" % position

    system = b"".join(user_block(tracks[0]["first_lba"] + i) for i in range(16))

    def long(offset):
        return int.from_bytes(system[offset:offset + 4], "big")

    def text(offset, length):
        return "".join(chr(c) if 0x20 <= c < 0x7F else "." for c in system[offset:offset + length])

    def descriptor(offset):
        return {"disc_address": long(offset), "load_size": long(offset + 4), "entry_offset": long(offset + 8),
                "work_ram_size": long(offset + 12)}

    ip, sp = descriptor(0x30), descriptor(0x40)
    ip_bytes = system[ip["disc_address"]:ip["disc_address"] + ip["load_size"]]
    sp_bytes = system[sp["disc_address"]:sp["disc_address"] + sp["load_size"]]
    result = {
        "identity": hashlib.sha256(table.encode()).hexdigest(),
        "total_sectors": position,
        "files": [{k: f[k] for k in ("name", "size", "sha256")} for f in files],
        "tracks": tracks,
        "boot": {
            "system_id": text(0, 16), "volume_name": text(0x10, 11), "system_name": text(0x20, 11),
            "hardware_id": text(0x100, 16), "product_code": text(0x180, 14),
            "region": "".join(chr(c) for c in system[0x1F0:0x1F3] if 0x20 < c < 0x7F),
            "ip": ip, "sp": sp,
            "ip_sha256": hashlib.sha256(ip_bytes).hexdigest(), "sp_sha256": hashlib.sha256(sp_bytes).hexdigest(),
            "main_entry_pc": 0xFF0000 + ip["entry_offset"], "sub_entry_pc": 0x6000 + sp["entry_offset"],
        },
    }
    if sector_digests:
        result["sector_sha256"] = {}
        for t in tracks:
            if t["type"] != "audio":
                for lba in range(t["first_lba"], t["first_lba"] + t["sector_count"]):
                    result["sector_sha256"][str(lba)] = hashlib.sha256(user_block(lba)).hexdigest()
    result["_user_block"] = user_block
    return result


def main():
    disc = read_disc(sys.argv[1], "--sector-digests" in sys.argv[2:])
    disc.pop("_user_block")
    print(json.dumps(disc, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
