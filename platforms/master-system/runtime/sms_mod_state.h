#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_MOD_STATE_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_MOD_STATE_H

/* SEG-147-T005 (ADR 0325): the seam through which a mod's own state joins the Master System snapshot container (`MODSTATE` section). The snapshot module never links a scheduler: the mod scheduler
 * (sms_mod_scheduler.h, sms_mod_scheduler_state) fills this table. The section holds { identity, state schema, compatibility digest, body }; the body is produced and understood only by the codec. */
#include <stdint.h>

#include "sms_machine.h"
#include "sms_pad.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SMS_MOD_BLOB_MAX 65536U                         /* the mod's own state (phases section 6.2) */
#define SMS_MOD_BODY_MAX (SMS_MOD_BLOB_MAX + 1024U)     /* the codec's whole body */

typedef enum SmsModStateStatus {
  SMS_MOD_STATE_OK = 0,
  SMS_MOD_STATE_UNAVAILABLE,   /* E_STATE_UNAVAILABLE: a guest-affecting mod without `state.mod` (or whose state cannot be saved) */
  SMS_MOD_STATE_NOT_CAPTURABLE /* not at the barrier */
} SmsModStateStatus;

typedef struct SmsModState {
  void *context;
  uint8_t identity[32];   /* the mod's package identity: an exact match is required to restore */
  uint32_t state_schema;  /* exact match required */
  uint8_t compat[32];     /* ABI major, granted capabilities, phases, ranges, provider version: exact match required */
  /* Writes the body into out[0..cap); returns its length, or a value greater than `cap` on failure (`*status` says why). Does not modify the mod. */
  uint32_t (*put)(void *context, uint8_t *out, uint32_t cap, SmsModStateStatus *status);
  /* Validates a body into private staged storage; nothing the mod or the machine owns is touched. NULL = invalid. */
  void *(*stage)(void *context, const uint8_t *body, uint32_t length);
  /* The only step that can still fail: hands the mod its blob, BEFORE any guest state is replaced. 0 on success. */
  int (*apply)(void *context, void *staged);
  /* Cannot fail: installs the scheduler state and the pad interception over the (already restored) machine and pad. Consumes `staged`. */
  void (*commit)(void *context, void *staged, SmsMachine *machine, SmsPad *pad);
  void (*discard)(void *context, void *staged);
} SmsModState;

#ifdef __cplusplus
}
#endif
#endif
