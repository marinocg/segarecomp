/* SEG-007-T132: standalone SHA-256 (FIPS 180-4), written from scratch for
 * the independent oracle only. See sha256_oracle.h for the independence
 * rationale; nothing here calls, links, or includes any production file. */
#include "sha256_oracle.h"

static const uint32_t ORACLE_SHA256_K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

static uint32_t oracle_rotr(uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32U - bits));
}

static void oracle_sha256_compress(OracleSha256 *state, const uint8_t *block) {
  uint32_t w[64];
  uint32_t a, b, c, d, e, f, g, h;
  unsigned t;
  for (t = 0U; t < 16U; ++t)
    w[t] = ((uint32_t)block[t * 4U] << 24U) | ((uint32_t)block[t * 4U + 1U] << 16U) |
           ((uint32_t)block[t * 4U + 2U] << 8U) | (uint32_t)block[t * 4U + 3U];
  for (t = 16U; t < 64U; ++t) {
    uint32_t s0 = oracle_rotr(w[t - 15U], 7U) ^ oracle_rotr(w[t - 15U], 18U) ^ (w[t - 15U] >> 3U);
    uint32_t s1 = oracle_rotr(w[t - 2U], 17U) ^ oracle_rotr(w[t - 2U], 19U) ^ (w[t - 2U] >> 10U);
    w[t] = w[t - 16U] + s0 + w[t - 7U] + s1;
  }
  a = state->h[0]; b = state->h[1]; c = state->h[2]; d = state->h[3];
  e = state->h[4]; f = state->h[5]; g = state->h[6]; h = state->h[7];
  for (t = 0U; t < 64U; ++t) {
    uint32_t s1 = oracle_rotr(e, 6U) ^ oracle_rotr(e, 11U) ^ oracle_rotr(e, 25U);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t temp1 = h + s1 + ch + ORACLE_SHA256_K[t] + w[t];
    uint32_t s0 = oracle_rotr(a, 2U) ^ oracle_rotr(a, 13U) ^ oracle_rotr(a, 22U);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temp2 = s0 + maj;
    h = g; g = f; f = e; e = d + temp1;
    d = c; c = b; b = a; a = temp1 + temp2;
  }
  state->h[0] += a; state->h[1] += b; state->h[2] += c; state->h[3] += d;
  state->h[4] += e; state->h[5] += f; state->h[6] += g; state->h[7] += h;
}

void oracle_sha256_init(OracleSha256 *state) {
  static const uint32_t initial[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                       0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  unsigned i;
  for (i = 0U; i < 8U; ++i) state->h[i] = initial[i];
  state->used = 0U;
  state->length = 0U;
}

void oracle_sha256_update(OracleSha256 *state, const uint8_t *data, uint32_t count) {
  while (count != 0U) {
    uint32_t take = 64U - state->used;
    if (take > count) take = count;
    {
      uint32_t i;
      for (i = 0U; i < take; ++i) state->block[state->used + i] = data[i];
    }
    state->used += take;
    data += take;
    count -= take;
    state->length += take;
    if (state->used == 64U) {
      oracle_sha256_compress(state, state->block);
      state->used = 0U;
    }
  }
}

void oracle_sha256_final(OracleSha256 *state, uint8_t digest[32]) {
  uint64_t bit_length = state->length * 8U;
  unsigned i;
  state->block[state->used++] = 0x80U;
  if (state->used > 56U) {
    while (state->used < 64U) state->block[state->used++] = 0U;
    oracle_sha256_compress(state, state->block);
    state->used = 0U;
  }
  while (state->used < 56U) state->block[state->used++] = 0U;
  for (i = 0U; i < 8U; ++i) state->block[63U - i] = (uint8_t)(bit_length >> (i * 8U));
  oracle_sha256_compress(state, state->block);
  for (i = 0U; i < 8U; ++i) {
    digest[i * 4U] = (uint8_t)(state->h[i] >> 24U);
    digest[i * 4U + 1U] = (uint8_t)(state->h[i] >> 16U);
    digest[i * 4U + 2U] = (uint8_t)(state->h[i] >> 8U);
    digest[i * 4U + 3U] = (uint8_t)state->h[i];
  }
}

void oracle_sha256_put_u8(OracleSha256 *state, uint8_t value) { oracle_sha256_update(state, &value, 1U); }
void oracle_sha256_put_u16(OracleSha256 *state, uint16_t value) {
  uint8_t bytes[2];
  bytes[0] = (uint8_t)(value >> 8U);
  bytes[1] = (uint8_t)value;
  oracle_sha256_update(state, bytes, 2U);
}
void oracle_sha256_put_u32(OracleSha256 *state, uint32_t value) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(value >> 24U);
  bytes[1] = (uint8_t)(value >> 16U);
  bytes[2] = (uint8_t)(value >> 8U);
  bytes[3] = (uint8_t)value;
  oracle_sha256_update(state, bytes, 4U);
}
void oracle_sha256_put_u64(OracleSha256 *state, uint64_t value) {
  uint8_t bytes[8];
  unsigned i;
  for (i = 0U; i < 8U; ++i) bytes[i] = (uint8_t)(value >> ((7U - i) * 8U));
  oracle_sha256_update(state, bytes, 8U);
}
