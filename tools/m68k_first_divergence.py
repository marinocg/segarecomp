#!/usr/bin/env python3
"""SEG-020-T005: bounded M68k first-divergence diagnosis against the pinned Musashi oracle.

Two subcommands, no state kept between runs and no generation-time input:

  oracle   Run the pinned local Musashi core one retired instruction at a time over a caller
           supplied image and print one JSON line per instruction boundary in the shared
           boundary schema below.
  compare  Sequentially compare a generated boundary stream (the field-level detail lines of
           ``genesis_m68k_checkpoint_write_detail``) with an oracle stream and print one
           deterministic JSON report.

Boundary schema (one JSON object per line; the generated stream wraps it as
``{"m68k_checkpoint": {...}}``): ``boundary`` (1-based ordinal of the retired instruction),
``pc`` (state after the boundary), ``sr``, ``usp``, ``d[8]``, ``a[8]`` (A7 = active stack
pointer), ``unsupported`` (optional, 1 = effects not completely observable) and ``effects``: a
list of ``{"k":1,"w":width,"a":address,"v":value}`` memory writes and
``{"k":2,"w":0,"a":handler,"v":vector}`` exception entries.

Comparison rules (ADR-0042 sections 2, 3, 5): sequential lockstep with a mandatory boundary
limit (no bisection/replay); registers, SR, PC, USP, memory writes and exception entries are
compared, cycle counts and device state are not. The order between writes to *different*
destinations within one boundary is not compared (exception-frame push order legitimately differs
between cores); the order of repeated writes to the same (address, width) is.
A boundary flagged unsupported is never reported equal. First differing domain is ``cpu`` when
any compared field differs. Nothing here can perturb generated code: fault injection used by
tests lives only in the tests' temporary copies.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

MUSASHI_PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
CHECKOUT_ENV = "SEGARECOMP_M68K_MULS_WORD_MUSASHI_CHECKOUT"
CPU_NAME = "mc68000"
SCHEMA = 1

# ---------------------------------------------------------------------------------------------
# comparison


def parse_stream(text: str) -> list[dict]:
    records = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        record = json.loads(line)
        if "device_checkpoint" in record:
            continue  # SEG-020-T006 device lines belong to genesis_device_divergence.py
        if "m68k_checkpoint" in record:
            record = record["m68k_checkpoint"]
        if record is None:
            continue
        records.append(record)
    return records


def _effect_map(record: dict) -> dict[str, dict]:
    """Canonical effect view: key -> effect. Order between distinct destinations (kind, address,
    width) is ignored; repeated writes to one destination keep their sequence (``#n`` suffix)."""
    counts: dict[str, int] = {}
    mapped: dict[str, dict] = {}
    for effect in record.get("effects", []):
        base = ("write@%08X/w%d" % (effect["a"], effect["w"])) if effect["k"] == 1 else "trap"
        n = counts.get(base, 0)
        counts[base] = n + 1
        mapped[base if n == 0 else "%s#%d" % (base, n)] = effect
    return mapped


def _effect_value(effect: dict | None) -> dict | None:
    if effect is None:
        return None
    if effect["k"] == 1:
        return {"value": effect["v"]}
    return {"handler": effect["a"], "vector": effect["v"]}


def field_differences(generated: dict, oracle: dict) -> list[dict]:
    diffs: list[dict] = []

    def scalar(name: str, g, o) -> None:
        if g != o:
            diffs.append({"field": name, "generated": g, "oracle": o})

    for i in range(8):
        scalar("d%d" % i, generated["d"][i], oracle["d"][i])
    for i in range(8):
        scalar("a%d" % i, generated["a"][i], oracle["a"][i])
    scalar("usp", generated["usp"], oracle["usp"])
    scalar("sr", generated["sr"], oracle["sr"])
    scalar("pc", generated["pc"], oracle["pc"])
    gm, om = _effect_map(generated), _effect_map(oracle)
    for key in sorted(set(gm) | set(om)):
        g, o = _effect_value(gm.get(key)), _effect_value(om.get(key))
        if g != o:
            diffs.append({"field": "effect:" + key, "generated": g, "oracle": o})
    return diffs


def compare_streams(generated: list[dict], oracle: list[dict], limit: int, initial_pc: int,
                    image: str | None = None) -> dict:
    """Sequential lockstep compare over at most ``limit`` boundaries."""
    if limit <= 0:
        raise ValueError("limit must be a positive boundary count")
    report: dict = {"schema": SCHEMA, "cpu": CPU_NAME, "boundary_limit": limit, "domain": "none"}
    if image is not None:
        report["image"] = image
    last_pc = initial_pc
    matched = 0
    for index in range(limit):
        g = generated[index] if index < len(generated) else None
        o = oracle[index] if index < len(oracle) else None
        if g is None and o is None:
            break  # both streams ended within the limit
        boundary = index + 1
        failure = {"schema": SCHEMA, "cpu": CPU_NAME, "boundary_limit": limit,
                   "last_matching_boundary": matched, "first_differing_boundary": boundary,
                   "pc": last_pc}
        if image is not None:
            failure["image"] = image
        if g is None or o is None:
            failure.update(domain="cpu", result="diverged",
                           fields=[{"field": "boundary_presence",
                                    "generated": g is not None, "oracle": o is not None}])
            return failure
        if g.get("boundary") != boundary or o.get("boundary") != boundary:
            failure.update(domain="cpu", result="diverged",
                           fields=[{"field": "boundary_ordinal", "generated": g.get("boundary"),
                                    "oracle": o.get("boundary")}])
            return failure
        if g.get("unsupported") or o.get("unsupported"):
            failure.update(domain="none", result="unsupported_for_comparison",
                           fields=[{"field": "unsupported", "generated": bool(g.get("unsupported")),
                                    "oracle": bool(o.get("unsupported"))}])
            return failure
        diffs = field_differences(g, o)
        if diffs:
            failure.update(domain="cpu", result="diverged", fields=diffs)
            return failure
        matched = boundary
        last_pc = g["pc"]
    report.update(result="no_divergence", compared_boundaries=matched, last_matching_boundary=matched)
    return report


def render(report: dict) -> str:
    return json.dumps(report, sort_keys=True, separators=(",", ":")) + "\n"


# ---------------------------------------------------------------------------------------------
# pinned Musashi oracle

ORACLE_SOURCE = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m68k.h"
#define RAM_BEGIN UINT32_C(0x00FF0000)
#define RAM_SIZE UINT32_C(0x10000)
#define MAX_EFFECTS 64
static uint8_t *rom; static uint32_t rom_size; static uint8_t ram[RAM_SIZE];
typedef struct { unsigned k, w, a, v; } Effect;
static Effect effects[MAX_EFFECTS]; static unsigned effect_count;
static Effect vec[MAX_EFFECTS]; static unsigned vec_count; /* aligned low longword data reads: candidates only */
static int stepping;
static unsigned int read8(unsigned int a) {
  if (a < rom_size) return rom[a];
  if (a >= RAM_BEGIN && a < RAM_BEGIN + RAM_SIZE) return ram[a - RAM_BEGIN];
  return 0U;
}
unsigned int m68k_read_memory_8(unsigned int a) { return read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return (read8(a) << 8U) | read8(a + 1U); }
unsigned int m68k_read_memory_32(unsigned int a) {
  const unsigned int v = (m68k_read_memory_16(a) << 16U) | m68k_read_memory_16(a + 2U);
  /* Candidate vector fetch only; classified after the instruction against the exception frame. */
  if (stepping && a < 0x400U && (a & 3U) == 0U && vec_count < MAX_EFFECTS)
    vec[vec_count++] = (Effect){2U, 0U, v, a >> 2U};
  return v;
}
unsigned int m68k_read_immediate_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_immediate_32(unsigned int a) { return (m68k_read_memory_16(a) << 16U) | m68k_read_memory_16(a + 2U); }
unsigned int m68k_read_pcrelative_8(unsigned int a) { return read8(a); }
unsigned int m68k_read_pcrelative_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_pcrelative_32(unsigned int a) { return m68k_read_immediate_32(a); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return read8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_immediate_32(a); }
static void store8(unsigned int a, unsigned int v) { if (a >= RAM_BEGIN && a < RAM_BEGIN + RAM_SIZE) ram[a - RAM_BEGIN] = (uint8_t)v; }
static void note(unsigned w, unsigned a, unsigned v) {
  if (stepping && effect_count < MAX_EFFECTS) effects[effect_count++] = (Effect){1U, w, a, v};
}
void m68k_write_memory_8(unsigned int a, unsigned int v) { note(1U, a, v & 0xFFU); store8(a, v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { note(2U, a, v & 0xFFFFU); store8(a, v >> 8U); store8(a + 1U, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { note(4U, a, v); store8(a, v >> 24U); store8(a + 1U, v >> 16U); store8(a + 2U, v >> 8U); store8(a + 3U, v); }
void m68k_write_memory_32_pd(unsigned int a, unsigned int v) { m68k_write_memory_32(a, v); }
int main(int argc, char **argv) {
  FILE *f; unsigned long steps, i; unsigned k;
  if (argc != 6) return 2;
  f = fopen(argv[1], "rb"); if (!f) return 2;
  fseek(f, 0, SEEK_END); rom_size = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
  rom = malloc(rom_size); if (!rom || fread(rom, 1, rom_size, f) != rom_size) return 2; fclose(f);
  m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset();
  for (k = 0; k < 8; ++k) { m68k_set_reg((m68k_register_t)(M68K_REG_D0 + k), 0U); m68k_set_reg((m68k_register_t)(M68K_REG_A0 + k), 0U); }
  m68k_set_reg(M68K_REG_SR, (unsigned)strtoul(argv[4], 0, 0));
  m68k_set_reg(M68K_REG_A7, (unsigned)strtoul(argv[3], 0, 0));
  m68k_set_reg(M68K_REG_PC, (unsigned)strtoul(argv[2], 0, 0));
  steps = strtoul(argv[5], 0, 0);
  (void)m68k_execute(1); /* drain the pending reset cycles; executes no instruction */
  for (i = 1; i <= steps; ++i) {
    unsigned unsupported, shape = 0U, match = 0U, first = 0U, frame_sp, frame_sp_before, j;
    frame_sp_before = m68k_get_reg(NULL, M68K_REG_A7);
    effect_count = 0; vec_count = 0; stepping = 1; (void)m68k_execute(1); stepping = 0;
    unsupported = effect_count >= MAX_EFFECTS || vec_count >= MAX_EFFECTS;
    /* MC68000 group-1/2 exception frame: SR word at SP, PC long at SP+2, resulting A7 == SP. */
    frame_sp = m68k_get_reg(NULL, M68K_REG_A7);
    for (k = 0; k < effect_count; ++k)
      if (effects[k].k == 1U && effects[k].w == 2U && effects[k].a == frame_sp)
        for (j = 0; j < effect_count; ++j)
          if (effects[j].k == 1U && effects[j].w == 4U && effects[j].a == frame_sp + 2U) shape = 1U;
    for (k = 0; k < vec_count; ++k)
      if (vec[k].a == m68k_get_reg(NULL, M68K_REG_PC)) { ++match; first = k; }
    if (shape && match == 1U && effect_count < MAX_EFFECTS) effects[effect_count++] = vec[first];
    else if (shape && vec_count != 0U) unsupported = 1U; /* frame but no unique matching vector fetch */
    else if (!shape && match != 0U && frame_sp != frame_sp_before) unsupported = 1U; /* stack moved without a recognizable frame */
    printf("{\"boundary\":%lu,\"pc\":%u,\"sr\":%u,\"usp\":%u,\"d\":[", i, m68k_get_reg(NULL, M68K_REG_PC),
           m68k_get_reg(NULL, M68K_REG_SR) & 0xFFFFU, m68k_get_reg(NULL, M68K_REG_USP));
    for (k = 0; k < 8; ++k) printf("%s%u", k ? "," : "", m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + k)));
    printf("],\"a\":[");
    for (k = 0; k < 8; ++k) printf("%s%u", k ? "," : "", m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + k)));
    printf("],\"unsupported\":%u,\"effects\":[", unsupported ? 1U : 0U);
    for (k = 0; k < effect_count; ++k)
      printf("%s{\"k\":%u,\"w\":%u,\"a\":%u,\"v\":%u}", k ? "," : "", effects[k].k, effects[k].w, effects[k].a, effects[k].v);
    printf("]}\n");
  }
  return 0;
}
'''


def _checked(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, **kwargs)
    if result.returncode != 0:
        raise RuntimeError("command failed: %s\n%s" % (args, result.stderr))
    return result


def _toolchain_env() -> dict:
    env = os.environ.copy()
    if not env.get("SDKROOT") and shutil.which("xcrun"):
        env["SDKROOT"] = _checked(["xcrun", "--show-sdk-path"]).stdout.strip()
    return env


def build_oracle(checkout: pathlib.Path, compiler: str, workdir: pathlib.Path) -> pathlib.Path:
    """Build the oracle runner from the pinned, unmodified Musashi checkout."""
    if _checked(["git", "-C", str(checkout), "rev-parse", "HEAD"]).stdout.strip() != MUSASHI_PIN:
        raise RuntimeError("Musashi checkout is not the pinned revision")
    if subprocess.run(["git", "-C", str(checkout), "diff", "--quiet", "HEAD", "--"]).returncode != 0:
        raise RuntimeError("Musashi checkout has local modifications")
    env = _toolchain_env()
    generator = workdir / "m68kmake"
    _checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
              str(checkout / "m68kmake.c"), "-o", str(generator)], env=env)
    shutil.copyfile(checkout / "m68k_in.c", workdir / "m68k_in.c")
    _checked([str(generator)], cwd=workdir, env=env)
    source = workdir / "oracle.c"
    source.write_text(ORACLE_SOURCE)
    binary = workdir / "oracle"
    _checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-error=unused-variable", "-pedantic",
              "-I", str(workdir), "-I", str(checkout), "-I", str(checkout / "softfloat"), str(source),
              str(checkout / "m68kcpu.c"), str(workdir / "m68kops.c"), str(checkout / "softfloat/softfloat.c"),
              "-o", str(binary)], env=env)
    return binary


def run_oracle(checkout: pathlib.Path, compiler: str, image: bytes, pc: int, sp: int, sr: int,
               steps: int) -> str:
    with tempfile.TemporaryDirectory() as directory:
        workdir = pathlib.Path(directory)
        binary = build_oracle(checkout, compiler, workdir)
        rom = workdir / "image.bin"
        rom.write_bytes(image)
        return _checked([str(binary), str(rom), hex(pc), hex(sp), hex(sr), str(steps)]).stdout


# ---------------------------------------------------------------------------------------------


def _int(text: str) -> int:
    return int(text, 0)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    cmp_p = sub.add_parser("compare", help="compare a generated boundary stream with an oracle stream")
    cmp_p.add_argument("--generated", required=True, type=pathlib.Path)
    cmp_p.add_argument("--oracle", required=True, type=pathlib.Path)
    cmp_p.add_argument("--limit", required=True, type=int, help="mandatory maximum boundary count")
    cmp_p.add_argument("--initial-pc", required=True, type=_int)
    cmp_p.add_argument("--image", help="platform-owned image identity echoed into the report")
    orc_p = sub.add_parser("oracle", help="emit Musashi boundary stream for an image")
    orc_p.add_argument("--musashi-checkout", type=pathlib.Path,
                       default=pathlib.Path(os.environ[CHECKOUT_ENV]) if os.environ.get(CHECKOUT_ENV) else None)
    orc_p.add_argument("--cc", default="cc")
    orc_p.add_argument("--image-file", required=True, type=pathlib.Path)
    orc_p.add_argument("--pc", required=True, type=_int)
    orc_p.add_argument("--sp", required=True, type=_int)
    orc_p.add_argument("--sr", default=0x2700, type=_int)
    orc_p.add_argument("--steps", required=True, type=int)
    args = parser.parse_args(argv)
    if args.command == "compare":
        report = compare_streams(parse_stream(args.generated.read_text()), parse_stream(args.oracle.read_text()),
                                 args.limit, args.initial_pc, args.image)
        sys.stdout.write(render(report))
        return 0 if report["result"] == "no_divergence" else 1
    if args.musashi_checkout is None:
        parser.error("--musashi-checkout or %s is required" % CHECKOUT_ENV)
    sys.stdout.write(run_oracle(args.musashi_checkout, args.cc, args.image_file.read_bytes(), args.pc, args.sp,
                                args.sr, args.steps))
    return 0


if __name__ == "__main__":
    sys.exit(main())
