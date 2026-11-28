#ifndef SEGARECOMP_GENESIS_MOD_SET_ARTIFACT_H
#define SEGARECOMP_GENESIS_MOD_SET_ARTIFACT_H

/*
 * SEG-141-T003/T006 (ADR 0327): the constants `segarecomp build --set-package` generates for a program with several mods: one row per mod that runs (data-only members only shape the image), IN SET ORDER.
 * Host-internal, not part of the SDK. A single-mod program keeps its own artifact symbols; mod_capture.cpp turns those into one row of this same type.
 */
#include <segarecomp_mod.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SegarecompModBindingRow {
  const char *id;          /* generated hook site id (shared between members that hook one function) */
  unsigned long hook_id;   /* what this member's `hook` entry receives */
  unsigned long modes;     /* what this member declared at the site (GENESIS_HOOK_MODE_*) */
  unsigned long reads;     /* the register slots this member may read there */
} SegarecompModBindingRow;

typedef struct SegarecompModMemberRow {
  const char *id, *identity, *capabilities;
  unsigned long long required[2], optional[2];
  unsigned long state_schema;
  unsigned long range_count;
  const unsigned long (*ranges)[4]; /* space, offset, length, modes */
  const unsigned char *rom_data;    /* native only: the declared m68k.rom bytes of the composed image */
  const SegaModProvider *(*native_entry)(void **state_out); /* non-null: a native ABI 2 member */
  const unsigned char *luau_artifact; /* non-null: a Luau member */
  unsigned long luau_artifact_size;
  const char *luau_bytecode_digest;
  unsigned long luau_memory_bytes, luau_budget_units;
  unsigned long hook_count, hook_phases;
  const SegarecompModBindingRow *hooks;
} SegarecompModMemberRow;

extern const char segarecomp_mod_set_identity[]; /* the composite identity (64 hex) */
extern const unsigned long segarecomp_mod_set_count;
extern const SegarecompModMemberRow segarecomp_mod_set_members[];

#ifdef __cplusplus
}
#endif
#endif
