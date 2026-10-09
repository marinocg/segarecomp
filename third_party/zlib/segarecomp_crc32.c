/* SEG-047 (ADR 0096): the one local zlib addition. The vendored subset (adler32, deflate, trees, zutil) omits crc32.c because the
 * product only ever uses the zlib wrapper (windowBits 15), whose Adler-32 trailer is computed by adler32.c. deflate.c still references
 * crc32() from its gzip-wrapper paths (windowBits > 15 / deflateSetHeader), which the product never selects; this plain bitwise CRC-32
 * (IEEE 802.3, reflected 0xEDB88320) only satisfies the linker and is correct if it were ever reached. */
#include "zlib.h"

uLong ZEXPORT crc32(uLong crc, const Bytef *buf, uInt len) {
  if (buf == Z_NULL) return 0UL;
  uLong c = crc ^ 0xffffffffUL;
  while (len--) {
    c ^= *buf++;
    for (int k = 0; k < 8; ++k) c = (c & 1UL) ? (0xedb88320UL ^ (c >> 1)) : (c >> 1);
  }
  return c ^ 0xffffffffUL;
}
