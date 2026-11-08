#ifndef SEGARECOMP_VIEWER_SV_STATE_STORE_H
#define SEGARECOMP_VIEWER_SV_STATE_STORE_H

/*
 * SEG-132 (ADR 0279): the platform-neutral file helpers of the save-state slots -- directory creation, a bounded read of one regular file, and the atomic
 * candidate + rename replacement. They are the same, already-accepted rules as the Genesis persistence helpers (SEG-131), without any Genesis type:
 *   - a path that is a directory, a symbolic link or a device is never read as, or replaced by, a file (lstat; reparse points on Windows);
 *   - a read is bounded by the caller's maximum and by the length the file had, and a file that grew meanwhile is refused;
 *   - a replacement writes `<name>.next` in the same directory, flushes it to the OS, validates its size and type, optionally keeps the previous generation as
 *     `<name>.prev` (best effort) and installs the new file with one rename. Any failure keeps the previous file. fsync-level power-loss durability is not claimed.
 * Plain C11 (POSIX / Win32 file calls only). UTF-8 paths.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SV_STORE_MAX_KEEP_BYTES (8U * 1024U * 1024U)   /* the largest previous generation that is kept as `.prev` */

typedef enum SvStoreStatus {
  SV_STORE_OK = 0,
  SV_STORE_FRESH,           /* read: there is no such file */
  SV_STORE_BAD_ARGUMENT,
  SV_STORE_PATH_TOO_LONG,
  SV_STORE_NOT_REGULAR,     /* a directory, a symbolic link, a device ... */
  SV_STORE_BAD_SIZE,        /* larger than the caller's bound, or grew while being read */
  SV_STORE_IO_ERROR,
  SV_STORE_REPLACE_FAILED   /* the final rename failed; the previous file is intact */
} SvStoreStatus;

const char *sv_store_status_text(SvStoreStatus status);

/* Creates `dir` and its missing parents (a directory that exists is fine; anything else in the way is NOT_REGULAR and is never replaced). */
SvStoreStatus sv_store_ensure_dir(const char *dir);
/* Writes `<dir>/<name>.next`, flushes, validates, optionally keeps `<name>.prev`, renames over `<dir>/<name>`. */
SvStoreStatus sv_store_replace_file(const char *dir, const char *name, const uint8_t *data, size_t size, int keep_previous);
/* Reads a whole regular file of at most `max_bytes` into a malloc'd buffer (caller frees). */
SvStoreStatus sv_store_read_file(const char *dir, const char *name, size_t max_bytes, uint8_t **data, size_t *size);

#ifdef __cplusplus
}
#endif
#endif
