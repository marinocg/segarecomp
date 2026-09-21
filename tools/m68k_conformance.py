#!/usr/bin/env python3
"""SEG-021-T003: table-driven generated-native differential conformance harness for base MC68000 forms.

One reusable harness. A family is validated by adding ROWS to ``tests/fixtures/m68k-conformance-vectors.json``;
no new runner, emitter driver or oracle is written. Every row names a legal form of the independent T001
baseline (``tests/fixtures/m68k-legal-forms.json``); the concrete primary words come from that form's
``word_ranges`` (this tool holds no legality knowledge). For every word the row's vector profile is expanded
into deterministic synthetic vectors (zero, negative, carry/borrow, signed overflow, X/C interaction,
boundary values, register aliasing, A7 byte auto-update, ...) and pushed through

    decode -> lift -> emitted strict-C11 -> compile -> generated-native execute      (one binary)
    pinned Musashi, one instruction from the identical memory image                   (one binary)

then compared with the SEG-020 machinery (``tools/m68k_first_divergence.py``: boundary schema,
``compare_streams``/``field_differences``) over D0-D7, A0-A7 (A7 = SSP), PC, SR/CCR, USP, byte-granular memory
writes (which cover auto-updated EA memory and exception stack frames) and the exception-vector entry hook.
Hooks for exception tasks: a k=2 effect carries the vector number when the final PC is a vector handler, and
the stacked frame is part of the memory-write comparison.

Without a pinned Musashi checkout the oracle comparison is SKIPPED (never failed); the synthetic
self-consistency checks (every word emits, compiles strictly, runs, is deterministic) still run.

Measured limits (honest): vectors run in supervisor mode; a write of a value equal to the previous byte is
invisible; timing is not compared; operand binding covers register-direct, (An), (An)+, -(An) EAs
(``bind_operand``) and grows by adding cases there, not by adding runners.

    python3 tools/m68k_conformance.py --emitter build/dev/tests/m68k_conformance_emitter --cc cc \
        [--musashi-checkout .tools/musashi] [--rows id,...] [--report OUT.json] [--update-manifest]
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import m68k_first_divergence as fd  # noqa: E402  (SEG-020 boundary schema + comparison + pinned oracle build)

TABLE = ROOT / "tests" / "fixtures" / "m68k-conformance-vectors.json"
FORMS = ROOT / "tests" / "fixtures" / "m68k-legal-forms.json"
MANIFEST = ROOT / "tests" / "fixtures" / "m68k-validation-manifest.json"
TOOLS = ROOT / "tests" / "tools"
SOURCE_TAG = "tests/fixtures/m68k-conformance-vectors.json"
CHECKOUT_ENVS = ("SEGARECOMP_M68K_CONFORMANCE_MUSASHI_CHECKOUT", "SEGARECOMP_M68K_MULS_WORD_MUSASHI_CHECKOUT",
                 "SEGARECOMP_M68K_BATCH_B_MUSASHI_CHECKOUT")
CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-Wno-type-limits", "-pedantic", "-Werror", "-O0"]
CODE_BASE = 0x2000
SIZES = {"b": 1, "w": 2, "l": 4}
SCHEMA = 1
# Base-MC68000 effective-address classes of the T001 denominator: (mode, register-or-None, extension bytes).
EA_CLASSES = {"dn": (0, None, 0), "an": (1, None, 0), "ind": (2, None, 0), "postinc": (3, None, 0),
              "predec": (4, None, 0), "disp": (5, None, 2), "index": (6, None, 2), "absw": (7, 0, 2),
              "absl": (7, 1, 4), "pcdisp": (7, 2, 2), "pcindex": (7, 3, 2), "imm": (7, 4, None)}
BASE_USP = 0x1A000
BASE_SSP = 0x17000
WINDOW = 0x100000


# ---------------------------------------------------------------------------------------------
# table loading and vector expansion


def load_table(path: pathlib.Path = TABLE) -> dict:
    table = json.loads(path.read_text(encoding="utf-8"))
    if table.get("schema") != SCHEMA or table.get("musashi_revision") != fd.MUSASHI_PIN:
        raise ValueError("conformance table schema/pin mismatch")
    return table


def load_forms() -> dict:
    data = json.loads(FORMS.read_text(encoding="utf-8"))
    return {row[0]: dict(zip(data["form_columns"], row)) for row in data["forms"]}


def form_words(form: dict) -> list[int]:
    return [w for lo, hi in form["word_ranges"] for w in range(lo, hi + 1)]


def base_d(i: int) -> int:
    return (0x9E3779B1 * (i + 1)) & 0xFFFFFFFF


def base_a(i: int) -> int:
    return 0x10000 + i * 0x1000


def sext(value: int, bits: int) -> int:
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def initial_state(row: dict, sr: int) -> dict:
    """One common state representation: registers, explicit USP and SSP, active A7, memory seeds."""
    state = {"d": [base_d(i) for i in range(8)], "a": [base_a(i) for i in range(8)], "mem": [],
             "usp": BASE_USP, "ssp": BASE_SSP, "sr": sr}
    for name, value in row.get("init", {}).items():  # table-data register presets, e.g. an index register value
        (state["d"] if name[0] == "d" else state["a"])[int(name[1])] = int(value, 16)
    return state


def sync_stacks(state: dict) -> None:
    """A7 is the active stack pointer: SSP in supervisor state, USP in user state."""
    state["a"][7] = state["ssp"] if state["sr"] & 0x2000 else state["usp"]


def ea_ext_bytes(cls: str, size: int, override: int | None) -> int:
    length = EA_CLASSES[cls][2]
    return (override if override is not None else (4 if size == 4 else 2)) if length is None else length


def ext_offset(spec: str, row: dict, form: dict) -> int:
    """Byte offset of this EA's extension words inside the row's extension suffix: the extension words of the
    source operand class (if it is an EA class) precede those of the destination operand class."""
    size = SIZES.get(form["size"], 2)
    if spec.endswith(".src"):
        return 0
    src = form["src"]
    return ea_ext_bytes(src, size, row.get("src_ext_bytes")) if src in EA_CLASSES else 0


def bind_operand(spec: str, word: int, ext: bytes, row: dict, form: dict, state: dict, value: int, phase: int):
    """Place ``value`` into the state through operand ``spec``. Phase 1 assigns registers, phase 2 memory (so an
    index register bound by another operand is final before addresses are computed). Returns the alias key.

    Operand specs (table data; the binder holds no per-family opcode logic):
      d@S / a@S     data / address register selected by opcode bits S..S+2 (full 32-bit register value)
      ea.src/ea.dst the EA in opcode bits 0..5 (mode 3..5, register 0..2); class is form[src|dst], extension
                    words come from the row's suffix; Dn/An take ``value`` as register, memory classes place
                    ``size`` bytes of ``value`` at the effective address; immediate is data in the suffix
      eaM.dst       the MOVE-style destination EA (register bits 9..11, mode bits 6..8), same classes
      pi@S / pd@S   (An)+ / -(An) operand with An in opcode bits S..S+2 (two-auto-update shapes: CMPM, ADDX/SUBX)
    """
    size = SIZES.get(form["size"], 2)
    if spec.startswith(("d@", "a@")):
        reg = (word >> int(spec[2:])) & 7
        if phase == 1:
            state["d" if spec[0] == "d" else "a"][reg] = value & 0xFFFFFFFF
            if spec[0] == "a":
                state["a"][reg] &= 0xFFFFFFFF
        return (spec[0], reg)
    if spec.startswith(("pi@", "pd@")):
        reg = (word >> int(spec[3:])) & 7
        if phase == 2:
            step = 2 if (reg == 7 and size == 1) else size
            address = state["a"][reg] - (step if spec[1] == "d" else 0)
            put_memory(state, address, value, size, row)
        return ("a", reg)
    kind, _, role = spec.partition(".")
    if kind not in ("ea", "eaM") or role not in ("src", "dst"):
        raise ValueError("row %s: unsupported operand spec %r" % (row["id"], spec))
    cls = form[role]
    if cls not in EA_CLASSES:
        raise ValueError("row %s: form operand %r is not an EA class" % (row["id"], cls))
    mode, regfield, _ = EA_CLASSES[cls]
    got_mode, got_reg = ((word >> 3) & 7, word & 7) if kind == "ea" else ((word >> 6) & 7, (word >> 9) & 7)
    if got_mode != mode or (regfield is not None and got_reg != regfield):
        raise ValueError("row %s: EA class %r does not match word %04X" % (row["id"], cls, word))
    reg = got_reg if regfield is None else None
    if cls in ("dn", "an"):
        if phase == 1:
            state["d" if cls == "dn" else "a"][reg] = value & 0xFFFFFFFF
        return ("d" if cls == "dn" else "a", reg)
    if cls == "imm":
        return ("imm", 0)  # the operand value is literal suffix data
    if phase == 1:
        return ("a", reg) if reg is not None and cls in ("ind", "postinc", "predec", "disp", "index") else ("m", cls)
    offset = ext_offset(spec, row, form) if role == "dst" else 0
    need = EA_CLASSES[cls][2]
    if need and len(ext) < offset + need:
        raise ValueError("row %s: extension suffix too short for %s at byte %d" % (row["id"], cls, offset))
    if cls in ("ind", "postinc", "predec"):
        step = 2 if (reg == 7 and size == 1) else size
        address = state["a"][reg] - (step if cls == "predec" else 0)
    elif cls == "disp":
        address = state["a"][reg] + sext(int.from_bytes(ext[offset:offset + 2], "big"), 16)
    elif cls == "absw":
        address = sext(int.from_bytes(ext[offset:offset + 2], "big"), 16)
    elif cls == "absl":
        address = int.from_bytes(ext[offset:offset + 4], "big")
    else:  # index / pcindex: brief extension word; pcdisp / pcindex are relative to the extension word address
        word_ext = int.from_bytes(ext[offset:offset + 2], "big")
        if cls == "pcdisp":
            address = CODE_BASE + 2 + offset + sext(word_ext, 16)
        else:
            xn = (state["a"] if word_ext & 0x8000 else state["d"])[(word_ext >> 12) & 7]
            xn = xn if word_ext & 0x800 else sext(xn & 0xFFFF, 16)
            base = state["a"][reg] if cls == "index" else CODE_BASE + 2 + offset
            address = base + sext(word_ext & 0xFF, 8) + xn
    put_memory(state, address & 0xFFFFFFFF, value, size, row)
    return ("m", cls, address & 0xFFFFFFFF)


def put_memory(state: dict, address: int, value: int, size: int, row: dict) -> None:
    if not 0 <= address <= WINDOW - size:
        raise ValueError("row %s: operand address %X outside the conformance window" % (row["id"], address))
    state["mem"].append((address, (value & ((1 << (8 * size)) - 1)).to_bytes(size, "big")))


def profile_cases(profile: dict, aliased: bool, has_x: bool, needs_y: bool, table: dict):
    """(x, y, sr) cases. With no bound operand a profile is state-only: one case per SR seed."""
    if not has_x:
        pairs = [(0, 0)]
    else:
        values = table["values"][profile["values"]] if "values" in profile else None
        if profile["kind"] == "single":
            pairs = [(int(v, 16), 0) for v in values]
        elif profile["kind"] == "cross":
            pairs = [(int(x, 16), int(y, 16)) for x in values for y in values]
        elif profile["kind"] == "pair_list":
            pairs = [(int(x, 16), int(y, 16)) for x, y in profile["pairs"]]
        else:
            raise ValueError("unknown profile kind %r" % profile["kind"])
        if aliased and needs_y:  # x and y name the same register: only consistent x == y states exist
            pairs = sorted({(x, x) for x, _ in pairs})
    for sr in profile["sr"]:
        for x, y in pairs:
            yield x, y, int(sr, 16)


def expand_row(row: dict, table: dict, forms: dict) -> list[dict]:
    """Deterministic vectors of one row: dicts with id/word/code/sr/usp/ssp/d/a/mem and case metadata.

    Each tested encoding is the T001 primary word plus one literal extension suffix of ``row["ext"]`` (default:
    one empty suffix), so a row describes COMPLETE instructions. ``bind`` is optional (no-operand rows)."""
    form = forms[row["form"]]
    words = form_words(form)
    full = {int(w, 16) for w in row.get("full_words", [])}
    if not full <= set(words):
        raise ValueError("row %s: full_words outside form %s" % (row["id"], row["form"]))
    suffixes = [bytes.fromhex(x) for x in row.get("ext", [""])]
    bind = row.get("bind", {})
    if set(bind) - {"x", "y"}:
        raise ValueError("row %s: bind keys must be x/y" % row["id"])
    vectors = []
    for word in words:
        profile = table["profiles"][row["full_profile"] if word in full and "full_profile" in row else row["profile"]]
        for ext in suffixes:
            probe = initial_state(row, 0x2700)
            keys = [bind_operand(spec, word, ext, row, form, probe, 0, 1) for spec in bind.values()]
            aliased = len(keys) > 1 and len({k for k in keys if k[0] in "da"}) < len([k for k in keys if k[0] in "da"])
            for index, (x, y, sr) in enumerate(profile_cases(profile, aliased, "x" in bind, "y" in bind, table)):
                state = initial_state(row, sr)
                sync_stacks(state)
                for phase in (1, 2):
                    for role, spec in bind.items():
                        bind_operand(spec, word, ext, row, form, state, x if role == "x" else y, phase)
                vectors.append({"id": "%s:%04X:%d" % (row["id"], word, len(vectors)), "row": row["id"], "word": word,
                                "code": "%04X%s" % (word, ext.hex().upper()), "sr": sr, "usp": state["usp"],
                                "ssp": state["ssp"], "d": state["d"], "a": state["a"], "mem": state["mem"],
                                "x": x, "y": y})
    return vectors


def vector_line(v: dict) -> str:
    fields = [v["id"], v["code"], "%X" % v["sr"], "%X" % v["usp"], "%X" % v["ssp"]] + ["%X" % x for x in v["d"]] + \
        ["%X" % x for x in v["a"][:7]] + [str(len(v["mem"]))] + ["%X:%s" % (a, p.hex().upper()) for a, p in v["mem"]]
    return " ".join(fields)


def manifest_credit(row: dict, form: dict, table: dict) -> dict[str, bool]:
    """Aspects a row may credit in the T002 manifest, from what the vectors actually vary and compare."""
    import m68k_capability_coverage as cov
    profiles = {row["profile"], row["full_profile"]} if "full_profile" in row else {row["profile"]}
    srs = {sr for p in profiles for sr in table["profiles"][p]["sr"]}
    return {"semantic": True, "ccr": cov.ccr_expected(form) and len(srs) >= 2, "ea": cov.ea_effect_expected(form)}


# ---------------------------------------------------------------------------------------------
# generated-native side


def _run(args, **kwargs):
    return subprocess.run(args, text=True, capture_output=True, **kwargs)


def _remove_functions(source: str, codes: set[str]) -> str:
    out, skip = [], False
    for line in source.split("\n"):
        m = re.match(r"static int cf_([0-9A-F]+)\(cap_state", line)
        if m:
            skip = m.group(1) in codes
        elif line.startswith("typedef struct { const char *code"):
            skip = False
        if not skip:
            out.append(line)
    text = "\n".join(out)
    for code in codes:
        text = text.replace('  {"%s", cf_%s},\n' % (code, code), "")
    return text


def build_generated(codes: list[str], emitter: pathlib.Path, cc: str, work: pathlib.Path, mutate=None):
    """Emit, strictly compile and link the generated side. Returns (runner, status per code)."""
    (work / "codes.txt").write_text("".join(c + "\n" for c in codes))
    proc = _run([str(emitter), "--out", str(work / "conf.c")], input="".join(c + "\n" for c in codes))
    if proc.returncode != 0:
        raise RuntimeError("emitter failed: " + proc.stderr)
    status = dict(line.split() for line in proc.stdout.splitlines())
    source = (work / "conf.c").read_text()
    if mutate is not None:  # test-only fault injection into a temporary copy
        source = mutate(source)
    for _ in range(6):
        (work / "conf.c").write_text(source)
        compiled = _run([cc, *CFLAGS, "-c", str(work / "conf.c"), "-o", str(work / "conf.o")])
        if compiled.returncode == 0:
            break
        lines = source.split("\n")
        starts = [(i + 1, re.match(r"static int cf_([0-9A-F]+)\(", l).group(1)) for i, l in enumerate(lines)
                  if re.match(r"static int cf_([0-9A-F]+)\(", l)]
        bad = set()
        for m in re.finditer(r"conf\.c:(\d+):\d+: error", compiled.stderr):
            n = int(m.group(1))
            owners = [c for s, c in starts if s <= n]
            if owners:
                bad.add(owners[-1])
        if not bad:
            raise RuntimeError("generated C failed to compile and is not attributable: " + compiled.stderr[:2000])
        for c in bad:
            status[c] = "compile_failed"
        source = _remove_functions(source, bad)
    else:
        raise RuntimeError("generated C failed to compile after bisection")
    runner = work / "runner"
    linked = _run([cc, "-std=c11", "-O0", "-I", str(TOOLS), "-o", str(runner), str(TOOLS / "m68k_conformance_runner.c"),
                   str(work / "conf.o")])
    if linked.returncode != 0:
        raise RuntimeError("runner failed to build: " + linked.stderr[:2000])
    return runner, status


def musashi_checkout(explicit: pathlib.Path | None) -> pathlib.Path | None:
    if explicit is not None:
        return explicit
    for name in CHECKOUT_ENVS:
        if os.environ.get(name):
            return pathlib.Path(os.environ[name])
    return None


def parse_records(text: str) -> dict[str, dict]:
    records = {}
    for line in text.splitlines():
        if line.strip():
            record = json.loads(line)
            records[record["id"]] = record
    return records


# ---------------------------------------------------------------------------------------------
# orchestration


def run_oracle(vectors: list[dict], checkout: pathlib.Path, cc: str, work: pathlib.Path) -> dict[str, dict]:
    """Run the vectors on the pinned Musashi core only (used by the run() comparison and by controls)."""
    work = work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    (work / "oracle-vectors.txt").write_text("".join(vector_line(v) + "\n" for v in vectors))
    binary = fd.build_oracle(checkout, cc, work, TOOLS / "m68k_conformance_oracle.c", (TOOLS,))
    proc = _run([str(binary), str(work / "oracle-vectors.txt")])
    if proc.returncode != 0:
        raise RuntimeError("Musashi oracle runner failed: " + proc.stderr[:2000])
    return parse_records(proc.stdout)


def run(table: dict, forms: dict, emitter: pathlib.Path, cc: str, checkout: pathlib.Path | None, work: pathlib.Path,
        rows: list[str] | None = None, mutate=None) -> dict:
    selected = [r for r in table["rows"] if rows is None or r["id"] in rows]
    vectors = [v for r in selected for v in expand_row(r, table, forms)]
    work = work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    (work / "vectors.txt").write_text("".join(vector_line(v) + "\n" for v in vectors))
    runner, status = build_generated(sorted({v["code"] for v in vectors}), emitter, cc, work, mutate)
    out1 = _run([str(runner), str(work / "vectors.txt")])
    out2 = _run([str(runner), str(work / "vectors.txt")])
    if out1.returncode != 0:
        raise RuntimeError("generated-native runner failed: " + out1.stderr[:2000])
    generated = parse_records(out1.stdout)
    deterministic = out1.stdout == out2.stdout
    oracle = run_oracle(vectors, checkout, cc, work) if checkout is not None else None
    report_rows = []
    for row in selected:
        form = forms[row["form"]]
        mine = [v for v in vectors if v["row"] == row["id"]]
        outcomes: dict[int, set] = {}
        first = None
        counts = {"compared": 0, "diverged": 0, "unsupported": 0}
        for v in mine:
            seen = outcomes.setdefault(v["word"], set())
            code_status = status.get(v["code"], "undecodable")
            g = generated.get(v["id"])
            if code_status != "ok" or g is None or g.get("missing"):
                counts["unsupported"] += 1
                seen.add("unsupported")
            elif oracle is not None:
                counts["compared"] += 1
                rep = fd.compare_streams([g], [oracle[v["id"]]], 1, CODE_BASE, v["id"])
                if rep["result"] != "no_divergence":
                    counts["diverged"] += 1
                    seen.add("diverged")
                    if first is None:
                        first = rep
                else:
                    seen.add("pass")
            else:
                seen.add("pass")
        by_word = {w: ("unsupported" if "unsupported" in o else "diverged" if "diverged" in o else "pass")
                   for w, o in outcomes.items()}
        words = form_words(form)
        credit = manifest_credit(row, form, table)
        passing = [w for w in words if by_word.get(w) == "pass"]
        report_rows.append({"row": row["id"], "form": row["form"], "vectors": len(mine), "words": len(words),
                            "passing_words": len(passing), "counts": counts,
                            "status": "unsupported" if counts["unsupported"] else (
                                "diverged" if counts["diverged"] else ("validated" if oracle is not None else "self_consistent")),
                            "credit": credit if oracle is not None else {}, "first_divergence": first,
                            "words_passing": ["%04X" % w for w in passing]})
    return {"schema": SCHEMA, "musashi": "pinned" if oracle is not None else "skipped", "deterministic": deterministic,
            "vectors": len(vectors), "rows": report_rows}


def credited_words(report: dict, forms: dict, table: dict) -> dict[str, set[int]]:
    """Words each aspect may take from a report: only fully validated rows with every word passing."""
    aspects: dict[str, set[int]] = {"semantic": set(), "ccr": set(), "ea": set()}
    for row in report["rows"]:
        if row["status"] != "validated" or row["passing_words"] != row["words"]:
            continue
        for aspect, allowed in row["credit"].items():
            if allowed:
                aspects[aspect] |= {int(w, 16) for w in row["words_passing"]}
    return aspects


def declared_words(table: dict, forms: dict) -> dict[str, set[int]]:
    """Words the TABLE claims (static, no oracle): what the committed manifest must contain for this source."""
    aspects: dict[str, set[int]] = {"semantic": set(), "ccr": set(), "ea": set()}
    for row in table["rows"]:
        form = forms[row["form"]]
        for aspect, allowed in manifest_credit(row, form, table).items():
            if allowed:
                aspects[aspect] |= set(form_words(form))
    return aspects


MANIFEST_KEYS = {"semantic": "semantic_validated_words", "ccr": "ccr_sr_validated_words",
                 "ea": "ea_side_effect_validated_words"}


def update_manifest(credited: dict[str, set[int]], path: pathlib.Path = MANIFEST) -> None:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    for aspect, words in credited.items():
        entry = manifest[MANIFEST_KEYS[aspect]]
        for word in words:
            sources = entry.setdefault("%04X" % word, [])
            if SOURCE_TAG not in sources:
                sources.append(SOURCE_TAG)
                sources.sort()
        manifest[MANIFEST_KEYS[aspect]] = dict(sorted(entry.items()))
    if SOURCE_TAG not in manifest["sources"]:
        manifest["sources"] = sorted(manifest["sources"] + [SOURCE_TAG])
    path.write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n", encoding="utf-8")


def render(report: dict) -> str:
    return json.dumps(report, sort_keys=True, indent=1) + "\n"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--emitter", required=True, type=pathlib.Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--table", type=pathlib.Path, default=TABLE)
    parser.add_argument("--musashi-checkout", type=pathlib.Path)
    parser.add_argument("--rows", help="comma-separated row ids (default: all)")
    parser.add_argument("--report", type=pathlib.Path)
    parser.add_argument("--work-dir", type=pathlib.Path)
    parser.add_argument("--update-manifest", action="store_true")
    args = parser.parse_args(argv)
    table, forms = load_table(args.table), load_forms()
    checkout = musashi_checkout(args.musashi_checkout)
    with tempfile.TemporaryDirectory() as scratch:
        work = args.work_dir or pathlib.Path(scratch)
        report = run(table, forms, args.emitter, args.cc, checkout, work,
                     args.rows.split(",") if args.rows else None)
    text = render(report)
    if args.report:
        args.report.write_text(text)
    bad = [r for r in report["rows"] if r["status"] in ("unsupported", "diverged")]
    print("conformance: %d vectors, %d rows, oracle %s, deterministic=%s, failing rows=%d" % (
        report["vectors"], len(report["rows"]), report["musashi"], report["deterministic"], len(bad)))
    for r in bad:
        print("  %s: %s (%d/%d words pass) %s" % (r["row"], r["status"], r["passing_words"], r["words"],
                                                 json.dumps(r["first_divergence"], sort_keys=True) if r["first_divergence"] else ""))
    if args.update_manifest:
        if report["musashi"] != "pinned":
            print("refusing to update the manifest without the pinned Musashi oracle", file=sys.stderr)
            return 2
        update_manifest(credited_words(report, forms, table))
    return 1 if bad or not report["deterministic"] else 0


if __name__ == "__main__":
    sys.exit(main())
