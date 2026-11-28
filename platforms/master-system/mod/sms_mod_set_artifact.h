#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_MOD_SET_ARTIFACT_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_MOD_SET_ARTIFACT_H

/* SEG-141-T007 (ADR 0327): the constants `segarecomp build --set-package` generates for a Master System program with several mods: one row per mod that runs, IN SET ORDER. Host-internal, not part of
 * the SDK. A single-mod program keeps its own artifact symbols; sms_mod_host.c turns those into one row of this same type. */
#include <segarecomp_mod.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SegarecompSmsModMemberRow {
  const char *id, *identity, *capabilities;
  unsigned long long required[2], optional[2];
  unsigned long state_schema;
  unsigned long range_count;
  const unsigned long (*ranges)[4]; /* space, offset, length, modes */
  const SegaModProvider *(*native_entry)(void **state_out); /* non-null: a native ABI 2 member */
  const unsigned char *luau_artifact;                       /* non-null: a Luau member */
  unsigned long luau_artifact_size;
  const char *luau_bytecode_digest;
  unsigned long luau_memory_bytes, luau_budget_units;
} SegarecompSmsModMemberRow;

extern const char segarecomp_sms_mod_set_identity[]; /* the composite identity (64 hex) */
extern const unsigned long segarecomp_sms_mod_set_count;
extern const SegarecompSmsModMemberRow segarecomp_sms_mod_set_members[];

/* The Luau adapter of one script member (sms_mod_luau.cpp): the provider table, its state and the phases the script defines. NULL = the stored script could not be loaded (MOD_REFUSED was printed). */
const SegaModProvider *segarecomp_sms_mod_luau_open(const SegarecompSmsModMemberRow *row, void **state_out, uint32_t *phase_mask);
int segarecomp_sms_mod_luau_nondeterministic(const void *state);

#ifdef __cplusplus
}
#endif
#endif
