#!/usr/bin/env python3
"""SEG-021-T028: generated-C execution of the Case C ownership fallback.

A legal `JMP (0,PC,D0.W)` has a finite Tier-1 set whose candidates fail target
admission; the source must own exactly the existing Tier-2 fact.  Project-authored
synthetic image only.  A runtime value selecting the represented target
dispatches; values selecting absent targets stop at the Tier-2 membership
diagnostic with no dispatch and no stack mutation.  Strict C11.
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

ACTUAL_SOURCE = r'''#include <stdint.h>
#include <stdio.h>
#include "runtime.h"
#include "generated.c"
static void run(uint32_t d0) {
  GenesisRuntime r = {0};
  GenesisControlTransfer t;
  r.pc = UINT32_C(START_PC);
  r.a[7] = UINT32_C(0x00FF0100);
  r.d[0] = d0;
  t = genesis_bridge_dispatch(&r);
  if (t.kind == GENESIS_CONTINUE_AT_PC)
    printf("continue %08X\n", (unsigned)t.next_pc);
  else
    printf("stop %d %d a7=%08X\n", (int)t.stop.stop_class,
           (int)(t.stop.diagnostic_category == GENESIS_DIAG_TIER2_COMPUTED_TARGET_NOT_EMITTED), (unsigned)r.a[7]);
}
int main(void) { VALUES return 0; }
'''


def checked(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, **kwargs)
    assert result.returncode == 0, (args, result.stderr)
    return result


def run_case(executable, compiler, mode, start_pc, values, expected):
    generated = checked([executable, mode]).stdout
    with tempfile.TemporaryDirectory() as directory:
        temp = pathlib.Path(directory)
        (temp / "generated.c").write_text(generated)
        (temp / "actual.c").write_text(ACTUAL_SOURCE.replace("START_PC", start_pc).replace(
            "VALUES", " ".join("run(%dU);" % v for v in values)))
        actual = temp / "actual"
        checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-I", str(ROOT / "platforms/genesis/runtime"), "-I", str(temp), str(temp / "actual.c"),
                 str(ROOT / "platforms/genesis/runtime/runtime.c"), "-o", str(actual)])
        lines = checked([str(actual)]).stdout.splitlines()
    assert lines == expected, lines
    print("PC-indexed Tier-2 ownership fixture ok:", mode)


def main():
    executable, compiler = sys.argv[1:3]
    stop = "stop 4 1 a7=00FF0100"
    # Discovery-stage seam (Case C): a finite candidate fails target admission.
    # D0=2 selects the represented target; 0/4/6 select absent targets.
    run_case(executable, compiler, "--emit-t028-case-c-pc-index-tier2", "0x00000C00", [2, 0, 4, 6],
             ["continue 00000C08", stop, stop, stop])
    # Emission-stage seam: retained Tier-1 set with an unrepresentable (orphan RTS) candidate.
    # D0 & 4 selects 0x0B0A / 0x0B0E; neither is a member of the compiled emitted set (the Tier-1
    # candidate blocks are only reachable through this source), so both fail closed without dispatch.
    run_case(executable, compiler, "--emit-t028-orphan-tier2", "0x00000B00", [0, 4],
             [stop, stop])


if __name__ == "__main__":
    main()
