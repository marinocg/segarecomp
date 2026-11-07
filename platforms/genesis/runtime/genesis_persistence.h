/*
 * SEG-131 (ADR 0278): Genesis cartridge persistence -- the battery-backed SRAM (`cartridge.srm`) and the X24C01 EEPROM array (`cartridge.eep`) -- and the
 * one bounded, atomic file-replacement helper the save states share.
 *
 * What is stored. SRAM: exactly `cartridge_sram->storage_bytes` dense bytes of `cartridge_sram_storage` (the existing odd/even-lane layout of the descriptor,
 * no header, no ROM). EEPROM: exactly its 128-byte array; the serial protocol lines/phase are never persisted (a new session starts idle).
 *
 * Lifecycle (ADR 0252 pattern). `genesis_persist_load` runs after the device is installed and before the first guest instruction. A missing file keeps the
 * deterministic 0xFF fill; an existing file must be a regular, non-symlink file of exactly the right length and is loaded unchanged whatever it contains
 * (all-zero included); anything else is an error and the file is left exactly as it was. `genesis_persist_commit` is called only for a clean player exit: it
 * writes a candidate in the same directory, flushes it, validates size and type, keeps the previous generation as `<name>.prev` (best effort) and replaces
 * the canonical file with one rename. Any failure keeps the previous save. fsync-level power-loss durability is NOT claimed.
 *
 * Plain C11 (POSIX / Win32 file calls only); no allocation beyond a bounded read buffer.
 */
#ifndef SEGARECOMP_GENESIS_PERSISTENCE_H
#define SEGARECOMP_GENESIS_PERSISTENCE_H

#include <stddef.h>
#include <stdint.h>

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GENESIS_PERSIST_SRAM_FILE "cartridge.srm"
#define GENESIS_PERSIST_EEPROM_FILE "cartridge.eep"
#define GENESIS_PERSIST_MAX_FILE_BYTES (4U * 1024U * 1024U)

typedef enum GenesisPersistStatus {
  GENESIS_PERSIST_OK = 0,
  GENESIS_PERSIST_FRESH,           /* load: no file yet; the deterministic initial fill stays */
  GENESIS_PERSIST_NOT_APPLICABLE,  /* this cartridge owns no supported persistent memory */
  GENESIS_PERSIST_BAD_ARGUMENT,
  GENESIS_PERSIST_PATH_TOO_LONG,
  GENESIS_PERSIST_NOT_REGULAR,     /* a directory, a symbolic link, a device ... */
  GENESIS_PERSIST_BAD_SIZE,        /* wrong length: left untouched, never repaired */
  GENESIS_PERSIST_IO_ERROR,        /* unreadable / unwritable */
  GENESIS_PERSIST_REPLACE_FAILED   /* the final rename failed; the previous save is intact */
} GenesisPersistStatus;

const char *genesis_persist_status_text(GenesisPersistStatus status);

/* The canonical file name for this cartridge's persistent memory, or NULL when there is none (no SRAM, unsupported layout, no storage). */
const char *genesis_persist_file_name(const GenesisRuntime *runtime);
/* Bytes the file holds (0 = none). */
uint32_t genesis_persist_file_bytes(const GenesisRuntime *runtime);

/* Loads `<dir>/<name>` into the cartridge memory. Does not modify the runtime unless it returns GENESIS_PERSIST_OK. */
GenesisPersistStatus genesis_persist_load(GenesisRuntime *runtime, const char *dir);
/* Clean-session commit of the current cartridge memory (see above). */
GenesisPersistStatus genesis_persist_commit(const GenesisRuntime *runtime, const char *dir);

/* Creates `dir` and its missing parents (a directory that exists is fine; anything else in the way is NOT_REGULAR... never replaced). */
GenesisPersistStatus genesis_persist_ensure_dir(const char *dir);
/* The shared atomic replace: writes `<dir>/<name>.next`, flushes, validates, optionally keeps `<name>.prev`, renames over `<dir>/<name>`. */
GenesisPersistStatus genesis_persist_replace_file(const char *dir, const char *name, const uint8_t *data, size_t size, int keep_previous);
/* Reads a whole regular file of at most `max_bytes` into a malloc'd buffer (caller frees). NOT_REGULAR / BAD_SIZE / IO_ERROR otherwise. */
GenesisPersistStatus genesis_persist_read_file(const char *dir, const char *name, size_t max_bytes, uint8_t **data, size_t *size);

#ifdef __cplusplus
}
#endif
#endif
