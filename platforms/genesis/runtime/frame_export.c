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
  size_t pixel_count;
  size_t index;

  if (frame == NULL || stream == NULL) {
    return GENESIS_FRAME_EXPORT_STATUS_INVALID_ARGUMENT;
  }
  if (!genesis_frame_artifact_is_populated(frame)) {
    return GENESIS_FRAME_EXPORT_STATUS_ARTIFACT_NOT_POPULATED;
  }

  if (fprintf(stream, "P6\n%u %u\n255\n", (unsigned)GENESIS_FRAME_WIDTH,
              (unsigned)GENESIS_FRAME_HEIGHT) < 0) {
    return GENESIS_FRAME_EXPORT_STATUS_IO_ERROR;
  }

  pixel_count = (size_t)GENESIS_FRAME_WIDTH * (size_t)GENESIS_FRAME_HEIGHT;
  for (index = 0; index < pixel_count; ++index) {
    uint8_t palette_index = frame->pixels[index];
    GenesisRgb888 color;
    uint8_t rgb[3];

    if (palette_index >= GENESIS_VDP_CRAM_ENTRY_COUNT) {
      return GENESIS_FRAME_EXPORT_STATUS_INVALID_PALETTE_INDEX;
    }
    if (genesis_vdp_decode_cram_entry(frame->palette_snapshot, palette_index, &color) != 0) {
      /* Defensive: genesis_vdp_decode_cram_entry only fails for a NULL
       * argument (never true here) or an out-of-range index already
       * rejected above; this branch is unreachable but is classified
       * identically if it were ever reached. */
      return GENESIS_FRAME_EXPORT_STATUS_INVALID_PALETTE_INDEX;
    }

    rgb[0] = color.r;
    rgb[1] = color.g;
    rgb[2] = color.b;
    if (fwrite(rgb, 1, sizeof(rgb), stream) != sizeof(rgb)) {
      return GENESIS_FRAME_EXPORT_STATUS_IO_ERROR;
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
