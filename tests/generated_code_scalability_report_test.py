#!/usr/bin/env python3
"""SEG-022-T001: the attribution categories must partition the generated source bytes exactly."""
import importlib.util
import pathlib
import sys
import tempfile

spec = importlib.util.spec_from_file_location("gcs", sys.argv[1])
gcs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gcs)

SAMPLE = b"""#include "x.h"
static GenesisControlTransfer genesis_frontier_stop_00000010(GenesisRuntime *runtime);
static GenesisControlTransfer genesis_block_00000010(GenesisRuntime *runtime) {
  uint32_t pc = runtime->pc;
const uint32_t v = 1;
  runtime->pc = pc;
}
static GenesisControlTransfer genesis_aot_00000020(GenesisRuntime *runtime) {
  uint32_t pc = runtime->pc;
const uint32_t v = 2;
  {
  static const uint32_t m68k_indirect_targets_00000030[] = {
    UINT32_C(0x00000010),
    UINT32_C(0x00000020),
    UINT32_C(0x00000030)
  };
  static const uint32_t m68k_indirect_targets_00000032[] = {UINT32_C(0x00000010), UINT32_C(0x00000020)};
  }
  runtime->pc = pc;
  if (runtime->pc == UINT32_C(0x22)) {
    GenesisInstructionProvenance source = {0};
    source.length = UINT32_C(2);
    GenesisControlTransfer frontier = {0};
    frontier.stop.provenance.mapping_claim_count = UINT8_C(1);
    return frontier;
  }
  { const uint32_t m68k_retirement_pc = runtime->pc; GenesisControlTransfer retired = r();
    return retired;
  }
}
static const GenesisCompiledEntryRecord genesis_compiled_entries[] = {
  { UINT32_C(0x00000010), genesis_block_00000010 },
  { UINT32_C(0x00000020), genesis_aot_00000020 }
};
static const uint8_t genesis_owned_region_data_0[] = { UINT8_C(0) };
int main(void) { return 0; }
"""

with tempfile.TemporaryDirectory() as tmp:
    path = pathlib.Path(tmp) / "g.c"
    path.write_bytes(SAMPLE)
    result = gcs.attribute(path)
assert result["total_bytes"] == len(SAMPLE)
assert result["category_sum_bytes"] == len(SAMPLE), result
assert result["category_bytes"]["unattributed_residual"] == 0, result
assert result["counts"]["aot_function"] == 1 and result["counts"]["ordinary_block"] == 1
assert result["counts"]["compiled_entry_rows"] == 2
assert result["category_bytes"]["mapping_metadata"] > 0
assert result["category_bytes"]["provenance"] > 0
assert result["category_bytes"]["owned_resolved_rom_literals"] > 0
ARRAY_BYTES = len(b"""  static const uint32_t m68k_indirect_targets_00000030[] = {
    UINT32_C(0x00000010),
    UINT32_C(0x00000020),
    UINT32_C(0x00000030)
  };
  static const uint32_t m68k_indirect_targets_00000032[] = {UINT32_C(0x00000010), UINT32_C(0x00000020)};
""")
assert result["indirect_target_arrays"] == {"array_count": 2, "element_count": 5, "bytes": ARRAY_BYTES,
                                            "max_elements_in_one_array": 3}, result["indirect_target_arrays"]
assert result["category_bytes"]["target_membership_structures"] >= ARRAY_BYTES
assert result["cells"].get("aot_function.body", {"bytes": 0})["bytes"] < len(SAMPLE) - ARRAY_BYTES
assert "aot_function.target_membership" in result["cells"]
assert result["compiled_entry_table"] == {"rows": 2, "rows_owned_by_ordinary_blocks": 1,
                                          "rows_owned_by_immutable_rom_aot": 1}
fp = result["fingerprints"]["final_compiled_entry_address_set"]
import hashlib
assert fp == {"count": 2, "sha256": hashlib.sha256(b"00000010\n00000020\n").hexdigest()}, fp
assert gcs.set_fingerprint([0x20, 0x10, 0x10]) == fp
metrics = gcs.parse_emitter_metrics("segarecomp: immutable-rom AOT enumeration: aligned_start_count=4 accepted_count=3 rejected_count=1\n")
assert metrics["immutable_rom_aot"] == {"aligned_start_count": 4, "accepted_count": 3, "rejected_count": 1}
print("ok")
