#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "sv_state_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define PATH_CAP 1536U

const char *sv_store_status_text(SvStoreStatus status) {
  switch (status) {
    case SV_STORE_OK: return "ok";
    case SV_STORE_FRESH: return "nothing saved here yet";
    case SV_STORE_BAD_ARGUMENT: return "invalid request";
    case SV_STORE_PATH_TOO_LONG: return "the save location path is too long";
    case SV_STORE_NOT_REGULAR: return "the save file is not a regular file";
    case SV_STORE_BAD_SIZE: return "the save file has the wrong size";
    case SV_STORE_IO_ERROR: return "the save file could not be read or written";
    case SV_STORE_REPLACE_FAILED: return "the new save could not replace the old one; the old save was kept";
  }
  return "unknown error";
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Portable file primitives (UTF-8 paths).                                                                                            */

static int join_path(char *out, size_t cap, const char *dir, const char *name, const char *suffix) {
  const size_t dir_len = strlen(dir);
  const int needs_separator = dir_len != 0U && dir[dir_len - 1U] != '/' && dir[dir_len - 1U] != '\\';
  const int written = snprintf(out, cap, "%s%s%s%s", dir, needs_separator ? "/" : "", name, suffix);
  return written > 0 && (size_t)written < cap;
}

typedef enum Kind { KIND_MISSING = 0, KIND_REGULAR = 1, KIND_OTHER = 2 } Kind;

#if defined(_WIN32)
static int to_wide(const char *utf8, wchar_t *out, int cap) { return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cap) > 0; }

static FILE *open_file(const char *path, const char *mode) {
  wchar_t wpath[PATH_CAP], wmode[8];
  if (!to_wide(path, wpath, (int)PATH_CAP) || !to_wide(mode, wmode, 8)) return NULL;
  return _wfopen(wpath, wmode);
}

static Kind kind_of(const char *path, uint64_t *size) {
  wchar_t wpath[PATH_CAP];
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!to_wide(path, wpath, (int)PATH_CAP)) return KIND_OTHER;
  if (!GetFileAttributesExW(wpath, GetFileExInfoStandard, &data)) {
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? KIND_MISSING : KIND_OTHER;
  }
  if ((data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) != 0U) return KIND_OTHER;
  if (size != NULL) *size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
  return KIND_REGULAR;
}

static int remove_file(const char *path) {
  wchar_t wpath[PATH_CAP];
  if (!to_wide(path, wpath, (int)PATH_CAP)) return -1;
  return DeleteFileW(wpath) ? 0 : (GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1);
}

static int replace_file(const char *from, const char *to) {
  wchar_t wfrom[PATH_CAP], wto[PATH_CAP];
  if (!to_wide(from, wfrom, (int)PATH_CAP) || !to_wide(to, wto, (int)PATH_CAP)) return -1;
  return MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
}

static int flush_to_os(FILE *f) { return fflush(f) == 0 && _commit(_fileno(f)) == 0; }
#else
static FILE *open_file(const char *path, const char *mode) { return fopen(path, mode); }

static Kind kind_of(const char *path, uint64_t *size) {
  struct stat st;
  if (lstat(path, &st) != 0) return errno == ENOENT ? KIND_MISSING : KIND_OTHER;  /* lstat: a symbolic link is never followed */
  if (!S_ISREG(st.st_mode)) return KIND_OTHER;
  if (size != NULL) *size = (uint64_t)st.st_size;
  return KIND_REGULAR;
}

static int remove_file(const char *path) { return remove(path) == 0 || errno == ENOENT ? 0 : -1; }
static int replace_file(const char *from, const char *to) { return rename(from, to); }
static int flush_to_os(FILE *f) { return fflush(f) == 0 && fsync(fileno(f)) == 0; }
#endif

SvStoreStatus sv_store_ensure_dir(const char *dir) {
  char path[PATH_CAP];
  size_t i, length;
  if (dir == NULL || *dir == '\0') return SV_STORE_BAD_ARGUMENT;
  length = strlen(dir);
  if (length >= sizeof path) return SV_STORE_PATH_TOO_LONG;
  memcpy(path, dir, length + 1U);
  for (i = 1U; i <= length; ++i) {
    if (path[i] != '/' && path[i] != '\\' && path[i] != '\0') continue;
    {
      const char saved = path[i];
      path[i] = '\0';
      if (i > 1U && !(i == 2U && path[1] == ':')) {  /* a drive prefix "C:" is not a directory to create */
        const Kind kind = kind_of(path, NULL);
        if (kind == KIND_OTHER) {
#if defined(_WIN32)
          /* a directory is KIND_OTHER on Windows (attributes): accept it only when it is a plain directory */
          wchar_t wide[PATH_CAP];
          DWORD attributes;
          if (!to_wide(path, wide, (int)PATH_CAP)) return SV_STORE_IO_ERROR;
          attributes = GetFileAttributesW(wide);
          if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) return SV_STORE_NOT_REGULAR;
#else
          struct stat st;
          if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return SV_STORE_NOT_REGULAR;  /* a directory (or a link to one) is fine; a file in the way is not */
#endif
        } else if (kind == KIND_REGULAR) {
          return SV_STORE_NOT_REGULAR;
        } else {
#if defined(_WIN32)
          wchar_t wide[PATH_CAP];
          if (!to_wide(path, wide, (int)PATH_CAP) || (!CreateDirectoryW(wide, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)) return SV_STORE_IO_ERROR;
#else
          if (mkdir(path, 0777) != 0 && errno != EEXIST) return SV_STORE_IO_ERROR;
#endif
        }
      }
      path[i] = saved;
    }
  }
  return SV_STORE_OK;
}

SvStoreStatus sv_store_read_file(const char *dir, const char *name, size_t max_bytes, uint8_t **data, size_t *size) {
  char path[PATH_CAP];
  uint64_t length = 0U;
  Kind kind;
  FILE *f;
  uint8_t *buffer;
  if (data != NULL) *data = NULL;
  if (size != NULL) *size = 0U;
  if (dir == NULL || name == NULL || data == NULL || size == NULL) return SV_STORE_BAD_ARGUMENT;
  if (!join_path(path, sizeof path, dir, name, "")) return SV_STORE_PATH_TOO_LONG;
  kind = kind_of(path, &length);
  if (kind == KIND_MISSING) return SV_STORE_FRESH;
  if (kind != KIND_REGULAR) return SV_STORE_NOT_REGULAR;
  if (length > max_bytes) return SV_STORE_BAD_SIZE;
  buffer = (uint8_t *)malloc(length == 0U ? 1U : (size_t)length);
  if (buffer == NULL) return SV_STORE_IO_ERROR;
  f = open_file(path, "rb");
  if (f == NULL) { free(buffer); return SV_STORE_IO_ERROR; }
  if (length != 0U && fread(buffer, 1U, (size_t)length, f) != (size_t)length) { fclose(f); free(buffer); return SV_STORE_IO_ERROR; }
  {  /* the file must end where its size said it does (it could have grown) */
    unsigned char extra;
    const size_t more = fread(&extra, 1U, 1U, f);
    fclose(f);
    if (more != 0U) { free(buffer); return SV_STORE_BAD_SIZE; }
  }
  *data = buffer;
  *size = (size_t)length;
  return SV_STORE_OK;
}

static SvStoreStatus write_candidate_and_replace(const char *dir, const char *name, const uint8_t *data, size_t size) {
  char canonical[PATH_CAP], candidate[PATH_CAP];
  uint64_t length = 0U;
  FILE *f;
  Kind kind;
  if (!join_path(canonical, sizeof canonical, dir, name, "") || !join_path(candidate, sizeof candidate, dir, name, ".next")) return SV_STORE_PATH_TOO_LONG;
  kind = kind_of(canonical, NULL);
  if (kind == KIND_OTHER) return SV_STORE_NOT_REGULAR;  /* never replace a directory, a link or a device */
  if (kind_of(candidate, NULL) == KIND_OTHER) return SV_STORE_NOT_REGULAR;
  if (remove_file(candidate) != 0) return SV_STORE_IO_ERROR;  /* a stale candidate is never mistaken for this run's */
  f = open_file(candidate, "wb");
  if (f == NULL) return SV_STORE_IO_ERROR;
  if ((size != 0U && fwrite(data, 1U, size, f) != size) || !flush_to_os(f)) {
    fclose(f);
    (void)remove_file(candidate);
    return SV_STORE_IO_ERROR;
  }
  if (fclose(f) != 0) { (void)remove_file(candidate); return SV_STORE_IO_ERROR; }
  if (kind_of(candidate, &length) != KIND_REGULAR || length != (uint64_t)size) {  /* validate what is about to become the save */
    (void)remove_file(candidate);
    return SV_STORE_IO_ERROR;
  }
  if (replace_file(candidate, canonical) != 0) {
    (void)remove_file(candidate);
    return SV_STORE_REPLACE_FAILED;
  }
  return SV_STORE_OK;
}

SvStoreStatus sv_store_replace_file(const char *dir, const char *name, const uint8_t *data, size_t size, int keep_previous) {
  char canonical[PATH_CAP], previous_name[256];
  if (dir == NULL || name == NULL || (data == NULL && size != 0U) || strlen(name) + 8U >= sizeof previous_name) return SV_STORE_BAD_ARGUMENT;
  if (!join_path(canonical, sizeof canonical, dir, name, "")) return SV_STORE_PATH_TOO_LONG;
  if (keep_previous && kind_of(canonical, NULL) == KIND_REGULAR) {  /* best effort: the previous generation survives the replace */
    uint8_t *old = NULL;
    size_t old_size = 0U;
    if (sv_store_read_file(dir, name, SV_STORE_MAX_KEEP_BYTES, &old, &old_size) == SV_STORE_OK) {
      snprintf(previous_name, sizeof previous_name, "%s.prev", name);
      (void)write_candidate_and_replace(dir, previous_name, old, old_size);
      free(old);
    }
  }
  return write_candidate_and_replace(dir, name, data, size);
}

