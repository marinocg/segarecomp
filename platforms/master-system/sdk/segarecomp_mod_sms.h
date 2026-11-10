/* segarecomp_mod_sms.h -- ABI 2 Master System feature table (docs/architecture/native-sdk-abi-2.md section 3). Lifecycle and capability reporting only: phase dispatch and guest access
 * land with SEG-147-T004. The table holds Master System fields only. */
#ifndef SEGARECOMP_MOD_SMS_H
#define SEGARECOMP_MOD_SMS_H

#include <segarecomp_mod.h>

#define SEGA_MOD_PLATFORM SEGA_MOD_PLATFORM_MASTER_SYSTEM
#if defined(SEGA_MOD_TARGET_PLATFORM) && (SEGA_MOD_TARGET_PLATFORM != SEGA_MOD_PLATFORM_ID_MASTER_SYSTEM)
#error "E_ABI_PLATFORM: this provider includes the Master System header but the target platform is different"
#endif

typedef enum SegaSmsSpace {
  SEGA_SMS_SPACE_Z80_WORK_RAM = 1, /* 8 KiB; the $E000 mirror is the same space; the only mod-writable space */
  SEGA_SMS_SPACE_Z80_CART_RAM      /* 32 KiB, read only */
} SegaSmsSpace;

typedef enum SegaSmsMapper { SEGA_SMS_MAPPER_SEGA = 1, SEGA_SMS_MAPPER_ROM_ONLY = 2 } SegaSmsMapper; /* declared in the manifest, never inferred */

typedef struct SegaMasterSystemFeatures {
  uint32_t size; /* the host's sizeof */
  uint32_t work_ram_bytes, cart_ram_bytes, vram_bytes, cram_bytes; /* 8192, 32768, 16384, 32 */
  uint32_t mapper;                                                 /* SegaSmsMapper */
  uint32_t reserved[8];
} SegaMasterSystemFeatures;

#endif /* SEGARECOMP_MOD_SMS_H */
