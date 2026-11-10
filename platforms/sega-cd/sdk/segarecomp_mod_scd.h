/* segarecomp_mod_scd.h -- ABI 2 Sega CD feature table (docs/architecture/native-sdk-abi-2.md section 3). Lifecycle and capability reporting only: phase dispatch and guest access land with
 * SEG-148-T005. The main side reuses the Genesis VDP view sizes. */
#ifndef SEGARECOMP_MOD_SCD_H
#define SEGARECOMP_MOD_SCD_H

#include <segarecomp_mod.h>

#define SEGA_MOD_PLATFORM SEGA_MOD_PLATFORM_SEGA_CD
#if defined(SEGA_MOD_TARGET_PLATFORM) && (SEGA_MOD_TARGET_PLATFORM != SEGA_MOD_PLATFORM_ID_SEGA_CD)
#error "E_ABI_PLATFORM: this provider includes the Sega CD header but the target platform is different"
#endif

typedef enum SegaCdSpace {
  SEGA_SCD_SPACE_MAIN_WORK_RAM = 1, /* the only mod-writable space */
  SEGA_SCD_SPACE_MAIN_WORD_RAM      /* read only, and only while the main CPU owns Word RAM */
} SegaCdSpace;

typedef struct SegaCdFeatures {
  uint32_t size; /* the host's sizeof */
  uint32_t work_ram_bytes, word_ram_bytes, vram_bytes, cram_bytes, vsram_bytes; /* 65536, 262144, 65536, 128, 80 */
  uint8_t disc_identity[32];                                                    /* segarecomp.disc.v1 digest, read only */
  uint32_t reserved[8];
} SegaCdFeatures;

#endif /* SEGARECOMP_MOD_SCD_H */
