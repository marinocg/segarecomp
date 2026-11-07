/*
 * SEG-131 (ADR 0278, docs/architecture/genesis-save-states-persistence-and-rewind.md): reversible Genesis machine snapshots.
 *
 * A snapshot is a versioned, canonical, little-endian "SGSTATE" container holding every execution-affecting mutable component of the
 * initialized machine: the MC68000 registers, work RAM, the VDP (including a DMA in progress), the Z80 bus and its RAM, the controller
 * latches, interrupt and raster scheduling, cartridge SRAM/EEPROM bytes and EEPROM protocol state, the Z80 CPU and, in a sound build, the
 * PSG, the YM2612 and the deterministic mixer. Host pointers, callbacks, immutable ROM references, SDL objects and diagnostic observers are
 * never serialized: they stay attached to the machine that is being restored.
 *
 * Capture is `const` and may happen only at the documented barrier (between two `genesis_runtime_run` slices). Restore is two-phase: the
 * whole container is parsed, bounded and validated into temporary storage first (nothing of the running machine is touched), and only then
 * committed with plain copies that cannot fail. Any failure leaves the machine byte-identical.
 *
 * Plain C11. A sound-disabled program passes NULL for `z80` and `audio` (and is then bound to sound-less states); define
 * SEGARECOMP_GENESIS_SOUND when compiling the module for a sound-carrying program.
 */
#ifndef SEGARECOMP_GENESIS_SNAPSHOT_H
#define SEGARECOMP_GENESIS_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GenesisZ80Machine;
struct GenesisAudio;

#define GENESIS_SNAPSHOT_MAGIC "SGSTATE"            /* 7 characters + NUL = 8 bytes */
#define GENESIS_SNAPSHOT_CONTAINER_VERSION 1U
#define GENESIS_SNAPSHOT_STATE_SCHEMA 1U
#define GENESIS_SNAPSHOT_MAX_FILE_BYTES (4U * 1024U * 1024U)
#define GENESIS_SNAPSHOT_MAX_THUMB_W 160U
#define GENESIS_SNAPSHOT_MAX_THUMB_H 112U

/* machine_flags */
#define GENESIS_SNAPSHOT_FLAG_SOUND 0x0001U
#define GENESIS_SNAPSHOT_FLAG_SRAM 0x0002U
#define GENESIS_SNAPSHOT_FLAG_EEPROM 0x0004U

typedef enum GenesisSnapshotStatus {
  GENESIS_SNAPSHOT_OK = 0,
  GENESIS_SNAPSHOT_BAD_ARGUMENT,
  GENESIS_SNAPSHOT_NO_MEMORY,
  GENESIS_SNAPSHOT_TOO_LARGE,         /* the file exceeds GENESIS_SNAPSHOT_MAX_FILE_BYTES */
  GENESIS_SNAPSHOT_TRUNCATED,         /* shorter than the structure it declares */
  GENESIS_SNAPSHOT_BAD_MAGIC,
  GENESIS_SNAPSHOT_UNSUPPORTED_VERSION,
  GENESIS_SNAPSHOT_BAD_LAYOUT,        /* section table / lengths / offsets / trailing bytes */
  GENESIS_SNAPSHOT_UNKNOWN_SECTION,   /* an unknown or duplicated section, or a required one missing */
  GENESIS_SNAPSHOT_INTEGRITY,         /* the SHA-256 does not match the content */
  GENESIS_SNAPSHOT_WRONG_CARTRIDGE,   /* source or variant identity differs from the running machine's */
  GENESIS_SNAPSHOT_INCOMPATIBLE,      /* region, machine profile (sound/SRAM/EEPROM) or execution fingerprint differs */
  GENESIS_SNAPSHOT_INVALID_STATE,     /* a value is outside what the device can hold */
  GENESIS_SNAPSHOT_IMAGE_MISSING,     /* a Z80 image the state executes is not in this program's registry */
  GENESIS_SNAPSHOT_NOT_CAPTURABLE     /* the machine is in a latched stop (Z80 view stop, mixer fault, ...): never snapshot it */
} GenesisSnapshotStatus;

/* A short, user-presentable reason for a status. Never NULL. */
const char *genesis_snapshot_status_text(GenesisSnapshotStatus status);

/* The parts of one initialized machine. `z80` and `audio` are both NULL (no sound) or both set. */
typedef struct GenesisSnapshotMachine {
  GenesisRuntime *runtime;
  struct GenesisZ80Machine *z80;
  struct GenesisAudio *audio;
} GenesisSnapshotMachine;

/* Who the state belongs to: the digest of the image the program was built from (the effective, possibly patched, image) and the variant
 * identity (the patch package; all zero = vanilla). Both are supplied by the host from its trusted launch identity; neither is derived from
 * a title. */
typedef struct GenesisSnapshotIdentity {
  uint8_t source_sha256[32];
  uint8_t variant_sha256[32];
} GenesisSnapshotIdentity;

/* A source-pixel thumbnail (nearest-neighbour reduced by the caller), RGB565 little-endian, row-major, at most 160x112. NULL/0 = none. */
typedef struct GenesisSnapshotThumbnail {
  uint16_t width, height;
  const uint8_t *rgb565;
} GenesisSnapshotThumbnail;

/* An owned, exactly-sized container. */
typedef struct GenesisSnapshotBuffer {
  uint8_t *data;
  uint32_t size;
} GenesisSnapshotBuffer;
void genesis_snapshot_buffer_free(GenesisSnapshotBuffer *buffer);

/* The header of a structurally valid, integrity-checked container (pointers refer into the inspected bytes). */
typedef struct GenesisSnapshotInfo {
  uint16_t container_version, state_schema, machine_flags;
  uint32_t region_profile;
  uint8_t source_sha256[32], variant_sha256[32], exec_fingerprint[32];
  uint64_t guest_frame;
  uint64_t guest_master_ticks;
  uint16_t thumb_width, thumb_height;
  const uint8_t *thumb_rgb565;  /* thumb_width * thumb_height * 2 bytes, or NULL */
  uint32_t size;
} GenesisSnapshotInfo;

/* Machine profile flags for a machine (sound / SRAM / EEPROM). */
uint16_t genesis_snapshot_machine_flags(const GenesisSnapshotMachine *machine);

/* Builds a snapshot of the machine at the barrier. Does not modify the machine. */
GenesisSnapshotStatus genesis_snapshot_capture(const GenesisSnapshotMachine *machine, const GenesisSnapshotIdentity *identity,
                                               const GenesisSnapshotThumbnail *thumbnail, GenesisSnapshotBuffer *out);

/* Validates the container structure, bounds and SHA-256 and reports its header. Needs no machine; used to list slots and history. */
GenesisSnapshotStatus genesis_snapshot_inspect(const uint8_t *data, size_t size, GenesisSnapshotInfo *info);

/* Restores a snapshot. On any status other than GENESIS_SNAPSHOT_OK the machine is byte-identical to before the call. */
GenesisSnapshotStatus genesis_snapshot_restore(const GenesisSnapshotMachine *machine, const GenesisSnapshotIdentity *identity,
                                               const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif
#endif
