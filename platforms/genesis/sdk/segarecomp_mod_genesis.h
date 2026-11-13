/* segarecomp_mod_genesis.h -- ABI 2 Genesis feature table (docs/architecture/native-sdk-abi-2.md section 3). ABI 1's segarecomp_genesis_patch.h is separate and unchanged. */
#ifndef SEGARECOMP_MOD_GENESIS_H
#define SEGARECOMP_MOD_GENESIS_H

#include <segarecomp_mod.h>

#define SEGA_MOD_PLATFORM SEGA_MOD_PLATFORM_GENESIS
#if defined(SEGA_MOD_TARGET_PLATFORM) && (SEGA_MOD_TARGET_PLATFORM != SEGA_MOD_PLATFORM_ID_GENESIS)
#error "E_ABI_PLATFORM: this provider includes the Genesis header but the target platform is different"
#endif

/* Space identifiers are named constants of this table (never a numeric literal shared across platforms); append-only. */
typedef enum SegaGenesisSpace {
  SEGA_GEN_SPACE_M68K_WORK_RAM = 1, /* the only mod-writable Genesis space */
  SEGA_GEN_SPACE_M68K_ROM,          /* read at build time only */
  SEGA_GEN_SPACE_Z80_RAM,           /* limited read */
  SEGA_GEN_SPACE_VDP_VRAM,
  SEGA_GEN_SPACE_VDP_CRAM,
  SEGA_GEN_SPACE_VDP_VSRAM,
  SEGA_GEN_SPACE_M68K_HOOK_REGS /* the register view of the function hook in flight: offset = slot (0..7 D0-D7, 8..15 A0-A7, 16 CCR, 17 cycle counter, 24..31 stack slot 0..7), width 4; mod-execution-phases.md 5.3 */
} SegaGenesisSpace;

typedef struct SegaGenesisFeatures {
  uint32_t size; /* the host's sizeof */
  uint32_t work_ram_bytes, vram_bytes, vdp_registers, cram_bytes, vsram_bytes, visible_lines; /* 65536, 65536, 24, 128, 80, 224 */
  uint32_t reserved[8];
} SegaGenesisFeatures;

#endif /* SEGARECOMP_MOD_GENESIS_H */
