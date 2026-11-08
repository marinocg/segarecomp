#ifndef SEGARECOMP_VIEWER_SEGA_CD_SCD_SNAPSHOT_H
#define SEGARECOMP_VIEWER_SEGA_CD_SCD_SNAPSHOT_H

/*
 * SEG-132 (ADR 0279, docs/architecture/sms-sega-cd-save-states-inventory.md section 3): reversible Sega CD machine snapshots.
 *
 * A snapshot is a versioned, canonical, little-endian "SCDSTAT" container owned by the Sega CD runtime -- not a larger Genesis struct dump. It holds every execution-affecting
 * mutable component of the BIOS-image-profile machine at the live driver's frame barrier: both MC68000 processors with their scheduling, interrupt and timing state, main work RAM,
 * sub PRG RAM and Word RAM, the Gate Array (ownership, banks, communication registers, timer, stopwatch), the CDC (buffer, decoder, a DMA in flight), the CDD (command/status, position,
 * latency, the 75 Hz grid), the delivery ledger, the RF5C164 PCM chip and its sample clock, CD-DA continuity and fader, the mixed-audio timeline (queue, clocks, hold value and the
 * not-yet-delivered output), the graphics ASIC operation in progress, the in-memory backup RAM, the live pad, overlay selection, dynamic-image bindings, the main-side Genesis VDP /
 * I-O / Z80 bus and, when attached, the Z80 machine, YM2612, PSG and mixer (through the Genesis section codecs, reused unchanged).
 *
 * What is NOT serialized: host pointers and callbacks, the immutable BIOS and disc (the verified objects stay attached and their IDENTITY is bound into the file), the program
 * tables, file handles, diagnostic observers and counters, the host-only instruction allowance (a restore never extends a bounded run), and presentation-only state.
 *
 * A snapshot is tied to the disc identity, the BIOS image and the generated executable-image contract (materialized sub images, overlay manifest, dynamic images, processor program
 * facts, sound configuration). Rebuilding compatible code does not invalidate a state; a different image set is refused.
 *
 * Capture is `const` and only at the live barrier. Restore is two-phase: the container is parsed, bounded and validated into STAGED copies (nothing of the running machine is touched),
 * then committed with plain copies that cannot fail. Configurations whose future cannot be reproduced exactly fail closed with a reason (see scd_snapshot_refusal).
 */
#include <stddef.h>
#include <stdint.h>

#include "scd_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCD_SNAPSHOT_MAGIC "SCDSTAT"   /* 7 characters + NUL = 8 bytes */
#define SCD_SNAPSHOT_CONTAINER_VERSION 1U
#define SCD_SNAPSHOT_STATE_SCHEMA 1U
#define SCD_SNAPSHOT_MAX_FILE_BYTES (4U * 1024U * 1024U)
#define SCD_SNAPSHOT_MAX_THUMB_W 160U
#define SCD_SNAPSHOT_MAX_THUMB_H 112U

typedef enum ScdSnapshotStatus {
  SCD_SNAPSHOT_OK = 0,
  SCD_SNAPSHOT_BAD_ARGUMENT,
  SCD_SNAPSHOT_NO_MEMORY,
  SCD_SNAPSHOT_TOO_LARGE,
  SCD_SNAPSHOT_TRUNCATED,
  SCD_SNAPSHOT_BAD_MAGIC,
  SCD_SNAPSHOT_UNSUPPORTED_VERSION,
  SCD_SNAPSHOT_BAD_LAYOUT,
  SCD_SNAPSHOT_UNKNOWN_SECTION,
  SCD_SNAPSHOT_INTEGRITY,
  SCD_SNAPSHOT_WRONG_DISC,        /* the state was taken on another disc or BIOS */
  SCD_SNAPSHOT_INCOMPATIBLE,      /* another executable-image set, sound configuration or backup-RAM presence */
  SCD_SNAPSHOT_INVALID_STATE,     /* a value is outside what the device can hold */
  SCD_SNAPSHOT_IMAGE_MISSING,     /* a Z80 image the state executes is not in this program's registry */
  SCD_SNAPSHOT_NOT_CAPTURABLE     /* a configuration or machine state whose future cannot be reproduced exactly (see scd_snapshot_refusal) */
} ScdSnapshotStatus;

const char *scd_snapshot_status_text(ScdSnapshotStatus status);

/* NULL when the machine can be snapshotted; otherwise the reason (a build pass, a diagnostic observer, a scripted or recorded input, a latched stop, the Z80 without its sound
 * devices, ...). Cheap; used for the provider's availability text and by capture itself. */
const char *scd_snapshot_refusal(const ScdMachine *machine);

typedef struct ScdSnapshotThumbnail {
  uint16_t width, height;
  const uint8_t *rgb565;
} ScdSnapshotThumbnail;

typedef struct ScdSnapshotBuffer {
  uint8_t *data;
  uint32_t size;
} ScdSnapshotBuffer;
void scd_snapshot_buffer_free(ScdSnapshotBuffer *buffer);

typedef struct ScdSnapshotInfo {
  uint16_t container_version, state_schema, machine_flags;
  uint8_t disc_sha256[32], bios_sha256[32], exec_fingerprint[32];
  uint64_t guest_onsets;           /* vertical-blank onsets processed (the video frame count) */
  uint64_t guest_ticks;            /* machine ticks (SCD_TICKS_PER_SECOND) */
  uint16_t thumb_width, thumb_height;
  const uint8_t *thumb_rgb565;
  uint32_t size;
} ScdSnapshotInfo;

/* Builds a snapshot at the barrier. Does not modify the machine. */
ScdSnapshotStatus scd_snapshot_capture(const ScdMachine *machine, const ScdSnapshotThumbnail *thumbnail, ScdSnapshotBuffer *out);
/* Structure + bounds + SHA-256 and the header. Needs no machine. */
ScdSnapshotStatus scd_snapshot_inspect(const uint8_t *data, size_t size, ScdSnapshotInfo *info);
/* Everything restore decides before it commits, including the full staged device validation. OK means "restore would succeed". `info` may be NULL. Never mutates the machine. */
ScdSnapshotStatus scd_snapshot_validate(ScdMachine *machine, const uint8_t *data, size_t size, ScdSnapshotInfo *info);
/* Restores a snapshot. On any status other than OK the machine is byte-identical to before the call. The instruction allowance, diagnostics and every host binding are untouched. */
ScdSnapshotStatus scd_snapshot_restore(ScdMachine *machine, const uint8_t *data, size_t size);

/* Guest milliseconds of a machine tick count. */
uint64_t scd_snapshot_ticks_to_ms(uint64_t ticks);

#ifdef __cplusplus
}
#endif
#endif
