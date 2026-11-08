#ifndef SEGARECOMP_VIEWER_SV_STATE_CODEC_H
#define SEGARECOMP_VIEWER_SV_STATE_CODEC_H

/*
 * SEG-132 (ADR 0279): the small, platform-neutral byte toolkit the console state codecs share -- a growable, bounded little-endian writer and a bounded reader with
 * a sticky error. It knows nothing about any machine: no sections, no devices, no reflection. Each console owns its container layout and its field-by-field codec
 * (the Genesis codec keeps its own identical, already-accepted copy).
 *
 * Header-only, plain C11. Every function is `static inline` so a translation unit pays only for what it uses.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SvcWriter {
  uint8_t *data;
  uint32_t size, capacity, limit;   /* `limit`: the largest size the writer will ever grow to (a fixed maximum, never a file length) */
  int error;                        /* sticky: allocation failure or limit exceeded */
} SvcWriter;

static inline void svc_w_init(SvcWriter *w, uint32_t limit) {
  memset(w, 0, sizeof *w);
  w->limit = limit;
}

static inline void svc_w_reserve(SvcWriter *w, uint32_t extra) {
  uint32_t wanted;
  if (w->error) return;
  if (extra > w->limit || w->size > w->limit - extra) { w->error = 1; return; }
  wanted = w->size + extra;
  if (wanted > w->capacity) {
    uint32_t capacity = w->capacity == 0U ? 4096U : w->capacity;
    uint8_t *grown;
    while (capacity < wanted) capacity *= 2U;
    if (capacity > w->limit) capacity = w->limit;
    grown = (uint8_t *)realloc(w->data, capacity);
    if (grown == NULL) { w->error = 1; return; }
    w->data = grown;
    w->capacity = capacity;
  }
}

static inline void svc_w_bytes(SvcWriter *w, const void *src, uint32_t count) {
  svc_w_reserve(w, count);
  if (w->error) return;
  if (count != 0U) memcpy(w->data + w->size, src, count);
  w->size += count;
}
static inline void svc_w_u8(SvcWriter *w, uint8_t v) { svc_w_bytes(w, &v, 1U); }
static inline void svc_w_u16(SvcWriter *w, uint16_t v) { uint8_t b[2]; b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); svc_w_bytes(w, b, 2U); }
static inline void svc_w_u32(SvcWriter *w, uint32_t v) { uint8_t b[4]; unsigned i; for (i = 0; i < 4U; ++i) b[i] = (uint8_t)(v >> (8U * i)); svc_w_bytes(w, b, 4U); }
static inline void svc_w_u64(SvcWriter *w, uint64_t v) { uint8_t b[8]; unsigned i; for (i = 0; i < 8U; ++i) b[i] = (uint8_t)(v >> (8U * i)); svc_w_bytes(w, b, 8U); }

typedef struct SvcReader {
  const uint8_t *p;
  uint32_t size, off;
  int error;                        /* sticky: a read past the end */
} SvcReader;

static inline void svc_r_init(SvcReader *r, const uint8_t *data, uint32_t size) { r->p = data; r->size = size; r->off = 0U; r->error = 0; }

static inline const uint8_t *svc_r_take(SvcReader *r, uint32_t count) {
  const uint8_t *at;
  if (r->error || count > r->size - r->off) { r->error = 1; return NULL; }
  at = r->p + r->off;
  r->off += count;
  return at;
}
static inline uint8_t svc_r_u8(SvcReader *r) { const uint8_t *b = svc_r_take(r, 1U); return b == NULL ? 0U : b[0]; }
static inline uint16_t svc_r_u16(SvcReader *r) { const uint8_t *b = svc_r_take(r, 2U); return b == NULL ? 0U : (uint16_t)(b[0] | ((unsigned)b[1] << 8)); }
static inline uint32_t svc_r_u32(SvcReader *r) {
  const uint8_t *b = svc_r_take(r, 4U);
  uint32_t v = 0U;
  unsigned i;
  if (b == NULL) return 0U;
  for (i = 0; i < 4U; ++i) v |= (uint32_t)b[i] << (8U * i);
  return v;
}
static inline uint64_t svc_r_u64(SvcReader *r) {
  const uint8_t *b = svc_r_take(r, 8U);
  uint64_t v = 0U;
  unsigned i;
  if (b == NULL) return 0U;
  for (i = 0; i < 8U; ++i) v |= (uint64_t)b[i] << (8U * i);
  return v;
}
static inline void svc_r_bytes(SvcReader *r, void *dst, uint32_t count) { const uint8_t *b = svc_r_take(r, count); if (b != NULL && count != 0U) memcpy(dst, b, count); }
/* The whole section was consumed without error. */
static inline int svc_r_done(const SvcReader *r) { return !r->error && r->off == r->size; }

static inline uint16_t svc_le16(const uint8_t *b) { return (uint16_t)(b[0] | ((unsigned)b[1] << 8)); }
static inline uint32_t svc_le32(const uint8_t *b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static inline uint64_t svc_le64(const uint8_t *b) { return (uint64_t)svc_le32(b) | ((uint64_t)svc_le32(b + 4) << 32); }
static inline void svc_put_le16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static inline void svc_put_le32(uint8_t *b, uint32_t v) { unsigned i; for (i = 0; i < 4U; ++i) b[i] = (uint8_t)(v >> (8U * i)); }
static inline void svc_put_le64(uint8_t *b, uint64_t v) { unsigned i; for (i = 0; i < 8U; ++i) b[i] = (uint8_t)(v >> (8U * i)); }

#ifdef __cplusplus
}
#endif
#endif
