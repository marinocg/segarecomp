#!/usr/bin/env python3
"""SEG-007-T050 synthetic end-to-end coverage (no ROM input).

Proves the wiring this task adds: runner (genesis_runtime_run) -> checkpoint
entry (T131's sticky classification mechanism) -> T042's exact two-post-entry
-VBlank-transition stable-frame condition -> T049's existing
genesis_vdp_produce_frame composition owner -> a real, deterministic
GenesisFrameArtifact -> T131's existing GenesisCheckpointEvidenceBundle.frame
member -> genesis_write_checkpoint_evidence_summary's frame-present
reporting.

Every VDP register/VRAM/CRAM/VSRAM byte pattern here is project-authored and
synthetic (no commercial ROM content). The expected frame_digest is computed
independently in this harness (over a hand-derived all-zero pixel/palette
buffer, using the exact same already-shared genesis_sha256_* primitives
runtime.c/vdp_render.c themselves reuse -- this is the one project-wide SHA-
256 implementation, not a re-derivation of a second one) rather than trusted
from genesis_vdp_produce_frame's own output, satisfying the "independently
constructed" requirement for this wiring-level test (pixel-level composition
correctness across plane/sprite/palette content is vdp_render.c's own
existing, separate test files' job, not this one's).
"""
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
#include "vdp_render.h"

#define SYNTHETIC_CHECKPOINT_TARGET UINT32_C(0x00654321)

static GenesisControlTransfer drive_to_checkpoint(GenesisRuntime *runtime) {
  GenesisControlTransfer result = {0};
  if (runtime->pc == 0U) {
    result.kind = GENESIS_CONTINUE_AT_PC;
    result.next_pc = SYNTHETIC_CHECKPOINT_TARGET;
  } else {
    result.kind = GENESIS_COMPLETE;
  }
  return result;
}

/* Never resolves to the classified target -- proves runner-allowance
   exhaustion alone can never satisfy the stable-frame condition. */
static GenesisControlTransfer drive_never_checkpoints(GenesisRuntime *runtime) {
  GenesisControlTransfer result = {0};
  (void)runtime;
  result.kind = GENESIS_CONTINUE_AT_PC;
  result.next_pc = 0U;
  return result;
}

#if defined(SEGARECOMP_TEST_CHECKPOINT_CLASSIFY_TARGET)
/* Minimal valid Mode-5/H40/non-interlaced/full-screen-H/2-cell-V register
   state. Every other register (plane bases, plane size, SAT base, register
   #7 backdrop) is left at 0; with all-zero VRAM/CRAM/VSRAM every plane and
   sprite pattern pixel decodes to palette index 0 (always transparent, see
   vdp_render.h's C4 citation), so every one of the 320x224 screen pixels
   resolves to the register #7 backdrop -- itself CRAM entry 0, decoding to
   RGB {0,0,0} from an all-zero CRAM buffer. This is a fully valid,
   deterministic render used only to prove WIRING; pixel-content correctness
   across real plane/sprite data is vdp_render.c's own separate test files'
   job. */
static void make_valid_registers(uint16_t regs[GENESIS_VDP_REGISTER_COUNT]) {
  memset(regs, 0, sizeof(uint16_t) * GENESIS_VDP_REGISTER_COUNT);
  regs[1] = 0x04;  /* Mode 5 select. */
  regs[11] = 0x04; /* full-screen H-scroll, 2-cell V-scroll. */
  regs[12] = 0x81; /* H40, non-interlaced. */
}
#endif

static void identity_setup(GenesisCheckpointIdentity *identity) {
  memset(identity, 0, sizeof(*identity));
  memcpy(identity->checkpoint_id, "frametest", 9U);
  identity->checkpoint_id_length = 9U;
  memcpy(identity->rom_sha256,
         "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 65U);
  identity->options.schema_version = GENESIS_DETERMINISTIC_OPTIONS_SCHEMA_VERSION;
  identity->options.stable_frame_vblank_count = GENESIS_STABLE_FRAME_VBLANK_COUNT;
}

int main(void) {
  GenesisCheckpointIdentity identity;
  GenesisTransactionEvidence transaction = {0};
  identity_setup(&identity);

  /* --- Negative: runner-allowance exhaustion alone is never success. --- */
  {
    GenesisRuntime runtime = {0};
    GenesisCheckpointEvidenceBundle bundle;
    memset(&bundle, 0xA5, sizeof(bundle));
    GenesisControlTransfer result = genesis_runtime_run(&runtime, drive_never_checkpoints, 8U);
    assert(result.kind == GENESIS_RUNNER_RESOURCE_LIMIT);
    assert(runtime.devices.interrupt.checkpoint_entered == 0U);
    assert(genesis_extract_checkpoint_evidence(&runtime, &identity, &transaction, genesis_vdp_produce_frame, &bundle) == 0);
  }

  /* --- Negative: no checkpoint reached at all (production classifier is
     UNKNOWN-only in this translation unit unless the test macro below is
     defined). --- */
#if !defined(SEGARECOMP_TEST_CHECKPOINT_CLASSIFY_TARGET)
  {
    GenesisRuntime runtime = {0};
    GenesisCheckpointEvidenceBundle bundle;
    memset(&bundle, 0xA5, sizeof(bundle));
    GenesisControlTransfer result = genesis_runtime_run(&runtime, drive_to_checkpoint, 2U);
    assert(result.kind == GENESIS_COMPLETE);
    assert(runtime.devices.interrupt.checkpoint_entered == 0U);
    assert(genesis_extract_checkpoint_evidence(&runtime, &identity, &transaction, genesis_vdp_produce_frame, &bundle) == 0);
  }
#else
  /* --- Checkpoint IS reached (via the real runner + T131 sticky
     classification mechanism) when the test-only classify macro names this
     harness's synthetic target. --- */
  {
    GenesisRuntime runtime = {0};
    GenesisControlTransfer result = genesis_runtime_run(&runtime, drive_to_checkpoint, 2U);
    assert(result.kind == GENESIS_COMPLETE);
    assert(runtime.devices.interrupt.checkpoint_entered == 1U);
    assert(runtime.devices.interrupt.vblank_transition_count_at_checkpoint_entry == 0U);

    /* --- Negative: exactly one post-entry VBlank transition (T042 section
       13's condition requires >= 2). --- */
    {
      GenesisRuntime one_vblank = runtime;
      GenesisCheckpointEvidenceBundle bundle;
      memset(&bundle, 0xA5, sizeof(bundle));
      one_vblank.devices.interrupt.vblank_transition_count = 1U;
      assert(genesis_extract_checkpoint_evidence(&one_vblank, &identity, &transaction, genesis_vdp_produce_frame, &bundle) == 0);
    }

    /* --- Negative: two post-entry VBlank transitions, but the reached VDP
       register state is outside vdp_render.c's documented bound surface
       (all-zero registers here -- Mode 5 not even selected): extraction
       still succeeds (cpu/ram/device/transaction evidence remain valid on
       their own), but bundle.frame stays exactly the never-rendered `{0}`
       value, and the summary must report frame-not-ready / non-pass. --- */
    {
      GenesisRuntime unsupported = runtime;
      GenesisCheckpointEvidenceBundle bundle;
      GenesisCheckpointEvidenceSummary summary = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
      memset(&bundle, 0xA5, sizeof(bundle));
      unsupported.devices.interrupt.vblank_transition_count = 2U;
      assert(genesis_extract_checkpoint_evidence(&unsupported, &identity, &transaction, genesis_vdp_produce_frame, &bundle) == 1);
      { GenesisFrameArtifact zero_frame = {0};
        assert(memcmp(&bundle.frame, &zero_frame, sizeof(bundle.frame)) == 0); }
      assert(genesis_write_checkpoint_evidence_summary(&bundle, &summary) == 0);
    }

    /* --- Positive: two post-entry VBlank transitions AND a valid Mode-5/
       H40/non-interlaced/full-screen-H/2-cell-V register state -> a real
       deterministic frame is produced and wired into the bundle. --- */
    {
      GenesisRuntime ready = runtime;
      make_valid_registers(ready.devices.vdp.registers);
      ready.devices.interrupt.vblank_transition_count = 2U;
      GenesisCheckpointEvidenceBundle bundle;
      memset(&bundle, 0xA5, sizeof(bundle));
      assert(genesis_extract_checkpoint_evidence(&ready, &identity, &transaction, genesis_vdp_produce_frame, &bundle) == 1);

      /* Independently-derived expected pixel/palette content and digest:
         every plane/sprite pattern pixel is transparent (all-zero VRAM), so
         every one of the 320*224 pixels resolves to the register #7
         backdrop, itself CRAM entry 0 from an all-zero CRAM buffer -- i.e.
         every pixel byte is exactly 0 and the whole palette_snapshot is
         exactly 0. This expectation is hand-derived from the documented
         six-layer priority/transparency rule (vdp_render.h's own C4
         citations), never obtained by calling genesis_vdp_produce_frame or
         genesis_vdp_compose_pixel to produce it. */
      { uint8_t expected_pixels[GENESIS_FRAME_WIDTH * GENESIS_FRAME_HEIGHT];
        uint8_t expected_palette[GENESIS_VDP_CRAM_BYTES];
        uint8_t expected_digest[32];
        GenesisSha256 digest;
        memset(expected_pixels, 0, sizeof(expected_pixels));
        memset(expected_palette, 0, sizeof(expected_palette));
        assert(memcmp(bundle.frame.pixels, expected_pixels, sizeof(expected_pixels)) == 0);
        assert(memcmp(bundle.frame.palette_snapshot, expected_palette,
                      sizeof(expected_palette)) == 0);
        genesis_sha256_init(&digest);
        genesis_sha256_update(&digest, expected_pixels, sizeof(expected_pixels));
        genesis_sha256_update(&digest, expected_palette, sizeof(expected_palette));
        genesis_sha256_final(&digest, expected_digest);
        assert(memcmp(bundle.frame.frame_digest, expected_digest, 32U) == 0);
        /* A genuine SHA-256 digest is (overwhelmingly, deterministically for
           THIS exact fixture) never literally all-zero -- confirming the
           sentinel genesis_write_checkpoint_evidence_summary relies on to
           detect "really rendered" actually distinguishes this bundle from
           the never-rendered case above. */
        { uint8_t all_zero[32] = {0};
          assert(memcmp(expected_digest, all_zero, 32U) != 0); }
      }

      /* Deterministic repeat: identical input state extracts a byte-
         identical frame (same digest), matching this bundle's existing
         whole-bundle determinism guarantee. */
      { GenesisCheckpointEvidenceBundle repeat;
        memset(&repeat, 0x5A, sizeof(repeat));
        assert(genesis_extract_checkpoint_evidence(&ready, &identity, &transaction, genesis_vdp_produce_frame, &repeat) == 1);
        assert(memcmp(bundle.frame.frame_digest, repeat.frame.frame_digest, 32U) == 0);
        assert(memcmp(bundle.bundle_digest, repeat.bundle_digest, 32U) == 0); }

      { GenesisCheckpointEvidenceSummary summary = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        assert(genesis_write_checkpoint_evidence_summary(&bundle, &summary) == 0); }
    }
  }
#endif
  return 0;
}
'''


def main() -> int:
    root = pathlib.Path(sys.argv[2])
    compiler = sys.argv[1]
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory)
        source = path / "checkpoint_frame.c"
        source.write_text(HARNESS)
        common = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                  "-I", str(root / "platforms/genesis/runtime"), str(source),
                  str(root / "platforms/genesis/runtime/runtime.c"),
                  str(root / "platforms/genesis/runtime/vdp_render.c")]

        no_checkpoint_binary = path / "checkpoint_frame_no_checkpoint"
        build = subprocess.run([*common, "-o", str(no_checkpoint_binary)],
                               text=True, capture_output=True)
        if build.returncode:
            raise RuntimeError(build.stderr)
        result = subprocess.run([str(no_checkpoint_binary)], text=True, capture_output=True)
        if result.returncode:
            raise RuntimeError(result.stderr or result.stdout)

        reached_binary = path / "checkpoint_frame_reached"
        reached_build = subprocess.run(
            [*common, "-DSEGARECOMP_TEST_CHECKPOINT_CLASSIFY_TARGET=UINT32_C(0x00654321)",
             "-o", str(reached_binary)], text=True, capture_output=True)
        if reached_build.returncode:
            raise RuntimeError(reached_build.stderr)
        reached_result = subprocess.run([str(reached_binary)], text=True, capture_output=True)
        if reached_result.returncode:
            raise RuntimeError(reached_result.stderr or reached_result.stdout)
        summaries = [json.loads(line) for line in reached_result.stdout.splitlines() if line]
        if len(summaries) != 2:
            raise RuntimeError("synthetic checkpoint harness did not emit both summaries")
        ready_summary = summaries[-1]
        if (ready_summary.get("present", {}).get("frame") is not True or
                ready_summary.get("frame_state") != "ready" or
                ready_summary.get("verdict") != "pass"):
            raise RuntimeError("ready frame summary did not report present.frame/frame_state/verdict")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
