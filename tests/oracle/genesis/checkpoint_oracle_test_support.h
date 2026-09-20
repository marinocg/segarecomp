#ifndef SEGARECOMP_TESTS_ORACLE_GENESIS_CHECKPOINT_ORACLE_TEST_SUPPORT_H
#define SEGARECOMP_TESTS_ORACLE_GENESIS_CHECKPOINT_ORACLE_TEST_SUPPORT_H

/*
 * SEG-007-T132 (hardened): the internal/synthetic test-only oracle entry
 * point and its fixture-stimulus type. This header, and everything it
 * declares, is NOT part of ADR-0012 Decision 4's public oracle contract --
 * a real production caller supplies only a ROM, deterministic options, and
 * a checkpoint identity; it never supplies an externally chosen checkpoint
 * PC or a list of VBlank-read PCs. Both belong ONLY to this repository's own
 * hand-authored synthetic mini-ROM fixtures.
 *
 * #include ONLY from tests/tools/genesis_checkpoint_oracle_fixture_test.c
 * (or an equivalent future oracle-internal test file); never from
 * checkpoint_oracle.h or any ADR-0012-facing production-adjacent header.
 */
#include "checkpoint_oracle.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SEG-007-T132: this oracle has no real Sonic checkpoint bound yet (the
 * runtime-selected frontier for that binding is still a pre-execution
 * static-discovery admission refusal; see the SEG-007-T133/T134 chain). Its
 * "checkpoint" is therefore a trivial, hand-authored, project-invented
 * condition over a synthetic mini-ROM: PC reaching `checkpoint_pc`, and
 * (separately) the CPU issuing a VDP status-register read while the fixture
 * driver has externally asserted the documented VBlank bit -- see
 * checkpoint_oracle.c's block comment for the full citation and rationale.
 * This struct is intentionally NOT part of the shared
 * GenesisCheckpointIdentity/GenesisDeterministicOptions schema: it is
 * oracle-fixture-only stimulus scheduling, never claimed as hardware or as
 * a schema-owned type, and is exposed only to this test-only header, never
 * to checkpoint_oracle.h.
 */
typedef struct GenesisCheckpointOracleFixture {
  uint32_t checkpoint_pc;
  /* Addresses (in the mini-ROM's own address space) of every CPU
   * instruction that reads the VDP status port ($C00004). Before executing
   * an instruction at one of these addresses, the oracle sets the
   * documented VBlank-pending status bit as if the display hardware had
   * just asserted it; the routed read observes and latches the transition
   * exactly like a real CPU-visible status read would. */
  uint32_t vblank_read_pc[8];
  uint32_t vblank_read_pc_count;
} GenesisCheckpointOracleFixture;

/*
 * The internal/synthetic test-only oracle entry point. Shares the public
 * genesis_checkpoint_oracle_derive's underlying step-loop/extraction engine
 * (checkpoint_oracle.c), but drives checkpoint-entry detection directly off
 * `fixture->checkpoint_pc` (never through the real, always-UNKNOWN public
 * classifier), and applies `fixture->vblank_read_pc` as external VBlank
 * stimulus. This is the ONLY oracle entry point that can currently reach
 * the bundle-population code, because it is the only one whose
 * checkpoint-entry condition can ever become true. See
 * checkpoint_oracle.h's `genesis_checkpoint_oracle_derive` doc comment for
 * the full return-code/failure-mode contract, which this function shares.
 *
 * `identity` (not a separate options argument -- `identity.options` is
 * already the schema's one authoritative deterministic-options value) must
 * already carry the correct `rom_sha256` of `rom_image` (this function does
 * not compute or check it against `rom_image` itself). `rom_image`/`rom_len`
 * are the caller's own synthetic, project-authored mini-ROM bytes (never a
 * commercial image).
 */
int genesis_checkpoint_oracle_derive_synthetic(const uint8_t *rom_image, size_t rom_len,
                                                GenesisCheckpointIdentity identity,
                                                const GenesisCheckpointOracleFixture *fixture,
                                                GenesisCheckpointEvidenceBundle *bundle_out);

#ifdef __cplusplus
}
#endif

#endif
