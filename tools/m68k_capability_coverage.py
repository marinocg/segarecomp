#!/usr/bin/env python3
"""SEG-021-T002: per-stage / per-route MC68000 capability coverage over the independent legal-form baseline.

The denominator is `tests/fixtures/m68k-legal-forms.json` (SEG-021-T001). This tool holds no legality
knowledge of its own. For every legal form it asks the public pipeline (through the
`m68k_capability_probe` executable, which calls only public decode/lift/effect/timing/emit/admission
entry points) about every concrete primary word of the form under one fixed extension-word pattern, and
adds two mechanical stages: strict C11 compilation of the emitted code (batched translation units) and
native execution of the compiled functions (one conformance binary, restart-on-crash).

A form passes a stage only if EVERY listed primary word passes it.

Output is deterministic (sorted keys, no timestamps, no wall-clock values); measured cost goes to stderr.

    python3 tools/m68k_capability_coverage.py --probe build/dev/tests/m68k_capability_probe \
        [--cc cc] [--json OUT.json] [--report OUT.md] [--update-snapshot]
"""
import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
FORMS = ROOT / "tests" / "fixtures" / "m68k-legal-forms.json"
MANIFEST = ROOT / "tests" / "fixtures" / "m68k-validation-manifest.json"
SNAPSHOT = ROOT / "tests" / "fixtures" / "m68k-capability-coverage.json"
REPORT = ROOT / "docs" / "testing" / "m68k-capability-coverage.md"

SCHEMA = 1
PROBE_FIELDS = ["decode", "lift", "effects", "ea_footprint", "timing", "emit_direct", "emit_routed",
                "aot", "static", "exception_vector"]
# Ordered stage list (bit position in the per-form mask).
PIPELINE_STAGES = ["decode", "lift", "effects", "ea_side_effects", "emit", "compile", "native_exec",
                   "exception_privilege", "timing_model"]
ROUTES = ["route_direct", "route_runtime_routed", "route_immutable_rom_aot", "route_static_discovery"]
VALIDATION = ["semantic_validated", "timing_validated"]
ALL_STAGES = PIPELINE_STAGES + ROUTES + VALIDATION
END_TO_END = ["decode", "lift", "effects", "emit", "compile", "native_exec"]
CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-pedantic", "-Werror", "-O0"]
EXTENSION_PATTERN = "every extension word 0x0004; 1 MiB linear window; D0-D7 small even values, A0-A7 inside window"
VECTORS = {"address_error_vector_3": 3, "illegal_vector_4": 4, "zero_divide_vector_5": 5, "chk_vector_6": 6,
           "trapv_vector_7": 7, "privilege_violation_vector_8": 8, "trap_vector_32_47": 32}

RUNNER = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t d[8]; uint32_t a[8]; uint16_t sr; uint32_t pc; uint32_t usp; uint8_t ram[0x100000]; } cap_state;
typedef int (*cap_fn)(cap_state *);
%(externs)s
static cap_state state;
static const cap_fn *tables[] = {%(tables)s};
static const unsigned short *words[] = {%(words)s};
static const unsigned *counts[] = {%(counts)s};
int main(int argc, char **argv) {
  const unsigned start = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 0U;
  for (unsigned i = 0; i < sizeof state.ram; ++i) state.ram[i] = (uint8_t)((i * 7U + 3U) & 0xFFU);
  for (unsigned c = 0; c < %(n)s; ++c) {
    for (unsigned k = 0; k < *counts[c]; ++k) {
      const unsigned w = words[c][k];
      if (w < start) continue;
      for (unsigned r = 0; r < 8U; ++r) { state.d[r] = 0x100U + r * 0x10U; state.a[r] = 0x10000U + r * 0x1000U; }
      state.sr = 0x271FU; state.pc = 0x2000U; state.usp = 0x18000U;
      const int rc = tables[c][k](&state);
      printf("%%04X %%d\n", w, rc);
      fflush(stdout);
    }
  }
  return 0;
}
'''


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def load_forms():
    data = json.loads(FORMS.read_text(encoding="utf-8"))
    cols = data["form_columns"]
    forms = [dict(zip(cols, row)) for row in data["forms"]]
    return data, forms


def expand(form):
    for lo, hi in form["word_ranges"]:
        yield from range(lo, hi + 1)


def run_probe(probe, workdir):
    proc = subprocess.run([str(probe), "--emit-dir", str(workdir)], capture_output=True, text=True, check=True)
    table = {}
    for line in proc.stdout.splitlines():
        parts = line.split()
        table[int(parts[0], 16)] = [int(x) for x in parts[1:]]
    if len(table) != 0x10000:
        raise SystemExit("probe did not report all 65,536 primary words")
    return table


def split_chunk(text):
    lines = text.split("\n")
    first = next(i for i, line in enumerate(lines) if line.startswith("static int w_"))
    tail = next(i for i, line in enumerate(lines) if line.startswith("int cap_touch_"))
    header = lines[:first]
    funcs, current = [], None
    for line in lines[first:tail]:
        if line.startswith("static int w_"):
            current = [line]
            funcs.append(current)
        else:
            current.append(line)
    return header, funcs, lines[tail:]


def build_chunk(header, funcs, tail_lines, suffix):
    body = ["\n".join(f) for f in funcs]
    names = [re.match(r"static int (w_[0-9A-F]{4})", f[0]).group(1) for f in funcs]
    tail = [tail_lines[0],
            "typedef int (*cap_fn)(cap_state *);",
            "const cap_fn cap_table_%s[] = {%s};" % (suffix, ",".join(names)),
            "const unsigned short cap_words_%s[] = {%s};" % (suffix, ",".join("0x" + n[2:] for n in names)),
            "const unsigned cap_count_%s = %d;" % (suffix, len(names)), ""]
    return "\n".join(header) + "\n" + "\n".join(body) + "\n" + "\n".join(tail)


def compile_chunk(cc, path):
    """Compile one translation unit strictly; on failure bisect out the failing functions."""
    suffix = path.stem.split("_")[1]
    failed = set()
    for _ in range(4):
        obj = path.with_suffix(".o")
        proc = subprocess.run([cc, *CFLAGS, "-c", str(path), "-o", str(obj)], capture_output=True, text=True)
        if proc.returncode == 0:
            return failed
        header, funcs, tail = split_chunk(path.read_text())
        # Map diagnostic lines to functions by recomputing each function's line span.
        line_no = len(header) + 1
        spans = []
        for f in funcs:
            count = sum(len(part.split("\n")) for part in ["\n".join(f)])
            spans.append((line_no, line_no + count - 1, re.match(r"static int (w_[0-9A-F]{4})", f[0]).group(1)))
            line_no += count
        bad = set()
        for match in re.finditer(re.escape(path.name) + r":(\d+):\d+: error", proc.stderr):
            n = int(match.group(1))
            for lo, hi, name in spans:
                if lo <= n <= hi:
                    bad.add(name)
        if not bad:  # whole-unit failure with no attributable function
            bad = {name for _, _, name in spans}
        failed |= {int(n[2:], 16) for n in bad}
        keep = [f for f in funcs if re.match(r"static int (w_[0-9A-F]{4})", f[0]).group(1) not in bad]
        path.write_text(build_chunk(header, keep, tail, suffix))
    raise SystemExit("could not compile chunk %s after bisection" % path.name)


def chunk_words(path):
    _, funcs, _ = split_chunk(path.read_text())
    return [int(re.match(r"static int w_([0-9A-F]{4})", f[0]).group(1), 16) for f in funcs]


def native_exec(cc, workdir, chunks):
    externs, tables, words, counts = [], [], [], []
    for path in chunks:
        s = path.stem.split("_")[1]
        externs.append("extern const cap_fn cap_table_%s[]; extern const unsigned short cap_words_%s[]; "
                       "extern const unsigned cap_count_%s;" % (s, s, s))
        tables.append("cap_table_%s" % s)
        words.append("cap_words_%s" % s)
        counts.append("&cap_count_%s" % s)
    runner = workdir / "runner.c"
    runner.write_text(RUNNER % {"externs": "\n".join(externs), "tables": ",".join(tables),
                                "words": ",".join(words), "counts": ",".join(counts), "n": len(chunks)})
    exe = workdir / "runner"
    subprocess.run([cc, "-std=c11", "-O0", "-o", str(exe), str(runner)] + [str(c.with_suffix(".o")) for c in chunks],
                   check=True, capture_output=True)
    order = [w for c in chunks for w in chunk_words(c)]
    result, crashed, start = {}, set(), 0
    while True:
        try:
            proc = subprocess.run([str(exe), str(start)], capture_output=True, text=True, timeout=300)
            rc, out = proc.returncode, proc.stdout
        except subprocess.TimeoutExpired as exc:
            rc, out = -1, (exc.stdout.decode() if isinstance(exc.stdout, bytes) else (exc.stdout or ""))
        last = None
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 2:
                last = int(parts[0], 16)
                result[last] = int(parts[1])
        if rc == 0:
            return result, crashed
        pending = [w for w in order if w >= start and w not in result]
        if not pending:
            return result, crashed
        culprit = pending[0]
        crashed.add(culprit)
        start = culprit + 1


def needed_vectors(form):
    need = {VECTORS[e] for e in form["exceptions"]}
    return need


def measure(probe, cc):
    data, forms = load_forms()
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="m68k-cap-") as tmp:
        workdir = pathlib.Path(tmp)
        table = run_probe(probe, workdir)
        t_probe = time.monotonic()
        chunks = sorted(workdir.glob("chunk_*.c"))
        failed = set()
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            for result in pool.map(lambda p: compile_chunk(cc, p), chunks):
                failed |= result
        t_compile = time.monotonic()
        executed, crashed = native_exec(cc, workdir, chunks)
        t_exec = time.monotonic()
        emitted = {w for c in chunks for w in chunk_words(c)} | failed
    print("cost: probe %.1fs, compile %.1fs (%d units), native %.1fs, total %.1fs" % (
        t_probe - started, t_compile - t_probe, len(chunks), t_exec - t_compile, t_exec - started), file=sys.stderr)
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    validated_words = {int(w, 16): set(v) for w, v in manifest["semantic_validated_words"].items()}
    timing_words = {int(w, 16) for w in manifest.get("timing_validated_words", [])}

    def word_stage(word):
        f = dict(zip(PROBE_FIELDS, table[word]))
        emit_direct = bool(f["emit_direct"])
        compiled = emit_direct and word not in failed
        ran = compiled and executed.get(word) == 0 and word not in crashed
        return f, emit_direct, compiled, ran

    rows = {}
    for form in forms:
        ws = list(expand(form))
        need = needed_vectors(form)
        if form["privilege"] != "user" and 8 not in need:
            need.add(8)
        passes = {s: True for s in ALL_STAGES}
        applicable = {s: True for s in ALL_STAGES}
        for word in ws:
            f, emit_direct, compiled, ran = word_stage(word)
            passes["decode"] &= bool(f["decode"])
            passes["lift"] &= bool(f["lift"])
            passes["effects"] &= bool(f["effects"])
            passes["ea_side_effects"] &= bool(f["ea_footprint"])
            passes["emit"] &= emit_direct
            passes["compile"] &= compiled
            passes["native_exec"] &= ran
            passes["exception_privilege"] &= (not need) or need <= {f["exception_vector"]}
            passes["timing_model"] &= bool(f["timing"])
            passes["route_direct"] &= compiled
            passes["route_runtime_routed"] &= bool(f["emit_routed"])
            passes["route_immutable_rom_aot"] &= bool(f["aot"])
            passes["route_static_discovery"] &= bool(f["static"])
            passes["semantic_validated"] &= word in validated_words
            passes["timing_validated"] &= word in timing_words
        applicable["exception_privilege"] = bool(need)
        rows[form["id"]] = (form, passes, applicable, len(ws))
    return data, forms, rows, table, manifest, validated_words


def pct(n, d):
    return "%d.%02d" % divmod((n * 10000 + d // 2) // d if d else 0, 100)


def summarize(data, forms, rows, table, manifest, validated_words):
    def tally(selected):
        out = {}
        for stage in ALL_STAGES:
            app = [r for r in selected if r[2][stage]]
            ok = [r for r in app if r[1][stage]]
            out[stage] = {"applicable_forms": len(app), "passing_forms": len(ok),
                          "percent_forms": pct(len(ok), len(app)),
                          "passing_words": sum(r[3] for r in ok)}
        e2e = [r for r in selected if all(r[1][s] for s in END_TO_END)]
        out["end_to_end_structural"] = {"applicable_forms": len(selected), "passing_forms": len(e2e),
                                        "percent_forms": pct(len(e2e), len(selected)),
                                        "passing_words": sum(r[3] for r in e2e)}
        return out

    all_rows = list(rows.values())
    by_family, by_mnemonic = {}, {}
    for r in all_rows:
        by_family.setdefault(r[0]["family"], []).append(r)
        by_mnemonic.setdefault(r[0]["mnemonic"], []).append(r)
    legal_words = {w for r in all_rows for w in expand(r[0])}
    over = {}
    partition = data["primary_word_partition"]
    legend = partition["legend"]
    for word in range(0x10000):
        cls = legend[partition["rows"][word >> 8][word & 0xFF]]
        if cls != "legal_user" and cls != "legal_privileged" and table[word][0]:
            over[cls] = over.get(cls, 0) + 1
    unsupported = []
    for mnemonic in sorted(by_mnemonic):
        fails = [r for r in by_mnemonic[mnemonic] if not all(r[1][s] for s in END_TO_END)]
        if not fails:
            continue
        first = {}
        for r in fails:
            stage = next(s for s in END_TO_END if not r[1][s])
            first[stage] = first.get(stage, 0) + 1
        unsupported.append({"mnemonic": mnemonic, "family": by_mnemonic[mnemonic][0][0]["family"],
                            "forms": len(by_mnemonic[mnemonic]), "unsupported_forms": len(fails),
                            "first_failing_stage": dict(sorted(first.items()))})
    masks = {}
    for fid, (form, passes, applicable, _n) in rows.items():
        masks[fid] = "".join(("1" if passes[s] else "0") if applicable[s] else "-" for s in ALL_STAGES)
    partially = sum(1 for r in all_rows if any(w in validated_words for w in expand(r[0])))
    return {
        "schema": SCHEMA,
        "dataset": {"id": data["dataset"], "forms": len(forms), "legal_primary_words": len(legal_words)},
        "measurement": {"extension_pattern": EXTENSION_PATTERN, "compiler_flags": " ".join(CFLAGS),
                        "stage_order": ALL_STAGES, "end_to_end_structural_stages": END_TO_END,
                        "form_passes_a_stage_only_if_every_word_passes": True},
        "totals": tally(all_rows),
        "by_family": {k: tally(v) for k, v in sorted(by_family.items())},
        "validation": {"semantic_validated_words": len(validated_words),
                       "forms_with_at_least_one_validated_word": partially,
                       "manifest_sources": manifest["sources"]},
        "decode_over_acceptance_words": dict(sorted(over.items())),
        "unsupported_mnemonics": unsupported,
        "form_masks": masks,
    }


def render_report(result):
    t = result["totals"]
    lines = ["# MC68000 capability coverage (SEG-021-T002)", "",
             "Generated by `python3 tools/m68k_capability_coverage.py` from `tests/fixtures/m68k-legal-forms.json`;",
             "snapshot `tests/fixtures/m68k-capability-coverage.json` is enforced by `tests/m68k_capability_ratchet_test.py`.",
             "Do not edit by hand.", "",
             "Denominator: %d legal forms, %d legal primary words. A form passes a stage only if every one of its "
             "primary words passes. Measurement condition: %s." % (
                 result["dataset"]["forms"], result["dataset"]["legal_primary_words"],
                 result["measurement"]["extension_pattern"]), "",
             "## Per-stage and per-route coverage (percent of applicable forms)", "",
             "| stage | applicable forms | passing forms | percent | passing words |", "| --- | ---: | ---: | ---: | ---: |"]
    for stage in ALL_STAGES + ["end_to_end_structural"]:
        s = t[stage]
        lines.append("| %s | %d | %d | %s%% | %d |" % (stage, s["applicable_forms"], s["passing_forms"],
                                                     s["percent_forms"], s["passing_words"]))
    lines += ["", "`end_to_end_structural` = %s. It is a structural bar (decode, typed IR, effects, portable C11, strict "
              "compile, native run to completion); it does not claim CCR/EA/timing correctness, which only the "
              "`semantic_validated`/`timing_validated` rows measure." % ", ".join(result["measurement"]["end_to_end_structural_stages"]),
              "", "## By family (end-to-end structural, forms)", "", "| family | forms | passing | percent |",
              "| --- | ---: | ---: | ---: |"]
    for family, tally in result["by_family"].items():
        s = tally["end_to_end_structural"]
        lines.append("| %s | %d | %d | %s%% |" % (family, s["applicable_forms"], s["passing_forms"], s["percent_forms"]))
    lines += ["", "## Stage definitions (public entry points only)", "",
              "- `decode`/`lift`: `decode_m68k_instruction` (general-startup profile) returns a decoded form; `lift_m68k_instruction` maps it to a typed IR kind (not the MOVEQ default).",
              "- `effects`: `m68k_operation_effect` reports a PC effect. `ea_side_effects`: the effect owner declares a complete register write footprint (EA auto-update and implicit stack effects are visible).",
              "- `emit`/`compile`/`native_exec`: `emit_m68k_operation_c` (linear-memory context) produces C; the batched units compile under strict C11 (`-std=c11 -Wall -Wextra -pedantic -Werror`); the compiled function runs to normal completion from a fixed state in one native conformance binary (a runtime stop code, crash or hang fails the word).",
              "- `exception_privilege`: applicable only to forms whose dataset lists exception/privilege classes; passes only if the effect owner models every listed vector (it currently models only vector 5).",
              "- `timing_model`: `m68k_instruction_cycles` returns a value (existence of a timing entry, not correctness).",
              "- Routes: `route_direct` = emitted, compiled C; `route_runtime_routed` = Genesis runtime-routed emission is non-empty; `route_immutable_rom_aot` = `m68k_operation_is_immutable_rom_aot_safe` admits the form; `route_static_discovery` = CPU-owned static discovery walks the form to a clean end.",
              "- `semantic_validated`/`timing_validated`: from `tests/fixtures/m68k-validation-manifest.json` (existing pinned-Musashi differential words); all words of a form must be listed."]
    v = result["validation"]
    lines += ["", "## Validation manifest", "",
              "%d primary words have existing pinned-Musashi differential evidence; %d forms have at least one such word "
              "(`semantic_validated` requires all words). Sources: %s." % (
                  v["semantic_validated_words"], v["forms_with_at_least_one_validated_word"],
                  ", ".join(v["manifest_sources"])), "",
              "## Decode over-acceptance (non-legal words the decoder accepts)", ""]
    over = result["decode_over_acceptance_words"]
    lines += ["- %s: %d words" % (k, n) for k, n in over.items()] or ["- none"]
    lines += ["", "## Unsupported mnemonics (forms failing the end-to-end structural bar)", "",
              "| mnemonic | family | forms | unsupported | first failing stage (forms) |", "| --- | --- | ---: | ---: | --- |"]
    for u in result["unsupported_mnemonics"]:
        lines.append("| %s | %s | %d | %d | %s |" % (u["mnemonic"], u["family"], u["forms"], u["unsupported_forms"],
                     ", ".join("%s %d" % kv for kv in u["first_failing_stage"].items())))
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", required=True)
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--json")
    ap.add_argument("--report")
    ap.add_argument("--update-snapshot", action="store_true")
    args = ap.parse_args()
    result = summarize(*measure(pathlib.Path(args.probe), args.cc))
    text = json.dumps(result, indent=1, sort_keys=True) + "\n"
    if args.update_snapshot:
        write(SNAPSHOT, text)
        write(REPORT, render_report(result))
    if args.json:
        write(pathlib.Path(args.json), text)
    if args.report:
        write(pathlib.Path(args.report), render_report(result))
    if not (args.json or args.report or args.update_snapshot):
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
