/* SEG-007-T050 add-on: operator-facing local frame export. See
 * frame_export.h for the full contract/citations. This file performs no
 * rendering/composition of its own; it is a pure PPM (P6) serializer over an
 * already-produced GenesisFrameArtifact, reusing vdp_render.h's existing
 * accepted CRAM-to-RGB conversion owner for every pixel. */

#include "frame_export.h"

#include <stddef.h> /* size_t */

#include "runtime.h"     /* genesis_frame_artifact_is_populated */
#include "vdp_render.h"  /* GenesisRgb888, genesis_vdp_decode_cram_entry, GENESIS_VDP_CRAM_ENTRY_COUNT */

GenesisFrameExportStatus genesis_frame_export_ppm_to_stream(const GenesisFrameArtifact *frame,
                                                             FILE *stream) {
  if (frame == NULL || stream == NULL) {
    return GENESIS_FRAME_EXPORT_STATUS_INVALID_ARGUMENT;
  }
  if (!genesis_frame_artifact_is_populated(frame)) {
    return GENESIS_FRAME_EXPORT_STATUS_ARTIFACT_NOT_POPULATED;
  }

  /* SEG-055: the artifact carries its own geometry (H32/H40, interlace mode 2) and per-row palettes. */
  {
    const unsigned width = GENESIS_FRAME_ARTIFACT_WIDTH(frame);
    const unsigned height = GENESIS_FRAME_ARTIFACT_HEIGHT(frame);
    uint8_t rgb[GENESIS_FRAME_MAX_WIDTH * 3U];
    unsigned row;
    if (width > GENESIS_FRAME_MAX_WIDTH || height > GENESIS_FRAME_MAX_HEIGHT) {
      return GENESIS_FRAME_EXPORT_STATUS_INVALID_PALETTE_INDEX;
    }
    if (fprintf(stream, "P6\n%u %u\n255\n", width, height) < 0) {
      return GENESIS_FRAME_EXPORT_STATUS_IO_ERROR;
    }
    for (row = 0; row < height; ++row) {
      if (genesis_vdp_frame_row_rgb888(frame, row, rgb) != 0) {
        return GENESIS_FRAME_EXPORT_STATUS_INVALID_PALETTE_INDEX;
      }
      if (fwrite(rgb, 1, (size_t)width * 3U, stream) != (size_t)width * 3U) {
        return GENESIS_FRAME_EXPORT_STATUS_IO_ERROR;
      }
    }
  }

  return GENESIS_FRAME_EXPORT_STATUS_OK;
}

GenesisFrameExportStatus genesis_frame_export_ppm_to_path(const GenesisFrameArtifact *frame,
                                                           const char *path) {
  FILE *stream;
  GenesisFrameExportStatus status;

  if (frame == NULL || path == NULL || path[0] == '\0') {
    return GENESIS_FRAME_EXPORT_STATUS_INVALID_ARGUMENT;
  }

  stream = fopen(path, "wb");
  if (stream == NULL) {
    return GENESIS_FRAME_EXPORT_STATUS_IO_ERROR;
  }

  status = genesis_frame_export_ppm_to_stream(frame, stream);
  if (fclose(stream) != 0 && status == GENESIS_FRAME_EXPORT_STATUS_OK) {
    status = GENESIS_FRAME_EXPORT_STATUS_IO_ERROR;
  }
  return status;
}
