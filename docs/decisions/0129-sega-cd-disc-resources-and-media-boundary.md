# ADR 0129: Sega CD disc resources and the build-time/runtime media boundary

- Status: Accepted (SEG-062-T001). Build-time side: SEG-062-T002. Runtime side: SEG-062-T005.
- Related: ADR 0126, contract sections 8 and 9.

## Inspected facts

`libs/media` is C++ (`segarecomp::media`: ROM header, SHA-256) linked only by build-time tools. Generated programs link
only strict-C11 runtime sources; `segarecomp_runtime_genesis` never links C++ and already carries a C SHA-256
(`genesis_sha256_init/update/final`).

## Decision

1. **The disc stays external.** A CUE/BIN image identified by the SHA-256 of every referenced file plus the SHA-256 of
   the normalized track table. No disc data sector is embedded in generated C. (Executable-image bytes appear only as
   the byte-identity constants of compiled instructions, which compiled code already is a function of.) Save data, if
   ever supported, is stored separately; CHD is deferred; there is no virtual filesystem and no asset bundle.
2. **One CUE parser, in C++.** `libs/media` `DiscImage` parses the supported CUE subset (`FILE ... BINARY`, `TRACK nn
   MODE1/2048 | MODE1/2352 | AUDIO`, `INDEX 01`, `PREGAP`; `INDEX 00` stored pregaps since SEG-063-T003, contract section 9a), resolves files strictly inside the CUE directory and
   produces a normalized track table: per track `{number, type, file index, file byte offset, sector size, first LBA,
   sector count}`.
3. **The table reaches the runtime as constant data in the generated program** (a `static const` array emitted next to
   the compiled code), never as a CUE text and never through a sidecar parser. The runtime receives only the directory
   of the disc files on its command line.
4. **Runtime sector reader** (`platforms/sega-cd/runtime/scd_disc.h`, strict C11): `scd_disc_open(dir, table)`
   opens each file, checks its size and SHA-256 (with the Genesis runtime's C SHA-256, no second implementation) and
   fails with a typed error before any guest instruction runs; `scd_disc_read_user_data(disc, lba, out[2048])` reads
   one Mode-1 user block by LBA, bounds-checked against the table, verifying the sync pattern and header of raw sectors;
   an audio-track LBA or an LBA outside every track is a typed error. Deterministic, no caching semantics.
5. **The runtime never depends on the harness, a C++ ABI or the CUE text.**

## Consequences

- Changing one byte of a data sector changes the file SHA-256: an old compiled program refuses the new disc at start-up
  (typed `disc_identity_mismatch`), and a rebuild produces a new identity.
- EDC/ECC of raw sectors is not checked (contract section 13).
