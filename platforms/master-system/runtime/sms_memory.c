#include "sms_memory.h"

#include <string.h>

static void sms_fail(SmsMemory *m, SmsError error, uint16_t address, uint8_t value, uint64_t cycles) {
  if (m->error != SMS_OK) return;
  m->error = error;
  m->error_address = address;
  m->error_value = value;
  m->error_cycles = cycles;
}

SmsError sms_memory_init(SmsMemory *m, const uint8_t *rom, uint32_t rom_size, SmsMapperFamily family) {
  memset(m, 0, sizeof *m);
  if (family != SMS_MAPPER_SEGA && family != SMS_MAPPER_ROM_ONLY) return SMS_ERROR_MAPPER_UNDECLARED;
  if (!sms_rom_size_supported(rom_size) || (family == SMS_MAPPER_ROM_ONLY && rom_size != SMS_ROM_ONLY_SIZE))
    return SMS_ERROR_ROM_SIZE_UNSUPPORTED;
  m->rom = rom;
  m->rom_size = rom_size;
  m->family = family;
  sms_memory_reset(m);
  return SMS_OK;
}

void sms_memory_reset(SmsMemory *m) {
  unsigned i;
  memset(m->ram, 0, sizeof m->ram);
  memset(m->cart_ram, 0, sizeof m->cart_ram);
  m->ram[0] = 0xABu; /* the BIOS leaves the last port $3E value at $C000 */
  for (i = 0; i < SMS_MAPPER_REG_COUNT; ++i) m->mapper_regs[i] = sms_sega_reset_regs[i];
  m->memory_control = 0xABu;
  m->error = SMS_OK;
  m->error_address = 0;
  m->error_value = 0;
  m->error_cycles = 0;
}

static uint32_t cart_ram_offset(const SmsMemory *m, uint16_t address) {
  return ((m->mapper_regs[0] & SMS_CTL_CART_RAM_BANK) != 0u ? SMS_BANK_SIZE : 0u) + (uint32_t)(address - 0x8000u);
}

uint8_t sms_memory_read(SmsMemory *m, uint16_t address, uint64_t cycles) {
  if (m->error != SMS_OK) return 0xFFu;
  if (address >= SMS_RAM_BASE) return m->ram[address & (SMS_RAM_SIZE - 1u)]; /* RAM, mirror, mapper-register copies */
  if (m->family == SMS_MAPPER_ROM_ONLY) {
    if (address < SMS_ROM_ONLY_SIZE) return m->rom[address];
    sms_fail(m, SMS_ERROR_UNMAPPED_READ, address, 0xFFu, cycles); /* $8000-$BFFF has no documented content */
    return 0xFFu;
  }
  {
    const uint32_t offset = sms_sega_rom_offset(m->rom_size, m->mapper_regs, address);
    if (offset == UINT32_MAX) return m->cart_ram[cart_ram_offset(m, address)]; /* slot 2 cartridge RAM */
    return m->rom[offset];
  }
}

void sms_memory_write(SmsMemory *m, uint16_t address, uint8_t value, uint64_t cycles) {
  if (m->error != SMS_OK) return;
  if (address >= SMS_RAM_BASE) {
    if (m->family == SMS_MAPPER_SEGA && address >= SMS_MAPPER_REG_BASE) {
      if (address == SMS_MAPPER_REG_BASE &&
          ((value & SMS_CTL_CART_RAM_OVERLAY) != 0u || (value & SMS_CTL_BANK_SHIFT_MASK) != 0u)) {
        sms_fail(m, SMS_ERROR_CONTROL_BIT_UNSUPPORTED, address, value, cycles);
        return;
      }
      m->mapper_regs[address - SMS_MAPPER_REG_BASE] = value; /* write-through: the RAM copy is written below */
    }
    m->ram[address & (SMS_RAM_SIZE - 1u)] = value;
    return;
  }
  if (m->family == SMS_MAPPER_SEGA && address >= 0x8000u && sms_cart_ram_in_slot2(m->mapper_regs))
    m->cart_ram[cart_ram_offset(m, address)] = value;
  /* every other ROM-address write has no effect (mask ROM; control bit 7 is stored and inert) */
}

int sms_memory_code_image(const SmsMemory *m, uint16_t address, Z80CodeImage *image) {
  uint32_t identity = 0;
  uint16_t base = 0;
  if (m->error != SMS_OK) return 0;
  if (!sms_mapper_code_image(m->family, m->rom_size, m->mapper_regs, address, &identity, &base)) return 0;
  image->identity = identity;
  image->window_base = base;
  return 1;
}

void sms_memory_control_write(SmsMemory *m, uint16_t port, uint8_t value, uint64_t cycles) {
  if (m->error != SMS_OK) return;
  if ((value & 0x40u) != 0u || (value & 0x10u) != 0u || (value & 0x08u) == 0u) {
    sms_fail(m, SMS_ERROR_CONTROL_BIT_UNSUPPORTED, port, value, cycles);
    return;
  }
  m->memory_control = value;
}

int sms_memory_io_disabled(const SmsMemory *m) { return (m->memory_control & 0x04u) != 0u; }

uint8_t sms_memory_host_read(void *context, uint16_t address, uint64_t cycles) {
  return sms_memory_read((SmsMemory *)context, address, cycles);
}
void sms_memory_host_write(void *context, uint16_t address, uint8_t value, uint64_t cycles) {
  sms_memory_write((SmsMemory *)context, address, value, cycles);
}
int sms_memory_host_code_image(void *context, uint16_t address, Z80CodeImage *image) {
  return sms_memory_code_image((const SmsMemory *)context, address, image);
}
