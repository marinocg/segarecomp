#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_MAPPER_CONTRACT_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_MAPPER_CONTRACT_H

#include <stdint.h>

/* Master System mapper contract table (SEG-009-T002; machine contract sections 3-4, ADR 0063).
 *
 * ONE data definition, consumed by both
 *   - the generation-time C++ ImageSet declaration (platforms/master-system/machine image_set.cpp), which builds the
 *     admissible windows from `sms_sega_slots` and the image identities from `SMS_IMAGE_*`, and
 *   - the run-time C11 memory map (sms_memory.c), whose read/write/code_image derive every decision from the same
 *     table through the pure functions below.
 * This header is plain C11 (and valid C++): constants, a slot table and pure static-inline functions, no state.
 * Public sources: SMS Power! "Mappers" and "Memory map" (machine contract references SP-MAP, SP-MEM).
 *
 * Address map: $0000-$03FF fixed ROM; $0400-$3FFF slot 0; $4000-$7FFF slot 1; $8000-$BFFF slot 2 (or cartridge RAM);
 * $C000-$DFFF work RAM; $E000-$FFFF mirror (+ mapper registers $FFFC-$FFFF). Registers: index 0 = $FFFC
 * (control), 1 = $FFFD slot 0 bank, 2 = $FFFE slot 1 bank, 3 = $FFFF slot 2 bank. */

/* Mapper families of the baseline profile. The value 0 is deliberately "undeclared": a family is never a default. */
typedef enum SmsMapperFamily {
  SMS_MAPPER_UNDECLARED = 0,
  SMS_MAPPER_SEGA = 1,
  SMS_MAPPER_ROM_ONLY = 2
} SmsMapperFamily;

#define SMS_BANK_SIZE UINT32_C(0x4000)
#define SMS_FIXED_SIZE UINT32_C(0x0400)    /* invariant first 1 KiB */
#define SMS_CODE_ADDRESS_LIMIT UINT32_C(0xC000) /* no code image at $C000-$FFFF */
#define SMS_RAM_BASE UINT32_C(0xC000)
#define SMS_RAM_SIZE UINT32_C(0x2000)
#define SMS_CART_RAM_SIZE UINT32_C(0x8000) /* always the full two 16 KiB banks */
#define SMS_MAPPER_REG_BASE UINT32_C(0xFFFC)
#define SMS_MAPPER_REG_COUNT 4u
#define SMS_ROM_MIN_SIZE UINT32_C(0x8000)
#define SMS_ROM_MAX_SIZE UINT32_C(0x80000)
#define SMS_ROM_ONLY_SIZE UINT32_C(0x8000)

/* Image identities (<= 0xFFFF): 1 = invariant first 1 KiB (sega) or the whole 32 KiB ROM (rom_only); bank n = 2 + n. */
#define SMS_IMAGE_FIXED UINT32_C(1)
#define SMS_IMAGE_BANK_FIRST UINT32_C(2)

/* $FFFC control register bits. */
#define SMS_CTL_ROM_WRITE_ENABLE 0x80u /* stored, no effect (mask ROM) */
#define SMS_CTL_CART_RAM_OVERLAY 0x10u /* cartridge RAM over $C000-$FFFF: unsupported (typed stop) */
#define SMS_CTL_CART_RAM_SLOT2 0x08u
#define SMS_CTL_CART_RAM_BANK 0x04u
#define SMS_CTL_BANK_SHIFT_MASK 0x03u  /* non-zero: unsupported (typed stop) */

/* Power-on values of $FFFC..$FFFF (315-5235). */
static const uint8_t sms_sega_reset_regs[SMS_MAPPER_REG_COUNT] = {0u, 0u, 1u, 2u};

/* A ROM slot: logical window `[window_base, window_base + length)` exposes bank offsets `[first_offset, ...)` of the
 * bank selected by register `bank_register`; image offset o is at logical window_base + o (ADR 0058 CodeWindow). */
typedef struct SmsSlot {
  uint16_t window_base;
  uint32_t first_offset;
  uint32_t length;
  uint8_t bank_register;
} SmsSlot;

#define SMS_SLOT_COUNT 3u
#define SMS_SLOT2 2u
static const SmsSlot sms_sega_slots[SMS_SLOT_COUNT] = {
    {0x0000u, 0x0400u, 0x3C00u, 1u}, /* slot 0: $0400-$3FFF, the first 1 KiB is the fixed image */
    {0x4000u, 0x0000u, 0x4000u, 2u}, /* slot 1 */
    {0x8000u, 0x0000u, 0x4000u, 3u}, /* slot 2 (cartridge RAM instead when control bit 3 is set) */
};

static inline int sms_rom_size_supported(uint32_t size) {
  return size >= SMS_ROM_MIN_SIZE && size <= SMS_ROM_MAX_SIZE && (size & (size - 1u)) == 0u;
}
static inline uint32_t sms_bank_count(uint32_t rom_size) { return rom_size / SMS_BANK_SIZE; }

/* Bank selected by `value` for a power-of-two ROM: value & (bank_count - 1). */
static inline uint32_t sms_bank_select(uint32_t rom_size, uint8_t value) {
  return (uint32_t)value & (sms_bank_count(rom_size) - 1u);
}

/* 1 when control bit 3 maps cartridge RAM into slot 2. */
static inline int sms_cart_ram_in_slot2(const uint8_t regs[SMS_MAPPER_REG_COUNT]) {
  return (regs[0] & SMS_CTL_CART_RAM_SLOT2) != 0u;
}

/* ROM offset served for a data read of logical `address` (< $C000) under the Sega mapper, or UINT32_MAX when the
 * address is not ROM (cartridge RAM in slot 2 or address >= $C000). */
static inline uint32_t sms_sega_rom_offset(uint32_t rom_size, const uint8_t regs[SMS_MAPPER_REG_COUNT],
                                           uint32_t address) {
  uint32_t slot;
  if (address < SMS_FIXED_SIZE) return address;
  if (address >= SMS_CODE_ADDRESS_LIMIT) return UINT32_MAX;
  for (slot = 0u; slot < SMS_SLOT_COUNT; ++slot) {
    const SmsSlot *s = &sms_sega_slots[slot];
    if (address >= s->window_base && address < (uint32_t)s->window_base + s->length + s->first_offset) {
      if (slot == SMS_SLOT2 && sms_cart_ram_in_slot2(regs)) return UINT32_MAX;
      return sms_bank_select(rom_size, regs[s->bank_register]) * SMS_BANK_SIZE + (address - s->window_base);
    }
  }
  return UINT32_MAX;
}

/* Current code image of `address` (Z80 host.code_image contract): returns 1 and fills identity/window base when
 * immutable ROM code is mapped there, 0 for work RAM, its mirror, cartridge RAM and (rom_only) $8000+. */
static inline int sms_mapper_code_image(SmsMapperFamily family, uint32_t rom_size,
                                        const uint8_t regs[SMS_MAPPER_REG_COUNT], uint16_t address,
                                        uint32_t *identity, uint16_t *window_base) {
  uint32_t slot;
  if (family == SMS_MAPPER_ROM_ONLY) {
    if ((uint32_t)address >= SMS_ROM_ONLY_SIZE) return 0;
    *identity = SMS_IMAGE_FIXED;
    *window_base = 0u;
    return 1;
  }
  if (family != SMS_MAPPER_SEGA || (uint32_t)address >= SMS_CODE_ADDRESS_LIMIT) return 0;
  if ((uint32_t)address < SMS_FIXED_SIZE) {
    *identity = SMS_IMAGE_FIXED;
    *window_base = 0u;
    return 1;
  }
  for (slot = 0u; slot < SMS_SLOT_COUNT; ++slot) {
    const SmsSlot *s = &sms_sega_slots[slot];
    if ((uint32_t)address >= s->window_base && (uint32_t)address < (uint32_t)s->window_base + s->first_offset + s->length) {
      if (slot == SMS_SLOT2 && sms_cart_ram_in_slot2(regs)) return 0;
      *identity = SMS_IMAGE_BANK_FIRST + sms_bank_select(rom_size, regs[s->bank_register]);
      *window_base = s->window_base;
      return 1;
    }
  }
  return 0;
}

#endif
