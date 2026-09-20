#!/usr/bin/env python3
"""SEG-007-T205: above-cap complete semantic-frontier obligation set reaches
generated-native execution.

`--emit-general-startup-runtime-c4-frontier-above-cap` emits the generated C
for a genuinely representable 65-exit fixture (`m68k_discovery_max_frontier_
exits` chained BNE.w branches, each with its own taken RESET sibling, plus one
terminal RESET primary) -- one exit more than the diagnostic-projection bound.
Before SEG-007-T205, C4's own entry point (`libs/codegen/c11/src/frontend.cpp`)
rejected any `partial.frontiers` above that same bound outright, even though
ADR-0028 Section 8 / SEG-007-T183 already made the bound a diagnostic-
reporting-only limit upstream. This proves the correction reaches strict-C11
compiled, generated-native execution -- not merely a unit-level struct
assertion (`t205_c4_emits_a_complete_frontier_set_exceeding_the_diagnostic_cap`
in `tests/m68k_pipeline_test.cpp`) -- by actually dispatching the compiled
generated C from the entry block and reaching one of the above-cap synthesized
frontier stops.
"""
import pathlib
import subprocess
import sys
import tempfile

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;

  /* Z (bit 2) clear: every BNE.w is taken, reaching the very first chained
     branch's own taken RESET sibling at 0x00003E02 -- one of the 65
     above-cap complete semantic-frontier obligations. */
  runtime.pc = UINT32_C(0x00003D00);
  runtime.sr = UINT16_C(0x0000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(transfer.stop.provenance.instruction.source_address == UINT32_C(0x00003E02));
  assert(transfer.stop.provenance.instruction.primary_bytes[0] == UINT8_C(0x4E));
  assert(transfer.stop.provenance.instruction.primary_bytes[1] == UINT8_C(0x70));

  return 0;
}
'''


def main():
    executable, compiler, root = sys.argv[1:]
    generated = subprocess.run([executable, "--emit-general-startup-runtime-c4-frontier-above-cap"],
                                text=True, capture_output=True)
    assert generated.returncode == 0, generated.stderr
    assert not generated.stdout.startswith("/* translation rejected"), generated.stdout
    # 64 sibling exits + 1 terminal primary exit == 65, one more than the
    # unchanged m68k_discovery_max_frontier_exits (64) diagnostic-reporting
    # bound -- every one of them reaches a real emitted frontier stop
    # function, none silently dropped or coalesced by the correction.
    assert generated.stdout.count("static GenesisControlTransfer genesis_frontier_stop_") == 65
    assert generated.stdout.count("static GenesisControlTransfer genesis_block_") == 64
    with tempfile.TemporaryDirectory() as temp:
        path = pathlib.Path(temp)
        (path / "generated.c").write_text(generated.stdout)
        (path / "harness.c").write_text(HARNESS)
        build = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
              "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"),
              str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "frontier-above-cap")],
            text=True, capture_output=True)
        assert build.returncode == 0, build.stderr
        ran = subprocess.run([str(path / "frontier-above-cap")], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr
    print("genesis startup runtime C4 frontier above-cap: ok")


if __name__ == "__main__":
    main()
