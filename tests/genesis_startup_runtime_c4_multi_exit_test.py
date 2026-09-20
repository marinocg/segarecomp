#!/usr/bin/env python3
"""SEG-007-T064 Checkpoint 4: runtime-selected multi-exit dispatch.

Proves the bounded multi-exit lowering is genuinely runtime-selected, not
compile-time-collapsed: the same compiled generated C, dispatched twice with
two independently constructed initial ``GenesisRuntime`` states, reaches a
*different* retained exit each time because the entry block's own emitted C
reads ``runtime->sr`` at execution time, exactly like any other Bcc.

The fixture (``--emit-general-startup-runtime-c4-multi-exit``) is a BNE at
the entry with a fallthrough RESET (``unsupported_cpu_form``) and a taken
MOVE.L (0x00200000).L,D0 (``unsupported_memory_region``): two independently
retained exits sharing one block, per
``general_startup_promotes_two_branches_to_two_distinct_precise_frontier_classes``
in ``tests/m68k_pipeline_test.cpp``, this time compiled and actually run.
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
  GenesisRuntime not_equal = {0};
  GenesisRuntime equal = {0};
  GenesisControlTransfer transfer;

  /* Z (bit 2) clear: BNE is taken, reaching the MOVE.L memory frontier. */
  not_equal.pc = UINT32_C(0x00000B00);
  not_equal.sr = UINT16_C(0x0000);
  transfer = genesis_bridge_dispatch(&not_equal);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_MEMORY_REGION);
  assert(transfer.stop.provenance.instruction.source_address == UINT32_C(0x00000B06));
  assert(transfer.stop.provenance.has_access == 1U);
  assert(transfer.stop.provenance.access_address == UINT32_C(0x00200000));

  /* Z (bit 2) set: BNE is not taken, reaching the fallthrough CPU frontier. */
  equal.pc = UINT32_C(0x00000B00);
  equal.sr = UINT16_C(0x0004);
  transfer = genesis_bridge_dispatch(&equal);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(transfer.stop.provenance.instruction.source_address == UINT32_C(0x00000B02));
  assert(transfer.stop.provenance.has_access == 0U);

  return 0;
}
'''


def main():
    executable, compiler, root = sys.argv[1:]
    generated = subprocess.run([executable, "--emit-general-startup-runtime-c4-multi-exit"],
                                text=True, capture_output=True)
    assert generated.returncode == 0, generated.stderr
    assert "genesis_frontier_stop_00000B02" in generated.stdout
    assert "genesis_frontier_stop_00000B06" in generated.stdout
    # SEG-007-T174 / ADR-0024 correction: this fixture never opts into any
    # `code_entry_candidate` (`validated_code_entry_candidate_roots` is
    # empty), so Tier 2 can never engage anywhere in this build and C4
    # emission keeps the exact pre-ADR-0024 single-pass shape -- each
    # frontier stop gets exactly one real definition, never also a separate
    # forward declaration. 2 exits * 1 definition each = 2 occurrences of
    # this exact prefix. (An earlier version of this assertion incorrectly
    # expected 4, matching a real defect where the two-pass forward-declare/
    # late-definition shape was applied unconditionally, even to a route
    # with zero candidates; SEG-007-T174 fixed C4 emission to gate that
    # two-pass shape on `validated_code_entry_candidate_roots` being
    # non-empty, restoring byte-for-byte compatibility with the pre-ADR-0024
    # generated text for every route that never opts into candidate-assisted
    # discovery.)
    assert generated.stdout.count("static GenesisControlTransfer genesis_frontier_stop_") == 2
    with tempfile.TemporaryDirectory() as temp:
        path = pathlib.Path(temp)
        (path / "generated.c").write_text(generated.stdout)
        (path / "harness.c").write_text(HARNESS)
        build = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
              "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"),
              str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "multi-exit")],
            text=True, capture_output=True)
        assert build.returncode == 0, build.stderr
        ran = subprocess.run([str(path / "multi-exit")], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr
    print("genesis startup runtime C4 multi-exit: ok")


if __name__ == "__main__":
    main()
