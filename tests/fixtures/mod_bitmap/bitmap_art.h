/* SEG-152-T005: the FABRICATED art of the bitmap compositor test, shared by the ABI 2 provider and its ABI 1 twin so both submit byte-identical pixels (tests/mod_bitmap_abi2_test.py recomputes it independently).
 * 8 x 8 blocks cycle through: raw RGB under alpha 0 (the compositor must show the picture), opaque colour, and half-transparent colour. */
#ifndef BITMAP_ART_H
#define BITMAP_ART_H
#include <stdint.h>

static void art_fill(uint8_t *rgba, unsigned w, unsigned h, unsigned stride, unsigned k) {
  unsigned x, y;
  for (y = 0U; y < h; ++y)
    for (x = 0U; x < w; ++x) {
      uint8_t *p = rgba + y * stride + x * 4U;
      const unsigned kind = (x / 8U + y / 8U + k) % 3U;
      if (kind == 0U) { p[0] = 200U; p[1] = 100U; p[2] = 50U; p[3] = 0U; }
      else if (kind == 1U) { p[0] = (uint8_t)(x * 6U + k); p[1] = (uint8_t)(y * 14U); p[2] = (uint8_t)(64U + ((x ^ y) & 63U)); p[3] = 255U; }
      else { p[0] = (uint8_t)(x * 5U); p[1] = (uint8_t)(255U - y * 3U); p[2] = 77U; p[3] = 128U; }
    }
}
#endif
