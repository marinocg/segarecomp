# 0317 - Launcher data location, bootstrap pointer and crash-recoverable migration (SEG-150-T008)

## Status

Accepted. Core only (`apps/segarecomp-launcher/data_location.{hpp,cpp}`); the Settings UI that drives it is SEG-150-T009.

## Context

All per-user launcher data sits under one directory: the "cache" root (`SEGARECOMP_CACHE_DIR`, else `%LOCALAPPDATA%\Segarecomp`, else the SDL preference path) with the
library at `<cache>/library` (`SEGARECOMP_LIBRARY_DIR` overrides the library alone). A user who wants that data elsewhere (another drive) has no safe way to do it, and a
naive move is dangerous: builds record absolute paths, the catalog references managed files by absolute path, a crash halfway leaves two half-roots, and "which root is
authoritative" must survive a restart. Environment overrides exist and must never be silently ignored or contradicted.

## Decision

### 1. Data classes (read from the code, not invented)

"Data root" = today's cache root `R`. Everything below is relative to it.

| Class | Area (stable name) | Lives at | Migrates |
|---|---|---|---|
| durable | catalog | `library/library.json` | yes, then references rebased (3) |
| durable | settings | `library/launcher-settings.json`, `library/viewer-settings.cfg`, `library/gamecontrollerdb.txt` | yes |
| durable | bios_profiles | `library/sega-cd-bios.json` (references external BIOS files) | yes, byte-identical |
| durable | covers | `library/covers/**` (downloaded and user-chosen cover images) | yes |
| durable | managed_roms | `library/roms/**` (ROMs unpacked from ZIPs) | yes, references rebased |
| durable | mods | `library/patches/**` (installed packages; approvals live in the catalog record) | yes |
| durable | saves | `library/saves/**` (Genesis and Master System saves and save states) | yes |
| durable | recordings | `library/sessions/**` | yes |
| durable | sega_cd_data | `library/sega-cd/<id16>/**` except the three build folders (backup RAM, input recordings, reports) | yes |
| regenerable | builds | `games/<32 hex>[.partial\|.failed]/**`, `library/sega-cd/<id16>/build[.partial\|.failed]/**` | **no** |
| regenerable | compiler_cache | `zig-cache/`, `sega-cd-objects/` | **no** |
| regenerable | downloads | `cover-downloads/` | **no** |
| regenerable | catalog_backup | `library/library.json.bak` (re-created by the next catalog save) | **no** |
| left behind | other | any other file or folder (unknown, user-dropped, `*.corrupt`) | never moved, never removed |
| external | (not under `R`) | ROMs, BIOS images, CUE/BIN, linked targets, the installation | **never moved, rewritten or read for migration** |

Builds, the compiler cache and downloads are regenerable and are **not migrated** (the simpler safe option): a build records absolute paths (generated C, objects,
the Zig cache, `status.json`), so reusing a copy at a new path would need per-artifact verification that does not exist. Games with a build simply show as not built at
the new root and are rebuilt on demand. The old copies stay where they are until the explicit cleanup (5).

### 2. Bootstrap pointer, outside every movable root

`Segarecomp-data-location.json` (`{"schema":1,"root":"<absolute UTF-8 path>"}`) lives **beside** the default root, i.e. in the parent of the default cache root
(`~/.local/share/`, `~/Library/Application Support/`, `%LOCALAPPDATA%`), never inside it. Moving, cleaning or deleting any data root therefore never moves it. It is
written atomically (temporary file + rename in the same directory). Absent bootstrap = today's defaults, so an existing profile upgrades with nothing moved and nothing
created. Resolution (`resolve_roots`, used by `cache_root()` / `library_root()`), per process, once for the bootstrap:

1. `SEGARECOMP_CACHE_DIR` (cache) / `SEGARECOMP_LIBRARY_DIR` (library) always win; the bootstrap is then not consulted (not even recovery runs).
2. A valid bootstrap whose root is an existing directory names `R`; the library is `R/library`.
3. A bootstrap whose root is unavailable (unplugged drive), damaged, or from a newer schema falls back to the default location with a typed notice
   (`EffectivePaths::data_location_notice`) and creates nothing at the missing path. It is never rewritten to repair it. Consequence: until the drive returns the old data is
   used and edits made meanwhile stay at the old root (the user sees the notice; nothing is merged automatically).

A root change takes effect at the next start (the bootstrap is read once per process); the running session keeps its roots. `SEGARECOMP_DATA_BOOTSTRAP` replaces the
bootstrap file path and is a local test seam (like `SEGARECOMP_SCD_BUILD_ARGS`), never set by Segarecomp.

### 3. Migration transaction

`build_migration_plan` is read-only and returns durable items (relative path, bytes, area), regenerable and left-behind summaries, the free-space estimate
(durable bytes + 10 % + 8 MiB, injectable provider) and typed refusals. `run_migration` re-plans, takes `BusyGuard::try_begin_maintenance()` (so a running build, game,
import or maintenance makes it `busy`), and runs on the caller's worker thread with a cancel token and a progress callback (`plan -> copy -> rebase -> verify -> commit
-> done`, files and bytes). Nothing at the old root is ever written.

Journal files sit beside the bootstrap: `<bootstrap>.journal` (id, phase, source, destination, whether the destination was created, owner pid and start time, counts) and
`<bootstrap>.manifest` (relative path, size, SHA-256 per file; digests filled after verification). Both are written atomically.

| Step | Journal phase | What happens |
|---|---|---|
| 1 | - | manifest (sizes) then journal `copy` written; destination created if absent; ownership marker `.segarecomp-migration-<id>` written (proves the journal owns the destination: it was verified absent or empty) |
| 2 | `copy` | per file: hash source, `copy_file`, preserve mtime; source size must still match the plan |
| 3 | `copy` | rebase: in the COPY of the catalog only, a reference to a migrated managed file (e.g. `library/roms/x`) is rewritten to the new root; an imported Sega CD program that lived in a regenerable build folder is cleared (rebuild); every external reference and the BIOS profile store stay byte-identical |
| 4 | `verify` | each destination file re-read and compared to the source digest and size; the source size is rechecked; the rebased catalog is re-read (same games, rebased references exist); the destination holds exactly the planned file set (counts, no extras) |
| 5 | `commit` | manifest rewritten with digests; cancel is last honoured here; journal `commit`; **atomic bootstrap write** (the commit point) |
| 6 | `done` | journal `done` (kept as the completion record), marker removed |

Refusals (typed, nothing written, old root and destination untouched): `env_override_cache`, `env_override_library` (the detail names the effective path; nothing else is
examined), `bootstrap_unusable`, `journal_present`, `previous_migration_pending_cleanup`, `source_missing`, `source_symlink`, `source_unsupported_entry`,
`source_unsafe_name`, `source_too_large` (more than 100000 files), `destination_not_absolute`, `destination_unsafe` (`.`/`..`, a filesystem root, inside a protected path
such as the installation), `destination_same` (another spelling, case or alias of the source, decided with `equivalent()`), `destination_inside_source`,
`source_inside_destination`, `destination_symlink` (any existing component that is a link), `destination_not_directory`, `destination_not_empty` (foreign content),
`destination_not_writable`, `insufficient_space`. Failures after the start: `copy_failed`, `source_changed`, `rebase_failed`, `verify_failed`,
`journal_write_failed`, `bootstrap_write_failed`, plus `cancelled`; each rolls the destination back and leaves the old root authoritative.

### 4. Recovery at startup

`recover_pending_migration` runs once before any root is used (inside the first `cache_root()` resolution). It is deterministic and never touches the source.

| Journal state | Bootstrap | Action |
|---|---|---|
| none | any | nothing |
| unreadable, unknown phase, newer schema | any | left alone (`journal_invalid`); new migrations are refused (`journal_present`) until `dismiss_completed_migration` |
| `copy` / `verify` / `commit` | does not name the destination | **rolled back**: the old root stays authoritative; only manifest files and the fixed derived catalog side files are removed from the destination and only when the ownership marker carrying this journal id is present; then only empty directories, the marker, the destination itself if the journal created it; journal and manifest removed. Foreign files in the destination survive and keep it in place. A missing manifest removes nothing |
| `commit` | names the destination | **completed**: the new root is authoritative, journal becomes `done`, marker removed |
| `done` | names the destination | kept as the completion record (`kept_completed`) |
| any unfinished phase, owner process alive (not this process, pid alive, younger than 24 h) | any | `owner_alive`: not touched |

A rollback that cannot finish keeps the journal and is retried at the next start (`rollback_incomplete`).

### 5. Old-root cleanup (separate and explicit)

Never part of the migration. `plan_old_root_cleanup` / `run_old_root_cleanup` are eligible only for a completed migration whose destination the bootstrap still names, and
refuse when the old root is still the effective root (`old_root_active`, e.g. forced by the environment) or maintenance is blocked. By default only regenerable items with
the shapes `storage.cpp` already recognizes are listed (`games/<32 hex>[.partial|.failed]`, `zig-cache`, `sega-cd-objects`, `cover-downloads`, a Sega CD game's build folders);
a folder containing a link is kept. With `include_migrated_copies`, durable files are also listed, but only those that are still bit-identical to the digest verified at
migration time and whose copy exists at the new root; everything else (changed files, unknown files, the catalog backup) is kept and reported. Every item is re-validated
lexically and physically inside the old root and is never a link; only empty parents are removed afterwards. Saves, recordings, ROMs and approvals are never targets of
build cleanup (storage.cpp) and are listed here only as verified duplicates on explicit request. The record is cleared when everything migrated was removed;
`dismiss_completed_migration` forgets it (the old root is then never offered again). A second migration is refused (`previous_migration_pending_cleanup`) until the first is
cleaned up or dismissed, so an old root is never silently orphaned.

## Consequences

* The old root is never modified, so every failure or cancel is "use the old root as before"; the bootstrap is written only after complete verified success.
* Moving costs a rebuild of every built game (no pretense that regenerable output is durable); the plan reports the count.
* Referenced files outside the data root are never copied or rewritten; a library whose references point into the old root (other than managed copies) keeps pointing there.
* An unavailable new root falls back to the default root rather than refusing to start; edits made in between are not merged.
* Not exercised on Windows or macOS: symlink refusal assumes the caller passes a canonical path (macOS `/tmp` and `/var` are links); drive-letter and case-insensitive aliasing
  rely on `std::filesystem::equivalent`. Permission refusal uses the permission bits (an OS-level denial while copying is reported as `copy_failed` and rolled back).
