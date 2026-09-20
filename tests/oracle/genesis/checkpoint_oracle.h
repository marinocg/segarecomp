#ifndef SEGARECOMP_TESTS_ORACLE_GENESIS_CHECKPOINT_ORACLE_H
#define SEGARECOMP_TESTS_ORACLE_GENESIS_CHECKPOINT_ORACLE_H

/*
 * SEG-007-T132 (hardened): the independent checkpoint-evidence oracle's
 * single ADR-0012 Decision 4 public entry point. This header includes ONLY
 * the shared neutral schema header; it never includes any other production
 * header (see tests/genesis_checkpoint_oracle_independence_test.py's grep
 * gate). It also never declares the fixture-taking synthetic/internal
 * entry point -- that lives in the test-only
 * checkpoint_oracle_test_support.h, included only by
 * tests/tools/genesis_checkpoint_oracle_fixture_test.c.
 */
#include "../../../platforms/genesis/runtime/checkpoint_evidence.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Typed oracle result codes. Every derive entry point below returns one of
 * these -- never a bare, undocumented magic number.
 */
#define GENESIS_CHECKPOINT_ORACLE_OK 0
/* rom_image/bundle_out/fixture NULL, an out-of-range rom_len, a malformed
 * GenesisDeterministicOptions field, or (public entry only) `options` not
 * byte-for-byte equal to `identity.options` (the ADR-0012 Decision 4
 * signature carries both; this oracle treats any disagreement between the
 * two as a caller bug and fails closed rather than silently preferring
 * one). */
#define GENESIS_CHECKPOINT_ORACLE_ERROR_INVALID_ARGUMENT 1
/* The instruction budget was exhausted before the checkpoint-entry and
 * stable-frame VBlank conditions were both satisfied. */
#define GENESIS_CHECKPOINT_ORACLE_ERROR_CHECKPOINT_NOT_REACHED 2
/* The CPU issued a bus access (read or write, any device or memory lane)
 * this oracle does not recognise, or recognises at an address but not at
 * the width/shape it was written for (see checkpoint_oracle.c's per-device
 * fault-latch documentation). Once latched, the step loop stops immediately
 * and no bundle is populated. */
#define GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_ACCESS 3
/* The checkpoint's VDP display-enable state is one this oracle's bounded
 * background-only frame model cannot faithfully represent (display
 * rendering enabled -- see oracle_display_state_is_supported's citation). */
#define GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_DISPLAY_STATE 4

/*
 * The ADR-0012 Decision 4 public oracle entry point.
 *
 * Runs the pinned Musashi MC68000 core (via this file's own thin bus
 * adapter) over `rom_image`, applying only the bounded device semantics
 * documented in checkpoint_oracle.c, until either:
 *   (a) a checkpoint PC classified as non-UNKNOWN by this oracle's own
 *       (real, non-test) classifier is reached AND at least
 *       `options.stable_frame_vblank_count` VBlank rising-edge transitions
 *       have since been observed through a routed status read, in which
 *       case a full evidence bundle is derived and
 *       GENESIS_CHECKPOINT_ORACLE_OK is returned with `*bundle_out`
 *       populated; or
 *   (b) `options.instruction_budget` CPU steps elapse without satisfying
 *       that condition, in which case
 *       GENESIS_CHECKPOINT_ORACLE_ERROR_CHECKPOINT_NOT_REACHED is returned
 *       and `*bundle_out` is left completely untouched; or
 *   (c) an unsupported device access or display state is observed, in
 *       which case GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_ACCESS /
 *       _UNSUPPORTED_DISPLAY_STATE is returned and `*bundle_out` is left
 *       completely untouched.
 *
 * IMPORTANT: this entry point cannot currently succeed. `GenesisCheckpointPcClass`
 * has no non-UNKNOWN enumerator yet -- T131/T132 established the sticky
 * checkpoint-entry *mechanism* but bound no real Sonic checkpoint target to
 * it (see runtime.c's own identically-shaped
 * genesis_checkpoint_pc_class_for_target, which this oracle's classifier
 * mirrors). Because the classifier this function uses always returns
 * GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN in a normal (non-test) build,
 * `checkpoint_entered` can never become true, the step loop always
 * exhausts `options.instruction_budget`, and case (b) above is the only
 * reachable outcome today -- case (a)'s bundle-population code is provably
 * unreachable by construction, not merely by convention. Success requires a
 * FUTURE task to both bind a non-UNKNOWN checkpoint class (mirroring
 * runtime.c's own precedent) and resolve real VBlank/interrupt timing;
 * neither is in this task's bounded scope.
 *
 * `options` and `identity.options` must describe the same deterministic
 * options (see GENESIS_CHECKPOINT_ORACLE_ERROR_INVALID_ARGUMENT above);
 * `identity` must already carry the correct `rom_sha256` of `rom_image`
 * (this function does not compute or check it against `rom_image` itself).
 * `rom_image`/`rom_len` are the caller's own ROM bytes (never a commercial
 * image for any bundled fixture/test in this repository).
 */
int genesis_checkpoint_oracle_derive(const uint8_t *rom_image, size_t rom_len,
                                      GenesisDeterministicOptions options,
                                      GenesisCheckpointIdentity identity,
                                      GenesisCheckpointEvidenceBundle *bundle_out);

#ifdef __cplusplus
}
#endif

#endif
