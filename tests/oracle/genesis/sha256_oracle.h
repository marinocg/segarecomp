#ifndef SEGARECOMP_TESTS_ORACLE_GENESIS_SHA256_ORACLE_H
#define SEGARECOMP_TESTS_ORACLE_GENESIS_SHA256_ORACLE_H

/*
 * SEG-007-T132: a standalone, from-scratch FIPS 180-4 SHA-256 implementation
 * used ONLY by the independent checkpoint-evidence oracle
 * (tests/oracle/genesis). It shares no code, translation unit, or object
 * file with platforms/genesis/runtime/runtime.c's own SHA-256 (genesis_sha256_*); see
 * ADR-0012 Decision 5 (independence audit) and
 * tests/genesis_checkpoint_oracle_independence_test.py, which mechanically
 * checks that no production symbol appears in any oracle binary.
 *
 * Reference: NIST FIPS PUB 180-4, "Secure Hash Standard (SHS)", section 6.2
 * ("SHA-256"). This is the primary standard defining the algorithm; it is
 * not derived from, or checked against, this project's production
 * implementation.
 */

#include <stddef.h>
#include <stdint.h>

typedef struct OracleSha256 {
  uint32_t h[8];
  uint8_t block[64];
  uint32_t used;
  uint64_t length;
} OracleSha256;

void oracle_sha256_init(OracleSha256 *state);
void oracle_sha256_update(OracleSha256 *state, const uint8_t *data, uint32_t count);
void oracle_sha256_final(OracleSha256 *state, uint8_t digest[32]);

/* Convenience big-endian scalar helpers matching contract section 14's
 * "big-endian integers" serialization rule. */
void oracle_sha256_put_u8(OracleSha256 *state, uint8_t value);
void oracle_sha256_put_u16(OracleSha256 *state, uint16_t value);
void oracle_sha256_put_u32(OracleSha256 *state, uint32_t value);
void oracle_sha256_put_u64(OracleSha256 *state, uint64_t value);

#endif
