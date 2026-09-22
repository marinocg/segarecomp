#!/usr/bin/env python3
"""SEG-021-T010 correction: generated-native, execution-level regressions for the
routed auto-updating MULS.W/MULU.W/DIVS.W/DIVU.W source (`m68k_emit_routed_muldiv_auto_update`,
libs/codegen/c11/src/m68k.cpp) and the widened `(d8,PC,Xn)` DIVS.W/DIVU.W source. Reuses the
existing C4/AOT runtime infrastructure (`genesis_bridge_dispatch`, `platforms/genesis/runtime`)
exactly like every other emitted-C generated-native test in this suite; introduces no new runner.

1. MUL auto-update timing regression: proves the routed auto-update source feeds the shared
   dynamic MUL retirement expression (`m68k_timing_mul_source`) with the ACTUALLY FETCHED word,
   not the AOT/C4 block's zero-initialized default. Observed through `runtime.scheduler.
   master_ticks`, which `genesis_runtime_retire_m68k_instruction` advances by
   `retirement_cycles * GENESIS_M68K_CYCLE_MASTER_TICKS` deterministically (SEG-007-T047 / ADR-0020).
2. DIVS.W/DIVU.W `(d8,PC,Xn)` execution regression: proves the widened source EA this task adds
   for the whole mul/div family actually EXECUTES correctly for DIVS/DIVU too, not merely
   decodes/lifts (the shared EA-read primitive is identical to MULS.W's own, already validated
   against pinned Musashi by the T003 conformance rows).
3. DIVU auto-update exception-ordering regression: for a fetched-zero divisor, proves the routed
   source read succeeds, the source An auto-update commits exactly once, Dn is not written with a
   division result, vector 5 is raised through the existing ADR-0037 path (no fallthrough PC
   advance), and -- as a separate boundary -- a FAILED routed source access leaves An completely
   unchanged (never a partial auto-update before the divisor is even known).
"""
import pathlib
import subprocess
import sys
import tempfile

GENESIS_M68K_CYCLE_MASTER_TICKS = 7


def mulu_word_cycles(source: int) -> int:
    """Independent transcription of the generated `genesis_m68k_mulu_word_cycles` helper
    (libs/codegen/c11/src/frontend.cpp): 38 + 2 * popcount(source)."""
    return 38 + 2 * bin(source & 0xFFFF).count("1")


def muls_word_cycles(source: int) -> int:
    """Independent transcription of the generated `genesis_m68k_muls_word_cycles` helper:
    38 + 2 * (number of adjacent-bit transitions in (source << 1), bits 0..16)."""
    bits = (source & 0xFFFF) << 1
    n = 0
    for i in range(16):
        n += ((bits >> i) ^ (bits >> (i + 1))) & 1
    return 38 + 2 * n


POSTINC_WORD_EA_CYCLES = 4  # (An)+ word-size EA addend (matches the generated `+ UINT32_C(4)`)


def checked(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, **kwargs)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return result


def build_and_run(compiler, runtime_dir, runtime_c, work, name, generated, harness):
    (work / "generated.c").write_text(generated)
    (work / "harness.c").write_text(harness)
    binary = work / name
    checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(runtime_dir), "-I", str(work), str(work / "harness.c"), str(runtime_c),
             "-o", str(binary)])
    return checked([str(binary)])


def mul_auto_update_timing_regression(executable, compiler, runtime_dir, runtime_c, work):
    # MULU.W (A0)+,D1: source fetched from work_ram, D1 seeded to 1 so the product stays
    # observable-but-irrelevant -- only the RETIREMENT cycle count (master_ticks) is asserted.
    mulu_generated = checked([executable, "--emit-operation-c4-mulu-word-auto-update"]).stdout
    assert not mulu_generated.startswith("/* translation rejected:"), mulu_generated
    assert "m68k_timing_mul_source = (uint16_t)(m68k_routed_value_1);" in mulu_generated, \
        "the auto-update MULU path must capture the actual routed source, not stay zero-initialized"
    for source, label in ((0x0000, "zero"), (0xFFFF, "allones")):
        expected_ticks = (mulu_word_cycles(source) + POSTINC_WORD_EA_CYCLES) * GENESIS_M68K_CYCLE_MASTER_TICKS
        harness = f'''#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {{
  GenesisRuntime runtime = {{0}};
  runtime.pc = UINT32_C(0x00000B00);
  runtime.a[0] = UINT32_C(0x00FF0010);
  runtime.work_ram[0x10] = (uint8_t)(UINT32_C({source}) >> 8); runtime.work_ram[0x11] = (uint8_t)UINT32_C({source});
  runtime.d[1] = UINT32_C(1);
  const GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
  /* Retirement (and its master_ticks advance) happens before the deliberate RESET frontier
     sentinel every emit_operation_c4_* fixture ends with -- this fixture reaches STOP only
     AFTER retiring, so the assertion below is not vacuous. */
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.scheduler.master_ticks == UINT64_C({expected_ticks}));
  assert(runtime.a[0] == UINT32_C(0x00FF0012));
  return 0;
}}
'''
        build_and_run(compiler, runtime_dir, runtime_c, work, "mulu-auto-timing-" + label, mulu_generated, harness)

    # MULS.W (A0)+,D1: same proof for the signed sibling's own distinct cycle formula.
    muls_generated = checked([executable, "--emit-operation-c4-muls-word-auto-update"]).stdout
    assert not muls_generated.startswith("/* translation rejected:"), muls_generated
    assert "m68k_timing_mul_source = (uint16_t)(m68k_routed_value_1);" in muls_generated, \
        "the auto-update MULS path must capture the actual routed source, not stay zero-initialized"
    for source, label in ((0x0000, "zero"), (0xFFFF, "allones")):
        expected_ticks = (muls_word_cycles(source) + POSTINC_WORD_EA_CYCLES) * GENESIS_M68K_CYCLE_MASTER_TICKS
        harness = f'''#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {{
  GenesisRuntime runtime = {{0}};
  runtime.pc = UINT32_C(0x00000B00);
  runtime.a[0] = UINT32_C(0x00FF0010);
  runtime.work_ram[0x10] = (uint8_t)(UINT32_C({source}) >> 8); runtime.work_ram[0x11] = (uint8_t)UINT32_C({source});
  runtime.d[1] = UINT32_C(1);
  const GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.scheduler.master_ticks == UINT64_C({expected_ticks}));
  assert(runtime.a[0] == UINT32_C(0x00FF0012));
  return 0;
}}
'''
        build_and_run(compiler, runtime_dir, runtime_c, work, "muls-auto-timing-" + label, muls_generated, harness)
    # Sanity: the two chosen sources really do produce different expected retirement, so this
    # regression would genuinely fail at a head that always captures the zero default.
    assert mulu_word_cycles(0x0000) != mulu_word_cycles(0xFFFF)
    assert muls_word_cycles(0x0000) != muls_word_cycles(0xFFFF)


def div_pc_indexed_execution_regression(executable, compiler, runtime_dir, runtime_c, work):
    # DIVS.W (8,PC,D2.L),D3 / DIVU.W (8,PC,D2.L),D3: D2 (long index) plus the 8-bit displacement
    # must resolve to pc_base(0x00000B02) + D2 + 8 == the work-RAM divisor address (0x00FF0010).
    d2 = (0x00FF0010 - 0x00000B02 - 8) & 0xFFFFFFFF
    for flag, kind_label in (("--emit-operation-c4-divs-word-pc-indexed", "divs"),
                              ("--emit-operation-c4-divu-word-pc-indexed", "divu")):
        generated = checked([executable, flag]).stdout
        assert not generated.startswith("/* translation rejected:"), generated
        harness = f'''#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {{
  GenesisRuntime runtime = {{0}};
  runtime.pc = UINT32_C(0x00000B00);
  runtime.sr = UINT16_C(0x2700);
  runtime.d[2] = UINT32_C({d2});
  runtime.d[3] = UINT32_C(100);  /* dividend */
  runtime.work_ram[0x10] = 0U; runtime.work_ram[0x11] = 5U;  /* divisor = 5 (big-endian word) */
  const GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);  /* retires successfully, then the RESET frontier sentinel */
  assert(runtime.d[3] == UINT32_C(20));  /* 100 / 5 = 20 rem 0, packed (0<<16)|20 */
  assert(runtime.sr == UINT16_C(0x2700));  /* quotient positive and nonzero: N=0,Z=0,V=0,C=0 */
  return 0;
}}
'''
        build_and_run(compiler, runtime_dir, runtime_c, work, kind_label + "-pc-indexed", generated, harness)


def divu_auto_update_ordering_regression(executable, compiler, runtime_dir, runtime_c, work):
    generated = checked([executable, "--emit-operation-c4-divu-word-auto-update"]).stdout
    assert not generated.startswith("/* translation rejected:"), generated
    assert "runtime->a[0] = m68k_muldiv_auto_ea;" in generated
    # The commit statement must precede the divisor==0 raise -- the auto-update is unconditional
    # even though the exception is raised afterward.
    commit_index = generated.index("runtime->a[0] = m68k_muldiv_auto_ea;")
    raise_index = generated.index("genesis_raise_divide_by_zero(")
    assert commit_index < raise_index, "the source auto-update must commit before the divisor==0 raise"

    # Success boundary: the routed source read succeeds (divisor fetched as zero), the source A0
    # auto-update commits exactly once, D3 is NOT written with a division result, vector 5 is
    # raised through the existing ADR-0037 path, and there is no ordinary fallthrough PC advance
    # (the generated dispatcher never assigns runtime->pc on this path -- only transfer.next_pc,
    # which the caller, not this direct genesis_bridge_dispatch call, would apply).
    zero_divisor_harness = '''#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  runtime.pc = UINT32_C(0x00000B00);
  runtime.sr = UINT16_C(0x2700);
  runtime.a[0] = UINT32_C(0x00FF0020);
  runtime.a[7] = UINT32_C(0x00FF0100);
  runtime.d[3] = UINT32_C(0x12345678);
  runtime.work_ram[0x20] = 0U; runtime.work_ram[0x21] = 0U;  /* divisor word = 0 */
  runtime.divide_by_zero_handler_present = 1U;
  runtime.divide_by_zero_handler_entry = UINT32_C(0x00001234);
  const GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00001234));  /* vector-5 handler, never the 0xB02 fallthrough */
  assert(runtime.a[0] == UINT32_C(0x00FF0022));  /* source auto-update committed exactly once */
  assert(runtime.d[3] == UINT32_C(0x12345678));  /* Dn completely unwritten */
  return 0;
}
'''
    build_and_run(compiler, runtime_dir, runtime_c, work, "divu-auto-zero-divisor", generated, zero_divisor_harness)

    # Failure boundary: A0 points into the ROM window, so the routed WORD read of the divisor
    # fails before ANY architectural effect -- the source An auto-update never commits, and D3/SR
    # are completely untouched. A different boundary from the success case above: here the
    # operand access itself never completes, so it has no side effect at all (not even the
    # auto-update), whereas above the operand access DID complete and its side effect persists.
    access_failure_harness = '''#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  runtime.pc = UINT32_C(0x00000B00);
  runtime.sr = UINT16_C(0x2700);
  runtime.a[0] = UINT32_C(0x00000100);  /* ROM window: the routed read fails */
  runtime.d[3] = UINT32_C(0xABCDEF01);
  const GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.a[0] == UINT32_C(0x00000100));  /* no partial auto-update */
  assert(runtime.d[3] == UINT32_C(0xABCDEF01));
  assert(runtime.sr == UINT16_C(0x2700));
  return 0;
}
'''
    build_and_run(compiler, runtime_dir, runtime_c, work, "divu-auto-access-failure", generated, access_failure_harness)


def main():
    executable, compiler, root = sys.argv[1:]
    runtime_dir = pathlib.Path(root) / "platforms" / "genesis" / "runtime"
    runtime_c = runtime_dir / "runtime.c"
    with tempfile.TemporaryDirectory() as directory:
        work = pathlib.Path(directory)
        mul_auto_update_timing_regression(executable, compiler, runtime_dir, runtime_c, work)
        div_pc_indexed_execution_regression(executable, compiler, runtime_dir, runtime_c, work)
        divu_auto_update_ordering_regression(executable, compiler, runtime_dir, runtime_c, work)
    print("MUL auto-update timing, DIV (d8,PC,Xn) execution, and DIV auto-update ordering regressions pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
