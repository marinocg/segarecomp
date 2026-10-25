#!/usr/bin/env python3
"""SEG-085-T002 (ADR 0192): qualification tooling for the historically representative evaluation cohort (OFFLINE, PRIVATE research code).

The cohort is evaluation-only. This module builds per-byte class strings (I instruction start / c continuation / D known data / U unknown) for candidate images from
public reconstructions, verifies them with checks that do not use the model, and writes the private registry layout that `segarecomp_ml_v3_train.Image` reads.
Durable output is identities, digests and aggregate counts only.

Evidence tiers (ADR 0190): a byte-identical rebuild is necessary but is NOT boundary corroboration. Every accepted image additionally passes (1) the CPU-owned
decoder length agreement at every labelled start, (2) no overlap of labelled code and data, and (3) the direct-control-target audit; contradicted bytes become
unknown. Unexecuted or undisassembled bytes are never negative.

Commands:
  gas-truth      per-byte classes of a GNU-assembler reconstruction (marker-label re-assembly; Toejam & Earl)
  corroborate    decoder audit, direct-target audit and containment of a private registry entry (no model)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

SCHEMA = "segarecomp.seg085.cohort.v1"
MARK = "__s85m_"
HARNESS = pathlib.Path(os.environ.get("SEGARECOMP_HARNESS_ROOT", "/root/dev/segarecomp-harness"))
DATA_ROOT = pathlib.Path(os.environ.get("SEG085_DATA", str(HARNESS / ".cache" / "seg085" / "data")))
MNEMONICS = frozenset("""
abcd add adda addi addq addx and andi asl asr bcc bcs beq bge bgt bhi bhs ble blo bls blt bmi bne bpl bvc bvs bra bsr bchg bclr bset btst
chk clr cmp cmpa cmpi cmpm dbcc dbcs dbeq dbf dbge dbgt dbhi dble dbls dblt dbmi dbne dbpl dbra dbt dbvc dbvs dbhs dblo divs divu eor eori
exg ext illegal jmp jsr lea link lsl lsr move movea movem movep moveq muls mulu nbcd neg negx nop not or ori pea reset rol ror roxl roxr rte
rtr rts sbcd scc scs seq sf sge sgt shi sle sls slt smi sne spl st svc svs shs slo stop sub suba subi subq subx swap tas trap trapv tst unlk
jra jbra jbsr jcc jcs jeq jge jgt jhi jhs jle jlo jls jlt jmi jne jpl jvc jvs jbeq jbne movm
""".split())
DATA_FIRST = re.compile(r"^(\.(byte|word|short|long|int|ascii|asciz|string|skip|space|zero|fill|align|balign|p2align|even|incbin|2byte|4byte|dc|ds|dcb)(\.[bwl])?|dc\.[bwl]|ds\.[bwl])$")
NONEMIT = frozenset(""".globl .global .local .hidden .type .size .file .ident .section .text .data .bss .previous .pushsection .popsection .set .equ .equiv .weak .comm .lcomm
.if .ifdef .ifndef .else .endif .elseif .extern .list .nolist .title .org .end .include .macro .endm .exitm .altmacro .noaltmacro .purgem""".split())


# Macros whose body is exactly a counted run of one instruction, expanded in the INSTRUMENTED copy only (the relinked image must still equal the authorized one).
EXPANSIONS = {"nops": "NOP"}


class CohortError(Exception):
    pass


def sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def strip_comments(text: str) -> str:
    """Remove /* ... */ comments outside strings (newlines kept)."""
    out, i, n, in_str, in_com = [], 0, len(text), False, False
    while i < n:
        ch = text[i]
        if in_com:
            if text.startswith("*/", i):
                in_com = False
                i += 2
                continue
            out.append("\n" if ch == "\n" else " ")
            i += 1
            continue
        if in_str:
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if ch in '"\n':
                in_str = False
            i += 1
            continue
        if ch == '"':
            in_str = True
        elif text.startswith("/*", i):
            in_com = True
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def split_statement(line: str):
    """(labels, first token, rest) of one gas statement; `|` comments and `#` lines removed; strings blanked."""
    text = line.rstrip("\r")
    if text.lstrip().startswith("#"):
        return [], "", ""
    text = re.sub(r'"(?:[^"\\]|\\.)*"', '""', text)
    text = text.split("|", 1)[0]
    labels = []
    while True:
        mo = re.match(r"^\s*([A-Za-z_.$][\w.$]*|\d+):(?!:)(.*)$", text)
        if not mo:
            break
        labels.append(mo.group(1))
        text = mo.group(2)
    toks = text.split()
    return labels, (toks[0] if toks else ""), " ".join(toks[1:])


def classify(first: str, macros: set) -> str:
    """I instruction, D data/padding, M macro call, N no bytes, X unknown (fails closed)."""
    f = first.lower()
    if not f:
        return "N"
    if f in macros:
        return "M"
    if DATA_FIRST.match(f):
        return "D"
    if f.startswith("."):
        return "N" if f in NONEMIT or f.startswith(".cfi_") else "X"
    return "I" if f.split(".")[0] in MNEMONICS else "X"


def instrument(text: str, prefix: str = MARK):
    """(instrumented text, rows). A marker label precedes every byte-emitting statement; macro definitions get none; a macro CALL is a failure unless its
    body was expanded away beforehand (the Toejam & Earl `nops` macro has no call sites). rows[k] = {kind, line}."""
    text = strip_comments(text)
    lines = text.split("\n")
    macros = set()
    for ln in lines:
        _, first, rest = split_statement(ln)
        if first.lower() == ".macro" and rest:
            macros.add(rest.split()[0].split(",")[0].lower())
    out, rows, depth = [], [], 0
    for i, ln in enumerate(lines):
        _, first, _rest = split_statement(ln)
        f = first.lower()
        if depth:
            if f == ".endm":
                depth -= 1
            elif f == ".macro":
                depth += 1
            out.append(ln)
            continue
        if f == ".macro":
            depth = 1
            out.append(ln)
            continue
        kind = classify(first, macros)
        if kind == "M" and f in EXPANSIONS:
            n = int(split_statement(ln)[2].split()[0], 0)                       # a literal repeat count only; anything else fails closed
            for _ in range(n):
                out.append(f"{prefix}{len(rows)}:")
                out.append(f" {EXPANSIONS[f]}")
                rows.append({"kind": "I", "line": i + 1})
            continue
        if kind in ("X", "M"):
            raise CohortError(f"unclassifiable statement at line {i + 1}: {kind}")
        if kind in ("I", "D"):
            out.append(f"{prefix}{len(rows)}:")
            rows.append({"kind": kind, "line": i + 1})
        out.append(ln)
    return "\n".join(out), rows


def objdump_contents(text: str) -> dict:
    secs, cur = {}, None
    for ln in text.split("\n"):
        mo = re.match(r"^Contents of section (\S+):", ln)
        if mo:
            cur = mo.group(1)
            secs[cur] = bytearray()
            continue
        mo = re.match(r"^ ([0-9a-f]{4,8}) ((?:[0-9a-f]{2,8} ){1,4})", ln)
        if mo and cur is not None:
            off = int(mo.group(1), 16)
            if off != len(secs[cur]):
                raise CohortError("objdump gap")
            secs[cur] += bytes.fromhex(mo.group(2).replace(" ", ""))
    return {k: bytes(v) for k, v in secs.items()}


def marker_values(sym_text: str, prefix: str) -> dict:
    """objdump -t of the LINKED image -> {marker index: final address} for one unit's markers."""
    out = {}
    for ln in sym_text.split("\n"):
        mo = re.match(r"^([0-9a-f]{8})\s.{7}\s(\S+)\s+[0-9a-f]{8}\s+" + re.escape(prefix) + r"(\d+)$", ln)
        if mo:
            if int(mo.group(3)) in out:
                raise CohortError("duplicate marker")
            out[int(mo.group(3))] = int(mo.group(1), 16)
    return out


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def link(objs, ld_script: pathlib.Path, out_elf: pathlib.Path, cwd: pathlib.Path) -> bytes:
    """Link unit objects with the project's linker script (same driver flags as the project makefile) and return the flat image."""
    run(["m68k-linux-gnu-gcc", "-nostdlib", "-ffreestanding", "-m68000", "-T", str(ld_script), "-Wl,--oformat", "-Wl,elf32-m68k", "-Wl,--build-id=none",
         *[str(o) for o in objs], "-o", str(out_elf)], cwd=str(cwd))
    flat = out_elf.with_suffix(".bin")
    run(["m68k-linux-gnu-objcopy", "-O", "binary", str(out_elf), str(flat)])
    return flat.read_bytes()


def reserved_sha256() -> dict:
    """Image SHA-256 -> reason for every identity this milestone must never use: the SEG-077 reserved blind titles, the SEG-072 sealed images (public manifest) and
    every development image. Fail-closed on a read error. Added after the first candidate (a SEG-077 reserved title) was examined before this check existed."""
    out = {}
    src = (HERE / "segarecomp_seg077.py").read_text()
    for mo in re.finditer(r'"(B077-\d+)": \{"title": "[^"]*", "raw_sha256": "([0-9a-f]{64})"', src):
        out[mo.group(2)] = f"SEG-077 reserved blind cohort ({mo.group(1)})"
    man = json.loads((HERE / "segarecomp_ml_v3.corpus_manifest.json").read_text())
    for e in man["sealed"]["entries"]:
        if "image_sha256" in e:
            out[e["image_sha256"]] = "SEG-072 sealed cohort"
    for droot in (HARNESS / ".cache" / "seg072" / "data",):
        for m in droot.rglob("meta.json"):
            mm = json.loads(m.read_text())
            if "sha256" in mm:
                out[mm["sha256"]] = "SEG-072 development image"
    if len([1 for v in out.values() if "reserved" in v]) != 8:
        raise CohortError("reserved list unreadable: refusing")
    return out


def refuse_reserved(sha: str) -> None:
    r = reserved_sha256()
    if sha in r:
        raise CohortError(f"reserved or development identity refused: {r[sha]}")


def cmd_gas_truth(a) -> int:
    """Toejam & Earl style: preprocessed `.s` units assembled with `as`, linked in the project's order with its linker script; the linked image (plain AND
    instrumented) must equal the authorized image byte for byte, so every marker address is an exact post-relaxation, post-link address."""
    work = pathlib.Path(a.work).resolve()
    rom = pathlib.Path(a.rom).read_bytes()
    if sha256_bytes(rom) != a.sha256:
        raise CohortError("image identity mismatch")
    refuse_reserved(a.sha256)
    flags = ["-m68000", "--bitwise-or", "--register-prefix-optional"]
    names = a.units.split(",")
    d = work / "x"
    plain_objs, marked_objs, all_rows = [], [], {}
    for n in names:
        src = d / f"{n}.s"
        prefix = f"{MARK}{n}_"
        inst, rows = instrument(src.read_text(encoding="latin1"), prefix)
        (d / f"{n}.m.s").write_text(inst, encoding="latin1")
        run(["m68k-linux-gnu-as", *flags, "-o", str(d / f"{n}.o"), str(src)])
        run(["m68k-linux-gnu-as", *flags, "-o", str(d / f"{n}.m.o"), str(d / f"{n}.m.s")])
        plain_objs.append(d / f"{n}.o")
        marked_objs.append(d / f"{n}.m.o")
        all_rows[n] = rows
    ld = work / a.ld
    img_plain = link(plain_objs, ld, d / "plain.elf", work)
    img_marked = link(marked_objs, ld, d / "marked.elf", work)
    if img_plain != rom:
        raise CohortError("plain relink differs from the authorized image")
    if img_marked != rom:
        raise CohortError("instrumented relink differs from the authorized image")
    syms = run(["m68k-linux-gnu-objdump", "-t", str(d / "marked.elf")])
    entries = []
    for n in names:
        marks = marker_values(syms, f"{MARK}{n}_")
        for k, r in enumerate(all_rows[n]):
            if k not in marks:
                raise CohortError(f"{n}: marker {k} missing")
            entries.append((marks[k], r["kind"]))
    entries.sort()
    cls = bytearray(len(rom))
    total = {"I": 0, "D": 0}
    for i, (addr, kind) in enumerate(entries):
        end = entries[i + 1][0] if i + 1 < len(entries) else len(rom)
        if end < addr or end > len(rom):
            raise CohortError("marker order")
        if end == addr:
            continue
        if any(cls[x] for x in range(addr, end)):
            raise CohortError("overlapping rows")
        if kind == "I":
            cls[addr] = ord("I")
            for x in range(addr + 1, end):
                cls[x] = ord("c")
        else:
            for x in range(addr, end):
                cls[x] = ord("D")
        total[kind] += 1
    unassigned = sum(1 for x in cls if not x)
    out = {"schema": SCHEMA + ".gas_truth", "size": len(rom), "rows": total, "unassigned_bytes": unassigned, "image_sha256": a.sha256, "markers": len(entries)}
    blob = bytes(cls).replace(b"\x00", b"U")
    pathlib.Path(a.out).write_bytes(blob)
    out["classes_sha256"] = sha256_bytes(blob)
    print(json.dumps(out, sort_keys=True))
    return 0


def target_contradictions(classes: bytes, target, nwords: int) -> list:
    """(source start, target byte, kind) for every labelled start whose direct control-flow target lands on labelled data ('data') or inside a labelled
    instruction ('inside'). Targets on labelled starts or unknown bytes, and targets outside the image, are not evidence either way. Pure."""
    out = []
    for a in (i for i, b in enumerate(classes) if b == ord("I")):
        t = int(target[a // 2])
        if t < 0 or t >= nwords:
            continue
        b = classes[t * 2:t * 2 + 1]
        if b == b"D":
            out.append((a, t * 2, "data"))
        elif b == b"c":
            out.append((a, t * 2, "inside"))
    return out


def mask_cells(classes: bytes, byte_offsets, cell: int = 128) -> tuple[bytes, int]:
    """Set every byte of each 128-byte cell holding one of the offsets to unknown (U). Returns (classes, masked cell count). Pure."""
    cells = sorted({o // cell for o in byte_offsets})
    out = bytearray(classes)
    for c in cells:
        for x in range(c * cell, min((c + 1) * cell, len(out))):
            out[x] = ord("U")
    i = 0
    while i < len(classes):                              # an instruction straddling a masked cell is unknown as a whole, never a truncated start
        if classes[i] == ord("I"):
            j = i + 1
            while j < len(classes) and classes[j] == ord("c"):
                j += 1
            if any(out[x] == ord("U") for x in range(i, j)):
                for x in range(i, j):
                    out[x] = ord("U")
            i = j
        else:
            i += 1
    return bytes(out), len(cells)


def containment(entry_img: bytes, entry_cls: bytes, others: dict) -> dict:
    """12-byte shingle containment of the labelled-code runs (ADR 0152): fraction of the entry's shingles present in each other lineage and vice versa."""
    import segarecomp_ml_v3_data as d3
    mine = d3.shingles(entry_img, entry_cls)
    res = {}
    for name, (img, cls) in others.items():
        theirs = d3.shingles(img, cls)
        inter = len(mine & theirs)
        res[name] = {"entry_in_other": round(inter / max(1, len(mine)), 6), "other_in_entry": round(inter / max(1, len(theirs)), 6)}
    return res


def cmd_corroborate(a) -> int:
    import numpy as np
    import segarecomp_ml_v3_train as tr
    import segarecomp_xplat_models as xm
    import segarecomp_xplat_t005 as x5
    d = pathlib.Path(a.entry).resolve()
    im = tr.Image(d)
    im = x5.attach(im, d)
    st = xm.structure(im)
    meta = json.loads((d / "meta.json").read_text())
    refuse_reserved(meta["sha256"])
    contra = target_contradictions(im.classes, st["target"], im.n_words)
    offsets = [o for s_, t_, _k in contra for o in (s_, t_)]
    masked, ncells = mask_cells(im.classes, offsets)
    (d / "classes.corroborated.bin").write_bytes(masked)
    # independence against the development registry (lineage-level union of labelled code shingles)
    others = {}
    droot = pathlib.Path(a.dev_root)
    for m in sorted(droot.rglob("meta.json")):
        mm = json.loads(m.read_text())
        if "classes_sha256" not in mm or mm.get("kind") != "genesis_rom":
            continue
        L = mm["lineage"]
        img, cls = pathlib.Path(mm["image"]).read_bytes(), (m.parent / "classes.bin").read_bytes()
        others.setdefault(L, [bytearray(), bytearray()])
        others[L][0] += img
        others[L][1] += cls
    ind = containment(im.rom, im.classes, {k: (bytes(v[0]), bytes(v[1])) for k, v in others.items()})
    worst = max(((max(v.values()), k) for k, v in ind.items()), default=(0.0, None))
    n_i = im.classes.count(b"I")
    out = {"schema": SCHEMA + ".corroboration", "id": im.id, "lineage": im.lineage, "image_sha256": meta["sha256"], "size": im.size,
           "decoder_audit": meta["audit"], "starts": n_i,
           "target_audit": {"direct_targets": sum(1 for a_ in (i for i, b in enumerate(im.classes) if b == ord("I")) if 0 <= int(st["target"][a_ // 2]) < im.n_words),
                            "contradictions": len(contra), "on_data": sum(1 for c in contra if c[2] == "data"), "inside_instruction": sum(1 for c in contra if c[2] == "inside"),
                            "masked_cells": ncells},
           "classes_sha256_raw": meta["classes_sha256"], "classes_sha256_corroborated": sha256_bytes(masked),
           "bytes": {k: masked.count(k.encode()) for k in "IcDU"},
           "independence": {"max_containment": worst[0], "with_lineage": worst[1], "threshold": 0.30, "independent": worst[0] < 0.30, "by_lineage": ind},
           "sealed_independence": "not measured: the sealed store is never opened; a titled disclosure is carried in the cohort record"}
    pathlib.Path(a.out).write_text(json.dumps(out, indent=1, sort_keys=True))
    print(json.dumps({k: out[k] for k in ("id", "starts", "target_audit", "decoder_audit")}, sort_keys=True), "independence max", worst)
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gas-truth")
    g.add_argument("--work", required=True)
    g.add_argument("--rom", required=True)
    g.add_argument("--sha256", required=True)
    g.add_argument("--units", required=True, help="comma-separated unit names in link order (preprocessed work/x/<name>.s)")
    g.add_argument("--ld", required=True, help="linker script relative to --work")
    g.add_argument("--out", required=True)
    g.set_defaults(func=cmd_gas_truth)
    c = sub.add_parser("corroborate")
    c.add_argument("--entry", required=True, help="private registry entry directory (meta.json, classes.bin, tokens.bin, seeds.json)")
    c.add_argument("--dev-root", default=str(HARNESS / ".cache" / "seg072" / "data"))
    c.add_argument("--out", required=True)
    c.set_defaults(func=cmd_corroborate)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    raise SystemExit(main())
