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

SCHEMA = 2
PROBE_FIELDS = ["decode", "lift", "effects", "ea_footprint", "ccr_declared", "timing", "emit_direct", "emit_routed",
                "aot", "static", "exception_vector"]
# Ordered stage list (bit position in the per-form mask).
# Structural stages: what the public pipeline declares/produces. Validated stages: independent evidence.
PIPELINE_STAGES = ["decode", "lift", "effects", "ea_footprint_declared", "ccr_sr_effect_declared",
                   "exception_privilege_modeled", "timing_model_present", "emit", "compile", "native_exec"]
# The direct route is emit -> compile -> native_exec above; the rows below are the other admission routes.
ROUTES = ["route_runtime_routed_admitted", "route_runtime_routed_compiles", "route_runtime_routed_executes",
          "route_immutable_rom_aot", "route_static_discovery"]
VALIDATION = ["semantic_validated", "ccr_sr_validated", "ea_side_effect_validated", "timing_validated"]
ALL_STAGES = PIPELINE_STAGES + ROUTES + VALIDATION
END_TO_END = ["decode", "lift", "effects", "emit", "compile", "native_exec"]
# Stages whose result depends on the C compiler/host rather than only on the pipeline.
COMPILER_DEPENDENT = ["compile", "native_exec", "route_runtime_routed_compiles", "route_runtime_routed_executes"]
# -Wno-type-limits: the conformance window starts at address 0, so the emitted lower-bound guard is an
# intentional `unsigned < 0` comparison that gcc's -Wextra flags; it is a probe-window artifact, not a defect.
CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-Wno-type-limits", "-pedantic", "-Werror", "-O0"]
RUNTIME_DIR = ROOT / "platforms" / "genesis" / "runtime"
EXTENSION_PATTERN = ("every extension word 0x0004 (primary words are exhaustive; extension-word, index, MOVEM-mask, "
                     "displacement and immediate values are NOT); one fixed register/RAM state restored before every word; "
                     "direct route: 1 MiB linear window at 0; runtime-routed route: Genesis work RAM only")
VECTORS = {"address_error_vector_3": 3, "illegal_vector_4": 4, "zero_divide_vector_5": 5, "chk_vector_6": 6,
           "trapv_vector_7": 7, "privilege_violation_vector_8": 8}
# Architectural condition-code expectation transcribed from the Motorola M68000 Family Programmer's Reference
# Manual condition-code columns; it names forms that ALWAYS modify CCR/SR, and holds no production knowledge.
CCR_ALWAYS_MNEMONICS = frozenset(
    "ABCD ADD ADDI ADDX AND ANDI ASL ASR BCHG BCLR BSET BTST CHK CLR CMP CMPA CMPI CMPM DIVS DIVU EOR EORI EXT LSL LSR "
    "MOVEQ MULS MULU NBCD NEG NEGX NOT OR ORI ROL ROR ROXL ROXR RTE RTR SBCD STOP SUB SUBI SUBX SWAP TAS TST".split())
CCR_MOVE_FORMS = frozenset(["ea_ea", "ea_ccr", "ea_sr"])

DIRECT_RUNNER = r"""#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t d[8]; uint32_t a[8]; uint16_t sr; uint32_t pc; uint32_t usp; uint8_t ram[0x100000]; } cap_state;
typedef int (*cap_fn)(cap_state *);
%(externs)s
static cap_state baseline, state;
uint32_t frame_ids[64], frame_continuations[64], frame_depth; /* emitted call-frame state, reset for every word */
static const cap_fn *tables[] = {%(tables)s};
static const unsigned short *words[] = {%(words)s};
static const unsigned *counts[] = {%(counts)s};
int main(int argc, char **argv) {
  const unsigned start = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 0U;
  for (unsigned i = 0; i < sizeof baseline.ram; ++i) baseline.ram[i] = (uint8_t)((i * 7U + 3U) & 0xFFU);
  for (unsigned r = 0; r < 8U; ++r) { baseline.d[r] = 0x100U + r * 0x10U; baseline.a[r] = 0x10000U + r * 0x1000U; }
  baseline.sr = 0x271FU; baseline.pc = 0x2000U; baseline.usp = 0x18000U;
  for (unsigned c = 0; c < %(n)s; ++c) {
    for (unsigned k = 0; k < *counts[c]; ++k) {
      const unsigned w = words[c][k];
      if (w < start) continue;
      memcpy(&state, &baseline, sizeof state); /* every word starts from the identical full baseline state */
      memset(frame_ids, 0, sizeof frame_ids); memset(frame_continuations, 0, sizeof frame_continuations); frame_depth = 0U;
      const int rc = tables[c][k](&state);
      printf("%%04X %%d\n", w, rc);
      fflush(stdout);
    }
  }
  return 0;
}
"""

ROUTED_RUNNER = r"""#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef GenesisControlTransfer (*cap_fn)(GenesisRuntime *);
%(externs)s
static GenesisRuntime baseline, runtime_state;
uint32_t frame_ids[64], frame_continuations[64], frame_depth; /* emitted call-frame state, reset for every word */
static const cap_fn *tables[] = {%(tables)s};
static const unsigned short *words[] = {%(words)s};
static const unsigned *counts[] = {%(counts)s};
int main(int argc, char **argv) {
  const unsigned start = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 0U;
  for (unsigned i = 0; i < sizeof baseline.work_ram; ++i) baseline.work_ram[i] = (uint8_t)((i * 7U + 3U) & 0xFFU);
  for (unsigned r = 0; r < 8U; ++r) { baseline.d[r] = 0x100U + r * 0x10U; baseline.a[r] = UINT32_C(0x00FF0000) + 0x2000U + r * 0x1000U; }
  baseline.sr = 0x271FU; baseline.pc = 0x2000U; baseline.usp = UINT32_C(0x00FF8000);
  for (unsigned c = 0; c < %(n)s; ++c) {
    for (unsigned k = 0; k < *counts[c]; ++k) {
      const unsigned w = words[c][k];
      if (w < start) continue;
      memcpy(&runtime_state, &baseline, sizeof runtime_state);
      memset(frame_ids, 0, sizeof frame_ids); memset(frame_continuations, 0, sizeof frame_continuations); frame_depth = 0U;
      const GenesisControlTransfer t = tables[c][k](&runtime_state);
      printf("%%04X %%d\n", w, t.kind == GENESIS_CONTINUE_AT_PC ? 0 : 1);
      fflush(stdout);
    }
  }
  return 0;
}
"""

FUNC_RE = re.compile(r"static \w+ (r?w_[0-9A-F]{4})\(")


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
    first = next(i for i, line in enumerate(lines) if FUNC_RE.match(line))
    tail = next(i for i, line in enumerate(lines) if line.startswith("int cap_touch_"))
    header = lines[:first]
    funcs, current = [], None
    for line in lines[first:tail]:
        if FUNC_RE.match(line):
            current = [line]
            funcs.append(current)
        else:
            current.append(line)
    return header, funcs, lines[tail:]


def func_name(func):
    return FUNC_RE.match(func[0]).group(1)


def build_chunk(header, funcs, tail_lines, suffix):
    names = [func_name(f) for f in funcs]
    tail = [tail_lines[0], tail_lines[1],
            "const cap_fn cap_table_%s[] = {%s};" % (suffix, ",".join(names)),
            "const unsigned short cap_words_%s[] = {%s};" % (suffix, ",".join("0x" + n[-4:] for n in names)),
            "const unsigned cap_count_%s = %d;" % (suffix, len(names)), ""]
    return "\n".join(header) + "\n" + "\n".join("\n".join(f) for f in funcs) + "\n" + "\n".join(tail)


def compile_chunk(cc, path, include_dir=None):
    """Compile one translation unit strictly; on failure bisect out the failing functions and recompile."""
    suffix = path.stem.split("_")[1]
    flags = CFLAGS + (["-I", str(include_dir)] if include_dir else [])
    failed = set()
    for attempt in range(5):
        proc = subprocess.run([cc, *flags, "-c", str(path), "-o", str(path.with_suffix(".o"))],
                              capture_output=True, text=True)
        if proc.returncode == 0:
            return failed
        if attempt == 4:
            break
        header, funcs, tail = split_chunk(path.read_text(encoding="utf-8"))
        line_no, spans = len(header) + 1, []
        for f in funcs:
            count = len("\n".join(f).split("\n"))
            spans.append((line_no, line_no + count - 1, func_name(f)))
            line_no += count
        bad = set()
        for match in re.finditer(re.escape(path.name) + r":(\d+):\d+: error", proc.stderr):
            n = int(match.group(1))
            bad |= {name for lo, hi, name in spans if lo <= n <= hi}
        if not bad:  # failure not attributable to one function: reject the whole unit
            bad = {name for _, _, name in spans}
        failed |= {int(n[-4:], 16) for n in bad}
        keep = [f for f in funcs if func_name(f) not in bad]
        write(path, build_chunk(header, keep, tail, suffix))
    raise SystemExit("could not compile %s after bisection" % path.name)


def chunk_words(path):
    _, funcs, _ = split_chunk(path.read_text(encoding="utf-8"))
    return [int(func_name(f)[-4:], 16) for f in funcs]


def native_exec(cc, workdir, chunks, routed=False, extra_objects=()):
    """Link every chunk with a runner and execute; a crash/hang fails only the culprit word (restart after it)."""
    externs, tables, words, counts = [], [], [], []
    for path in chunks:
        s = path.stem.split("_")[1]
        externs.append("extern const cap_fn cap_table_%s[]; extern const unsigned short cap_words_%s[]; "
                       "extern const unsigned cap_count_%s;" % (s, s, s))
        tables.append("cap_table_%s" % s)
        words.append("cap_words_%s" % s)
        counts.append("&cap_count_%s" % s)
    runner = workdir / ("rrunner.c" if routed else "runner.c")
    write(runner, (ROUTED_RUNNER if routed else DIRECT_RUNNER) % {
        "externs": "\n".join(externs), "tables": ",".join(tables), "words": ",".join(words),
        "counts": ",".join(counts), "n": len(chunks)})
    exe = workdir / ("rrunner" if routed else "runner")
    flags = ["-std=c11", "-O0"] + (["-I", str(RUNTIME_DIR)] if routed else [])
    subprocess.run([cc, *flags, "-o", str(exe), str(runner)] + [str(c.with_suffix(".o")) for c in chunks] +
                   [str(o) for o in extra_objects], check=True, capture_output=True)
    order = [w for c in chunks for w in chunk_words(c)]
    result, crashed, start = {}, set(), 0
    while True:
        try:
            proc = subprocess.run([str(exe), str(start)], capture_output=True, text=True, timeout=300)
            rc, out = proc.returncode, proc.stdout
        except subprocess.TimeoutExpired as exc:
            rc, out = -1, (exc.stdout.decode() if isinstance(exc.stdout, bytes) else (exc.stdout or ""))
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 2:
                result[int(parts[0], 16)] = int(parts[1])
        if rc == 0:
            return result, crashed
        pending = [w for w in order if w >= start and w not in result]
        if not pending:
            return result, crashed
        crashed.add(pending[0])
        start = pending[0] + 1


def needed_vectors(form, word):
    """Exception classes the form can architecturally raise for this concrete primary word."""
    need = set()
    for name in form["exceptions"]:
        if name == "trap_vector_32_47":
            need.add(32 + (word & 0xF))  # TRAP #n -> vector 32 + n (TRAP is 0x4E40 | n)
        else:
            need.add(VECTORS[name])
    if form["privilege"] != "user":
        need.add(8)
    return need


def exception_modeled(need, effect_vector):
    """The effect contract carries a single synchronous-exception vector, so it models a form only if at most
    one class is required and that class is the one the effect names."""
    return not need or need == {effect_vector}


def ccr_expected(form):
    mnemonic = form["mnemonic"]
    if mnemonic in ("ADDQ", "SUBQ"):
        return form["dst"] != "an"
    if mnemonic == "MOVE":
        return form["form"] in CCR_MOVE_FORMS
    return mnemonic in CCR_ALWAYS_MNEMONICS


def ea_effect_expected(form):
    return form["auto_update"] != "none"


def diff_masks(old_masks, new_masks, stages):
    """Compare per-form stage masks: (drops, improvements needing a snapshot update, tolerated improvements)."""
    drops, improvements, tolerated = [], [], []
    for form_id, old in old_masks.items():
        for stage, o, n in zip(stages, old, new_masks[form_id]):
            if o == n:
                continue
            if o == "1" and n == "0":
                drops.append("%s: %s dropped" % (form_id, stage))
            elif stage in COMPILER_DEPENDENT and o == "0" and n == "1":
                tolerated.append("%s: %s improved" % (form_id, stage))
            else:
                improvements.append("%s: %s %s -> %s" % (form_id, stage, o, n))
    return drops, improvements, tolerated


def measure(probe, cc):
    data, forms = load_forms()
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="m68k-cap-") as tmp:
        workdir = pathlib.Path(tmp)
        table = run_probe(probe, workdir)
        t_probe = time.monotonic()
        chunks = sorted(workdir.glob("chunk_*.c"))
        rchunks = sorted(workdir.glob("rchunk_*.c"))
        failed, rfailed = set(), set()
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            runtime_obj = workdir / "runtime.o"
            runtime_job = pool.submit(subprocess.run, [cc, "-std=c11", "-O0", "-I", str(RUNTIME_DIR), "-c",
                                                       str(RUNTIME_DIR / "runtime.c"), "-o", str(runtime_obj)],
                                      capture_output=True, text=True)
            for result in pool.map(lambda p: compile_chunk(cc, p), chunks):
                failed |= result
            for result in pool.map(lambda p: compile_chunk(cc, p, RUNTIME_DIR), rchunks):
                rfailed |= result
            if runtime_job.result().returncode != 0:
                raise SystemExit("could not compile the Genesis runtime for the routed harness")
        t_compile = time.monotonic()
        executed, crashed = native_exec(cc, workdir, chunks)
        rexecuted, rcrashed = native_exec(cc, workdir, rchunks, routed=True, extra_objects=[runtime_obj])
        t_exec = time.monotonic()
    print("cost: probe %.1fs, compile %.1fs (%d direct + %d routed units), native %.1fs, total %.1fs" % (
        t_probe - started, t_compile - t_probe, len(chunks), len(rchunks), t_exec - t_compile, t_exec - started),
        file=sys.stderr)
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    aspects = {name: {int(w, 16) for w in manifest[key]} for name, key in (
        ("semantic", "semantic_validated_words"), ("ccr", "ccr_sr_validated_words"),
        ("ea", "ea_side_effect_validated_words"), ("timing", "timing_validated_words"))}

    rows = {}
    for form in forms:
        ws = list(expand(form))
        ccr_needed, ea_needed = ccr_expected(form), ea_effect_expected(form)
        exc_needed = bool(form["exceptions"]) or form["privilege"] != "user"
        passes = {s: True for s in ALL_STAGES}
        applicable = {s: True for s in ALL_STAGES}
        applicable["ccr_sr_effect_declared"] = applicable["ccr_sr_validated"] = ccr_needed
        applicable["ea_side_effect_validated"] = ea_needed
        applicable["exception_privilege_modeled"] = exc_needed
        for word in ws:
            f = dict(zip(PROBE_FIELDS, table[word]))
            emit = bool(f["emit_direct"])
            compiled = emit and word not in failed
            ran = compiled and executed.get(word) == 0 and word not in crashed
            r_compiled = bool(f["emit_routed"]) and word not in rfailed
            r_ran = r_compiled and rexecuted.get(word) == 0 and word not in rcrashed
            passes["decode"] &= bool(f["decode"])
            passes["lift"] &= bool(f["lift"])
            passes["effects"] &= bool(f["effects"])
            passes["ea_footprint_declared"] &= bool(f["ea_footprint"])
            passes["ccr_sr_effect_declared"] &= bool(f["ccr_declared"])
            passes["exception_privilege_modeled"] &= exception_modeled(needed_vectors(form, word), f["exception_vector"])
            passes["timing_model_present"] &= bool(f["timing"])
            passes["emit"] &= emit
            passes["compile"] &= compiled
            passes["native_exec"] &= ran
            passes["route_runtime_routed_admitted"] &= bool(f["emit_routed"])
            passes["route_runtime_routed_compiles"] &= r_compiled
            passes["route_runtime_routed_executes"] &= r_ran
            passes["route_immutable_rom_aot"] &= bool(f["aot"])
            passes["route_static_discovery"] &= bool(f["static"])
            passes["semantic_validated"] &= word in aspects["semantic"]
            passes["ccr_sr_validated"] &= word in aspects["ccr"]
            passes["ea_side_effect_validated"] &= word in aspects["ea"]
            passes["timing_validated"] &= word in aspects["timing"]
        rows[form["id"]] = (form, passes, applicable, len(ws))
    return data, forms, rows, table, manifest, aspects


def pct(n, d):
    return "%d.%02d" % divmod((n * 10000 + d // 2) // d if d else 0, 100)


def summarize(data, forms, rows, table, manifest, aspects):
    def tally(selected):
        out = {}
        for stage in ALL_STAGES:
            app = [r for r in selected if r[2][stage]]
            ok = [r for r in app if r[1][stage]]
            out[stage] = {"applicable_forms": len(app), "passing_forms": len(ok),
                          "percent_forms": pct(len(ok), len(app)), "passing_words": sum(r[3] for r in ok)}
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
        if cls not in ("legal_user", "legal_privileged") and table[word][0]:
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
    touched = {name: sum(1 for r in all_rows if any(w in words for w in expand(r[0]))) for name, words in aspects.items()}
    return {
        "schema": SCHEMA,
        "dataset": {"id": data["dataset"], "forms": len(forms), "legal_primary_words": len(legal_words)},
        "measurement": {"extension_pattern": EXTENSION_PATTERN, "compiler_flags": " ".join(CFLAGS),
                        "stage_order": ALL_STAGES, "end_to_end_structural_stages": END_TO_END,
                        "form_passes_a_stage_only_if_every_word_passes": True},
        "totals": tally(all_rows),
        "by_family": {k: tally(v) for k, v in sorted(by_family.items())},
        "validation": {"validated_words": {k: len(v) for k, v in sorted(aspects.items())},
                       "forms_with_at_least_one_validated_word": dict(sorted(touched.items())),
                       "manifest_sources": manifest["sources"]},
        "decode_over_acceptance_words": dict(sorted(over.items())),
        "unsupported_mnemonics": unsupported,
        "form_masks": masks,
    }


STAGE_DEFINITIONS = [
    ("decode / lift", "structural", "`decode_m68k_instruction` (general-startup profile) returns a decoded form; `lift_m68k_instruction` maps it to a typed IR kind (not the MOVEQ default)."),
    ("effects", "structural", "`m68k_operation_effect` reports a PC effect."),
    ("ea_footprint_declared", "structural", "the effect owner DECLARES a complete architectural register write footprint. This is a claim, not evidence that auto-update, A7 byte adjustment, aliasing or implicit stack effects are correct (see `ea_side_effect_validated`)."),
    ("ccr_sr_effect_declared", "structural", "applicable to forms the Motorola manual says always modify CCR/SR (`CCR_ALWAYS_MNEMONICS`); passes if the effect owner declares `affects_condition_codes`. It does not check which flags or their values (see `ccr_sr_validated`)."),
    ("exception_privilege_modeled", "structural", "applicable to forms listing exception/privilege classes; per concrete word (TRAP #n needs vector 32+n) it passes only if the effect contract, which carries one synchronous-exception vector, represents every required class. Forms that can raise several classes remain unsupported."),
    ("timing_model_present", "structural", "`m68k_instruction_cycles` returns a value (existence of an entry, not correctness; see `timing_validated`)."),
    ("emit / compile / native_exec", "structural (direct route)", "`emit_m68k_operation_c` (linear-memory context) produces C; batched units compile under strict C11 (`-std=c11 -Wall -Wextra -Wno-type-limits -pedantic -Werror`); the function runs to normal completion in one native binary, each word from the identical restored baseline state (a runtime stop code, crash or hang fails the word)."),
    ("route_runtime_routed_admitted / compiles / executes", "structural (runtime-routed route)", "the Genesis runtime-routed lowering emits non-empty C (admitted); that C compiles under the same strict flags against the real `platforms/genesis/runtime` header (compiles); it runs against the real runtime linked from `runtime.c`, from a restored baseline with work RAM only, and continues at PC rather than stopping (executes). Whole-program C4 preflight facts are not exercised, so absolute-address forms stop at the runtime memory gate under the fixed extension pattern."),
    ("route_immutable_rom_aot", "structural", "`m68k_operation_is_immutable_rom_aot_safe` admits the form."),
    ("route_static_discovery", "structural", "the CPU-owned static discovery walks the form to a clean end."),
    ("semantic_validated", "validated", "every word has existing pinned-Musashi differential evidence comparing the result state."),
    ("ccr_sr_validated", "validated", "applicable to CCR-modifying forms; every word has existing Musashi evidence that compares SR/CCR."),
    ("ea_side_effect_validated", "validated", "applicable to forms with auto-update/implicit-stack effects; every word has existing Musashi evidence comparing the full D/A register state and memory."),
    ("timing_validated", "validated", "no existing test compares generated timing against an oracle."),
]


def render_report(result):
    t = result["totals"]
    lines = ["# MC68000 capability coverage (SEG-021-T002)", "",
             "Generated by `python3 tools/m68k_capability_coverage.py` from `tests/fixtures/m68k-legal-forms.json`;",
             "snapshot `tests/fixtures/m68k-capability-coverage.json` is enforced by `tests/m68k_capability_ratchet_test.py`.",
             "Do not edit by hand.", "",
             "Denominator: %d legal forms, %d legal primary words. A form passes a stage only if every one of its "
             "concrete primary words passes. **Measurement condition:** %s. This baseline proves nothing about other "
             "extension-word values, indexed extension detail, MOVEM masks, displacements or immediate operands; "
             "T003 and the family tasks own those dimensions." % (
                 result["dataset"]["forms"], result["dataset"]["legal_primary_words"],
                 result["measurement"]["extension_pattern"]), "",
             "**Structural rows say what the pipeline declares or produces. Only the `*_validated` rows are independent "
             "evidence of correctness.**", "",
             "## Structural coverage (percent of applicable forms)", "",
             "| stage | applicable forms | passing forms | percent | passing words |", "| --- | ---: | ---: | ---: | ---: |"]

    def row(stage):
        s = t[stage]
        return "| %s | %d | %d | %s%% | %d |" % (stage, s["applicable_forms"], s["passing_forms"], s["percent_forms"], s["passing_words"])

    lines += [row(s) for s in PIPELINE_STAGES + ROUTES]
    lines += ["", "`end_to_end_structural` (%s, direct route): %d of %d forms, %s%%. It is a structural bar, not a "
              "correctness claim." % (", ".join(result["measurement"]["end_to_end_structural_stages"]),
                                       t["end_to_end_structural"]["passing_forms"],
                                       t["end_to_end_structural"]["applicable_forms"],
                                       t["end_to_end_structural"]["percent_forms"]),
              "", "## Independently validated coverage (existing pinned-Musashi differential evidence)", "",
              "| stage | applicable forms | passing forms | percent | passing words |", "| --- | ---: | ---: | ---: | ---: |"]
    lines += [row(s) for s in VALIDATION]
    v = result["validation"]
    lines += ["", "Validated primary words per aspect: %s. Forms with at least one validated word: %s. Sources: %s." % (
        ", ".join("%s %d" % kv for kv in v["validated_words"].items()),
        ", ".join("%s %d" % kv for kv in v["forms_with_at_least_one_validated_word"].items()),
        ", ".join(v["manifest_sources"])), "",
        "## Stage definitions (public entry points only)", ""]
    lines += ["- `%s` (%s): %s" % d for d in STAGE_DEFINITIONS]
    lines += ["", "## By family (end-to-end structural, forms)", "", "| family | forms | passing | percent |",
              "| --- | ---: | ---: | ---: |"]
    for family, tally in result["by_family"].items():
        s = tally["end_to_end_structural"]
        lines.append("| %s | %d | %d | %s%% |" % (family, s["applicable_forms"], s["passing_forms"], s["percent_forms"]))
    lines += ["", "## Decode over-acceptance (non-legal words the decoder accepts)", ""]
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
