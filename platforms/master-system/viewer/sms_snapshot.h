#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_SNAPSHOT_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_SNAPSHOT_H

/*
 * SEG-132 (ADR 0279, docs/architecture/sms-sega-cd-save-states-inventory.md): reversible Master System machine snapshots.
 *
 * A snapshot is a versioned, canonical, little-endian "SMSSTAT" container holding every execution-affecting mutable component of the composed machine: the Z80
 * registers, interrupt state and T-state counter, work RAM, cartridge RAM and mapper/memory-control registers, the I/O control latch, the scanline scheduler
 * position, the scripted-input state, the VDP (VRAM, CRAM, registers, control-port latch, flags, counters), the renderer (frame in progress, published frame
 * and presentation identity), the SN76489 PSG (through its paired save/load API) with its PCM sampling counters, and the controller device. Host pointers,
 * callbacks, the immutable ROM, trace buffers, the host audio ring and every SDL object are never serialized: they stay attached to the machine that is being
 * restored.
 *
 * Capture is `const` and may happen only at the viewer's frame barrier (between two `sms_run_until_frame` calls). Restore is two-phase: the container is
 * parsed, bounded and validated into temporary copies first (nothing of the running machine is touched) and then committed with plain copies that cannot
 * fail. Any failure leaves the machine byte-identical.
 *
 * Plain C11. The Master System runtime does not depend on this module; it lives with the viewer.
 */
#include <stddef.h>
#include <stdint.h>

#include "sms_machine.h"
#include "sms_pad.h"
#include "sms_psg.h"
#include "sms_render.h"
#include "sms_vdp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SMS_SNAPSHOT_MAGIC "SMSSTAT"   /* 7 characters + NUL = 8 bytes */
#define SMS_SNAPSHOT_CONTAINER_VERSION 1U
#define SMS_SNAPSHOT_STATE_SCHEMA 1U
#define SMS_SNAPSHOT_MAX_FILE_BYTES (1024U * 1024U)
#define SMS_SNAPSHOT_MAX_THUMB_W 160U
#define SMS_SNAPSHOT_MAX_THUMB_H 112U

/* machine_flags */
#define SMS_SNAPSHOT_FLAG_SCRIPT_INPUT 0x0001U   /* the machine is driven by a scripted-input schedule (cursor + prefix digest are part of the state) */

typedef enum SmsSnapshotStatus {
  SMS_SNAPSHOT_OK = 0,
  SMS_SNAPSHOT_BAD_ARGUMENT,
  SMS_SNAPSHOT_NO_MEMORY,
  SMS_SNAPSHOT_TOO_LARGE,
  SMS_SNAPSHOT_TRUNCATED,
  SMS_SNAPSHOT_BAD_MAGIC,
  SMS_SNAPSHOT_UNSUPPORTED_VERSION,
  SMS_SNAPSHOT_BAD_LAYOUT,
  SMS_SNAPSHOT_UNKNOWN_SECTION,
  SMS_SNAPSHOT_INTEGRITY,
  SMS_SNAPSHOT_WRONG_CARTRIDGE,   /* source or variant identity differs from the running machine's */
  SMS_SNAPSHOT_INCOMPATIBLE,      /* mapper declaration, ROM size, PSG configuration, input mode or execution fingerprint differs */
  SMS_SNAPSHOT_INVALID_STATE,     /* a value is outside what the device can hold */
  SMS_SNAPSHOT_NOT_CAPTURABLE     /* the machine is in a latched stop (typed error, dead machine): never snapshot it */
} SmsSnapshotStatus;

/* A short, user-presentable reason. Never NULL. */
const char *sms_snapshot_status_text(SmsSnapshotStatus status);

/* The parts of one composed machine. `machine->pad.context`, `machine->vdp.port.context` and `machine->psg.context` must be `pad`, `vdp` and `psg`. */
typedef struct SmsSnapshotMachine {
  SmsMachine *machine;
  SmsVdp *vdp;
  SmsRenderer *renderer;
  SmsPsg *psg;
  SmsPad *pad;
  int script_input;   /* 1: a scripted-input schedule drives the machine (its cursor and consumed-prefix digest are state); 0: live input (the viewer) */
} SmsSnapshotMachine;

/* Who the state belongs to: the digest of the cartridge image the program was built from (the effective, possibly patched, image) and the variant identity
 * (all zero = vanilla). Supplied by the host from its trusted launch identity. */
typedef struct SmsSnapshotIdentity {
  uint8_t source_sha256[32];
  uint8_t variant_sha256[32];
} SmsSnapshotIdentity;

/* A source-pixel thumbnail (reduced by the caller), RGB565 little-endian, row-major, at most 160x112. NULL/0 = none. */
typedef struct SmsSnapshotThumbnail {
  uint16_t width, height;
  const uint8_t *rgb565;
} SmsSnapshotThumbnail;

typedef struct SmsSnapshotBuffer {
  uint8_t *data;
  uint32_t size;
} SmsSnapshotBuffer;
void sms_snapshot_buffer_free(SmsSnapshotBuffer *buffer);

typedef struct SmsSnapshotInfo {
  uint16_t container_version, state_schema, machine_flags;
  uint32_t mapper_family;
  uint8_t source_sha256[32], variant_sha256[32], exec_fingerprint[32];
  uint64_t guest_frame;
  uint64_t guest_cycles;          /* Z80 T-states */
  uint16_t thumb_width, thumb_height;
  const uint8_t *thumb_rgb565;    /* refers into the inspected bytes, or NULL */
  uint32_t size;
} SmsSnapshotInfo;

/* Builds a snapshot at the barrier. Does not modify the machine. */
SmsSnapshotStatus sms_snapshot_capture(const SmsSnapshotMachine *machine, const SmsSnapshotIdentity *identity, const SmsSnapshotThumbnail *thumbnail,
                                       SmsSnapshotBuffer *out);

/* Structure + bounds + SHA-256 and the header. Needs no machine; used to list slots and history. */
SmsSnapshotStatus sms_snapshot_inspect(const uint8_t *data, size_t size, SmsSnapshotInfo *info);

/* Everything restore decides before it commits: structure, integrity, identity, machine profile, execution fingerprint AND the full staged device validation.
 * SMS_SNAPSHOT_OK means "restore would succeed". `info` may be NULL. Never mutates the machine. */
SmsSnapshotStatus sms_snapshot_validate(const SmsSnapshotMachine *machine, const SmsSnapshotIdentity *identity, const uint8_t *data, size_t size,
                                        SmsSnapshotInfo *info);

/* Restores a snapshot. On any status other than SMS_SNAPSHOT_OK the machine is byte-identical to before the call. In live-input mode (no script) the
 * machine's pending recorded events are dropped (`input_next = input_count`). Host-side objects (ring, extension, pacer) are the caller's to reset. */
SmsSnapshotStatus sms_snapshot_restore(const SmsSnapshotMachine *machine, const SmsSnapshotIdentity *identity, const uint8_t *data, size_t size);

/* Guest milliseconds of a Z80 T-state count (11 / 39,375,000 s per T-state). */
uint64_t sms_snapshot_cycles_to_ms(uint64_t cycles);

#ifdef __cplusplus
}
#endif
#endif
