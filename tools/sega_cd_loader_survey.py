#!/usr/bin/env python3
"""SEG-098-T001 (ADR 0210): aggregate, privacy-safe survey of Sega CD executable-loader coverage.

Inputs (all local, never copied into the output): the disc (`--cue`), the product CLI, and optionally the `status.json` of an independent
`segarecomp sega-cd build` run whose extents are the generated-native build-pass delivery evidence.
Output: classes and counts only. No address, offset, LBA, module name, hash or byte of the disc is printed or written; the observed extents are
used in memory to compute overlaps and class counts. Observations are coverage evidence and a falsification control, never admission input.
"""
import argparse
import json
import subprocess
import sys


def scan(cli, cue, *flags):
    done = subprocess.run([cli, "sega-cd", "discover-modules", "--cue", cue, *flags], capture_output=True, text=True, timeout=600)
    if not done.stdout.strip():
        raise SystemExit("discover-modules produced no report")
    return json.loads(done.stdout)


def dest_class(cpu, base):
    if cpu == "main":
        return "main_work_ram" if base >= 0xFF0000 else "main_word_ram" if 0x200000 <= base < 0x240000 else "main_other"
    return "sub_prg_ram" if base < 0x80000 else "sub_word_ram" if base < 0xC0000 else "sub_other"


def size_class(n):
    return "lt_2k" if n < 2048 else "2k_to_32k" if n < 32768 else "ge_32k"


def static_view(report):
    mods = report["modules"]
    by_base = {}
    for m in mods:
        for b in m["bases"]:
            by_base.setdefault(b, 0)
            by_base[b] += 1
    refusals = {}
    for r in report["refusals"]:
        refusals[r["reason"]] = refusals.get(r["reason"], 0) + 1
    exec_bytes = sum(m["range"]["length"] for m in mods)
    return {
        "complete": report["complete"], "states": report["states"], "steps": report["steps"],
        "load_operations": len(report["operations"]), "modules": len(mods), "mappings": sum(len(m["bases"]) for m in mods),
        "max_overlay_alternatives_per_base": max(by_base.values(), default=0), "refusals_by_class": refusals,
        "executable_range_bytes": exec_bytes, "estimated_aligned_starts": exec_bytes // 2,
        "survey": report.get("survey", {}),
    }


def observed_view(status, static_modules):
    ext = []
    for e in status.get("extents", []):
        cpu, off, base, length = e.split(":")
        ext.append((cpu, int(off, 0), int(base, 0), int(length, 0)))
    classes, sizes, bases = {}, {}, {}
    for cpu, off, base, length in ext:
        k = cpu + ":" + dest_class(cpu, base)
        classes[k] = classes.get(k, 0) + 1
        s = size_class(length)
        sizes[s] = sizes.get(s, 0) + 1
        bases.setdefault((cpu, base), 0)
        bases[(cpu, base)] += 1
    static_spans = [(m["block"] * 2048, m["block"] * 2048 + m["size"]) for m in static_modules]
    overlap = sum(1 for _, off, _, length in ext if any(off < hi and lo < off + length for lo, hi in static_spans))
    hist = status.get("history", [])
    return {
        "build_rounds": status.get("rounds"), "build_status": status.get("status"), "stop_class": status.get("stop"),
        "observed_extents": len(ext), "observed_by_destination_class": classes, "observed_by_size_class": sizes,
        "observed_overlay_groups_gt1": sum(1 for v in bases.values() if v > 1), "observed_bytes": sum(e[3] for e in ext),
        "observed_estimated_aligned_starts": sum(e[3] for e in ext) // 2,
        "observed_extents_overlapping_static_modules": overlap,
        "frontier_refusal": (status.get("frontier") or {}).get("refusal"),
        "history_rounds_with_admission": sum(1 for h in hist if "admitted" in h),
    }


class Track1:
    """Read-only view of track 1's 2048-byte user data of a single-file BIN/CUE (MODE1/2352 or cooked 2048)."""

    def __init__(self, cue):
        import os
        import re
        text = open(cue, encoding="latin-1").read()
        name = re.search(r'FILE\s+"([^"]+)"', text).group(1)
        self.path = os.path.join(os.path.dirname(cue), name)
        size = os.path.getsize(self.path)
        self.raw = "MODE1/2352" in text or "MODE2/2352" in text
        self.stride = 2352 if self.raw else 2048
        self.start = 16 if self.raw else 0
        self.sectors = size // self.stride
        self.fh = open(self.path, "rb")

    def sector(self, lba):
        self.fh.seek(lba * self.stride + self.start)
        return self.fh.read(2048)

    def read(self, offset, length):
        out = b""
        while length > 0:
            lba, within = divmod(offset, 2048)
            chunk = self.sector(lba)[within:within + length]
            if not chunk:
                break
            out += chunk
            offset += len(chunk)
            length -= len(chunk)
        return out


def iso_files(track):
    """(extent lba, size) of every ISO9660 file reachable from the root (bounded); empty when there is no volume."""
    pvd = track.sector(16)
    if pvd[1:6] != b"CD001":
        return []
    root = pvd[156:190]
    work, seen, files = [(int.from_bytes(root[2:6], "little"), int.from_bytes(root[10:14], "little"))], set(), []
    while work and len(seen) < 512:
        lba, size = work.pop()
        if lba in seen:
            continue
        seen.add(lba)
        data = track.read(lba * 2048, size)
        pos = 0
        while pos < len(data):
            n = data[pos]
            if n == 0:
                pos = (pos // 2048 + 1) * 2048
                continue
            rec = data[pos:pos + n]
            pos += n
            ident = rec[33:33 + rec[32]]
            if ident in (b"\x00", b"\x01"):
                continue
            ext, length = int.from_bytes(rec[2:6], "little"), int.from_bytes(rec[10:14], "little")
            if rec[25] & 2:
                work.append((ext, length))
            else:
                files.append((ext, length))
    return files


def source_probe(track, extents, boot_images):
    """How the observed sources relate to independent, immutable structure: ISO9660 file starts; LBA constants inside the boot images."""
    files = iso_files(track)
    starts = {f[0] for f in files}
    spans = [(f[0], f[0] + max(1, (f[1] + 2047) // 2048)) for f in files]
    probe = {"iso9660_files": len(files), "iso9660_bytes": sum(f[1] for f in files),
             "observed_sources_at_iso_file_start": 0, "observed_sources_inside_an_iso_file": 0, "observed_sources_with_lba_constant_in_boot_images": 0,
             "iso_files_with_lba_constant_in_boot_images": 0}
    for e in extents:
        lba = e[1] // 2048
        probe["observed_sources_at_iso_file_start"] += lba in starts
        probe["observed_sources_inside_an_iso_file"] += any(lo <= lba < hi for lo, hi in spans)
        probe["observed_sources_with_lba_constant_in_boot_images"] += any(lba.to_bytes(4, "big") in img or lba.to_bytes(2, "big") in img for img in boot_images) if lba < 65536 else any(lba.to_bytes(4, "big") in img for img in boot_images)
    probe["iso_files_with_lba_constant_in_boot_images"] = sum(1 for s in starts if any(s.to_bytes(4, "big") in img for img in boot_images))
    return probe


def boot_image_bytes(track):
    area = b"".join(track.sector(i) for i in range(16))
    out = []
    for desc in (0x30, 0x40):
        addr, size = int.from_bytes(area[desc:desc + 4], "big"), int.from_bytes(area[desc + 4:desc + 8], "big")
        if 0 < size and addr + size <= len(area):
            out.append(area[addr:addr + size])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--segarecomp", required=True)
    ap.add_argument("--cue", required=True)
    ap.add_argument("--status", help="status.json of an independent build run (observed extents)")
    ap.add_argument("--probe-source-constants", action="store_true", help="relate the observed sources to ISO9660 file starts and LBA constants of the boot images (aggregate counts)")
    args = ap.parse_args()
    out = {"schema": "segarecomp.sega_cd.loader_survey.v1"}
    base = scan(args.segarecomp, args.cue)
    out["scan_descriptor_entry_only"] = static_view(base)
    table = scan(args.segarecomp, args.cue, "--module-table-roots")
    out["scan_module_table_roots"] = static_view(table)
    whatif = scan(args.segarecomp, args.cue, "--module-table-roots", "--computed-call-whatif")
    out["scan_module_table_roots_computed_call_whatif"] = static_view(whatif)
    if args.status:
        status = json.load(open(args.status))
        out["observed"] = observed_view(status, table["modules"])
        if args.probe_source_constants:
            ext = []
            for e in status.get("extents", []):
                cpu, off, base, length = e.split(":")
                ext.append((cpu, int(off, 0), int(base, 0), int(length, 0)))
            track = Track1(args.cue)
            out["source_probe"] = source_probe(track, ext, boot_image_bytes(track))
        out["static_vs_observed"] = {"static_sources": len(table["modules"]),
                                      "observed_sources": out["observed"]["observed_extents"],
                                      "overlap": out["observed"]["observed_extents_overlapping_static_modules"]}
    else:
        out["observed"] = "AUTHENTIC_BUILD_EVIDENCE_NOT_SUPPLIED"
    json.dump(out, sys.stdout, indent=1, sort_keys=True)
    print()


if __name__ == "__main__":
    main()
