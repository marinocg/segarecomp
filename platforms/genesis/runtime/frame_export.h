#ifndef SEGARECOMP_RUNTIME_GENESIS_FRAME_EXPORT_H
#define SEGARECOMP_RUNTIME_GENESIS_FRAME_EXPORT_H

/*
 * SEG-007-T050 add-on: operator-facing local frame export.
 *
 * This module is a pure, optional diagnostic *consumer* of an already-
 * produced `GenesisFrameArtifact` (checkpoint_evidence.h). It performs no
 * rendering, VDP composition, or guest-state reconstruction of its own: it
 * reads exactly the `pixels` (per-pixel CRAM entry index, 0-63) and
 * `palette_snapshot` (raw 128-byte CRAM) fields the artifact already
 * carries, and reuses the existing accepted CRAM-to-RGB conversion owner,
 * `genesis_vdp_decode_cram_entry` (vdp_render.h, SEG-007-T049), for every
 * pixel's color. It never invokes a second/parallel renderer and never
 * affects guest execution, checkpoint selection, frame production, or the
 * frame digest.
 *
 * Output format: binary PPM ("P6"), the simplest widely-supported
 * uncompressed true-color image container (Netpbm project,
 * "PPM Format Specification" <https://netpbm.sourceforge.net/doc/ppm.html>):
 * an ASCII header (`P6\n<width> <height>\n255\n`) followed by exactly
 * width*height RGB triplets, one byte per channel, row-major, top-left
 * origin -- no compression, no dependency on any image library. Output is a
 * pure deterministic function of the artifact's own `pixels` and
 * `palette_snapshot` fields; the same artifact always produces byte-for-byte
 * identical output.
 *
 * Export outcome is deliberately classified into disjoint categories so a
 * caller (or test) never conflates "the artifact itself is invalid/
 * unpopulated" with "the local filesystem/write operation failed" -- per
 * this task's own Non-goals, export failure is host/tooling failure only and
 * must never be reinterpreted as a CPU frontier, renderer unsupported-state
 * result, checkpoint failure, or frame digest mismatch; the real
 * `GenesisFrameArtifact` this module reads from remains valid regardless of
 * export outcome.
 */

#include <stdio.h>

#include "checkpoint_evidence.h" /* GenesisFrameArtifact */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GenesisFrameExportStatus {
  /* Export completed; the destination now holds a complete, deterministic
   * P6 PPM byte-for-byte encoding of `frame`. */
  GENESIS_FRAME_EXPORT_STATUS_OK = 0,
  /* A required pointer argument (`frame`, `stream`, or `path`) was NULL, or
   * an empty `path` string was supplied. No output was produced. */
  GENESIS_FRAME_EXPORT_STATUS_INVALID_ARGUMENT = 1,
  /* `frame` is the never-rendered `{0}` sentinel (see
   * `genesis_frame_artifact_is_populated`, runtime.h) rather than a real
   * produced artifact. No output was produced. This is an artifact-validity
   * failure, never a filesystem/write failure. */
  GENESIS_FRAME_EXPORT_STATUS_ARTIFACT_NOT_POPULATED = 2,
  /* `frame->pixels` contains at least one CRAM entry index
   * >= GENESIS_VDP_CRAM_ENTRY_COUNT (64), which
   * `genesis_vdp_decode_cram_entry` already rejects as out of range. Treated
   * as an invalid/incomplete artifact, distinct from a filesystem/write
   * failure; the destination stream/file may already contain a partially
   * written (and therefore incomplete/unusable) header and/or prior pixel
   * bytes when this status is returned. */
  GENESIS_FRAME_EXPORT_STATUS_INVALID_PALETTE_INDEX = 3,
  /* Opening, writing to, or closing the destination failed (host/tooling
   * failure only -- e.g. bad path, permission, disk full, short write).
   * The source `GenesisFrameArtifact` itself was valid and is unaffected. */
  GENESIS_FRAME_EXPORT_STATUS_IO_ERROR = 4
} GenesisFrameExportStatus;

/*
 * Writes `frame` to the already-open binary-mode stream `stream` as a
 * deterministic 320x224 P6 PPM. Performs no rendering of its own: every
 * pixel's color comes from `genesis_vdp_decode_cram_entry(frame->
 * palette_snapshot, frame->pixels[i], ...)` (vdp_render.h). This is the pure
 * / directly-testable entry point (no filesystem access of its own); see
 * `genesis_frame_export_ppm_to_path` for the path-based convenience wrapper
 * real callers use.
 *
 * Returns GENESIS_FRAME_EXPORT_STATUS_OK on success. Returns
 * GENESIS_FRAME_EXPORT_STATUS_INVALID_ARGUMENT if `frame` or `stream` is
 * NULL (no bytes are written). Returns
 * GENESIS_FRAME_EXPORT_STATUS_ARTIFACT_NOT_POPULATED if `frame` is the
 * never-rendered sentinel (no bytes are written). Returns
 * GENESIS_FRAME_EXPORT_STATUS_INVALID_PALETTE_INDEX if any pixel's palette
 * index is out of range (the header and/or some prior pixel bytes may
 * already have been written to `stream` before this is detected). Returns
 * GENESIS_FRAME_EXPORT_STATUS_IO_ERROR if any `fprintf`/`fwrite` call on
 * `stream` fails.
 */
GenesisFrameExportStatus genesis_frame_export_ppm_to_stream(const GenesisFrameArtifact *frame,
                                                             FILE *stream);

/*
 * Convenience wrapper: opens `path` in binary write mode ("wb", truncating
 * any existing file), calls `genesis_frame_export_ppm_to_stream`, and closes
 * the file. This is the entry point real (non-test) callers use.
 *
 * Returns GENESIS_FRAME_EXPORT_STATUS_INVALID_ARGUMENT if `frame` or `path`
 * is NULL, or if `path` is an empty string. Returns
 * GENESIS_FRAME_EXPORT_STATUS_IO_ERROR if `path` cannot be opened for
 * writing, or if closing it fails even though the stream-level export
 * itself reported success. Otherwise returns exactly what
 * `genesis_frame_export_ppm_to_stream` returned.
 */
GenesisFrameExportStatus genesis_frame_export_ppm_to_path(const GenesisFrameArtifact *frame,
                                                           const char *path);

#ifdef __cplusplus
}
#endif

#endif
