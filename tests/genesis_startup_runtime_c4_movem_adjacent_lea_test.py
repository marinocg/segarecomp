#!/usr/bin/env python3
"""C4 adjacent LEA/MOVEM validation and interior-entry safety.

Drives the same real production driver every other C4 regression uses
(m68k_pipeline_tests' argv-selected fixture builders, calling directly into
analyze_m68k_frontend / preflight_m68k_general_startup_c4 /
emit_m68k_general_startup_runtime_c -- never a private test-only
reimplementation), then compiles representative generated C as strict C11
and executes both normal and direct-interior dispatch against the real
Genesis runtime. The one MOVEM body selects validated immutable values only
when its live architectural base matches the adjacent LEA result.
"""
import pathlib
from compiled_entry_rows import rows
import subprocess
import sys
import tempfile

# SEG-007-T075 positive fixture (see
# emit_general_startup_runtime_c4_movem_adjacent_lea_source in
# tests/m68k_pipeline_test.cpp for the exact encoded sequence):
#   LEA (0x00000B16).L,A1 ; MOVEM.W (A1)+,D0-D1  -- (An)+ family, WORD size
#   LEA (0x00000B1A).L,A2 ; MOVEM.L (A2),D2-D3   -- (An) family, LONG size
# then the established RESET frontier.
HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  runtime.pc = UINT32_C(0x00000B00);
  GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B14));
  assert(runtime.d[0] == UINT32_C(0xFFFFFFFE));
  assert(runtime.d[1] == UINT32_C(0x00001234));
  assert(runtime.a[1] == UINT32_C(0x00000B1A));
  assert(runtime.d[2] == UINT32_C(0x80000001));
  assert(runtime.d[3] == UINT32_C(0x7FFFFFFE));
  assert(runtime.a[2] == UINT32_C(0x00000B1A));
  return 0;
}
'''

# SEG-007-T075 fix regression fixture: an `absolute_word` LEA producer whose
# 16-bit field has bit 15 set (0x8B16). Decode sign-extends this to the raw
# 32-bit value 0xFFFF8B16 -- LEA's own C4 emission writes exactly that raw
# value into A1, unmasked. A prior version of this capability instead
# resolved the fold's base address through `m68k_genesis_canonical_ea_
# address`, which additionally masks an `absolute_word` value to 24 bits
# (0x00FF8B16) -- a different address than the one LEA's own statement
# actually assigns. This fixture's mapping claim sits at the RAW value, so it
# proves both that the fold occurs at all (it would silently not fold, and
# instead route, under the pre-fix masked formula, since no claim exists at
# the masked address) and that the folded literals equal the raw/sign-
# extended source data, not some other value.
SIGN_EXTEND_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  runtime.pc = UINT32_C(0x00000B00);
  GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B08));
  assert(runtime.d[0] == UINT32_C(0xFFFF9ABC));
  assert(runtime.d[1] == UINT32_C(0x0000007F));
  assert(runtime.a[1] == UINT32_C(0xFFFF8B1A));
  return 0;
}
'''

INTERIOR_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  const uint32_t offset = UINT32_C(0x100);
  runtime.pc = UINT32_C(0x00000B06);
  runtime.a[1] = UINT32_C(0x00FF0100);
  runtime.work_ram[offset + 0U] = UINT8_C(0x80);
  runtime.work_ram[offset + 1U] = UINT8_C(0x01);
  runtime.work_ram[offset + 2U] = UINT8_C(0x12);
  runtime.work_ram[offset + 3U] = UINT8_C(0x34);
  GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B0A));
  assert(runtime.d[0] == UINT32_C(0xFFFF8001));
  assert(runtime.d[1] == UINT32_C(0x00001234));
  assert(runtime.a[1] == UINT32_C(0x00FF0104));
  printf("%u %u %u %u\n", (unsigned)runtime.pc, (unsigned)runtime.d[0],
         (unsigned)runtime.d[1], (unsigned)runtime.a[1]);
  return 0;
}
'''

NORMAL_INTERIOR_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  runtime.pc = UINT32_C(0x00000B00);
  GenesisControlTransfer transfer = {0};
  for (unsigned dispatch = 0; dispatch < 2U; ++dispatch) {
    transfer = genesis_bridge_dispatch(&runtime);
    if (transfer.kind != GENESIS_CONTINUE_AT_PC) break;
    assert(transfer.next_pc == runtime.pc);
  }
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B0A));
  assert(runtime.d[0] == UINT32_C(0x00001111));
  assert(runtime.d[1] == UINT32_C(0x00002222));
  assert(runtime.a[1] == UINT32_C(0x00000B10));
  printf("%u %u %u %u\n", (unsigned)runtime.pc, (unsigned)runtime.d[0],
         (unsigned)runtime.d[1], (unsigned)runtime.a[1]);
  return 0;
}
'''


def main():
  executable, compiler, root = sys.argv[1:]

  # Normal entry executes both LEA producers. Both MOVEMs therefore select
  # their validated immutable values and complete to the existing frontier.
  first = subprocess.run([executable, "--emit-general-startup-runtime-c4-movem-adjacent-lea"],
                          text=True, capture_output=True)
  second = subprocess.run([executable, "--emit-general-startup-runtime-c4-movem-adjacent-lea"],
                           text=True, capture_output=True)
  assert first.returncode == second.returncode == 0
  assert first.stdout == second.stdout
  assert not first.stdout.startswith("/* translation rejected:")
  assert first.stdout.count("genesis_route_access(") == 4
  assert first.stdout.count("m68k_movem_use_fold_") > 0
  assert "UINT32_C(0x0000FFFE)" in first.stdout
  assert "UINT32_C(0x80000001)" in first.stdout

  # Strict-C11 compile and real normal-entry semantic execution.
  with tempfile.TemporaryDirectory() as temp:
    path = pathlib.Path(temp)
    (path / "generated.c").write_text(first.stdout)
    (path / "harness.c").write_text(HARNESS)
    build = subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
          "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path),
          str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"),
         "-o", str(path / "movem-adjacent-lea")],
        text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "movem-adjacent-lea")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr

  # The absolute-word producer's raw sign-extended base participates in the
  # same live-state guard and still selects its immutable values after LEA.
  sign_extend_first = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-movem-adjacent-lea-absolute-word-sign-extend"],
      text=True, capture_output=True)
  sign_extend_second = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-movem-adjacent-lea-absolute-word-sign-extend"],
      text=True, capture_output=True)
  assert sign_extend_first.returncode == sign_extend_second.returncode == 0, sign_extend_first.stderr
  assert sign_extend_first.stdout == sign_extend_second.stdout
  assert not sign_extend_first.stdout.startswith("/* translation rejected:"), sign_extend_first.stdout
  assert sign_extend_first.stdout.count("genesis_route_access(") == 2
  assert "UINT32_C(0xFFFF8B16)" in sign_extend_first.stdout
  assert "UINT32_C(0x00009ABC)" in sign_extend_first.stdout
  assert "UINT32_C(0x0000007F)" in sign_extend_first.stdout

  with tempfile.TemporaryDirectory() as temp:
    path = pathlib.Path(temp)
    (path / "generated.c").write_text(sign_extend_first.stdout)
    (path / "harness.c").write_text(SIGN_EXTEND_HARNESS)
    build = subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
          "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path),
          str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"),
         "-o", str(path / "movem-adjacent-lea-sign-extend")],
        text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "movem-adjacent-lea-sign-extend")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr

  # Directly dispatch to the MOVEM interior label with A1 pointing at live
  # work RAM whose words differ from the adjacent LEA's ROM-derived facts.
  # The same-block form aliases B06 to the B00 owner; the split form owns B06
  # separately. Both must route through architectural state and produce the
  # same result, proving semantic partition invariance.
  interior_sources = []
  for option in (
      "--emit-general-startup-runtime-c4-movem-adjacent-lea-interior",
      "--emit-general-startup-runtime-c4-movem-adjacent-lea-interior-split",
  ):
    emitted = subprocess.run([executable, option], text=True, capture_output=True)
    repeated = subprocess.run([executable, option], text=True, capture_output=True)
    assert emitted.returncode == repeated.returncode == 0, emitted.stderr
    assert emitted.stdout == repeated.stdout
    assert not emitted.stdout.startswith("/* translation rejected:"), emitted.stdout
    assert emitted.stdout.count("genesis_route_access(") == 2
    interior_sources.append(emitted.stdout)
  assert "{ UINT32_C(0x00000B06), genesis_block_00000B00 }" in rows(interior_sources[0])
  assert "case UINT32_C(0x00000B06): goto genesis_instruction_00000B06;" in interior_sources[0]
  assert "m68k_movem_use_fold_" in interior_sources[0]
  assert "{ UINT32_C(0x00000B06), genesis_block_00000B06 }" in rows(interior_sources[1])
  assert "m68k_movem_use_fold_" in interior_sources[1]

  # Entering through LEA makes the same validated fold available regardless
  # of whether static discovery represents the adjacent instructions in one
  # block or across its unique fallthrough-continuation partition.
  normal_results = []
  for index, source_text in enumerate(interior_sources):
    with tempfile.TemporaryDirectory() as temp:
      path = pathlib.Path(temp)
      (path / "generated.c").write_text(source_text)
      (path / "harness.c").write_text(NORMAL_INTERIOR_HARNESS)
      program = path / f"movem-normal-{index}"
      build = subprocess.run(
          [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
           "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path),
           str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"),
           "-o", str(program)], text=True, capture_output=True)
      assert build.returncode == 0, build.stderr
      ran = subprocess.run([str(program)], text=True, capture_output=True)
      assert ran.returncode == 0, ran.stderr
      normal_results.append(ran.stdout)
  assert normal_results[0] == normal_results[1]

  results = []
  for index, source_text in enumerate(interior_sources):
    with tempfile.TemporaryDirectory() as temp:
      path = pathlib.Path(temp)
      (path / "generated.c").write_text(source_text)
      (path / "harness.c").write_text(INTERIOR_HARNESS)
      program = path / f"movem-interior-{index}"
      build = subprocess.run(
          [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
           "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path),
           str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"),
           "-o", str(program)], text=True, capture_output=True)
      assert build.returncode == 0, build.stderr
      ran = subprocess.run([str(program)], text=True, capture_output=True)
      assert ran.returncode == 0, ran.stderr
      results.append(ran.stdout)
  assert results[0] == results[1]

  # A partition is not enough by itself: only the unique static fallthrough
  # relation preserves the producer fact. A branch relation or an ambiguous
  # second incoming relation remains fail-closed.
  for forge in ("interior-split-branch", "interior-split-ambiguous"):
    rejected = subprocess.run(
        [executable, f"--emit-general-startup-runtime-c4-movem-adjacent-lea-{forge}"],
        text=True, capture_output=True)
    assert rejected.returncode == 0, (forge, rejected.stderr)
    assert rejected.stdout.startswith("/* translation rejected:"), forge

  # Negative cases (a)-(g): each proves the existing routed/fail-closed
  # behavior is preserved unchanged -- generated C is not rejected, but the
  # named MOVEM transfer's slots remain routed through genesis_route_access
  # exactly as before this task, never folded.
  for forge, expected_route_calls in (
      ("different-register", 2),      # (a) producer/consumer name different An.
      ("intervening-write", 3),       # (b) an intervening MOVEA also writes An
                                       #     (2 MOVEM slots + the MOVEA's own
                                       #     routed register-indirect read).
      ("cross-block", 2),             # (c) producer and consumer in different blocks.
      ("nonfoldable-producer", 2),    # (d) LEA's own source EA is not foldable.
      ("out-of-range", 2),            # (e) transfer span overruns the mapping claim.
      ("store-direction", 2),         # (f) registers_to_memory (wrong direction).
      ("disp16", 2),                  # (g) d16(An) -- excluded EA family.
  ):
    negative = subprocess.run(
        [executable, f"--emit-general-startup-runtime-c4-movem-adjacent-lea-{forge}"],
        text=True, capture_output=True)
    assert negative.returncode == 0, (forge, negative.stderr)
    assert not negative.stdout.startswith("/* translation rejected:"), forge
    assert negative.stdout.count("genesis_route_access(") == expected_route_calls, forge

  # (h) A forged/stale propagated M68kMovemAdjacentLeaFact: the independent
  # C4 re-verification function (valid_c4_movem_adjacent_lea_fact) rejects
  # the whole translation, mirroring the established valid_c4_static_memory_
  # fact forged-fact negative-test convention (SEG-007-T067/T072's own
  # "fact-duplicate"/"fact-unbound" cases) exactly.
  for forge in ("fact-duplicate", "fact-unbound"):
    forged = subprocess.run(
        [executable, f"--emit-general-startup-runtime-c4-movem-adjacent-lea-{forge}"],
        text=True, capture_output=True)
    assert forged.returncode == 0, forge
    assert forged.stdout.startswith("/* translation rejected:"), forge

  print("genesis_startup_runtime_c4_movem_adjacent_lea_test: OK")


if __name__ == "__main__":
  main()
