/* SEG-062 (ADR 0129): the strict-C11 runtime sector reader. It consumes only the build-time track table emitted with the generated
 * program (no CUE parsing), opens each file inside the given directory, verifies its size and SHA-256 before any guest instruction
 * runs, and reads Mode-1 user blocks by LBA with bounds checks; raw sectors are checked for sync and header. */
#include "scd_runtime.h"

#include <stdio.h>
#include <string.h>

#include "runtime.h" /* genesis_sha256_*: the runtime's one C SHA-256 */

const char *scd_disc_error_name(ScdDiscError error) {
  switch (error) {
  case SCD_DISC_OK: return "ok";
  case SCD_DISC_TABLE_INVALID: return "disc_table_invalid";
  case SCD_DISC_FILE_MISSING: return "disc_file_missing";
  case SCD_DISC_SIZE_MISMATCH: return "disc_size_mismatch";
  case SCD_DISC_IDENTITY_MISMATCH: return "disc_identity_mismatch";
  case SCD_DISC_LBA_OUT_OF_RANGE: return "disc_lba_out_of_range";
  case SCD_DISC_LBA_AUDIO: return "disc_lba_audio";
  case SCD_DISC_RAW_SECTOR_INVALID: return "disc_raw_sector_invalid";
  case SCD_DISC_IO: return "disc_io";
  }
  return "unknown";
}

void scd_disc_close(ScdDisc *disc) {
  uint32_t i;
  for (i = 0; i < 8U; ++i) {
    if (disc->handles[i] != NULL) fclose((FILE *)disc->handles[i]);
    disc->handles[i] = NULL;
  }
}

static int scd_plain_name(const char *name) {
  size_t i, n = strlen(name);
  if (n == 0U || n > 255U || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
  for (i = 0; i < n; ++i)
    if (name[i] == '/' || name[i] == '\\' || name[i] == ':' || (unsigned char)name[i] < 0x20U) return 0;
  return 1;
}

ScdDiscError scd_disc_open(ScdDisc *disc, const char *directory) {
  uint32_t f, t;
  static uint8_t chunk[65536];
  if (disc->file_count == 0U || disc->file_count > 8U || disc->track_count == 0U) return SCD_DISC_TABLE_INVALID;
  for (t = 0; t < disc->track_count; ++t)
    if (disc->tracks[t].file_index >= disc->file_count || disc->tracks[t].type > 2U ||
        (disc->tracks[t].sector_size != 2048U && disc->tracks[t].sector_size != 2352U))
      return SCD_DISC_TABLE_INVALID;
  for (f = 0; f < disc->file_count; ++f) {
    char path[4096];
    FILE *file;
    GenesisSha256 sha;
    uint8_t digest[32];
    char hex[65];
    uint64_t total = 0U;
    size_t got;
    unsigned i;
    if (!scd_plain_name(disc->files[f].name) || strlen(directory) + strlen(disc->files[f].name) + 2U > sizeof path) {
      scd_disc_close(disc);
      return SCD_DISC_TABLE_INVALID;
    }
    snprintf(path, sizeof path, "%s/%s", directory, disc->files[f].name);
    file = fopen(path, "rb");
    if (file == NULL) {
      scd_disc_close(disc);
      return SCD_DISC_FILE_MISSING;
    }
    disc->handles[f] = file;
    genesis_sha256_init(&sha);
    while ((got = fread(chunk, 1U, sizeof chunk, file)) != 0U) {
      genesis_sha256_update(&sha, chunk, (uint32_t)got);
      total += got;
    }
    if (ferror(file)) {
      scd_disc_close(disc);
      return SCD_DISC_IO;
    }
    if (total != disc->files[f].size) {
      scd_disc_close(disc);
      return SCD_DISC_SIZE_MISMATCH;
    }
    genesis_sha256_final(&sha, digest);
    for (i = 0; i < 32U; ++i) snprintf(hex + 2U * i, 3U, "%02x", digest[i]);
    if (strcmp(hex, disc->files[f].sha256) != 0) {
      scd_disc_close(disc);
      return SCD_DISC_IDENTITY_MISMATCH;
    }
  }
  return SCD_DISC_OK;
}

const ScdDiscTrack *scd_disc_track_of(const ScdDisc *disc, uint32_t lba) {
  uint32_t t;
  for (t = 0; t < disc->track_count; ++t)
    if (lba >= disc->tracks[t].first_lba && lba - disc->tracks[t].first_lba < disc->tracks[t].sector_count) return &disc->tracks[t];
  return NULL;
}

static uint8_t scd_bcd(uint32_t v) { return (uint8_t)(((v / 10U) << 4) | (v % 10U)); }

ScdDiscError scd_disc_read_user_data(ScdDisc *disc, uint32_t lba, uint8_t out[2048]) {
  const ScdDiscTrack *track = scd_disc_track_of(disc, lba);
  FILE *file;
  uint64_t offset;
  if (track == NULL) return SCD_DISC_LBA_OUT_OF_RANGE;
  if (track->type == 2U) return SCD_DISC_LBA_AUDIO;
  file = (FILE *)disc->handles[track->file_index];
  if (file == NULL) return SCD_DISC_IO;
  offset = track->file_offset + (uint64_t)(lba - track->first_lba) * track->sector_size;
  if (offset > (uint64_t)0x7FFFFFFF || fseek(file, (long)offset, SEEK_SET) != 0) return SCD_DISC_IO;
  if (track->type == 1U) {
    static const uint8_t sync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    uint8_t header[16];
    const uint32_t msf = lba + 150U;
    if (fread(header, 1U, sizeof header, file) != sizeof header) return SCD_DISC_IO;
    if (memcmp(header, sync, sizeof sync) != 0 || header[12] != scd_bcd(msf / 75U / 60U) || header[13] != scd_bcd(msf / 75U % 60U) ||
        header[14] != scd_bcd(msf % 75U) || header[15] != 1U)
      return SCD_DISC_RAW_SECTOR_INVALID;
  }
  return fread(out, 1U, 2048U, file) == 2048U ? SCD_DISC_OK : SCD_DISC_IO;
}
