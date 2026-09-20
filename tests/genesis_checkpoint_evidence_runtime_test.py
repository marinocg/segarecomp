#!/usr/bin/env python3
"""SEG-007-T131 synthetic runtime substrate coverage (no ROM input)."""
import pathlib
import json
import subprocess
import sys
import tempfile


HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"

#define SYNTHETIC_CHECKPOINT_TARGET UINT32_C(0x00123456)

_Static_assert(GENESIS_MAX_RAW_BYTES == SEGARECOMP_GENESIS_MAX_RAW_INSTRUCTION_BYTES,
               "production raw-byte aliases disagree");
_Static_assert(GENESIS_Z80_RAM_BYTES == SEGARECOMP_GENESIS_Z80_RAM_WINDOW_BYTES,
               "production Z80 RAM aliases disagree");

static GenesisControlTransfer drive_dispatch(GenesisRuntime *runtime) {
  GenesisControlTransfer result = {0};
  if (runtime->pc == 0U) {
    result.kind = GENESIS_CONTINUE_AT_PC;
    result.next_pc = SYNTHETIC_CHECKPOINT_TARGET;
  } else {
    result.kind = GENESIS_COMPLETE;
  }
  return result;
}

static void assert_rejected_without_output_mutation(const GenesisRuntime *runtime,
                                                    const GenesisCheckpointIdentity *identity,
                                                    const GenesisTransactionEvidence *transaction) {
  GenesisCheckpointEvidenceBundle output;
  GenesisCheckpointEvidenceBundle before;
  memset(&output, 0xA5, sizeof(output));
  before = output;
  assert(genesis_extract_checkpoint_evidence(runtime, identity, transaction, 0, &output) == 0);
  assert(memcmp(&output, &before, sizeof(output)) == 0);
}

int main(void) {
  GenesisRuntime runtime = {0}, again = {0};
  GenesisRuntimeStop stop = {0};
  GenesisCheckpointIdentity identity = {0};
  GenesisTransactionEvidence transaction = {0};
  GenesisCheckpointEvidenceBundle bundle = {0};
  GenesisCheckpointEvidenceBundle canonical_repeat = {0};
  /* Every real category (cpu/ram/device/transaction) is present and
     matching, and the caller's own verdict_passed claims pass -- but
     frame_present/frame_matches are 0 because frame is never ready, so
     genesis_write_checkpoint_evidence_summary must still force "fail". */
  GenesisCheckpointEvidenceSummary summary = {1,1,1,1,0,1,1,1,1,0,1};
  /* A lying caller: verdict_passed=1 AND frame_present=1 (claiming frame is
     ready). The emitted verdict must still be forced to "fail" because this
     function enforces the invariant itself rather than trusting the caller. */
  GenesisCheckpointEvidenceSummary lying_summary = {1,1,1,1,1,1,1,1,1,1,1};
  uint32_t value;
  (void)lying_summary; /* only the forcing-variant build (see the Python driver) prints with this */

  assert(runtime.devices.interrupt.vblank_pending == 0U);
  assert(runtime.devices.interrupt.vblank_transition_count == 0U);
  assert(runtime.devices.interrupt.checkpoint_entered == 0U);
  /* T131 has no production checkpoint class, so the production classifier is
     an UNKNOWN no-op even when the shared drive dispatcher sees this synthetic
     resolved target. */
  assert(genesis_runtime_run(&runtime, drive_dispatch, 2U).kind == GENESIS_COMPLETE);
  assert(runtime.devices.interrupt.checkpoint_entered == 0U);
  value = UINT32_C(0x0008); /* synthetic documented VBlank status observation */
  runtime.devices.vdp.status_register = (uint16_t)value;
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                              GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.interrupt.vblank_pending == 1U);
  assert(runtime.devices.interrupt.vblank_transition_count == 1U);
  assert(runtime.devices.interrupt.vblank_status_read_count == 1U);
  assert(genesis_route_access(&runtime, UINT32_C(0x00C00004), GENESIS_ACCESS_WORD,
                              GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.devices.interrupt.vblank_transition_count == 1U); /* no overwrite on 1 -> 1 */
  assert(runtime.devices.interrupt.vblank_status_read_count == 2U);

  /* Extraction fails closed before its checkpoint observation-count condition. */
  assert(genesis_extract_checkpoint_evidence(&runtime, &identity, &transaction, 0, &bundle) == 0);
  runtime.devices.interrupt.checkpoint_entered = 1U;
  runtime.devices.interrupt.vblank_transition_count_at_checkpoint_entry = 1U;
  runtime.devices.interrupt.vblank_transition_count = 3U;
  memcpy(identity.checkpoint_id, "synthetic", 9U);
  identity.checkpoint_id_length = 9U;
  memcpy(identity.rom_sha256, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 65U);
  identity.options.schema_version = GENESIS_DETERMINISTIC_OPTIONS_SCHEMA_VERSION;
  identity.options.stable_frame_vblank_count = GENESIS_STABLE_FRAME_VBLANK_COUNT;
  transaction.has_full_transactions = 1U;
  transaction.transaction_count = 1U;
  transaction.transactions[0].ordinal = 0U;
  transaction.transactions[0].kind = GENESIS_BUS_DATA_READ;
  transaction.transactions[0].region = GENESIS_REGION_SYNTHETIC_WORK_RAM;
  transaction.transactions[0].raw_byte_count = 1U;
  transaction.transactions[0].raw_bytes[0] = UINT8_C(0x5A);
  assert(genesis_extract_checkpoint_evidence(&runtime, &identity, &transaction, 0, &bundle) == 1);
  assert(bundle.schema_version == GENESIS_CHECKPOINT_EVIDENCE_SCHEMA_VERSION);
  assert(bundle.cpu.pc_class == GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN);
  assert(memcmp(&bundle.device.devices, &runtime.devices, sizeof(runtime.devices)) == 0);
  assert(memcmp(&bundle.frame, &(GenesisFrameArtifact){0}, sizeof(bundle.frame)) == 0);

  /* Identical synthetic state extracts byte-identically. */
  again = runtime;
  { GenesisCheckpointEvidenceBundle repeated = {0};
    transaction.transactions[0].raw_bytes[1] = UINT8_C(0xA5); /* excluded trailing capacity */
    assert(genesis_extract_checkpoint_evidence(&again, &identity, &transaction, 0, &repeated) == 1);
    assert(memcmp(bundle.bundle_digest, repeated.bundle_digest, sizeof(bundle.bundle_digest)) == 0);
    assert(memcmp(bundle.transaction.transaction_digest, repeated.transaction.transaction_digest,
                  sizeof(bundle.transaction.transaction_digest)) == 0);
    canonical_repeat = repeated; }
  transaction.transactions[0].raw_byte_count = GENESIS_MAX_RAW_BYTES + 1U;
  assert(genesis_extract_checkpoint_evidence(&runtime, &identity, &transaction, 0, &canonical_repeat) == 0);
  transaction.transactions[0].raw_byte_count = 1U;
  transaction.transactions[0].kind = (GenesisBusKind)99;
  assert(genesis_extract_checkpoint_evidence(&runtime, &identity, &transaction, 0, &canonical_repeat) == 0);
  transaction.transactions[0].kind = GENESIS_BUS_DATA_READ;
  transaction.transactions[0].region = (GenesisBusRegion)99;
  assert(genesis_extract_checkpoint_evidence(&runtime, &identity, &transaction, 0, &canonical_repeat) == 0);
  transaction.transactions[0].region = GENESIS_REGION_SYNTHETIC_WORK_RAM;

  /* Every identity bound and extraction-policy value is rejected before the
     caller-owned output bundle can change. */
  identity.checkpoint_id_length = GENESIS_MAX_NAME_LENGTH + 1U;
  assert_rejected_without_output_mutation(&runtime, &identity, &transaction);
  identity.checkpoint_id_length = 9U;
  identity.checkpoint_id[0] = '!';
  assert_rejected_without_output_mutation(&runtime, &identity, &transaction);
  identity.checkpoint_id[0] = 's';
  identity.rom_sha256[64] = 'x';
  assert_rejected_without_output_mutation(&runtime, &identity, &transaction);
  identity.rom_sha256[64] = '\0';
  identity.options.schema_version = 0U;
  assert_rejected_without_output_mutation(&runtime, &identity, &transaction);
  identity.options.schema_version = GENESIS_DETERMINISTIC_OPTIONS_SCHEMA_VERSION;
  identity.options.stable_frame_vblank_count = GENESIS_STABLE_FRAME_VBLANK_COUNT - 1U;
  assert_rejected_without_output_mutation(&runtime, &identity, &transaction);
  identity.options.stable_frame_vblank_count = GENESIS_STABLE_FRAME_VBLANK_COUNT;

  /* Two modular rising transitions from a snapshot near UINT32_MAX satisfy
     the extraction observation-count requirement. */
  runtime.devices.interrupt.vblank_transition_count_at_checkpoint_entry = UINT32_MAX;
  runtime.devices.interrupt.vblank_transition_count = 1U;
  assert(genesis_extract_checkpoint_evidence(&runtime, &identity, &transaction, 0, &canonical_repeat) == 1);
  runtime.devices.interrupt.vblank_transition_count_at_checkpoint_entry = 1U;
  runtime.devices.interrupt.vblank_transition_count = 3U;

  { GenesisCheckpointEvidenceBundle invalid_summary = bundle;
    invalid_summary.identity.options.schema_version = 0U;
    assert(genesis_write_checkpoint_evidence_summary(&invalid_summary, &summary) == 1); }
  assert(genesis_write_checkpoint_evidence_summary(&bundle, &summary) == 0);
  return 0;
}
'''


def main() -> int:
    root = pathlib.Path(sys.argv[2])
    compiler = sys.argv[1]
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory)
        source = path / "checkpoint.c"
        binary = path / "checkpoint"
        source.write_text(HARNESS)
        common = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                  "-I", str(root / "platforms/genesis/runtime"), str(source),
                  str(root / "platforms/genesis/runtime/runtime.c")]
        build = subprocess.run([*common, "-o", str(binary)], text=True, capture_output=True)
        if build.returncode:
            raise RuntimeError(build.stderr)
        schema = path / "schema_only.c"
        schema.write_text(
            '#include "checkpoint_evidence.h"\n'
            '_Static_assert(GENESIS_MAX_RAW_BYTES == 12U, "standalone raw literal changed");\n'
            '_Static_assert(GENESIS_Z80_RAM_BYTES == 8192U, "standalone Z80 literal changed");\n'
            'int main(void) { return GENESIS_FRAME_WIDTH == 320U ? 0 : 1; }\n')
        schema_build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                       "-I", str(root / "platforms/genesis/runtime"), str(schema),
                                       "-o", str(path / "schema_only")], text=True, capture_output=True)
        if schema_build.returncode:
            raise RuntimeError(schema_build.stderr)
        result = subprocess.run([str(binary)], text=True, capture_output=True)
        if result.returncode:
            raise RuntimeError(result.stderr)
        output = result.stdout
        forbidden = ("digest", "work_ram", "vram", "cram", "pixels", "options", "transaction_digest")
        if any(item in output for item in forbidden):
            raise RuntimeError("sanitized summary exposed a forbidden raw/digest field")
        report = json.loads(output)
        if report.get("checkpoint_id") != "synthetic" or report.get("verdict") != "fail":
            raise RuntimeError("sanitized summary omitted required classification")
        if report.get("present", {}).get("frame") is not False or report.get("frame_state") != "not_ready":
            raise RuntimeError("sanitized summary did not report the inert frame placeholder")
        if "frame" in report.get("matches", {}):
            raise RuntimeError("sanitized summary claimed a frame match")
        if output.count("\n") != 1:
            raise RuntimeError("rejected summary emitted output")

        # SEG-007-T132: synthetic proof that a lying caller (verdict_passed=1
        # AND frame_present=1, claiming frame is ready) still gets a forced
        # "fail" verdict -- genesis_write_checkpoint_evidence_summary enforces
        # the "no PASS while frame is absent/not-ready" invariant itself.
        forcing_source = path / "checkpoint_forcing.c"
        forcing_source.write_text(HARNESS.replace(
            "assert(genesis_write_checkpoint_evidence_summary(&bundle, &summary) == 0);",
            "assert(genesis_write_checkpoint_evidence_summary(&bundle, &lying_summary) == 0);"))
        forcing_binary = path / "checkpoint_forcing"
        forcing_build = subprocess.run([*common[:-2], str(forcing_source), common[-1],
                                        "-o", str(forcing_binary)], text=True, capture_output=True)
        if forcing_build.returncode:
            raise RuntimeError(forcing_build.stderr)
        forcing_result = subprocess.run([str(forcing_binary)], text=True, capture_output=True)
        if forcing_result.returncode:
            raise RuntimeError(forcing_result.stderr)
        forcing_report = json.loads(forcing_result.stdout)
        if forcing_report.get("verdict") != "fail":
            raise RuntimeError("a lying caller's verdict_passed/frame_present=1 was not forced to fail")
        if forcing_report.get("present", {}).get("frame") is not False or \
                forcing_report.get("frame_state") != "not_ready":
            raise RuntimeError("forced-fail summary did not still report the inert frame placeholder")

        sticky_source = path / "checkpoint_sticky.c"
        sticky_source.write_text(HARNESS.replace(
            "assert(runtime.devices.interrupt.checkpoint_entered == 0U);\n  value =",
            "assert(runtime.devices.interrupt.checkpoint_entered == 1U);\n  "
            "assert(runtime.devices.interrupt.vblank_transition_count_at_checkpoint_entry == 0U);\n  "
            "runtime.pc = 0U; runtime.devices.interrupt.vblank_transition_count = 7U;\n  "
            "assert(genesis_runtime_run(&runtime, drive_dispatch, 2U).kind == GENESIS_COMPLETE);\n  "
            "assert(runtime.devices.interrupt.vblank_transition_count_at_checkpoint_entry == 0U);\n  "
            "runtime.devices.interrupt.vblank_transition_count = 0U;\n  value ="))
        sticky_binary = path / "checkpoint_sticky"
        sticky_build = subprocess.run([*common[:-2], str(sticky_source), common[-1],
                                       "-DSEGARECOMP_TEST_CHECKPOINT_CLASSIFY_TARGET=UINT32_C(0x00123456)",
                                       "-o", str(sticky_binary)], text=True, capture_output=True)
        if sticky_build.returncode:
            raise RuntimeError(sticky_build.stderr)
        sticky_result = subprocess.run([str(sticky_binary)], text=True, capture_output=True)
        if sticky_result.returncode:
            raise RuntimeError(sticky_result.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
