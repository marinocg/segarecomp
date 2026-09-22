#!/usr/bin/env python3
"""SEG-007-T238/SEG-021-T026: strict-C11 compile/execute proof for the build-time-only,
explicitly opt-in broad aligned-M68k-ROM AOT representation experiment.

Proves the ONE observable contract the experiment's seam guarantees on a
project-authored synthetic fixture: a valid, independently decoded/lowered
aligned candidate reaches real, dispatchable generated code through the
static precompiled-entry lookup, and the generated runtime never fetches or
decodes a target byte to do it.
"""

import json
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
  GenesisReportMetadata metadata = {0};
  static const char digest[] = "0000000000000000000000000000000000000000000000000000000000000000";

  /* The ordinary CFG root remains selectable beside the independent AOT
     identities. Its (A1) read supplies the dynamic JSR destination. */
  runtime.pc = UINT32_C(0x00000C00);
  runtime.a[1] = UINT32_C(0x00FF0000);
  runtime.a[7] = UINT32_C(0x00FF0100);
  runtime.work_ram[0] = UINT8_C(0x0C);
  runtime.work_ram[1] = UINT8_C(0x06);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C06));

  /* Dispatch directly to V1 (base+0x06): a valid, experimentally-proposed
     aligned entry admitted through the unmodified existing decode/lowering
     walk. It must reach real generated code and execute its own BRA.S back
     to the ingress (base = 0x00000C00). */
  runtime.pc = UINT32_C(0x00000C06);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C00));
  assert(runtime.d[3] == UINT32_C(0)); /* compiled data remains inert */

  /* Dispatch to V2 (base+0x08): the second independently admitted valid
     experimental entry. */
  runtime.pc = UINT32_C(0x00000C08);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C00));

  /* The invalid-encoding and excluded-form candidate addresses never became
     emitted-code representation: dispatching to either must fail closed
     after lookup misses, never by fetching or decoding a byte there. */
  runtime.pc = UINT32_C(0x00000C0A);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind != GENESIS_CONTINUE_AT_PC);

  runtime.pc = UINT32_C(0x00000C0C);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind != GENESIS_CONTINUE_AT_PC);

  /* Two valid starts overlap: the second starts in the first instruction's
     extension word, but each has its own PC-keyed compiled body. */
  runtime.pc = UINT32_C(0x00000C12);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C14));
  assert(runtime.d[3] == UINT32_C(0));

  /* The same bytes classified above as inert data execute only when the
     architectural PC explicitly selects their compiled identity. */
  runtime.pc = UINT32_C(0x00000C14);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(runtime.d[3] == UINT32_C(5));

  /* SEG-021-T026 actual-cause regression. The final valid AOT identity is a
     MULS whose exact sequential PC has no compiled entry or pre-existing
     frontier. Its semantics retire, then its generation-time-derived
     relation frontier fails closed with source provenance instead of handing
     an unrepresented PC to the internal dispatcher. The NOP->MOVEQ case above
     is the positive near-neighbor: its exact successor is compiled and still
     returns CONTINUE_AT_PC normally. */
  runtime.pc = UINT32_C(0x00000C62);
  runtime.d[2] = UINT32_C(2);
  runtime.d[3] = UINT32_C(3);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET);
  assert(transfer.stop.diagnostic_category == GENESIS_DIAG_KNOWN_BUT_UNEMITTED_TARGET);
  assert(transfer.stop.provenance.has_instruction_provenance != 0U);
  assert(transfer.stop.provenance.instruction.source_address == UINT32_C(0x00000C62));
  assert(runtime.pc == UINT32_C(0x00000C64));

  /* Exercise the same serializer used by generated main. An AOT-only source
     must carry the accepted mapping/fetch provenance required for canonical
     reporting; success prints one sanitized JSON line and returns zero. */
  metadata.cpu_dimensions = GENESIS_CPU_DIMENSIONS_NONE;
  return genesis_write_sanitized_report(&transfer, digest, &metadata);
}
'''


def main() -> None:
    emitter, compiler, root = sys.argv[1:]
    generated = subprocess.run(
        [emitter, "--emit-experiment-aligned-aot"],
        text=True, capture_output=True,
    )
    assert generated.returncode == 0, generated.stderr
    assert not generated.stdout.startswith("/* translation rejected:")
    assert "static GenesisCompiledEntry genesis_compiled_entry_lookup(uint32_t address)" in generated.stdout
    assert "while (low < high)" in generated.stdout
    assert "GenesisCompiledEntry entry = genesis_compiled_entry_lookup(runtime->pc)" in generated.stdout
    assert "if (runtime->pc == UINT32_C(0x00000C06))" not in generated.stdout
    assert "GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET" in generated.stdout
    attachment = generated.stdout.split(
        "void genesis_attach_route_provenance", 1
    )[1].split("GenesisControlTransfer genesis_static_stop", 1)[0]
    mismatch_body = generated.stdout.split(
        "genesis_aot_00000C62(GenesisRuntime *runtime) {", 1
    )[1].split("static const GenesisCompiledEntryRecord", 1)[0]
    assert "source->source_address == UINT32_C(0x00000C62)" not in attachment
    assert attachment.count("source->source_address ==") == 1
    assert mismatch_body.count(
        "frontier.stop.provenance.mapping_claim_count = UINT8_C(1)"
    ) == 1
    assert mismatch_body.count(
        "frontier.stop.provenance.bus_access_count = UINT8_C(1)"
    ) == 1
    # Fixture-local output-size ratchet: the prior broad AOT attachment table
    # duplicated provenance for every aligned identity and crossed this bound.
    # Local mismatch producers keep the complete generated source bounded.
    assert len(generated.stdout.encode("utf-8")) < 100_000

    repeated = subprocess.run(
        [emitter, "--emit-experiment-aligned-aot"],
        text=True, capture_output=True,
    )
    assert repeated.returncode == 0, repeated.stderr
    assert repeated.stdout == generated.stdout

    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(generated.stdout, encoding="utf-8")
        (path / "harness.c").write_text(HARNESS, encoding="utf-8")
        runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "experiment-aligned-aot"
        built = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(runtime_dir), "-I", str(path), str(path / "harness.c"),
             str(runtime_dir / "runtime.c"), "-o", str(executable)],
            text=True, capture_output=True,
        )
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr
        report = json.loads(ran.stdout)
        assert report["schema_version"] == 1
        assert report["report_kind"] == "sanitized"
        assert report["result"] == "stop"
        assert report["stop_class"] == "known_but_unemitted_target"
        assert report["diagnostic_category"] == "known_but_unemitted_target"
        assert report["cpu_dimensions"] is None
        assert report["c4_lowering_dimensions"] is None

    print("genesis_experiment_aligned_aot_generated_test: OK")


if __name__ == "__main__":
    main()
