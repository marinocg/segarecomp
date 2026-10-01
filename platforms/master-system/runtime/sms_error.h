#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_ERROR_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_ERROR_H

/* Typed Master System platform error surface (machine contract section 13, SEG-009-T001/T002).
 *
 * Distinct from Z80Outcome. Generation-time classes (profile, mapper identity, ROM size) are raised by the C++ machine
 * library before any C is written; run-time classes are latched by the C11 runtime (first error wins, sticky) and stop
 * the machine permanently. Numeric values are stable: they are part of the status/diagnostic surface of later tasks.
 * Classes whose owner is a later task are defined here so the surface is one enum. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum SmsError {
  SMS_OK = 0,
  SMS_ERROR_PROFILE_UNSUPPORTED = 1,      /* T002: non-baseline region/TV standard/console at ingestion */
  SMS_ERROR_MAPPER_UNDECLARED = 2,        /* T002: missing, unknown or conflicting mapper identity declaration */
  SMS_ERROR_MAPPER_UNSUPPORTED = 3,       /* T002: declared non-baseline mapper family */
  SMS_ERROR_ROM_SIZE_UNSUPPORTED = 4,     /* T002: size outside 32-512 KiB power of two (or not 32 KiB for rom_only) */
  SMS_ERROR_CONTROL_BIT_UNSUPPORTED = 5,  /* T002: $FFFC bit 4 / bank shift; incompatible port $3E write */
  SMS_ERROR_UNMAPPED_READ = 6,            /* T002: rom_only data read of $8000-$BFFF */
  SMS_ERROR_BIOS_UNSUPPORTED = 7,         /* T003 */
  SMS_ERROR_PORT_UNIMPLEMENTED = 8,       /* T003 */
  SMS_ERROR_VDP_MODE_UNSUPPORTED = 9,     /* T004 */
  SMS_ERROR_HCOUNTER_UNRESOLVED = 10,     /* T004 */
  SMS_ERROR_PSG_DATA_BEFORE_LATCH = 11    /* T007 */
} SmsError;

static inline const char *sms_error_name(SmsError error) {
  switch (error) {
    case SMS_OK: return "SMS_OK";
    case SMS_ERROR_PROFILE_UNSUPPORTED: return "SMS_ERROR_PROFILE_UNSUPPORTED";
    case SMS_ERROR_MAPPER_UNDECLARED: return "SMS_ERROR_MAPPER_UNDECLARED";
    case SMS_ERROR_MAPPER_UNSUPPORTED: return "SMS_ERROR_MAPPER_UNSUPPORTED";
    case SMS_ERROR_ROM_SIZE_UNSUPPORTED: return "SMS_ERROR_ROM_SIZE_UNSUPPORTED";
    case SMS_ERROR_CONTROL_BIT_UNSUPPORTED: return "SMS_ERROR_CONTROL_BIT_UNSUPPORTED";
    case SMS_ERROR_UNMAPPED_READ: return "SMS_ERROR_UNMAPPED_READ";
    case SMS_ERROR_BIOS_UNSUPPORTED: return "SMS_ERROR_BIOS_UNSUPPORTED";
    case SMS_ERROR_PORT_UNIMPLEMENTED: return "SMS_ERROR_PORT_UNIMPLEMENTED";
    case SMS_ERROR_VDP_MODE_UNSUPPORTED: return "SMS_ERROR_VDP_MODE_UNSUPPORTED";
    case SMS_ERROR_HCOUNTER_UNRESOLVED: return "SMS_ERROR_HCOUNTER_UNRESOLVED";
    case SMS_ERROR_PSG_DATA_BEFORE_LATCH: return "SMS_ERROR_PSG_DATA_BEFORE_LATCH";
  }
  return "SMS_ERROR_INVALID";
}

#ifdef __cplusplus
}
#endif
#endif
