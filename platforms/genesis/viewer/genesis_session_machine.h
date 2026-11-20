#ifndef SEGARECOMP_VIEWER_GENESIS_SESSION_MACHINE_H
#define SEGARECOMP_VIEWER_GENESIS_SESSION_MACHINE_H

/*
 * SEG-146-T006: the whole-machine checkpoint digest of a session (genesis_session.h). It is the SHA-256 of the machine's snapshot container (genesis_snapshot.h) captured with a zero
 * identity and no thumbnail: CPU, work RAM, VDP, I/O latches, cartridge SRAM/EEPROM state, Z80, PSG, YM2612, mixer and mod state - exactly what a save state restores, in the same
 * build-independent encoding. The profile names the machine shape (`machine-<flags in hex>`: sound / mod / SRAM / EEPROM), so a recording is only enforced against a machine of the
 * same shape. Linked only where genesis_snapshot.c is (the viewer, and the headless programs that carry sound or a mod).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_session.h"
#include "genesis_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GenesisSessionMachineDigest {
  GenesisSnapshotMachine machine;
  const GenesisSnapshotModState *(*mod_state)(void);   /* optional: the attached mod scheduler's state, queried when the session is created and at every digest */
  char profile[40];
} GenesisSessionMachineDigest;

static inline int genesis_session_machine_digest_fn(void *ctx, const GenesisRuntime *runtime, uint8_t out[32]) {
  const GenesisSessionMachineDigest *m = (const GenesisSessionMachineDigest *)ctx;
  GenesisSnapshotIdentity identity;
  GenesisSnapshotBuffer buffer;
  GenesisSha256 sha;
  GenesisSnapshotMachine machine = m->machine;
  if (m->mod_state != NULL) machine.mod = m->mod_state();
  if (runtime != machine.runtime) return -1;
  memset(&identity, 0, sizeof identity);
  if (genesis_snapshot_capture(&machine, &identity, NULL, &buffer) != GENESIS_SNAPSHOT_OK) return -1;
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, buffer.data, buffer.size);
  genesis_sha256_final(&sha, out);
  genesis_snapshot_buffer_free(&buffer);
  return 0;
}

static inline void genesis_session_machine_digest_prepare(void *ctx, GenesisSessionDigest *self) {
  GenesisSessionMachineDigest *m = (GenesisSessionMachineDigest *)ctx;
  GenesisSnapshotMachine machine = m->machine;
  if (m->mod_state != NULL) machine.mod = m->mod_state();
  snprintf(m->profile, sizeof m->profile, "machine-%04x", (unsigned)genesis_snapshot_machine_flags(&machine));
  self->profile = m->profile;
}

/* `storage` must outlive the session. `mod_state` may be NULL. */
static inline void genesis_session_machine_digest_init(GenesisSessionDigest *digest, GenesisSessionMachineDigest *storage, const GenesisSnapshotMachine *machine,
                                                       const GenesisSnapshotModState *(*mod_state)(void)) {
  storage->machine = *machine;
  storage->mod_state = mod_state;
  digest->profile = storage->profile;
  digest->fn = genesis_session_machine_digest_fn;
  digest->ctx = storage;
  digest->prepare = genesis_session_machine_digest_prepare;
  genesis_session_machine_digest_prepare(storage, digest);
}

#ifdef __cplusplus
}
#endif
#endif
