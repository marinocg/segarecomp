#!/usr/bin/env python3
"""SEG-077-T009 spike: execution evidence beyond generic random input (OFFLINE research tooling).

Reuses the frozen T001 tracer (segarecomp_genesis_evidence: MAME 0.276 offline container, read tap, VDP DMA reconstruction, mapping
check) without modifying it, and adds:
  * a `movie` input policy: per-frame controller-1 states from a deterministic input recording (Gens .gmv converted to one byte per
    frame, bit set = pressed: Up Down Left Right A B C Start), with an optional frame offset; no state loads, no randomness
  * indirect-edge capture: for a supplied set of indirect control sites (JMP/JSR through registers or memory), the streaming trace
    consumer records every distinct (site PC, next executed PC) pair; nothing else from the trace is stored

Output directory layout is the T001 one (trace.json, pcs.json, reads.json, dma.json) plus edges.json, so the existing label pipelines
consume it unchanged.

  gmv2frames  convert a Gens movie (.gmv: 64-byte header, 3 bytes per frame, controller-1 byte active-low) to a frames file
  sites       indirect control sites of a labelled sample (JMP/JSR with a register / memory EA) -> sites.json
  trace       one movie or explore scenario with edge capture
"""
from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_genesis_evidence as ge  # noqa: E402

GMV_HEADER = 64
GMV_FRAME = 3
BUTTON_ORDER = ("P1 Up", "P1 Down", "P1 Left", "P1 Right", "P1 A", "P1 B", "P1 C", "P1 Start")


def gmv_frames(data: bytes) -> bytes:
    """Gens movie -> one byte per frame, bit i set = BUTTON_ORDER[i] pressed (the .gmv controller-1 byte is active-low)."""
    if len(data) < GMV_HEADER or not data[:16].startswith(b"Gens Movie"):
        raise ge.EvidenceError("not a Gens movie")
    body = data[GMV_HEADER:]
    if len(body) % GMV_FRAME:
        raise ge.EvidenceError("truncated Gens movie body")
    return bytes((~body[i]) & 0xFF for i in range(0, len(body), GMV_FRAME))


def indirect_sites(img: bytes, classes: bytes) -> list[int]:
    """Labelled instruction starts that are JMP/JSR through a register or memory EA (not abs.w / abs.l / d16(PC))."""
    out = []
    for pc in range(0, len(classes), 2):
        if classes[pc] != ord("I"):
            continue
        w = ge.u16(img, pc)
        if w is not None and w & 0xFF80 == 0x4E80 and (w & 0x3F) not in (0x38, 0x39, 0x3A):
            out.append(pc)
    return out


MOVIE_POLICY = r"""
local movie = nil
local function movie_policy()
  if movie == nil then
    local f = io.open("/in/frames.bin", "rb") movie = f:read("a") f:close()
  end
  local i = frame - S.offset
  local p = {}
  if i >= 1 and i <= #movie then
    local b = movie:byte(i)
    for k, n in ipairs(names) do if (b >> (k - 1)) & 1 == 1 then p[n] = 1 end end
  end
  return p
end
"""

EDGE_AWK = r"""awk -v SITES="$SITES" 'BEGIN{n=split(SITES,a,",");for(i=1;i<=n;i++)site[a[i]]=1}
{ if (substr($0,7,1) != ":") { prev=""; next } pc=substr($0,1,6);
  if (!(pc in seen)) { seen[pc]=1; print > "/out/pcs_first.txt" }
  if (prev in site) { k=prev" "pc; if (!(k in edge)) { edge[k]=1; print k > "/out/edges.txt" } }
  prev=pc }' /tmp/trace.fifo"""


def scenario_lua(sc: dict, size: int) -> str:
    base = sc if sc["policy"] != "movie" else dict(sc, policy="none")
    lua = ge.scenario_lua(dict(base, policy=("explore" if sc["policy"] == "explore" else "none")), size)
    if sc["policy"] == "movie":
        lua = lua.replace('policy = "none"', 'policy = "movie", offset = %d' % int(sc.get("offset", 0)), 1)
        lua = lua.replace("local function on_frame()", MOVIE_POLICY + "\nlocal function on_frame()", 1)
        lua = lua.replace("  local p = policy()\n", "  local p = (S.policy == \"movie\") and movie_policy() or policy()\n", 1)
        if 'local p = (S.policy == "movie") and movie_policy() or policy()' not in lua:
            raise ge.EvidenceError("movie policy injection failed")
    return lua


TRACE_SH = r"""set -e
cd /tmp
mkfifo /tmp/trace.fifo
: > /out/edges.txt
""" + EDGE_AWK + r""" &
AWK=$!
/usr/games/mame genesis -cart /in/rom.bin -debug -debugger none -autoboot_script /in/scenario.lua -video none -sound none -nothrottle \
  -seconds_to_run "$SECONDS_TO_RUN" -skip_gameinfo -homepath /tmp -nvram_directory /tmp -cfg_directory /tmp -snapshot_directory /tmp \
  -state_directory /tmp -diff_directory /tmp -input_directory /tmp > /out/mame.txt 2>&1
wait $AWK
"""


def cmd_gmv2frames(a) -> int:
    fr = gmv_frames(pathlib.Path(a.gmv).read_bytes())
    pathlib.Path(a.out).write_bytes(fr)
    print(json.dumps({"frames": len(fr), "frames_with_input": sum(1 for b in fr if b)}))
    return 0


def cmd_sites(a) -> int:
    d = pathlib.Path(a.sample)
    m = json.loads((d / "meta.json").read_text())
    img = pathlib.Path(m["image"]).read_bytes()
    s = indirect_sites(img, (d / "classes.bin").read_bytes())
    pathlib.Path(a.out).write_text(json.dumps(s))
    print(json.dumps({"sites": len(s)}))
    return 0


def cmd_trace(a) -> int:
    out = pathlib.Path(a.out)
    ge.require_private(out.parent)
    out.mkdir(parents=True, exist_ok=False)
    ge.container_dir(out)
    inp = out / "in"
    inp.mkdir()
    img_path, ident = ge.stage_image(pathlib.Path(a.rom), inp)
    sc = json.loads(a.scenario)
    (inp / "scenario.lua").write_text(scenario_lua(sc, ident["size"]))
    (inp / "trace.sh").write_text(TRACE_SH)
    frames = inp / "frames.bin"
    frames.write_bytes(pathlib.Path(a.frames).read_bytes() if a.frames else b"")
    sites = json.loads(pathlib.Path(a.sites).read_text()) if a.sites else []
    for p in (inp, inp / "scenario.lua", inp / "trace.sh", frames):
        p.chmod(0o755 if p.is_dir() else 0o644)
    t0 = time.time()
    seconds = int(sc["seconds"]) + (60 if sc["policy"] == "explore" else 0)
    cmd = ["docker", "run", "--rm", "--network", "none", "--user", "1000:1000", "--cpus", "1", "--memory", "2g",
           "-e", f"SECONDS_TO_RUN={seconds}", "-e", "SITES=" + ",".join(f"{s:06X}" for s in sites),
           "-v", f"{img_path}:/in/rom.bin:ro", "-v", f"{inp / 'scenario.lua'}:/in/scenario.lua:ro", "-v", f"{inp / 'trace.sh'}:/in/trace.sh:ro",
           "-v", f"{frames}:/in/frames.bin:ro", "-v", f"{out}:/out", ge.EVIDENCE_IMAGE, "sh", "/in/trace.sh"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=int(a.timeout))
    wall = time.time() - t0
    if r.returncode:
        raise ge.EvidenceError(f"trace container failed: {r.stderr[-400:]}")
    run = dict(ln.split() for ln in (out / "run.txt").read_text().splitlines() if ln.strip())
    first = ge.parse_trace_first((out / "pcs_first.txt").read_text(errors="replace").splitlines())
    with open(out / "vdp.log") as f:
        vdp = ge.replay_vdp_log(f)
    reads = ge.parse_reads((out / "reads.txt").read_text().split(), ident["size"])
    tap = set()
    for v in reads.values():
        tap |= v
    raw_ext = vdp.rom_extents(ident["size"])
    ext = [tuple(x) for s0, e0 in raw_ext for x in ge.ranges(sorted(k for k in range(s0, e0) if k in tap))]
    img_b = img_path.read_bytes()
    vectors = {ge.u32(img_b, 4 * i) for i in range(1, 64)}
    edges = sorted({tuple(int(x, 16) for x in ln.split()) for ln in (out / "edges.txt").read_text().splitlines() if ln.strip()})
    edges = [e for e in edges if e[1] not in vectors]      # an exception entry right after a site is not that site's target
    (out / "pcs.json").write_text(json.dumps({"rom": {str(k): v for k, v in sorted(first["rom"].items())}, "ram": first["ram"]}))
    (out / "reads.json").write_text(json.dumps({str(pc): ge.ranges(sorted(v)) for pc, v in sorted(reads.items())}))
    (out / "dma.json").write_text(json.dumps({"events": [[s, n, c] for (s, n), c in sorted(vdp.events.items())], "rom_extents": ext}))
    (out / "edges.json").write_text(json.dumps(edges))
    res = {"schema": "segarecomp.genesis_trace2.v1", "scenario": sc, "image": ident, "frames": int(run["frames"]),
           "map_mismatch": int(run["map_mismatch"]), "rom_pcs": len(first["rom"]), "ram_pcs": len(first["ram"]),
           "rom_pcs_beyond_image": sum(1 for p in first["rom"] if p >= ident["size"]), "edges": len(edges), "sites": len(sites),
           "readers": len(reads), "dma_rom_extents": len(ext), "dma_unknown_register_commands": vdp.unknown_dma, "wall_seconds": round(wall, 1),
           "pcs_sha256": ge.sha256(json.dumps([sorted(first["rom"]), first["ram"]]).encode()),
           "edges_sha256": ge.sha256(json.dumps(edges).encode()), "extraction_version": ge.EXTRACTION_VERSION,
           "plan_sha256": None, "emulator": {"image": ge.EVIDENCE_IMAGE, "image_id": ge.docker_image_id(ge.EVIDENCE_IMAGE)}}
    (out / "trace.json").write_text(json.dumps(res, indent=1, sort_keys=True))
    for f in ("vdp.log", "reads.txt", "pcs_first.txt", "edges.txt"):
        (out / f).unlink()
    print(json.dumps({k: res[k] for k in ("frames", "map_mismatch", "rom_pcs", "ram_pcs", "edges", "sites", "wall_seconds", "pcs_sha256")}))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gmv2frames")
    g.add_argument("--gmv", required=True)
    g.add_argument("--out", required=True)
    g.set_defaults(fn=cmd_gmv2frames)
    s = sub.add_parser("sites")
    s.add_argument("--sample", required=True)
    s.add_argument("--out", required=True)
    s.set_defaults(fn=cmd_sites)
    t = sub.add_parser("trace")
    t.add_argument("--rom", required=True)
    t.add_argument("--scenario", required=True)
    t.add_argument("--out", required=True)
    t.add_argument("--frames")
    t.add_argument("--sites")
    t.add_argument("--timeout", default=10800)
    t.set_defaults(fn=cmd_trace)
    a = ap.parse_args(argv)
    try:
        return a.fn(a)
    except ge.EvidenceError as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
