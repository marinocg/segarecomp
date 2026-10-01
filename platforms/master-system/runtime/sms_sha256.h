#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_SHA256_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* Streaming SHA-256 (FIPS 180-4) for the deterministic machine-state digest (ADR 0064 section 5). Plain C11, no I/O. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SmsSha256 {
  uint32_t h[8];
  uint8_t block[64];
  uint32_t block_used;
  uint64_t total_bytes;
} SmsSha256;

void sms_sha256_init(SmsSha256 *s);
void sms_sha256_update(SmsSha256 *s, const void *data, size_t size);
void sms_sha256_update_u8(SmsSha256 *s, uint8_t value);
void sms_sha256_update_u16(SmsSha256 *s, uint16_t value); /* little endian */
void sms_sha256_update_u32(SmsSha256 *s, uint32_t value); /* little endian */
void sms_sha256_update_u64(SmsSha256 *s, uint64_t value); /* little endian */
void sms_sha256_final(SmsSha256 *s, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif
