#!/usr/bin/env python3
"""SEG-077-T001: evidence-tiered I/c/D/U labels for an authorized Genesis cartridge image (OFFLINE research tooling).

The labels are a *partial* truth for the unchanged SEG-072/075 sample layout (`segarecomp_ml_v3_train.Image`): unknown stays unknown, and
U is never a negative. Each label needs evidence from outside the production decoder:

  E  executed      PC observed in an independent emulator's MC68000 instruction trace (MAME `trace`, CPU `:maincpu`), inside the
                   cartridge image, and decoded with the SAME length by two independent decoders (Oxore/m68k-disasm, traced mode, and
                   the CPU-owned base-68000 decoder token report). Disagreement, odd PCs and overlapping executed spans are typed and
                   masked, never labelled.
  R  static reach  an instruction reached from accepted code only through direct control flow:
                     * a direct Bcc/BRA/BSR/DBcc/JMP/JSR (abs.w, abs.l, d16(PC)) target;
                     * fall-through from a non-terminating instruction that is NOT a call/trap, or a call/trap whose return site was
                       itself executed (inline-argument calls never extend code);
                   and decoded with the same length by m68k-disasm's walk/follow mode and the CPU-owned decoder, with no overlap
                   with accepted code. m68k-disasm's own walk is a candidate source only: it decodes inline data after calls.
  D  data          hardware-defined vector table + cartridge header (0x000-0x1FF), and ROM bytes that were the source of an observed
                   68K->VDP DMA. The DMA is reconstructed from the trace-time VDP control-port write stream with the documented
                   register semantics. A DMA extent that overlaps accepted code is masked whole; a Ghidra code-entry candidate
                   (unchanged ADR-0025 recipe) inside data vetoes the following 10 bytes.
  U  unknown       everything else, including ROM bytes of code copied to RAM and executed there (no validated copy relation), bytes
                   that only bounded scenarios did not execute, and any contradiction.

Excluded evidence, with the reason recorded in the frozen protocol: CPU data-read taps/watchpoints (MAME reports opcode fetches and VDP
DMA reads through the same path, and checksum/copy loops read code), byte-exact reassembly (`dc.b` reproduces anything), decoder
agreement alone (validity, not intent), non-execution (never data), and any model score.

Subcommands:
  identity     raw/normalized image identity (plain, 512-byte copier header, SMD interleave), vectors, header checks
  trace        one deterministic scenario under MAME in the pinned offline container: deduplicated ROM/RAM PCs (the trace is
               streamed through a FIFO, never stored), reconstructed ROM DMA extents, and a ROM mapping check
  ghidra       the unchanged pinned Ghidra recipe (ExportCodeEntryCandidates) run directly in its image, offline
  label        union of scenario evidence -> sample directory (image ref, classes.bin, tokens.bin, seeds.json, relocs.json, spans.json,
               meta.json) + evidence.json
  report       sanitized aggregate report (no addresses, bytes or listings)
  synthetic    label the authored synthetic cartridge and score every label against its GNU as listing truth
  run-title    the frozen protocol for one title: every scenario x replica in parallel (resumable: a completed run directory is reused
               only when its trace.json is complete), Ghidra, the replica determinism check, then label from replica A
  freeze       write the pilot protocol (tools/segarecomp_genesis_evidence.t001.plan.json + .digest) binding code and fixture digests
  verify-plan  fail unless the committed plan, its digest and every bound file still agree

Private outputs must live under an ignored directory; durable output is aggregate-only.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
SCHEMA = "segarecomp.genesis_evidence.v1"
CELL_BYTES = 128
TOKEN_FIELDS = 8
EVIDENCE_IMAGE = "segarecomp-seg077/evidence:1"
GHIDRA_IMAGE = "segarecomp/ghidra-mcp-headless:6.0.0"
MAME_CPU = ":maincpu"
HEADER_END = 0x200
CART_SPACE = 0x400000
RAM_START = 0xE00000          # 64 KiB work RAM mirrored through 0xE00000-0xFFFFFF
GHIDRA_VETO_BYTES = 10        # longest MC68000 instruction
MAX_IMAGE = 0x400000          # m68k-disasm limit and linear cartridge space
EXTRACTION_VERSION = 1


class EvidenceError(Exception):
    pass


def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def s8(v: int) -> int:
    return v - 0x100 if v & 0x80 else v


def s16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def u16(img: bytes, off: int) -> int | None:
    if off < 0 or off + 2 > len(img):
        return None
    return (img[off] << 8) | img[off + 1]


def u32(img: bytes, off: int) -> int | None:
    hi, lo = u16(img, off), u16(img, off + 2)
    return None if hi is None or lo is None else (hi << 16) | lo


# ------------------------------------------------------------------------------------------------------------------------- identity
def _has_sega(img: bytes) -> bool:
    return len(img) >= 0x110 and b"SEGA" in img[0x100:0x110]


def deinterleave_smd(body: bytes) -> bytes:
    """SMD copier format: each 16 KiB block stores its odd bytes in the first 8 KiB and its even bytes in the second."""
    if len(body) % 0x4000:
        raise EvidenceError("SMD body is not a whole number of 16 KiB blocks")
    out = bytearray(len(body))
    for b in range(0, len(body), 0x4000):
        for i in range(0x2000):
            out[b + 2 * i + 1] = body[b + i]
            out[b + 2 * i] = body[b + 0x2000 + i]
    return bytes(out)


def normalize(raw: bytes) -> tuple[bytes, str]:
    """Return (linear cartridge image, format). Only unambiguous, header-verified forms are accepted."""
    if _has_sega(raw) and len(raw) % 0x200 == 0:
        return raw, "plain"
    if len(raw) % 0x4000 == 0x200:
        body = raw[0x200:]
        if _has_sega(body):
            return body, "copier512"
        if raw[8:10] == b"\xaa\xbb":
            smd = deinterleave_smd(body)
            if _has_sega(smd):
                return smd, "smd"
    raise EvidenceError("IDENTITY_UNRESOLVED: no unambiguous Genesis cartridge layout")


def identity(raw: bytes) -> dict:
    img, fmt = normalize(raw)
    if len(img) > MAX_IMAGE:
        raise EvidenceError("IMAGE_TOO_LARGE: banked/mapper cartridges are outside the linear-map protocol")
    if len(img) % 2:
        raise EvidenceError("ODD_IMAGE_SIZE")
    reset, ssp = u32(img, 4), u32(img, 0)
    rom_start, rom_end = u32(img, 0x1A0), u32(img, 0x1A4)
    return {"raw_size": len(raw), "raw_sha256": sha256(raw), "format": fmt, "size": len(img), "sha256": sha256(img),
            "reset": reset, "ssp": ssp, "reset_in_image": reset is not None and 0 <= reset < len(img) and reset % 2 == 0,
            "header_rom_start": rom_start, "header_rom_end": rom_end,
            "header_rom_end_matches": rom_end is not None and rom_end + 1 == len(img)}


# ---------------------------------------------------------------------------------------------------------------------------- VDP
class VdpDma:
    """68K->VDP DMA reconstruction from the control-port write stream (Sega Genesis VDP, mode 5).

    A control word 10xx_xxxx_xxxx_xxxx with no pending first half writes register (bits 12-8) <- (bits 7-0); any other word starts a
    two-word command whose second word completes it. Reads of the control port and data-port accesses cancel a pending first half. A
    command with CD5 (second word bit 7) set while register 1 bit 4 (M1) is set starts a DMA; register 0x17 bit 7 clear selects the
    68K-bus source (bits 6-0 of 0x17 : 0x16 : 0x15, a word address), length 0x14:0x13 words (0 = 0x10000). After the transfer the length
    registers read zero and the source advances by the length inside its 128 KiB window (register 0x17 unchanged)."""

    def __init__(self):
        self.regs = [0] * 32
        self.pending = None
        self.events: dict[tuple[int, int], int] = {}

    def cancel(self):
        self.pending = None

    def write(self, word: int):
        word &= 0xFFFF
        if self.pending is None:
            if word & 0xC000 == 0x8000:
                self.regs[(word >> 8) & 0x1F] = word & 0xFF
                return
            self.pending = word
            return
        second, self.pending = word, None
        if second & 0x80 and self.regs[1] & 0x10 and not self.regs[0x17] & 0x80:
            length = self.regs[0x13] | (self.regs[0x14] << 8) or 0x10000
            src_words = self.regs[0x15] | (self.regs[0x16] << 8) | ((self.regs[0x17] & 0x7F) << 16)
            key = (src_words * 2, length * 2)
            self.events[key] = self.events.get(key, 0) + 1
            low = ((self.regs[0x15] | (self.regs[0x16] << 8)) + length) & 0xFFFF
            self.regs[0x15], self.regs[0x16] = low & 0xFF, low >> 8
            self.regs[0x13] = self.regs[0x14] = 0

    def rom_extents(self, size: int) -> list[tuple[int, int]]:
        """Distinct ROM byte extents [start, end) of observed 68K-bus DMA sources inside the image."""
        out = set()
        for (src, nbytes) in self.events:
            if src >= CART_SPACE:
                continue
            # a DMA source crossing a 128 KiB boundary wraps on hardware; keep only the in-window part
            window_end = (src & ~0x1FFFF) + 0x20000
            end = min(src + nbytes, window_end, size)
            if src < end:
                out.add((src, end))
        return sorted(out)


def replay_vdp_log(lines) -> VdpDma:
    """Lines 'W <offset> <data> <mask>' / 'R <offset>' / 'D' (data-port access) in bus order."""
    v = VdpDma()
    for ln in lines:
        p = ln.split()
        if not p:
            continue
        if p[0] == "W":
            data, mask = int(p[2], 16), int(p[3], 16)
            if mask == 0xFFFF:
                v.write(data)
            elif mask in (0xFF00, 0x00FF):
                byte = (data >> 8) & 0xFF if mask == 0xFF00 else data & 0xFF
                v.write((byte << 8) | byte)
            else:
                raise EvidenceError(f"unexpected VDP write mask {mask:#x}")
        elif p[0] in ("R", "D"):
            v.cancel()
        else:
            raise EvidenceError(f"bad VDP log line: {ln!r}")
    return v


# -------------------------------------------------------------------------------------------------------------------------- trace
TRACE_LINE = re.compile(r"^([0-9A-F]{6}): (.*)$")


def parse_trace_first(lines) -> dict:
    """First-occurrence MAME trace lines ('PPPPPP: text'). Returns {'rom': {pc: text}, 'ram': n, 'other': n}."""
    rom, ram, other = {}, set(), set()
    for ln in lines:
        m = TRACE_LINE.match(ln.rstrip("\n"))
        if not m:
            continue
        pc = int(m.group(1), 16)
        if pc < CART_SPACE:
            rom.setdefault(pc, m.group(2).strip())
        elif pc >= RAM_START:
            ram.add(pc)
        else:
            other.add(pc)
    return {"rom": rom, "ram": sorted(ram), "other": len(other)}


def parse_reads(keys, size: int) -> dict[int, set[int]]:
    """Read-tap keys (current PC << 26 | word offset << 2 | lane: 0 word, 1 high byte, 2 low byte) -> {reader pc: byte addresses}."""
    out: dict[int, set[int]] = {}
    for k in keys:
        v = int(k, 16)
        pc, off, lane = v >> 26, (v >> 2) & 0xFFFFFF, v & 3
        if off >= size:
            continue
        lanes = (off, off + 1) if lane == 0 else (off,) if lane == 1 else (off + 1,)
        out.setdefault(pc, set()).update(b for b in lanes if b < size)
    return out


def ranges(sorted_addrs: list[int]) -> list[list[int]]:
    out = []
    for a in sorted_addrs:
        if out and out[-1][1] == a:
            out[-1][1] = a + 1
        else:
            out.append([a, a + 1])
    return out


def unranges(rs) -> set[int]:
    return {k for s0, e0 in rs for k in range(s0, e0)}


TRAMPOLINE_FIRST_WORDS = {0x4EF9, 0x4EB9, 0x4EF8, 0x4EB8}


def ram_copy_origin(ram_pcs: list[int], ram: bytes, img: bytes) -> dict:
    """Locate the ROM origin of code executed from work RAM. Executed RAM PCs are grouped into runs (gap <= 10 bytes); a run's bytes from
    the end-of-run RAM image (first PC to last PC + 2) must occur verbatim in the ROM image; every occurrence is a copy origin. (The
    data-read rule widens each origin to the whole contiguous read extent of every reader that read it: the copier's source block.) A single absolute JMP/JSR or BRA trampoline (<= 6 bytes, written by code) is exempt. Any other run without
    a ROM occurrence is unresolved, which disables data-read evidence for the whole image (fail closed)."""
    offs = sorted({pc & 0xFFFF for pc in ram_pcs})
    runs = []
    for o in offs:
        if runs and o - runs[-1][1] <= 10:
            runs[-1][1] = o
        else:
            runs.append([o, o])
    zones, unresolved, trampolines, located = [], 0, 0, 0
    for a, b in runs:
        chunk = ram[a:b + 2]
        first = (chunk[0] << 8) | chunk[1] if len(chunk) >= 2 else None
        if b - a <= 4 and first is not None and (first in TRAMPOLINE_FIRST_WORDS or first & 0xFF00 == 0x6000):
            trampolines += 1
            continue
        if len(chunk) < 4:
            chunk = ram[a:a + 4]
        hits, i = [], img.find(chunk)
        while i >= 0:
            hits.append(i)
            i = img.find(chunk, i + 1)
        if not hits:
            unresolved += 1
            continue
        located += 1
        zones += [[h, h + len(chunk)] for h in hits]
    return {"runs": len(runs), "trampolines": trampolines, "located": located, "unresolved": unresolved, "zones": sorted(zones)}


PREFETCH_SHADOW = 4


def data_reads(code: dict, readers: dict[int, set[int]], copy_zones: list, size: int, typed: dict) -> set[int]:
    """DR bytes: observed data reads (see module doc). `code` = accepted {pc: (len, tier)}; readers = union of scenarios."""
    def bump(k, n=1):
        typed[k] = typed.get(k, 0) + n
    code_bytes = bytearray(size)
    shadow = bytearray(size)
    for pc, (n, tier) in code.items():
        for k in range(pc, pc + n):
            code_bytes[k] = 1
        if tier == "E":
            for k in range(pc + n, min(size, pc + n + PREFETCH_SHADOW)):
                shadow[k] = 1
    excluded = bytearray(size)
    for s0, e0 in copy_zones:
        for k in range(s0, min(e0, size)):
            excluded[k] = 1
    origin = bytes(excluded)
    for addrs in readers.values():                 # widen each copy origin to its copier's contiguous source extent
        for s0, e0 in ranges(sorted(addrs)):
            if any(origin[s0:e0]):
                for k in range(s0, e0):
                    excluded[k] = 1
    out = set()
    for pc, addrs in sorted(readers.items()):
        if pc < CART_SPACE:
            if pc not in code or code[pc][1] != "E":
                bump("DR_READER_NOT_ACCEPTED")
                continue
            lo, hi = pc, pc + code[pc][0] + PREFETCH_SHADOW
        else:
            lo = hi = -1
        own = [a for a in addrs if not lo <= a < hi]
        if any(code_bytes[a] for a in own):
            bump("DR_CODE_READER")
            continue
        for a in own:
            if shadow[a]:
                bump("DR_PREFETCH_SHADOW")
            elif excluded[a]:
                bump("DR_COPY_ZONE")
            else:
                out.add(a)
    return out


LUA = r"""
local S = {seconds = __SECONDS__, policy = "__POLICY__", seed = __SEED__, start_window = __START_WINDOW__, size = __SIZE__}
local m = manager.machine
local dbg = m.debugger
if dbg == nil then print("SEG077_NO_DEBUGGER") m:exit() return end
local cpu = m.devices[":maincpu"]
local sp = cpu.spaces["program"]
local vdp = io.open("/out/vdp.log", "w")
local function vw(offset, data, mask)
  local r = offset & 0x1F
  if r >= 4 and r < 8 then vdp:write(string.format("W %x %x %x\n", r, data, mask))
  elseif r < 4 then vdp:write("D\n") end
end
local function vr(offset, data, mask)
  local r = offset & 0x1F
  if r >= 4 and r < 8 then vdp:write(string.format("R %x\n", r))
  elseif r < 4 then vdp:write("D\n") end
end
local cur = cpu.state["CURPC"]
local rd = {}
_G.seg077_rdtap = sp:install_read_tap(0x000000, S.size - 1, "seg077rd", function(offset, data, mask)
  local lane = 0
  if mask == 0xff00 then lane = 1 elseif mask == 0x00ff then lane = 2 end
  rd[(cur.value << 26) | (offset << 2) | lane] = true
end)
_G.seg077_wtap = sp:install_write_tap(0xC00000, 0xDFFFFF, "seg077w", vw)
_G.seg077_rtap = sp:install_read_tap(0xC00000, 0xDFFFFF, "seg077r", vr)
local pad = m.ioport.ports[":ctrl1:mdpad:PAD"]
local names = {"P1 Up", "P1 Down", "P1 Left", "P1 Right", "P1 A", "P1 B", "P1 C", "P1 Start"}
local F = {}
for _, n in ipairs(names) do F[n] = pad.fields[n] if F[n] == nil then print("SEG077_NO_FIELD " .. n) m:exit() return end end
local state = S.seed * 2654435761 % 2147483648 + 1
local function rnd(k) state = (state * 1103515245 + 12345) % 2147483648 return state % k end
local frame = 0
local held = {}
local function set(n, v) if held[n] ~= v then F[n]:set_value(v) held[n] = v end end
local function policy()
  if S.policy == "none" then return {} end
  local p = {}
  if frame < S.start_window * 60 then
    if frame % 120 < 6 then p["P1 Start"] = 1 end
    if frame % 120 >= 60 and frame % 120 < 66 then p["P1 A"] = 1 end
    return p
  end
  local f = frame % 900
  if f < 4 or (f >= 24 and f < 28) then p["P1 Start"] = 1 return p end
  if frame % 8 == 0 or _G.seg077_act == nil then
    local d = rnd(100)
    local a = {}
    if d < 50 then a["P1 Right"] = 1 elseif d < 65 then a["P1 Left"] = 1 elseif d < 75 then a["P1 Up"] = 1 elseif d < 85 then a["P1 Down"] = 1 end
    if rnd(100) < 30 then a["P1 A"] = 1 end
    if rnd(100) < 30 then a["P1 B"] = 1 end
    if rnd(100) < 30 then a["P1 C"] = 1 end
    _G.seg077_act = a
  end
  return _G.seg077_act
end
local function on_frame()
  frame = frame + 1
  local p = policy()
  for _, n in ipairs(names) do set(n, p[n] or 0) end
end
if emu.add_machine_frame_notifier then _G.seg077_frame = emu.add_machine_frame_notifier(on_frame)
else emu.register_frame_done(on_frame) end
local function on_stop()
  _G.seg077_rdtap:remove()
  vdp:close()
  local r = io.open("/out/reads.txt", "w")
  for k in pairs(rd) do r:write(string.format("%x\n", k)) end
  r:close()
  local ram = {}
  for a = 0xFF0000, 0xFFFFFF do ram[#ram + 1] = string.char(sp:read_u8(a)) end
  local rf = io.open("/out/ram.bin", "wb") rf:write(table.concat(ram)) rf:close()
  local mis, words = 0, 0
  local f = io.open("/in/rom.bin", "rb")
  local rom = f:read("a") f:close()
  for a = 0, S.size - 2, 2 do
    local w = (rom:byte(a + 1) << 8) | rom:byte(a + 2)
    if sp:read_u16(a) ~= w then mis = mis + 1 end
    words = words + 1
  end
  local o = io.open("/out/run.txt", "w")
  o:write(string.format("frames %d\nmap_words %d\nmap_mismatch %d\n", frame, words, mis))
  o:close()
end
_G.seg077_stop = emu.add_machine_stop_notifier(on_stop)
dbg:command("trace /tmp/trace.fifo,maincpu,noloop")
dbg:command("go")
"""

TRACE_SH = r"""set -e
cd /tmp
mkfifo /tmp/trace.fifo
awk '!s[substr($0,1,6)]++' /tmp/trace.fifo > /out/pcs_first.txt &
AWK=$!
/usr/games/mame genesis -cart /in/rom.bin -debug -debugger none -autoboot_script /in/scenario.lua -video none -sound none -nothrottle \
  -seconds_to_run "$SECONDS_TO_RUN" -skip_gameinfo -homepath /tmp -nvram_directory /tmp -cfg_directory /tmp -snapshot_directory /tmp \
  -state_directory /tmp -diff_directory /tmp -input_directory /tmp > /out/mame.txt 2>&1
wait $AWK
"""


def scenario_lua(sc: dict, size: int) -> str:
    sub = {"__SECONDS__": str(int(sc["seconds"])), "__POLICY__": sc["policy"], "__SEED__": str(int(sc.get("seed", 0))),
           "__START_WINDOW__": str(int(sc.get("start_window", 0))), "__SIZE__": str(size)}
    if sc["policy"] not in ("none", "random"):
        raise EvidenceError(f"unknown scenario policy {sc['policy']}")
    out = LUA
    for k, v in sub.items():
        out = out.replace(k, v)
    return out


def docker_image_id(image: str) -> str:
    r = subprocess.run(["docker", "image", "inspect", image, "--format", "{{.Id}}"], capture_output=True, text=True)
    if r.returncode:
        raise EvidenceError(f"image {image} not built")
    return r.stdout.strip()


def require_private(path: pathlib.Path) -> None:
    """Private outputs only under a git-ignored directory (fail closed)."""
    p = path.resolve()
    probe = p / ".seg077-probe"
    for parent in [p] + list(p.parents):
        if (parent / ".git").exists():
            r = subprocess.run(["git", "-C", str(parent), "check-ignore", "-q", str(probe)], capture_output=True)
            if r.returncode != 0:
                raise EvidenceError(f"private output {p} is not git-ignored")
            return
    raise EvidenceError(f"private output {p} is not inside a git checkout (cannot prove it is ignored)")


def stage_image(rom: pathlib.Path, work: pathlib.Path) -> tuple[pathlib.Path, dict]:
    """Normalized image for the containers; a plain image is mounted directly (never copied)."""
    raw = rom.read_bytes()
    ident = identity(raw)
    if ident["format"] == "plain":
        return rom.resolve(), ident
    img, _ = normalize(raw)
    p = work / "normalized.bin"
    p.write_bytes(img)
    return p, ident


def cmd_trace(a) -> int:
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=False)
    require_private(out)
    out.chmod(0o777)
    inp = out / "in"
    inp.mkdir()
    img_path, ident = stage_image(pathlib.Path(a.rom), inp)
    sc = json.loads(a.scenario)
    (inp / "scenario.lua").write_text(scenario_lua(sc, ident["size"]))
    (inp / "trace.sh").write_text(TRACE_SH)
    for p in (inp, inp / "scenario.lua", inp / "trace.sh"):
        p.chmod(0o755 if p.is_dir() else 0o644)
    t0, c0 = time.time(), os.times()
    cmd = ["docker", "run", "--rm", "--network", "none", "--user", "1000:1000", "--cpus", "1", "--memory", "2g",
           "-e", f"SECONDS_TO_RUN={int(sc['seconds'])}",
           "-v", f"{img_path}:/in/rom.bin:ro", "-v", f"{inp / 'scenario.lua'}:/in/scenario.lua:ro", "-v", f"{inp / 'trace.sh'}:/in/trace.sh:ro",
           "-v", f"{out}:/out", EVIDENCE_IMAGE, "sh", "/in/trace.sh"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=int(a.timeout))
    wall = time.time() - t0
    if r.returncode:
        raise EvidenceError(f"trace container failed: {r.stderr[-400:]}")
    run = dict(ln.split() for ln in (out / "run.txt").read_text().splitlines() if ln.strip())
    first = parse_trace_first((out / "pcs_first.txt").read_text(errors="replace").splitlines())
    with open(out / "vdp.log") as f:
        vdp = replay_vdp_log(f)
    ext = vdp.rom_extents(ident["size"])
    rom_pcs = sorted(first["rom"])
    reads = parse_reads((out / "reads.txt").read_text().split(), ident["size"])
    copy = ram_copy_origin(first["ram"], (out / "ram.bin").read_bytes(), img_path.read_bytes())
    dma_bytes = {k for s0, e0 in ext for k in range(s0, e0)}
    read_bytes = set()
    for addrs in reads.values():
        read_bytes |= addrs
    pcs_digest = sha256(json.dumps(rom_pcs).encode())
    dma_digest = sha256(json.dumps(sorted(vdp.events.items())).encode())
    reads_digest = sha256(json.dumps(sorted((pc, sorted(a)) for pc, a in reads.items())).encode())
    res = {"schema": SCHEMA + ".trace", "scenario": sc, "image": ident, "emulator": {"image": EVIDENCE_IMAGE, "image_id": docker_image_id(EVIDENCE_IMAGE),
           "machine": "genesis", "cpu": MAME_CPU}, "extraction_version": EXTRACTION_VERSION,
           "frames": int(run["frames"]), "map_words": int(run["map_words"]), "map_mismatch": int(run["map_mismatch"]),
           "rom_pcs": len(rom_pcs), "rom_pcs_beyond_image": sum(1 for p in rom_pcs if p >= ident["size"]),
           "ram_pcs": len(first["ram"]), "other_pcs": first["other"], "readers": len(reads),
           "read_bytes": len(read_bytes), "dma_bytes_seen_by_read_tap": len(dma_bytes & read_bytes), "ram_copy": {k: v for k, v in copy.items() if k != "zones"}, "dma_events": sum(vdp.events.values()), "dma_distinct": len(vdp.events),
           "dma_rom_extents": len(ext), "dma_rom_bytes": sum(e - s for s, e in ext), "pcs_sha256": pcs_digest, "dma_sha256": dma_digest, "reads_sha256": reads_digest,
           "wall_seconds": round(wall, 1)}
    (out / "pcs.json").write_text(json.dumps({"rom": {str(k): v for k, v in sorted(first["rom"].items())}, "ram": first["ram"]}))
    (out / "reads.json").write_text(json.dumps({str(pc): ranges(sorted(a)) for pc, a in sorted(reads.items())}))
    (out / "copy.json").write_text(json.dumps(copy))
    (out / "reads.txt").unlink()
    (out / "ram.bin").unlink()
    (out / "dma.json").write_text(json.dumps({"events": [[s, n, c] for (s, n), c in sorted(vdp.events.items())], "rom_extents": ext}))
    (out / "trace.json").write_text(json.dumps(res, indent=1, sort_keys=True))
    (out / "vdp.log").unlink()            # raw bus log is not retained; the reconstructed events are
    (out / "pcs_first.txt").unlink()
    print(json.dumps({k: res[k] for k in ("frames", "map_mismatch", "rom_pcs", "ram_pcs", "dma_rom_extents", "dma_rom_bytes", "readers",
                                          "read_bytes", "dma_bytes_seen_by_read_tap", "ram_copy",
                                          "pcs_sha256", "dma_sha256", "reads_sha256", "wall_seconds")}))
    return 0


# ------------------------------------------------------------------------------------------------------------------------- decode
DIS_LINE = re.compile(r"^\t(\S[^|]*?)\s*\| ((?:[0-9a-f]{4} ?)+)@([0-9a-f]{8})\s*$")


def parse_disasm(text: str) -> dict[int, tuple[int, str]]:
    """m68k-disasm `-frdc` output -> {address: (length, mnemonic)} for decoded instructions (raw `.short` data lines are ignored)."""
    out = {}
    for ln in text.splitlines():
        m = DIS_LINE.match(ln)
        if not m:
            continue
        mnem = m.group(1).split()[0]
        if mnem.startswith("."):
            continue
        out[int(m.group(3), 16)] = (len(m.group(2).split()) * 2, mnem)
    return out


def run_disasm(img_path: pathlib.Path, pcs: list[int], walk: bool, work: pathlib.Path) -> dict[int, tuple[int, str]]:
    tf = work / ("pcs_walk.txt" if walk else "pcs_traced.txt")
    tf.write_text("".join(f"{p}\n" for p in pcs))
    tf.chmod(0o644)
    flags = ["-frdc"] + (["-fwalk", "-ffollow-jumps"] if walk else [])
    cmd = ["docker", "run", "--rm", "--network", "none", "--user", "1000:1000", "-v", f"{img_path}:/in/rom.bin:ro", "-v", f"{tf}:/in/pcs.txt:ro",
           EVIDENCE_IMAGE, "/opt/m68k-disasm/build/m68k-disasm", *flags, "-t", "/in/pcs.txt", "/in/rom.bin"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
    if r.returncode:
        raise EvidenceError(f"m68k-disasm failed: {r.stderr[-300:]}")
    return parse_disasm(r.stdout)


def tokens_for(cli: str, img: bytes, work: pathlib.Path) -> bytes:
    sys.path.insert(0, str(HERE))
    import segarecomp_ml_region_gen2 as gen2
    p, rep = work / "tok.bin", work / "tok.report"
    p.write_bytes(img)
    subprocess.run([cli, "m68k-decode-token-report", "--rom", str(p), "--output", str(rep)], check=True, capture_output=True, timeout=1800)
    body = gen2.parse_token_report(rep.read_bytes(), len(img))
    p.unlink()
    rep.unlink()
    return body[:(len(img) // 2) * TOKEN_FIELDS]


def token_len(tokens: bytes, pc: int) -> int | None:
    """Byte length of an ordinary base-68000 decode at an even pc, else None (status 2 = ordinary)."""
    w = pc // 2
    if pc & 1 or (w + 1) * TOKEN_FIELDS > len(tokens):
        return None
    status, length = tokens[w * TOKEN_FIELDS], tokens[w * TOKEN_FIELDS + 1]
    return length * 2 if status == 2 and length else None


# --------------------------------------------------------------------------------------------------------------------- direct flow
def flow(img: bytes, pc: int, length: int) -> tuple[list[int], str]:
    """(direct targets, kind) of the instruction at pc. kind: 'term' (no fall-through), 'call' (fall-through only when its return
    site is executed), 'normal'. Dynamic JMP/JSR targets are not followed."""
    w = u16(img, pc)
    if w is None:
        return [], "term"
    if w & 0xF000 == 0x6000:
        cond, d8 = (w >> 8) & 0xF, w & 0xFF
        disp = s16(u16(img, pc + 2)) if d8 == 0 else s8(d8)
        tgt = [pc + 2 + disp]
        return tgt, ("term" if cond == 0 else "call" if cond == 1 else "normal")
    if w & 0xF0F8 == 0x50C8:
        return [pc + 2 + s16(u16(img, pc + 2))], "normal"
    if w & 0xFF80 == 0x4E80:                       # JSR (0x4E80) / JMP (0x4EC0)
        is_jmp = bool(w & 0x40)
        ea = w & 0x3F
        tgt = []
        if ea == 0x38:
            tgt = [s16(u16(img, pc + 2)) & 0xFFFFFFFF]
        elif ea == 0x39:
            tgt = [u32(img, pc + 2)]
        elif ea == 0x3A:
            tgt = [pc + 2 + s16(u16(img, pc + 2))]
        return tgt, ("term" if is_jmp else "call")
    if w in (0x4E75, 0x4E73, 0x4E77, 0x4AFC, 0x4E72):  # RTS RTE RTR ILLEGAL STOP
        return [], "term"
    if w & 0xFFF0 == 0x4E40:                       # TRAP #n
        return [], "call"
    if w & 0xF000 in (0xA000, 0xF000):             # line A / line F emulator traps
        return [], "term"
    return [], "normal"


def build_code(img: bytes, executed: list[int], traced: dict, walk: dict, tokens: bytes, typed: dict) -> dict[int, tuple[int, str]]:
    """Accepted instructions {pc: (length, tier)} with tier E (executed) or R (direct-flow static reach)."""
    def bump(k):
        typed[k] = typed.get(k, 0) + 1
    size = len(img)
    cand = {}
    for pc in executed:
        if pc & 1:
            bump("E_ODD_PC")
            continue
        if pc >= size:
            bump("E_BEYOND_IMAGE")
            continue
        # m68k-disasm's traced mode sometimes prints a traced location as a raw `.short`; its walk mode decodes the same address as
        # an instruction. The decode at that exact address is used either way.
        t = walk.get(pc) or traced.get(pc)
        tl = token_len(tokens, pc)
        if t is None:
            bump("E_DISASM_NO_DECODE")
            continue
        if tl is None:
            bump("E_CPU_DECODER_REJECTED")
            continue
        if t[0] != tl:
            bump("E_LENGTH_DISAGREE")
            continue
        if pc + tl > size:
            bump("E_SPAN_BEYOND_IMAGE")
            continue
        cand[pc] = tl
    owner = {}
    bad = set()
    for pc in sorted(cand):
        for k in range(pc, pc + cand[pc]):
            if k in owner and owner[k] != pc:
                bad.add(pc)
                bad.add(owner[k])
            owner.setdefault(k, pc)
    for pc in bad:
        bump("E_OVERLAPPING_EXECUTED")
    acc = {pc: (n, "E") for pc, n in cand.items() if pc not in bad}
    taken = bytearray(size)
    for pc, (n, _) in acc.items():
        for k in range(pc, pc + n):
            taken[k] = 1
    executed_set = set(acc)
    queue = sorted(acc)
    seen = set(queue)
    while queue:
        nxt = []
        for pc in queue:
            n = acc[pc][0]
            tgts, kind = flow(img, pc, n)
            succ = list(tgts)
            if kind == "normal" or (kind == "call" and pc + n in executed_set):
                succ.append(pc + n)
            for s in succ:
                if s in acc or s in seen:
                    continue
                seen.add(s)
                if s & 1 or s < HEADER_END or s >= size:
                    bump("R_TARGET_OUTSIDE")
                    continue
                w = walk.get(s)
                tl = token_len(tokens, s)
                if w is None:
                    bump("R_DISASM_NO_DECODE")
                    continue
                if tl is None or w[0] != tl:
                    bump("R_DECODE_DISAGREE")
                    continue
                if s + tl > size or any(taken[k] for k in range(s, s + tl)):
                    bump("R_OVERLAP")
                    continue
                for k in range(s, s + tl):
                    taken[k] = 1
                acc[s] = (tl, "R")
                nxt.append(s)
        queue = sorted(nxt)
    return acc


# --------------------------------------------------------------------------------------------------------------------------- labels
def assemble(img: bytes, code: dict, dma_extents: list, ghidra: list[int], typed: dict, dr: set[int] | None = None) -> tuple[bytearray, list]:
    def bump(k, n=1):
        typed[k] = typed.get(k, 0) + n
    size = len(img)
    cls = bytearray(b"U" * size)
    for pc, (n, _) in code.items():
        cls[pc] = ord("I")
        for k in range(pc + 1, pc + n):
            cls[k] = ord("c")
    extents = []
    hw = (0, min(HEADER_END, size))
    for kind, (s, e) in [("HW_HEADER", hw)] + [("DMA", x) for x in dma_extents]:
        if any(cls[k] in b"Ic" for k in range(s, e)):
            bump(f"{kind}_CODE_CONFLICT")
            extents.append({"kind": kind, "start": s, "end": e, "masked": True})
            continue
        for k in range(s, e):
            cls[k] = ord("D")
        extents.append({"kind": kind, "start": s, "end": e, "masked": False})
    dr_new = 0
    for a in sorted(dr or ()):
        if cls[a] == ord("U"):
            cls[a] = ord("D")
            dr_new += 1
    typed["DR_BYTES_ADDED"] = dr_new
    for a in ghidra:
        if 0 <= a < size and cls[a] == ord("D"):
            bump("GHIDRA_CODE_IN_DATA")
            for k in range(a, min(a + GHIDRA_VETO_BYTES, size)):
                if cls[k] == ord("D"):
                    cls[k] = ord("U")
    return cls, extents


def cell_stats(cls: bytes, img: bytes, tokens: bytes) -> dict:
    n = (len(cls) + CELL_BYTES - 1) // CELL_BYTES
    pos = neg = masked = mixed = hard = 0
    labels = []
    for c in range(n):
        ch = cls[c * CELL_BYTES:(c + 1) * CELL_BYTES]
        if b"I" in ch:
            labels.append(1)
            pos += 1
            if b"D" in ch:
                mixed += 1
        elif ch.count(b"D") == len(ch):
            labels.append(0)
            neg += 1
        else:
            labels.append(-1)
            masked += 1
    for c, y in enumerate(labels):
        if y != 0:
            continue
        near = any(0 <= c + d < n and labels[c + d] == 1 for d in (-1, 1))
        ords = sum(1 for k in range(c * CELL_BYTES, min((c + 1) * CELL_BYTES, len(img)), 2) if token_len(tokens, k))
        if near or ords * 2 * 2 >= CELL_BYTES:      # adjacent to code, or >= half its words decode as ordinary instructions
            hard += 1
    return {"cells": n, "positive": pos, "negative": neg, "masked": masked, "mixed_positive": mixed, "hard_negative": hard}


def load_scenarios(runs: list[pathlib.Path], size: int, sha: str):
    pcs, ext, info = set(), set(), []
    readers: dict[int, set[int]] = {}
    zones, unresolved = [], 0
    for r in runs:
        t = json.loads((r / "trace.json").read_text())
        if t["image"]["sha256"] != sha:
            raise EvidenceError(f"{r}: trace is for a different image")
        if t["map_mismatch"]:
            raise EvidenceError(f"{r}: ROM mapping mismatch ({t['map_mismatch']} words)")
        p = [int(k) for k in json.loads((r / "pcs.json").read_text())["rom"]]
        pcs.update(p)
        ext.update(tuple(x) for x in json.loads((r / "dma.json").read_text())["rom_extents"])
        for pc, rs in json.loads((r / "reads.json").read_text()).items():
            readers.setdefault(int(pc), set()).update(unranges(rs))
        cp = json.loads((r / "copy.json").read_text())
        zones += cp["zones"]
        unresolved += cp["unresolved"]
        info.append({k: t[k] for k in ("scenario", "frames", "rom_pcs", "ram_pcs", "dma_rom_extents", "pcs_sha256", "dma_sha256", "wall_seconds")})
    return sorted(pcs), sorted(ext), info, readers, zones, unresolved


def label(rom: pathlib.Path, runs: list[pathlib.Path], ghidra_json: pathlib.Path | None, cli: str, out: pathlib.Path, lineage: str,
          sid: str) -> dict:
    require_private(out)
    plan_digest = verify_plan() if PLAN.exists() else None
    out.mkdir(parents=True, exist_ok=False)
    work = out / ".work"
    work.mkdir()
    work.chmod(0o755)
    img_path, ident = stage_image(rom, work)
    img = img_path.read_bytes()
    t0 = time.time()
    executed, dma_ext, info, readers, zones, unresolved = load_scenarios(runs, len(img), ident["sha256"])
    traced = run_disasm(img_path, executed, False, work)
    walk = run_disasm(img_path, executed, True, work)
    tokens = tokens_for(cli, img, work)
    ghidra = []
    if ghidra_json is not None:
        g = json.loads(ghidra_json.read_text())
        ghidra = sorted(int(x["address"]) for x in g if x.get("kind") == "code_entry_candidate")
    typed: dict = {}
    code = build_code(img, executed, traced, walk, tokens, typed)
    if unresolved:
        typed["DR_DISABLED_RAM_CODE_ORIGIN_UNRESOLVED"] = unresolved
        dr = set()
    else:
        dr = data_reads(code, readers, zones, len(img), typed)
    cls, extents = assemble(img, code, dma_ext, ghidra, typed, dr)
    if ident["format"] == "plain":
        image_ref = str(img_path)
    else:
        keep = out / "image.bin"
        shutil.move(str(img_path), keep)
        image_ref = str(keep.resolve())
    (out / "classes.bin").write_bytes(bytes(cls))
    (out / "tokens.bin").write_bytes(tokens)
    reset = ident["reset"]
    seeds = sorted({reset // CELL_BYTES}) if ident["reset_in_image"] and cls[reset] == ord("I") else []
    (out / "seeds.json").write_text(json.dumps(seeds))
    (out / "relocs.json").write_text("[]")
    tiers = {"E": sorted(pc for pc, (_, t) in code.items() if t == "E"), "R": sorted(pc for pc, (_, t) in code.items() if t == "R")}
    (out / "spans.json").write_text(json.dumps({"tiers": tiers, "data_extents": extents, "typed": dict(sorted(typed.items()))}, sort_keys=True))
    counts = {c: cls.count(c.encode()) for c in "IcDU"}
    sys.path.insert(0, str(HERE))
    import segarecomp_ml_v3_data as v3d
    stats = cell_stats(cls, img, tokens)
    ghidra_in_exec = sum(1 for a in ghidra if a in code)
    meta = {"schema": "segarecomp.xplat_corpus.sample.v1", "evidence_schema": SCHEMA, "id": sid, "kind": "genesis_commercial", "lineage": lineage,
            "candidate": lineage, "platform": "genesis", "cpu_variant": "MC68000", "provenance": "commercial-evidence",
            "truth": "partial: E executed (MAME trace, two-decoder length agreement) + R direct-flow static reach + D hardware header / "
                     "observed ROM DMA sources; U elsewhere (never negative)", "completeness": "partial",
            "image": image_ref, "sha256": ident["sha256"], "size": len(img), "identity": ident,
            "classes_sha256": sha256(bytes(cls)), "tokens_sha256": sha256(tokens), "seeds_sha256": sha256(json.dumps(seeds).encode()),
            "seed_cells": len(seeds), "seeds_source": "reset vector, only where labelled an instruction start",
            "relocations": 0, "audit": v3d.audit(bytes(cls), tokens), "bytes": counts, "base_address": 0,
            "scenarios": info, "typed": dict(sorted(typed.items())), "cells": stats,
            "tiers": {"E": len(tiers["E"]), "R": len(tiers["R"])},
            "decoders": {"m68k_disasm_traced": len(traced), "m68k_disasm_walk": len(walk), "executed_rom_pcs": len(executed)},
            "ghidra": {"candidates": len(ghidra), "in_accepted_code_starts": ghidra_in_exec},
            "tools": {"evidence_image_id": docker_image_id(EVIDENCE_IMAGE), "cli_sha256": sha256(pathlib.Path(cli).read_bytes()),
                      "extraction_version": EXTRACTION_VERSION, "plan_sha256": plan_digest},
            "label_seconds": round(time.time() - t0, 1)}
    (out / "meta.json").write_text(json.dumps(meta, indent=1, sort_keys=True))
    shutil.rmtree(work)
    return meta


def cmd_label(a) -> int:
    meta = label(pathlib.Path(a.rom), [pathlib.Path(r) for r in a.runs], pathlib.Path(a.ghidra) if a.ghidra else None, a.cli,
                 pathlib.Path(a.out), a.lineage, a.id)
    print(json.dumps(sanitized(meta), sort_keys=True))
    return 0


def sanitized(meta: dict) -> dict:
    """Aggregate-only view (no addresses, bytes, paths or listings)."""
    return {"id": meta["id"], "lineage": meta["lineage"], "sha256": meta["sha256"], "size": meta["size"], "format": meta["identity"]["format"],
            "bytes": meta["bytes"], "cells": meta["cells"], "tiers": meta["tiers"], "typed": meta["typed"], "audit": meta["audit"],
            "decoders": meta["decoders"], "ghidra": meta["ghidra"], "seed_cells": meta["seed_cells"],
            "scenarios": [{"id": s["scenario"]["id"], "frames": s["frames"], "rom_pcs": s["rom_pcs"], "ram_pcs": s["ram_pcs"],
                           "dma_rom_extents": s["dma_rom_extents"], "wall_seconds": s["wall_seconds"]} for s in meta["scenarios"]],
            "label_seconds": meta["label_seconds"]}


# --------------------------------------------------------------------------------------------------------------------------- ghidra
def cmd_ghidra(a) -> int:
    out = pathlib.Path(a.out)
    require_private(out.parent)
    work = out.parent / (out.name + ".work")
    work.mkdir(parents=True, exist_ok=False)
    (work / "in").mkdir()
    (work / "out").mkdir()
    img_path, ident = stage_image(pathlib.Path(a.rom), work / "in")
    rom_in = work / "in" / "rom.bin"
    if img_path != rom_in:
        shutil.copyfile(img_path, rom_in)       # the Ghidra container user must be able to read it; removed below
    rom_in.chmod(0o644)
    (work / "in").chmod(0o755)
    (work / "out").chmod(0o777)
    scripts = HERE / "ghidra" / "scripts"
    sha = ident["sha256"]
    t0 = time.time()
    cmd = ["docker", "run", "--rm", "--network", "none", "-v", f"{work / 'in'}:/inputs:ro", "-v", f"{scripts}:/scripts:ro",
           "-v", f"{work / 'out'}:/analysis-out", "--entrypoint", "/opt/ghidra/support/analyzeHeadless", GHIDRA_IMAGE, "/tmp", "seg077",
           "-import", "/inputs/rom.bin", "-processor", "68000:BE:32:default", "-loader", "BinaryLoader", "-loader-baseAddr", "0x0",
           "-max-cpu", "1", "-analysisTimeoutPerFile", "1200", "-scriptPath", "/scripts", "-preScript", "PinAnalysisRecipe.java",
           "-postScript", "ExportCodeEntryCandidates", sha, "/analysis-out/candidates.json", "-deleteProject"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
    produced = work / "out" / "candidates.json"
    if r.returncode or not produced.is_file():
        raise EvidenceError(f"ghidra failed: {r.stdout[-300:]} {r.stderr[-300:]}")
    sys.path.insert(0, str(HERE))
    import ghidra as gh
    canonical = gh.canonicalize_candidates(produced.read_text(), sha)
    out.write_text(canonical)
    shutil.rmtree(work)
    n = len(json.loads(canonical))
    print(json.dumps({"sha256": sha, "candidates": n, "wall_seconds": round(time.time() - t0, 1), "image_id": docker_image_id(GHIDRA_IMAGE)}))
    return 0


# ------------------------------------------------------------------------------------------------------------------------ identity
def cmd_identity(a) -> int:
    print(json.dumps(identity(pathlib.Path(a.rom).read_bytes()), sort_keys=True))
    return 0


def cmd_report(a) -> int:
    rows = [sanitized(json.loads((pathlib.Path(d) / "meta.json").read_text())) for d in a.samples]
    print(json.dumps(rows, indent=1, sort_keys=True))
    return 0


# ----------------------------------------------------------------------------------------------------------------------- synthetic
def listing_truth(listing: str, obj: bytes, img: bytes) -> bytearray:
    """Complete truth of the authored fixture: its GNU as listing through the unchanged SEG-075 labeller, relocated fields masked by
    the object's relocations (the fixture links .text at address 0, so section offsets are image offsets)."""
    sys.path.insert(0, str(HERE))
    import segarecomp_xplat_formats as fm
    import segarecomp_xplat_corpus as xc
    _, rows = fm.parse_gas_listing(listing)
    rows = [r for r in rows if r.section == ".text"]
    text = [s for s in fm.parse_elf32(obj)["sections"] if s["name"] == ".text"][0]
    typed = xc.Typed()
    res = xc.section_labels(rows, img, xc.reloc_mask(len(img), text["relocs"]), "gas", typed, ".text")
    if res is None:
        raise EvidenceError(f"synthetic listing does not reproduce the image: {typed.counts}")
    return res[0]


def score(pred: bytes, truth: bytes) -> dict:
    """Soundness of a partial labelling against complete truth: every I, c and D must agree exactly; U is abstention."""
    wrong = {"I": 0, "c": 0, "D": 0}
    labelled = {"I": 0, "c": 0, "D": 0}
    for p, t in zip(pred, truth):
        ch = chr(p)
        if ch in wrong:
            labelled[ch] += 1
            if p != t:
                wrong[ch] += 1
    starts_true = truth.count(b"I")
    return {"labelled": labelled, "wrong": wrong, "true_starts": starts_true, "recalled_starts": sum(1 for p, t in zip(pred, truth)
                                                                                                     if p == t == ord("I")),
            "true_data": truth.count(b"D"), "recalled_data": sum(1 for p, t in zip(pred, truth) if p == t == ord("D"))}


def cmd_synthetic(a) -> int:
    img = pathlib.Path(a.rom).read_bytes()
    truth = listing_truth(pathlib.Path(a.listing).read_text(), pathlib.Path(a.object).read_bytes(), img)
    pred = (pathlib.Path(a.sample) / "classes.bin").read_bytes()
    s = score(pred, truth)
    s["sound"] = not any(s["wrong"].values())
    print(json.dumps(s, sort_keys=True))
    return 0 if s["sound"] else 1


# --------------------------------------------------------------------------------------------------------------------------- plan
PLAN = HERE / "segarecomp_genesis_evidence.t001.plan.json"
PLAN_DIGEST = HERE / "segarecomp_genesis_evidence.t001.plan.digest"
BOUND = ("tools/segarecomp_genesis_evidence.py", "tools/segarecomp_genesis_evidence.Dockerfile",
         "tests/fixtures/genesis_evidence/synthetic_cart.s", "tests/segarecomp_genesis_evidence_test.py",
         "tools/ghidra/scripts/ExportCodeEntryCandidates.java", "tools/ghidra/scripts/PinAnalysisRecipe.java",
         "tools/segarecomp_xplat_corpus.py", "tools/segarecomp_xplat_formats.py", "tools/segarecomp_ml_v3_data.py")
SCENARIOS = [{"id": "attract", "policy": "none", "seconds": 180},
             {"id": "play-s1", "policy": "random", "seed": 1, "seconds": 480, "start_window": 40},
             {"id": "play-s2", "policy": "random", "seed": 2, "seconds": 480, "start_window": 40},
             {"id": "play-s3", "policy": "random", "seed": 3, "seconds": 480, "start_window": 40}]
PROTOCOL = {
    "schema": SCHEMA + ".plan",
    "task": "SEG-077-T001",
    "status": "frozen before the first full Chuck Rock analysis",
    "pilot": {"title": "Chuck Rock (USA)", "lineage": "G077-01", "raw_sha256": "d4cd7003c104f77bf9a9a78f43067f15a63f23925bc77746d0fdde419f594dc4",
              "raw_size": 1048576, "reset": 512},
    "scenarios": SCENARIOS,
    "replicas": 2,
    "evidence_tiers": {
        "I/c": ["E executed: MAME 0.276 trace of :maincpu, PC inside the image, m68k-disasm (walk decode at that PC, else traced) and the "
                "CPU-owned decoder agree on the length; odd PCs, disagreements and overlapping executed spans are masked",
                "R static reach: direct Bcc/BRA/BSR/DBcc/JMP/JSR(abs.w|abs.l|d16(PC)) targets and fall-through of non-call/trap, "
                "non-terminating instructions (calls/traps only when the return site was executed), from accepted code, with "
                "m68k-disasm walk + CPU-owned decoder length agreement and no overlap"],
        "D": ["HW_HEADER: 0x000-0x1FF vector table and cartridge header", "DMA: observed 68K-bus VDP DMA sources inside the image "
              "(control-port reconstruction; an extent overlapping accepted code is masked whole)",
              "DR: observed CPU data reads by an accepted executed reader (or RAM code) that never reads accepted code outside its own "
              "fetch window, outside the 4-byte prefetch shadow after executed code and outside copy origins of RAM-executed code "
              "widened to the copier's contiguous read extent; disabled for the image when any non-trampoline RAM-executed run has no "
              "ROM origin", "Ghidra veto: an ADR-0025 code-entry candidate inside data unlabels the next 10 bytes"],
        "U": "everything else; unknown is never a negative",
        "excluded": ["decoder agreement alone", "non-execution", "byte-exact reassembly", "model scores", "agent judgement",
                     "Ghidra as a label source (veto/disagreement only)", "sega2asm (not selected for the pilot: no gap it fills)"]},
    "cell_labels": "unchanged segarecomp_ml_v3.cell_labels: any I -> positive, all D -> negative, otherwise masked",
    "gates": {
        "G1_synthetic_soundness": "0 wrong I, c or D bytes on the committed synthetic cartridge vs its GNU as listing truth",
        "G2_determinism": "both replicas of every scenario have identical PC, DMA and read digests",
        "G3_mapping": "MAME ROM view equals the image (0 mismatched words) and no executed cartridge-space PC beyond the image",
        "G4_decoder_consistency": "E rejections (no decode, CPU-decoder rejection, length disagreement, overlap) <= 2% of executed ROM PCs",
        "G5_reproduction": "an independent validator (T002) re-runs label from the retained scenario evidence and obtains the same classes digest"},
    "usefulness": {"U1_positive_cells_fraction_min": 0.10, "U2_negative_cells_fraction_min": 0.02, "U3_mixed_positive_cells_min": 50},
    "cost": {"C1_cpu_hours_per_title_max": 10.0, "per_container": "1 CPU, 2 GiB", "trace_timeout_seconds": 7200},
    "classes": {
        "PROCEED": "G1-G5 hold and U1, U2, U3 and C1 hold",
        "PARTIAL": "G1-G5 hold, positive cells >= 5% of cells, but U1, U2, U3 or C1 fails (usable with documented limitation)",
        "STOP": "any of G1-G4 fails after at most one bounded correction cycle, or positive cells < 5% of cells"},
    "stop_classes": ["INPUT_REQUIRED", "RETENTION_DECISION_REQUIRED", "TOOLCHAIN_INFEASIBLE", "BUDGET_EXHAUSTED"],
    "binding": "T001 self-assesses; T002 (independent) owns the binding class",
    "report_metrics": ["qualified starts by tier (E/R) and continuation bytes", "D bytes by tier (HW/DMA/DR)", "unknown bytes",
                       "positive/negative/masked/mixed/hard-negative cells", "typed rejections", "tool disagreement rates (E decode, "
                       "Ghidra-in-data, DMA vs read-tap agreement, m68k-disasm walk instructions not admitted)",
                       "per-scenario coverage and saturation", "replica determinism", "wall and CPU cost per stage"],
    "storage": "<HARNESS_ROOT>/.cache/m68k-corpus/ (git-ignored; ADR 0151 retention); committed evidence is aggregate-only",
}


def bound_digests() -> dict:
    root = HERE.parent
    return {f: sha256((root / f).read_bytes()) for f in BOUND}


def canonical(doc: dict) -> bytes:
    return (json.dumps(doc, indent=1, sort_keys=True) + "\n").encode()


def cmd_freeze(a) -> int:
    if PLAN.exists() and not a.force:
        raise EvidenceError("plan already frozen; a revision needs a recorded amendment")
    doc = dict(PROTOCOL)
    doc["bound_code"] = bound_digests()
    doc["evidence_image_id"] = docker_image_id(EVIDENCE_IMAGE)
    doc["ghidra_image_id"] = docker_image_id(GHIDRA_IMAGE)
    data = canonical(doc)
    PLAN.write_bytes(data)
    PLAN_DIGEST.write_text(sha256(data) + "\n")
    print(json.dumps({"plan_sha256": sha256(data)}))
    return 0


def verify_plan() -> str:
    data = PLAN.read_bytes()
    digest = sha256(data)
    if digest != PLAN_DIGEST.read_text().strip():
        raise EvidenceError("plan digest mismatch")
    doc = json.loads(data)
    if {k: v for k, v in doc.items() if k not in ("bound_code", "evidence_image_id", "ghidra_image_id")} != json.loads(json.dumps(PROTOCOL)):
        raise EvidenceError("committed plan differs from the protocol in code")
    if doc["bound_code"] != bound_digests():
        raise EvidenceError("bound code changed after the freeze")
    return digest


def cmd_run_title(a) -> int:
    from concurrent.futures import ThreadPoolExecutor
    verify_plan()
    root = pathlib.Path(a.out)
    root.mkdir(parents=True, exist_ok=True)
    require_private(root)
    rom = pathlib.Path(a.rom)
    ident = identity(rom.read_bytes())
    (root / "identity.json").write_text(json.dumps(ident, indent=1, sort_keys=True))
    jobs = [(sc, rep) for sc in SCENARIOS for rep in "ab"[:PROTOCOL["replicas"]]]

    def one(job):
        sc, rep = job
        d = root / "runs" / f"{sc['id']}-{rep}"
        if (d / "trace.json").is_file():
            return d
        if d.exists():
            shutil.rmtree(d)
        d.parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run([sys.executable, str(pathlib.Path(__file__).resolve()), "trace", "--rom", str(rom), "--scenario", json.dumps(sc),
                            "--out", str(d), "--timeout", str(PROTOCOL["cost"]["trace_timeout_seconds"])], capture_output=True, text=True)
        if r.returncode:
            raise EvidenceError(f"{sc['id']}-{rep}: {r.stderr[-300:]}")
        return d
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=int(a.workers)) as ex:
        futs = [ex.submit(one, j) for j in jobs]
        if not (root / "ghidra.json").is_file():
            r = subprocess.run([sys.executable, str(pathlib.Path(__file__).resolve()), "ghidra", "--rom", str(rom), "--out", str(root / "ghidra.json")],
                               capture_output=True, text=True)
            if r.returncode:
                raise EvidenceError(f"ghidra: {r.stderr[-300:]}")
        dirs = [f.result() for f in futs]
    det = {}
    for sc in SCENARIOS:
        ts = [json.loads((root / "runs" / f"{sc['id']}-{rep}" / "trace.json").read_text()) for rep in "ab"[:PROTOCOL["replicas"]]]
        keys = ("pcs_sha256", "dma_sha256", "reads_sha256")
        det[sc["id"]] = all(t[k] == ts[0][k] for t in ts for k in keys)
    runs_a = [root / "runs" / f"{sc['id']}-a" for sc in SCENARIOS if det[sc["id"]]]
    if not runs_a:
        raise EvidenceError("no deterministic scenario")
    sample = root / "sample"
    if sample.exists():
        shutil.rmtree(sample)
    meta = label(rom, runs_a, root / "ghidra.json", a.cli, sample, a.lineage, a.id)
    traces = [json.loads((d / "trace.json").read_text()) for d in dirs]
    summary = sanitized(meta)
    summary["determinism"] = det
    summary["trace_wall_seconds_sum"] = round(sum(t["wall_seconds"] for t in traces), 1)
    summary["run_title_wall_seconds"] = round(time.time() - t0, 1)
    summary["map_mismatch"] = sum(t["map_mismatch"] for t in traces)
    summary["rom_pcs_beyond_image"] = sum(t["rom_pcs_beyond_image"] for t in traces)
    summary["dma_bytes_seen_by_read_tap"] = [t["dma_bytes_seen_by_read_tap"] for t in traces]
    summary["dma_rom_bytes"] = [t["dma_rom_bytes"] for t in traces]
    summary["ram_copy"] = [t["ram_copy"] for t in traces]
    (root / "report.json").write_text(json.dumps(summary, indent=1, sort_keys=True))
    print(json.dumps(summary, sort_keys=True))
    return 0


def cmd_verify_plan(a) -> int:
    print(json.dumps({"plan_sha256": verify_plan()}))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("identity")
    p.add_argument("--rom", required=True)
    p.set_defaults(fn=cmd_identity)
    p = sub.add_parser("trace")
    p.add_argument("--rom", required=True)
    p.add_argument("--scenario", required=True, help='JSON, e.g. {"id":"play-s1","policy":"random","seed":1,"seconds":300,"start_window":40}')
    p.add_argument("--out", required=True)
    p.add_argument("--timeout", default=7200)
    p.set_defaults(fn=cmd_trace)
    p = sub.add_parser("ghidra")
    p.add_argument("--rom", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_ghidra)
    p = sub.add_parser("label")
    p.add_argument("--rom", required=True)
    p.add_argument("--runs", nargs="+", required=True)
    p.add_argument("--ghidra")
    p.add_argument("--cli", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--lineage", required=True)
    p.add_argument("--id", required=True)
    p.set_defaults(fn=cmd_label)
    p = sub.add_parser("report")
    p.add_argument("samples", nargs="+")
    p.set_defaults(fn=cmd_report)
    p = sub.add_parser("synthetic")
    p.add_argument("--rom", required=True)
    p.add_argument("--listing", required=True)
    p.add_argument("--object", required=True)
    p.add_argument("--sample", required=True)
    p.set_defaults(fn=cmd_synthetic)
    p = sub.add_parser("run-title")
    p.add_argument("--rom", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--cli", required=True)
    p.add_argument("--lineage", required=True)
    p.add_argument("--id", required=True)
    p.add_argument("--workers", default=8)
    p.set_defaults(fn=cmd_run_title)
    p = sub.add_parser("freeze")
    p.add_argument("--force", action="store_true")
    p.set_defaults(fn=cmd_freeze)
    p = sub.add_parser("verify-plan")
    p.set_defaults(fn=cmd_verify_plan)
    a = ap.parse_args(argv)
    try:
        return a.fn(a)
    except EvidenceError as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
