#!/usr/bin/env python3
"""SEG-084-T001: bounded diagnostics of the two SEG-083 games that failed a frozen gate, from existing private artifacts only (no replay, no new trace).

  shoveit   decompose the progression-gate failure: replay fidelity versus continued code discovery (growth windows, checkpoint states, determinism)
  nhl94     classify the 204 executed starts the independent disassembler did not decode (E_DISASM_NO_DECODE): disassembler limitation / instruction-form
            mismatch / mapping issue / unsound execution label / unresolved, using the CPU decoder token report, the emulator-observed successor of each
            executed PC, and ONE narrow re-run of the pinned m68k-disasm image on the same executed PCs
  gates     the proposed future separation of replay fidelity, continued code discovery, label confidence and acquisition yield (a pure function applied to
            the five SEG-083 results; it does not amend ADR 0187 and admits nothing retroactively)

Output is aggregate (counts and closed-vocabulary classes); no address, opcode word or title is written.
"""
from __future__ import annotations

import argparse
import collections
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
GROWTH_EVERY = 1800
FIFTH_MIN_FRACTION = 0.005
TOKEN_FIELDS = 8
SUCCESSOR_MATCH_MIN = 0.95
UNSOUND_MAX = 0.05
CLASSES = ("DISASSEMBLER_LIMITATION", "INSTRUCTION_FORM_MISMATCH", "ROM_MAPPING_ISSUE", "UNSOUND_EXECUTION_LABELS", "UNRESOLVED")


# --------------------------------------------------------------------------------------------------------------------- Shove It!
def fifth_distribution(growth: dict, frames: int) -> dict:
    """New executed PCs per fifth of movie time (growth windows of 1800 frames, the frozen gate's own bucketing)."""
    per = [0] * 5
    for k, v in growth.items():
        per[min(4, (int(k) * GROWTH_EVERY) * 5 // max(1, frames))] += v
    total = sum(per)
    return {"per_fifth": per, "total": total, "fractions": [round(x / total, 5) if total else 0.0 for x in per],
            "fifths_ge_min": sum(1 for x in per if total and x >= FIFTH_MIN_FRACTION * total),
            "first_window_fraction": round(growth.get("0", 0) / total, 5) if total else None,
            "frames_to_first_window_end": GROWTH_EVERY, "windows_with_new_code": len(growth)}


def replay_fidelity(res: dict) -> dict:
    """The fidelity-only part of the SEG-081/083 replay gate (everything except continued code discovery)."""
    g = res["gate_replay"]
    det = g.get("determinism", {})
    return {"identity_ok": g["identity_ok"], "played_to_end": g["played_to_end"], "no_perturbation": g["no_perturbation"],
            "deterministic": bool(det) and all(det.values()), "distinct_states": g["distinct_ram_vram_states"], "states_ok": g["distinct_ram_vram_states"] >= 5,
            "domains_complete": not g["digest_domains_missing"]}


def proposed_gates(res: dict) -> dict:
    """Four independent verdicts (pure). Fidelity, label confidence and yield no longer depend on how many movie fifths discover new code."""
    f = replay_fidelity(res)
    fidelity = all(f[k] for k in ("identity_ok", "played_to_end", "no_perturbation", "deterministic", "states_ok", "domains_complete"))
    t = res["gate_tracing"]["tracing_ok"] if "gate_tracing" in res else False
    c = res["consistency"]["ok"] if "consistency" in res else False
    label = bool(fidelity and t and c)
    cls = res["classification"]["class"]
    discovery = res["gate_replay"].get("fifths_with_new_code")
    return {"replay_fidelity": fidelity, "continued_discovery_fifths": discovery, "label_confidence": label,
            "acquisition_yield": cls if cls.startswith("QUALIFIED") else ("QUALIFIED_LOW_YIELD" if label else None),
            "original_class": cls,
            "proposed_class": ("QUALIFIED_USEFUL" if label and res["classification"].get("code_yield_rule") and res["classification"].get("novelty_rule")
                               else "QUALIFIED_LOW_YIELD" if label else
                               "REPLAY_INVALID" if not fidelity else "TRACING_UNAVAILABLE" if not t else "LABEL_POLICY_CONFLICT")}


# ------------------------------------------------------------------------------------------------------------------------ NHL '94
def raw_kinds(text: str, pcs: set) -> dict:
    """Per executed PC: how m68k-disasm printed the address ('decoded', 'short' raw data, or 'absent')."""
    sys.path.insert(0, str(HERE))
    import segarecomp_genesis_evidence as ge
    out = {p: "absent" for p in pcs}
    for ln in text.splitlines():
        m = ge.DIS_LINE.match(ln)
        if not m:
            continue
        a = int(m.group(3), 16)
        if a in out:
            out[a] = "short" if m.group(1).split()[0].startswith(".") else "decoded"
    return out


def classify_pc(pc: int, size: int, tokens: bytes, succ: set, accepted_spans: list, kinds: tuple) -> dict:
    w = pc // 2
    status = tokens[w * TOKEN_FIELDS] if (w + 1) * TOKEN_FIELDS <= len(tokens) else None
    length = tokens[w * TOKEN_FIELDS + 1] * 2 if status == 2 else 0
    control = tokens[w * TOKEN_FIELDS + 3] if status == 2 else None
    inside = any(s < pc < s + n for s, n in accepted_spans)
    straight = control in (0, None)             # token control 0 = no control transfer
    return {"outside_image": pc >= size, "header_or_vector": pc < 0x200, "cpu_ordinary": status == 2, "cpu_length": length, "inside_other_instruction": inside,
            "successor_matches_length": bool(length and straight and (pc + length) in succ), "has_successor": bool(succ), "straight_line": straight,
            "disasm": kinds[0] if kinds[0] == kinds[1] else "mixed"}


def classify_rejections(rows: list) -> dict:
    """Closed-vocabulary classification of the rejected executed starts (pure)."""
    n = len(rows)
    if not n:
        return {"class": "UNRESOLVED", "reason": "no rejected starts", "counts": {}}
    c = collections.Counter()
    for r in rows:
        if r["outside_image"] or r["header_or_vector"]:
            c["mapping"] += 1
        elif not r["cpu_ordinary"]:
            c["cpu_not_ordinary"] += 1
        elif r["inside_other_instruction"]:
            c["inside_other"] += 1
        elif r["straight_line"] and r["has_successor"] and not r["successor_matches_length"]:
            c["successor_contradicts"] += 1
        elif r["straight_line"] and r["successor_matches_length"]:
            c["corroborated"] += 1
        else:
            c["unverifiable"] += 1
    counts = dict(c)
    checkable = c["corroborated"] + c["successor_contradicts"] + c["inside_other"]
    out = {"counts": counts, "rejected": n, "disasm_kinds": dict(collections.Counter(r["disasm"] for r in rows))}
    if c["mapping"]:
        out.update({"class": "ROM_MAPPING_ISSUE", "reason": f"{c['mapping']} rejected PCs lie outside the image or in the header/vector area"})
    elif (c["successor_contradicts"] + c["inside_other"]) / n > UNSOUND_MAX:
        out.update({"class": "UNSOUND_EXECUTION_LABELS", "reason": "the emulator-observed successor or overlap contradicts the CPU decoder length for more than 5 % of the rejected starts"})
    elif c["cpu_not_ordinary"] / n > UNSOUND_MAX:
        out.update({"class": "INSTRUCTION_FORM_MISMATCH", "reason": "the CPU decoder itself reports a non-ordinary decode for more than 5 % of the rejected starts"})
    elif checkable and c["corroborated"] / n >= SUCCESSOR_MATCH_MIN and all(r["disasm"] in ("short", "absent") for r in rows):
        out.update({"class": "DISASSEMBLER_LIMITATION", "reason": "the CPU decoder is ordinary, the emulator-observed successor equals pc + decoded length for >= 95 % of the "
                                                                     "rejected starts, and m68k-disasm printed raw data or nothing there"})
    else:
        out.update({"class": "UNRESOLVED", "reason": "the bounded evidence neither corroborates nor contradicts the CPU decoder for enough of the rejected starts"})
    return out


def cmd_nhl(a) -> int:
    sys.path.insert(0, str(HERE))
    import segarecomp_genesis_evidence as ge
    priv = pathlib.Path(a.private)
    img = pathlib.Path(a.rom).read_bytes()
    lab = priv / "labels" / "tas"
    run = priv / "runs" / "tas_a"
    tokens = (lab / "tokens.bin").read_bytes()
    executed = sorted(int(p) for p in json.loads((run / "pcs.json").read_text())["rom"])
    edges = [tuple(e) for e in json.loads((run / "edges.json").read_text())]
    succ = collections.defaultdict(set)
    for p, q in edges:
        succ[p].add(q)
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        w = pathlib.Path(d)
        texts = {}
        for mode, flags in (("traced", ["-frdc"]), ("walk", ["-frdc", "-fwalk", "-ffollow-jumps"])):
            tf = w / f"pcs_{mode}.txt"
            tf.write_text("".join(f"{p}\n" for p in executed))
            tf.chmod(0o644)
            ip = w / "rom.bin"
            ip.write_bytes(img)
            ip.chmod(0o644)
            r = subprocess.run(["docker", "run", "--rm", "--network", "none", "--user", "1000:1000", "-v", f"{ip}:/in/rom.bin:ro", "-v", f"{tf}:/in/pcs.txt:ro", ge.EVIDENCE_IMAGE,
                                "/opt/m68k-disasm/build/m68k-disasm", *flags, "-t", "/in/pcs.txt", "/in/rom.bin"], capture_output=True, text=True, timeout=3600)
            if r.returncode:
                raise SystemExit(f"m68k-disasm failed: {r.stderr[-200:]}")
            texts[mode] = r.stdout
    traced, walk = ge.parse_disasm(texts["traced"]), ge.parse_disasm(texts["walk"])
    rejected, accepted_spans = [], []
    for pc in executed:
        if pc & 1 or pc >= len(img):
            continue
        t = walk.get(pc) or traced.get(pc)
        tl = ge.token_len(tokens, pc)
        if t is None:
            rejected.append(pc)
        elif tl is not None and t[0] == tl:
            accepted_spans.append((pc, tl))
    accepted_spans.sort()
    kt, kw = raw_kinds(texts["traced"], set(rejected)), raw_kinds(texts["walk"], set(rejected))
    rows = [classify_pc(pc, len(img), tokens, succ.get(pc, set()), accepted_spans, (kt[pc], kw[pc])) for pc in rejected]
    res = classify_rejections(rows)
    res["executed_rom_pcs"] = len(executed)
    res["rejected_starts"] = len(rejected)
    res["accepted_e_fraction_recomputed"] = round(1 - len(rejected) / max(1, len(executed)), 5)
    res["opcode_line_histogram"] = dict(sorted(collections.Counter(f"{int.from_bytes(img[p:p + 2], 'big') >> 12:X}" for p in rejected).items()))
    res["contiguous_runs"] = sum(1 for i, p in enumerate(rejected) if i == 0 or p - rejected[i - 1] > 10)
    pathlib.Path(a.out).write_text(json.dumps(res, indent=1, sort_keys=True))
    print(json.dumps({k: res[k] for k in ("class", "rejected_starts", "counts", "disasm_kinds") if k in res}, sort_keys=True))
    return 0


def cmd_shoveit(a) -> int:
    res = json.loads(pathlib.Path(a.result).read_text())
    run = pathlib.Path(a.private) / "runs" / "tas_a"
    tr = json.loads((run / "trace.json").read_text())
    out = {"fifths": fifth_distribution(tr["new_pcs_per_growth_window"], tr["frames"]), "fidelity": replay_fidelity(res), "original_class": res["classification"]["class"],
           "movie_finished_at_frame": tr["movie_finished_at"], "frames": tr["frames"], "executed_rom_pcs": tr["rom_pcs"], "map_mismatch": tr["map_mismatch"],
           "proposed": proposed_gates(res)}
    pathlib.Path(a.out).write_text(json.dumps(out, indent=1, sort_keys=True))
    print(json.dumps({"fifths": out["fifths"]["per_fifth"], "fidelity": all(out["fidelity"][k] for k in ("identity_ok", "played_to_end", "no_perturbation", "deterministic", "states_ok")),
                      "proposed": out["proposed"]["proposed_class"]}))
    return 0


def cmd_gates(a) -> int:
    out = {}
    for item in a.result:
        g, p = item.split("=", 1)
        out[g] = proposed_gates(json.loads(pathlib.Path(p).read_text()))
    pathlib.Path(a.out).write_text(json.dumps(out, indent=1, sort_keys=True))
    print(json.dumps({g: v["proposed_class"] for g, v in out.items()}, sort_keys=True))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("shoveit")
    p.add_argument("--private", required=True), p.add_argument("--result", required=True), p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_shoveit)
    p = sub.add_parser("nhl94")
    p.add_argument("--private", required=True), p.add_argument("--rom", required=True), p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_nhl)
    p = sub.add_parser("gates")
    p.add_argument("--result", nargs="+", required=True, help="game=path"), p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_gates)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
