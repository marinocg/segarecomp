#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_MEMORY_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_MEMORY_H

#include <stdint.h>

#include "segarecomp/codegen/c11/runtime/z80_runtime.h"
#include "sms_error.h"
#include "sms_mapper_contract.h"

/* Master System memory map, mapper and memory control (SEG-009-T002; machine contract sections 3, 4, 5).
 *
 * One object answers data `read`, `write` and `code_image` from the mapper contract table
 * (sms_mapper_contract.h), the same table the generation-time ImageSet is declared from. ROM bytes are supplied by the
 * generated program as an embedded const array (ADR 0064 section 2); this module never opens a file.
 *
 * Errors are typed and sticky: the first SmsError is latched in `error` with the offending address/value/T-state and
 * the machine must stop (the Z80 ABI offers no way to abort a run from inside a callback, so the platform checks
 * `error` after every `z80_run` and never resumes a machine with `error != SMS_OK`). After an error, reads return
 * $FF, writes are ignored and code_image answers 0, so the state observed at the stop is not advanced further.
 * The host callback adapters below plug the object into `Z80Host` (context = the SmsMemory). */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SmsMemory {
  const uint8_t *rom;
  uint32_t rom_size;
  SmsMapperFamily family;
  uint8_t ram[SMS_RAM_SIZE];
  uint8_t cart_ram[SMS_CART_RAM_SIZE]; /* always 32 KiB, zero at power-on, deterministic, never persisted */
  uint8_t mapper_regs[SMS_MAPPER_REG_COUNT]; /* $FFFC..$FFFF; write-only registers (reads return the RAM copy) */
  uint8_t memory_control;                    /* port $3E */
  SmsError error;
  uint16_t error_address; /* address (or port for memory control) of the offending access */
  uint8_t error_value;    /* value written, or 0xFF for a read */
  uint64_t error_cycles;  /* instruction-start T-state passed by the host */
} SmsMemory;

/* Binds the cartridge. Returns SMS_OK, SMS_ERROR_ROM_SIZE_UNSUPPORTED (size not 32..512 KiB power of two, or not
 * 32 KiB for rom_only) or SMS_ERROR_MAPPER_UNDECLARED (family is not a baseline family). Does not reset. */
SmsError sms_memory_init(SmsMemory *m, const uint8_t *rom, uint32_t rom_size, SmsMapperFamily family);

/* Power-on / post-BIOS state: work RAM zero except $C000 = $AB, mapper registers 0,0,1,2, cartridge RAM zero, memory
 * control $AB, no error. */
void sms_memory_reset(SmsMemory *m);

uint8_t sms_memory_read(SmsMemory *m, uint16_t address, uint64_t cycles);
void sms_memory_write(SmsMemory *m, uint16_t address, uint8_t value, uint64_t cycles);

/* 1 and identity/window base when immutable ROM code is mapped at `address` under the current mapping, else 0. */
int sms_memory_code_image(const SmsMemory *m, uint16_t address, Z80CodeImage *image);

/* Latches `error` (first error wins, sticky) with the offending address/port, value and instruction-start T-state. Used by
 * the machine for platform errors that are not memory errors (an unimplemented port class); same latch, same rules. */
void sms_memory_latch_error(SmsMemory *m, SmsError error, uint16_t address, uint8_t value, uint64_t cycles);

/* Port $3E write. A write with the cartridge (bit 6) and work RAM (bit 4) enabled and BIOS (bit 3) disabled is accepted
 * (bit 2 is the I/O chip disable and is modelled; bits 7, 5, 1-0 have no effect); any other write latches
 * SMS_ERROR_CONTROL_BIT_UNSUPPORTED. `port` is recorded in the error for diagnostics. */
void sms_memory_control_write(SmsMemory *m, uint16_t port, uint8_t value, uint64_t cycles);
/* 1 while the I/O chip is disabled by port $3E bit 2 (reads of $C0-$FF return $FF). */
int sms_memory_io_disabled(const SmsMemory *m);

/* Z80Host adapters; `context` must be the SmsMemory. */
uint8_t sms_memory_host_read(void *context, uint16_t address, uint64_t cycles);
void sms_memory_host_write(void *context, uint16_t address, uint8_t value, uint64_t cycles);
int sms_memory_host_code_image(void *context, uint16_t address, Z80CodeImage *image);

#ifdef __cplusplus
}
#endif

#endif
