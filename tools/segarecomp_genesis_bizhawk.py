#!/usr/bin/env python3
"""SEG-080-T001: BizHawk-replayed TAS versus equal-duration control acquisition experiment (OFFLINE research tooling).

A narrow adapter around the merged SEG-077 evidence pipeline (`segarecomp_genesis_evidence`, unchanged). The tracer here is the TAS's own
original emulator: BizHawk 2.9.1 (Genplus-gx waterbox core) driven by a Lua script that uses only BizHawk's native M68K bus callbacks.

  Captured per run (nothing else is stored):
    * every distinct executed 68000 PC (`event.onmemoryexecuteany`, scope "M68K BUS") with the frame bucket of its first execution;
    * every distinct non-sequential transition (previous PC -> PC) for indirect-target statistics;
    * every distinct (reader PC, ROM byte) data read (`event.onmemoryread`, address = nil); the reader is the PC last reported by the
      execute callback. The callback reports an address, not an access size: an even address could be a byte or a word read, so the
      conservative rule records exactly the one byte AT the reported address. That under-approximates reads, never over-approximates;
    * five frozen checkpoints (zone/act, player position, work-RAM digest, optional screenshot digest) and trace growth every 1800 frames;
    * the ROM mapping check (every ROM word read through the bus equals the image).
  Not available under this tracer (and therefore disabled identically for both conditions): VDP control-port write DATA (the callback
  value is always 0), hence the DMA-source tier. Graphics DMA sources therefore stay U.

Subcommands:
  identity   pin ROM / movie / emulator identities (aggregate record)
  run        one scenario in BizHawk (movie | random | playback) -> a trace directory in the SEG-077 T001 layout (+ checkpoints.json)
  label      unchanged SEG-077 `label()` over trace directories (frozen plan check replaced by this task's own plan)
  evaluate   independent source-truth evaluation (Gate B), marginal TAS-over-control yield (Gate C) and the decision
  freeze / verify-plan   the pre-registration (plan + digest binding code and input identities)

Private outputs live only under an ignored directory; durable output is aggregate-only.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import random
import resource
import shutil
import subprocess
import sys
import time
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_genesis_evidence as ge  # noqa: E402

SCHEMA = "segarecomp.genesis_bizhawk.v1"
PLAN = HERE / "segarecomp_genesis_bizhawk.t001.plan.json"
PLAN_DIGEST = HERE / "segarecomp_genesis_bizhawk.t001.plan.digest"
ROM_SHA1 = "8bca5dcef1af3e00098666fd892dc1c2a76333f9"
ROM_SHA256 = "193bc4064ce0daf27ea9e908ed246d87ec576cc294833badebb590b6ad8e8f6b"
ROM_SIZE = 1048576
FRAMES = 54000
CHECKPOINTS = [7200, 18000, 28800, 39600, 54000]
GROWTH_EVERY = 1800
CELL = ge.CELL_BYTES

PROTOCOL = {
    "schema": SCHEMA + ".plan",
    "task": "SEG-080-T001",
    "game": "Sonic the Hedgehog 2 (World, REV01)",
    "rom": {"size": ROM_SIZE, "sha1": ROM_SHA1, "sha256": ROM_SHA256},
    "movie": "tasvideos 5640M, BizHawk 2.9.1 .bk2, core Genplus-gx, 2-player (6-button sync setting), starts from power-on",
    "emulator": "BizHawk 2.9.1 official linux-x64 package under Mono; the movie header names version 2.9.1 and core Genplus-gx",
    "window": {"frames": FRAMES, "note": "first 15 minutes at 60 frames/s of emulated gameplay from power-on; not adapted after inspection",
               "checkpoints": CHECKPOINTS, "growth_every_frames": GROWTH_EVERY},
    "control": {"policy": "SEG-077 `random` policy (segarecomp_genesis_evidence.LUA), ported verbatim", "seed": 1, "start_window": 40,
                "seconds": FRAMES // 60},
    "gate_a": {
        "needs": ["movie/ROM identities", "no savestate in the movie", "movie plays to its end in the original emulator",
                  "five frozen checkpoints show actual progression (>= 2 different zone/act values, game mode in-level at >= 2)",
                  "trace repeat: identical PC-set digest, read-set digest and checkpoint state digests",
                  "trace fidelity: ROM mapping mismatch 0; every PC even; accepted-E fraction of executed ROM PCs >= 0.99; "
                  "execute callbacks seen at the reset-vector PC"],
        "failure": ["REPLAY_INVALID", "TRACING_UNAVAILABLE"]},
    "gate_b": {"new_label_accuracy_min": 0.999, "code_as_data_conflicts_max": 0,
               "subjects": ["all TAS labels", "TAS labels absent (same class) from the control labels"],
               "separate_metrics": ["I (start)", "c (continuation)", "D"], "failure": "LABELS_UNSAFE"},
    "gate_c": {"extra_code_positive_cells_min": 24, "extra_mixed_cells_min": 8, "failure": "INSUFFICIENT_CODE_YIELD",
               "secondary": {"extra_nontrivial_negative_cells_min": 200, "label": "DATA_ONLY_PROMISE"},
               "nontrivial_negative": "a fully-D 128-byte cell whose bytes are not all one repeated value",
               "extra": "cell index labelled correctly by the TAS under the cell predicate, not satisfying it in the control labels",
               "correct": "every I/c/D label inside the cell agrees with the independent source truth"},
    "limits": {"cpu_hours_tracing_and_verification": 8, "engineering_hours_compatibility": 3, "sync_correction_attempts": 1},
    "tiers": "SEG-077 `label()` unchanged; DMA-source tier absent for both conditions; DR read rule: one byte at the reported address",
    "novelty": "descriptive only (no threshold): 4-gram of (family, ea_src, ea_dst, width, control) signatures over consecutive labelled "
               "starts, and 3-cell class-pattern windows, against every SEG-072 registry image except Sonic 2",
    "evaluator_isolation": "the source-truth classes of `s2` are read only by `evaluate`; `run` and `label` never open them",
    "decision": ["REPLAY_INVALID", "TRACING_UNAVAILABLE", "LABELS_UNSAFE", "INSUFFICIENT_CODE_YIELD", "ACQUISITION_PROMISING",
                 "BUDGET_EXHAUSTED"],
    "disclosure": "Sonic 2 was used in SEG-072 and in SEG-077 rule selection: a controlled known-truth engineering test, not blind",
}
PLAN_EXTRA = ("bound_code", "inputs")
BOUND = ["tools/segarecomp_genesis_bizhawk.py", "tools/segarecomp_genesis_evidence.py"]


# ------------------------------------------------------------------------------------------------------------------------- Lua
LUA = r"""
local S = {mode = "__MODE__", frames = __FRAMES__, seed = __SEED__, start_window = __SW__, size = __SIZE__, out = "__OUT__",
            rom = "__ROM__", ck = {__CK__}, growth = __GROWTH__}
local function w(name) return assert(io.open(S.out .. "/" .. name, "w")) end
local function ts() return os.clock() end
local isck = {} for _, c in ipairs(S.ck) do isck[c] = true end
local pcs, nuniq = {}, 0
local edges, nedge = {}, 0
local reads, nread = {}, 0
local prev, lastpc, frame_now = -1, 0, 0
local collect = S.mode ~= "playback"
local run = w("run.txt")
run:write("mode ", S.mode, "\nmovie_mode_start ", tostring(movie.mode()), "\nmovie_length ", tostring(movie.length()), "\n")
-- ROM mapping check through the M68K bus (no callbacks are registered yet)
local mis, words = 0, 0
do
  local f = assert(io.open(S.rom, "rb")) local rom = f:read("a") f:close()
  for a = 0, S.size - 2, 2 do
    local v = (rom:byte(a + 1) << 8) | rom:byte(a + 2)
    if memory.read_u16_be(a, "M68K BUS") ~= v then mis = mis + 1 end
    words = words + 1
  end
end
run:write("map_words ", words, "\nmap_mismatch ", mis, "\n")
local reset_pc = memory.read_u32_be(4, "M68K BUS")
run:write("reset_pc ", reset_pc, "\n")
if collect then
  event.onmemoryexecuteany(function(a)
    a = a & 0xFFFFFF
    if not pcs[a] then pcs[a] = frame_now // S.growth nuniq = nuniq + 1 end
    if prev >= 0 then
      local d = a - prev
      if d <= 0 or d > 10 then
        local k = (prev << 24) | a
        if not edges[k] then edges[k] = true nedge = nedge + 1 end
      end
    end
    prev = a lastpc = a
  end, "x", "M68K BUS")
  event.onmemoryread(function(a)
    if a < S.size then
      local off, lane = a, 1
      if a & 1 == 1 then off, lane = a - 1, 2 end
      local k = (lastpc << 26) | (off << 2) | lane
      if not reads[k] then reads[k] = true nread = nread + 1 end
    end
  end, nil, "r", "M68K BUS")
end
-- control policy: SEG-077 `random` policy, verbatim
local names = {"P1 Up", "P1 Down", "P1 Left", "P1 Right", "P1 A", "P1 B", "P1 C", "P1 Start"}
local state = S.seed * 2654435761 % 2147483648 + 1
local function rnd(k) state = (state * 1103515245 + 12345) % 2147483648 return state % k end
local act = nil
local function policy(frame)
  if frame >= (S.frames // 60) * 60 then return {} end
  local p = {}
  if frame < S.start_window * 60 then
    if frame % 120 < 6 then p["P1 Start"] = 1 end
    if frame % 120 >= 60 and frame % 120 < 66 then p["P1 A"] = 1 end
    return p
  end
  local f = frame % 900
  if f < 4 or (f >= 24 and f < 28) then p["P1 Start"] = 1 return p end
  if frame % 8 == 0 or act == nil then
    local d = rnd(100)
    local a = {}
    if d < 50 then a["P1 Right"] = 1 elseif d < 65 then a["P1 Left"] = 1 elseif d < 75 then a["P1 Up"] = 1 elseif d < 85 then a["P1 Down"] = 1 end
    if rnd(100) < 30 then a["P1 A"] = 1 end
    if rnd(100) < 30 then a["P1 B"] = 1 end
    if rnd(100) < 30 then a["P1 C"] = 1 end
    act = a
  end
  return act
end
local ckf = w("checkpoints.txt")
local function snap(tag)
  local function b(a) return memory.read_u8(a, "M68K BUS") end
  local function wd(a) return memory.read_u16_be(a, "M68K BUS") end
  local ram = memory.hash_region(0xFF0000, 0x10000, "M68K BUS")
  local vram = memory.hash_region(0, 0x10000, "VRAM")
  local shot = ""
  if isck[frame_now] then
    local path = S.out .. "/shot_" .. frame_now .. ".png"
    local ok = pcall(function() client.screenshot(path) end)
    shot = ok and path or ""
  end
  ckf:write(string.format("%s\t%d\tzone=%d act=%d mode=%d x=%d y=%d rings=%d lives=%d ram=%s vram=%s uniq=%d reads=%d edges=%d shot=%s\n",
    tag, emu.framecount(), b(0xFFFE10), b(0xFFFE11), b(0xFFF600), wd(0xFFB008), wd(0xFFB00C), wd(0xFFFE20), b(0xFFFE12), ram, vram,
    nuniq, nread, nedge, shot))
  ckf:flush()
end
local t0 = ts()
local last = S.frames
if S.mode == "playback" then last = movie.length() end
local ended = -1
for f = 0, last - 1 do
  frame_now = f
  if S.mode == "random" then
    local p = policy(f + 1)
    local t = {}
    for _, n in ipairs(names) do t[n] = p[n] == 1 end
    joypad.set(t)
  end
  emu.frameadvance()
  frame_now = f + 1
  if S.mode == "movie" or S.mode == "playback" then
    if ended < 0 and movie.mode() == "FINISHED" then ended = frame_now end
  end
  if isck[frame_now] then snap("ck") elseif frame_now % S.growth == 0 then snap("g") end
end
snap("final")
run:write("frames ", emu.framecount(), "\nmovie_mode_end ", tostring(movie.mode()), "\nmovie_finished_at ", ended, "\ncpu_lua ", ts() - t0, "\n")
run:close() ckf:close()
if collect then
  local o = w("pcs.txt") for a, bkt in pairs(pcs) do o:write(string.format("%06x %d\n", a, bkt)) end o:close()
  o = w("edges.txt") for k in pairs(edges) do o:write(string.format("%06x %06x\n", k >> 24, k & 0xFFFFFF)) end o:close()
  o = w("reads.txt") for k in pairs(reads) do o:write(string.format("%x\n", k)) end o:close()
end
local d = w("done") d:write("ok\n") d:close()
client.exit()
"""


def render_lua(sc: dict, out: str, rom: str) -> str:
    sub = {"__MODE__": sc["mode"], "__FRAMES__": str(int(sc["frames"])), "__SEED__": str(int(sc.get("seed", 0))),
           "__SW__": str(int(sc.get("start_window", 0))), "__SIZE__": str(ROM_SIZE), "__OUT__": out, "__ROM__": rom,
           "__CK__": ",".join(str(c) for c in sc.get("checkpoints", CHECKPOINTS)), "__GROWTH__": str(GROWTH_EVERY)}
    if sc["mode"] not in ("movie", "random", "playback"):
        raise ge.EvidenceError(f"unknown mode {sc['mode']}")
    out_lua = LUA
    for k, v in sub.items():
        out_lua = out_lua.replace(k, v)
    return out_lua


# ----------------------------------------------------------------------------------------------------------------------- identity
def sha256_file(p: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def inspect_movie(path: pathlib.Path) -> dict:
    """Header, sync settings, input-log shape and frame count of a .bk2 (read-only)."""
    with zipfile.ZipFile(path) as z:
        names = sorted(z.namelist())
        header = z.read("Header.txt").decode()
        sync = json.loads(z.read("SyncSettings.json").decode())
        log = z.read("Input Log.txt").decode().splitlines()
    rows = [ln for ln in log if ln.startswith("|")]
    hdr = dict(ln.split(" ", 1) for ln in header.splitlines() if " " in ln)
    key = next(ln for ln in log if ln.startswith("LogKey:"))
    return {"sha256": sha256_file(path), "entries": names, "starts_from_savestate": any("State" in n and n != "BizState 1.0" for n in names)
            or "StartsFromSavestate" in header, "header": {k: hdr.get(k) for k in ("emuVersion", "OriginalEmuVersion", "Platform", "GameName",
            "SHA1", "Core", "rerecordCount")}, "sync": sync["o"], "frames": len(rows), "log_key_sha256": ge.sha256(key.encode()),
            "input_log_sha256": ge.sha256("\n".join(rows).encode()), "players_in_log": 2 if "#P2 Up" in key else 1,
            "p2_frames_with_input": sum(1 for r in rows if any(c != "." for c in r.split("|")[3]) )}


def cmd_identity(a) -> int:
    rom = pathlib.Path(a.rom).read_bytes()
    ident = {"rom": {"size": len(rom), "sha1": hashlib.sha1(rom).hexdigest(), "sha256": hashlib.sha256(rom).hexdigest(),
                     "md5": hashlib.md5(rom).hexdigest()}}
    ident["rom_ok"] = (ident["rom"]["size"], ident["rom"]["sha1"], ident["rom"]["sha256"]) == (ROM_SIZE, ROM_SHA1, ROM_SHA256)
    ident["movie"] = inspect_movie(pathlib.Path(a.movie))
    bh = pathlib.Path(a.bizhawk)
    ident["emulator"] = {"package_sha256": sha256_file(pathlib.Path(a.package)) if a.package else None,
                         "EmuHawk.exe": sha256_file(bh / "EmuHawk.exe"), "gpgx.wbx.zst": sha256_file(next(bh.rglob("gpgx.wbx.zst"))),
                         "mono": subprocess.run(["mono", "--version"], capture_output=True, text=True).stdout.splitlines()[0]}
    mh = ident["movie"]["header"]
    ident["movie_rom_md5_matches"] = (mh["SHA1"] or "").lower() == ident["rom"]["md5"]   # BizHawk labels its MD5 field "SHA1" here
    print(json.dumps(ident, indent=1, sort_keys=True))
    if a.out:
        pathlib.Path(a.out).write_text(json.dumps(ident, indent=1, sort_keys=True))
    return 0 if ident["rom_ok"] and ident["movie_rom_md5_matches"] else 2


# -------------------------------------------------------------------------------------------------------------------------- run
def cmd_run(a) -> int:
    out = pathlib.Path(a.out)
    ge.require_private(out.parent if not out.exists() else out)
    sc = json.loads(a.scenario)
    user, bh = a.user, pathlib.Path(a.bizhawk)
    job = pathlib.Path(a.jobs) / sc["id"]
    if job.exists():
        shutil.rmtree(job)
    job.mkdir(parents=True)
    shutil.copy(a.rom, job / "rom.bin")
    if sc["mode"] in ("movie", "playback"):
        shutil.copy(a.movie, job / "movie.bk2")
    (job / "scenario.lua").write_text(render_lua(sc, str(job), str(job / "rom.bin")))
    if os.geteuid() == 0:
        subprocess.run(["chown", "-R", user, str(job)], check=True)
    disp = f":{int(a.display)}"
    xvfb = subprocess.Popen(["Xvfb", disp, "-screen", "0", "640x480x24"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    emu = (f"cd {bh} && DISPLAY={disp} HOME=/home/{user} LD_LIBRARY_PATH={bh} timeout {int(a.timeout)} mono EmuHawk.exe "
           f"--lua={job / 'scenario.lua'} " + (f"--movie={job / 'movie.bk2'} " if sc["mode"] in ("movie", "playback") else "") +
           f"{job / 'rom.bin'} > {job / 'bizhawk.log'} 2>&1")
    cmd = ["su", user, "-s", "/bin/bash", "-c", emu] if os.geteuid() == 0 else ["bash", "-c", emu]
    r0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.time()
    try:
        subprocess.run(cmd, timeout=int(a.timeout) + 60)
    finally:
        xvfb.terminate()
    wall = time.time() - t0
    r1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (r1.ru_utime + r1.ru_stime) - (r0.ru_utime + r0.ru_stime)
    if not (job / "done").exists():
        raise ge.EvidenceError(f"BizHawk run did not complete: see {job / 'bizhawk.log'}")
    run = dict(ln.split(" ", 1) for ln in (job / "run.txt").read_text().splitlines() if " " in ln)
    out.mkdir(parents=True, exist_ok=True)
    ck = [ln.split("\t") for ln in (job / "checkpoints.txt").read_text().splitlines()]
    res = {"schema": SCHEMA + ".trace", "scenario": sc, "frames": int(run["frames"]), "map_words": int(run["map_words"]),
           "map_mismatch": int(run["map_mismatch"]), "movie_mode_start": run["movie_mode_start"], "movie_mode_end": run["movie_mode_end"],
           "movie_finished_at": int(run["movie_finished_at"]), "movie_length": run["movie_length"], "reset_pc": int(run["reset_pc"]),
           "wall_seconds": round(wall, 1), "cpu_seconds": round(cpu, 1)}
    (out / "checkpoints.json").write_text(json.dumps([{"tag": t[0], "frame": int(t[1]), "state": t[2]} for t in ck], indent=0))
    for shot in sorted(job.glob("shot_*.png")):
        shutil.copy(shot, out / shot.name)
    if sc["mode"] != "playback":
        pcs = {}
        for ln in (job / "pcs.txt").read_text().split("\n"):
            if ln:
                p, b = ln.split()
                pcs[int(p, 16)] = int(b)
        rom_pcs = sorted(p for p in pcs if p < ge.CART_SPACE)
        ram = sorted(p for p in pcs if p >= ge.RAM_START)
        other = sum(1 for p in pcs if ge.CART_SPACE <= p < ge.RAM_START)
        reads = ge.parse_reads((job / "reads.txt").read_text().split(), ROM_SIZE)
        edges = [tuple(int(x, 16) for x in ln.split()) for ln in (job / "edges.txt").read_text().splitlines() if ln]
        read_bytes = set()
        for s in reads.values():
            read_bytes |= s
        growth = {}
        for p, b in pcs.items():
            growth[b] = growth.get(b, 0) + 1
        res.update({"image": {"sha256": ROM_SHA256, "size": ROM_SIZE, "format": "plain"}, "rom_pcs": len(rom_pcs), "ram_pcs": len(ram),
                    "other_pcs": other, "odd_pcs": sum(1 for p in pcs if p & 1), "rom_pcs_beyond_image": sum(1 for p in rom_pcs if p >= ROM_SIZE),
                    "readers": len(reads), "read_bytes": len(read_bytes), "edges": len(edges), "dma_rom_extents": 0, "dma_events": 0, "dma_sha256": ge.sha256(b"[]"),
                    "pcs_sha256": ge.sha256(json.dumps([rom_pcs, ram]).encode()),
                    "reads_sha256": ge.sha256(json.dumps(sorted((pc, sorted(s)) for pc, s in reads.items())).encode()),
                    "edges_sha256": ge.sha256(json.dumps(sorted(edges)).encode()),
                    "new_pcs_per_growth_window": {str(k): v for k, v in sorted(growth.items())},
                    "reset_pc_executed": res["reset_pc"] in pcs})
        (out / "pcs.json").write_text(json.dumps({"rom": {str(p): "" for p in rom_pcs}, "ram": ram}))
        (out / "reads.json").write_text(json.dumps({str(pc): ge.ranges(sorted(s)) for pc, s in sorted(reads.items())}))
        (out / "dma.json").write_text(json.dumps({"events": [], "rom_extents": []}))
        (out / "edges.json").write_text(json.dumps(sorted(edges)))
    (out / "trace.json").write_text(json.dumps(res, indent=1, sort_keys=True))
    shutil.rmtree(job)
    print(json.dumps({k: res.get(k) for k in ("frames", "map_mismatch", "rom_pcs", "ram_pcs", "odd_pcs", "readers", "read_bytes", "edges",
                                              "pcs_sha256", "reads_sha256", "movie_finished_at", "wall_seconds", "cpu_seconds")}))
    return 0


# ------------------------------------------------------------------------------------------------------------------------ label
def cmd_label(a) -> int:
    ge.PLAN = pathlib.Path("/nonexistent/seg080-no-t001-plan")      # the T001 plan binds MAME; this task has its own plan
    meta = ge.label(pathlib.Path(a.rom), [pathlib.Path(r) for r in a.runs], None, a.cli, pathlib.Path(a.out), a.lineage, a.id)
    print(json.dumps(ge.sanitized(meta), sort_keys=True))
    return 0


# -------------------------------------------------------------------------------------------------------------------- evaluation
def cell_sets(cls: bytes, truth: bytes, img: bytes) -> dict:
    """Per-cell predicates over a prediction and truth. A cell is `correct` only when every I/c/D label inside it agrees with truth."""
    n = (len(cls) + CELL - 1) // CELL
    pos, neg, mixed, bad = set(), set(), set(), set()
    for c in range(n):
        s, e = c * CELL, min(len(cls), (c + 1) * CELL)
        ch, th = cls[s:e], truth[s:e]
        if any(p != 85 and p != t for p, t in zip(ch, th)):     # 85 == ord("U"): abstention
            bad.add(c)
            continue
        if b"I" in ch:
            pos.add(c)
            if b"D" in ch:
                mixed.add(c)
        elif ch.count(b"D") == len(ch) and len(set(img[s:e])) > 1:
            neg.add(c)
    return {"pos": pos, "neg": neg, "mixed": mixed, "bad": bad}


def class_accuracy(pred: bytes, truth: bytes, other: bytes | None = None) -> dict:
    """Per-class soundness of `pred` against complete truth over labels that differ from `other` (None: all labels)."""
    out = {}
    for ch in "IcD":
        code = ord(ch)
        lab = wrong = 0
        for i, (p, t) in enumerate(zip(pred, truth)):
            if p != code or (other is not None and other[i] == code):
                continue
            lab += 1
            wrong += p != t
        out[ch] = {"labelled": lab, "wrong": wrong, "accuracy": None if not lab else round(1 - wrong / lab, 6)}
    cad = sum(1 for p, t in zip(pred, truth) if p == ord("D") and t in (ord("I"), ord("c")) and (other is None or True))
    out["code_as_data"] = cad if other is None else sum(1 for i, (p, t) in enumerate(zip(pred, truth)) if p == ord("D") and other[i] != ord("D")
                                                        and t in (ord("I"), ord("c")))
    out["data_as_code"] = sum(1 for p, t in zip(pred, truth) if p in (ord("I"), ord("c")) and t == ord("D"))
    out["false_starts"] = sum(1 for p, t in zip(pred, truth) if p == ord("I") and t != ord("I"))
    out["error_bearing_cells"] = len({i // CELL for i, (p, t) in enumerate(zip(pred, truth)) if p != 85 and p != t})
    out["labels_over_nothing_known"] = 0      # the registry truth is complete (no reference-unknown bytes for s2)
    return out


def pick_gate_b(tas_cls: bytes, ctl_cls: bytes, truth: bytes) -> dict:
    allr = class_accuracy(tas_cls, truth)
    new = class_accuracy(tas_cls, truth, ctl_cls)
    thr = PROTOCOL["gate_b"]["new_label_accuracy_min"]
    ok = all(v["labelled"] == 0 or v["accuracy"] >= thr for v in (new["I"], new["c"], new["D"], allr["I"], allr["c"], allr["D"]))
    ok = ok and new["code_as_data"] == 0 and allr["code_as_data"] == 0
    return {"all": allr, "new": new, "pass": bool(ok)}


def marginal(tas: dict, ctl: dict) -> dict:
    return {k: sorted(tas[k] - ctl[k]) for k in ("pos", "neg", "mixed")}


def bootstrap_extra(tas: dict, ctl: dict, blocks: int = 64, reps: int = 2000, seed: int = 80) -> dict:
    """Region-level (equal address blocks) bootstrap of the TAS-minus-control extra cell counts. Descriptive, not a threshold."""
    cells = (ROM_SIZE + CELL - 1) // CELL
    per = cells // blocks
    rows = []
    for b in range(blocks):
        lo, hi = b * per, (b + 1) * per
        rows.append(tuple(len({c for c in (tas[k] - ctl[k]) if lo <= c < hi}) for k in ("pos", "mixed", "neg")))
    rng = random.Random(seed)
    sums = [[0, 0, 0] for _ in range(reps)]
    for r in range(reps):
        for _ in range(blocks):
            x = rows[rng.randrange(blocks)]
            for j in range(3):
                sums[r][j] += x[j]
    out = {}
    for j, k in enumerate(("extra_pos", "extra_mixed", "extra_neg")):
        v = sorted(s[j] for s in sums)
        out[k] = {"p2.5": v[int(0.025 * reps)], "median": v[reps // 2], "p97.5": v[int(0.975 * reps)]}
    return out


def decide(gate_a: dict, gate_b: dict | None, gate_c: dict | None, cpu_hours: float) -> dict:
    """The frozen decision logic (pure)."""
    lim = PROTOCOL["limits"]["cpu_hours_tracing_and_verification"]
    if cpu_hours > lim:
        return {"primary": "BUDGET_EXHAUSTED", "secondary": None}
    if not gate_a.get("replay_ok", False):
        return {"primary": "REPLAY_INVALID", "secondary": None}
    if not gate_a.get("tracing_ok", False):
        return {"primary": "TRACING_UNAVAILABLE", "secondary": None}
    if gate_b is None or not gate_b["pass"]:
        return {"primary": "LABELS_UNSAFE", "secondary": None}
    c = PROTOCOL["gate_c"]
    code_ok = gate_c["extra_pos_correct"] >= c["extra_code_positive_cells_min"] and gate_c["extra_mixed_correct"] >= c["extra_mixed_cells_min"]
    data_ok = gate_c["extra_neg_correct"] >= c["secondary"]["extra_nontrivial_negative_cells_min"]
    if code_ok:
        return {"primary": "ACQUISITION_PROMISING", "secondary": None}
    return {"primary": "INSUFFICIENT_CODE_YIELD", "secondary": "DATA_ONLY_PROMISE" if data_ok else None}


def signatures(img: bytes, cls: bytes, tokens: bytes) -> list[tuple]:
    """Consecutive labelled starts -> signature tuples (family, ea_src, ea_dst, width, control) from the CPU-owned token report."""
    sigs, last_end = [], -1
    for pc in range(0, len(cls), 2):
        if cls[pc] != ord("I"):
            continue
        t = tokens[(pc // 2) * ge.TOKEN_FIELDS:(pc // 2 + 1) * ge.TOKEN_FIELDS]
        n = ge.token_len(tokens, pc)
        sig = (t[2], t[4], t[5], t[6], t[3])
        sigs.append((pc, sig, pc == last_end))
        last_end = pc + (n or 2)
    return sigs


def ngrams(sigs, k: int = 4) -> set:
    out, run = set(), []
    for _pc, sig, contiguous in sigs:
        run = (run + [sig]) if contiguous else [sig]
        if len(run) >= k:
            out.add(tuple(run[-k:]))
    return out


def cell_patterns(cls: bytes) -> set:
    n = (len(cls) + CELL - 1) // CELL
    kind = []
    for c in range(n):
        ch = cls[c * CELL:(c + 1) * CELL]
        kind.append("M" if b"I" in ch and b"D" in ch else "P" if b"I" in ch else "N" if ch.count(b"D") == len(ch) else "U")
    return {"".join(kind[i:i + 3]) for i in range(n - 2)}



# ------------------------------------------------------------------------------------------------------------------------- gates
def load_label_dir(d: pathlib.Path) -> dict:
    meta = json.loads((d / "meta.json").read_text())
    return {"meta": meta, "cls": (d / "classes.bin").read_bytes(), "tokens": (d / "tokens.bin").read_bytes()}


def parse_state(s: str) -> dict:
    return dict(kv.split("=", 1) for kv in s.split())


def gate_a(identity: dict, playback: dict, tas_a: dict, tas_b: dict, ck_a: list, ck_b: list, labelled: dict) -> dict:
    """Gate A criteria over aggregate run records (pure)."""
    movie = identity["movie"]
    ident_ok = bool(identity["rom_ok"] and identity["movie_rom_md5_matches"] and not movie["starts_from_savestate"]
                    and movie["players_in_log"] == 2 and movie["header"]["Core"] == "Genplus-gx"
                    and movie["header"]["emuVersion"].endswith("2.9.1"))
    frozen = {c: next(x for x in ck_a if x["tag"] == "ck" and x["frame"] == c) for c in CHECKPOINTS}
    st = {c: parse_state(frozen[c]["state"]) for c in CHECKPOINTS}
    level = {c: (st[c]["zone"], st[c]["act"]) for c in CHECKPOINTS if int(st[c]["mode"]) == 12}
    progression = len(set(level.values())) >= 2
    played_to_end = playback["movie_finished_at"] > 0 or playback["movie_mode_end"] == "FINISHED"
    fingerprint = lambda ck: [(x["frame"], x["state"].split(" shot=")[0]) for x in ck]
    determinism = {"pcs": tas_a["pcs_sha256"] == tas_b["pcs_sha256"], "reads": tas_a["reads_sha256"] == tas_b["reads_sha256"],
                   "edges": tas_a["edges_sha256"] == tas_b["edges_sha256"], "checkpoints": fingerprint(ck_a) == fingerprint(ck_b)}
    typed = labelled["meta"]["typed"]
    executed = labelled["meta"]["decoders"]["executed_rom_pcs"]
    rejected = sum(v for k, v in typed.items() if k.startswith("E_"))
    accepted = labelled["meta"]["tiers"]["E"] / executed if executed else 0
    fidelity = {"map_mismatch": tas_a["map_mismatch"], "odd_pcs": tas_a["odd_pcs"], "accepted_E_fraction": round(accepted, 5),
                "reset_pc_executed": tas_a["reset_pc_executed"], "e_rejections": rejected}
    fid_ok = tas_a["map_mismatch"] == 0 and tas_a["odd_pcs"] == 0 and accepted >= 0.99 and tas_a["reset_pc_executed"]
    replay_ok = ident_ok and played_to_end and progression and tas_a["frames"] == FRAMES
    return {"identity_ok": ident_ok, "played_to_end": played_to_end, "movie_length_frames": movie["frames"], "progression": progression,
            "checkpoint_levels": {str(c): list(v) for c, v in level.items()}, "determinism": determinism, "fidelity": fidelity,
            "replay_ok": replay_ok, "tracing_ok": bool(fid_ok and all(determinism.values()))}


def hard_cells(cls: bytes, img: bytes, tokens: bytes, neg: set) -> set:
    n = (len(cls) + CELL - 1) // CELL
    out = set()
    for c in neg:
        near = any(0 <= c + d < n and b"I" in cls[(c + d) * CELL:(c + d + 1) * CELL] for d in (-1, 1))
        ords = sum(1 for k in range(c * CELL, min((c + 1) * CELL, len(img)), 2) if ge.token_len(tokens, k))
        if near or ords * 4 >= CELL:
            out.add(c)
    return out


def indirect_targets(edges: list, img: bytes) -> set:
    out = set()
    for prev, pc in edges:
        w = ge.u16(img, prev) if prev < len(img) else None
        if w is not None and w & 0xFF80 == 0x4E80 and (w & 0x3F) not in (0x38, 0x39, 0x3A):
            out.add(pc)
    return out


def reference_novelty(tas_new: set, tas_all: set, ref_grams: set) -> dict:
    return {"new_ngrams": len(tas_new), "new_ngrams_absent_from_reference": len(tas_new - ref_grams),
            "fraction_absent": round(len(tas_new - ref_grams) / len(tas_new), 4) if tas_new else None,
            "all_ngrams": len(tas_all), "all_absent_from_reference": len(tas_all - ref_grams)}


def cmd_evaluate(a) -> int:
    root = pathlib.Path(a.private)
    img = pathlib.Path(a.rom).read_bytes()
    truth = pathlib.Path(a.truth).read_bytes()          # the ONLY place the source-backed truth is opened
    ident = json.loads(pathlib.Path(a.identity).read_text())
    runs = {k: json.loads((root / "runs" / k / "trace.json").read_text()) for k in ("playback", "tas_a", "tas_b", "ctl_a", "ctl_b")}
    ck = {k: json.loads((root / "runs" / k / "checkpoints.json").read_text()) for k in runs}
    tas, ctl = load_label_dir(root / "labels" / "tas"), load_label_dir(root / "labels" / "ctl")
    ga = gate_a(ident, runs["playback"], runs["tas_a"], runs["tas_b"], ck["tas_a"], ck["tas_b"], tas)
    ga["control_repeat_determinism"] = {k: runs["ctl_a"][k] == runs["ctl_b"][k] for k in ("pcs_sha256", "reads_sha256", "edges_sha256")}
    gb = pick_gate_b(tas["cls"], ctl["cls"], truth)
    gb["control_all"] = class_accuracy(ctl["cls"], truth)
    ts, cs = cell_sets(tas["cls"], truth, img), cell_sets(ctl["cls"], truth, img)
    m = marginal(ts, cs)
    # a cell counts as extra only when it is correct (no label disagrees with the truth) in the TAS labels
    extra = {k: len(m[k]) for k in m}
    th, ch = hard_cells(tas["cls"], img, tas["tokens"], ts["neg"]), hard_cells(ctl["cls"], img, ctl["tokens"], cs["neg"])
    pcs = {k: {int(p) for p in json.loads((root / "runs" / k / "pcs.json").read_text())["rom"]} for k in ("tas_a", "ctl_a")}
    edges = {k: [tuple(e) for e in json.loads((root / "runs" / k / "edges.json").read_text())] for k in ("tas_a", "ctl_a")}
    it, ic = indirect_targets(edges["tas_a"], img), indirect_targets(edges["ctl_a"], img)
    new_I = sum(1 for p, c, t in zip(tas["cls"], ctl["cls"], truth) if p == 73 and c != 73 and t == 73)
    new_c = sum(1 for p, c, t in zip(tas["cls"], ctl["cls"], truth) if p == 99 and c != 99 and t == 99)
    sg_t, sg_c = signatures(img, tas["cls"], tas["tokens"]), signatures(img, ctl["cls"], ctl["tokens"])
    gt_all, gc_all = ngrams(sg_t), ngrams(sg_c)
    ref = set()
    refpat = set()
    for d in sorted(pathlib.Path(a.registry).iterdir()):
        if d.name == "s2" or not (d / "classes.bin").exists():
            continue
        cls_r, tok_r = (d / "classes.bin").read_bytes(), (d / "tokens.bin").read_bytes()
        ref |= ngrams(signatures(b"", cls_r, tok_r))
        refpat |= cell_patterns(cls_r)
    pt, pc_ = cell_patterns(tas["cls"]), cell_patterns(ctl["cls"])
    cpu_h = sum(r["cpu_seconds"] for r in runs.values()) / 3600 + float(a.verification_cpu_hours)
    gc = {"extra_pos_correct": extra["pos"], "extra_mixed_correct": extra["mixed"], "extra_neg_correct": extra["neg"]}
    res = {"schema": SCHEMA + ".results", "gate_a": ga, "gate_b": gb, "gate_c": gc, "cpu_hours": round(cpu_h, 3),
           "pc_sets": {"tas": len(pcs["tas_a"]), "control": len(pcs["ctl_a"]), "intersection": len(pcs["tas_a"] & pcs["ctl_a"]),
                       "union": len(pcs["tas_a"] | pcs["ctl_a"]), "added_by_tas": len(pcs["tas_a"] - pcs["ctl_a"]),
                       "added_by_control": len(pcs["ctl_a"] - pcs["tas_a"])},
           "marginal": {"new_verified_instruction_starts": new_I, "new_verified_continuation_bytes": new_c, "new_pos_cells": extra["pos"],
                        "new_nontrivial_negative_cells": extra["neg"], "new_mixed_cells": extra["mixed"],
                        "new_hard_boundary_windows": len(th - ch), "new_indirect_targets": len(it - ic),
                        "labels_per_cpu_hour_tas_trace": round((new_I + new_c) / max(1e-9, runs["tas_a"]["cpu_seconds"] / 3600), 1)},
           "cells": {"tas": {k: len(v) for k, v in ts.items()}, "control": {k: len(v) for k, v in cs.items()}},
           "tier_counts": {"tas": tas["meta"]["bytes"], "control": ctl["meta"]["bytes"]},
           "typed": {"tas": tas["meta"]["typed"], "control": ctl["meta"]["typed"]},
           "novelty": {"instruction_ngram_vs_seg072_excluding_s2": reference_novelty(gt_all - gc_all, gt_all, ref),
                       "cell_pattern_windows": {"tas_only": len(pt - pc_), "tas_only_absent_from_reference": len((pt - pc_) - refpat)},
                       "descriptive_only": True},
           "bootstrap_region_blocks": bootstrap_extra(ts, cs),
           "growth": {k: runs[k]["new_pcs_per_growth_window"] for k in ("tas_a", "ctl_a")}}
    res["decision"] = decide(ga, gb, gc, cpu_h)
    pathlib.Path(a.out).write_text(json.dumps(res, indent=1, sort_keys=True))
    print(json.dumps({"decision": res["decision"], "gate_a": {k: ga[k] for k in ("replay_ok", "tracing_ok", "progression")},
                      "gate_b_pass": gb["pass"], "gate_c": gc, "cpu_hours": res["cpu_hours"]}))
    return 0

# --------------------------------------------------------------------------------------------------------------------- plan I/O
def bound_digests() -> dict:
    root = HERE.parent
    return {f: ge.sha256((root / f).read_bytes()) for f in BOUND}


def cmd_freeze(a) -> int:
    if PLAN.exists() and not a.force:
        raise ge.EvidenceError("plan already frozen; a revision needs a recorded amendment")
    doc = dict(PROTOCOL)
    doc["bound_code"] = bound_digests()
    doc["inputs"] = json.loads(pathlib.Path(a.identity).read_text())
    data = ge.canonical(doc)
    PLAN.write_bytes(data)
    PLAN_DIGEST.write_text(ge.sha256(data) + "\n")
    print(json.dumps({"plan_sha256": ge.sha256(data)}))
    return 0


def verify_plan() -> dict:
    data = PLAN.read_bytes()
    if ge.sha256(data) != PLAN_DIGEST.read_text().strip():
        raise ge.EvidenceError("plan digest mismatch")
    doc = json.loads(data)
    if {k: v for k, v in doc.items() if k not in PLAN_EXTRA} != json.loads(json.dumps(PROTOCOL)):
        raise ge.EvidenceError("committed plan differs from the protocol in code")
    if doc["bound_code"] != bound_digests():
        raise ge.EvidenceError("bound code changed after the freeze")
    return doc


def cmd_verify_plan(a) -> int:
    print(json.dumps({"plan_sha256": ge.sha256(PLAN.read_bytes()), "ok": bool(verify_plan())}))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("identity")
    p.add_argument("--rom", required=True), p.add_argument("--movie", required=True), p.add_argument("--bizhawk", required=True)
    p.add_argument("--package"), p.add_argument("--out")
    p.set_defaults(fn=cmd_identity)
    p = sub.add_parser("run")
    p.add_argument("--rom", required=True), p.add_argument("--movie"), p.add_argument("--bizhawk", required=True)
    p.add_argument("--scenario", required=True, help='JSON {"id","mode":"movie|random|playback","frames":54000,"seed":1,"start_window":40}')
    p.add_argument("--out", required=True), p.add_argument("--jobs", required=True)
    p.add_argument("--user", default="bhuser"), p.add_argument("--display", default="99"), p.add_argument("--timeout", default="7200")
    p.set_defaults(fn=cmd_run)
    p = sub.add_parser("label")
    p.add_argument("--rom", required=True), p.add_argument("--cli", required=True), p.add_argument("--out", required=True)
    p.add_argument("--id", default="s2"), p.add_argument("--lineage", default="D01"), p.add_argument("runs", nargs="+")
    p.set_defaults(fn=cmd_label)
    p = sub.add_parser("evaluate")
    p.add_argument("--private", required=True), p.add_argument("--rom", required=True), p.add_argument("--truth", required=True)
    p.add_argument("--identity", required=True), p.add_argument("--registry", required=True), p.add_argument("--out", required=True)
    p.add_argument("--verification-cpu-hours", default="0")
    p.set_defaults(fn=cmd_evaluate)
    p = sub.add_parser("freeze")
    p.add_argument("--identity", required=True), p.add_argument("--force", action="store_true")
    p.set_defaults(fn=cmd_freeze)
    p = sub.add_parser("verify-plan")
    p.set_defaults(fn=cmd_verify_plan)
    a = ap.parse_args(argv)
    try:
        return a.fn(a)
    except ge.EvidenceError as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
