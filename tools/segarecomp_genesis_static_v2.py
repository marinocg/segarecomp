#!/usr/bin/env python3
"""SEG-077-T009 v2: static-first, evidence-tiered I/c/D/U labels with corrected measurement (OFFLINE research tooling).

v2 changes over the frozen v1 (tools/segarecomp_genesis_static.py, plan 5a1cd094, preserved unchanged with its admission record):
  * scoring: predictions over reference-unknown bytes are reported separately and excluded from every error-rate denominator (v1
    counted them in the denominator only); errors are split into false instruction starts, continuation errors and data errors; error-
    bearing cells, recalled starts and marginal mixed / hard-negative cells are reported per tier
  * decoders: every admitted instruction is decoded by the CPU-owned decoder, m68k-disasm AND Capstone; the CPU decoder must agree with
    every independent decoder that decodes it, and at least one must decode it (v1 used Capstone only where m68k-disasm abstained)
  * provenance: every instruction records its root kind or the edge (fall / direct / callfall / dyncallfall / table) and parent that
    admitted it, for the error taxonomy


T001's protocol labelled code only where an emulator executed it (bounded by scenario reach). This labeller starts from the image itself
and admits each evidence tier only after measuring its wrong-label rate on held-out SEG-072 Genesis images with complete source truth.

Code tiers (each instruction records the first tier that admitted it; descent inside a tier uses the base flow rules):
  V   recursive descent from hardware roots: reset and every exception/interrupt vector target inside the image; base flow = direct
      Bcc/BRA/BSR/DBcc/JMP/JSR (abs.w, abs.l, d16(PC)) targets and fall-through of non-terminating instructions; after a DIRECT call the
      fall-through is followed unless the callee is a return-address consumer (its first instruction reads or adjusts (A7) as a long)
  CD  fall-through after dynamic calls (JSR/BSR through registers or memory) and TRAP
  J   jump/branch tables reached from accepted code: `move.w d8(PC,Xn),Dy` + `jmp d8(PC,Dy)` relative word tables, `movea.l d8(PC,Xn),Ay`
      + `jmp (Ay)` absolute long tables, and `jmp d8(PC,Xn)` over a run of BRA.W/BRA.S/JMP abs.l entries; entries are enumerated while each
      target is an even in-image address that both decoders accept, the table does not reach accepted code or the lowest target, <= 256
  P   pointer-table roots: in a run of >= 3 consecutive aligned longwords outside code where >= 3 entries are accepted code starts, the
      remaining entries pointing inside the image become roots
  G   Ghidra (unchanged ADR-0025 recipe) code-entry candidates that are also the value of an aligned absolute longword pointer elsewhere
  X   executed PCs from the T001 MAME traces (when supplied; E-tier rules of segarecomp_genesis_evidence)
Every admitted instruction must decode to the same length in the CPU-owned decoder and in m68k-disasm (traced mode at that PC, walk as a
fallback); disagreements are removed together with everything reachable only through them (fixpoint). Overlapping candidates: the first
admitted wins and the conflict is counted.

Data tiers:
  HW  0x000-0x1FF vectors and header
  JT  bytes of resolved relative-word / absolute-long jump tables
  PT  pointer-table runs whose entries are all accepted code starts (>= 3)
  X   DMA / admitted data reads from the T001 traces (when supplied)
Code wins over static data only for executed code; any static code/data overlap masks both.

Subcommands:
  calibrate run the labeller on SEG-072 Genesis images and score every tier against the hidden truth (aggregate-only output)
  freeze    write tools/segarecomp_genesis_static.t009.plan.json (+ .digest): rules, sets, admission and usefulness criteria, bound code
  admit     the frozen held-out admission procedure -> tools/segarecomp_genesis_static.t009.admission.json (aggregate-only)
  title     label one title with the ADMITTED tiers (+ T001 execution evidence) -> private sample (T001 sample layout) + report
Development set (rules built and debugged here): d05 d06 d07 d09 d10 d11 d12 flicky. Held-out admission set (seen only after the
freeze): s1 s2 sk ps2 sf1 sf2 (complete source truth).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_genesis_evidence as ge  # noqa: E402

SCHEMA = "segarecomp.genesis_static.v2"
EVIDENCE_IMAGE = "segarecomp-seg077/evidence:t009"     # the same image (id 9bff8705...) under the T009 tag
CAPSTONE_PY = pathlib.Path("/root/dev/segarecomp-harness/.cache/m68k-corpus/tools/capvenv/bin/python")   # capstone==5.0.6 (BSD)
CAPSTONE_SNIPPET = r"""
import sys, json, capstone
img = open(sys.argv[1], "rb").read()
md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_M68K_000)
out = {}
for pc in json.load(open(sys.argv[2])):
    ins = next(md.disasm(img[pc:pc + 10], pc, 1), None)
    if ins is not None:
        out[pc] = ins.size
print(json.dumps(out))
"""
CODE_TIERS = ("X", "V", "CD", "J", "P", "G", "IP", "IL")
DATA_TIERS = ("HW", "JT", "PT", "X", "FILL", "DREF", "PTD", "DC")
DECOMP_IMAGE = "segarecomp-seg077/decomp:1"
DC_MIN_CONSUMED = 256
FILL_MIN = 256
DREF_MAX = 4096
MAX_TABLE = 256
MIN_PTR_RUN = 3


def w16(img, a):
    return ge.u16(img, a)


def is_rac(img: bytes, callee: int) -> bool:
    """Return-address consumer: the callee's first instruction moves (A7)/(A7)+ as a long, or adds/subtracts to (A7) as a long."""
    w = w16(img, callee)
    if w is None:
        return False
    if w >> 12 == 2 and (w & 0x3F) in (0x17, 0x1F):                 # move.l / movea.l (a7)[+],<ea>
        return True
    if w & 0xF000 == 0x5000 and (w >> 6) & 3 == 2 and (w & 0x3F) == 0x17:   # addq/subq.l #k,(a7)
        return True
    if w & 0xF0C0 in (0xD080, 0x9080, 0xD180, 0x9180) and (w >> 6) & 3 == 2 and (w & 0x3F) == 0x17:  # add/sub.l Dn,(a7)
        return True
    if w in (0x0697, 0x0497):                                          # addi.l / subi.l #,(a7)
        return True
    return False


def dynamic_call(img: bytes, pc: int) -> bool:
    w = w16(img, pc)
    if w is None:
        return False
    if w & 0xFFC0 == 0x4E80 and (w & 0x3F) not in (0x38, 0x39, 0x3A):
        return True                                                   # JSR through a register / memory EA
    return w & 0xFFF0 == 0x4E40                                       # TRAP


def table_entries(img: bytes, pc: int, n: int, owner: bytearray, tok: bytes, starts_map: dict | None = None) -> tuple[str, list[int], tuple[int, int]] | None:
    """Recognize a jump/branch table dispatch at the accepted instruction pc. Returns (kind, targets, table byte extent) or None."""
    size = len(img)
    starts_map = {k: v[0] for k, v in (starts_map or {}).items()}
    w = w16(img, pc)
    if w is None:
        return None

    def code_bytes_has(a):
        return 0 <= a < size and owner[a]

    def ok_target(t):
        return t is not None and t % 2 == 0 and ge.HEADER_END <= t < size and ge.token_len(tok, t)

    def d8(ext):
        return ge.s8(ext & 0xFF)
    # relative word table: move.w d8(PC,Xn),Dy ; jmp d8(PC,Dy)
    if w & 0xF1FF == 0x303B:
        nxt = pc + n
        j = w16(img, nxt)
        if j == 0x4EFB:
            t1 = pc + 2 + d8(w16(img, pc + 2))
            t2 = nxt + 2 + d8(w16(img, nxt + 2))
            out, lo = [], None
            for i in range(MAX_TABLE):
                a = t1 + 2 * i
                if code_bytes_has(a) or (lo is not None and a >= lo):
                    break
                e = w16(img, a)
                if e is None:
                    break
                t = t2 + ge.s16(e)
                if not ok_target(t):
                    break
                out.append(t)
                lo = t if lo is None else min(lo, t)
            if out:
                return "rel16", out, (t1, t1 + 2 * len(out))
    # absolute long table: movea.l d8(PC,Xn),Ay ; jmp (Ay)
    if w & 0xF1FF == 0x207B:
        ay = (w >> 9) & 7
        nxt = pc + n
        if w16(img, nxt) == 0x4ED0 | ay:
            t1 = pc + 2 + d8(w16(img, pc + 2))
            out, lo = [], None
            for i in range(MAX_TABLE):
                a = t1 + 4 * i
                if code_bytes_has(a) or (lo is not None and a >= lo):
                    break
                t = ge.u32(img, a)
                if not ok_target(t):
                    break
                out.append(t)
                lo = t if lo is None else min(lo, t)
            if out:
                return "abs32", out, (t1, t1 + 4 * len(out))
    # register-based tables: lea d16(PC),An ; move.w d8(An,Xn),Dy ; jmp d8(An,Dy)   |   lea d16(PC),An ; movea.l d8(An,Xn),Am ; jmp (Am)
    if w & 0xFFF8 == 0x4EF0 or (w & 0xFFF8 == 0x4ED0):
        base_reg = w & 7
        prev = [q for q in range(pc - 2, max(0x200, pc - 14) - 1, -2) if q in starts_map]
        load = next((q for q in prev if q + starts_map[q] == pc), None)
        if load is not None:
            lw = w16(img, load)
            lea = None
            for q in prev:
                qw = w16(img, q)
                if q < load and qw is not None and qw & 0xF1FF == 0x41FA:
                    lea = q
                    break
            if lea is not None:
                an = (w16(img, lea) >> 9) & 7
                t = lea + 2 + ge.s16(w16(img, lea + 2))
                if w & 0xFFF8 == 0x4EF0 and lw & 0xF1F8 == 0x3030 and lw & 7 == an and (lw >> 9) & 7 == (w16(img, pc + 2) >> 12) & 7 \
                        and base_reg == an:
                    t1 = t + d8(w16(img, load + 2))
                    t2 = t + d8(w16(img, pc + 2))
                    out, lo = [], None
                    for i in range(MAX_TABLE):
                        a = t1 + 2 * i
                        if code_bytes_has(a) or (lo is not None and a >= lo):
                            break
                        e = w16(img, a)
                        if e is None:
                            break
                        tg = t2 + ge.s16(e)
                        if not ok_target(tg):
                            break
                        out.append(tg)
                        lo = tg if lo is None else min(lo, tg)
                    if out:
                        return "rel16", out, (t1, t1 + 2 * len(out))
                if w & 0xFFF8 == 0x4ED0 and lw & 0xF1F8 == 0x2070 and lw & 7 == an and (lw >> 9) & 7 == base_reg:
                    t1 = t + d8(w16(img, load + 2))
                    out, lo = [], None
                    for i in range(MAX_TABLE):
                        a = t1 + 4 * i
                        if code_bytes_has(a) or (lo is not None and a >= lo):
                            break
                        tg = ge.u32(img, a)
                        if not ok_target(tg):
                            break
                        out.append(tg)
                        lo = tg if lo is None else min(lo, tg)
                    if out:
                        return "abs32", out, (t1, t1 + 4 * len(out))
    # branch table: jmp d8(PC,Xn) over BRA.W / BRA.S / JMP abs.l entries (the entries are instructions: code roots, not data)
    if w == 0x4EFB:
        prev_is_move = False
        base = pc + 2 + d8(w16(img, pc + 2))
        first = w16(img, base)
        if first is None:
            return None
        if first == 0x6000:
            stride = 4
        elif first & 0xFF00 == 0x6000:
            stride = 2
        elif first == 0x4EF9:
            stride = 6
        else:
            return None
        del prev_is_move
        out = []
        for i in range(MAX_TABLE):
            a = base + stride * i
            e = w16(img, a)
            if code_bytes_has(a) or e is None:
                break
            if (stride == 4 and e != 0x6000) or (stride == 2 and (e & 0xFF00 != 0x6000 or e == 0x6000)) or (stride == 6 and e != 0x4EF9):
                break
            if not ok_target(a):
                break
            out.append(a)
        if out:
            return "branch", out, (base, base)
    return None


def operand_refs(img: bytes, pc: int) -> list[tuple[str, int]]:
    """In-image addresses an accepted instruction names as an operand: ('pea'|'imm'|'lea'|'pcread', address)."""
    w = w16(img, pc)
    out = []
    if w is None:
        return out
    if w & 0xF1FF == 0x41FA:
        out.append(("lea", pc + 2 + ge.s16(w16(img, pc + 2))))
    elif w & 0xF1FF == 0x41F9:
        out.append(("lea", ge.u32(img, pc + 2)))
    elif w == 0x487A:
        out.append(("pea", pc + 2 + ge.s16(w16(img, pc + 2))))
    elif w == 0x4879:
        out.append(("pea", ge.u32(img, pc + 2)))
    elif w & 0xF03F == 0x203C:
        out.append(("imm", ge.u32(img, pc + 2)))
    elif w & 0xC03F == 0x003A and (w >> 12) in (1, 2, 3):             # move.b/l/w d16(PC),<ea>: a data read
        out.append(("pcread", pc + 2 + ge.s16(w16(img, pc + 2))))
    return [(k, a) for k, a in out if a is not None]


def descend(img: bytes, tok: bytes, roots: dict, forbidden: set, allow: set, accepted: dict, owner: bytearray, typed: dict,
            tables: list, prov: dict | None = None):
    """Extend `accepted` {pc: (len, tier)} from roots {pc: tier} with the base flow rules (+ the extra rules in `allow`)."""
    size = len(img)

    def bump(k):
        typed[k] = typed.get(k, 0) + 1
    prov = {} if prov is None else prov
    queue = sorted((pc, t, ("root", None)) for pc, t in roots.items())
    while queue:
        nxt = []
        for pc, tier, how in queue:
            if pc in accepted or pc in forbidden:
                continue
            if pc % 2 or pc < ge.HEADER_END or pc >= size:
                bump("ROOT_OUTSIDE")
                continue
            n = ge.token_len(tok, pc)
            if n is None or pc + n > size:
                bump("NO_DECODE")
                continue
            if any(owner[k] for k in range(pc, pc + n)):
                bump("OVERLAP")
                continue
            accepted[pc] = (n, tier)
            prov[pc] = how
            for k in range(pc, pc + n):
                owner[k] = 1
            tgts, kind = ge.flow(img, pc, n)
            succ = [(t_, "direct") for t_ in tgts]
            if kind == "normal":
                succ.append((pc + n, "fall"))
            elif kind == "call":
                if tgts and not dynamic_call(img, pc):
                    if not is_rac(img, tgts[0]):
                        succ.append((pc + n, "callfall"))
                    else:
                        bump("RAC_CALL")
                elif "CD" in allow:
                    nxt.append((pc + n, "CD" if tier in ("V", "X") else tier, ("dyncallfall", pc)))
            for s, e in succ:
                if s not in accepted:
                    nxt.append((s, tier, (e, pc)))
            if "J" in allow:
                t = table_entries(img, pc, n, owner, tok, accepted)
                if t:
                    kind_t, targets, ext = t
                    tables.append((kind_t, ext))
                    for s in targets:
                        nxt.append((s, "J" if tier in ("V", "X", "CD") else tier, ("table", pc)))
        seen_q, queue = set(), []
        for item in sorted(nxt):
            if item[0] not in seen_q:
                seen_q.add(item[0])
                queue.append(item)


def disasm_lengths(img_path: pathlib.Path, pcs: list[int], work: pathlib.Path) -> dict:
    tf = work / "pcs.txt"
    tf.write_text("".join(f"{p}\n" for p in sorted(pcs)))
    tf.chmod(0o644)
    out = {}
    for flags in (["-frdc"], ["-frdc", "-fwalk"]):
        r = subprocess.run(["docker", "run", "--rm", "--network", "none", "--user", "1000:1000", "-v", f"{img_path}:/in/rom.bin:ro",
                            "-v", f"{tf}:/in/pcs.txt:ro", EVIDENCE_IMAGE, "/opt/m68k-disasm/build/m68k-disasm", *flags, "-t", "/in/pcs.txt",
                            "/in/rom.bin"], capture_output=True, text=True, timeout=3600)
        if r.returncode:
            raise ge.EvidenceError(f"m68k-disasm failed: {r.stderr[-300:]}")
        for pc, v in ge.parse_disasm(r.stdout).items():
            out.setdefault(pc, v)
    return out


def capstone_lengths(img_path: pathlib.Path, pcs: list[int], work: pathlib.Path) -> dict:
    """Capstone M68K (68000 mode) instruction lengths at the given PCs: the independent decoder used where m68k-disasm abstains."""
    sf, pf = work / "cap.py", work / "cap.json"
    sf.write_text(CAPSTONE_SNIPPET)
    pf.write_text(json.dumps(sorted(pcs)))
    r = subprocess.run([str(CAPSTONE_PY), "-I", str(sf), str(img_path), str(pf)], capture_output=True, text=True, timeout=3600)
    if r.returncode:
        raise ge.EvidenceError(f"capstone failed: {r.stderr[-300:]}")
    return {int(k): v for k, v in json.loads(r.stdout).items()}


def decompress_blobs(img_path: pathlib.Path, cands: list[int], work: pathlib.Path, typed: dict) -> list[tuple[int, int]]:
    """sega2asm decompressors (only formats whose routine signature is in the ROM) at code-referenced candidate offsets."""
    cf = work / "dc.json"
    cf.write_text(json.dumps(cands))
    cf.chmod(0o644)
    r = subprocess.run(["docker", "run", "--rm", "--network", "none", "--user", "1000:1000", "-v", f"{img_path}:/in/rom.bin:ro",
                        "-v", f"{cf}:/in/c.json:ro", DECOMP_IMAGE, "seg077decomp", "/in/rom.bin", "/in/c.json"],
                       capture_output=True, text=True, timeout=7200)
    if r.returncode:
        raise ge.EvidenceError(f"decompression helper failed: {r.stderr[-300:]}")
    doc = json.loads(r.stdout)
    typed["DC_DETECTED_FORMATS"] = len({m["Name"] for m in doc.get("detected") or []})
    by_off: dict = {}
    for x in doc.get("results") or []:
        by_off.setdefault(x["offset"], []).append(x)
    out = []
    for off, xs in sorted(by_off.items()):
        ok = [x for x in xs if x["consumed"] >= DC_MIN_CONSUMED and x["out_len"] >= x["consumed"]]
        if not ok:
            continue
        if len({x["consumed"] for x in ok}) != 1:
            typed["DC_REJECTED_AMBIGUOUS"] = typed.get("DC_REJECTED_AMBIGUOUS", 0) + 1
            continue
        out.append((off, off + ok[0]["consumed"]))
    return out


def pointer_runs(img: bytes, accepted: dict, owner: bytearray) -> list[tuple[int, int, list[int]]]:
    """Runs of consecutive aligned longwords outside code: (start, end, values)."""
    size = len(img)
    runs, cur = [], []
    a = 0x200
    while a + 4 <= size:
        v = ge.u32(img, a)
        inside = v is not None and 0x200 <= v < size and v % 2 == 0 and not any(owner[k] for k in range(a, a + 4))
        if inside:
            cur.append((a, v))
            a += 4
            continue
        if len(cur) >= MIN_PTR_RUN:
            runs.append((cur[0][0], cur[-1][0] + 4, [x[1] for x in cur]))
        cur = []
        a += 2
    if len(cur) >= MIN_PTR_RUN:
        runs.append((cur[0][0], cur[-1][0] + 4, [x[1] for x in cur]))
    return runs


def label_static(img: bytes, tok: bytes, img_path: pathlib.Path, work: pathlib.Path, tiers: tuple, ghidra: list[int] = (),
                 executed: list[int] = (), exec_lengths: dict | None = None) -> dict:
    """Return {'code': {pc: (len, tier)}, 'data': [(start, end, tier)], 'typed': {...}, 'tables': n}."""
    size = len(img)
    typed: dict = {}
    forbidden: set = set()
    prov: dict = {}
    for _ in range(6):
        prov = {}
        accepted: dict = {}
        owner = bytearray(size)
        tables: list = []
        roots = {}
        if "X" in tiers:
            for pc in executed:
                if pc % 2 == 0 and pc < size:
                    roots[pc] = "X"
        for i in range(1, 64):
            t = ge.u32(img, 4 * i)
            if t is not None and t % 2 == 0 and ge.HEADER_END <= t < size:
                roots.setdefault(t, "V")
        allow = set(tiers)
        descend(img, tok, roots, forbidden, allow, accepted, owner, typed, tables, prov)
        if "P" in tiers:
            starts = set(accepted)
            new = {}
            for s0, e0, vals in pointer_runs(img, accepted, owner):
                if sum(1 for v in vals if v in starts) >= MIN_PTR_RUN:
                    for v in vals:
                        if v not in accepted:
                            new[v] = "P"
            descend(img, tok, new, forbidden, allow, accepted, owner, typed, tables, prov)
        for tier_name, kinds in (("IP", ("pea", "imm")), ("IL", ("lea",))):
            if tier_name in tiers:
                new = {}
                for pc in list(accepted):
                    for k, adr in operand_refs(img, pc):
                        if k in kinds and adr % 2 == 0 and ge.HEADER_END <= adr < size and adr not in accepted:
                            new[adr] = tier_name
                descend(img, tok, new, forbidden, allow, accepted, owner, typed, tables, prov)
        if "G" in tiers and ghidra:
            ptr_values = set()
            for a in range(0x200, size - 3, 2):
                if not owner[a]:
                    v = ge.u32(img, a)
                    if v is not None and v < size:
                        ptr_values.add(v)
            new = {c: "G" for c in ghidra if c in ptr_values and c not in accepted}
            descend(img, tok, new, forbidden, allow, accepted, owner, typed, tables, prov)
        # two-decoder agreement (fixpoint): remove disagreements and re-derive
        lens = disasm_lengths(img_path, list(accepted), work)
        cap = capstone_lengths(img_path, list(accepted), work)
        bad = set()
        agree = {"cpu_m68k_cap": 0, "cpu_m68k_only": 0, "cpu_cap_only": 0, "independent_disagree": 0, "no_independent": 0}
        for pc, (n, _) in accepted.items():
            m = lens[pc][0] if pc in lens else None
            c = cap.get(pc)
            if m is None and c is None:
                agree["no_independent"] += 1
                bad.add(pc)
            elif (m is not None and m != n) or (c is not None and c != n):
                agree["independent_disagree"] += 1
                bad.add(pc)
            elif m is not None and c is not None:
                agree["cpu_m68k_cap"] += 1
            elif m is not None:
                agree["cpu_m68k_only"] += 1
            else:
                agree["cpu_cap_only"] += 1
        typed["DECODERS"] = agree
        if not bad - forbidden:
            break
        typed["DECODER_DISAGREE_REMOVED"] = typed.get("DECODER_DISAGREE_REMOVED", 0) + len(bad - forbidden)
        forbidden |= bad
    data = [(0, min(ge.HEADER_END, size), "HW")]
    for kind_t, (s0, e0) in tables:
        if kind_t in ("rel16", "abs32") and e0 > s0:
            data.append((s0, e0, "JT"))
    if "FILL" in tiers:
        a = 0x200
        while a < size:
            b = a
            while b < size and img[b] == img[a] and not owner[b]:
                b += 1
            if b - a >= FILL_MIN and img[a] in (0x00, 0xFF):
                data.append((a, b, "FILL"))
            a = max(b, a + 1)
    if "DREF" in tiers:
        refs = set()
        for pc in accepted:
            for k, adr in operand_refs(img, pc):
                if k in ("lea", "pcread") and 0x200 <= adr < size and not owner[adr]:
                    refs.add(adr)
        bounds = sorted(refs)
        for i, s0 in enumerate(bounds):
            e0 = s0
            limit = min(size, s0 + DREF_MAX, bounds[i + 1] if i + 1 < len(bounds) else size)
            while e0 < limit and not owner[e0]:
                e0 += 1
            if e0 < limit and e0 - s0 < DREF_MAX and owner[e0]:
                data.append((s0, e0, "DREF"))      # only regions closed by accepted code inside the cap
    if "PTD" in tiers:
        for s0, e0, vals in pointer_runs(img, accepted, owner):
            if e0 - s0 >= 16:
                data.append((s0, e0, "PTD"))      # runs of >= 4 aligned in-image pointers (any targets) outside code
    if "PT" in tiers:
        starts = set(accepted)
        for s0, e0, vals in pointer_runs(img, accepted, owner):
            if all(v in starts for v in vals):
                data.append((s0, e0, "PT"))
    if "DC" in tiers:
        cands = set()
        for pc in accepted:
            for k, adr in operand_refs(img, pc):
                if k in ("lea", "pcread") and 0x200 <= adr < size and not owner[adr]:   # data-access references only
                    cands.add(adr)
        for s0, e0, vals in pointer_runs(img, accepted, owner):
            cands.update(v for v in vals if not owner[v])
        blobs = decompress_blobs(img_path, sorted(cands), work, typed)
        taken = bytearray(size)
        for off, end in blobs:
            if any(owner[k] for k in range(off, end)) or any(taken[k] for k in range(off, end)):
                typed["DC_REJECTED_OVERLAP"] = typed.get("DC_REJECTED_OVERLAP", 0) + 1
                continue
            for k in range(off, end):
                taken[k] = 1
            data.append((off, end, "DC"))
    return {"code": accepted, "data": data, "typed": typed, "tables": len(tables), "prov": prov}


def classes_of(size: int, res: dict, extra_data: list = (), mask_on_conflict: bool = False) -> tuple[bytearray, dict]:
    cls = bytearray(b"U" * size)
    tier_of = bytearray(size)          # index into CODE_TIERS + DATA_TIERS for scoring
    names = list(CODE_TIERS) + ["D_" + t for t in DATA_TIERS]
    for pc, (n, t) in res["code"].items():
        cls[pc] = ord("I")
        for k in range(pc + 1, pc + n):
            cls[k] = ord("c")
        for k in range(pc, pc + n):
            tier_of[k] = names.index(t) + 1
    conflicts = 0
    for s0, e0, t in list(res["data"]) + list(extra_data):
        if any(cls[k] in b"Ic" for k in range(s0, e0)):
            conflicts += 1
            if mask_on_conflict:                 # observed data over static code: both sides become unknown
                for k in range(s0, e0):
                    if cls[k] in b"Ic":
                        cls[k] = ord("U")
                        tier_of[k] = 0
            continue
        for k in range(s0, e0):
            cls[k] = ord("D")
            tier_of[k] = names.index("D_" + t) + 1
    return cls, {"names": names, "tier_of": tier_of, "data_code_conflicts": conflicts}


def score(cls: bytes, truth: bytes, tier_of: bytes, names: list) -> dict:
    """Per tier: labelled bytes and wrong bytes vs known truth (truth U never counts as wrong)."""
    out = {n: {"labelled": 0, "wrong": 0} for n in names}
    for p, t, k in zip(cls, truth, tier_of):
        if p == ord("U") or k == 0:
            continue
        n = names[k - 1]
        out[n]["labelled"] += 1
        if t == ord("U"):
            continue
        pc, tc = chr(p), chr(t)
        if (pc in "Ic" and tc == "D") or (pc == "D" and tc in "Ic") or (pc == "I" and tc == "c") or (pc == "c" and tc == "I"):
            out[n]["wrong"] += 1
    starts_true = truth.count(b"I")
    out["_recall"] = {"true_starts": starts_true, "found_starts": sum(1 for p, t in zip(cls, truth) if p == t == ord("I")),
                      "true_data": truth.count(b"D"), "found_data": sum(1 for p, t in zip(cls, truth) if p == t == ord("D"))}
    return out


WRONG = {("I", "c"), ("I", "D"), ("c", "I"), ("c", "D"), ("D", "I"), ("D", "c")}


def score_v2(cls: bytes, truth: bytes, tier_of: bytes, names: list, tok: bytes | None = None, cell: int = 128) -> dict:
    """Corrected per-tier measurement (pure). Rates use ONLY predictions against known truth. Cells: 128 bytes; a cell is positive
    (any I), negative (all D) or masked; mixed = positive with D; hard negative = negative adjacent to a positive cell or with >= half
    of its words ordinary decodes; trivial negative = negative made only of FILL bytes. Marginal cell credit goes to a tier when it is
    the ONLY tier contributing the cell's I bytes (positive / mixed-code side) or D bytes (negative / mixed-data side)."""
    T = {n: {"pred_known": 0, "pred_unknown": 0, "false_start": 0, "cont_wrong": 0, "data_wrong": 0, "recalled_starts": 0,
             "error_cells": 0, "marginal_positive": 0, "marginal_mixed": 0, "marginal_hard_negative": 0, "marginal_trivial_negative": 0}
         for n in names}
    err_cells = {n: set() for n in names}
    for i, (p, t, k) in enumerate(zip(cls, truth, tier_of)):
        if p == ord("U") or k == 0:
            continue
        n = names[k - 1]
        if t == ord("U"):
            T[n]["pred_unknown"] += 1
            continue
        T[n]["pred_known"] += 1
        pc, tc = chr(p), chr(t)
        if (pc, tc) in WRONG:
            T[n]["false_start" if pc == "I" else "cont_wrong" if pc == "c" else "data_wrong"] += 1
            err_cells[n].add(i // cell)
        elif pc == tc == "I":
            T[n]["recalled_starts"] += 1
    ncell = (len(cls) + cell - 1) // cell
    labels = []
    for c in range(ncell):
        ch = cls[c * cell:(c + 1) * cell]
        labels.append(1 if b"I" in ch else 0 if ch.count(b"D") == len(ch) else -1)
    fill = names.index("D_FILL") + 1 if "D_FILL" in names else -1
    for c in range(ncell):
        lo, hi = c * cell, min(len(cls), (c + 1) * cell)
        i_tiers = {tier_of[k] for k in range(lo, hi) if cls[k] == ord("I")}
        d_tiers = {tier_of[k] for k in range(lo, hi) if cls[k] == ord("D")}
        if labels[c] == 1:
            if len(i_tiers) == 1:
                T[names[next(iter(i_tiers)) - 1]]["marginal_positive"] += 1
            if d_tiers:
                for side in (i_tiers, d_tiers):
                    if len(side) == 1:
                        T[names[next(iter(side)) - 1]]["marginal_mixed"] += 1
        elif labels[c] == 0 and len(d_tiers) == 1:
            n = names[next(iter(d_tiers)) - 1]
            if d_tiers == {fill}:
                T[n]["marginal_trivial_negative"] += 1
                continue
            near = any(0 <= c + d < ncell and labels[c + d] == 1 for d in (-1, 1))
            dec = tok is not None and sum(1 for k in range(lo, hi, 2) if ge.token_len(tok, k)) * 4 >= (hi - lo)
            if near or dec:
                T[n]["marginal_hard_negative"] += 1
    for n in names:
        T[n]["error_cells"] = len(err_cells[n])
        w = T[n]["false_start"] + T[n]["cont_wrong"] + T[n]["data_wrong"]
        T[n]["wrong_known"] = w
        T[n]["rate_known"] = round(w / T[n]["pred_known"], 6) if T[n]["pred_known"] else None
    return T


def taxonomy(cls: bytes, truth: bytes, res: dict, tier_of: bytes, names: list) -> dict:
    """Root-cause classes of wrong labels (pure given the labeller result). Code: per wrong instruction, by provenance; an error whose
    parent instruction is itself wrong is 'inherited'. Data: by data tier."""
    def wrong_at(k):
        t = truth[k]
        return t != ord("U") and (chr(cls[k]), chr(t)) in WRONG
    code = res["code"]
    prov = res.get("prov", {})
    bad_ins = {pc for pc, (n, _) in code.items() if cls[pc] in b"Ic" and any(wrong_at(k) for k in range(pc, pc + n))}
    out: dict = {}

    def add(k, nbytes):
        out.setdefault(k, {"instructions": 0, "bytes": 0})
        out[k]["instructions"] += 1
        out[k]["bytes"] += nbytes
    for pc in sorted(bad_ins):
        n = code[pc][0]
        nb = sum(1 for k in range(pc, pc + n) if wrong_at(k))
        how = prov.get(pc, ("root", None))
        if how[0] == "root":
            add(f"incorrect_root:{code[pc][1]}", nb)
        elif how[1] in bad_ins:
            add("inherited_from_wrong_parent", nb)
        elif how[0] in ("fall", "direct", "callfall"):
            add(f"direct_flow:{how[0]}", nb)
        elif how[0] == "dyncallfall":
            add("dynamic_call_fallthrough", nb)
        elif how[0] == "table":
            add("indirect_dispatch_table", nb)
        else:
            add("other", nb)
    data_class = {"D_JT": "pointer_table_boundary", "D_PT": "pointer_table_boundary", "D_PTD": "pointer_table_boundary",
                  "D_DC": "compressed_blob_provenance", "D_FILL": "data_extent", "D_DREF": "data_extent", "D_HW": "hardware_header",
                  "D_X": "execution_data"}
    for k in range(len(cls)):
        if cls[k] == ord("D") and wrong_at(k) and tier_of[k]:
            c = data_class.get(names[tier_of[k] - 1], "other_data")
            out.setdefault(c, {"instructions": 0, "bytes": 0})
            out[c]["bytes"] += 1
    out["unassessable_predictions_over_reference_unknown"] = {"bytes": sum(1 for p, t in zip(cls, truth) if p != ord("U") and t == ord("U"))}
    dec = res["typed"].get("DECODERS", {})
    out["decoder_disagreement_removed"] = {"instructions": res["typed"].get("DECODER_DISAGREE_REMOVED", 0), "final_vector": dec}
    return out


# ------------------------------------------------------------------------------------------------------------------------ calibrate
SEG072 = pathlib.Path("/root/dev/segarecomp-harness/.cache/seg072/data")
DEV_SET = ("d05", "d06", "d07", "d09", "d10", "d11", "d12", "flicky")
HELD_OUT = ("s1", "s2", "sk", "ps2", "sf1", "sf2")


def seg072_images(ids) -> list[pathlib.Path]:
    out = []
    for m in sorted(SEG072.rglob("meta.json")):
        d = json.loads(m.read_text())
        if d.get("kind") == "genesis_rom" and d["id"] in ids:
            out.append(m.parent)
    return out


def cmd_calibrate(a) -> int:
    ids = DEV_SET if a.set == "dev" else HELD_OUT
    work = pathlib.Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    work.chmod(0o755)
    tiers = tuple(a.tiers.split(","))
    agg = {}
    per = {}
    for d in seg072_images(ids):
        m = json.loads((d / "meta.json").read_text())
        img_path = pathlib.Path(m["image"])
        img = img_path.read_bytes()
        tok = (d / "tokens.bin").read_bytes()
        truth = (d / "classes.bin").read_bytes()
        gj = work / f"{m['id']}.ghidra.json"
        ghidra = []
        if "G" in tiers:
            if not gj.is_file():
                r = subprocess.run([sys.executable, str(HERE / "segarecomp_genesis_evidence.py"), "ghidra", "--rom", str(img_path), "--out", str(gj)],
                                   capture_output=True, text=True)
                if r.returncode:
                    raise SystemExit(r.stderr[-300:])
            ghidra = [int(x["address"]) for x in json.loads(gj.read_text()) if x.get("kind") == "code_entry_candidate"]
        res = label_static(img, tok, img_path, work, tiers, ghidra)
        cls, info = classes_of(len(img), res)
        sc = score(cls, truth, info["tier_of"], info["names"])
        per[m["id"]] = {k: v for k, v in sc.items()}
        per[m["id"]]["typed"] = res["typed"]
        for k, v in sc.items():
            if k.startswith("_"):
                continue
            agg.setdefault(k, {"labelled": 0, "wrong": 0})
            agg[k]["labelled"] += v["labelled"]
            agg[k]["wrong"] += v["wrong"]
        r = sc["_recall"]
        print(json.dumps({"id": m["id"], "starts": f"{r['found_starts']}/{r['true_starts']}", "data": f"{r['found_data']}/{r['true_data']}",
                          "wrong": {k: v["wrong"] for k, v in sc.items() if not k.startswith("_") and v["labelled"]},
                          "labelled": {k: v["labelled"] for k, v in sc.items() if not k.startswith("_") and v["labelled"]}}), flush=True)
    print(json.dumps({"aggregate": {k: dict(v, rate=round(v["wrong"] / v["labelled"], 6) if v["labelled"] else None) for k, v in agg.items()}}))
    if a.out:
        pathlib.Path(a.out).write_text(json.dumps({"set": a.set, "tiers": tiers, "per_image": per, "aggregate": agg}, indent=1, sort_keys=True))
    return 0


# ---------------------------------------------------------------------------------------------------------------------- frozen plan
PLAN = HERE / "segarecomp_genesis_static_v2.t009.plan.json"
PLAN_DIGEST = HERE / "segarecomp_genesis_static_v2.t009.plan.digest"
ADMISSION = HERE / "segarecomp_genesis_static_v2.t009.admission.json"
BOUND = ("tools/segarecomp_genesis_static.py", "tools/segarecomp_genesis_evidence.py", "tools/segarecomp_genesis_decomp/main.go",
         "tools/segarecomp_genesis_decomp.Dockerfile", "tools/segarecomp_genesis_evidence.Dockerfile",
         "tools/ghidra/scripts/ExportCodeEntryCandidates.java", "tools/ghidra/scripts/PinAnalysisRecipe.java")
BASE_TIERS = ("V", "CD", "J", "PT", "FILL", "PTD", "DC")
OPTIONAL_TIERS = ("P", "G", "IP", "IL", "DREF")
RULES = {
    "development_set": list(DEV_SET), "heldout_set": list(HELD_OUT),
    "base_tiers": list(BASE_TIERS), "optional_tiers": list(OPTIONAL_TIERS), "always": ["HW (vectors + header)", "JT (resolved table bytes)"],
    "wrong_label": "predicted I vs truth c/D, c vs I/D, D vs I/c; truth U never counts",
    "admission": "a tier is admitted when, over the held-out images, its wrong bytes / labelled bytes <= 0.001 and no image exceeds 0.005; "
                 "a tier labelling < 1000 bytes in total is admitted only with 0 wrong bytes. Procedure: measure the base tiers together; "
                 "while a base tier fails, drop the failing base tier with the highest aggregate rate and re-measure; then measure each "
                 "optional tier alone on top of the admitted base (marginal bytes) and admit it if it passes",
    "chuck_rock": "admitted static tiers + T001 execution evidence (executed roots through the same two-decoder descent; DMA and admitted "
                  "data reads from the A1 runs); any static code / execution-data or static data / executed-code overlap masks both",
    "soundness": "the labeller with the admitted tiers gives 0 wrong I/c/D (frozen definition; labels over listing-unknown bytes such "
                 "as .org fill are reported separately) on the committed synthetic fixture, static-only and with each G1 scenario's "
                 "execution evidence, and the Chuck Rock labels are digest-reproducible",
    "usefulness_and_classes": "as ADR 0171: PROCEED when positive cells >= 10%, negative cells >= 2% and >= 50 mixed cells; PARTIAL when "
                              "positive cells >= 5%; STOP otherwise or when a soundness gate fails; the independent validator binds",
    "decoders": "CPU-owned token decoder + m68k-disasm bb6d839 (traced, walk); Capstone 5.0.6 where m68k-disasm abstains",
    "decompression": "sega2asm 78f37b0 decompressors, only formats whose routine signature is detected in the ROM; candidates = lea / "
                     "PC-relative read targets and pointer-table entries outside code; consumed >= 256 bytes, output >= input, all "
                     "decoding formats agree on the extent, no overlap with code or another blob",
}


def bound_digests() -> dict:
    root = HERE.parent
    return {f: hashlib.sha256((root / f).read_bytes()).hexdigest() for f in BOUND}


def cmd_freeze(a) -> int:
    if PLAN.exists() and not a.force:
        raise SystemExit("plan already frozen")
    doc = {"schema": SCHEMA + ".plan", "task": "SEG-077-T009", "status": "frozen before the held-out admission set is labelled",
           "rules": RULES, "bound_code": bound_digests(),
           "images": {k: subprocess.run(["docker", "image", "inspect", k, "--format", "{{.Id}}"], capture_output=True, text=True).stdout.strip()
                      for k in (EVIDENCE_IMAGE, DECOMP_IMAGE, ge.GHIDRA_IMAGE)},
           "capstone_lib_sha256": hashlib.sha256(next(CAPSTONE_PY.parent.parent.glob("lib/python3*/site-packages/capstone/lib/libcapstone.so")).read_bytes()).hexdigest()}
    data = (json.dumps(doc, indent=1, sort_keys=True) + "\n").encode()
    PLAN.write_bytes(data)
    PLAN_DIGEST.write_text(hashlib.sha256(data).hexdigest() + "\n")
    print(json.dumps({"plan_sha256": hashlib.sha256(data).hexdigest()}))
    return 0


def verify_plan() -> dict:
    data = PLAN.read_bytes()
    if hashlib.sha256(data).hexdigest() != PLAN_DIGEST.read_text().strip():
        raise SystemExit("T009 plan digest mismatch")
    doc = json.loads(data)
    if doc["rules"] != json.loads(json.dumps(RULES)) or doc["bound_code"] != bound_digests():
        raise SystemExit("T009 code or rules changed after the freeze")
    return doc


def run_set(ids, tiers, work, ghidra_needed):
    agg, per = {}, {}
    for d in seg072_images(ids):
        m = json.loads((d / "meta.json").read_text())
        img_path = pathlib.Path(m["image"])
        img = img_path.read_bytes()
        tok = (d / "tokens.bin").read_bytes()
        truth = (d / "classes.bin").read_bytes()
        ghidra = []
        if ghidra_needed:
            gj = work / f"{m['id']}.ghidra.json"
            if not gj.is_file():
                r = subprocess.run([sys.executable, str(HERE / "segarecomp_genesis_evidence.py"), "ghidra", "--rom", str(img_path), "--out", str(gj)],
                                   capture_output=True, text=True)
                if r.returncode:
                    raise SystemExit(r.stderr[-300:])
            ghidra = [int(x["address"]) for x in json.loads(gj.read_text()) if x.get("kind") == "code_entry_candidate"]
        res = label_static(img, tok, img_path, work, tuple(tiers), ghidra)
        cls, info = classes_of(len(img), res)
        sc = score(cls, truth, info["tier_of"], info["names"])
        per[m["id"]] = sc
        for k, v in sc.items():
            if k.startswith("_"):
                continue
            agg.setdefault(k, {"labelled": 0, "wrong": 0})
            agg[k]["labelled"] += v["labelled"]
            agg[k]["wrong"] += v["wrong"]
    return agg, per


def tier_passes(name, agg, per) -> bool:
    lab, wr = agg.get(name, {}).get("labelled", 0), agg.get(name, {}).get("wrong", 0)
    if lab == 0:
        return True
    if lab < 1000:
        return wr == 0
    if wr / lab > 0.001:
        return False
    return all(p[name]["wrong"] / p[name]["labelled"] <= 0.005 for p in per.values() if p.get(name, {}).get("labelled"))


def key(t):
    return t if t in CODE_TIERS else "D_" + t


def cmd_admit(a) -> int:
    verify_plan()
    work = pathlib.Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    work.chmod(0o755)
    base = list(BASE_TIERS)
    log = []
    while True:
        agg, per = run_set(HELD_OUT, base, work, False)
        failing = [t for t in base if not tier_passes(key(t), agg, per)]
        log.append({"tiers": list(base), "aggregate": agg, "failing": failing})
        print(json.dumps({"base": base, "failing": failing}), flush=True)
        if not failing:
            break
        worst = max(failing, key=lambda t: agg[key(t)]["wrong"] / max(1, agg[key(t)]["labelled"]))
        base.remove(worst)
    base_agg, base_per = agg, per
    optional = {}
    for t in OPTIONAL_TIERS:
        agg, per = run_set(HELD_OUT, base + [t], work, t == "G")
        ok = tier_passes(key(t), agg, per)
        optional[t] = {"aggregate": agg.get(key(t)), "admitted": ok,
                       "base_tiers_still_pass": all(tier_passes(key(b), agg, per) for b in base)}
        print(json.dumps({"optional": t, "admitted": ok, "agg": agg.get(key(t))}), flush=True)
    admitted = base + [t for t, v in optional.items() if v["admitted"] and v["base_tiers_still_pass"]]
    recall = {}
    agg, per = run_set(HELD_OUT, admitted, work, "G" in admitted)
    for i, p in per.items():
        r = p["_recall"]
        recall[i] = {"starts_found_fraction": round(r["found_starts"] / max(1, r["true_starts"]), 4),
                     "data_found_fraction": round(r["found_data"] / max(1, r["true_data"]), 4),
                     "wrong_total": sum(v["wrong"] for k, v in p.items() if not k.startswith("_"))}
    doc = {"schema": SCHEMA + ".admission", "plan_sha256": hashlib.sha256(PLAN.read_bytes()).hexdigest(), "admitted_tiers": admitted,
           "base_procedure": log, "base_aggregate": base_agg, "optional": optional, "final_aggregate": agg, "heldout_recall": recall,
           "data_boundary": "aggregates only (counts and fractions)"}
    ADMISSION.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"admitted": admitted, "recall": recall}, indent=1))
    return 0


def admitted_tiers() -> list[str]:
    doc = json.loads(ADMISSION.read_text())
    if doc["plan_sha256"] != hashlib.sha256(PLAN.read_bytes()).hexdigest():
        raise SystemExit("admission record belongs to another plan")
    return doc["admitted_tiers"]


def execution_evidence(runs: list[pathlib.Path], t001_sample: pathlib.Path | None) -> tuple[list[int], list]:
    """Executed ROM PCs (union of the runs) and the T001 sample's validated execution data extents (DMA, DR)."""
    pcs = set()
    for r in runs:
        pcs.update(int(k) for k in json.loads((r / "pcs.json").read_text())["rom"])
    data = []
    if t001_sample is not None:
        for e in json.loads((t001_sample / "spans.json").read_text())["data_extents"]:
            if e["kind"] in ("DMA", "DR") and not e["masked"]:
                data.append((e["start"], e["end"], "X"))
    return sorted(pcs), data


def cmd_title(a) -> int:
    verify_plan()
    tiers = admitted_tiers()
    out = pathlib.Path(a.out)
    ge.require_private(out.parent)
    out.mkdir(parents=True, exist_ok=False)
    work = out / ".work"
    work.mkdir()
    work.chmod(0o755)
    img_path, ident = ge.stage_image(pathlib.Path(a.rom), work)
    img = img_path.read_bytes()
    tok = ge.tokens_for(a.cli, img, work)
    runs = [pathlib.Path(r) for r in (a.runs or [])]
    executed, xdata = execution_evidence(runs, pathlib.Path(a.t001_sample) if a.t001_sample else None)
    ghidra = []
    if "G" in tiers and a.ghidra:
        ghidra = [int(x["address"]) for x in json.loads(pathlib.Path(a.ghidra).read_text()) if x.get("kind") == "code_entry_candidate"]
    use = tuple(tiers) + (("X",) if executed else ())
    res = label_static(img, tok, img_path, work, use, ghidra, executed)
    cls, info = classes_of(len(img), res, xdata, mask_on_conflict=True)
    sys.path.insert(0, str(HERE))
    import segarecomp_ml_v3_data as v3d
    reset = ident["reset"]
    seeds = sorted({reset // ge.CELL_BYTES}) if ident["reset_in_image"] and cls[reset] == ord("I") else []
    (out / "classes.bin").write_bytes(bytes(cls))
    (out / "tokens.bin").write_bytes(tok)
    (out / "seeds.json").write_text(json.dumps(seeds))
    (out / "relocs.json").write_text("[]")
    names = info["names"]
    tier_bytes = {n: 0 for n in names}
    for k in info["tier_of"]:
        if k:
            tier_bytes[names[k - 1]] += 1
    starts_by_tier = {}
    for pc, (n, t) in res["code"].items():
        if cls[pc] == ord("I"):
            starts_by_tier.setdefault(t, []).append(pc)
    data_ext = [{"kind": t, "start": s0, "end": e0} for s0, e0, t in list(res["data"]) + xdata]
    (out / "spans.json").write_text(json.dumps({"tiers": {t: sorted(v) for t, v in starts_by_tier.items()}, "data_extents": data_ext,
                                                "typed": res["typed"]}, sort_keys=True))
    stats = ge.cell_stats(bytes(cls), img, tok)
    image_ref = str(img_path) if ident["format"] == "plain" else None
    if image_ref is None:
        keep = out / "image.bin"
        keep.write_bytes(img)
        image_ref = str(keep.resolve())
    meta = {"schema": "segarecomp.xplat_corpus.sample.v1", "evidence_schema": SCHEMA, "id": a.id, "kind": "genesis_commercial",
            "lineage": a.lineage, "candidate": a.lineage, "platform": "genesis", "cpu_variant": "MC68000", "provenance": "commercial-evidence",
            "truth": "partial, static-first: admitted static tiers " + ",".join(tiers) + (" + T001 execution evidence" if executed else "") +
                     "; U elsewhere (never negative)", "completeness": "partial", "image": image_ref, "sha256": ident["sha256"],
            "size": len(img), "identity": ident, "classes_sha256": hashlib.sha256(bytes(cls)).hexdigest(),
            "tokens_sha256": hashlib.sha256(tok).hexdigest(), "seeds_sha256": hashlib.sha256(json.dumps(seeds).encode()).hexdigest(),
            "seed_cells": len(seeds), "relocations": 0, "audit": v3d.audit(bytes(cls), tok), "base_address": 0,
            "bytes": {c: cls.count(c.encode()) for c in "IcDU"}, "cells": stats, "tier_bytes": tier_bytes, "typed": res["typed"],
            "data_code_conflicts": info["data_code_conflicts"], "executed_rom_pcs": len(executed),
            "tools": {"plan_sha256": hashlib.sha256(PLAN.read_bytes()).hexdigest(), "admitted_tiers": tiers,
                      "cli_sha256": hashlib.sha256(pathlib.Path(a.cli).read_bytes()).hexdigest()}}
    (out / "meta.json").write_text(json.dumps(meta, indent=1, sort_keys=True))
    import shutil
    shutil.rmtree(work)
    rep_ = {k: meta[k] for k in ("bytes", "cells", "tier_bytes", "typed", "data_code_conflicts", "executed_rom_pcs", "audit", "classes_sha256")}
    rep_["positive_fraction"] = round(stats["positive"] / stats["cells"], 4)
    rep_["negative_fraction"] = round(stats["negative"] / stats["cells"], 4)
    print(json.dumps(rep_, sort_keys=True))
    return 0


def t009_wrong(pred: bytes, truth: bytes) -> dict:
    """The frozen T009 wrong-label definition (truth U never counts) plus labelled-over-unknown counts reported separately."""
    w = {"I": 0, "c": 0, "D": 0}
    over_u = {"I": 0, "c": 0, "D": 0}
    for p, t in zip(pred, truth):
        pc, tc = chr(p), chr(t)
        if pc == "U":
            continue
        if tc == "U":
            over_u[pc] += 1
        elif (pc in "Ic" and tc == "D") or (pc == "D" and tc in "Ic") or (pc == "I" and tc == "c") or (pc == "c" and tc == "I"):
            w[pc] += 1
    return {"wrong": w, "labelled_over_truth_unknown": over_u, "recalled_starts": sum(1 for p, t in zip(pred, truth) if p == t == ord("I")),
            "true_starts": truth.count(b"I"), "recalled_data": sum(1 for p, t in zip(pred, truth) if p == t == ord("D")),
            "true_data": truth.count(b"D")}


def cmd_synthetic(a) -> int:
    """Soundness of the admitted static tiers (+ execution evidence) on the committed fixture vs its GNU as listing truth."""
    verify_plan()
    tiers = admitted_tiers()
    g = pathlib.Path(a.g1)
    img_path = g / "fixture.bin"
    img = img_path.read_bytes()
    truth = ge.listing_truth((g / "fixture.lst").read_text(), (g / "fixture.o").read_bytes(), img)
    work = pathlib.Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    work.chmod(0o755)
    tok = ge.tokens_for(a.cli, img, work)
    out = {}
    for scen in ("g1-attract", "g1-play", "g1-explore"):
        executed, xdata = execution_evidence([g / scen], g / f"sample-{scen}")
        res = label_static(img, tok, img_path, work, tuple(tiers) + ("X",), [], executed)
        cls, info = classes_of(len(img), res, xdata, mask_on_conflict=True)
        sc = t009_wrong(bytes(cls), truth)
        sc["sound"] = not any(sc["wrong"].values())
        out[scen] = sc
    static_only = label_static(img, tok, img_path, work, tuple(tiers), [], [])
    cls, info = classes_of(len(img), static_only)
    sc = t009_wrong(bytes(cls), truth)
    sc["sound"] = not any(sc["wrong"].values())
    out["static-only"] = sc
    out["sound"] = all(v["sound"] for v in out.values() if isinstance(v, dict))
    print(json.dumps(out, sort_keys=True))
    return 0 if out["sound"] else 1


EXPOSED = HELD_OUT          # used by the v1 admission: exposed validation data from now on, never a fresh independent test
V1_ADMITTED = ("V", "J", "PT", "FILL", "IP")


def measure(ids, tiers, work, ghidra_needed):
    """score_v2 + taxonomy summed over images (aggregates only)."""
    tot, tax, per = {}, {}, {}
    for d in seg072_images(ids):
        m = json.loads((d / "meta.json").read_text())
        img_path = pathlib.Path(m["image"])
        img = img_path.read_bytes()
        tok = (d / "tokens.bin").read_bytes()
        truth = (d / "classes.bin").read_bytes()
        ghidra = []
        if ghidra_needed:
            gj = work / f"{m['id']}.ghidra.json"
            if not gj.is_file():
                r = subprocess.run([sys.executable, str(HERE / "segarecomp_genesis_evidence.py"), "ghidra", "--rom", str(img_path), "--out", str(gj)],
                                   capture_output=True, text=True)
                if r.returncode:
                    raise SystemExit(r.stderr[-300:])
            ghidra = [int(x["address"]) for x in json.loads(gj.read_text()) if x.get("kind") == "code_entry_candidate"]
        res = label_static(img, tok, img_path, work, tuple(tiers), ghidra)
        cls, info = classes_of(len(img), res)
        sc = score_v2(bytes(cls), truth, info["tier_of"], info["names"], tok)
        tx = taxonomy(bytes(cls), truth, res, info["tier_of"], info["names"])
        per[m["id"]] = {"tiers": {k: v for k, v in sc.items() if v["pred_known"] or v["pred_unknown"]},
                        "recall_starts": round(sum(1 for p_, t_ in zip(cls, truth) if p_ == t_ == ord("I")) / max(1, truth.count(b"I")), 4),
                        "decoders": res["typed"].get("DECODERS")}
        for n, v in sc.items():
            a = tot.setdefault(n, {})
            for k, x in v.items():
                if isinstance(x, int):
                    a[k] = a.get(k, 0) + x
        for k, v in tx.items():
            if isinstance(v, dict) and "bytes" in v:
                a = tax.setdefault(k, {"instructions": 0, "bytes": 0})
                a["instructions"] += v.get("instructions", 0)
                a["bytes"] += v["bytes"]
    for n, a in tot.items():
        a["rate_known"] = round(a["wrong_known"] / a["pred_known"], 6) if a.get("pred_known") else None
    return {"tiers": {k: v for k, v in tot.items() if v.get("pred_known") or v.get("pred_unknown")}, "taxonomy": tax, "per_image": per}


def cmd_audit(a) -> int:
    work = pathlib.Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    work.chmod(0o755)
    configs = [("v1_admitted", list(V1_ADMITTED))] + [(f"v1_admitted+{t}", list(V1_ADMITTED) + [t]) for t in ("CD", "PTD", "DC", "P", "G", "IL", "DREF")]
    if a.only:
        configs = [c for c in configs if c[0] in a.only]
    doc = {"schema": SCHEMA + ".audit", "note": "v2 measurement; exposed = the six images used by the v1 admission", "sets": {}}
    for set_name, ids in (("development", DEV_SET), ("exposed_validation", EXPOSED)):
        doc["sets"][set_name] = {}
        for cname, tiers in configs:
            r = measure(ids, tiers, work, "G" in tiers)
            doc["sets"][set_name][cname] = r
            print(json.dumps({"set": set_name, "config": cname,
                              "tiers": {k: [v.get("pred_known"), v.get("pred_unknown"), v.get("wrong_known"), v.get("rate_known")]
                                        for k, v in r["tiers"].items()}}), flush=True)
    pathlib.Path(a.out).write_text(json.dumps(doc, indent=1, sort_keys=True))
    return 0


def ghidra_instructions(img_path: pathlib.Path, work: pathlib.Path, seed: bool) -> list[tuple[int, int]]:
    """Every instruction the pinned Ghidra recipe disassembles (optionally with Genesis vector seeding as a pre-script)."""
    d = work / ("ghidra-seed" if seed else "ghidra-noseed")
    (d / "in").mkdir(parents=True, exist_ok=True)
    (d / "out").mkdir(exist_ok=True)
    import shutil
    shutil.copyfile(img_path, d / "in" / "rom.bin")
    (d / "in" / "rom.bin").chmod(0o644)
    (d / "in").chmod(0o755)
    ge.container_dir(d / "out", 10001)
    out = d / "out" / "ins.json"
    if out.exists():
        out.unlink()
    pre = ["-preScript", "PinAnalysisRecipe.java"] + (["-preScript", "SeedGenesisVectors.java"] if seed else [])
    cmd = ["docker", "run", "--rm", "--network", "none", "-v", f"{d / 'in'}:/inputs:ro", "-v", f"{HERE / 'ghidra' / 'scripts'}:/scripts:ro",
           "-v", f"{d / 'out'}:/analysis-out", "--entrypoint", "/opt/ghidra/support/analyzeHeadless", ge.GHIDRA_IMAGE, "/tmp", "seg077",
           "-import", "/inputs/rom.bin", "-processor", "68000:BE:32:default", "-loader", "BinaryLoader", "-loader-baseAddr", "0x0",
           "-max-cpu", "1", "-analysisTimeoutPerFile", "1200", "-scriptPath", "/scripts", *pre,
           "-postScript", "ExportInstructionStarts.java", "/analysis-out/ins.json", "-deleteProject"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
    if not out.is_file():
        raise SystemExit(f"ghidra export failed: {r.stdout[-300:]}")
    data = json.loads(out.read_text())
    (d / "in" / "rom.bin").unlink()
    return [(a, n) for a, n in data]


def cmd_ghidra_audit(a) -> int:
    work = pathlib.Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    doc = {}
    for set_name, ids in (("development", DEV_SET), ("exposed_validation", EXPOSED)):
        for seed in (False, True):
            agg = {"instructions": 0, "starts_on_truth_I": 0, "false_starts_known": 0, "starts_on_unknown": 0, "true_starts": 0}
            per = {}
            for d in seg072_images(ids):
                m = json.loads((d / "meta.json").read_text())
                truth = (d / "classes.bin").read_bytes()
                ins = ghidra_instructions(pathlib.Path(m["image"]), work / m["id"], seed)
                ok = sum(1 for pc, n in ins if pc < len(truth) and truth[pc] == ord("I"))
                unk = sum(1 for pc, n in ins if pc < len(truth) and truth[pc] == ord("U"))
                bad = len(ins) - ok - unk
                per[m["id"]] = {"instructions": len(ins), "recall_starts": round(ok / max(1, truth.count(b"I")), 4),
                                "false_start_rate_known": round(bad / max(1, ok + bad), 5)}
                agg["instructions"] += len(ins)
                agg["starts_on_truth_I"] += ok
                agg["false_starts_known"] += bad
                agg["starts_on_unknown"] += unk
                agg["true_starts"] += truth.count(b"I")
            agg["recall_starts"] = round(agg["starts_on_truth_I"] / max(1, agg["true_starts"]), 4)
            agg["false_start_rate_known"] = round(agg["false_starts_known"] / max(1, agg["starts_on_truth_I"] + agg["false_starts_known"]), 5)
            key_ = f"{set_name}/{'vector_seeded' if seed else 'recipe_only'}"
            doc[key_] = {"aggregate": agg, "per_image": per}
            print(json.dumps({key_: agg}), flush=True)
    pathlib.Path(a.out).write_text(json.dumps(doc, indent=1, sort_keys=True))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("calibrate")
    c.add_argument("--set", choices=("dev", "heldout"), required=True)
    c.add_argument("--tiers", default="V,CD,J,P,G,PT")
    c.add_argument("--work", required=True)
    c.add_argument("--out")
    c.set_defaults(fn=cmd_calibrate)
    f = sub.add_parser("freeze")
    f.add_argument("--force", action="store_true")
    f.set_defaults(fn=cmd_freeze)
    ad = sub.add_parser("admit")
    ad.add_argument("--work", required=True)
    ad.set_defaults(fn=cmd_admit)
    t = sub.add_parser("title")
    t.add_argument("--rom", required=True)
    t.add_argument("--cli", required=True)
    t.add_argument("--out", required=True)
    t.add_argument("--lineage", required=True)
    t.add_argument("--id", required=True)
    t.add_argument("--runs", nargs="*")
    t.add_argument("--t001-sample")
    t.add_argument("--ghidra")
    t.set_defaults(fn=cmd_title)
    sy = sub.add_parser("synthetic")
    sy.add_argument("--g1", required=True)
    sy.add_argument("--cli", required=True)
    sy.add_argument("--work", required=True)
    sy.set_defaults(fn=cmd_synthetic)
    au = sub.add_parser("audit")
    au.add_argument("--work", required=True)
    au.add_argument("--out", required=True)
    au.add_argument("--only", nargs="*")
    au.set_defaults(fn=cmd_audit)
    ga = sub.add_parser("ghidra-audit")
    ga.add_argument("--work", required=True)
    ga.add_argument("--out", required=True)
    ga.set_defaults(fn=cmd_ghidra_audit)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
