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
static const uint32_t genesis_compiled_entry_addresses[] = {
  UINT32_C(0x00000010),
  UINT32_C(0x00000020),
};
static const uint8_t genesis_compiled_entry_owner_ids[] = {
  UINT8_C(0),
  UINT8_C(1),
};
static const GenesisCompiledEntry genesis_compiled_owners[] = {
  genesis_block_00000010,
  genesis_aot_00000020,
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

# SEG-022-T002: `measure` must use the streaming --generated-c-output path, never stdout.
import argparse
import os
import stat

FAKE = r"""#!/usr/bin/env python3
import sys
a = sys.argv
mode = a[a.index("--fake-mode") + 1] if "--fake-mode" in a else "ok"
if "--generated-c-output" not in a:
    sys.stdout.write("int stdout_route;\n"); raise SystemExit(0)   # legacy route: must not be used
out = a[a.index("--generated-c-output") + 1]
open(a[a.index("--immutable-aot-address-report") + 1], "w").write("4\n6\n")
if mode == "shard":   # SEG-022-T003: a large program is emitted as a translation-unit set instead of `out`
    import os
    sd = a[a.index("--generated-c-shard-dir") + 1]
    os.makedirs(sd)
    open(sd + "/bridge_generated.h", "w").write("#ifndef H\n#endif\n")
    open(sd + "/bridge_generated_main.c", "w").write("int main(void) { return 0; }\n")
    open(sd + "/bridge_generated_entries_00.c", "w").write(
        "static const uint32_t genesis_compiled_entry_addresses[] = {\n  UINT32_C(0x00000004),\n  UINT32_C(0x00000006),\n};\n"
        "static const uint8_t genesis_compiled_entry_owner_ids[] = {\n  UINT8_C(0),\n  UINT8_C(1),\n};\n"
        "static const GenesisCompiledEntry genesis_compiled_owners[] = {\n  genesis_block_00000004,\n  genesis_aot_00000006,\n};\n")
    open(sd + "/bridge_generated.units", "w").write("bridge_generated_main.c\nbridge_generated_entries_00.c\n")
    raise SystemExit(0)
if mode == "fail":
    open(out + ".partial", "w").write("partial"); raise SystemExit(1)
if mode == "nofile":
    raise SystemExit(0)
open(out, "w").write("int streamed_route;\n")
"""

import subprocess


def portable_timed(command, stdout=None, cwd=None):
    """Portable stand-in for the /usr/bin/time wrapper: runs the fake emitter via this interpreter;
    any other command (the compiler) is reported as failed without being executed."""
    if not command[0].endswith("fake.py"):
        return {"returncode": 1, "wall_seconds": 0.0, "peak_rss_bytes": None, "stderr": ""}
    done = subprocess.run([sys.executable] + command, stdout=stdout or subprocess.DEVNULL,
                          stderr=subprocess.PIPE, text=True, cwd=cwd)
    return {"returncode": done.returncode, "wall_seconds": 0.0, "peak_rss_bytes": None, "stderr": done.stderr}


gcs.timed = portable_timed
with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d)
    def run(mode):
        fake = d / "fake.py"
        fake.write_text(FAKE.replace('"ok"', repr(mode)))
        rom = d / "r.bin"; rom.write_bytes(b"x")
        (d / "out").mkdir(exist_ok=True)
        (d / "out" / "generated.c").write_text("stale")
        ns = argparse.Namespace(segarecomp=str(fake), rom=str(rom), external_hints=None, out_dir=str(d / "out"),
                                cc="/usr/bin/false", opt="-O0", product_root=str(d), jobs=1)
        return gcs.measure(ns)
    ok = run("ok")
    assert ok["generation"]["returncode"] == 0 and (d / "out" / "generated.c").read_text() == "int streamed_route;\n", ok
    assert not (d / "out" / "admitted_aot_addresses.txt").exists()
    for mode in ("fail", "nofile"):
        bad = run(mode)
        assert bad["generation"]["returncode"] != 0, (mode, bad)
        assert "source" not in bad
        assert not (d / "out" / "generated.c").exists() and not (d / "out" / "generated.c.partial").exists()


    # SEG-022-T003: sharded output is measured per translation unit; the final compiled-address set
    # is read from the entries TU; no single `generated.c` exists.
    sharded = run("shard")
    assert sharded["generation"]["returncode"] == 0, sharded
    units = sharded["source"]["translation_units"]
    assert units["count"] == 2 and units["total_bytes"] > 0 and units["header_bytes"] > 0, units
    assert sharded["fingerprints"]["final_compiled_entry_address_set"] == gcs.set_fingerprint([4, 6]), sharded["fingerprints"]
    assert sharded["fingerprints"]["aot_owned_entry_address_set"] == gcs.set_fingerprint([6])
    assert sharded["compile"]["translation_units"]["count"] == 2
print("ok-sharded")

# SEG-022-T010: grouped AOT owners (non-static, per-owner entry switch/labels) across several files
# plus the header must still partition exactly and classify the owner dispatch separately.
OWNER_TU = b"""#include "bridge_generated.h"
GenesisControlTransfer genesis_aot_owner_0000(GenesisRuntime *runtime) {
  switch (runtime->pc) {
  case UINT32_C(0x00000004): goto genesis_aot_entry_00000004;
  default: return genesis_internal_dispatch_inconsistency_stop(runtime);
  }
genesis_aot_entry_00000004: {
  uint32_t pc = runtime->pc;
  pc += UINT32_C(4);
  runtime->pc = pc;
  return genesis_runtime_retire_m68k_instruction(runtime, UINT32_C(8), runtime->pc);
}
}
"""
OWNER_HEADER = b"GenesisControlTransfer genesis_aot_owner_0000(GenesisRuntime *runtime);\n"
with tempfile.TemporaryDirectory() as tmp:
    tu = pathlib.Path(tmp) / "a.c"
    header = pathlib.Path(tmp) / "b.h"
    tu.write_bytes(OWNER_TU)
    header.write_bytes(OWNER_HEADER)
    multi = gcs.attribute([tu, header])
assert multi["total_bytes"] == len(OWNER_TU) + len(OWNER_HEADER)
assert multi["category_sum_bytes"] == multi["total_bytes"], multi
assert multi["category_bytes"]["unattributed_residual"] == 0, multi
assert multi["category_bytes"]["owner_entry_dispatch"] > 0, multi
assert multi["counts"]["aot_function"] == 1 and multi["counts"]["aot_entry_label"] == 1, multi
assert multi["category_bytes"]["immutable_rom_aot_bodies"] > 0, multi
