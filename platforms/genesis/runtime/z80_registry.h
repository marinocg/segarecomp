/*
 * SEG-032-T003 (ADR 0073): the compiled Z80 image registry the build generates (platforms/genesis/machine z80_images). Plain C11.
 * Ordinal k (1-based) is the code-image identity of image k in the generated z80_run; 0 means "unknown". The signature is the
 * SHA-256 activation signature (contract section 7) computed at the runnable transition.
 */
#ifndef SEGARECOMP_GENESIS_Z80_REGISTRY_H
#define SEGARECOMP_GENESIS_Z80_REGISTRY_H

#include <stdint.h>

extern const uint32_t genesis_z80_image_count;
extern const uint8_t genesis_z80_image_content_hashes[][32]; /* absent when genesis_z80_image_count == 0 */
uint32_t genesis_z80_image_for_signature(const uint8_t signature[32]);

#endif
